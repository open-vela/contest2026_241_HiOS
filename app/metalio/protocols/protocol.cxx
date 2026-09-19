/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Base protocol implementation — ported from
 * MetalioClaw4 main/protocols/protocol.cc.
 */

#include "protocol.h"

#include <cstdio>
#include <unistd.h>

#define TAG "Protocol"

void Protocol::OnIncomingJson(std::function<void(const cJSON *root)> callback)
{
    on_incoming_json_ = std::move(callback);
}

void Protocol::OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback)
{
    on_incoming_audio_ = std::move(callback);
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback)
{
    on_audio_channel_opened_ = std::move(callback);
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback)
{
    on_audio_channel_closed_ = std::move(callback);
}

void Protocol::OnNetworkError(std::function<void(const std::string &message)> callback)
{
    on_network_error_ = std::move(callback);
}

void Protocol::OnConnected(std::function<void()> callback)
{
    on_connected_ = std::move(callback);
}

void Protocol::OnDisconnected(std::function<void()> callback)
{
    on_disconnected_ = std::move(callback);
}

void Protocol::SetError(const std::string &message)
{
    error_occurred_ = true;
    if (on_network_error_)
    {
        on_network_error_(message);
    }
}

void Protocol::SendAbortSpeaking(AbortReason reason)
{
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected)
    {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    SendText(message);
}

bool Protocol::SendWakeWordDetected(const std::string &wake_word)
{
    write(1, "DET_IN\n", 7);
    /* Avoid uClibc++ string-concat temporaries on the app-thread stack. */
    char json[320];
    const int n = snprintf(json, sizeof(json),
        "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"detect\","
        "\"text\":\"%s\"}",
        session_id_.c_str(), wake_word.c_str());
    if (n <= 0 || (size_t)n >= sizeof(json))
    {
        write(1, "DET_BAD\n", 8);
        return false;
    }
    write(1, "DET_GO\n", 7);
    const bool ok = SendText(std::string(json, (size_t)n));
    write(1, ok ? "DET_OUT1\n" : "DET_OUT0\n", 9);
    return ok;
}

void Protocol::SendStartListening(ListeningMode mode)
{
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime)
    {
        message += ",\"mode\":\"realtime\"";
    }
    else if (mode == kListeningModeAutoStop)
    {
        message += ",\"mode\":\"auto\"";
    }
    else
    {
        message += ",\"mode\":\"manual\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendStopListening()
{
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void Protocol::SendMcpMessage(const std::string &payload)
{
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

bool Protocol::IsTimeout() const
{
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout)
    {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}
