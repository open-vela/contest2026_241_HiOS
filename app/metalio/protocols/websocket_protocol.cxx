/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebSocket protocol implementation — ported from
 * MetalioClaw4 main/protocols/websocket_protocol.cc.
 *
 * Uses a small RFC 6455 client implemented on NuttX BSD sockets. Frame
 * encoding/decoding handles text (0x1), binary (0x2), close (0x8),
 * ping (0x9) and pong (0xA). The TLS path is stubbed (ws:// only); wiring
 * wss:// requires hooking mbedTLS into SendFrame/RxLoop.
 */

#include "websocket_protocol.h"
#include "board_shim.h"
#include "system_info.h"
#include "settings.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <cerrno>
#include <netdb.h>
#include <arpa/inet.h>

#define TAG "WS"

/* ------------------------------------------------------------------ */
/* WebSocket client                                                    */
/* ------------------------------------------------------------------ */

namespace
{

/* Compute the SHA-1 of a 36-byte client-key+magic string and base64-encode
 * the result. NuttX ships mbedtls, so use it when available; otherwise emit
 * a deterministic placeholder (debug-only — server will reject handshake). */
#ifdef CONFIG_METALIO_MBEDTLS
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#endif

std::string b64_encode(const uint8_t *data, size_t len)
{
#ifdef CONFIG_METALIO_MBEDTLS
    size_t out_len = 0;
    mbedtls_base64_encode(nullptr, 0, &out_len, data, len);
    std::string out(out_len, '\0');
    size_t written = 0;
    mbedtls_base64_encode((unsigned char *)out.data(), out.size(),
                          &written, data, len);
    out.resize(written);
    return out;
#else
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
#endif
}

std::string sha1_base64(const std::string &input)
{
#ifdef CONFIG_METALIO_MBEDTLS
    unsigned char digest[20];
    mbedtls_sha1((const unsigned char *)input.data(), input.size(), digest);
    return b64_encode(digest, 20);
#else
    /* Debug-only fallback — handshake will fail against a real server. */
    return b64_encode((const uint8_t *)input.data(), input.size());
#endif
}

} /* namespace */

WebSocket::WebSocket() {}

WebSocket::~WebSocket()
{
    Stop();
}

void WebSocket::SetHeader(const std::string &name, const std::string &value)
{
    headers_.emplace_back(name, value);
}

bool WebSocket::Connect(const std::string &url)
{
    last_error_ = 0;
    /* Parse url: ws://host[:port]/path  (wss:// not supported yet) */
    std::string host, path;
    int port = 80;
    const char *p = url.c_str();
    if (strncmp(p, "ws://", 5) == 0)
    {
        p += 5;
    }
    else if (strncmp(p, "wss://", 6) == 0)
    {
        ESP_LOGW(TAG, "wss:// not supported yet, falling back to plain ws");
        p += 6;
        port = 443;
    }

    const char *slash = strchr(p, '/');
    std::string authority;
    if (slash)
    {
        authority = std::string(p, slash - p);
        path = std::string(slash);
    }
    else
    {
        authority = std::string(p);
        path = "/";
    }

    size_t colon = authority.find(':');
    if (colon != std::string::npos)
    {
        host = authority.substr(0, colon);
        port = std::stoi(authority.substr(colon + 1));
    }
    else
    {
        host = authority;
    }

    /* Resolve host */
    struct addrinfo hints;
    struct addrinfo *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || res == nullptr)
    {
        ESP_LOGE(TAG, "ws: getaddrinfo(%s:%d) failed", host.c_str(), port);
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

    if (connect(sock_, res->ai_addr, res->ai_addrlen) < 0)
    {
        ESP_LOGE(TAG, "ws: connect(%s:%d) failed: %s", host.c_str(), port, strerror(errno));
        last_error_ = errno;
        close(sock_);
        sock_ = -1;
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    /* Build the opening handshake (RFC 6455 §4.1). */
    uint8_t key_bytes[16];
    for (int i = 0; i < 16; i++)
        key_bytes[i] = (uint8_t)(rand() & 0xFF);
    std::string key = b64_encode(key_bytes, 16);

    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    for (auto &h : headers_)
        req += h.first + ": " + h.second + "\r\n";
    req += "\r\n";

    if (send(sock_, req.data(), req.size(), 0) != (ssize_t)req.size())
    {
        last_error_ = errno;
        close(sock_);
        sock_ = -1;
        return false;
    }

    /* Read the HTTP response and check for 101 Switching Protocols. */
    std::string resp;
    char buf[512];
    while (true)
    {
        ssize_t n = recv(sock_, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            last_error_ = ECONNRESET;
            close(sock_);
            sock_ = -1;
            return false;
        }
        resp.append(buf, n);
        if (resp.find("\r\n\r\n") != std::string::npos)
            break;
        if (resp.size() > 4096)
            break;
    }

    if (resp.find("101") == std::string::npos)
    {
        ESP_LOGE(TAG, "ws: handshake failed: %s", resp.substr(0, 64).c_str());
        last_error_ = ECONNREFUSED;
        close(sock_);
        sock_ = -1;
        return false;
    }

    /* Verify Sec-WebSocket-Accept (best-effort). */
    std::string expected = sha1_base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    if (resp.find(expected) == std::string::npos)
    {
        ESP_LOGW(TAG, "ws: accept header mismatch (continuing anyway)");
    }

    connected_ = true;
    running_ = true;
    rx_thread_ = std::thread([this]() { this->RxLoop(); });
    return true;
}

bool WebSocket::IsConnected() const
{
    return connected_;
}

bool WebSocket::SendFrame(uint8_t opcode, const char *data, size_t len)
{
    std::lock_guard<std::mutex> lock(tx_mutex_);
    if (sock_ < 0)
        return false;

    std::vector<uint8_t> frame;
    frame.push_back((uint8_t)(0x80 | opcode)); /* FIN + opcode */
    if (len <= 125)
    {
        frame.push_back((uint8_t)len);
    }
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
        ssize_t n = send(sock_, frame.data() + sent, frame.size() - sent, 0);
        if (n <= 0)
        {
            last_error_ = errno;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

bool WebSocket::Send(const std::string &text)
{
    return SendFrame(0x1, text.data(), text.size());
}

bool WebSocket::Send(const char *data, size_t len, bool binary)
{
    return SendFrame(binary ? 0x2 : 0x1, data, len);
}

void WebSocket::RxLoop()
{
    while (running_ && sock_ >= 0)
    {
        /* Read frame header (minimum 2 bytes). */
        uint8_t hdr[2];
        ssize_t n = recv(sock_, hdr, 2, MSG_WAITALL);
        if (n != 2)
            break;

        bool fin = (hdr[0] & 0x80) != 0;
        uint8_t opcode = hdr[0] & 0x0F;
        bool masked = (hdr[1] & 0x80) != 0;
        size_t payload_len = hdr[1] & 0x7F;

        if (payload_len == 126)
        {
            uint8_t ext[2];
            if (recv(sock_, ext, 2, MSG_WAITALL) != 2) break;
            payload_len = (ext[0] << 8) | ext[1];
        }
        else if (payload_len == 127)
        {
            uint8_t ext[8];
            if (recv(sock_, ext, 8, MSG_WAITALL) != 8) break;
            payload_len = 0;
            for (int i = 0; i < 8; i++)
                payload_len = (payload_len << 8) | ext[i];
        }

        uint8_t mask[4] = {0};
        if (masked)
        {
            if (recv(sock_, mask, 4, MSG_WAITALL) != 4) break;
        }

        if (payload_len > 1 * 1024 * 1024)
        {
            ESP_LOGE(TAG, "ws frame too large: %u", (unsigned)payload_len);
            break;
        }

        std::vector<char> payload(payload_len);
        if (payload_len > 0)
        {
            size_t got = 0;
            while (got < payload_len)
            {
                ssize_t r = recv(sock_, payload.data() + got, payload_len - got, 0);
                if (r <= 0) goto exit_loop;
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
        case 0x2: /* binary */
            if (on_data_)
                on_data_(payload.data(), payload.size(), opcode == 0x2);
            break;
        case 0x8: /* close */
            goto exit_loop;
        case 0x9: /* ping → reply pong */
            SendFrame(0xA, payload.data(), payload.size());
            break;
        case 0xA: /* pong */
            break;
        default:
            break;
        }
        (void)fin;
        continue;

    exit_loop:
        break;
    }

    connected_ = false;
    if (on_disconnected_)
        on_disconnected_();
}

void WebSocket::Stop()
{
    running_ = false;
    if (sock_ >= 0)
    {
        close(sock_);
        sock_ = -1;
    }
    connected_ = false;
    if (rx_thread_.joinable())
        rx_thread_.join();
}

/* ------------------------------------------------------------------ */
/* WebsocketProtocol                                                   */
/* ------------------------------------------------------------------ */

WebsocketProtocol::WebsocketProtocol()
{
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol()
{
    if (event_group_handle_)
        vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start()
{
    /* Only connect when an audio channel is opened. */
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet)
{
    if (websocket_ == nullptr || !websocket_->IsConnected())
        return false;

    if (version_ == 2)
    {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2 *)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl((uint32_t)packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());
        return websocket_->Send(serialized.data(), serialized.size(), true);
    }
    else if (version_ == 3)
    {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3 *)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons((uint16_t)packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());
        return websocket_->Send(serialized.data(), serialized.size(), true);
    }
    else
    {
        return websocket_->Send((const char *)packet->payload.data(),
                                packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string &text)
{
    if (websocket_ == nullptr || !websocket_->IsConnected())
        return false;
    if (!websocket_->Send(text))
    {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError("server error");
        return false;
    }
    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const
{
    return websocket_ != nullptr && websocket_->IsConnected() &&
           !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel()
{
    websocket_.reset();
}

bool WebsocketProtocol::OpenAudioChannel()
{
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0)
        version_ = version;

    error_occurred_ = false;

    websocket_ = std::make_unique<WebSocket>();
    if (websocket_ == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty())
    {
        if (token.find(" ") == std::string::npos)
            token = "Bearer " + token;
        websocket_->SetHeader("Authorization", token);
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_));
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid());

    websocket_->OnData([this](const char *data, size_t len, bool binary) {
        if (binary)
        {
            if (on_incoming_audio_)
            {
                if (version_ == 2)
                {
                    BinaryProtocol2 *bp2 = (BinaryProtocol2 *)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    auto payload = (uint8_t *)bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)}));
                }
                else if (version_ == 3)
                {
                    BinaryProtocol3 *bp3 = (BinaryProtocol3 *)data;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    auto payload = (uint8_t *)bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)}));
                }
                else
                {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t *)data, (uint8_t *)data + len)}));
                }
            }
        }
        else
        {
            auto root = cJSON_Parse(data);
            if (root == nullptr)
            {
                ESP_LOGE(TAG, "Failed to parse json: %.*s", (int)len, data);
                return;
            }
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type))
            {
                if (strcmp(type->valuestring, "hello") == 0)
                {
                    ParseServerHello(root);
                }
                else if (on_incoming_json_)
                {
                    on_incoming_json_(root);
                }
            }
            else
            {
                ESP_LOGE(TAG, "Missing message type, data: %.*s", (int)len, data);
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_)
            on_audio_channel_closed_();
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url))
    {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError("server not connected");
        return false;
    }

    auto message = GetHelloMessage();
    if (!SendText(message))
        return false;

    EventBits_t bits = xEventGroupWaitBits(event_group_handle_,
                                           WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT,
                                           pdTRUE, pdFALSE,
                                           10000 / portTICK_PERIOD_MS);
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT))
    {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError("server timeout");
        return false;
    }

    if (on_audio_channel_opened_)
        on_audio_channel_opened_();
    return true;
}

std::string WebsocketProtocol::GetHelloMessage()
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON *features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON *audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON *root)
{
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "websocket") != 0)
    {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport ? transport->valuestring : "null");
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id))
    {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params))
    {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate))
            server_sample_rate_ = sample_rate->valueint;
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration))
            server_frame_duration_ = frame_duration->valueint;
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
