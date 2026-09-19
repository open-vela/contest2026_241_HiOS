/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * MQTT protocol — ported from MetalioClaw4 main/protocols/mqtt_protocol.h.
 *
 * The upstream implementation depends on Board::GetNetwork()->CreateMqtt() and
 * CreateUdp() which provide an ESP-IDF style MQTT/UDP abstraction. The openvela
 * port implements a minimal MQTT v3.1.1 client over NuttX BSD sockets plus a
 * UDP audio channel encrypted with AES-CTR (mbedTLS). This is sufficient for
 * the device's hello/listen/goodbye control flow and OPUS audio streaming.
 */

#ifndef METALIO_MQTT_PROTOCOL_H
#define METALIO_MQTT_PROTOCOL_H

#include <nuttx/config.h>

#include "protocol.h"
#include "esp_timer_shim.h"
#include "freertos_shim.h"
#include "esp_err_shim.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <functional>
#include <string>
#include <map>
#include <mutex>
#include <memory>
#include <atomic>
#include <thread>

/* AES-CTR for UDP audio — use NuttX mbedTLS when available. */
#if defined(CONFIG_CRYPTO_MBEDTLS) || defined(CONFIG_METALIO_MBEDTLS)
#include <mbedtls/aes.h>
#define METALIO_MQTT_HAVE_AES 1
#endif

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 60000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

/* Minimal in-app MQTT client over BSD sockets (TLS when port==8883). */
class Mqtt
{
public:
    Mqtt();
    ~Mqtt();

    void SetKeepAlive(int seconds);
    bool Connect(const std::string &host, int port,
                 const std::string &client_id,
                 const std::string &username,
                 const std::string &password);
    bool IsConnected() const;
    int  GetLastError() const { return last_error_; }

    bool Publish(const std::string &topic, const std::string &payload);
    bool Subscribe(const std::string &topic);

    void OnConnected(std::function<void()> cb) { on_connected_ = std::move(cb); }
    void OnDisconnected(std::function<void()> cb) { on_disconnected_ = std::move(cb); }
    void OnMessage(std::function<void(const std::string &topic, const std::string &payload)> cb)
    {
        on_message_ = std::move(cb);
    }

    void Stop();

private:
    struct TlsImpl;

    int  sock_ = -1;
    int  keepalive_ = 240;
    int  last_error_ = 0;
    bool use_tls_ = false;
    std::unique_ptr<TlsImpl> tls_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::thread rx_thread_;
    /* Serializes ALL mbedtls ssl_read/ssl_write (and plain socket IO).
     * Concurrent Publish (wake/app) + RxLoop without this corrupts TLS and
     * hangs/blue-screens at WAKE_START / listen commands. */
    std::mutex io_mutex_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;
    std::function<void(const std::string &, const std::string &)> on_message_;

    bool SendConnect(const std::string &client_id,
                     const std::string &username,
                     const std::string &password);
    bool SendPacket(const uint8_t *buf, size_t len);
    int  NetSend(const uint8_t *buf, size_t len);
    int  NetRecv(uint8_t *buf, size_t len);
    bool TlsSetup(const std::string &host);
    void TlsTeardown();
    bool WaitConnack(int timeout_ms);
    void RxLoop();
    bool ReadMqttPacketLocked(uint8_t &fh, std::string &payload);
};

/* Minimal UDP audio transport used by MqttProtocol. */
class Udp
{
public:
    Udp();
    ~Udp();

    bool Connect(const std::string &host, int port);
    ssize_t Send(const std::string &data);
    void OnMessage(std::function<void(const std::string &)> cb) { on_message_ = std::move(cb); }
    void StartReceive();
    void Stop();

private:
    int sock_ = -1;
    std::atomic<bool> running_{false};
    std::thread rx_thread_;
    std::function<void(const std::string &)> on_message_;
};

class MqttProtocol : public Protocol
{
public:
    MqttProtocol();
    ~MqttProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;
    void ClearOpeningAudioGate() override;

private:
    EventGroupHandle_t event_group_handle_;

    std::string publish_topic_;
    std::string subscribe_topic_;

    std::mutex channel_mutex_;
    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<Udp> udp_;
#ifdef METALIO_MQTT_HAVE_AES
    mbedtls_aes_context aes_ctx_;
#endif
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_ = 0;
    uint32_t local_sequence_ = 0;
    uint32_t remote_sequence_ = 0;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    /* True while OpenAudioChannel runs OR until ClearOpeningAudioGate. */
    std::atomic<bool> opening_audio_{false};

    bool StartMqttClient(bool report_error = false);
    void ParseServerHello(const cJSON *root);
    std::string DecodeHexString(const std::string &hex_string);

    bool SendText(const std::string &text) override;
    std::string GetHelloMessage();
};

#endif /* METALIO_MQTT_PROTOCOL_H */
