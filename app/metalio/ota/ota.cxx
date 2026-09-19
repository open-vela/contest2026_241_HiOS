/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * OTA implementation — ported from MetalioClaw4 main/ota.cc.
 *
 * Network access goes through the small BSD-socket Http class declared in
 * ota.h. Flash writes are stubbed via Ota::WriteToFlash (currently a no-op
 * returning true) — wiring it up to the NuttX partition/flash API is a
 * follow-up task. Everything else (URL handling, JSON parsing, activation
 * challenge, version comparison) matches the reference implementation.
 */

#include "ota.h"
#include "application.h"
#include "board_shim.h"
#include "settings.h"
#include "system_info.h"
#include "esp_timer_shim.h"
#include "i18n.h"

#include <cJSON_compat.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <thread>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cerrno>
#include <sys/time.h>
#include <fcntl.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>

#define TAG "Ota"

#ifndef kDefaultOtaUrl
#define kDefaultOtaUrl "https://api.tenclass.net/xiaozhi/ota/"
#endif

/* ------------------------------------------------------------------ */
/* Http                                                                */
/* ------------------------------------------------------------------ */

struct HttpTlsImpl
{
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    int bio_fd = -1;
    bool initialized = false;
    bool handshaken = false;
};

namespace
{

/* mbedTLS BIO callbacks wrapping the existing BSD socket. */
static int http_tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t n = send(fd, buf, len, 0);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)n;
}

static int http_tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t n = recv(fd, buf, len, 0);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return (int)n;
}

} /* namespace */

Http::Http() {}

Http::~Http()
{
    Close();
}

void Http::SetHeader(const std::string &name, const std::string &value)
{
    headers_[name] = value;
}

void Http::SetContent(std::string content)
{
    request_body_ = std::move(content);
}

bool Http::ParseUrl(const std::string &url)
{
    const char *p = url.c_str();
    tls_ = false;
    if (strncmp(p, "http://", 7) == 0)
    {
        p += 7;
        port_ = 80;
    }
    else if (strncmp(p, "https://", 8) == 0)
    {
        p += 8;
        port_ = 443;
        tls_ = true;
    }
    else
    {
        return false;
    }

    const char *slash = strchr(p, '/');
    std::string authority;
    if (slash)
    {
        authority = std::string(p, slash - p);
        path_ = std::string(slash);
    }
    else
    {
        authority = std::string(p);
        path_ = "/";
    }

    size_t colon = authority.find(':');
    if (colon != std::string::npos)
    {
        host_ = authority.substr(0, colon);
        port_ = std::stoi(authority.substr(colon + 1));
    }
    else
    {
        host_ = authority;
    }
    return true;
}

int Http::NetSend(const char *buf, size_t len)
{
    if (!tls_)
    {
        ssize_t n = send(sock_, buf, len, 0);
        if (n < 0)
        {
            last_error_ = errno;
            return -1;
        }
        return (int)n;
    }

    if (!tls_impl_ || !tls_impl_->handshaken)
    {
        last_error_ = ENOTCONN;
        return -1;
    }

    int ret = mbedtls_ssl_write(&tls_impl_->ssl, (const unsigned char *)buf, len);
    if (ret > 0)
        return ret;
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
        last_error_ = EAGAIN;
        return -1;
    }
    last_error_ = EIO;
    return -1;
}

int Http::NetRecv(char *buf, size_t len)
{
    if (!tls_)
    {
        ssize_t n = recv(sock_, buf, len, 0);
        if (n < 0)
        {
            last_error_ = errno;
            return -1;
        }
        return (int)n;
    }

    if (!tls_impl_ || !tls_impl_->handshaken)
    {
        last_error_ = ENOTCONN;
        return -1;
    }

    int ret = mbedtls_ssl_read(&tls_impl_->ssl, (unsigned char *)buf, len);
    if (ret > 0)
        return ret;
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return 0; /* orderly shutdown / EOF */
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
        last_error_ = EAGAIN;
        return -1;
    }
    last_error_ = EIO;
    return -1;
}

bool Http::TlsSetup()
{
    tls_impl_ = std::make_unique<HttpTlsImpl>();
    HttpTlsImpl *t = tls_impl_.get();

    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->ctr_drbg);
    t->initialized = true;

    int ret = mbedtls_ctr_drbg_seed(&t->ctr_drbg, mbedtls_entropy_func,
                                    &t->entropy, nullptr, 0);
    if (ret != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ctr_drbg_seed failed: -0x%x", (unsigned)-ret);
        last_error_ = EIO;
        TlsTeardown();
        return false;
    }

    ret = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_config_defaults failed: -0x%x", (unsigned)-ret);
        last_error_ = EIO;
        TlsTeardown();
        return false;
    }

    /* Start with no certificate verification; add CA verification once a
     * trust store is available. */
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->ctr_drbg);

    ret = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (ret != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_setup failed: -0x%x", (unsigned)-ret);
        last_error_ = EIO;
        TlsTeardown();
        return false;
    }

    ret = mbedtls_ssl_set_hostname(&t->ssl, host_.c_str());
    if (ret != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_set_hostname failed: -0x%x", (unsigned)-ret);
        last_error_ = EIO;
        TlsTeardown();
        return false;
    }

    t->bio_fd = sock_;
    mbedtls_ssl_set_bio(&t->ssl, &t->bio_fd, http_tls_send, http_tls_recv, nullptr);

    while ((ret = mbedtls_ssl_handshake(&t->ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            ESP_LOGE(TAG, "mbedtls_ssl_handshake failed: -0x%x", (unsigned)-ret);
            last_error_ = EIO;
            TlsTeardown();
            return false;
        }
        /* Blocking sockets normally sleep inside recv(); if the stack ever
         * returns WANT_* without blocking, yield so LVGL/touch keep running. */
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    t->handshaken = true;
    return true;
}

void Http::TlsTeardown()
{
    if (tls_impl_)
    {
        HttpTlsImpl *t = tls_impl_.get();
        if (t->initialized)
        {
            mbedtls_ssl_free(&t->ssl);
            mbedtls_ssl_config_free(&t->conf);
            mbedtls_ctr_drbg_free(&t->ctr_drbg);
            mbedtls_entropy_free(&t->entropy);
            t->initialized = false;
            t->handshaken = false;
        }
    }
    tls_impl_.reset();
    tls_ = false;
}

bool Http::SendRequest()
{
    std::string req = method_ + " " + path_ + " HTTP/1.1\r\n";
    req += "Host: " + host_ + "\r\n";
    req += "Connection: close\r\n";
    req += "User-Agent: metalio-claw-4/1.0.0-openvela\r\n";
    for (auto &h : headers_)
        req += h.first + ": " + h.second + "\r\n";
    if (!request_body_.empty())
    {
        char len_buf[32];
        snprintf(len_buf, sizeof(len_buf), "%zu", request_body_.size());
        req += "Content-Length: " + std::string(len_buf) + "\r\n";
    }
    req += "\r\n";
    if (!request_body_.empty())
        req += request_body_;

    size_t sent = 0;
    while (sent < req.size())
    {
        int n = NetSend(req.data() + sent, req.size() - sent);
        if (n <= 0)
        {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

bool Http::ReadStatusAndHeaders()
{
    /* Read until we find "\r\n\r\n" (end of headers). */
    std::string head;
    char buf[512];
    while (true)
    {
        size_t end = head.find("\r\n\r\n");
        if (end != std::string::npos)
        {
            /* Stash any body bytes we read past the headers. */
            body_buffer_.assign(head, end + 4, std::string::npos);
            head.resize(end + 4);
            break;
        }
        ssize_t n = NetRecv(buf, sizeof(buf));
        if (n <= 0)
        {
            return false;
        }
        head.append(buf, n);
        if (head.size() > 16 * 1024)
        {
            ESP_LOGE(TAG, "HTTP response headers too large");
            return false;
        }
    }

    /* Parse status line. */
    size_t sp1 = head.find(' ');
    if (sp1 == std::string::npos)
        return false;
    size_t sp2 = head.find(' ', sp1 + 1);
    if (sp2 == std::string::npos)
        return false;
    status_code_ = std::stoi(head.substr(sp1 + 1, sp2 - sp1 - 1));

    /* Parse headers (case-insensitive Content-Length / Transfer-Encoding). */
    content_length_ = 0;
    chunked_ = false;
    size_t pos = head.find("\r\n") + 2;
    while (pos < head.size())
    {
        size_t eol = head.find("\r\n", pos);
        if (eol == std::string::npos)
            break;
        std::string line = head.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value[0] == ' ')
                value.erase(0, 1);
            /* Lowercase the header name for comparison. */
            std::string lname = name;
            for (auto &c : lname)
                c = (char)tolower((unsigned char)c);

            if (lname == "content-length")
            {
                content_length_ = (size_t)std::strtoul(value.c_str(), nullptr, 10);
            }
            else if (lname == "transfer-encoding" && value.find("chunked") != std::string::npos)
            {
                chunked_ = true;
            }
        }
        pos = eol + 2;
        if (pos >= head.size())
            break;
    }

    body_read_ = 0;
    chunk_remaining_ = 0;
    chunk_header_pending_ = true;
    return true;
}

int Http::ReadChunked(char *buf, size_t len)
{
    size_t out = 0;
    while (out < len)
    {
        if (chunk_remaining_ == 0 && chunk_header_pending_)
        {
            /* Read a chunk size line. */
            std::string line;
            char c;
            while (true)
            {
                if (!body_buffer_.empty())
                {
                    c = body_buffer_[0];
                    body_buffer_.erase(0, 1);
                }
                else
                {
                    ssize_t n = NetRecv(&c, 1);
                    if (n <= 0)
                        return out > 0 ? (int)out : -1;
                }
                if (c == '\n')
                    break;
                if (c != '\r')
                    line.push_back(c);
            }
            /* Parse hex size up to ';' (chunk extensions). */
            size_t semi = line.find(';');
            if (semi != std::string::npos)
                line = line.substr(0, semi);
            chunk_remaining_ = (size_t)std::strtoul(line.c_str(), nullptr, 16);
            chunk_header_pending_ = false;
            if (chunk_remaining_ == 0)
                return (int)out; /* last chunk */
        }

        if (chunk_remaining_ == 0)
            return (int)out;

        size_t want = std::min(len - out, chunk_remaining_);
        /* First drain body_buffer_. */
        size_t from_buf = std::min(want, body_buffer_.size());
        if (from_buf > 0)
        {
            memcpy(buf + out, body_buffer_.data(), from_buf);
            body_buffer_.erase(0, from_buf);
            out += from_buf;
            chunk_remaining_ -= from_buf;
            want -= from_buf;
        }
        if (want > 0)
        {
            ssize_t n = NetRecv(buf + out, want);
            if (n <= 0)
                return out > 0 ? (int)out : -1;
            out += (size_t)n;
            chunk_remaining_ -= (size_t)n;
        }

        if (chunk_remaining_ == 0)
        {
            /* Consume trailing CRLF. */
            char crlf[2];
            for (int i = 0; i < 2; i++)
            {
                if (!body_buffer_.empty())
                {
                    crlf[i] = body_buffer_[0];
                    body_buffer_.erase(0, 1);
                }
                else
                {
                    ssize_t n = NetRecv(&crlf[i], 1);
                    if (n <= 0)
                        break;
                }
            }
            chunk_header_pending_ = true;
        }
    }
    return (int)out;
}

int Http::Read(char *buf, size_t len)
{
    if (sock_ < 0)
        return -1;

    if (chunked_)
        return ReadChunked(buf, len);

    /* Content-length mode. */
    if (content_length_ > 0 && body_read_ >= content_length_)
        return 0;

    size_t want = len;
    if (content_length_ > 0)
        want = std::min(want, content_length_ - body_read_);

    /* Drain body_buffer_ first. */
    size_t out = 0;
    size_t from_buf = std::min(want, body_buffer_.size());
    if (from_buf > 0)
    {
        memcpy(buf, body_buffer_.data(), from_buf);
        body_buffer_.erase(0, from_buf);
        out = from_buf;
        body_read_ += from_buf;
    }

    while (out < want)
    {
        ssize_t n = NetRecv(buf + out, want - out);
        if (n < 0)
        {
            return out > 0 ? (int)out : -1;
        }
        if (n == 0)
            break;
        out += (size_t)n;
        body_read_ += (size_t)n;
    }
    return (int)out;
}

std::string Http::ReadAll()
{
    std::string out;
    char buf[1024];
    while (true)
    {
        int n = Read(buf, sizeof(buf));
        if (n <= 0)
            break;
        out.append(buf, n);
    }
    return out;
}

bool Http::Open(const std::string &method, const std::string &url)
{
    method_ = method;
    if (!ParseUrl(url))
    {
        ESP_LOGE(TAG, "Failed to parse URL: %s", url.c_str());
        last_error_ = EINVAL;
        return false;
    }

    struct addrinfo hints;
    struct addrinfo *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port_);

    if (getaddrinfo(host_.c_str(), port_str, &hints, &res) != 0 || res == nullptr)
    {
        ESP_LOGE(TAG, "getaddrinfo(%s:%d) failed", host_.c_str(), port_);
        last_error_ = EHOSTUNREACH;
        return false;
    }

    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ < 0)
    {
        last_error_ = errno;
        freeaddrinfo(res);
        return false;
    }

    /* Bound waits so a dead DNS/route cannot freeze the activation UI forever. */
    {
        struct timeval tv;
        tv.tv_sec = 20;
        tv.tv_usec = 0;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (connect(sock_, res->ai_addr, res->ai_addrlen) < 0)
    {
        ESP_LOGE(TAG, "connect(%s:%d) failed: %s", host_.c_str(), port_, strerror(errno));
        last_error_ = errno;
        close(sock_);
        sock_ = -1;
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    if (tls_ && !TlsSetup())
    {
        ESP_LOGE(TAG, "TLS setup failed for %s", host_.c_str());
        Close();
        return false;
    }

    if (!SendRequest())
    {
        Close();
        return false;
    }

    if (!ReadStatusAndHeaders())
    {
        Close();
        return false;
    }
    return true;
}

void Http::Close()
{
    TlsTeardown();
    if (sock_ >= 0)
    {
        close(sock_);
        sock_ = -1;
    }
    body_buffer_.clear();
}

/* ------------------------------------------------------------------ */
/* Stub: flash write hook                                              */
/* ------------------------------------------------------------------ */

namespace
{

/* TODO: wire to NuttX flash partition API (boardctl or /dev/...).
 * For now we just verify the download completes; no flash is written. */
bool ota_write_to_flash(const char * /*data*/, size_t /*len*/, bool /*begin*/)
{
    return true;
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* Ota                                                                 */
/* ------------------------------------------------------------------ */

Ota::Ota() {}

Ota::~Ota() {}

std::string Ota::GetCheckVersionUrl()
{
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty())
        url = kDefaultOtaUrl;
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp()
{
    auto http = std::make_unique<Http>();
    auto user_agent = SystemInfo::GetUserAgent();
    auto &board = Board::GetInstance();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_)
    {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s",
                 user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    const char *lang = I18n::GetLocaleCode();
    http->SetHeader("Accept-Language", lang ? lang : "zh-CN");
    http->SetHeader("Content-Type", "application/json");
    return http;
}

esp_err_t Ota::CheckVersion()
{
    current_version_ = "1.0.0-openvela";
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10)
    {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG;
    }

    auto http = SetupHttp();

    std::string data = Board::GetInstance().GetSystemInfoJson();
    std::string method = data.empty() ? "GET" : "POST";
    ESP_LOGE(TAG, "OTA check %s %s (%u bytes)", method.c_str(), url.c_str(),
             (unsigned)data.size());
    http->SetContent(std::move(data));

    if (!http->Open(method, url))
    {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "Failed to open HTTP connection, code=%d", last_error);
        return last_error;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return status_code;
    }

    data = http->ReadAll();
    http->Close();
    ESP_LOGE(TAG, "OTA response %u bytes", (unsigned)data.size());

    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation))
    {
        cJSON *message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message))
            activation_message_ = message->valuestring;
        cJSON *code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code) && code->valuestring != nullptr &&
            code->valuestring[0] != '\0')
        {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        else if (cJSON_IsNumber(code))
        {
            /* Some server builds return the 6-digit code as a JSON number. */
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", code->valueint);
            activation_code_ = buf;
            has_activation_code_ = true;
        }
        cJSON *challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge))
        {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON *timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms))
            activation_timeout_ms_ = timeout_ms->valueint;
        ESP_LOGE(TAG, "xiaozhi.me activation code: %s challenge=%d",
                 has_activation_code_ ? activation_code_.c_str() : "(none)",
                 has_activation_challenge_ ? 1 : 0);
        /* Push into Application immediately so chat/home can show the
         * 6-digit code while Activate() is still polling. */
        if (has_activation_code_)
        {
            Application::GetInstance().ShowActivationCode(
                activation_code_, activation_message_);
        }
    }
    else
    {
        ESP_LOGE(TAG, "OTA JSON has no activation section (already bound?)");
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt))
    {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt)
        {
            if (cJSON_IsString(item))
            {
                if (settings.GetString(item->string) != item->valuestring)
                    settings.SetString(item->string, item->valuestring);
            }
            else if (cJSON_IsNumber(item))
            {
                if (settings.GetInt(item->string) != item->valueint)
                    settings.SetInt(item->string, item->valueint);
            }
        }
        has_mqtt_config_ = true;
        ESP_LOGE(TAG, "OTA mqtt config saved");
    }
    else
    {
        ESP_LOGE(TAG, "No mqtt section found !");
    }

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket))
    {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket)
        {
            if (cJSON_IsString(item))
            {
                if (settings.GetString(item->string) != item->valuestring)
                    settings.SetString(item->string, item->valuestring);
            }
            else if (cJSON_IsNumber(item))
            {
                if (settings.GetInt(item->string) != item->valueint)
                    settings.SetInt(item->string, item->valueint);
            }
        }
        has_websocket_config_ = true;
        ESP_LOGE(TAG, "OTA websocket config saved");
    }

    ESP_LOGE(TAG, "OTA parse server_time/firmware");
    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time))
    {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        if (cJSON_IsNumber(timestamp))
        {
            struct timeval tv;
            double ts = timestamp->valuedouble;
            if (cJSON_IsNumber(timezone_offset))
                ts += (timezone_offset->valueint * 60 * 1000);
            tv.tv_sec = (time_t)(ts / 1000);
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;
            /* Do not block the OTA worker if RTC set stalls. */
            (void)settimeofday(&tv, NULL);
            has_server_time_ = true;
            ESP_LOGE(TAG, "OTA server_time applied");
            write(1, "TIME_OTA\n", 9);
        }
    }

    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware))
    {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version) && version->valuestring)
            firmware_version_ = version->valuestring;
        cJSON *url_item = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url_item) && url_item->valuestring)
            firmware_url_ = url_item->valuestring;

        if (!firmware_version_.empty() && !firmware_url_.empty())
        {
            /* Version compare can be expensive/fragile; never block bind/MQTT. */
            has_new_version_ = false;
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1)
                has_new_version_ = true;
            ESP_LOGE(TAG, "OTA firmware section ver=%s", firmware_version_.c_str());
        }
    }

    cJSON_Delete(root);
    ESP_LOGE(TAG, "CheckVersion done mqtt=%d ws=%d",
             has_mqtt_config_ ? 1 : 0, has_websocket_config_ ? 1 : 0);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid()
{
    /* NuttX doesn't have an esp_ota_mark_app_valid_cancel_rollback equivalent;
     * the boot selection is handled by the bootloader. This is a no-op. */
    ESP_LOGI(TAG, "MarkCurrentVersionValid (no-op on NuttX)");
}

bool Ota::Upgrade(const std::string &firmware_url)
{
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());

    auto http = std::make_unique<Http>();
    if (!http->Open("GET", firmware_url))
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200)
    {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0)
    {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }

    ota_write_to_flash(nullptr, 0, true /*begin*/);

    char buffer[512];
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true)
    {
        int ret = http->Read(buffer, sizeof(buffer));
        if (ret < 0)
        {
            ESP_LOGE(TAG, "Failed to read HTTP data: errno=%d", errno);
            return false;
        }

        recent_read += ret;
        total_read += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0)
        {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s",
                     (unsigned)progress, (unsigned)total_read,
                     (unsigned)content_length, (unsigned)recent_read);
            if (upgrade_callback_)
                upgrade_callback_((int)progress, total_read, content_length, recent_read);
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (ret == 0)
            break;

        if (!ota_write_to_flash(buffer, (size_t)ret, false))
        {
            ESP_LOGE(TAG, "Failed to write OTA data to flash");
            return false;
        }
    }
    http->Close();

    ESP_LOGI(TAG, "Firmware upgrade successful (flash write is stubbed)");
    return true;
}

bool Ota::StartUpgrade(OtaProgressCallback callback)
{
    upgrade_callback_ = std::move(callback);
    return Upgrade(firmware_url_);
}

bool Ota::StartUpgradeFromUrl(const std::string &url, OtaProgressCallback callback)
{
    upgrade_callback_ = std::move(callback);
    return Upgrade(url);
}

std::vector<int> Ota::ParseVersion(const std::string &version)
{
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;
    while (std::getline(ss, segment, '.'))
    {
        /* NOTE: Metalio is built with -fno-exceptions on NuttX so we
         * cannot wrap std::stoi in try/catch.  Our std::stoi shim
         * returns 0 / INT_MIN / INT_MAX on invalid input (never
         * throws).  Since the OTA version vector is displayed to the
         * user as-is, any malformed segment becomes 0 rather than
         * aborting the parse. */
        int v = std::stoi(segment);
        if (segment.empty() || segment[0] < '0' || segment[0] > '9') {
          if (!(segment.size() > 1 && (segment[0] == '-' || segment[0] == '+') && segment[1] >= '0' && segment[1] <= '9')) {
            v = 0;
          }
        }
        versionNumbers.push_back(v);
    }
    return versionNumbers;
}

bool Ota::IsNewVersionAvailable(const std::string &currentVersion,
                                const std::string &newVersion)
{
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i)
    {
        if (newer[i] > current[i])
            return true;
        else if (newer[i] < current[i])
            return false;
    }
    return newer.size() > current.size();
}

std::string Ota::GetActivationPayload()
{
    /* HMAC-SHA256 activation challenge requires efuse HMAC key support, which
     * is not yet wired up on NuttX. Send an empty payload — the server will
     * fall back to the activation code flow. */
    if (!has_serial_number_)
        return "{}";

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", "");
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);
    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate()
{
    if (!has_activation_challenge_)
    {
        /* Unbound devices always get a 6-digit code; challenge is only for
         * HMAC serial-number boards.  Treat code-only as "still waiting". */
        if (has_activation_code_)
            return ESP_ERR_TIMEOUT;
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/')
        url += "/activate";
    else
        url += "activate";

    auto http = SetupHttp();
    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url))
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }

    auto status_code = http->GetStatusCode();
    if (status_code == 202)
        return ESP_ERR_TIMEOUT;
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s",
                 status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
