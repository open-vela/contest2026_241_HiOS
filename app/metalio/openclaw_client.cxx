/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * OpenClaw client implementation — BSD-socket HTTP/WebSocket client used
 * by the openvela application layer to talk to the OpenClaw cloud API.
 *
 * The HTTP path uses raw HTTP/1.1 over a single short-lived TCP connection
 * per request. The voice path opens a WebSocket (RFC 6455) and streams
 * binary OPUS frames in both directions.
 *
 * TLS is not yet implemented (https/wss require mbedTLS handshake wiring).
 */

#include "openclaw_client.h"
#include "esp_log_shim.h"
#include "cJSON_compat.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <chrono>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cerrno>
#include <sys/time.h>

#define TAG "OpenClaw"

/* ------------------------------------------------------------------ */
/* URL parsing                                                         */
/* ------------------------------------------------------------------ */

bool OpenClawClient::ParseBaseUrl()
{
    const char *p = base_url_.c_str();
    if (strncmp(p, "http://", 7) == 0)
    {
        p += 7;
        port_ = 80;
    }
    else if (strncmp(p, "https://", 8) == 0)
    {
        ESP_LOGW(TAG, "https:// not supported yet, falling back to plain http");
        p += 8;
        port_ = 443;
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
        path_prefix_ = std::string(slash);
        if (!path_prefix_.empty() && path_prefix_.back() == '/')
            path_prefix_.pop_back();
    }
    else
    {
        authority = std::string(p);
        path_prefix_ = "";
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

OpenClawClient::OpenClawClient(const std::string &base_url)
    : base_url_(base_url)
{
    ParseBaseUrl();
}

OpenClawClient::~OpenClawClient()
{
    StopVoiceSession();
}

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */

namespace
{

int open_tcp(const std::string &host, int port)
{
    struct addrinfo hints;
    struct addrinfo *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || res == nullptr)
        return -1;

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0)
    {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0)
    {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return sock;
}

bool send_all(int sock, const std::string &data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        ssize_t n = send(sock, data.data() + sent, data.size() - sent, 0);
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

bool read_http_response(int sock, int &status_code, std::string &body)
{
    std::string buf;
    char tmp[1024];
    /* Read until end of headers. */
    while (true)
    {
        if (buf.find("\r\n\r\n") != std::string::npos)
            break;
        ssize_t n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0)
            return false;
        buf.append(tmp, n);
        if (buf.size() > 16 * 1024)
            return false;
    }

    size_t sp1 = buf.find(' ');
    size_t sp2 = buf.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos)
        return false;
    status_code = std::stoi(buf.substr(sp1 + 1, sp2 - sp1 - 1));

    /* Find Content-Length / Transfer-Encoding. */
    size_t content_length = 0;
    bool chunked = false;
    size_t hdr_end = buf.find("\r\n\r\n") + 4;
    std::string headers = buf.substr(0, hdr_end);
    std::string lower;
    lower.reserve(headers.size());
    for (char c : headers)
        lower.push_back((char)tolower((unsigned char)c));

    size_t cl_pos = lower.find("content-length:");
    if (cl_pos != std::string::npos)
    {
        size_t eol = lower.find("\r\n", cl_pos);
        content_length = (size_t)std::strtoul(headers.c_str() + cl_pos + 15, nullptr, 10);
        (void)eol;
    }
    if (lower.find("transfer-encoding: chunked") != std::string::npos)
        chunked = true;

    body = buf.substr(hdr_end);

    if (chunked)
    {
        /* Decode chunked body — keep reading until we see a 0-size chunk. */
        while (true)
        {
            size_t end = body.find("\r\n");
            if (end == std::string::npos)
            {
                ssize_t n = recv(sock, tmp, sizeof(tmp), 0);
                if (n <= 0)
                    break;
                body.append(tmp, n);
                continue;
            }
            std::string size_line = body.substr(0, end);
            body.erase(0, end + 2);
            size_t chunk_size = (size_t)std::strtoul(size_line.c_str(), nullptr, 16);
            if (chunk_size == 0)
                break;
            while (body.size() < chunk_size + 2)
            {
                ssize_t n = recv(sock, tmp, sizeof(tmp), 0);
                if (n <= 0)
                    break;
                body.append(tmp, n);
            }
            body.erase(chunk_size, 2); /* drop trailing CRLF */
        }
        return true;
    }

    /* Content-length mode — read remaining bytes. */
    while (body.size() < content_length)
    {
        ssize_t n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0)
            break;
        body.append(tmp, n);
    }
    if (body.size() > content_length)
        body.resize(content_length);
    return true;
}

} /* namespace */

bool OpenClawClient::HttpPost(const std::string &path, const std::string &body,
                              int &status_code, std::string &response)
{
    int sock = open_tcp(host_, port_);
    if (sock < 0)
    {
        last_error_ = ECONNREFUSED;
        return false;
    }

    std::string req = "POST " + path + " HTTP/1.1\r\n";
    req += "Host: " + host_ + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n";
    req += "User-Agent: metalio-claw-4/1.0.0-openvela\r\n";
    req += "\r\n";
    req += body;

    if (!send_all(sock, req))
    {
        last_error_ = errno;
        close(sock);
        return false;
    }

    bool ok = read_http_response(sock, status_code, response);
    close(sock);
    if (!ok)
        last_error_ = ECONNRESET;
    return ok;
}

bool OpenClawClient::HttpGet(const std::string &path,
                             int &status_code, std::string &response)
{
    int sock = open_tcp(host_, port_);
    if (sock < 0)
    {
        last_error_ = ECONNREFUSED;
        return false;
    }
    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host_ + "\r\n";
    req += "Connection: close\r\n";
    req += "User-Agent: metalio-claw-4/1.0.0-openvela\r\n";
    req += "\r\n";
    if (!send_all(sock, req))
    {
        last_error_ = errno;
        close(sock);
        return false;
    }
    bool ok = read_http_response(sock, status_code, response);
    close(sock);
    if (!ok)
        last_error_ = ECONNRESET;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool OpenClawClient::Connect()
{
    ESP_LOGI(TAG, "Connect base URL: %s (host=%s port=%d)",
             base_url_.c_str(), host_.c_str(), port_);

    int status = 0;
    std::string body;
    std::string path = path_prefix_.empty() ? "/health" : (path_prefix_ + "/health");
    bool ok = HttpGet(path, status, body);
    if (!ok)
    {
        ESP_LOGW(TAG, "GET %s failed — assuming offline (%d)", path.c_str(), last_error_);
        online_ = false;
        /* Don't hard-fail: the device may still boot in offline mode. */
        return false;
    }
    online_ = (status == 200);
    ESP_LOGI(TAG, "Health check status=%d, online=%d", status, online_);
    return online_;
}

bool OpenClawClient::SendText(const std::string &text, std::string &reply)
{
    if (!online_ && !Connect())
        return false;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "text", text.c_str());
    char *req_str = cJSON_PrintUnformatted(req);
    std::string body(req_str);
    cJSON_free(req_str);
    cJSON_Delete(req);

    int status = 0;
    std::string resp;
    std::string path = path_prefix_.empty() ? "/api/chat" : (path_prefix_ + "/api/chat");
    if (!HttpPost(path, body, status, resp))
    {
        ESP_LOGE(TAG, "SendText HTTP failed: %d", last_error_);
        return false;
    }
    if (status != 200)
    {
        ESP_LOGE(TAG, "SendText status=%d body=%s", status, resp.c_str());
        return false;
    }

    cJSON *root = cJSON_Parse(resp.c_str());
    if (!root)
    {
        reply = resp;
        return true;
    }
    cJSON *reply_item = cJSON_GetObjectItem(root, "reply");
    if (cJSON_IsString(reply_item))
        reply = reply_item->valuestring;
    else
        reply = resp;
    cJSON_Delete(root);
    return true;
}

/* ------------------------------------------------------------------ */
/* Voice session (WebSocket)                                           */
/* ------------------------------------------------------------------ */

namespace
{

/* Small base64 encoder (mbedTLS may not be linked for the OpenClaw client). */
std::string b64_encode(const uint8_t *data, size_t len)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3)
    {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)data[i + 2];
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? tbl[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? tbl[v & 0x3F] : '=');
    }
    return out;
}

} /* namespace */

bool OpenClawClient::WsConnect(const std::string &path)
{
    voice_sock_ = open_tcp(host_, port_);
    if (voice_sock_ < 0)
    {
        last_error_ = ECONNREFUSED;
        return false;
    }

    uint8_t key[16];
    srand((unsigned)time(nullptr));
    for (int i = 0; i < 16; i++)
        key[i] = (uint8_t)(rand() & 0xFF);
    std::string key_b64 = b64_encode(key, 16);

    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host_ + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + key_b64 + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    req += "\r\n";
    if (!send_all(voice_sock_, req))
    {
        last_error_ = errno;
        close(voice_sock_);
        voice_sock_ = -1;
        return false;
    }

    /* Read handshake response. */
    std::string resp;
    char tmp[512];
    while (resp.find("\r\n\r\n") == std::string::npos)
    {
        ssize_t n = recv(voice_sock_, tmp, sizeof(tmp), 0);
        if (n <= 0)
        {
            last_error_ = ECONNRESET;
            close(voice_sock_);
            voice_sock_ = -1;
            return false;
        }
        resp.append(tmp, n);
        if (resp.size() > 4096)
            break;
    }
    if (resp.find("101") == std::string::npos)
    {
        ESP_LOGE(TAG, "WS handshake failed: %s", resp.substr(0, 64).c_str());
        last_error_ = ECONNREFUSED;
        close(voice_sock_);
        voice_sock_ = -1;
        return false;
    }
    return true;
}

bool OpenClawClient::WsSendFrame(uint8_t opcode, const uint8_t *data, size_t len)
{
    std::lock_guard<std::mutex> lock(voice_tx_mutex_);
    if (voice_sock_ < 0)
        return false;
    std::vector<uint8_t> frame;
    frame.push_back((uint8_t)(0x80 | opcode));
    if (len <= 125)
        frame.push_back((uint8_t)len);
    else if (len <= 65535)
    {
        frame.push_back(126);
        frame.push_back((uint8_t)(len >> 8));
        frame.push_back((uint8_t)(len & 0xFF));
    }
    else
    {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--)
            frame.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
    }
    frame.insert(frame.end(), data, data + len);

    size_t sent = 0;
    while (sent < frame.size())
    {
        ssize_t n = send(voice_sock_, frame.data() + sent, frame.size() - sent, 0);
        if (n <= 0)
        {
            last_error_ = errno;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

void OpenClawClient::WsRxLoop()
{
    while (voice_running_ && voice_sock_ >= 0)
    {
        uint8_t hdr[2];
        ssize_t n = recv(voice_sock_, hdr, 2, MSG_WAITALL);
        if (n != 2)
            break;

        uint8_t opcode = hdr[0] & 0x0F;
        bool masked = (hdr[1] & 0x80) != 0;
        size_t payload_len = hdr[1] & 0x7F;
        if (payload_len == 126)
        {
            uint8_t ext[2];
            if (recv(voice_sock_, ext, 2, MSG_WAITALL) != 2) break;
            payload_len = (ext[0] << 8) | ext[1];
        }
        else if (payload_len == 127)
        {
            uint8_t ext[8];
            if (recv(voice_sock_, ext, 8, MSG_WAITALL) != 8) break;
            payload_len = 0;
            for (int i = 0; i < 8; i++)
                payload_len = (payload_len << 8) | ext[i];
        }

        uint8_t mask[4] = {0};
        if (masked && recv(voice_sock_, mask, 4, MSG_WAITALL) != 4)
            break;

        if (payload_len > 1024 * 1024)
            break;

        std::vector<uint8_t> payload(payload_len);
        if (payload_len > 0)
        {
            size_t got = 0;
            while (got < payload_len)
            {
                ssize_t r = recv(voice_sock_, payload.data() + got,
                                 payload_len - got, 0);
                if (r <= 0)
                    goto exit_loop;
                got += (size_t)r;
            }
            if (masked)
            {
                for (size_t i = 0; i < payload_len; i++)
                    payload[i] ^= mask[i % 4];
            }
        }

        switch (opcode)
        {
        case 0x1: /* text */
            if (on_text_)
                on_text_(std::string((const char *)payload.data(), payload.size()));
            break;
        case 0x2: /* binary */
            if (on_audio_)
                on_audio_(payload);
            break;
        case 0x8: /* close */
            goto exit_loop;
        case 0x9: /* ping */
            WsSendFrame(0xA, payload.data(), payload.size());
            break;
        case 0xA: /* pong */
            break;
        default:
            break;
        }
        continue;

    exit_loop:
        break;
    }

    voice_active_ = false;
    if (on_disconnected_)
        on_disconnected_();
}

bool OpenClawClient::StartVoiceSession()
{
    if (voice_active_)
        return true;
    std::string path = path_prefix_.empty() ? "/api/voice" : (path_prefix_ + "/api/voice");
    if (!WsConnect(path))
    {
        ESP_LOGE(TAG, "WS connect failed: %d", last_error_);
        return false;
    }
    voice_active_ = true;
    voice_running_ = true;
    voice_thread_ = std::thread([this]() { this->WsRxLoop(); });
    ESP_LOGI(TAG, "Voice session started");
    return true;
}

void OpenClawClient::StopVoiceSession()
{
    voice_running_ = false;
    if (voice_sock_ >= 0)
    {
        /* Send WS close frame. */
        uint8_t close_frame[2] = {0x88, 0x00};
        std::lock_guard<std::mutex> lock(voice_tx_mutex_);
        if (voice_sock_ >= 0)
            send(voice_sock_, close_frame, 2, 0);
    }
    if (voice_thread_.joinable())
        voice_thread_.join();
    if (voice_sock_ >= 0)
    {
        close(voice_sock_);
        voice_sock_ = -1;
    }
    voice_active_ = false;
    ESP_LOGI(TAG, "Voice session stopped");
}

bool OpenClawClient::SendAudio(const std::vector<uint8_t> &opus_frame)
{
    if (!voice_active_)
        return false;
    return WsSendFrame(0x2, opus_frame.data(), opus_frame.size());
}

bool OpenClawClient::ReceiveAudio(std::vector<uint8_t> &opus_frame, int timeout_ms)
{
    /* The async receive path is driven by WsRxLoop → on_audio_ callback.
     * This synchronous helper is a fallback that's not implemented — callers
     * should use the OnAudio callback instead. */
    (void)opus_frame;
    (void)timeout_ms;
    return false;
}
