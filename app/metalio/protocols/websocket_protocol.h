/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebSocket protocol — ported from MetalioClaw4 main/protocols/websocket_protocol.h.
 *
 * The upstream implementation depends on Board::GetNetwork()->CreateWebSocket().
 * The openvela port implements a small WebSocket client (RFC 6455) over NuttX
 * BSD sockets with text/binary/ping/pong frame handling. TLS is stubbed —
 * wiring it up requires mbedTLS handshake integration.
 */

#ifndef METALIO_WEBSOCKET_PROTOCOL_H
#define METALIO_WEBSOCKET_PROTOCOL_H

#include "protocol.h"
#include "freertos_shim.h"

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

#include <sys/socket.h>
#include <netinet/in.h>

#include <functional>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

/* Minimal WebSocket client over BSD sockets. */
class WebSocket
{
public:
    WebSocket();
    ~WebSocket();

    void SetHeader(const std::string &name, const std::string &value);
    bool Connect(const std::string &url);
    bool IsConnected() const;
    int  GetLastError() const { return last_error_; }

    bool Send(const std::string &text);
    bool Send(const char *data, size_t len, bool binary);

    void OnData(std::function<void(const char *data, size_t len, bool binary)> cb)
    {
        on_data_ = std::move(cb);
    }
    void OnDisconnected(std::function<void()> cb)
    {
        on_disconnected_ = std::move(cb);
    }

    void Stop();

private:
    int sock_ = -1;
    int last_error_ = 0;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::thread rx_thread_;
    std::mutex tx_mutex_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::function<void(const char *, size_t, bool)> on_data_;
    std::function<void()> on_disconnected_;

    bool SendFrame(uint8_t opcode, const char *data, size_t len);
    void RxLoop();
};

class WebsocketProtocol : public Protocol
{
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;

    void ParseServerHello(const cJSON *root);
    bool SendText(const std::string &text) override;
    std::string GetHelloMessage();
};

#endif /* METALIO_WEBSOCKET_PROTOCOL_H */
