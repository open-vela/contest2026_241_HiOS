/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * OTA firmware update — ported from MetalioClaw4 main/ota.h/.cc.
 *
 * Replaces the ESP-IDF esp_http_client + esp_ota_ops APIs with:
 *   - A minimal HTTP/1.1 client (Http class) over NuttX BSD sockets.
 *   - A flash-write stub (WriteToFlash) that needs to be wired up to the
 *     NuttX flash partition API (boardctl BOARDIOC_FLASH_WRITE or a custom
 *     driver). Until wired, downloaded images are verified and discarded.
 *
 * The wire protocol (server_info JSON, activation flow, version comparison)
 * is preserved verbatim so the existing tenclass OTA server keeps working.
 */

#ifndef METALIO_OTA_H
#define METALIO_OTA_H

#include "esp_err_shim.h"
#include "esp_log_shim.h"

#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <map>

/* ------------------------------------------------------------------ */
/* Minimal HTTP client over BSD sockets                               */
/* ------------------------------------------------------------------ */

struct HttpTlsImpl; /* opaque mbedTLS state, defined in ota.cxx */

class Http
{
public:
    Http();
    ~Http();

    void SetHeader(const std::string &name, const std::string &value);
    void SetContent(std::string content);

    bool Open(const std::string &method, const std::string &url);
    void Close();

    int  GetStatusCode() const { return status_code_; }
    size_t GetBodyLength() const { return content_length_; }
    int  GetLastError() const { return last_error_; }

    /* Read up to `len` bytes into `buf`. Returns bytes read, 0 on EOF,
     * or -1 on error. */
    int  Read(char *buf, size_t len);
    std::string ReadAll();

private:
    int sock_ = -1;
    bool tls_ = false;
    std::unique_ptr<HttpTlsImpl> tls_impl_;
    int status_code_ = 0;
    size_t content_length_ = 0;
    size_t body_read_ = 0;
    int last_error_ = 0;
    std::string method_;
    std::string host_;
    int port_ = 80;
    std::string path_;
    bool chunked_ = false;
    /* chunked-encoding state */
    size_t chunk_remaining_ = 0;
    bool chunk_header_pending_ = true;

    std::map<std::string, std::string> headers_;
    std::string request_body_;
    /* Buffered response body bytes (read ahead while parsing headers). */
    std::string body_buffer_;

    bool ParseUrl(const std::string &url);
    bool SendRequest();
    bool ReadStatusAndHeaders();
    int  ReadChunked(char *buf, size_t len);

    /* Socket / TLS-aware transport. Returns bytes, 0 on EOF (recv), or -1
     * on error (last_error_ is set). */
    int  NetSend(const char *buf, size_t len);
    int  NetRecv(char *buf, size_t len);
    bool TlsSetup();
    void TlsTeardown();
};

using OtaProgressCallback = std::function<void(int progress, size_t downloaded,
                                              size_t total, size_t speed)>;

class Ota
{
public:
    Ota();
    ~Ota();

    esp_err_t CheckVersion();
    esp_err_t Activate();
    bool HasActivationChallenge() const { return has_activation_challenge_; }
    bool HasNewVersion() const { return has_new_version_; }
    bool HasMqttConfig() const { return has_mqtt_config_; }
    bool HasWebsocketConfig() const { return has_websocket_config_; }
    bool HasActivationCode() const { return has_activation_code_; }
    bool HasServerTime() const { return has_server_time_; }
    bool StartUpgrade(OtaProgressCallback callback);
    bool StartUpgradeFromUrl(const std::string &url, OtaProgressCallback callback);
    void MarkCurrentVersionValid();

    const std::string &GetFirmwareVersion() const { return firmware_version_; }
    const std::string &GetCurrentVersion() const { return current_version_; }
    const std::string &GetFirmwareUrl() const { return firmware_url_; }
    const std::string &GetActivationMessage() const { return activation_message_; }
    const std::string &GetActivationCode() const { return activation_code_; }
    std::string GetCheckVersionUrl();

private:
    std::string activation_message_;
    std::string activation_code_;
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string activation_challenge_;
    std::string serial_number_;
    int activation_timeout_ms_ = 30000;

    bool Upgrade(const std::string &firmware_url);
    OtaProgressCallback upgrade_callback_;
    std::vector<int> ParseVersion(const std::string &version);
    bool IsNewVersionAvailable(const std::string &currentVersion,
                               const std::string &newVersion);
    std::string GetActivationPayload();
    std::unique_ptr<Http> SetupHttp();
};

#endif /* METALIO_OTA_H */
