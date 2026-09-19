/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * API endpoint constants — ported from MetalioClaw4 main/api_endpoints.h.
 *
 * This is a pure header of constexpr path strings and small URL-builder
 * helpers used by openclaw_client / ota / protocol layers.  The ESP-IDF
 * esp_log.h include is redirected to the NuttX esp_log_shim.h; everything
 * else is platform-independent.
 */

#pragma once

#include <cstdio>
#include <cstring>
#include <string>

/* Set to 1 to print HTTP request URL, body and response at the OpenClaw
 * call sites. */
#ifndef API_HTTP_DEBUG
#define API_HTTP_DEBUG 0
#endif

#if API_HTTP_DEBUG
#include "esp_log_shim.h"
#endif

namespace api {

#if API_HTTP_DEBUG

namespace detail {

inline const char *HttpBodyForLog(const std::string &body)
{
    return body.empty() ? "(empty)" : body.c_str();
}

}  /* namespace detail */

inline void LogHttpRequest(const char *tag, const char *method,
                           const std::string &url,
                           const std::string &body = std::string(),
                           const char *extra = nullptr)
{
    ESP_LOGI(tag, "[API_HTTP] >>> %s %s", method, url.c_str());
    if (extra != nullptr && extra[0] != '\0')
    {
        ESP_LOGI(tag, "[API_HTTP] >>> %s", extra);
    }
    ESP_LOGI(tag, "[API_HTTP] >>> body=%s", detail::HttpBodyForLog(body));
}

inline void LogHttpBinaryRequest(const char *tag, const char *method,
                                 const std::string &url, size_t body_bytes,
                                 const char *extra = nullptr)
{
    ESP_LOGI(tag, "[API_HTTP] >>> %s %s", method, url.c_str());
    if (extra != nullptr && extra[0] != '\0')
    {
        ESP_LOGI(tag, "[API_HTTP] >>> %s", extra);
    }
    ESP_LOGI(tag, "[API_HTTP] >>> body=(binary %u bytes)",
             static_cast<unsigned>(body_bytes));
}

inline void LogHttpResponse(const char *tag, int status,
                            const std::string &body)
{
    ESP_LOGI(tag, "[API_HTTP] <<< status=%d body=%s", status,
             detail::HttpBodyForLog(body));
}

#else  /* !API_HTTP_DEBUG */

inline void LogHttpRequest(const char *, const char *, const std::string &,
                           const std::string & = std::string(),
                           const char * = nullptr) {}

inline void LogHttpBinaryRequest(const char *, const char *, const std::string &,
                                 size_t, const char * = nullptr) {}

inline void LogHttpResponse(const char *, int, const std::string &) {}

#endif  /* API_HTTP_DEBUG */

/* ------------------------------------------------------------------ */
/* Host and prefix                                                    */
/* ------------------------------------------------------------------ */

/* Default cloud host.  Override at build time with
 *   -DCONFIG_METALIO_API_HOST=\"https://example.com\"
 * if a different backend is required. */
#ifndef CONFIG_METALIO_API_HOST
#define CONFIG_METALIO_API_HOST "http://xxxxx.com"
#endif

constexpr const char *kHost = CONFIG_METALIO_API_HOST;

constexpr const char *kApiV1Prefix = "/api/v1";
constexpr const char *kXiaozhiDevicePrefix = "/xiaozhi/device";

/* ------------------------------------------------------------------ */
/* OpenClaw                                                            */
/* ------------------------------------------------------------------ */

constexpr const char *kOpenClawDeviceStatus =
    "/api/v1/devices/status";
constexpr const char *kOpenClawConversationList =
    "/api/v1/conversation?page=1&size=100";
constexpr const char *kOpenClawUpload = "/api/v1/upload";
constexpr const char *kOpenClawMessagesFmt =
    "/api/v1/conversation/%s/messages?page=1&size=100";
constexpr const char *kOpenClawRemoveAll =
    "/api/v1/conversation/removeAll";
constexpr const char *kOpenClawConversationDeleteFmt =
    "/api/v1/conversation/delete/%s";

/* ------------------------------------------------------------------ */
/* ASR                                                                 */
/* ------------------------------------------------------------------ */

constexpr const char *kAsrTranscribe = "/api/v1/asr/transcribe";
constexpr const char *kAsrAudioRecords =
    "/api/v1/asr/audio-records?originalName=";
/* Device-side raw WAV transcription (octet-stream body). */
constexpr const char *kXiaozhiAsrWav = "/xiaozhi/api/asr?format=wav";

/* ------------------------------------------------------------------ */
/* DashScope text-to-image                                             */
/* ------------------------------------------------------------------ */

constexpr const char *kText2Image = "/xiaozhi/api/dashscope/text2image";
constexpr const char *kText2ImageTaskFmt =
    "/xiaozhi/api/dashscope/text2image/tasks/%s?maxSide=450";

/* ------------------------------------------------------------------ */
/* Sinicloud real-time interpretation: exchange token, returns wsUrl   */
/* ------------------------------------------------------------------ */

constexpr const char *kSinicloudToken = "/xiaozhi/api/sinicloud/token";

/* ------------------------------------------------------------------ */
/* Weather                                                             */
/* ------------------------------------------------------------------ */

constexpr const char *kWeatherDistrictPath =
    "/api/v1/weather/district?dataType=all&districtId=";

/* ------------------------------------------------------------------ */
/* GPS                                                                 */
/* ------------------------------------------------------------------ */

constexpr const char *kGpsLocationReport =
    "/xiaozhi/device/gps/location/report/cell";
constexpr const char *kGpsStaticMap =
    "/xiaozhi/device/gps/location/static-map";

/* ------------------------------------------------------------------ */
/* URL builders                                                        */
/* ------------------------------------------------------------------ */

inline std::string Url(const char *path)
{
    return std::string(kHost) + path;
}

inline std::string WeatherDistrictUrl(const std::string &district_id)
{
    return Url(kWeatherDistrictPath) + district_id;
}

inline std::string OpenClawMessagesUrl(const std::string &conversation_id)
{
    char path[192];
    std::snprintf(path, sizeof(path), kOpenClawMessagesFmt,
                  conversation_id.c_str());
    return Url(path);
}

inline std::string OpenClawConversationDeleteUrl(
    const std::string &conversation_id)
{
    char path[192];
    std::snprintf(path, sizeof(path), kOpenClawConversationDeleteFmt,
                  conversation_id.c_str());
    return Url(path);
}

inline std::string AsrAudioRecordsUrl(const char *original_name)
{
    if (original_name == nullptr || original_name[0] == '\0')
    {
        return Url(kAsrAudioRecords);
    }
    return Url(kAsrAudioRecords) + original_name;
}

inline std::string Text2ImageTaskUrl(const std::string &task_id)
{
    char path[192];
    std::snprintf(path, sizeof(path), kText2ImageTaskFmt, task_id.c_str());
    return Url(path);
}

/* ------------------------------------------------------------------ */
/* Log redaction                                                       */
/*                                                                    */
/* Response bodies may contain full claw-domain URLs (e.g. static map  */
/* URLs).  Strip them before logging to avoid leaking the backend      */
/* hostname.                                                          */
/* ------------------------------------------------------------------ */

inline std::string RedactClawUrlsForLog(const std::string &text)
{
    std::string out = text;
    const size_t prefix_len = std::strlen(kHost);
    size_t pos = 0;
    while ((pos = out.find(kHost, pos)) != std::string::npos)
    {
        size_t end = out.find_first_of("\"' \t\r\n,}", pos + prefix_len);
        if (end == std::string::npos)
        {
            end = out.size();
        }
        out.replace(pos, end - pos, "[redacted]");
        pos += 10;  /* length of "[redacted]" */
    }
    return out;
}

}  /* namespace api */
