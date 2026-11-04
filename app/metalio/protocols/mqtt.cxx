#include "hi_task_cpp.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * MQTT protocol implementation — ported from
 * MetalioClaw4 main/protocols/mqtt_protocol.cc.
 *
 * Replaces the ESP-IDF Mqtt/Udp abstractions with the small BSD-socket
 * Mqtt/Udp classes declared in mqtt_protocol.h. AES-CTR audio encryption
 * uses mbedTLS when METALIO_MQTT_HAVE_AES is enabled; otherwise the audio
 * payload is sent unencrypted (debug builds only).
 */

#include "mqtt_protocol.h"
#include "app_dns.h"
#include "application.h"
#include "board_shim.h"
#include "settings.h"
#include "system_info.h"
#include "esp_log_shim.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <cerrno>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <algorithm>
#include <pthread.h>
#include <sched.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/error.h>

#define TAG "MQTT"

/* Fallback broker endpoint used when "mqtt/endpoint" is not present in the
 * settings KV.  Override at build time (e.g. via EXTRAFLAGS
 * -DMETALIO_MQTT_DEFAULT_ENDPOINT=\"192.168.1.100:1883\") or leave empty to
 * keep the original "endpoint not specified" behaviour. */
#ifndef METALIO_MQTT_DEFAULT_ENDPOINT
#define METALIO_MQTT_DEFAULT_ENDPOINT ""
#endif

/* ------------------------------------------------------------------ */
/* Minimal MQTT v3.1.1 control packet helpers                         */
/* ------------------------------------------------------------------ */

struct Mqtt::TlsImpl
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

static int mqtt_tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    /* MSG_DONTWAIT: blocking send held io_mutex_ and froze the app thread
     * mid MCP reply (serial died right after MCP_BAT_SKIP). */
    ssize_t n = send(fd, buf, len, MSG_DONTWAIT);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        ESP_LOGE(TAG, "mqtt socket send failed fd=%d errno=%d", fd, errno);
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)n;
}

static int mqtt_tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    /* Blocking recv is OK for RxLoop (poll'd first). SendPacket only pumps
     * recv after poll(POLLIN), so it will not stall with no data. */
    ssize_t n = recv(fd, buf, len, 0);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return (int)n;
}

/* Encode a remaining-length field per MQTT spec section 2.2.3. */
size_t encode_remaining_length(uint8_t *buf, size_t value)
{
    size_t bytes = 0;
    do
    {
        uint8_t encoded = value & 0x7F;
        value >>= 7;
        if (value > 0)
            encoded |= 0x80;
        buf[bytes++] = encoded;
    } while (value > 0 && bytes < 4);
    return bytes;
}

/* Decode a remaining-length field; returns bytes consumed or 0 on error. */
size_t decode_remaining_length(const uint8_t *buf, size_t avail, size_t &value)
{
    size_t multiplier = 1;
    size_t bytes = 0;
    value = 0;
    do
    {
        if (bytes >= avail || bytes >= 4)
            return 0;
        value += (buf[bytes] & 0x7F) * multiplier;
        multiplier *= 128;
    } while (buf[bytes++] & 0x80);
    return bytes;
}

/* Append a UTF-8 string field (length-prefixed). */
void append_string(std::string &out, const std::string &s)
{
    uint16_t len = htons((uint16_t)s.size());
    out.append(reinterpret_cast<const char *>(&len), 2);
    out.append(s);
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* Mqtt                                                                */
/* ------------------------------------------------------------------ */

Mqtt::Mqtt() {}

Mqtt::~Mqtt()
{
    Stop();
}

void Mqtt::SetKeepAlive(int seconds)
{
    keepalive_ = seconds;
}

void Mqtt::TlsTeardown()
{
    if (!tls_)
        return;
    if (tls_->initialized)
    {
        mbedtls_ssl_free(&tls_->ssl);
        mbedtls_ssl_config_free(&tls_->conf);
        mbedtls_ctr_drbg_free(&tls_->ctr_drbg);
        mbedtls_entropy_free(&tls_->entropy);
        tls_->initialized = false;
        tls_->handshaken = false;
    }
    tls_.reset();
}

bool Mqtt::TlsSetup(const std::string &host)
{
    TlsTeardown();
    tls_ = std::make_unique<TlsImpl>();
    TlsImpl *t = tls_.get();

    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->ctr_drbg);
    t->initialized = true;
    t->bio_fd = sock_;

    int ret = mbedtls_ctr_drbg_seed(&t->ctr_drbg, mbedtls_entropy_func,
                                    &t->entropy, nullptr, 0);
    if (ret != 0)
    {
        ESP_LOGE(TAG, "mqtt tls seed failed: -0x%x", (unsigned)-ret);
        TlsTeardown();
        return false;
    }

    ret = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
    {
        ESP_LOGE(TAG, "mqtt tls config failed: -0x%x", (unsigned)-ret);
        TlsTeardown();
        return false;
    }

    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->ctr_drbg);

    ret = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (ret != 0)
    {
        ESP_LOGE(TAG, "mqtt tls setup failed: -0x%x", (unsigned)-ret);
        TlsTeardown();
        return false;
    }

    mbedtls_ssl_set_hostname(&t->ssl, host.c_str());
    mbedtls_ssl_set_bio(&t->ssl, &t->bio_fd, mqtt_tls_send, mqtt_tls_recv, nullptr);

    const auto handshake_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(metalio_app_cellular_active() ? 20 : 6);
    while ((ret = mbedtls_ssl_handshake(&t->ssl)) != 0)
    {
        if (std::chrono::steady_clock::now() >= handshake_deadline)
        {
            TlsTeardown();
            return false;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            ESP_LOGE(TAG, "mqtt tls handshake failed: -0x%x", (unsigned)-ret);
            TlsTeardown();
            return false;
        }
    }

    t->handshaken = true;
    ESP_LOGE(TAG, "mqtt TLS handshake ok (%s)", host.c_str());
    return true;
}

int Mqtt::NetSend(const uint8_t *buf, size_t len)
{
    if (use_tls_)
    {
        if (!tls_ || !tls_->handshaken)
            return -1;
        int ret = mbedtls_ssl_write(&tls_->ssl, buf, len);
        if (ret > 0)
            return ret;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            errno = EAGAIN;
            return -1;
        }
        ESP_LOGE(TAG, "mqtt TLS write failed code=-0x%x", (unsigned)-ret);
        errno = EPROTO;
        return -1;
    }

    ssize_t n = send(sock_, buf, len, MSG_DONTWAIT);
    return (n < 0) ? -1 : (int)n;
}

int Mqtt::NetRecv(uint8_t *buf, size_t len)
{
    if (use_tls_)
    {
        if (!tls_ || !tls_->handshaken)
            return -1;
        int ret = mbedtls_ssl_read(&tls_->ssl, buf, len);
        if (ret > 0)
            return ret;
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            errno = EAGAIN;
            return -1;
        }
        ESP_LOGE(TAG, "mqtt TLS read failed code=-0x%x", (unsigned)-ret);
        errno = EPROTO;
        return -1;
    }

    ssize_t n = recv(sock_, buf, len, 0);
    return (n < 0) ? -1 : (int)n;
}

bool Mqtt::WaitConnack(int timeout_ms)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    uint8_t fh = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        struct pollfd pfd;
        pfd.fd = sock_;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 200);
        if (pr < 0)
            return false;
        if (pr == 0)
            continue;

        int n = NetRecv(&fh, 1);
        if (n <= 0)
            return false;

        /* remaining length */
        size_t rl_value = 0;
        uint8_t rlbuf[4];
        size_t rl_bytes = 0;
        for (size_t i = 0; i < 4; i++)
        {
            int r = NetRecv(&rlbuf[i], 1);
            if (r <= 0)
                return false;
            rl_bytes++;
            if (!(rlbuf[i] & 0x80))
                break;
        }
        if (decode_remaining_length(rlbuf, rl_bytes, rl_value) == 0)
            return false;

        std::string payload;
        if (rl_value > 0)
        {
            payload.resize(rl_value);
            size_t got = 0;
            while (got < rl_value)
            {
                int r = NetRecv((uint8_t *)&payload[got], rl_value - got);
                if (r <= 0)
                    return false;
                got += (size_t)r;
            }
        }

        uint8_t pkt_type = (fh >> 4) & 0x0F;
        if (pkt_type == 2) /* CONNACK */
        {
            uint8_t rc = payload.size() >= 2 ? (uint8_t)payload[1] : 0xFF;
            if (rc != 0)
            {
                ESP_LOGE(TAG, "MQTT CONNACK refused rc=%u", rc);
                return false;
            }
            ESP_LOGE(TAG, "MQTT CONNACK ok");
            return true;
        }
        /* Ignore other packets before CONNACK. */
    }
    ESP_LOGE(TAG, "MQTT CONNACK timeout");
    return false;
}

bool Mqtt::Connect(const std::string &host, int port,
                   const std::string &client_id,
                   const std::string &username,
                   const std::string &password)
{
    last_error_ = 0;
    use_tls_ = (port == 8883);

    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (metalio_voice_resolve_ipv4(host.c_str(), &address.sin_addr.s_addr) < 0)
    {
        last_error_ = EHOSTUNREACH;
        return false;
    }
    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "voice TCP socket allocation failed errno=%d", last_error_);
        return false;
    }
    if (metalio_app_bind_active_interface(sock_) < 0)
    {
        last_error_ = errno ? errno : ENODEV;
        close(sock_); sock_ = -1;
        return false;
    }

    /* Bound connect/TLS so wake→Connecting cannot hang the UI forever. */
    {
        struct timeval tv;
        tv.tv_sec = 8;
        tv.tv_usec = 0;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (connect(sock_, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0)
    {
        ESP_LOGE(TAG, "connect(%s:%d) failed: %s", host.c_str(), port, strerror(errno));
        last_error_ = errno;
        close(sock_);
        sock_ = -1;
        return false;
    }

    if (use_tls_ && !TlsSetup(host))
    {
        last_error_ = ECONNRESET;
        close(sock_);
        sock_ = -1;
        return false;
    }

    if (!SendConnect(client_id, username, password))
    {
        last_error_ = ECONNRESET;
        TlsTeardown();
        close(sock_);
        sock_ = -1;
        return false;
    }

    if (!WaitConnack(8000))
    {
        last_error_ = ETIMEDOUT;
        TlsTeardown();
        close(sock_);
        sock_ = -1;
        return false;
    }

    // Bootstrap TLS/CONNACK is complete. Receive must no longer wait while
    // holding io_mutex_: the application sends listen/detect on this mutex.
    int flags = fcntl(sock_, F_GETFL, 0);
    if (flags < 0 || fcntl(sock_, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        last_error_ = errno;
        TlsTeardown();
        close(sock_);
        sock_ = -1;
        return false;
    }
    rx_buffer_.clear();
    connected_ = true;
    running_ = true;
    rx_thread_ = HiTask::LegacyThread([this]() { this->RxLoop(); });

    if (on_connected_)
        on_connected_();
    return true;
}

bool Mqtt::SendConnect(const std::string &client_id,
                       const std::string &username,
                       const std::string &password)
{
    /* CONNECT variable header: protocol name "MQTT", level 4, connect flags,
     * keep-alive. */
    std::string vh;
    append_string(vh, "MQTT");
    vh.push_back(0x04); /* protocol level 4 = MQTT 3.1.1 */

    uint8_t flags = 0x02; /* clean session */
    std::string payload;
    append_string(payload, client_id);
    if (!username.empty())
    {
        flags |= 0x80;
        append_string(payload, username);
    }
    if (!password.empty())
    {
        flags |= 0x40;
        append_string(payload, password);
    }
    vh.push_back((char)flags);

    uint16_t ka = htons((uint16_t)keepalive_);
    vh.append(reinterpret_cast<const char *>(&ka), 2);

    size_t remaining = vh.size() + payload.size();
    std::string packet;
    packet.reserve(1 + 4 + remaining);
    packet.push_back(0x10); /* CONNECT */
    uint8_t rl[4];
    size_t rl_bytes = encode_remaining_length(rl, remaining);
    packet.append(reinterpret_cast<const char *>(rl), rl_bytes);
    packet.append(vh);
    packet.append(payload);
    return SendPacket(reinterpret_cast<const uint8_t *>(packet.data()), packet.size());
}

bool Mqtt::SendPacket(const uint8_t *buf, size_t len)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (sock_ < 0)
        return false;
    size_t sent = 0;
    int spins = 0;
    while (sent < len)
    {
        int n = NetSend(buf + sent, len - sent);
        if (n > 0)
        {
            sent += (size_t)n;
            spins = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
        {
            /* TLS write may need a read to proceed; pump while holding
             * io_mutex_ so RxLoop cannot race mbedtls. Only poll when
             * readable — never block here (see MCP_BAT_SKIP hang). */
            if (use_tls_ && sock_ >= 0)
            {
                struct pollfd pfd;
                pfd.fd = sock_;
                pfd.events = POLLIN;
                if (poll(&pfd, 1, 0) > 0)
                {
                    uint8_t sink[256];
                    int received = NetRecv(sink, sizeof(sink));
                    if (received > 0)
                        rx_buffer_.append(reinterpret_cast<const char *>(sink), received);
                }
            }
            if (++spins > 200)
            {
                last_error_ = EAGAIN;
                write(1, "TX_SSL_SPIN\n", 12);
                return false;
            }
            usleep(1000);
            continue;
        }
        last_error_ = errno;
        return false;
    }
    return true;
}

bool Mqtt::Publish(const std::string &topic, const std::string &payload)
{
    if (!IsConnected())
        return false;

    /* PUBLISH, QoS0. */
    std::string vh;
    append_string(vh, topic);
    size_t remaining = vh.size() + payload.size();
    std::string packet;
    packet.reserve(1 + 4 + remaining);
    packet.push_back(0x30);
    uint8_t rl[4];
    size_t rl_bytes = encode_remaining_length(rl, remaining);
    packet.append(reinterpret_cast<const char *>(rl), rl_bytes);
    packet.append(vh);
    packet.append(payload);
    return SendPacket(reinterpret_cast<const uint8_t *>(packet.data()), packet.size());
}

bool Mqtt::Subscribe(const std::string &topic)
{
    if (!IsConnected())
        return false;
    /* SUBSCRIBE, QoS1 (packet id = 1). */
    std::string vh;
    uint16_t packet_id = htons(1);
    vh.append(reinterpret_cast<const char *>(&packet_id), 2);
    std::string payload;
    append_string(payload, topic);
    payload.push_back(0x00); /* QoS0 */
    size_t remaining = vh.size() + payload.size();
    std::string packet;
    packet.push_back(0x82); /* SUBSCRIBE with QoS1 flag */
    uint8_t rl[4];
    size_t rl_bytes = encode_remaining_length(rl, remaining);
    packet.append(reinterpret_cast<const char *>(rl), rl_bytes);
    packet.append(vh);
    packet.append(payload);
    return SendPacket(reinterpret_cast<const uint8_t *>(packet.data()), packet.size());
}

bool Mqtt::IsConnected() const
{
    return connected_;
}

void Mqtt::Stop()
{
    running_ = false;
    connected_ = false;
    // Keep socket closure and TLS destruction serialized with every publish.
    // A lease-change worker can otherwise free TLS while the app sends MCP.
    if (sock_ >= 0)
    {
        const uint8_t disc[2] = {0xe0, 0};
        SendPacket(disc, sizeof(disc));
    }
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (sock_ >= 0)
        {
            shutdown(sock_, SHUT_RDWR);
            close(sock_);
            sock_ = -1;
        }
    }
    if (rx_thread_.joinable()) rx_thread_.join();
    std::lock_guard<std::mutex> lock(io_mutex_);
    TlsTeardown();
}

int Mqtt::ReadMqttPacketLocked(uint8_t &fh_out, std::string &payload)
{
    // Caller holds io_mutex_. Preserve split TLS records / MQTT frames and
    // consume already decrypted bytes before waiting for new socket data.
    for (int pass = 0; pass < 2; ++pass)
    {
        size_t remaining = 0, multiplier = 1, header = 1;
        bool complete_length = false;
        while (header < rx_buffer_.size() && header <= 4)
        {
            const uint8_t byte = static_cast<uint8_t>(rx_buffer_[header++]);
            remaining += (byte & 0x7f) * multiplier;
            if (!(byte & 0x80)) { complete_length = true; break; }
            multiplier *= 128;
        }
        if ((!complete_length && header == 5) || remaining > 256 * 1024)
            return -1;
        if (complete_length && rx_buffer_.size() >= header + remaining)
        {
            fh_out = static_cast<uint8_t>(rx_buffer_[0]);
            payload.assign(rx_buffer_.data() + header, remaining);
            rx_buffer_.erase(0, header + remaining);
            return 1;
        }
        if (pass != 0) return 0;
        uint8_t bytes[4096];
        int n = NetRecv(bytes, sizeof(bytes));
        if (n > 0)
            rx_buffer_.append(reinterpret_cast<const char *>(bytes), n);
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        else
            return -1;
    }
    return 0;
}

void Mqtt::RxLoop()
{
    const int ping_seconds = keepalive_ > 0 ? std::max(1, std::min(20, keepalive_ / 2)) : 0;
    auto next_ping = std::chrono::steady_clock::now() + std::chrono::seconds(ping_seconds);
    auto ping_sent = std::chrono::steady_clock::time_point{};
    bool ping_pending = false;
    while (running_ && sock_ >= 0)
    {
        const auto now = std::chrono::steady_clock::now();
        if (ping_pending && now - ping_sent >= std::chrono::seconds(10))
        {
            last_error_ = ETIMEDOUT;
            write(1, "VOICE_PING_TIMEOUT\n", 19);
            break;
        }
        if (ping_seconds && !ping_pending && now >= next_ping)
        {
            const uint8_t ping[] = {0xc0, 0};
            if (!SendPacket(ping, sizeof(ping))) break;
            ping_pending = true;
            ping_sent = now;
            next_ping = now + std::chrono::seconds(ping_seconds);
        }
        uint8_t fh = 0;
        std::string payload;
        int result;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            if (!running_ || sock_ < 0) break;
            result = ReadMqttPacketLocked(fh, payload);
        }
        if (result < 0) break;
        if (result == 0)
        {
            struct pollfd pfd{sock_, POLLIN, 0};
            int ready = poll(&pfd, 1, 20);
            if (ready < 0 && errno != EINTR) break;
            continue;
        }

        const uint8_t pkt_type = (fh >> 4) & 0x0F;
        switch (pkt_type)
        {
        case 3: /* PUBLISH */
        {
            if (payload.size() < 2)
                break;
            uint8_t qos = (fh >> 1) & 0x03;
            uint16_t topic_len = ntohs(*(uint16_t *)payload.data());
            if ((size_t)topic_len + 2 > payload.size())
                break;
            std::string topic = payload.substr(2, topic_len);
            size_t msg_off = 2 + topic_len;
            if (qos > 0)
            {
                if (msg_off + 2 > payload.size())
                    break;
                msg_off += 2;
            }
            std::string msg = payload.substr(msg_off);
            if (on_message_)
                on_message_(topic, msg);
            break;
        }
        case 13: /* PINGRESP */
            ping_pending = false;
            write(1, "VOICE_PING_OK\n", 14);
            break;
        case 2:  /* CONNACK */
        case 9:  /* SUBACK */
            break;
        default:
            break;
        }
    }

    connected_ = false;
    if (on_disconnected_)
        on_disconnected_();
}

/* ------------------------------------------------------------------ */
/* Udp                                                                 */
/* ------------------------------------------------------------------ */

Udp::Udp() {}

Udp::~Udp()
{
    Stop();
}

bool Udp::Connect(const std::string &host, int port)
{
    if (port <= 0 || port > 65535) return false;
    const auto begin = std::chrono::steady_clock::now();
    static std::string s_dns_host;
    static uint32_t s_dns_addr = 0;
    static int s_dns_network = -1;
    static std::chrono::steady_clock::time_point s_dns_expires{};
    const int network = metalio_app_cellular_active();
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1)
    {
        if (s_dns_addr && s_dns_host == host && s_dns_network == network &&
            begin < s_dns_expires)
            sa.sin_addr.s_addr = s_dns_addr;
        else if (metalio_voice_resolve_ipv4(host.c_str(), &sa.sin_addr.s_addr) < 0)
            return false;
    }
    const auto resolved = std::chrono::steady_clock::now();
    ESP_LOGE(TAG, "voice UDP resolve %lld ms",
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(resolved-begin).count());

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0)
    {
        return false;
    }

    if (metalio_app_bind_active_interface(sock_) < 0)
    {
        close(sock_);
        sock_ = -1;
        return false;
    }

    {
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (connect(sock_, reinterpret_cast<struct sockaddr *>(&sa),
                sizeof(sa)) < 0)
    {
        close(sock_);
        sock_ = -1;
        /* Cached address may be stale — clear so the next open re-resolves. */
        if (s_dns_host == host)
            s_dns_addr = 0;
        return false;
    }
    char peer[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &sa.sin_addr, peer, sizeof(peer));
    ESP_LOGE(TAG, "voice UDP peer %s:%d", peer, port);
    s_dns_host = host;
    s_dns_addr = sa.sin_addr.s_addr;
    s_dns_network = network;
    s_dns_expires = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    ESP_LOGE(TAG, "voice UDP connect %lld ms",
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-resolved).count());
    return true;
}

ssize_t Udp::Send(const std::string &data)
{
    if (sock_ < 0)
        return -1;
    return send(sock_, data.data(), data.size(), 0);
}

void Udp::StartReceive()
{
    running_ = true;
    rx_thread_ = HiTask::LegacyThread([this]()
    {
        // Voice packets must reach the decoder while LVGL paints the first
        // reply. Keep this voice-only receiver above UI work, below decode
        // and speaker output; recv() blocks when no audio is arriving.
        struct sched_param voice_priority{};
        voice_priority.sched_priority = CONFIG_METALIO_APP_PRIORITY + 12;
        pthread_setschedparam(pthread_self(), SCHED_RR, &voice_priority);
        char buf[2048];
        while (running_ && sock_ >= 0)
        {
            ssize_t n = recv(sock_, buf, sizeof(buf), 0);
            if (n > 0)
            {
                if (on_message_)
                    on_message_(std::string(buf, buf + n));
                continue;
            }
            /* SO_RCVTIMEO: keep living through silence so TTS can arrive
             * after the user finishes speaking (STT → LLM → TTS). */
            const int receive_error = n < 0 ? errno : 0;
            if (!running_)
                break;
            if (n < 0 && (receive_error == EAGAIN ||
                          receive_error == EWOULDBLOCK ||
                          receive_error == EINTR))
                continue;
            /* Connected UDP can report a delayed ICMP error while the
             * cellular NAT mapping is being established. That error does
             * not close the socket. Keep receiving on this live session;
             * otherwise uplink continues but every later reply is lost. */
            if (n == 0 || (n < 0 &&
                (receive_error == ECONNREFUSED || receive_error == EHOSTUNREACH ||
                 receive_error == ENETUNREACH || receive_error == ENETDOWN)))
            {
                ESP_LOGW(TAG, "voice UDP transient receive error=%d", receive_error);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            ESP_LOGE(TAG, "voice UDP receiver stopped error=%d", receive_error);
            break;
        }
    });
}

void Udp::Stop()
{
    running_ = false;
    if (sock_ >= 0)
    {
        shutdown(sock_, SHUT_RDWR);
        close(sock_);
        sock_ = -1;
    }
    if (rx_thread_.joinable())
        rx_thread_.join();
}

/* ------------------------------------------------------------------ */
/* MqttProtocol                                                        */
/* ------------------------------------------------------------------ */

MqttProtocol::MqttProtocol()
{
    event_group_handle_ = xEventGroupCreate();

    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void *arg) {
            MqttProtocol *protocol = (MqttProtocol *)arg;
            // LPWORK has a different NuttX task group / descriptor table.
            // Create the network pthread from the application owner, otherwise
            // CONNECT succeeds there but app-side publish sees an invalid fd.
            auto alive = protocol->reconnect_alive_;
            Application::GetInstance().Schedule([protocol, alive]() {
                if (alive->load()) protocol->StartReconnectWorker();
            });
        },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol()
{
    stopping_.store(true);
    reconnect_alive_->store(false);
    client_ready_.store(false);
    ESP_LOGI(TAG, "MqttProtocol deinit");
    if (reconnect_timer_ != nullptr)
    {
        esp_timer_stop(reconnect_timer_);
    }

    {
        std::lock_guard<std::mutex> lock(reconnect_worker_mutex_);
        if (reconnect_joinable_)
        {
            pthread_join(reconnect_worker_, nullptr);
            reconnect_joinable_ = false;
        }
    }
    if (reconnect_timer_)
    {
        // All network callbacks have finished before their timer is freed.
        esp_timer_delete(reconnect_timer_);
        reconnect_timer_ = nullptr;
    }
    udp_.reset();
    mqtt_.reset();

    if (event_group_handle_ != nullptr)
        vEventGroupDelete(event_group_handle_);
}

std::shared_ptr<Mqtt> MqttProtocol::GetMqttClient()
{
    std::lock_guard<std::mutex> lock(mqtt_pointer_mutex_);
    return mqtt_;
}

void MqttProtocol::StartReconnectWorker()
{
    std::lock_guard<std::mutex> lock(reconnect_worker_mutex_);
    if (stopping_.load() || reconnect_running_.load()) return;
    if (reconnect_joinable_)
    {
        pthread_join(reconnect_worker_, nullptr);
        reconnect_joinable_ = false;
    }
    reconnect_running_.store(true);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // TLS handshake retains the large stack previously supplied by OTA.
    pthread_attr_setstacksize(&attr, 131072);
    int rc = hi_task_create_legacy(&reconnect_worker_, &attr, [](void *arg)->void * {
        auto *self = static_cast<MqttProtocol *>(arg);
        self->StartMqttClient(false);
        self->reconnect_running_.store(false);
        return nullptr;
    }, this);
    pthread_attr_destroy(&attr);
    if (rc == 0) reconnect_joinable_ = true;
    else
    {
        reconnect_running_.store(false);
        if (!stopping_.load() && reconnect_timer_)
            esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    }
}

bool MqttProtocol::OnNetworkReady()
{
    if (stopping_.load()) return false;
    network_generation_.fetch_add(1);
    client_ready_.store(false);
    if (reconnect_timer_)
    {
        esp_timer_stop(reconnect_timer_);
        esp_timer_start_once(reconnect_timer_, 1000);
    }
    return true;
}

bool MqttProtocol::Start()
{
    if (network_generation_.load() == 0) OnNetworkReady();
    else if (!client_ready_.load() && !stopping_.load() && reconnect_timer_)
        esp_timer_start_once(reconnect_timer_, 1000);
    return !stopping_.load();
}

bool MqttProtocol::StartMqttClient(bool report_error)
{
    std::lock_guard<std::mutex> start_lock(mqtt_start_mutex_);
    if (stopping_.load()) return false;
    const auto network = network_generation_.load();
    auto client = GetMqttClient();
    if (client != nullptr && client->IsConnected() && client_ready_.load() &&
        connected_generation_.load() == network)
    {
        ESP_LOGE(TAG, "MQTT already connected");
        return true;
    }

    if (client != nullptr)
    {
        ESP_LOGW(TAG, "Mqtt client already started");
        client->Stop();
        client.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    {
        std::lock_guard<std::mutex> lock(mqtt_pointer_mutex_);
        publish_topic_ = settings.GetString("publish_topic");
        subscribe_topic_ = settings.GetString("subscribe_topic");
    }

    if (endpoint.empty())
        endpoint = METALIO_MQTT_DEFAULT_ENDPOINT;

    if (endpoint.empty())
    {
        ESP_LOGE(TAG, "MQTT endpoint is not specified");
        if (report_error)
        {
            SetError("server not found");
        }
        return false;
    }

    ESP_LOGE(TAG, "MQTT broker %s (default port 8883+TLS if no :port)",
             endpoint.c_str());

    if (client_id.empty())
    {
        client_id = SystemInfo::GetMacAddress();
    }

    client_ready_.store(false);
    client = std::make_shared<Mqtt>();
    {
        std::lock_guard<std::mutex> lock(mqtt_pointer_mutex_);
        mqtt_ = client;
    }
    client->SetKeepAlive(keepalive_interval);

    client->OnDisconnected([this]() {
        client_ready_.store(false);
        if (stopping_.load()) return;
        if (on_disconnected_)
            on_disconnected_();
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds",
                 MQTT_RECONNECT_INTERVAL_MS / 1000);
        if (reconnect_timer_)
            esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    });

    client->OnConnected([this]() {
        if (stopping_.load()) return;
        if (reconnect_timer_)
            esp_timer_stop(reconnect_timer_);
    });

    client->OnMessage([this](const std::string &topic, const std::string &payload) {
        if (stopping_.load()) return;
        (void)topic;
        cJSON *root = cJSON_Parse(payload.c_str());
        if (root == nullptr)
        {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON *type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type))
        {
            ESP_LOGE(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0)
        {
            ParseServerHello(root);
        }
        else if (strcmp(type->valuestring, "goodbye") == 0)
        {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s",
                     session_id ? session_id->valuestring : "null");
            /* Match Claw4: never Close/SendText on the MQTT RX thread. */
            const bool sid_match =
                (session_id == nullptr ||
                 session_id_ == session_id->valuestring);
            if (sid_match)
            {
                bool have_channel = false;
                {
                    std::lock_guard<std::mutex> lock(channel_mutex_);
                    have_channel = (udp_ != nullptr) || !session_id_.empty() ||
                                   opening_audio_.load(std::memory_order_acquire);
                }
                if (!have_channel)
                {
                    /* Cloud often bye's after boot MCP with empty session —
                     * ignore; scheduling Close here burned the app thread. */
                    write(1, "MQTT_BYE_IGN\n", 13);
                }
                else
                {
                    write(1, "MQTT_BYE\n", 9);
                    Application::GetInstance().Schedule([this]() {
                        CloseAudioChannel();
                    });
                }
            }
        }
        else if (opening_audio_.load(std::memory_order_acquire) &&
                 (type == nullptr || strcmp(type->valuestring, "mcp") != 0))
        {
            /* Wake OpenAudioChannel: drop stt/tts until hello+UDP are up.
             * Still allow MCP so 打开应用/音量/背光/蓝牙 work during connect. */
            char mark[48];
            int n = snprintf(mark, sizeof(mark), "MQTT_DROP %s\n",
                             type->valuestring ? type->valuestring : "?");
            if (n > 0)
                write(1, mark, (size_t)n);
        }
        else if (on_incoming_json_)
        {
            /* Trace server→device control messages (tts/stt/llm/…). */
            char mark[48];
            int n = snprintf(mark, sizeof(mark), "MQTT_IN %s\n",
                             type->valuestring ? type->valuestring : "?");
            if (n > 0)
                write(1, mark, (size_t)n);
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ESP_LOGI(TAG, "Connecting to endpoint %s", endpoint.c_str());
    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos)
    {
        broker_address = endpoint.substr(0, pos);
        broker_port = std::stoi(endpoint.substr(pos + 1));
    }
    else
    {
        broker_address = endpoint;
    }

    ESP_LOGE(TAG, "MQTT connect %s:%d tls=%d", broker_address.c_str(),
             broker_port, broker_port == 8883 ? 1 : 0);

    if (!client->Connect(broker_address, broker_port, client_id, username, password))
    {
        ESP_LOGE(TAG, "Failed to connect to endpoint, code=%d", client->GetLastError());
        if (report_error) SetError("server not connected");
        if (!stopping_.load() && reconnect_timer_)
            esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
        return false;
    }

    if (stopping_.load()) return false;
    ESP_LOGI(TAG, "Connected to endpoint");
    last_incoming_time_ = std::chrono::steady_clock::now();

    /* Subscribe to the downstream topic so server->device messages reach
     * OnMessage().  Default to publish_topic + "/down" when no explicit
     * subscribe_topic is configured. */
    if (subscribe_topic_.empty() && !publish_topic_.empty())
        subscribe_topic_ = publish_topic_ + "/down";
    if (!subscribe_topic_.empty())
    {
        if (!client->Subscribe(subscribe_topic_))
        {
            if (!stopping_.load() && reconnect_timer_)
                esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
            return false;
        }
        ESP_LOGI(TAG, "Subscribed to %s", subscribe_topic_.c_str());
    }
    if (network_generation_.load() != network)
    {
        client->Stop();
        if (!stopping_.load() && reconnect_timer_)
            esp_timer_start_once(reconnect_timer_, 1000);
        return false;
    }
    connected_generation_.store(network);
    client_ready_.store(true);
    if (!client->IsConnected() || stopping_.load())
    {
        client_ready_.store(false);
        return false;
    }
    write(1, "VOICE_NET_READY\n", 16);
    if (on_connected_) on_connected_();
    return true;
}

bool MqttProtocol::SendText(const std::string &text)
{
    std::shared_ptr<Mqtt> client;
    std::string topic;
    {
        std::lock_guard<std::mutex> lock(mqtt_pointer_mutex_);
        client = mqtt_;
        topic = publish_topic_;
    }
    if (topic.empty())
    {
        ESP_LOGE(TAG, "publish_topic empty — cannot send hello/text");
        write(1, "TX_NO_TOPIC\n", 12);
        return false;
    }
    if (!client || !client->IsConnected())
    {
        write(1, "TX_NO_MQTT\n", 11);
        return false;
    }
    if (!client->Publish(topic, text))
    {
        /* Do not SetError here — a single listen/detect publish glitch was
         * MAIN_EVENT_ERROR → Idle and killed the whole dialogue session. */
        ESP_LOGE(TAG, "Failed to publish message (%u bytes)",
                 (unsigned)text.size());
        write(1, "TX_PUB_FAIL\n", 12);
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet)
{
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr)
    {
        static int s_tx_null;
        if ((++s_tx_null % 17) == 1)
            write(1, "TX_NULL\n", 8);
        return false;
    }

#ifdef METALIO_MQTT_HAVE_AES
    std::string nonce(aes_nonce_);
    *(uint16_t *)&nonce[2] = htons((uint16_t)packet->payload.size());
    *(uint32_t *)&nonce[8] = htonl(packet->timestamp);
    *(uint32_t *)&nonce[12] = htonl(++local_sequence_);

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + packet->payload.size());
    memcpy(&encrypted[0], nonce.data(), nonce.size());

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&aes_ctx_, packet->payload.size(), &nc_off,
                              (uint8_t *)&nonce[0], stream_block,
                              (uint8_t *)packet->payload.data(),
                              (uint8_t *)&encrypted[nonce.size()]) != 0)
    {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        write(1, "TX_AES_FAIL\n", 12);
        return false;
    }
    if (udp_->Send(encrypted) <= 0)
    {
        static int s_tx_fail;
        if ((++s_tx_fail % 17) == 1)
            write(1, "TX_FAIL\n", 8);
        return false;
    }
    {
        static int s_tx_ok;
        if ((++s_tx_ok % 17) == 1)
            write(1, "TX_OK\n", 6);
    }
    return true;
#else
    /* No encryption available — send plain OPUS frame with the same nonce
     * layout so the wire format matches. Debug builds only. */
    std::string frame;
    frame.resize(16 + packet->payload.size());
    memset(&frame[0], 0x01, 1);
    *(uint16_t *)&frame[2] = htons((uint16_t)packet->payload.size());
    *(uint32_t *)&frame[8] = htonl(packet->timestamp);
    *(uint32_t *)&frame[12] = htonl(++local_sequence_);
    memcpy(&frame[16], packet->payload.data(), packet->payload.size());
    if (udp_->Send(frame) <= 0)
    {
        static int s_tx_fail;
        if ((++s_tx_fail % 17) == 1)
            write(1, "TX_FAIL\n", 8);
        return false;
    }
    return true;
#endif
}

void MqttProtocol::CloseAudioChannel()
{
    bool had_udp = false;
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        had_udp = (udp_ != nullptr);
        udp_.reset();
        sid = session_id_;
        session_id_.clear();
    }
    const bool was_opening = opening_audio_.exchange(false, std::memory_order_acq_rel);

    /* Idempotent: no live UDP/session and not mid-open → do not publish
     * goodbye (server bye with empty session was CH_CLOSE↔STXT forever). */
    if (!had_udp && sid.empty() && !was_opening)
    {
        write(1, "CH_CLOSE_NOP\n", 13);
        return;
    }

    write(1, "CH_CLOSE\n", 9);
    if (had_udp || !sid.empty())
    {
        std::string message = "{";
        message += "\"session_id\":\"" + sid + "\",";
        message += "\"type\":\"goodbye\"";
        message += "}";
        SendText(message);
    }

    if (had_udp && on_audio_channel_closed_)
        on_audio_channel_closed_();
}

bool MqttProtocol::OpenAudioChannel()
{
    write(1, "AUD_OPEN\n", 9);
    opening_audio_.store(true, std::memory_order_release);

    if (!client_ready_.load() && !stopping_.load() && reconnect_timer_)
        esp_timer_start_once(reconnect_timer_, 1000);
    const auto prepare_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!client_ready_.load() && !stopping_.load() &&
           std::chrono::steady_clock::now() < prepare_until)
        usleep(10000);
    auto client = GetMqttClient();
    if (!client_ready_.load() || !client || !client->IsConnected())
    {
        /* Never StartMqttClient/TLS from the wake worker — concurrent
         * mbedtls use with the RX thread hangs at WAKE_START and blue-screens.
         * Fail the wake; the reconnect timer will restore MQTT on a safer path. */
        ESP_LOGE(TAG, "MQTT is not connected (wake open refused)");
        write(1, "AUD_MQTT_NEED\n", 14);
        opening_audio_.store(false, std::memory_order_release);
        return false;
    }

    /* Drop any stale UDP session without firing on_audio_channel_closed_
     * (that callback would Schedule→Idle and kill the wake→listen path). */
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (udp_ != nullptr)
        {
            write(1, "AUD_REOPEN\n", 11);
            udp_.reset();
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message))
    {
        ESP_LOGE(TAG, "Failed to send hello (topic empty or publish failed)");
        write(1, "AUD_HELLO_TX_FAIL\n", 18);
        opening_audio_.store(false, std::memory_order_release);
        return false;
    }
    write(1, "AUD_HELLO_TX\n", 13);

    EventBits_t bits = xEventGroupWaitBits(event_group_handle_,
                                           MQTT_PROTOCOL_SERVER_HELLO_EVENT,
                                           pdTRUE, pdFALSE,
                                           8000 / portTICK_PERIOD_MS);
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT))
    {
        ESP_LOGE(TAG, "Failed to receive server hello");
        write(1, "AUD_HELLO_TO\n", 13);
        SetError("server timeout");
        opening_audio_.store(false, std::memory_order_release);
        return false;
    }
    write(1, "AUD_HELLO_OK\n", 13);

    std::unique_lock<std::mutex> lock(channel_mutex_);
    udp_ = std::make_unique<Udp>();
    udp_->OnMessage([this](const std::string &data) {
        /*
         * UDP Encrypted OPUS Packet Format:
         * |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
         * |payload payload_len|
         */
        if (data.size() < 16)
        {
            ESP_LOGE(TAG, "Invalid audio packet size: %u", (unsigned)data.size());
            return;
        }
        if ((uint8_t)data[0] != 0x01)
        {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", (uint8_t)data[0]);
            return;
        }
        uint32_t timestamp = ntohl(*(uint32_t *)&data[8]);
        uint32_t sequence = ntohl(*(uint32_t *)&data[12]);
        if (sequence < remote_sequence_)
        {
            ESP_LOGW(TAG, "Received audio packet with old sequence: %lu, expected: %lu",
                     (unsigned long)sequence, (unsigned long)remote_sequence_);
            return;
        }
        if (sequence != remote_sequence_ + 1)
        {
            ESP_LOGW(TAG, "Received audio packet with wrong sequence: %lu, expected: %lu",
                     (unsigned long)sequence, (unsigned long)remote_sequence_ + 1);
        }

        size_t decrypted_size = data.size() - 16;
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);

#ifdef METALIO_MQTT_HAVE_AES
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        auto nonce = (uint8_t *)data.data();
        auto encrypted = (uint8_t *)data.data() + 16;
        int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, decrypted_size, &nc_off,
                                        nonce, stream_block, encrypted,
                                        (uint8_t *)packet->payload.data());
        if (ret != 0)
        {
            ESP_LOGE(TAG, "Failed to decrypt audio data, ret: %d", ret);
            return;
        }
#else
        memcpy(packet->payload.data(), data.data() + 16, decrypted_size);
#endif
        if (on_incoming_audio_)
            on_incoming_audio_(std::move(packet));
        remote_sequence_ = sequence;
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    if (!udp_->Connect(udp_server_, udp_port_))
    {
        ESP_LOGE(TAG, "UDP connect %s:%d failed", udp_server_.c_str(),
                 udp_port_);
        write(1, "AUD_UDP_FAIL\n", 13);
        udp_.reset();
        SetError("udp connect failed");
        return false;
    }
    write(1, "AUD_UDP_OK\n", 11);
    udp_->StartReceive();
    lock.unlock();

    if (on_audio_channel_opened_)
        on_audio_channel_opened_();
    write(1, "AUD_OPEN_DONE\n", 14);
    /* Keep opening_audio_ set until ClearOpeningAudioGate() after wake
     * listen TX — prevents MCP/STT racing TLS mid-wake (blue screen). */
    return true;
}

void MqttProtocol::ClearOpeningAudioGate()
{
    const bool was = opening_audio_.exchange(false, std::memory_order_acq_rel);
    if (was)
        write(1, "AUD_GATE_OFF\n", 13);
}

std::string MqttProtocol::GetHelloMessage()
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON *features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
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

void MqttProtocol::ParseServerHello(const cJSON *root)
{
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0)
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

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp))
    {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;
    udp_port_ = cJSON_GetObjectItem(udp, "port")->valueint;
    auto key = cJSON_GetObjectItem(udp, "key")->valuestring;
    auto nonce = cJSON_GetObjectItem(udp, "nonce")->valuestring;

    aes_nonce_ = DecodeHexString(nonce);
#ifdef METALIO_MQTT_HAVE_AES
    {
        const std::string key_bin = DecodeHexString(key);
        if (key_bin.size() != 16 || aes_nonce_.size() != 16)
        {
            ESP_LOGE(TAG, "UDP AES key/nonce length bad key=%u nonce=%u",
                     (unsigned)key_bin.size(), (unsigned)aes_nonce_.size());
            write(1, "AUD_AES_BAD\n", 12);
            return;
        }
        mbedtls_aes_init(&aes_ctx_);
        mbedtls_aes_setkey_enc(&aes_ctx_,
                               (const unsigned char *)key_bin.data(), 128);
    }
#endif
    local_sequence_ = 0;
    remote_sequence_ = 0;
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

static inline uint8_t CharToHex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

std::string MqttProtocol::DecodeHexString(const std::string &hex_string)
{
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i + 1 < hex_string.size(); i += 2)
    {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

bool MqttProtocol::IsAudioChannelOpened() const
{
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
