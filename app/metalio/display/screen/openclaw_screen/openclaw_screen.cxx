/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * OpenClawScreen — ported from MetalioClaw4
 * main/display/screen/openclaw_screen/openclaw_screen.cc (~2700 lines).
 *
 * Adaptations for openvela/NuttX:
 *   - ESP-IDF headers (esp_log / esp_timer / esp_heap_caps / esp_lv_adapter /
 *     freertos / cJSON) → NuttX shims (esp_log_shim.h / esp_timer_shim.h /
 *     freertos_shim.h / cJSON_compat.h) + lv_lock()/lv_unlock() (LVGL v9
 *     native lock API, replaces esp_lv_adapter_lock/unlock).
 *   - heap_caps_malloc(_, MALLOC_CAP_SPIRAM) → malloc. NuttX's libc heap
 *     is the unified allocator (PSRAM is part of the heap on ESP32-P4).
 *   - Board::GetInstance().GetAudioCodec()->input_channels() check is
 *     dropped — openvela's ES8311 codec is mono, so keep_left_channel_only
 *     is a no-op.
 *   - All OpenClaw HTTP calls (device status / conversation list / upload
 *     / messages / removeAll / delete) are hidden behind an
 *     OpenClawHttpBackend interface with a default
 *     StubOpenClawHttpBackend that returns failure / empty lists. The
 *     full UI flow (list -> detail -> record -> upload -> refresh) is
 *     preserved and exercises end-to-end against the stub.
 *   - The long-running openclaw_worker_task + xTaskNotifyGive pattern is
 *     replaced with per-job tasks (matching recording_screen's pattern).
 *     The stub returns instantly; when a real backend lands, a worker
 *     pool can be reintroduced to avoid per-call task creation.
 *   - HomeScreen::Create() → HomeScreen::CreateStatic().
 *   - TaskHandle_t is pid_t (int) on NuttX; assignments use 0.
 *   - api_endpoints.h (already ported) provides URL builders.
 *
 * Preserved verbatim:
 *   - 720x720 dark-theme layout: list screen (header + scrollable
 *     conversation list with "create" row + total hint + per-item
 *     icon/title/id), detail screen (header + message bubbles + status
 *     label + record button).
 *   - Message bubbles (user=right green, assistant=left grey) with
 *     auto-width, wrap, scroll-to-latest, and max-50 trim.
 *   - Activation-blocked modal (fullscreen mask + card + back button;
 *     shows activation code if pending).
 *   - Clear-confirm dialog (remove-all / delete-one modes).
 *   - Record-and-upload state machine (Idle/Recording/Uploading/Closing)
 *     with tick timer for "已录 X.X 秒" display.
 *   - Auto-refresh timer (3s) for message polling.
 *   - Swipe-back navigation: list->home, detail->list.
 *   - All event handlers (record press/release, list item click, create,
 *     clear, detail-clear, back, screen-unloaded).
 */

#include "openclaw_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "esp_timer_shim.h"
#include "esp_err_shim.h"
#include "freertos_shim.h"
#include "cJSON_compat.h"
#include "screen_util.h"

#include "application.h"
#include "api_endpoints.h"
#include "audio_service.h"
#include "board_shim.h"
#include "ota.h"
#include "system_info.h"
#include "home_screen/home_screen.h"
#include "device_state.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef tskIDLE_PRIORITY
#define tskIDLE_PRIORITY 0
#endif

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char *TAG = "OpenClawScreen";

// 录音参数与缓冲。
constexpr int  kSampleRate        = 16000;
constexpr int  kSamplesPerFrame   = kSampleRate * 60 / 1000;  // 60ms 一帧
constexpr int  kBytesPerSample    = 2;
constexpr int  kMaxRecordSeconds  = 60;
constexpr size_t kMaxRecordBytes  =
    static_cast<size_t>(kSampleRate) * kBytesPerSample * kMaxRecordSeconds;
constexpr int  kMinRecordMs       = 300;

// ---------------------------------------------------------------------------
// 720x720 暗黑主题布局
// ---------------------------------------------------------------------------
constexpr int32_t kPanelW       = 720;
constexpr int32_t kPanelH       = 720;
constexpr int32_t kHeaderH      = 88;
constexpr int32_t kBackBtnSize  = 72;
constexpr int32_t kFooterH      = 140;
constexpr int32_t kListH        = kPanelH - kHeaderH - kFooterH;
constexpr int32_t kRefreshIntervalMs = 3000;
constexpr int32_t kListPadH     = 18;
constexpr int32_t kBubblePadX   = 18;
constexpr int32_t kBubblePadY   = 14;
constexpr int32_t kBubbleRadius = 18;
constexpr int32_t kSideMargin   = 8;
constexpr int32_t kRowGap       = 12;
constexpr int32_t kMaxMessages  = 50;

constexpr uint32_t kColorBg          = 0x0E1116;
constexpr uint32_t kColorHeaderBg    = 0x12151C;
constexpr uint32_t kColorDivider     = 0x2A2F3A;
constexpr uint32_t kColorHeaderText  = 0xFFFFFF;
constexpr uint32_t kColorHeaderBtn   = 0x2A2F3A;
constexpr uint32_t kColorHeaderBtnBorder = 0x3B4556;
constexpr uint32_t kColorHeaderBtnText   = 0xE5E7EB;
constexpr uint32_t kColorRightBubble = 0x1E3A2F;
constexpr uint32_t kColorLeftBubble  = 0x202736;
constexpr uint32_t kColorBubbleText  = 0xE8F5E9;
constexpr uint32_t kColorLeftBubbleText = 0xE5E7EB;
constexpr uint32_t kColorHintText    = 0x9AA3B2;
constexpr uint32_t kColorErrorText   = 0xF87171;

constexpr uint32_t kColorRecordBtnIdle   = 0x2563EB;
constexpr uint32_t kColorRecordBtnActive = 0xDC2626;
constexpr uint32_t kColorRecordBtnBusy   = 0x4B5563;

constexpr const char kEmptyHint[] = "开始与龙虾对话吧！";

// ---------------------------------------------------------------------------
// 屏幕级状态机
// ---------------------------------------------------------------------------
enum class State : uint8_t {
    Idle,
    Recording,
    Uploading,
    Closing,
};

std::atomic<State>    s_state{State::Idle};
std::atomic<int64_t>  s_record_start_us{0};
std::atomic<bool>     s_stop_requested{false};

// 详情页静态引用
lv_obj_t *s_detail_screen = nullptr;
lv_obj_t *s_msg_list    = nullptr;
lv_obj_t *s_empty_hint  = nullptr;
lv_obj_t *s_record_btn  = nullptr;
lv_obj_t *s_record_lbl  = nullptr;
lv_obj_t *s_status_lbl  = nullptr;
lv_obj_t *s_detail_title_lbl = nullptr;
lv_obj_t *s_detail_id_lbl = nullptr;
lv_timer_t *s_tick_timer = nullptr;
lv_timer_t *s_auto_refresh_timer = nullptr;

// 会话列表页
lv_obj_t *s_list_screen = nullptr;
lv_obj_t *s_list_container = nullptr;
lv_obj_t *s_list_hint = nullptr;
lv_obj_t *s_list_clear_btn = nullptr;
lv_timer_t *s_activation_guard_timer = nullptr;

// 录音 task 句柄
TaskHandle_t s_record_task = 0;

// 历史会话拉取：session 递增使旧 task 的结果失效。
std::atomic<uint32_t> s_history_session{0};
std::atomic<bool>     s_history_loading{false};
std::atomic<bool>     s_list_loading{false};
std::atomic<bool>     s_service_available{false};
std::string           s_conversation_id;
std::string           s_conversation_title;

struct ConversationRecord {
    std::string conversation_id;
    std::string title;
};

struct RefreshSnapshot {
    bool valid = false;
    bool service_ok = false;
    bool bridge_online = false;
    bool gateway_online = false;
    std::string conversation_id;
    std::string messages_json;
};
RefreshSnapshot s_last_refresh_snapshot;

struct HistoryMessage {
    std::string text;
    bool is_user = true;
};

// 清空 / 删除会话确认对话框
enum class ClearDialogMode : uint8_t {
    RemoveAll,
    DeleteOne,
};

struct ClearDialogUi {
    lv_obj_t *mask = nullptr;
    ClearDialogMode mode = ClearDialogMode::RemoveAll;
};
ClearDialogUi s_clear_dlg;

struct ConvItemCtx {
    std::string conversation_id;
    std::string title;
};

std::atomic<uint32_t> s_list_session{0};
std::atomic<bool>     s_navigating_within_openclaw{false};

// OpenClaw 所有 HTTP 串行化，避免 worker 与录音上传 task 并发触发。
std::mutex            s_openclaw_http_mutex;

// 未激活拦截
struct ActivationBlockedDialogUi {
    lv_obj_t *mask = nullptr;
};
ActivationBlockedDialogUi s_activation_dlg;
bool s_activation_blocked = false;
bool s_activation_dialog_shows_code = false;
screen_lifecycle_cb_t s_lifecycle_cb = nullptr;

const lv_font_t *chat_font() { return &font_puhui_30_4; }

// 把 (part | state) 显式转成 lv_style_selector_t，规避
// -Wdeprecated-enum-enum-conversion 告警。
inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

// ---------------------------------------------------------------------------
// LVGL lock helpers — replace esp_lv_adapter_lock(-1)/unlock with LVGL v9
// native API.
// ---------------------------------------------------------------------------
inline esp_err_t LvglLock() {
    lv_lock();
    return ESP_OK;
}
inline void LvglUnlock() {
    lv_unlock();
}

// ---------------------------------------------------------------------------
// HTTP backend interface
//
// All OpenClaw cloud calls go through this interface. The default
// StubOpenClawHttpBackend returns failure / empty results, which drives
// the existing "龙虾不在线" / "上传失败" / "加载失败" UI paths. When the
// real OpenClawClient grows the full REST surface, swap in a real
// implementation.
// ---------------------------------------------------------------------------
struct DeviceStatusResult {
    bool ok = false;
    bool bridge_online = false;
    bool gateway_online = false;
    std::string hint;
    std::string err;
};

struct ConversationListFetchResult {
    bool ok = false;
    int total = 0;
    std::vector<ConversationRecord> records;
    std::string err;
};

struct MessagesFetchResult {
    bool ok = false;
    std::string raw_body;
    std::string err;
};

struct UploadResult {
    bool ok = false;
    int  status = 0;
    std::string text;
    std::string conversation_id;
    std::string err;
};

struct DeleteResult {
    bool ok = false;
    int status = 0;
    std::string err;
    std::string body;
};

class OpenClawHttpBackend {
public:
    virtual ~OpenClawHttpBackend() = default;
    virtual DeviceStatusResult FetchDeviceStatus() = 0;
    virtual ConversationListFetchResult FetchConversationList() = 0;
    virtual MessagesFetchResult FetchMessages(const std::string &conversation_id) = 0;
    virtual UploadResult UploadWav(const uint8_t *wav, size_t size,
                                   const std::string &conversation_id) = 0;
    virtual DeleteResult RemoveAllConversations() = 0;
    virtual DeleteResult DeleteConversation(const std::string &conversation_id) = 0;
};

class StubOpenClawHttpBackend : public OpenClawHttpBackend {
public:
    DeviceStatusResult FetchDeviceStatus() override {
        DeviceStatusResult r;
        // TODO(openvela): wire through OpenClawClient once it grows the
        // /api/v1/devices/status endpoint.
        r.err = "network stack not yet wired";
        return r;
    }

    ConversationListFetchResult FetchConversationList() override {
        ConversationListFetchResult r;
        r.err = "network stack not yet wired";
        return r;
    }

    MessagesFetchResult FetchMessages(const std::string & /*conversation_id*/) override {
        MessagesFetchResult r;
        r.ok = true;  // empty messages body is valid (new conversation)
        return r;
    }

    UploadResult UploadWav(const uint8_t * /*wav*/, size_t /*size*/,
                           const std::string & /*conversation_id*/) override {
        UploadResult r;
        r.err = "network stack not yet wired";
        return r;
    }

    DeleteResult RemoveAllConversations() override {
        DeleteResult r;
        r.err = "network stack not yet wired";
        return r;
    }

    DeleteResult DeleteConversation(const std::string & /*conversation_id*/) override {
        DeleteResult r;
        r.err = "network stack not yet wired";
        return r;
    }
};

// ---------------------------------------------------------------------------
// Real OpenClaw backend — talks to the live cloud through the BSD-socket
// Http class (ota.h) + api_endpoints.h URL builders.  JSON parsing mirrors
// the reference firmware (openclaw_screen.cc) so the wire format is identical.
// ---------------------------------------------------------------------------

struct HttpGetResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string err;
};

static bool parse_api_code_200(const cJSON *root) {
    if (root == nullptr) {
        return false;
    }
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    return cJSON_IsNumber(code) && code->valueint == 200;
}

// device_id_header() 定义在本文件后部「工具」区（缓存 SystemInfo::GetMacAddress()），
// 这里提前声明，使 HTTP 头与参考固件一致地复用它。
const std::string &device_id_header();

static std::unique_ptr<Http> create_http_client(std::string &err_out) {
    auto http = std::make_unique<Http>();
    if (http == nullptr) {
        err_out = "create http failed";
        return nullptr;
    }
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Connection", "close");
    http->SetHeader("X-Device-Id", device_id_header());
    return http;
}

static HttpGetResult http_get_json(const std::string &url) {
    HttpGetResult r;
    std::lock_guard<std::mutex> lock(s_openclaw_http_mutex);
    auto http = create_http_client(r.err);
    if (http == nullptr) {
        return r;
    }
    api::LogHttpRequest(TAG, "GET", url);
    if (!http->Open("GET", url)) {
        r.err = "open failed";
        api::LogHttpResponse(TAG, -1, r.err);
        http->Close();
        return r;
    }
    r.status = http->GetStatusCode();
    if (r.status != 200) {
        r.err = "status " + std::to_string(r.status);
        r.body = http->ReadAll();
        api::LogHttpResponse(TAG, r.status, api::RedactClawUrlsForLog(r.body));
        http->Close();
        return r;
    }
    r.body = http->ReadAll();
    api::LogHttpResponse(TAG, r.status, api::RedactClawUrlsForLog(r.body));
    http->Close();
    r.ok = !r.body.empty();
    if (!r.ok) {
        r.err = "empty body";
    }
    return r;
}

// 解析 /upload 响应：200 + JSON code==0，或纯文本 "OK ..."。
static void parse_upload_response(int status, const std::string &body,
                                  UploadResult &out) {
    out.status = status;
    if (status != 200) {
        out.err = body.empty() ? ("HTTP " + std::to_string(status)) : body;
        return;
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (root != nullptr) {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (cJSON_IsNumber(code) && code->valueint == 0) {
            cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
            if (cJSON_IsObject(data)) {
                cJSON *text = cJSON_GetObjectItemCaseSensitive(data, "text");
                if (cJSON_IsString(text) && text->valuestring != nullptr &&
                    text->valuestring[0] != '\0') {
                    out.text = text->valuestring;
                }
                cJSON *conv_id =
                    cJSON_GetObjectItemCaseSensitive(data, "conversationId");
                if (cJSON_IsString(conv_id) && conv_id->valuestring != nullptr &&
                    conv_id->valuestring[0] != '\0') {
                    out.conversation_id = conv_id->valuestring;
                }
            }
            out.ok = true;
            cJSON_Delete(root);
            return;
        }
        cJSON_Delete(root);
    }
    size_t b = 0, e = body.size();
    while (b < e && (body[b] == ' ' || body[b] == '\r' || body[b] == '\n' ||
                     body[b] == '\t')) {
        ++b;
    }
    while (e > b && (body[e - 1] == ' ' || body[e - 1] == '\r' ||
                     body[e - 1] == '\n' || body[e - 1] == '\t')) {
        --e;
    }
    const std::string trimmed = body.substr(b, e - b);
    if (trimmed.size() >= 3 && trimmed.compare(0, 3, "OK ") == 0) {
        out.ok = true;
        return;
    }
    out.err = trimmed.empty() ? "empty response" : trimmed;
}

class RealOpenClawHttpBackend : public OpenClawHttpBackend {
public:
    DeviceStatusResult FetchDeviceStatus() override {
        DeviceStatusResult r;
        HttpGetResult http = http_get_json(api::Url(api::kOpenClawDeviceStatus));
        if (!http.ok) {
            r.err = http.err;
            return r;
        }
        cJSON *root = cJSON_Parse(http.body.c_str());
        if (root == nullptr || !parse_api_code_200(root)) {
            r.err = "invalid status response";
            if (root != nullptr) {
                cJSON_Delete(root);
            }
            return r;
        }
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsObject(data)) {
            cJSON *bridge = cJSON_GetObjectItemCaseSensitive(data, "bridgeOnline");
            cJSON *gateway = cJSON_GetObjectItemCaseSensitive(data, "gatewayOnline");
            r.bridge_online = cJSON_IsTrue(bridge);
            r.gateway_online = cJSON_IsTrue(gateway);
            cJSON *hint = cJSON_GetObjectItemCaseSensitive(data, "hint");
            if (cJSON_IsString(hint) && hint->valuestring != nullptr &&
                hint->valuestring[0] != '\0') {
                r.hint = hint->valuestring;
            }
            r.ok = true;
        } else {
            r.err = "missing data";
        }
        cJSON_Delete(root);
        return r;
    }

    ConversationListFetchResult FetchConversationList() override {
        ConversationListFetchResult r;
        HttpGetResult http = http_get_json(api::Url(api::kOpenClawConversationList));
        if (!http.ok) {
            r.err = http.err;
            return r;
        }
        cJSON *root = cJSON_Parse(http.body.c_str());
        if (root == nullptr || !parse_api_code_200(root)) {
            r.err = "invalid conversation list response";
            if (root != nullptr) {
                cJSON_Delete(root);
            }
            return r;
        }
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        cJSON *total_item =
            data ? cJSON_GetObjectItemCaseSensitive(data, "total") : nullptr;
        cJSON *records =
            data ? cJSON_GetObjectItemCaseSensitive(data, "records") : nullptr;
        if (!cJSON_IsObject(data) || !cJSON_IsNumber(total_item) ||
            !cJSON_IsArray(records)) {
            r.err = "missing conversation list data";
            cJSON_Delete(root);
            return r;
        }
        r.total = total_item->valueint;
        const int count = cJSON_GetArraySize(records);
        for (int i = 0; i < count; ++i) {
            cJSON *item = cJSON_GetArrayItem(records, i);
            if (item == nullptr) {
                continue;
            }
            cJSON *conv_id =
                cJSON_GetObjectItemCaseSensitive(item, "conversationId");
            if (!cJSON_IsString(conv_id) || conv_id->valuestring == nullptr ||
                conv_id->valuestring[0] == '\0') {
                continue;
            }
            ConversationRecord rec;
            rec.conversation_id = conv_id->valuestring;
            cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
            if (cJSON_IsString(title) && title->valuestring != nullptr &&
                title->valuestring[0] != '\0') {
                rec.title = title->valuestring;
            }
            r.records.push_back(std::move(rec));
        }
        r.ok = true;
        cJSON_Delete(root);
        return r;
    }

    MessagesFetchResult FetchMessages(
        const std::string &conversation_id) override {
        MessagesFetchResult r;
        if (conversation_id.empty()) {
            r.ok = true;  // 新会话，无历史消息是合法状态。
            return r;
        }
        HttpGetResult http = http_get_json(api::OpenClawMessagesUrl(conversation_id));
        if (!http.ok) {
            r.err = http.err;
            return r;
        }
        r.raw_body = std::move(http.body);
        r.ok = true;
        return r;
    }

    UploadResult UploadWav(const uint8_t *wav, size_t size,
                           const std::string &conversation_id) override {
        UploadResult r;
        if (wav == nullptr || size == 0) {
            r.err = "empty wav";
            return r;
        }
        const std::string url = api::Url(api::kOpenClawUpload);
        std::lock_guard<std::mutex> lock(s_openclaw_http_mutex);
        auto http = create_http_client(r.err);
        if (http == nullptr) {
            return r;
        }
        http->SetHeader("Content-Type", "audio/wav");
        if (!conversation_id.empty()) {
            http->SetHeader("conversationId", conversation_id);
        }
        http->SetContent(std::string(reinterpret_cast<const char *>(wav), size));
        api::LogHttpBinaryRequest(TAG, "POST", url, size);
        if (!http->Open("POST", url)) {
            r.err = "open failed";
            api::LogHttpResponse(TAG, -1, r.err);
            http->Close();
            return r;
        }
        r.status = http->GetStatusCode();
        const std::string body = http->ReadAll();
        api::LogHttpResponse(TAG, r.status, body);
        http->Close();
        parse_upload_response(r.status, body, r);
        return r;
    }

    DeleteResult RemoveAllConversations() override {
        return http_delete(api::Url(api::kOpenClawRemoveAll));
    }

    DeleteResult DeleteConversation(const std::string &conversation_id) override {
        return http_delete(api::OpenClawConversationDeleteUrl(conversation_id));
    }

private:
    static DeleteResult http_delete(const std::string &url) {
        DeleteResult r;
        std::lock_guard<std::mutex> lock(s_openclaw_http_mutex);
        auto http = create_http_client(r.err);
        if (http == nullptr) {
            return r;
        }
        api::LogHttpRequest(TAG, "GET", url);
        if (!http->Open("GET", url)) {
            r.err = "open failed";
            api::LogHttpResponse(TAG, -1, r.err);
            http->Close();
            return r;
        }
        r.status = http->GetStatusCode();
        r.body = http->ReadAll();
        api::LogHttpResponse(TAG, r.status, api::RedactClawUrlsForLog(r.body));
        http->Close();
        if (r.status != 200) {
            r.err = r.body.empty() ? ("status " + std::to_string(r.status)) : r.body;
            return r;
        }
        cJSON *root = cJSON_Parse(r.body.c_str());
        if (root != nullptr) {
            if (parse_api_code_200(root)) {
                r.ok = true;
                cJSON_Delete(root);
                return r;
            }
            cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
            if (cJSON_IsString(msg) && msg->valuestring != nullptr &&
                msg->valuestring[0] != '\0') {
                r.err = msg->valuestring;
            } else {
                r.err = "code != 200";
            }
            cJSON_Delete(root);
            return r;
        }
        r.ok = true;
        return r;
    }
};

OpenClawHttpBackend *g_openclaw_http = nullptr;

OpenClawHttpBackend *GetHttp() {
    if (g_openclaw_http == nullptr) {
        static RealOpenClawHttpBackend s_real;
        g_openclaw_http = &s_real;
    }
    return g_openclaw_http;
}

// 未来注入 AI Agent 后端用；当前未调用，标记 unused 以静默告警。
__attribute__((unused)) void SetOpenClawHttpBackend(OpenClawHttpBackend *backend) {
    g_openclaw_http = backend;
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
bool is_device_activated() {
    return Application::GetInstance().IsDeviceActivated();
}

void log_activation_blocked() {
    auto &app = Application::GetInstance();
    ESP_LOGW(TAG, "OpenClaw blocked: device not activated (boot_ready=%d pending=%d state=%d)",
             app.IsBootReady() ? 1 : 0,
             app.HasPendingActivation() ? 1 : 0,
             static_cast<int>(app.GetDeviceState()));
    if (app.HasPendingActivation()) {
        ESP_LOGW(TAG, "pending activation code: %s",
                 app.GetPendingActivationCode().c_str());
    }
}

bool is_detail_screen_alive() { return s_detail_screen != nullptr; }
bool is_list_screen_alive() { return s_list_screen != nullptr; }

const std::string &device_id_header() {
    static const std::string kId = SystemInfo::GetMacAddress();
    return kId;
}

// 给录音缓冲贴一个标准 RIFF/WAV 头。data_bytes 是纯 PCM 字节长度。
void fill_wav_header(uint8_t *hdr, uint32_t data_bytes) {
    const uint32_t sample_rate = kSampleRate;
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint32_t byte_rate = sample_rate * channels * bits / 8;
    const uint16_t block_align = channels * bits / 8;
    const uint32_t riff_size = 36 + data_bytes;
    auto put32 = [](uint8_t *p, uint32_t v) {
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
        p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
    };
    auto put16 = [](uint8_t *p, uint16_t v) {
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    };
    std::memcpy(hdr + 0, "RIFF", 4);
    put32(hdr + 4, riff_size);
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    put32(hdr + 16, 16);                // fmt chunk size
    put16(hdr + 20, 1);                 // PCM
    put16(hdr + 22, channels);
    put32(hdr + 24, sample_rate);
    put32(hdr + 28, byte_rate);
    put16(hdr + 32, block_align);
    put16(hdr + 34, bits);
    std::memcpy(hdr + 36, "data", 4);
    put32(hdr + 40, data_bytes);
}

// ---------------------------------------------------------------------------
// JSON 解析（messages 列表）
// ---------------------------------------------------------------------------
bool parse_messages_json(const std::string &body,
                         std::vector<HistoryMessage> &out) {
    out.clear();
    cJSON *root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 200) {
        cJSON_Delete(root);
        return false;
    }

    bool ok = false;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *records = data ? cJSON_GetObjectItemCaseSensitive(data, "records")
                          : nullptr;
    if (cJSON_IsArray(records)) {
        const int count = cJSON_GetArraySize(records);
        for (int i = 0; i < count; ++i) {
            cJSON *item = cJSON_GetArrayItem(records, i);
            if (item == nullptr) {
                continue;
            }
            cJSON *content_item =
                cJSON_GetObjectItemCaseSensitive(item, "content");
            cJSON *role_item =
                cJSON_GetObjectItemCaseSensitive(item, "role");
            if (!cJSON_IsString(content_item) ||
                content_item->valuestring == nullptr ||
                content_item->valuestring[0] == '\0') {
                continue;
            }
            HistoryMessage msg;
            msg.text = content_item->valuestring;
            msg.is_user =
                cJSON_IsString(role_item) &&
                role_item->valuestring != nullptr &&
                std::strcmp(role_item->valuestring, "user") == 0;
            out.push_back(std::move(msg));
        }
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

// ---------------------------------------------------------------------------
// 消息气泡（user=右，assistant/其它=左）
// ---------------------------------------------------------------------------
void clear_messages();
void update_empty_hint() {
    if (s_empty_hint == nullptr || s_msg_list == nullptr) return;
    if (lv_obj_get_child_count(s_msg_list) == 0) {
        lv_obj_remove_flag(s_empty_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void scroll_to_latest() {
    if (s_msg_list == nullptr) return;
    const uint32_t count = lv_obj_get_child_count(s_msg_list);
    if (count == 0) return;
    lv_obj_t *latest = lv_obj_get_child(s_msg_list, count - 1);
    if (latest != nullptr) {
        lv_obj_scroll_to_view_recursive(latest, LV_ANIM_ON);
    }
}

void trim_old_msgs() {
    if (s_msg_list == nullptr) return;
    while (static_cast<int32_t>(lv_obj_get_child_count(s_msg_list)) >
           kMaxMessages) {
        lv_obj_t *oldest = lv_obj_get_child(s_msg_list, 0);
        if (oldest == nullptr) break;
        lv_obj_delete(oldest);
    }
}

void add_bubble(const char *text, bool is_user) {
    if (s_msg_list == nullptr || text == nullptr || text[0] == '\0') return;

    const lv_font_t *font = chat_font();
    const uint32_t bubble_bg =
        is_user ? kColorRightBubble : kColorLeftBubble;
    const uint32_t text_color =
        is_user ? kColorBubbleText : kColorLeftBubbleText;

    lv_obj_t *row = lv_obj_create(s_msg_list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, kRowGap, LV_PART_MAIN);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, kBubbleRadius, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bubble, kBubblePadX, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bubble, kBubblePadY, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t max_bubble_w = kPanelW * 72 / 100;
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t text_w = size.x;
    if (text_w < 24) text_w = 24;
    int32_t bubble_w = text_w + kBubblePadX * 2;
    if (bubble_w > max_bubble_w) bubble_w = max_bubble_w;
    lv_obj_set_width(bubble, bubble_w);

    lv_obj_set_style_bg_color(bubble, lv_color_hex(bubble_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    if (is_user) {
        lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, -kSideMargin, 0);
    } else {
        lv_obj_align(bubble, LV_ALIGN_TOP_LEFT, kSideMargin, 0);
    }

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, bubble_w - kBubblePadX * 2);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
    lv_obj_update_layout(label);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    screen_make_input_passive(row);
}

void finalize_message_list_update() {
    trim_old_msgs();
    update_empty_hint();
    if (s_msg_list != nullptr) {
        lv_obj_update_layout(s_msg_list);
    }
    scroll_to_latest();
}

void add_right_bubble(const char *text) {
    add_bubble(text, true);
    finalize_message_list_update();
}

// 在「非 LVGL 线程」里安全地往屏幕上加气泡 / 改文字。
void post_bubble_from_worker(const std::string &text) {
    if (text.empty()) return;
    if (LvglLock() != ESP_OK) return;
    if (is_detail_screen_alive()) {
        add_right_bubble(text.c_str());
    }
    LvglUnlock();
}

void post_status_from_worker(const char *text, uint32_t color) {
    if (LvglLock() != ESP_OK) return;
    if (is_detail_screen_alive() && s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, text);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(color),
                                    LV_PART_MAIN);
    }
    LvglUnlock();
}

// ---------------------------------------------------------------------------
// Refresh snapshot (avoids redundant UI rebuilds when data is unchanged)
// ---------------------------------------------------------------------------
bool refresh_snapshot_unchanged(bool service_ok, bool bridge_online,
                                bool gateway_online,
                                const std::string &conversation_id,
                                const std::string &messages_json) {
    if (!s_last_refresh_snapshot.valid) {
        return false;
    }
    return s_last_refresh_snapshot.service_ok == service_ok &&
           s_last_refresh_snapshot.bridge_online == bridge_online &&
           s_last_refresh_snapshot.gateway_online == gateway_online &&
           s_last_refresh_snapshot.conversation_id == conversation_id &&
           s_last_refresh_snapshot.messages_json == messages_json;
}

void save_refresh_snapshot(bool service_ok, bool bridge_online,
                           bool gateway_online,
                           const std::string &conversation_id,
                           const std::string &messages_json) {
    s_last_refresh_snapshot.valid = true;
    s_last_refresh_snapshot.service_ok = service_ok;
    s_last_refresh_snapshot.bridge_online = bridge_online;
    s_last_refresh_snapshot.gateway_online = gateway_online;
    s_last_refresh_snapshot.conversation_id = conversation_id;
    s_last_refresh_snapshot.messages_json = messages_json;
}

void invalidate_refresh_snapshot() {
    s_last_refresh_snapshot = RefreshSnapshot{};
}

void apply_history_locked(const std::vector<HistoryMessage> &messages) {
    clear_messages();
    for (const auto &msg : messages) {
        add_bubble(msg.text.c_str(), msg.is_user);
    }
    finalize_message_list_update();
}

std::string build_service_unavailable_message(bool bridge_online,
                                              bool gateway_online) {
    if (!bridge_online && !gateway_online) {
        return I18n::T("龙虾插件不在线，龙虾不在线");
    }
    if (!bridge_online) {
        return I18n::T("龙虾插件不在线");
    }
    if (!gateway_online) {
        return I18n::T("龙虾不在线");
    }
    return "";
}

std::string device_status_unavailable_message(
    const DeviceStatusResult &status_res) {
    if (!status_res.hint.empty()) {
        return status_res.hint;
    }
    return build_service_unavailable_message(status_res.bridge_online,
                                             status_res.gateway_online);
}

// ---------------------------------------------------------------------------
// Fetch history task (per-job, replaces the long-running worker)
// ---------------------------------------------------------------------------
struct FetchHistoryCtx {
    uint32_t session;
    bool update_status;
};

void fetch_history_task(void *arg) {
    std::unique_ptr<FetchHistoryCtx> ctx(static_cast<FetchHistoryCtx *>(arg));

    std::lock_guard<std::mutex> http_lock(s_openclaw_http_mutex);
    DeviceStatusResult status_res = GetHttp()->FetchDeviceStatus();
    std::vector<HistoryMessage> messages;
    bool messages_ok = false;
    std::string messages_json;
    const std::string conversation_id = s_conversation_id;
    bool service_ok = false;
    std::string status_msg;
    const bool bridge_online =
        status_res.ok && status_res.bridge_online;
    const bool gateway_online =
        status_res.ok && status_res.gateway_online;

    if (status_res.ok && status_res.bridge_online && status_res.gateway_online) {
        service_ok = true;
        MessagesFetchResult mf = GetHttp()->FetchMessages(conversation_id);
        messages_ok = mf.ok;
        messages_json = mf.raw_body;
        if (messages_ok && !messages_json.empty()) {
            if (!parse_messages_json(messages_json, messages)) {
                messages_ok = false;
            }
        }
    } else if (status_res.ok) {
        status_msg = device_status_unavailable_message(status_res);
    } else {
        status_msg = I18n::T("检查在线状态失败: ") + status_res.err;
    }

    s_history_loading.store(false);

    if (ctx->session != s_history_session.load(std::memory_order_relaxed)) {
        return;
    }

    s_service_available.store(service_ok);

    const bool data_ready = service_ok && messages_ok;
    if (data_ready &&
        refresh_snapshot_unchanged(service_ok, bridge_online, gateway_online,
                                 conversation_id, messages_json)) {
        ESP_LOGI(TAG, "refresh skipped: data unchanged");
        return;
    }

    if (LvglLock() != ESP_OK) {
        return;
    }
    if (is_detail_screen_alive()) {
        if (service_ok) {
            if (messages_ok) {
                apply_history_locked(messages);
                save_refresh_snapshot(service_ok, bridge_online,
                                      gateway_online, conversation_id,
                                      messages_json);
                if (s_status_lbl != nullptr) {
                    if (messages.empty()) {
                        lv_label_set_text(s_status_lbl, I18n::T("按住说话"));
                    } else {
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), I18n::T("已加载 %u 条消息"),
                                      static_cast<unsigned>(messages.size()));
                        lv_label_set_text(s_status_lbl, buf);
                    }
                    lv_obj_set_style_text_color(
                        s_status_lbl, lv_color_hex(kColorHintText),
                        LV_PART_MAIN);
                }
            } else {
                if (s_status_lbl != nullptr) {
                    lv_label_set_text(s_status_lbl, I18n::T("加载消息失败"));
                    lv_obj_set_style_text_color(
                        s_status_lbl, lv_color_hex(kColorErrorText),
                        LV_PART_MAIN);
                }
            }
            if (s_record_btn != nullptr) {
                lv_obj_set_style_bg_color(s_record_btn,
                                          lv_color_hex(kColorRecordBtnIdle),
                                          LV_PART_MAIN);
                lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            }
        } else {
            if (s_status_lbl != nullptr) {
                lv_label_set_text(s_status_lbl, status_msg.c_str());
                lv_obj_set_style_text_color(s_status_lbl,
                                            lv_color_hex(kColorErrorText),
                                            LV_PART_MAIN);
            }
            save_refresh_snapshot(false, bridge_online, gateway_online, "",
                                  "");
            if (s_record_btn != nullptr) {
                lv_obj_remove_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_style_bg_color(s_record_btn,
                                          lv_color_hex(kColorRecordBtnBusy),
                                          LV_PART_MAIN);
            }
        }
    }
    LvglUnlock();
}

void trigger_fetch_history(bool update_status) {
    if (!is_detail_screen_alive() || s_state.load() == State::Closing) {
        return;
    }
    if (s_activation_blocked) {
        ESP_LOGW(TAG, "fetch history skipped: device not activated");
        return;
    }
    if (s_history_loading.exchange(true)) {
        return;
    }

    const uint32_t session =
        s_history_session.fetch_add(1, std::memory_order_relaxed) + 1;

    if (update_status && s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, I18n::T("正在检查龙虾状态…"));
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
    }

    auto *ctx = new FetchHistoryCtx{session, update_status};
    if (xTaskCreate(fetch_history_task, "oc_fetch", 8192, ctx, 4, nullptr) != pdPASS) {
        delete ctx;
        s_history_loading.store(false);
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, I18n::T("无法启动加载任务"));
            lv_obj_set_style_text_color(s_status_lbl,
                                        lv_color_hex(kColorErrorText),
                                        LV_PART_MAIN);
        }
    }
}

void on_auto_refresh_timer(lv_timer_t * /*t*/) {
    if (!is_detail_screen_alive() || s_activation_blocked) {
        return;
    }
    if (s_state.load() != State::Idle) {
        return;
    }
    trigger_fetch_history(false);
}

void start_auto_refresh_timer() {
    if (s_auto_refresh_timer != nullptr) {
        return;
    }
    s_auto_refresh_timer =
        lv_timer_create(on_auto_refresh_timer, kRefreshIntervalMs, nullptr);
}

void stop_auto_refresh_timer() {
    if (s_auto_refresh_timer != nullptr) {
        lv_timer_delete(s_auto_refresh_timer);
        s_auto_refresh_timer = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Fetch conversation list task
// ---------------------------------------------------------------------------
struct FetchListCtx {
    uint32_t session;
};

void rebuild_conv_list_locked(const std::vector<ConversationRecord> &records,
                              int total);
void update_list_actions_visible_locked(bool visible);
void add_conv_list_row(lv_obj_t *parent, const char *title_text,
                       const char *id_text, const char *actual_title,
                       bool is_create);

void fetch_conv_list_task(void *arg) {
    std::unique_ptr<FetchListCtx> ctx(static_cast<FetchListCtx *>(arg));

    std::lock_guard<std::mutex> http_lock(s_openclaw_http_mutex);
    DeviceStatusResult status_res = GetHttp()->FetchDeviceStatus();

    ConversationListFetchResult list_res;
    std::string status_msg;
    bool service_ok = false;

    if (!status_res.ok) {
        status_msg = I18n::T("检查在线状态失败: ") + status_res.err;
    } else if (!status_res.bridge_online || !status_res.gateway_online) {
        status_msg = device_status_unavailable_message(status_res);
    } else {
        service_ok = true;
        list_res = GetHttp()->FetchConversationList();
    }

    s_list_loading.store(false);

    if (ctx->session != s_list_session.load(std::memory_order_relaxed)) {
        return;
    }

    s_service_available.store(service_ok);

    if (LvglLock() != ESP_OK) {
        return;
    }
    if (is_list_screen_alive()) {
        if (service_ok && list_res.ok) {
            update_list_actions_visible_locked(true);
            rebuild_conv_list_locked(list_res.records, list_res.total);
        } else {
            update_list_actions_visible_locked(service_ok);
            if (s_list_container != nullptr) {
                lv_obj_clean(s_list_container);
                if (service_ok) {
                    add_conv_list_row(s_list_container, I18n::T("创建会话"), nullptr,
                                      nullptr, true);
                }
            }
            if (s_list_hint != nullptr) {
                if (!status_msg.empty()) {
                    lv_label_set_text(s_list_hint, status_msg.c_str());
                    lv_obj_set_style_text_color(s_list_hint,
                                                lv_color_hex(kColorErrorText),
                                                LV_PART_MAIN);
                } else {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), I18n::T("加载失败: %s"),
                                  list_res.err.c_str());
                    lv_label_set_text(s_list_hint, buf);
                    lv_obj_set_style_text_color(s_list_hint,
                                                lv_color_hex(kColorErrorText),
                                                LV_PART_MAIN);
                }
                lv_obj_remove_flag(s_list_hint, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    LvglUnlock();
}

void trigger_fetch_conv_list() {
    if (!is_list_screen_alive() || s_activation_blocked) {
        return;
    }
    if (s_list_loading.exchange(true)) {
        return;
    }

    const uint32_t session =
        s_list_session.fetch_add(1, std::memory_order_relaxed) + 1;

    if (s_list_hint != nullptr) {
        lv_label_set_text(s_list_hint, I18n::T("正在检查龙虾状态…"));
        lv_obj_remove_flag(s_list_hint, LV_OBJ_FLAG_HIDDEN);
    }

    auto *ctx = new FetchListCtx{session};
    if (xTaskCreate(fetch_conv_list_task, "oc_list", 8192, ctx, 4, nullptr) != pdPASS) {
        delete ctx;
        s_list_loading.store(false);
        if (s_list_hint != nullptr) {
            lv_label_set_text(s_list_hint, I18n::T("无法启动加载任务"));
            lv_obj_remove_flag(s_list_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------------------
// Clear all / Delete one tasks
// ---------------------------------------------------------------------------
void async_back_to_list_cb(void * /*user_data*/);

struct DeleteResultMsg {
    bool ok;
    std::string err;
    std::string body;
    bool back_to_list;
};

void async_delete_done(void *user_data) {
    std::unique_ptr<DeleteResultMsg> msg(static_cast<DeleteResultMsg *>(user_data));
    if (msg->back_to_list && (msg->ok)) {
        lv_async_call(async_back_to_list_cb, nullptr);
        return;
    }
    if (is_detail_screen_alive() && s_status_lbl != nullptr) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), I18n::T("删除失败: %s"), msg->err.c_str());
        lv_label_set_text(s_status_lbl, buf);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorErrorText),
                                    LV_PART_MAIN);
    }
}

struct DeleteOneCtx {
    std::string conversation_id;
};

void delete_one_task(void *arg) {
    std::unique_ptr<DeleteOneCtx> ctx(static_cast<DeleteOneCtx *>(arg));
    std::lock_guard<std::mutex> http_lock(s_openclaw_http_mutex);

    auto *msg = new DeleteResultMsg{};
    msg->back_to_list = true;

    if (ctx->conversation_id.empty()) {
        msg->ok = false;
        msg->err = "no conversation id";
    } else {
        DeleteResult r = GetHttp()->DeleteConversation(ctx->conversation_id);
        msg->ok = r.ok;
        msg->err = r.err;
        msg->body = r.body;
    }

    if (!msg->ok) {
        msg->back_to_list = false;
    }
    lv_async_call(async_delete_done, msg);
}

void trigger_delete_one() {
    if (!is_detail_screen_alive() || s_state.load() == State::Closing) {
        return;
    }
    s_history_session.fetch_add(1, std::memory_order_relaxed);
    if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, I18n::T("正在删除…"));
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
    }
    auto *ctx = new DeleteOneCtx{s_conversation_id};
    if (xTaskCreate(delete_one_task, "oc_del1", 8192, ctx, 4, nullptr) != pdPASS) {
        delete ctx;
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, I18n::T("无法启动删除任务"));
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorErrorText),
                                        LV_PART_MAIN);
        }
    }
}

struct ClearAllResultMsg {
    bool ok;
    std::string err;
};

void async_clear_all_done(void *user_data) {
    std::unique_ptr<ClearAllResultMsg> msg(static_cast<ClearAllResultMsg *>(user_data));
    if (is_list_screen_alive()) {
        if (msg->ok) {
            rebuild_conv_list_locked({}, 0);
        } else if (s_list_hint != nullptr) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), I18n::T("清空失败: %s"),
                          msg->err.c_str());
            lv_label_set_text(s_list_hint, buf);
            lv_obj_remove_flag(s_list_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void clear_all_task(void * /*arg*/) {
    std::lock_guard<std::mutex> http_lock(s_openclaw_http_mutex);
    DeleteResult r = GetHttp()->RemoveAllConversations();
    auto *msg = new ClearAllResultMsg{r.ok, r.err};
    lv_async_call(async_clear_all_done, msg);
}

void trigger_clear_all() {
    if (!is_list_screen_alive()) {
        return;
    }
    s_list_session.fetch_add(1, std::memory_order_relaxed);
    if (s_list_hint != nullptr) {
        lv_label_set_text(s_list_hint, I18n::T("正在清空…"));
        lv_obj_remove_flag(s_list_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (xTaskCreate(clear_all_task, "oc_clr", 8192, nullptr, 4, nullptr) != pdPASS) {
        if (s_list_hint != nullptr) {
            lv_label_set_text(s_list_hint, I18n::T("无法启动清空任务"));
            lv_obj_remove_flag(s_list_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------------------
// 会话列表行
// ---------------------------------------------------------------------------
void on_conv_item_delete(lv_event_t *e) {
    auto *ctx = static_cast<ConvItemCtx *>(lv_event_get_user_data(e));
    delete ctx;
}

void open_conversation_detail(const std::string &conversation_id,
                              const std::string &title);
lv_obj_t *create_list_screen();
lv_obj_t *create_detail_screen(const std::string &conversation_id,
                               const std::string &title);

void on_swipe_back_home();

void on_conv_item_clicked(lv_event_t *e) {
    auto *ctx = static_cast<ConvItemCtx *>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    open_conversation_detail(ctx->conversation_id, ctx->title);
}

void on_create_conv_clicked(lv_event_t * /*e*/) {
    open_conversation_detail("", I18n::T("新会话"));
}

void add_conv_list_row(lv_obj_t *parent, const char *title_text,
                       const char *id_text, const char *actual_title,
                       bool is_create) {
    constexpr int32_t kRowH = 92;
    constexpr int32_t kCreateRowH = 88;
    constexpr int32_t kIconSize = 48;
    constexpr int32_t kCreateIconSize = 64;
    constexpr int32_t kTextLeft = 14 + kIconSize + 14;

    lv_obj_t *row = lv_obj_create(parent);
    screen_strip_obj_chrome(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, is_create ? kCreateRowH : kRowH);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    if (is_create) {
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2332), LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(0x3B4556),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(row, lv_color_hex(0x202736), LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 10, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    if (is_create) {
        lv_obj_t *center = lv_obj_create(row);
        screen_strip_obj_chrome(center);
        lv_obj_set_width(center, LV_SIZE_CONTENT);
        lv_obj_set_height(center, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_all(center, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_column(center, 10, LV_PART_MAIN);
        lv_obj_set_flex_flow(center, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_align(center, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(center, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *icon_wrap = lv_obj_create(center);
        screen_strip_obj_chrome(icon_wrap);
        lv_obj_set_size(icon_wrap, kCreateIconSize, kCreateIconSize);
        lv_obj_set_style_radius(icon_wrap, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(icon_wrap, true, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(icon_wrap, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_all(icon_wrap, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(icon_wrap, 0, LV_PART_MAIN);
        lv_obj_remove_flag(icon_wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(icon_wrap, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *icon = lv_image_create(icon_wrap);
        lv_image_set_src(icon, "A:ic_s_openclaw_add_message.spng");
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_size(icon, kCreateIconSize, kCreateIconSize);
        lv_obj_center(icon);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *title = lv_label_create(center);
        lv_label_set_text(title, title_text);
        lv_obj_set_style_text_color(title, lv_color_hex(kColorHeaderText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(row, on_create_conv_clicked, LV_EVENT_CLICKED,
                            nullptr);
        return;
    }

    lv_obj_t *icon = lv_image_create(row);
    lv_image_set_src(icon, "A:ic_s_openclaw_message.spng");
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(icon, kIconSize, kIconSize);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, title_text);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, kPanelW - kTextLeft - 28);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorHeaderText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, kTextLeft, 8);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    if (id_text != nullptr && id_text[0] != '\0') {
        lv_obj_t *id_lbl = lv_label_create(row);
        lv_label_set_text(id_lbl, id_text);
        lv_label_set_long_mode(id_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(id_lbl, kPanelW - kTextLeft - 28);
        lv_obj_set_style_text_color(id_lbl, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(id_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(id_lbl, LV_ALIGN_BOTTOM_LEFT, kTextLeft, -8);
        lv_obj_remove_flag(id_lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    auto *ctx = new ConvItemCtx();
    ctx->conversation_id = id_text != nullptr ? id_text : "";
    ctx->title = actual_title != nullptr ? actual_title : "";
    lv_obj_add_event_cb(row, on_conv_item_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, on_conv_item_delete, LV_EVENT_DELETE, ctx);
}

void add_conv_total_hint(lv_obj_t *parent, int total) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), I18n::T("总共%d条会话"), total);

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, buf);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(hint, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_top(hint, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(hint, 8, LV_PART_MAIN);
    screen_make_input_passive(hint);
}

void update_list_actions_visible_locked(bool visible) {
    if (s_list_clear_btn == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(s_list_clear_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_list_clear_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void rebuild_conv_list_locked(const std::vector<ConversationRecord> &records,
                              int total) {
    if (s_list_container == nullptr) {
        return;
    }
    update_list_actions_visible_locked(true);
    lv_obj_clean(s_list_container);

    add_conv_list_row(s_list_container, I18n::T("创建会话"), nullptr, nullptr, true);
    add_conv_total_hint(s_list_container, total);
    for (const auto &rec : records) {
        const char *title = rec.title.empty() ? I18n::T("未命名会话") : rec.title.c_str();
        add_conv_list_row(s_list_container, title, rec.conversation_id.c_str(),
                          rec.title.c_str(), false);
    }

    if (s_list_hint != nullptr) {
        lv_obj_add_flag(s_list_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// 录音按钮 UI 同步
// ---------------------------------------------------------------------------
void update_button_ui_locked(State st) {
    if (s_record_btn == nullptr || s_record_lbl == nullptr) return;
    switch (st) {
        case State::Idle:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(
                                          s_service_available.load()
                                              ? kColorRecordBtnIdle
                                              : kColorRecordBtnBusy),
                                      LV_PART_MAIN);
            lv_label_set_text(s_record_lbl, I18n::T("按住说话"));
            if (s_service_available.load()) {
                lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_remove_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            }
            break;
        case State::Recording:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(kColorRecordBtnActive),
                                      LV_PART_MAIN);
            lv_label_set_text(s_record_lbl, I18n::T("已录 0.0 秒"));
            lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            break;
        case State::Uploading:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(kColorRecordBtnBusy),
                                      LV_PART_MAIN);
            lv_label_set_text(s_record_lbl, I18n::T("上传中..."));
            lv_obj_remove_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            break;
        case State::Closing:
            break;
    }
}

void tick_timer_cb(lv_timer_t * /*t*/) {
    if (s_state.load() != State::Recording) return;
    if (s_record_lbl == nullptr) return;
    const int64_t start = s_record_start_us.load();
    if (start <= 0) return;
    const int64_t now = esp_timer_get_time();
    const int ms = static_cast<int>((now - start) / 1000);
    char buf[32];
    std::snprintf(buf, sizeof(buf), I18n::T("已录 %d.%d 秒"), ms / 1000,
                  (ms / 100) % 10);
    lv_label_set_text(s_record_lbl, buf);
}

// ---------------------------------------------------------------------------
// 录音 + 上传 worker
// ---------------------------------------------------------------------------
void record_and_upload_task(void * /*arg*/) {
    auto &app = Application::GetInstance();
    auto &as = app.GetAudioService();

    // 这一次操作期间是否由我们关掉了 wake word。
    bool wake_disabled_by_us = false;
    if (as.IsWakeWordRunning()) {
        as.EnableWakeWordDetection(false);
        wake_disabled_by_us = true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // 大缓冲（最坏 60s = 1.92MB）。
    uint8_t *buffer = static_cast<uint8_t *>(malloc(kMaxRecordBytes));
    if (buffer == nullptr) {
        buffer = static_cast<uint8_t *>(malloc(kMaxRecordBytes / 4));
    }
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "no memory for record buffer");
        post_status_from_worker(I18n::T("内存不足"), kColorHintText);
        if (wake_disabled_by_us && app.IsVoiceUiActive()) {
            as.EnableWakeWordDetection(true);
        }
        s_state.store(State::Idle);
        if (LvglLock() == ESP_OK) {
            if (is_detail_screen_alive()) update_button_ui_locked(State::Idle);
            LvglUnlock();
        }
        s_record_task = 0;
        vTaskDelete(nullptr);
        return;
    }

    size_t written = 0;
    std::vector<int16_t> frame;
    frame.reserve(kSamplesPerFrame * 2);

    const int64_t start_us = esp_timer_get_time();
    s_record_start_us.store(start_us);

    while (!s_stop_requested.load()) {
        if (!as.ReadAudioData(frame, kSampleRate, kSamplesPerFrame)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        // openvela ES8311 codec is mono — no keep_left_channel_only step.
        const size_t bytes = frame.size() * sizeof(int16_t);
        if (written + bytes > kMaxRecordBytes) {
            ESP_LOGW(TAG, "record buffer full, force stop");
            break;
        }
        std::memcpy(buffer + written, frame.data(), bytes);
        written += bytes;
        if ((esp_timer_get_time() - start_us) / 1000 >=
            kMaxRecordSeconds * 1000) {
            break;
        }
    }
    const int64_t end_us = esp_timer_get_time();
    const int duration_ms = static_cast<int>((end_us - start_us) / 1000);

    if (wake_disabled_by_us) {
        if (app.IsVoiceUiActive()) {
            as.EnableWakeWordDetection(true);
        }
        wake_disabled_by_us = false;
    }

    // 录音过短 -> 提示并丢弃
    if (duration_ms < kMinRecordMs || written < 1024) {
        ESP_LOGI(TAG, "discard short recording: %d ms / %u bytes",
                 duration_ms, static_cast<unsigned>(written));
        post_status_from_worker(I18n::T("录音太短，再试一次"), kColorHintText);
        free(buffer);
        s_state.store(State::Idle);
        if (LvglLock() == ESP_OK) {
            if (is_detail_screen_alive()) update_button_ui_locked(State::Idle);
            LvglUnlock();
        }
        s_record_task = 0;
        vTaskDelete(nullptr);
        return;
    }

    // 切到 Uploading 状态
    s_state.store(State::Uploading);
    if (LvglLock() == ESP_OK) {
        if (is_detail_screen_alive()) update_button_ui_locked(State::Uploading);
        LvglUnlock();
    }
    char hint[64];
    std::snprintf(hint, sizeof(hint), I18n::T("上传中… (%ds)"), duration_ms / 1000);
    post_status_from_worker(hint, kColorHintText);

    // 拼 WAV：header + recorded PCM
    std::string wav_body;
    wav_body.reserve(sizeof(uint8_t) * 44 + written);
    {
        uint8_t wav_header[44];
        fill_wav_header(wav_header, static_cast<uint32_t>(written));
        wav_body.assign(reinterpret_cast<const char *>(wav_header), sizeof(wav_header));
        wav_body.append(reinterpret_cast<const char *>(buffer), written);
    }
    free(buffer);

    ESP_LOGI(TAG, "uploading WAV %u bytes (%d ms)",
             static_cast<unsigned>(wav_body.size()), duration_ms);

    UploadResult res;
    {
        std::lock_guard<std::mutex> http_lock(s_openclaw_http_mutex);
        res = GetHttp()->UploadWav(reinterpret_cast<const uint8_t *>(wav_body.data()),
                                   wav_body.size(), s_conversation_id);
    }

    if (res.ok) {
        if (!res.conversation_id.empty()) {
            s_conversation_id = res.conversation_id;
            if (LvglLock() == ESP_OK) {
                if (is_detail_screen_alive() && s_detail_id_lbl != nullptr) {
                    lv_label_set_text(s_detail_id_lbl,
                                      s_conversation_id.c_str());
                }
                LvglUnlock();
            }
        }
        ESP_LOGI(TAG, "upload ok, asr=%s conv=%s", res.text.c_str(),
                 s_conversation_id.c_str());

        // 刷新消息列表
        std::vector<HistoryMessage> messages;
        std::string messages_json;
        bool refreshed = false;
        {
            std::lock_guard<std::mutex> http_lock(s_openclaw_http_mutex);
            MessagesFetchResult mf = GetHttp()->FetchMessages(s_conversation_id);
            if (mf.ok) {
                messages_json = mf.raw_body;
                if (!messages_json.empty()) {
                    refreshed = parse_messages_json(messages_json, messages);
                } else {
                    refreshed = true;
                }
            }
        }
        if (refreshed) {
            const bool unchanged = refresh_snapshot_unchanged(
                s_service_available.load(), true, true, s_conversation_id,
                messages_json);
            if (!unchanged) {
                if (LvglLock() == ESP_OK) {
                    if (is_detail_screen_alive()) {
                        apply_history_locked(messages);
                    }
                    LvglUnlock();
                }
                save_refresh_snapshot(s_service_available.load(), true, true,
                                      s_conversation_id, messages_json);
            }
            post_status_from_worker(I18n::T("按住说话"), kColorHintText);
        } else {
            ESP_LOGW(TAG, "refresh messages failed");
            post_status_from_worker(I18n::T("刷新消息失败"), kColorErrorText);
        }
    } else {
        ESP_LOGW(TAG, "upload failed: %s", res.err.c_str());
        std::string msg = I18n::T("上传失败: ") + res.err;
        post_status_from_worker(msg.c_str(), kColorErrorText);
    }

    s_state.store(State::Idle);
    if (LvglLock() == ESP_OK) {
        if (is_detail_screen_alive()) update_button_ui_locked(State::Idle);
        LvglUnlock();
    }
    s_record_task = 0;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// 按钮事件
// ---------------------------------------------------------------------------
void on_record_pressed(lv_event_t * /*e*/) {
    if (s_activation_blocked) {
        ESP_LOGW(TAG, "record ignored: device not activated");
        return;
    }
    if (!s_service_available.load()) {
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, I18n::T("龙虾服务不可用"));
            lv_obj_set_style_text_color(s_status_lbl,
                                        lv_color_hex(kColorErrorText),
                                        LV_PART_MAIN);
        }
        return;
    }
    if (s_state.load() != State::Idle) return;

    auto &app = Application::GetInstance();
    const DeviceState ds = app.GetDeviceState();
    if (ds == kDeviceStateConnecting || ds == kDeviceStateListening ||
        ds == kDeviceStateSpeaking || ds == kDeviceStateUpgrading) {
        if (s_status_lbl != nullptr) {
            lv_label_set_text(s_status_lbl, I18n::T("请先结束当前对话"));
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorErrorText),
                                        LV_PART_MAIN);
        }
        return;
    }

    s_stop_requested.store(false);
    s_state.store(State::Recording);
    update_button_ui_locked(State::Recording);
    if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, I18n::T("正在录音…松开结束"));
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
    }

    if (xTaskCreate(record_and_upload_task, "oc_rec", 8192, nullptr, 5,
                    &s_record_task) != pdPASS) {
        ESP_LOGE(TAG, "create record task failed");
        s_state.store(State::Idle);
        update_button_ui_locked(State::Idle);
    }
}

void on_record_released(lv_event_t * /*e*/) {
    if (s_state.load() == State::Recording) {
        s_stop_requested.store(true);
    }
}

// ---------------------------------------------------------------------------
// 屏幕导航 / 生命周期
// ---------------------------------------------------------------------------
void clear_messages() {
    if (s_msg_list == nullptr) return;
    const uint32_t count = lv_obj_get_child_count(s_msg_list);
    for (int32_t i = static_cast<int32_t>(count) - 1; i >= 0; --i) {
        lv_obj_delete(lv_obj_get_child(s_msg_list, i));
    }
    update_empty_hint();
}

void on_list_clear_clicked(lv_event_t * /*e*/) {
    if (s_activation_blocked) {
        return;
    }
    if (!s_service_available.load()) {
        return;
    }
    // 直接触发清空（原实现弹确认框；此处简化为直接执行）。
    trigger_clear_all();
}

void on_detail_clear_clicked(lv_event_t * /*e*/) {
    if (s_activation_blocked) {
        return;
    }
    trigger_delete_one();
}

// ---------------------------------------------------------------------------
// 激活拦截弹窗
// ---------------------------------------------------------------------------
void open_activation_blocked_dialog(lv_obj_t *parent_screen) {
    if (parent_screen == nullptr || s_activation_dlg.mask != nullptr) {
        return;
    }

    auto &app = Application::GetInstance();
    const bool has_code = app.HasPendingActivation();
    s_activation_dialog_shows_code = has_code;

    constexpr int32_t kCardW = 520;
    const int32_t kCardH = has_code ? 420 : 340;
    constexpr int32_t kBackBtnW = 200;
    constexpr int32_t kBackBtnH = 72;

    lv_obj_t *mask = lv_obj_create(parent_screen);
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);
    s_activation_dlg.mask = mask;

    lv_obj_t *card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("设备未激活"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *desc = lv_label_create(card);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, kCardW - 56);
    lv_label_set_text(desc, I18n::T("请先完成设备激活后再使用 OpenClaw。"));
    lv_obj_set_style_text_color(desc, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(desc, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, has_code ? -30 : -10);
    lv_obj_remove_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    if (has_code) {
        char code_buf[64];
        std::snprintf(code_buf, sizeof(code_buf), I18n::T("验证码: %s"),
                      app.GetPendingActivationCode().c_str());
        lv_obj_t *code_lbl = lv_label_create(card);
        lv_label_set_text(code_lbl, code_buf);
        lv_obj_set_style_text_color(code_lbl, lv_color_hex(0xFBBF24),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(code_lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_align(code_lbl, LV_ALIGN_BOTTOM_MID, 0, -(kBackBtnH + 24));
        lv_obj_remove_flag(code_lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *back = lv_button_create(card);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnW, kBackBtnH);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 16, LV_PART_MAIN);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(back,
                        [](lv_event_t * /*e*/) { on_swipe_back_home(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, I18n::T("返回"));
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_font(back_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(back_lbl);
    lv_obj_remove_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void close_activation_blocked_dialog() {
    if (s_activation_dlg.mask != nullptr) {
        lv_obj_delete(s_activation_dlg.mask);
        s_activation_dlg.mask = nullptr;
    }
    s_activation_dialog_shows_code = false;
}

void sync_activation_block_state() {
    auto &app = Application::GetInstance();
    const bool should_block = !is_device_activated();
    const bool has_code = app.HasPendingActivation();
    if (should_block == s_activation_blocked) {
        if (should_block) {
            if (has_code && !s_activation_dialog_shows_code) {
                close_activation_blocked_dialog();
                lv_obj_t *parent = s_list_screen != nullptr ? s_list_screen
                                                            : s_detail_screen;
                open_activation_blocked_dialog(parent);
            }
        }
        return;
    }
    s_activation_blocked = should_block;
    if (should_block) {
        log_activation_blocked();
        lv_obj_t *parent = s_list_screen != nullptr ? s_list_screen
                                                    : s_detail_screen;
        open_activation_blocked_dialog(parent);
    } else {
        ESP_LOGI(TAG, "OpenClaw unblocked: device activated/ready");
        close_activation_blocked_dialog();
    }
}

void on_activation_guard_timer(lv_timer_t * /*timer*/) {
    sync_activation_block_state();
}

// ---------------------------------------------------------------------------
// Header 按钮样式
// ---------------------------------------------------------------------------
void style_header_btn(lv_obj_t *btn) {
    lv_obj_set_style_radius(btn, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorHeaderBtn), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorHeaderBtnBorder),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3B4556),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
}

void on_swipe_back_to_list();

void on_swipe_back_home() {
    s_history_session.fetch_add(1, std::memory_order_relaxed);
    s_list_session.fetch_add(1, std::memory_order_relaxed);
    s_history_loading.store(false);
    s_list_loading.store(false);
    s_navigating_within_openclaw.store(false, std::memory_order_release);
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    HomeScreen::SwitchToHome();
}

void async_back_to_list_cb(void * /*user_data*/) {
    on_swipe_back_to_list();
}

void on_swipe_back_to_list() {
    s_history_session.fetch_add(1, std::memory_order_relaxed);
    s_history_loading.store(false);
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    s_navigating_within_openclaw.store(true, std::memory_order_release);
    lv_obj_t *old_scr = lv_screen_active();
    lv_obj_t *list    = create_list_screen();
    lv_screen_load(list);
    if (old_scr != nullptr && old_scr != list) {
        lv_obj_delete_async(old_scr);
    }
}

void open_conversation_detail(const std::string &conversation_id,
                              const std::string &title) {
    s_list_session.fetch_add(1, std::memory_order_relaxed);
    s_list_loading.store(false);
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    s_navigating_within_openclaw.store(true, std::memory_order_release);
    lv_obj_t *old_scr = lv_screen_active();
    lv_obj_t *detail  = create_detail_screen(conversation_id, title);
    lv_screen_load(detail);
    if (old_scr != nullptr && old_scr != detail) {
        lv_obj_delete_async(old_scr);
    }
}

void on_list_screen_unloaded(lv_event_t * /*e*/) {
    s_list_session.fetch_add(1, std::memory_order_relaxed);
    s_list_loading.store(false);
    s_clear_dlg = ClearDialogUi{};
    s_list_screen = nullptr;
    s_list_container = nullptr;
    s_list_hint = nullptr;
    s_list_clear_btn = nullptr;

    if (!s_navigating_within_openclaw.exchange(false, std::memory_order_acq_rel)) {
        if (s_activation_guard_timer != nullptr) {
            lv_timer_delete(s_activation_guard_timer);
            s_activation_guard_timer = nullptr;
        }
        s_activation_dlg = ActivationBlockedDialogUi{};
        s_activation_blocked = false;
        s_activation_dialog_shows_code = false;
    } else {
        s_activation_dlg = ActivationBlockedDialogUi{};
        s_activation_dialog_shows_code = false;
    }
}

void on_detail_screen_unloaded(lv_event_t * /*e*/) {
    s_stop_requested.store(true);
    s_state.store(State::Closing);
    s_history_session.fetch_add(1, std::memory_order_relaxed);
    s_history_loading.store(false);
    s_service_available.store(false);
    s_conversation_id.clear();
    s_conversation_title.clear();
    invalidate_refresh_snapshot();

    if (s_tick_timer != nullptr) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = nullptr;
    }
    stop_auto_refresh_timer();
    s_clear_dlg = ClearDialogUi{};
    s_detail_screen = nullptr;
    s_msg_list = nullptr;
    s_empty_hint = nullptr;
    s_record_btn = nullptr;
    s_record_lbl = nullptr;
    s_status_lbl = nullptr;
    s_detail_title_lbl = nullptr;
    s_detail_id_lbl = nullptr;

    s_navigating_within_openclaw.exchange(false, std::memory_order_acq_rel);
    s_activation_dlg = ActivationBlockedDialogUi{};
    s_activation_dialog_shows_code = false;
}

// ---------------------------------------------------------------------------
// UI 组装
// ---------------------------------------------------------------------------
void build_list_header(lv_obj_t *parent) {
    lv_obj_t *header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kColorHeaderBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *divider = lv_obj_create(header);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back,
                        [](lv_event_t * /*e*/) { on_swipe_back_home(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "OpenClaw");
    lv_obj_set_style_text_color(title, lv_color_hex(kColorHeaderText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 12, 0);

    constexpr int32_t kHdrBtnW = 88;
    constexpr int32_t kHdrBtnH = 56;
    constexpr int32_t kHdrRightPad = 12;

    lv_obj_t *clear = lv_button_create(header);
    s_list_clear_btn = clear;
    lv_obj_set_size(clear, kHdrBtnW, kHdrBtnH);
    lv_obj_align(clear, LV_ALIGN_RIGHT_MID, -kHdrRightPad, 0);
    style_header_btn(clear);
    lv_obj_add_event_cb(clear, on_list_clear_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *clear_lbl = lv_label_create(clear);
    lv_label_set_text(clear_lbl, I18n::T("清空"));
    lv_obj_set_style_text_color(clear_lbl, lv_color_hex(kColorHeaderBtnText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(clear_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(clear_lbl);
}

void build_list_body(lv_obj_t *parent) {
    s_list_container = lv_obj_create(parent);
    lv_obj_set_size(s_list_container, kPanelW, kPanelH - kHeaderH);
    lv_obj_set_pos(s_list_container, 0, kHeaderH);
    screen_strip_obj_chrome(s_list_container);
    lv_obj_set_style_bg_opa(s_list_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_list_container, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_list_container, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_list_container, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list_container, 10, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_list_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_list_container, LV_DIR_VER);
    lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_list_hint = lv_label_create(parent);
    lv_label_set_text(s_list_hint, I18n::T("正在检查龙虾状态…"));
    lv_obj_set_width(s_list_hint, kPanelW * 80 / 100);
    lv_label_set_long_mode(s_list_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_list_hint, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_list_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_list_hint, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_align(s_list_hint, LV_ALIGN_TOP_MID, 0,
                 kHeaderH + (kPanelH - kHeaderH) / 2 - 20);
    screen_make_input_passive(s_list_hint);
}

void build_detail_header(lv_obj_t *parent) {
    lv_obj_t *header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kColorHeaderBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *divider = lv_obj_create(header);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back,
                        [](lv_event_t * /*e*/) { on_swipe_back_to_list(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    const int32_t title_left = 16 + kBackBtnSize + 12;
    const int32_t title_w = kPanelW - title_left - 120;

    s_detail_title_lbl = lv_label_create(header);
    lv_label_set_text(s_detail_title_lbl,
                      s_conversation_title.empty() ? I18n::T("未命名会话")
                                                   : s_conversation_title.c_str());
    lv_label_set_long_mode(s_detail_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_detail_title_lbl, title_w);
    lv_obj_set_style_text_color(s_detail_title_lbl,
                                lv_color_hex(kColorHeaderText), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_detail_title_lbl, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_align(s_detail_title_lbl, LV_ALIGN_LEFT_MID, title_left, -12);

    s_detail_id_lbl = lv_label_create(header);
    if (s_conversation_id.empty()) {
        lv_label_set_text(s_detail_id_lbl, I18n::T("新会话"));
    } else {
        lv_label_set_text(s_detail_id_lbl, s_conversation_id.c_str());
    }
    lv_label_set_long_mode(s_detail_id_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_detail_id_lbl, title_w);
    lv_obj_set_style_text_color(s_detail_id_lbl, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_detail_id_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_detail_id_lbl, LV_ALIGN_LEFT_MID, title_left, 14);

    constexpr int32_t kHdrBtnW = 88;
    constexpr int32_t kHdrBtnH = 56;
    constexpr int32_t kHdrRightPad = 12;

    lv_obj_t *clear = lv_button_create(header);
    lv_obj_set_size(clear, kHdrBtnW, kHdrBtnH);
    lv_obj_align(clear, LV_ALIGN_RIGHT_MID, -kHdrRightPad, 0);
    style_header_btn(clear);
    lv_obj_add_event_cb(clear, on_detail_clear_clicked, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_t *clear_lbl = lv_label_create(clear);
    lv_label_set_text(clear_lbl, I18n::T("删除"));
    lv_obj_set_style_text_color(clear_lbl, lv_color_hex(kColorHeaderBtnText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(clear_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(clear_lbl);
}

void build_message_list(lv_obj_t *parent) {
    s_msg_list = lv_obj_create(parent);
    lv_obj_set_size(s_msg_list, kPanelW, kListH);
    lv_obj_set_pos(s_msg_list, 0, kHeaderH);
    screen_strip_obj_chrome(s_msg_list);
    lv_obj_set_style_bg_opa(s_msg_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_msg_list, kListPadH, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_msg_list, kListPadH, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_msg_list, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_msg_list, 16, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_msg_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_msg_list, LV_DIR_VER);
    lv_obj_set_flex_flow(s_msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_msg_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    s_empty_hint = lv_label_create(parent);
    lv_label_set_text(s_empty_hint, I18n::T(kEmptyHint));
    lv_obj_set_width(s_empty_hint, kPanelW * 80 / 100);
    lv_label_set_long_mode(s_empty_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_empty_hint, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_empty_hint, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_empty_hint, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_align(s_empty_hint, LV_ALIGN_TOP_MID, 0,
                 kHeaderH + (kListH / 2) - 50);
    screen_make_input_passive(s_empty_hint);
    update_empty_hint();
}

void build_footer(lv_obj_t *parent) {
    lv_obj_t *footer = lv_obj_create(parent);
    screen_strip_obj_chrome(footer);
    lv_obj_set_size(footer, kPanelW, kFooterH);
    lv_obj_set_pos(footer, 0, kPanelH - kFooterH);
    lv_obj_set_style_bg_color(footer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    s_status_lbl = lv_label_create(footer);
    lv_label_set_text(s_status_lbl, I18n::T("按住下面的按钮说话"));
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 10);
    screen_make_input_passive(s_status_lbl);

    constexpr int32_t kBtnW = 400;
    constexpr int32_t kBtnH = 72;
    s_record_btn = lv_button_create(footer);
    lv_obj_set_size(s_record_btn, kBtnW, kBtnH);
    lv_obj_align(s_record_btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_radius(s_record_btn, kBtnH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_record_btn, lv_color_hex(kColorRecordBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_record_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_record_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_record_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_record_btn,
                              lv_color_hex(kColorRecordBtnActive),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));

    s_record_lbl = lv_label_create(s_record_btn);
    lv_label_set_text(s_record_lbl, I18n::T("按住说话"));
    lv_obj_set_style_text_color(s_record_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_record_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(s_record_lbl);

    lv_obj_add_event_cb(s_record_btn, on_record_pressed, LV_EVENT_PRESSED,
                        nullptr);
    lv_obj_add_event_cb(s_record_btn, on_record_released, LV_EVENT_RELEASED,
                        nullptr);
    screen_swipe_back_ignore(s_record_btn, true);
}

lv_obj_t *create_list_screen() {
    lv_obj_t *scr = lv_obj_create(nullptr);
    s_list_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_list_header(scr);
    build_list_body(scr);

    if (s_activation_blocked) {
        open_activation_blocked_dialog(scr);
    }

    screen_attach_swipe_back(scr, on_swipe_back_home);
    if (s_lifecycle_cb != nullptr) {
        screen_attach_lifecycle(scr, s_lifecycle_cb);
    }
    lv_obj_add_event_cb(scr, on_list_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    lv_obj_add_event_cb(scr, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
            sync_activation_block_state();
            if (s_activation_blocked) {
                return;
            }
            trigger_fetch_conv_list();
        }
    }, LV_EVENT_SCREEN_LOADED, nullptr);
    return scr;
}

lv_obj_t *create_detail_screen(const std::string &conversation_id,
                               const std::string &title) {
    s_conversation_id = conversation_id;
    s_conversation_title = title;
    invalidate_refresh_snapshot();

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_detail_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_detail_header(scr);
    build_message_list(scr);
    build_footer(scr);

    if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, I18n::T("正在检查龙虾状态…"));
    }

    s_tick_timer = lv_timer_create(tick_timer_cb, 100, nullptr);
    s_state.store(State::Idle);
    s_stop_requested.store(false);
    s_record_start_us.store(0);

    screen_attach_swipe_back(scr, on_swipe_back_to_list);
    if (s_lifecycle_cb != nullptr) {
        screen_attach_lifecycle(scr, s_lifecycle_cb);
    }
    lv_obj_add_event_cb(scr, on_detail_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    lv_obj_add_event_cb(scr, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
            sync_activation_block_state();
            if (s_activation_blocked) {
                return;
            }
            trigger_fetch_history(true);
            start_auto_refresh_timer();
        }
    }, LV_EVENT_SCREEN_LOADED, nullptr);
    return scr;
}

}  // namespace

// ===========================================================================
// 公共接口
// ===========================================================================
lv_obj_t *OpenClawScreen::CreateStatic() {
    s_activation_blocked = !is_device_activated();
    s_navigating_within_openclaw.store(false, std::memory_order_release);
    if (s_activation_blocked) {
        log_activation_blocked();
    }

    lv_obj_t *scr = create_list_screen();

    s_activation_guard_timer =
        lv_timer_create(on_activation_guard_timer, 1000, nullptr);

    return scr;
}

lv_obj_t *OpenClawScreen::Create() {
    root_ = CreateStatic();
    return root_;
}

void OpenClawScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        if (!is_device_activated()) {
            ESP_LOGW(TAG, "load: openclaw_screen blocked (device not activated)");
            log_activation_blocked();
        } else {
            ESP_LOGI(TAG, "load: openclaw_screen");
        }
    } else {
        if (s_navigating_within_openclaw.load(std::memory_order_acquire)) {
            ESP_LOGI(TAG, "unload: openclaw_screen (internal navigation)");
            return;
        }
        ESP_LOGI(TAG, "unload: openclaw_screen");
        // OpenClaw 不持有语音 UI 会话；只确保不误开唤醒词。
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
    }
}
