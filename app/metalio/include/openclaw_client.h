/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * OpenClaw HTTP/WebSocket client — openvela-specific integration that
 * complements the ported xiaozhi protocol stack.
 *
 * OpenClaw is the openvela cloud API used for:
 *   - Text chat (POST /api/chat)
 *   - Voice sessions (WebSocket /api/voice)
 *   - Audio streaming (binary frames over the voice WebSocket)
 *
 * The client uses NuttX BSD sockets directly (no esp_http_client dependency).
 * TLS is stubbed — wss/https require mbedTLS handshake wiring, which is a
 * follow-up task.
 */

#ifndef METALIO_OPENCLAW_CLIENT_H
#define METALIO_OPENCLAW_CLIENT_H

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

class OpenClawClient
{
public:
    explicit OpenClawClient(const std::string &base_url);
    ~OpenClawClient();

    /* Open the base URL and verify reachability (HTTP HEAD or GET /health). */
    bool Connect();

    /* Synchronous text chat — POST /api/chat with {"text": "..."} and return
     * the assistant reply in `reply`. Returns true on HTTP 200. */
    bool SendText(const std::string &text, std::string &reply);

    /* Voice session — opens a WebSocket to /api/voice and starts the receive
     * loop. Audio frames are delivered to the OnAudio callback. */
    bool StartVoiceSession();
    void StopVoiceSession();
    bool IsVoiceSessionActive() const { return voice_active_; }

    /* Send an OPUS frame to the server (binary WebSocket frame). */
    bool SendAudio(const std::vector<uint8_t> &opus_frame);

    /* Receive an OPUS frame from the server (blocks up to timeout_ms). */
    bool ReceiveAudio(std::vector<uint8_t> &opus_frame, int timeout_ms = 1000);

    /* Callbacks (set before Connect/StartVoiceSession). */
    void OnAudio(std::function<void(const std::vector<uint8_t> &)> cb) { on_audio_ = std::move(cb); }
    void OnText(std::function<void(const std::string &)> cb) { on_text_ = std::move(cb); }
    void OnDisconnected(std::function<void()> cb) { on_disconnected_ = std::move(cb); }

    bool online() const { return online_; }
    int  last_error() const { return last_error_; }

private:
    std::string base_url_;
    std::string host_;
    int port_ = 80;
    std::string path_prefix_;

    bool online_ = false;
    int last_error_ = 0;

    /* Voice WebSocket state. */
    int voice_sock_ = -1;
    std::atomic<bool> voice_active_{false};
    std::atomic<bool> voice_running_{false};
    std::thread voice_thread_;
    std::mutex voice_tx_mutex_;
    std::function<void(const std::vector<uint8_t> &)> on_audio_;
    std::function<void(const std::string &)> on_text_;
    std::function<void()> on_disconnected_;

    bool ParseBaseUrl();
    bool HttpPost(const std::string &path, const std::string &body,
                  int &status_code, std::string &response);
    bool HttpGet(const std::string &path,
                 int &status_code, std::string &response);

    /* Minimal WebSocket helpers (RFC 6455). */
    bool WsConnect(const std::string &path);
    bool WsSendFrame(uint8_t opcode, const uint8_t *data, size_t len);
    void WsRxLoop();
};

#endif /* METALIO_OPENCLAW_CLIENT_H */
