/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * AiImageGenScreen — ported from MetalioClaw4
 * main/display/screen/ai_image_gen_screen/ai_image_gen_screen.cc.
 *
 * Adaptations for NuttX/openvela:
 *   - esp_log.h               -> esp_log_shim.h
 *   - esp_timer.h             -> esp_timer_shim.h (esp_timer_get_time)
 *   - esp_lv_adapter.h        -> removed; esp_lv_adapter_lock/unlock
 *                                replaced with lv_lock()/lv_unlock().
 *   - esp_heap_caps.h         -> removed; heap_caps_malloc/free replaced
 *                                with malloc/free (NuttX has no PSRAM
 *                                pool allocator). HeapCapsDeleter /
 *                                CapsBuffer are preserved so buffer
 *                                management code is unchanged.
 *   - freertos/FreeRTOS.h + task.h -> freertos_shim.h.
 *   - cJSON.h                 -> cJSON_compat.h.
 *   - board.h                 -> board_shim.h.
 *   - http.h                  -> ota.h (BSD-socket Http class).
 *                                Board::GetNetwork()->CreateHttp(0)
 *                                becomes a plain Http instance.
 *   - HomeScreen::Create()    -> HomeScreen::CreateStatic().
 *
 * The LVGL widget tree, layout constants, colour palette, recording
 * state machine, ASR / text2image / polling / download flow, JSON
 * parsing and image gallery (single + tabbed multi-image) are preserved
 * verbatim from the original.
 */

#include "ai_image_gen_screen.h"
#include "horde_protocol.h"
#include "horde_webp.h"
#include "image_prompt.h"
#include "i18n.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON_compat.h"
#include "esp_log_shim.h"
#include "esp_timer_shim.h"
#include "freertos_shim.h"

#include "api_endpoints.h"
#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "../../../audio/speech_capture.h"
#include "board_shim.h"
#include "device_state.h"
#include "home_screen/home_screen.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "hi_music_screen/local_music_player.h"
#include "radio_screen/radio_player.h"
#include "music_screen/music_screen.h"
#include "ota.h"
#include "lvgl_image.h"
#include "screen_util.h"
#include "system_info.h"
#include "input_method/hi_input_method.h"

#include <unistd.h>
#include <metalio/metalio.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_ai_slogan_32_bold);

namespace {

// TaskHandle_t is the pthread ID. getpid() is the task-group ID on NuttX;
// using it here leaves a completed worker handle permanently occupied.
// Used by
// clear_own_worker_handle() to confirm the caller is the worker task
// before clearing the stored handle (preserves the original guard).
static inline TaskHandle_t xTaskGetCurrentTaskHandle() {
    return static_cast<TaskHandle_t>(pthread_self());
}

constexpr const char* TAG = "AiImageGenScreen";

constexpr int  kSampleRate       = 16000;
constexpr int  kSamplesPerFrame  = kSampleRate * 60 / 1000;
constexpr int  kBytesPerSample   = 2;
constexpr int  kMaxRecordSeconds = 30;
constexpr size_t kMaxRecordBytes =
    static_cast<size_t>(kSampleRate) * kBytesPerSample * kMaxRecordSeconds;
constexpr int  kMinRecordMs      = 300;

constexpr int32_t kPanelW      = 720;
constexpr int32_t kPanelH      = 720;
constexpr int32_t kHeaderH     = 88;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kFooterH     = 108;
constexpr int32_t kBodyH       = kPanelH - kHeaderH - kFooterH;
constexpr int32_t kHeaderSidePad = 8;
constexpr int32_t kPromptAreaH = 56;
constexpr int32_t kTabBarH     = 52;
constexpr int32_t kGalleryH    = kBodyH - kPromptAreaH - 20;

constexpr int   kPollIntervalMs      = 3000;
constexpr int   kPollFirstDelayMs    = 1500;
constexpr int   kMaxPollAttempts     = 400;
constexpr int   kMaxPollHttpFails    = 8;
constexpr int   kHttpTimeoutMs       = 30000;
constexpr size_t kImageMaxBytes      = 800 * 1024;
constexpr size_t kMaxButtonTextBytes = 48;

constexpr uint32_t kColorBg         = 0x0E1116;
constexpr uint32_t kColorHeaderBg   = 0x12151C;
constexpr uint32_t kColorDivider    = 0x2A2F3A;
constexpr uint32_t kColorCard       = 0x1A1F2A;
constexpr uint32_t kColorText       = 0xFFFFFF;
constexpr uint32_t kColorHintText   = 0x9AA3B2;
constexpr uint32_t kColorPromptText = 0xE5E7EB;

constexpr uint32_t kColorRecordBtnIdle   = 0x2563EB;
constexpr uint32_t kColorRecordBtnActive = 0xDC2626;
constexpr uint32_t kColorRecordBtnBusy   = 0x4B5563;
constexpr uint32_t kColorTabActive       = 0x3B82F6;

enum class State : uint8_t {
    Idle,
    Recording,
    Busy,  // ASR / 生图 / 下载
    Closing,
};

std::atomic<State>   s_state{State::Idle};
std::atomic<int64_t> s_record_start_us{0};
std::atomic<int64_t> s_gen_start_us{0};  // >0 表示处于「生成中」并显示已用时
std::atomic<bool>    s_stop_requested{false};
std::atomic<uint32_t> s_session{0};
std::mutex           s_http_mutex;

lv_obj_t* s_screen      = nullptr;
lv_obj_t* s_prompt_lbl  = nullptr;
lv_obj_t* s_gallery     = nullptr;
lv_obj_t* s_record_btn  = nullptr;
lv_obj_t* s_record_lbl  = nullptr;
lv_obj_t* s_n_dd        = nullptr;
lv_timer_t* s_tick_timer = nullptr;
TaskHandle_t s_worker_task = 0;

std::vector<std::unique_ptr<LvglAllocatedImage>> s_image_holders;

// Image generation connects directly to Horde. Speech keeps its own ASR
// endpoint so cloud migration never removes the required voice input mode.
std::string cloud_asr_url() {
    Settings config("imagegen");
    std::string url=config.GetString("asr_url","");
    if(url.empty())return HiSpeech::ImageAsrUrl(api::ServiceUrl(api::kXiaozhiAsrWav));
    if(url.compare(0,8,"https://") || url.find_first_of("\r\n \t")!=std::string::npos)return {};
    return url;
}
const char* idle_button_text() { return cloud_asr_url().empty()?"输入描述":"按住说话"; }
void open_text_composer(lv_event_t*);
bool screen_alive() { return s_screen != nullptr; }

bool session_alive(uint32_t session) {
    return session == s_session.load(std::memory_order_acquire) &&
           s_state.load(std::memory_order_acquire) != State::Closing;
}

// openvela: heap_caps_free -> free. The HeapCapsDeleter functor is kept
// so CapsBuffer (= std::unique_ptr<uint8_t, HeapCapsDeleter>) usage in
// the rest of the file is unchanged.
struct HeapCapsDeleter {
    void operator()(void* p) const {
        if (p != nullptr) {
            free(p);
        }
    }
};
using CapsBuffer = std::unique_ptr<uint8_t, HeapCapsDeleter>;

CapsBuffer alloc_caps(size_t n) {
    // openvela: heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    // -> malloc(). NuttX does not expose a PSRAM-specific allocator; the
    // standard heap is used. When a PSRAM pool is wired up, replace this
    // with the appropriate allocator (and re-enable the SPIRAM fallback).
    void* p = malloc(n);
    return CapsBuffer(static_cast<uint8_t*>(p));
}

struct WakeWordGuard {
    AudioService& as;
    bool disabled = false;
    explicit WakeWordGuard(AudioService& audio) : as(audio) {
        if (as.IsWakeWordRunning()) {
            as.EnableWakeWordDetection(false);
            disabled = true;
            vTaskDelayMs(100);
        }
    }
    ~WakeWordGuard() {
        // 仅当语音 UI 会话仍在时恢复，避免生图页把桌面唤醒词误打开。
        if (disabled && Application::GetInstance().IsVoiceUiActive()) {
            as.EnableWakeWordDetection(true);
        }
    }
    WakeWordGuard(const WakeWordGuard&) = delete;
    WakeWordGuard& operator=(const WakeWordGuard&) = delete;
};

const std::string& device_id_header() {
    static const std::string kId = SystemInfo::GetMacAddress();
    return kId;
}

void fill_wav_header(uint8_t* hdr, uint32_t data_bytes) {
    const uint32_t sample_rate = kSampleRate;
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint32_t byte_rate = sample_rate * channels * bits / 8;
    const uint16_t block_align = channels * bits / 8;
    const uint32_t riff_size = 36 + data_bytes;
    auto put32 = [](uint8_t* p, uint32_t v) {
        p[0] = v & 0xFF;
        p[1] = (v >> 8) & 0xFF;
        p[2] = (v >> 16) & 0xFF;
        p[3] = (v >> 24) & 0xFF;
    };
    auto put16 = [](uint8_t* p, uint16_t v) {
        p[0] = v & 0xFF;
        p[1] = (v >> 8) & 0xFF;
    };
    std::memcpy(hdr + 0, "RIFF", 4);
    put32(hdr + 4, riff_size);
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    put32(hdr + 16, 16);
    put16(hdr + 20, 1);
    put16(hdr + 22, channels);
    put32(hdr + 24, sample_rate);
    put32(hdr + 28, byte_rate);
    put16(hdr + 32, block_align);
    put16(hdr + 34, bits);
    std::memcpy(hdr + 36, "data", 4);
    put32(hdr + 40, data_bytes);
}

void keep_left_channel_only(std::vector<int16_t>& samples) {
    if (samples.size() < 2) {
        return;
    }
    const size_t mono_count = samples.size() / 2;
    for (size_t i = 0, j = 0; i < mono_count; ++i, j += 2) {
        samples[i] = samples[j];
    }
    samples.resize(mono_count);
}

int selected_image_count() {
    if (s_n_dd == nullptr) {
        return 1;
    }
    const uint32_t sel = lv_dropdown_get_selected(s_n_dd);
    const int n = static_cast<int>(sel) + 1;
    if (n < 1) {
        return 1;
    }
    if (n > 4) {
        return 4;
    }
    return n;
}

// 状态文案写在底部按钮上（底部不再另放状态行）。
void post_button_text(const char* text, uint32_t session) {
#if defined(HI_SERVICES_PROBE) && HI_SERVICES_PROBE
    printf("AI_IMAGE_STATUS %s\n",text);fflush(stdout);
#endif
    if (text == nullptr) {
        return;
    }
    if (session != s_session.load(std::memory_order_acquire)) {
        return;
    }
    lv_lock();
    if (session == s_session.load(std::memory_order_acquire) && screen_alive() &&
        s_record_lbl != nullptr) {
        // 过长错误体（HTML/JSON）不适合塞进按钮。
        char buf[kMaxButtonTextBytes];
        std::snprintf(buf, sizeof(buf), "%s", text);
        lv_label_set_text(s_record_lbl, buf);
    }
    lv_unlock();
}

void update_button_ui_locked(State st) {
    if (s_record_btn == nullptr || s_record_lbl == nullptr) {
        return;
    }
    switch (st) {
        case State::Idle:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(kColorRecordBtnIdle),
                                      LV_PART_MAIN);
            lv_label_set_text(s_record_lbl, I18n::T(idle_button_text()));
            lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            if (s_n_dd != nullptr) {
                lv_obj_remove_state(s_n_dd, LV_STATE_DISABLED);
            }
            break;
        case State::Recording:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(kColorRecordBtnActive),
                                      LV_PART_MAIN);
            lv_label_set_text(s_record_lbl, I18n::T("已录 0.0 秒"));
            lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            if (s_n_dd != nullptr) {
                lv_obj_add_state(s_n_dd, LV_STATE_DISABLED);
            }
            break;
        case State::Busy:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(kColorRecordBtnBusy),
                                      LV_PART_MAIN);
            // 文案由 post_button_text / tick_timer 覆盖（识别中 / 生成中已用时）
            lv_obj_remove_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            if (s_n_dd != nullptr) {
                lv_obj_add_state(s_n_dd, LV_STATE_DISABLED);
            }
            break;
        case State::Closing:
            break;
    }
}

void set_state_ui(State st, uint32_t session) {
    if (session != s_session.load(std::memory_order_acquire)) {
        return;
    }
    s_state.store(st, std::memory_order_release);
    lv_lock();
    if (session == s_session.load(std::memory_order_acquire) && screen_alive()) {
        update_button_ui_locked(st);
    }
    lv_unlock();
}

// 退出 worker：先恢复 Idle（可点），再可选覆盖错误文案。
void clear_own_worker_handle() {
    if (s_worker_task == xTaskGetCurrentTaskHandle()) {
        s_worker_task = 0;
    }
}

void finish_worker(uint32_t session, const char* button_text = nullptr) {
    s_gen_start_us.store(0, std::memory_order_release);
    set_state_ui(State::Idle, session);
    if (button_text != nullptr && button_text[0] != '\0') {
        post_button_text(button_text, session);
    }
    clear_own_worker_handle();
}

void clear_gallery_locked() {
    // 先删 LVGL 节点，再释放 image holder，避免短暂悬空引用。
    if (s_gallery != nullptr) {
        while (lv_obj_get_child_count(s_gallery) > 0) {
            lv_obj_delete(lv_obj_get_child(s_gallery, 0));
        }
    }
    s_image_holders.clear();
}

void set_prompt_locked(const std::string& prompt) {
    if (s_prompt_lbl == nullptr) {
        return;
    }
    if (prompt.empty()) {
        lv_obj_set_style_text_font(s_prompt_lbl,&font_ai_slogan_32_bold,0);
        lv_obj_set_style_text_align(s_prompt_lbl,LV_TEXT_ALIGN_CENTER,0);
        lv_label_set_text(s_prompt_lbl, "图片AI创意，由你定义！");
    } else {
        lv_obj_set_style_text_font(s_prompt_lbl,&font_puhui_20_4,0);
        lv_obj_set_style_text_align(s_prompt_lbl,LV_TEXT_ALIGN_LEFT,0);
        lv_label_set_text(s_prompt_lbl, prompt.c_str());
    }
}

void add_image_widget(lv_obj_t* parent, const LvglAllocatedImage* holder) {
    if (parent == nullptr || holder == nullptr) {
        return;
    }
    lv_obj_t* iv = lv_image_create(parent);
    lv_obj_set_size(iv, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(iv, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_src(iv, holder->image_dsc());
    lv_obj_center(iv);
    screen_make_input_passive(iv);
}

lv_obj_t* make_image_card(lv_obj_t* parent, const LvglAllocatedImage* holder) {
    lv_obj_t* wrap = lv_obj_create(parent);
    screen_strip_obj_chrome(wrap);
    lv_obj_set_size(wrap, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wrap, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(wrap, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wrap, 8, LV_PART_MAIN);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    add_image_widget(wrap, holder);
    return wrap;
}

void tick_timer_cb(lv_timer_t* /*t*/) {
    if (s_record_lbl == nullptr) {
        return;
    }
    const State st = s_state.load(std::memory_order_acquire);
    if (st == State::Recording) {
        const int64_t start = s_record_start_us.load(std::memory_order_acquire);
        if (start <= 0) {
            lv_label_set_text(s_record_lbl,I18n::T("准备录音…"));return;
        }
        const int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
        char buf[32];
        std::snprintf(buf, sizeof(buf), I18n::T("已录 %d.%d 秒"), ms / 1000,
                      (ms / 100) % 10);
        lv_label_set_text(s_record_lbl, buf);
        return;
    }
    if (st == State::Busy) {
        const int64_t start = s_gen_start_us.load(std::memory_order_acquire);
        if (start <= 0) {
            return;
        }
        const int sec =
            static_cast<int>((esp_timer_get_time() - start) / 1000000);
        char buf[48];
        std::snprintf(buf, sizeof(buf), I18n::T("生成中… 已用时 %d 秒"),
                      sec < 0 ? 0 : sec);
        lv_label_set_text(s_record_lbl, buf);
    }
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
struct HttpJsonResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string err;
};

std::unique_ptr<Http> create_http_client(std::string& err_out) {
    // openvela: Http is a standalone BSD-socket client (ota/ota.h). The
    // reference uses Board::GetInstance().GetNetwork()->CreateHttp(0).
    auto http = std::make_unique<Http>();
    if (http == nullptr) {
        err_out = "create http failed";
        return nullptr;
    }
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
    http->SetDeadlineMs(kHttpTimeoutMs);
    return http;
}

HttpJsonResult http_exchange(const char* method, const std::string& url,
                             std::string body, const char* content_type, bool horde = false, bool public_service=false) {
    HttpJsonResult r;
    std::lock_guard<std::mutex> lock(s_http_mutex);
    auto http = create_http_client(r.err);
    if (http == nullptr) {
        return r;
    }
    if(!horde&&!public_service)http->SetHeader("X-Device-Id",device_id_header());
#if defined(HI_SERVICES_PROBE) && HI_SERVICES_PROBE
    if(!horde&&!public_service)http->SetHeader("X-HiOS-Diagnostic","image-speech");
#endif
    if(horde) {
        http->SetHeader("apikey","0000000000");
        http->SetHeader("Client-Agent","HiOS:1.1:personal");
    }
    if (content_type != nullptr && content_type[0] != '\0') {
        http->SetHeader("Content-Type", content_type);
    }
    if (body.empty()) {
        api::LogHttpRequest(TAG, method, url);
    } else if (content_type != nullptr &&
               std::strcmp(content_type, "application/json") == 0) {
        api::LogHttpRequest(TAG, method, url, body);
        http->SetContent(std::move(body));
    } else {
        api::LogHttpBinaryRequest(TAG, method, url, body.size());
        http->SetContent(std::move(body));
    }
    if (!http->Open(method, url)) {
        r.err = "open failed";
        api::LogHttpResponse(TAG, -1, r.err);
        return r;
    }
    r.status = http->GetStatusCode();
    char chunk[2048];
    while(true) {
        int count=http->Read(chunk,sizeof(chunk));
        if(count==0) break;
        if(count<0 || r.body.size()+count>64*1024) {r.err="响应失败或超过大小限制";http->Close();return r;}
        r.body.append(chunk,count);
    }
    if (std::strcmp(method, "GET") == 0) {
        api::LogHttpResponse(TAG, r.status, api::RedactClawUrlsForLog(r.body));
    } else {
        api::LogHttpResponse(TAG, r.status, r.body);
    }
    http->Close();
    if (r.status < 200 || r.status >= 300) {
        cJSON* error=cJSON_Parse(r.body.c_str());
        r.err="HTTP "+std::to_string(r.status)+": "+HiHorde::Message(error,"网络请求失败");
        cJSON_Delete(error);
        return r;
    }
    r.ok = true;
    return r;
}

HttpJsonResult http_post_binary(const std::string& url, std::string body,
                                const char* content_type) {
    return http_exchange("POST", url, std::move(body), content_type);
}

HttpJsonResult http_post_json(const std::string& url, std::string json) {
    return http_exchange("POST", url, std::move(json), "application/json");
}

HttpJsonResult http_get_json(const std::string& url) {
    return http_exchange("GET", url, std::string(), nullptr);
}

struct BinaryDownload {
    bool ok = false;
    CapsBuffer data;
    size_t size = 0;
    int width=0,height=0;
    std::string err;
};

bool looks_like_image(const uint8_t* buf, size_t total) {
    if (buf == nullptr || total < 3) {
        return false;
    }
    if (buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF) {
        return true;  // JPEG
    }
    if (total >= 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' &&
        buf[3] == 'G') {
        return true;  // PNG
    }
    if (total >= 12 && buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' &&
        buf[3] == 'F' && buf[8] == 'W' && buf[9] == 'E' && buf[10] == 'B' &&
        buf[11] == 'P') {
        return true;  // WebP
    }
    return false;
}

BinaryDownload download_image(const std::string& url) {
    BinaryDownload out;
    std::lock_guard<std::mutex> lock(s_http_mutex);
    auto http = create_http_client(out.err);
    if (http == nullptr) {
        return out;
    }
    // Result storage is a separate HTTPS host. Its handshake and binary
    // transfer need a budget independent of the small JSON polling calls.
    http->SetDeadlineMs(90000);
    if (!http->Open("GET", url)) {
        out.err = "open failed";
        return out;
    }
    if (http->GetStatusCode() != 200) {
        char ebuf[32];
        std::snprintf(ebuf, sizeof(ebuf), "HTTP %d", http->GetStatusCode());
        http->Close();
        out.err = ebuf;
        return out;
    }

    constexpr size_t kInitialCap = 64 * 1024;
    constexpr size_t kChunk = 4 * 1024;
    size_t cap = kInitialCap;
    size_t total = 0;
    CapsBuffer buf = alloc_caps(cap);
    if (!buf) {
        http->Close();
        out.err = I18n::T("内存不足");
        return out;
    }
    CapsBuffer chunk = alloc_caps(kChunk);
    if (!chunk) {
        http->Close();
        out.err = I18n::T("内存不足");
        return out;
    }

    bool read_error = false;
    while (true) {
        int n = http->Read(reinterpret_cast<char*>(chunk.get()), kChunk);
        if (n < 0) {
            read_error = true;
            break;
        }
        if (n == 0) {
            break;
        }
        if (total + static_cast<size_t>(n) > kImageMaxBytes) {
            read_error = true;
            break;
        }
        if (total + static_cast<size_t>(n) > cap) {
            size_t new_cap = cap * 2;
            while (new_cap < total + static_cast<size_t>(n)) {
                new_cap *= 2;
            }
            CapsBuffer nb = alloc_caps(new_cap);
            if (!nb) {
                read_error = true;
                break;
            }
            std::memcpy(nb.get(), buf.get(), total);
            buf = std::move(nb);
            cap = new_cap;
        }
        std::memcpy(buf.get() + total, chunk.get(), n);
        total += static_cast<size_t>(n);
    }
    const size_t expected=http->GetBodyLength();
#if defined(HI_SERVICES_PROBE) && HI_SERVICES_PROBE
    printf("AI_IMAGE_DOWNLOAD bytes=%u expected=%u error=%d\n",unsigned(total),unsigned(expected),http->GetLastError());fflush(stdout);
#endif
    http->Close();

    if (read_error || (expected&&expected!=total) || total == 0 || !looks_like_image(buf.get(), total)) {
        out.err = read_error ? I18n::T("图片下载失败") : I18n::T("非图片响应");
        return out;
    }
    if(total>=12 && std::memcmp(buf.get(),"RIFF",4)==0 && std::memcmp(buf.get()+8,"WEBP",4)==0) {
        uint8_t* pixels=HiHorde::DecodeWebP(buf.get(),total,out.width,out.height);
        if(!pixels){out.err="图片解码失败";return out;}
        out.data.reset(pixels);out.size=size_t(out.width)*out.height*4;out.ok=true;return out;
    }
    out.ok = true;
    out.data = std::move(buf);
    out.size = total;
    return out;
}

bool parse_asr_text(const std::string& body, std::string& text_out,
                    std::string& err_out) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err_out = I18n::T("转写结果解析失败");
        return false;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        err_out = (cJSON_IsString(msg) && msg->valuestring != nullptr)
                      ? msg->valuestring
                      : I18n::T("转写失败");
        cJSON_Delete(root);
        return false;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsString(data) && data->valuestring != nullptr) {
        text_out = data->valuestring;
    } else if (cJSON_IsObject(data)) {
        cJSON* text = cJSON_GetObjectItemCaseSensitive(data, "text");
        if (cJSON_IsString(text) && text->valuestring != nullptr) {
            text_out = text->valuestring;
        }
    }
    cJSON_Delete(root);
    if (text_out.empty()) {
        err_out = I18n::T("未识别到有效文字");
        return false;
    }
    return true;
}

using TaskPollResult=HiHorde::Status;
// Release queued remote work on exit, timeout, or failed retrieval.
struct HordeTask {
    std::string id;
    bool completed=false;
    ~HordeTask() {
        if(!completed&&!id.empty())http_exchange("DELETE",HiHorde::Url("status",id),{},nullptr,true);
    }
};

bool apply_images_ui(uint32_t session,
                     std::vector<BinaryDownload>& images) {
    if (!session_alive(session)) {
        images.clear();
        return false;
    }
    lv_lock();
    if (!session_alive(session) || !screen_alive() || s_gallery == nullptr) {
        lv_unlock();
        images.clear();
        return false;
    }

    clear_gallery_locked();

    std::vector<std::unique_ptr<LvglAllocatedImage>> decoded;
    decoded.reserve(images.size());
    for (auto& img : images) {
        if (!img.ok || !img.data || img.size == 0) {
            continue;
        }
        uint8_t* raw = img.data.release();
        /* NOTE: Original ESP-IDF code wrapped this in try / catch (const
         * std::exception& ex).  We build Metalio with -fno-exceptions on
         * NuttX (smaller .text, no unresolved .gcc_except_table relocs).
         * LvglAllocatedImage now reports decode failures by freeing the
         * raw buffer and leaving image_dsc()->data == nullptr.  We test
         * that and skip the bad entries instead. */
        {
            auto holder = img.width>0
                ?std::make_unique<LvglAllocatedImage>(raw,img.size,img.width,img.height,img.width*4,LV_COLOR_FORMAT_ARGB8888)
                :std::make_unique<LvglAllocatedImage>(raw,img.size);
            if (holder && holder->image_dsc() && holder->image_dsc()->data != nullptr) {
                decoded.push_back(std::move(holder));
            }
            /* else: already freed inside the ctor, nothing more to do */
        }
    }
    images.clear();

    if (decoded.empty()) {
        lv_unlock();
        return false;
    }

    if (decoded.size() == 1) {
        make_image_card(s_gallery, decoded[0].get());
        s_image_holders = std::move(decoded);
        lv_unlock();
        return true;
    }

    lv_obj_t* tv = lv_tabview_create(s_gallery);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, kTabBarH);
    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorTabActive),
                              static_cast<lv_part_t>(LV_PART_ITEMS) |
                                  static_cast<lv_state_t>(LV_STATE_CHECKED));
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText),
                                static_cast<lv_part_t>(LV_PART_ITEMS) |
                                    static_cast<lv_state_t>(LV_STATE_CHECKED));
    screen_swipe_back_ignore(bar, true);

    lv_obj_t* content = lv_tabview_get_content(tv);
    // 禁止内容区左右滑动切 Tab，只能点顶部 Tab 项切换。
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    screen_swipe_back_ignore(content, true);

    for (size_t i = 0; i < decoded.size(); ++i) {
        char tab_name[12];
        std::snprintf(tab_name, sizeof(tab_name), "%d",
                      static_cast<int>(i + 1));
        lv_obj_t* tab = lv_tabview_add_tab(tv, tab_name);
        screen_strip_obj_chrome(tab);
        lv_obj_set_style_bg_color(tab, lv_color_hex(kColorBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
        lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        screen_swipe_back_ignore(tab, true);
        make_image_card(tab, decoded[i].get());
    }

    s_image_holders = std::move(decoded);
    lv_unlock();
    return true;
}

// ---------------------------------------------------------------------------
// Worker: record → ASR → text2image → poll → download
// ---------------------------------------------------------------------------
struct GenerationRequest {uint32_t session;std::string prompt;};
void worker_task(void* arg) {
    std::unique_ptr<GenerationRequest> request(static_cast<GenerationRequest*>(arg));
    const uint32_t session=request->session;
    std::string prompt=std::move(request->prompt);request.reset();
    if(!session_alive(session)){clear_own_worker_handle();/* Returning through the pthread shim releases C++/capture guards. */return;}
    auto& app = Application::GetInstance();
    auto& as = app.GetAudioService();

    if (prompt.empty()) {
    const std::string asr_url=cloud_asr_url();
    if(asr_url.empty()){finish_worker(session,I18n::T("请用文字输入描述"));return;}
    CapsBuffer buffer = alloc_caps(kMaxRecordBytes);
    if (!buffer) {
        buffer = alloc_caps(kMaxRecordBytes / 4);
    }
    if (!buffer) {
        ESP_LOGE(TAG, "no memory for record buffer");
        finish_worker(session, I18n::T("内存不足"));
        /* Returning through the pthread shim releases C++/capture guards. */
        return;
    }

    size_t written = 0;
    std::vector<int16_t> frame;
    frame.reserve(kSamplesPerFrame * 2);
    int64_t start_us = 0;

    {
        HiSpeech::CaptureOwner<AudioService> capture(as);
        if(!capture.Ready()){
            finish_worker(session,I18n::T("麦克风忙，请重试"));/* Returning through the pthread shim releases C++/capture guards. */return;
        }
        WakeWordGuard wake_guard(as);
        if(metalio_nt26_get_call_state()!=METALIO_CALL_IDLE){finish_worker(session,"通话中无法录音");return;}
        Application::GetInstance().RequestVoiceUiDesired(false);
        LocalMusicPlayer::Instance().Stop();RadioPlayer::Instance().Pause();MusicScreen::PauseForVoice();
        for(int i=0;i<300&&!as.LocalPlaybackSettled()&&session_alive(session)&&!s_stop_requested;++i)usleep(10000);
        if(!as.LocalPlaybackSettled()||!session_alive(session)||s_stop_requested){finish_worker(session,"请等待播放停止后重新长按");return;}
        as.ResetDecoder();
        for(int i=0;i<300&&!as.VoicePlaybackSettled()&&session_alive(session)&&!s_stop_requested;++i)usleep(10000);
        if(!as.VoicePlaybackSettled()||!session_alive(session)||s_stop_requested){finish_worker(session,"请等待播报结束后重新长按");return;}
        if(metalio_nt26_get_call_state()!=METALIO_CALL_IDLE||BluetoothScreen::PrepareVoiceRoute()<0||
           metalio_audio_select_local()<0||metalio_nt26_call_control(3,BluetoothScreen::IsHeadsetConnected()?0:1)<0){
            finish_worker(session,"麦克风通路未就绪，请重试");return;
        }
        as.GetCodec()->EnableInput(true);capture.Configure();
        {std::vector<int16_t> settle;for(int i=0;i<2;++i)if(!as.ReadAudioData(settle,kSampleRate,kSamplesPerFrame)){finish_worker(session,"麦克风未就绪，请重试");return;}}
        start_us=esp_timer_get_time();s_record_start_us.store(start_us);
#if defined(HI_SERVICES_PROBE) && HI_SERVICES_PROBE
        int channel_peak[2]={0,0};uint64_t channel_nonzero[2]={0,0};
        printf("AI_AUDIO_ROUTE headset=%d sco=%d voice_ready=%d\n",BluetoothScreen::IsHeadsetConnected(),BluetoothScreen::IsHeadsetCaptureReady(),metalio_bt_voice_ready());
        printf("AI_SPEECH_READY channels=%d\n",capture.Channels());fflush(stdout);
#endif
        while (!s_stop_requested.load(std::memory_order_acquire) &&
               session_alive(session)) {
            if (!as.ReadAudioData(frame, kSampleRate, kSamplesPerFrame)) {
                vTaskDelayMs(5);
                continue;
            }
#if defined(HI_SERVICES_PROBE) && HI_SERVICES_PROBE
            for(size_t i=0;i<frame.size();++i){const int ch=capture.Channels()==2?i%2:0;channel_peak[ch]=std::max(channel_peak[ch],std::abs(int(frame[i])));channel_nonzero[ch]+=frame[i]!=0;}
#endif
            if(!HiSpeech::MicOnly(frame,capture.Channels()))continue;
            const size_t bytes = frame.size() * sizeof(int16_t);
            if (written + bytes > kMaxRecordBytes) {
                break;
            }
            std::memcpy(buffer.get() + written, frame.data(), bytes);
            written += bytes;
            if ((esp_timer_get_time() - start_us) / 1000 >=
                kMaxRecordSeconds * 1000) {
                break;
            }
        }
#if defined(HI_SERVICES_PROBE) && HI_SERVICES_PROBE
        printf("AI_CAPTURE_CHANNELS peak0=%d peak1=%d nz0=%llu nz1=%llu wall_ms=%lld voice_ready=%d\n",channel_peak[0],channel_peak[1],(unsigned long long)channel_nonzero[0],(unsigned long long)channel_nonzero[1],(long long)((esp_timer_get_time()-start_us)/1000),metalio_bt_voice_ready());fflush(stdout);
#endif
    }

    if (!session_alive(session)) {
        clear_own_worker_handle();
        /* Returning through the pthread shim releases C++/capture guards. */
        return;
    }

    const int duration_ms = static_cast<int>(written*1000/(kSampleRate*kBytesPerSample));
    if (duration_ms < kMinRecordMs || written < 1024) {
        finish_worker(session, I18n::T("录音太短，再试一次"));
        /* Returning through the pthread shim releases C++/capture guards. */
        return;
    }
    auto level=HiSpeech::Normalize(reinterpret_cast<int16_t*>(buffer.get()),written/2);
#if defined(HI_SERVICES_PROBE) && HI_SERVICES_PROBE
    printf("AI_SPEECH_PCM ms=%d peak=%d rms=%d gain=%.1f\n",duration_ms,level.peak,level.rms,double(level.gain));fflush(stdout);
#endif
    if(level.peak<80||level.rms<12){finish_worker(session,"声音过轻，请靠近麦克风重试");return;}

    set_state_ui(State::Busy, session);
    post_button_text(I18n::T("识别中…"), session);

    uint8_t wav_header[44];
    fill_wav_header(wav_header, static_cast<uint32_t>(written));
    std::string wav_body;
    wav_body.reserve(sizeof(wav_header) + written);
    wav_body.assign(reinterpret_cast<const char*>(wav_header), sizeof(wav_header));
    wav_body.append(reinterpret_cast<const char*>(buffer.get()), written);
    buffer.reset();

    HttpJsonResult asr = http_post_binary(asr_url,
                                          std::move(wav_body),
                                          "application/octet-stream");
    if (!session_alive(session)) {
        clear_own_worker_handle();
        /* Returning through the pthread shim releases C++/capture guards. */
        return;
    }
    if (!asr.ok) {
        std::string msg = std::string(I18n::T("识别失败: ")) + asr.err;
        finish_worker(session, msg.c_str());
        /* Returning through the pthread shim releases C++/capture guards. */
        return;
    }

    std::string parse_err;
    if (!parse_asr_text(asr.body, prompt, parse_err)) {
        finish_worker(session, parse_err.c_str());
        /* Returning through the pthread shim releases C++/capture guards. */
        return;
    }
    asr.body.clear();
    asr.body.shrink_to_fit();
    }
    std::string parse_err;

    int n = 1;
    lv_lock();
    if (session_alive(session) && screen_alive()) {
        n = selected_image_count();
        set_prompt_locked(prompt);
        clear_gallery_locked();
    }
    lv_unlock();

    s_gen_start_us.store(esp_timer_get_time(), std::memory_order_release);
    post_button_text(I18n::T("生成中… 已用时 0 秒"), session);

    std::string generation_prompt=prompt;
    if(HiImagePrompt::Chinese(prompt)){
        post_button_text("正在理解中文描述…",session);generation_prompt.clear();
        for(const auto& part:HiImagePrompt::Chunks(prompt)){
            if(!session_alive(session)){clear_own_worker_handle();return;}
            auto response=http_exchange("GET","https://api.mymemory.translated.net/get?langpair=zh-CN%7Cen&mt=1&q="+HiImagePrompt::Escape(part),{},nullptr,false,true);
            std::string english;if(!response.ok||!HiImagePrompt::Translation(response.body,english)){finish_worker(session,"中文描述暂时无法理解，请稍后重试");return;}
            if(!generation_prompt.empty())generation_prompt+=" ";generation_prompt+=english;
        }
        post_button_text("生成中…",session);
    }
    std::string req_json=HiHorde::Request(generation_prompt,n);
    if(req_json.empty()){finish_worker(session,I18n::T("描述过长或内存不足"));return;}
    if(!session_alive(session)){clear_own_worker_handle();return;}
    HttpJsonResult create=http_exchange("POST",std::string(HiHorde::kBase)+"async",std::move(req_json),"application/json",true);
    if(!create.ok){finish_worker(session,(std::string("生图失败: ")+create.err).c_str());return;}
    HordeTask task;
    if(!HiHorde::Created(create.body,task.id,parse_err)){finish_worker(session,parse_err.c_str());return;}
    // Parse before the session check so a request accepted during navigation is cancelled.
    if(!session_alive(session)){clear_own_worker_handle();return;}
    create.body.clear();create.body.shrink_to_fit();
    const std::string& task_id=task.id;
#if defined(HI_SERVICES_PROBE) && HI_SERVICES_PROBE
    printf("AI_IMAGE_TASK %s\n",task_id.c_str());fflush(stdout);
#endif

    std::vector<std::string> image_urls;
    int http_fails = 0;
    bool got_images = false;
    for (int attempt = 0; attempt < kMaxPollAttempts; ++attempt) {
        if (!session_alive(session)) {
            clear_own_worker_handle();
            /* Returning through the pthread shim releases C++/capture guards. */
            return;
        }
        vTaskDelayMs(attempt == 0 ? kPollFirstDelayMs : kPollIntervalMs);
        if (!session_alive(session)) {
            clear_own_worker_handle();
            /* Returning through the pthread shim releases C++/capture guards. */
            return;
        }

        HttpJsonResult poll = http_exchange("GET",HiHorde::Url("check",task_id),{},nullptr,true);
        if (!poll.ok) {
            ++http_fails;
            ESP_LOGW(TAG, "poll failed (%d): %s", http_fails, poll.err.c_str());
            if (http_fails >= kMaxPollHttpFails) {
                finish_worker(session, I18n::T("查询任务失败"));
                /* Returning through the pthread shim releases C++/capture guards. */
                return;
            }
            continue;
        }
        TaskPollResult st = HiHorde::Parse(poll.body);
        if(st.ok&&st.finished&&st.err.empty()) {
            auto full=http_exchange("GET",HiHorde::Url("status",task_id),{},nullptr,true);
            if(!full.ok){if(++http_fails>=kMaxPollHttpFails){finish_worker(session,full.err.c_str());return;}continue;}
            st=HiHorde::Parse(full.body,true);
        }
        if (!st.ok) {
            finish_worker(session,
                          st.err.empty() ? I18n::T("查询任务失败")
                                         : st.err.c_str());
            /* Returning through the pthread shim releases C++/capture guards. */
            return;
        }
        http_fails=0;
        if(st.finished)task.completed=true;
        if (!st.image_urls.empty()) {
            image_urls = std::move(st.image_urls);
            got_images = true;
            break;
        }
        if (st.finished && !st.succeeded) {
            finish_worker(session,
                          st.err.empty() ? I18n::T("生图失败") : st.err.c_str());
            /* Returning through the pthread shim releases C++/capture guards. */
            return;
        }
    }

    if (!got_images || image_urls.empty()) {
        finish_worker(session, I18n::T("生成超时，请重试"));
        /* Returning through the pthread shim releases C++/capture guards. */
        return;
    }

    // 下载阶段：停止「生成中已用时」刷新，按钮改为下载中。
    s_gen_start_us.store(0, std::memory_order_release);
    post_button_text(I18n::T("下载中…"), session);

    std::vector<BinaryDownload> images;
    images.reserve(image_urls.size());
    int ok_count = 0;
    for (size_t i = 0; i < image_urls.size(); ++i) {
        if (!session_alive(session)) {
            clear_own_worker_handle();
            /* Returning through the pthread shim releases C++/capture guards. */
            return;
        }
        BinaryDownload dl = download_image(image_urls[i]);
        if(!dl.ok&&session_alive(session)){vTaskDelayMs(500);dl=download_image(image_urls[i]);}
        if (dl.ok) {
            ++ok_count;
        } else {
            ESP_LOGW(TAG, "download image %u failed: %s",
                     static_cast<unsigned>(i), dl.err.c_str());
        }
        images.push_back(std::move(dl));
    }

    if (ok_count == 0) {
        finish_worker(session, I18n::T("图片下载失败"));
        /* Returning through the pthread shim releases C++/capture guards. */
        return;
    }

    if(!apply_images_ui(session, images)){finish_worker(session,I18n::T("图片解码失败"));return;}
    finish_worker(session);
    /* Returning through the pthread shim releases C++/capture guards. */
}

// ---------------------------------------------------------------------------
// Events / UI build
// ---------------------------------------------------------------------------
void on_record_pressed(lv_event_t* /*e*/) {
    if(cloud_asr_url().empty()){if(s_state.load()==State::Idle)open_text_composer(nullptr);return;}
    if (s_state.load(std::memory_order_acquire) != State::Idle) {
        return;
    }
    if(metalio_nt26_get_call_state()!=METALIO_CALL_IDLE){
        if(s_record_lbl)lv_label_set_text(s_record_lbl,"通话中无法录音");return;
    }
    auto& app = Application::GetInstance();
    const DeviceState ds = app.GetDeviceState();
    if (ds == kDeviceStateConnecting || ds == kDeviceStateListening ||
        ds == kDeviceStateSpeaking || ds == kDeviceStateUpgrading) {
        if (s_record_lbl != nullptr) {
            lv_label_set_text(s_record_lbl, I18n::T("请先结束当前对话"));
        }
        return;
    }
    if (s_worker_task != 0) {
        return;
    }

    s_stop_requested.store(false, std::memory_order_release);
    s_record_start_us.store(0,std::memory_order_release);
    s_state.store(State::Recording, std::memory_order_release);
    update_button_ui_locked(State::Recording);

    auto* request=new GenerationRequest{s_session.load(std::memory_order_acquire),{}};
    BaseType_t ok = xTaskCreate(worker_task, "ai_img_gen", 32768, request, 5,
                                &s_worker_task);
    if (ok != pdPASS) {
        delete request;
        ESP_LOGE(TAG, "create worker failed");
        s_worker_task = 0;
        s_state.store(State::Idle, std::memory_order_release);
        update_button_ui_locked(State::Idle);
        if (s_record_lbl != nullptr) {
            lv_label_set_text(s_record_lbl, I18n::T("任务启动失败"));
        }
    }
}

lv_obj_t* s_text_sheet=nullptr;
void close_text_composer() {
    if(!s_text_sheet)return;
    HiInputMethod_Suspend();auto* old=s_text_sheet;s_text_sheet=nullptr;
    lv_obj_add_flag(old,LV_OBJ_FLAG_HIDDEN);lv_obj_delete_async(old);
}
void open_text_composer(lv_event_t*) {
    if(s_state.load()!=State::Idle || s_worker_task)return;
    if(s_text_sheet)return;
    auto* sheet=lv_obj_create(s_screen);s_text_sheet=sheet;
    screen_strip_obj_chrome(sheet);lv_obj_set_style_pad_all(sheet,0,0);
    lv_obj_set_size(sheet,688,224);lv_obj_set_pos(sheet,16,kHeaderH+12);
    lv_obj_remove_flag(sheet,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(sheet,lv_color_hex(kColorCard),0);
    lv_obj_set_style_bg_opa(sheet,LV_OPA_COVER,0);
    lv_obj_set_style_text_font(sheet,&font_puhui_20_4,0);
    lv_obj_set_style_text_color(sheet,lv_color_hex(kColorText),0);screen_swipe_back_ignore(sheet,true);
    auto* title=lv_label_create(sheet);lv_label_set_text(title,"文字生图");lv_obj_set_pos(title,16,10);
    auto* field=lv_textarea_create(sheet);lv_obj_set_size(field,522,144);lv_obj_set_pos(field,14,58);
    lv_textarea_set_max_length(field,512);lv_textarea_set_placeholder_text(field,"描述你想画的内容…");
    lv_obj_set_style_text_font(field,&font_puhui_20_4,0);
    lv_obj_set_style_bg_color(field,lv_color_hex(kColorBg),0);
    lv_obj_set_style_text_color(field,lv_color_hex(kColorText),0);HiInputMethod_Bind(field,true);
    auto* close=lv_button_create(sheet);lv_obj_set_size(close,96,42);lv_obj_set_pos(close,574,6);
    auto* label=lv_label_create(close);lv_label_set_text(label,"关闭");lv_obj_center(label);
    lv_obj_add_event_cb(close,[](lv_event_t*){close_text_composer();},LV_EVENT_CLICKED,nullptr);
    auto* send=lv_button_create(sheet);lv_obj_set_size(send,116,144);lv_obj_set_pos(send,552,58);
    label=lv_label_create(send);lv_label_set_text(label,"生成");lv_obj_center(label);
    lv_obj_add_event_cb(send,[](lv_event_t* event){
        auto* field=static_cast<lv_obj_t*>(lv_event_get_user_data(event));
        if(!HiInputMethod_Flush(field))return;
        std::string prompt=lv_textarea_get_text(field);
        if(prompt.find_first_not_of(" \r\n\t")==std::string::npos || prompt.size()>2048)return;
        if(s_state.load()!=State::Idle || s_worker_task)return;
        auto* request=new GenerationRequest{s_session.load(std::memory_order_acquire),std::move(prompt)};
        s_stop_requested=false;s_state=State::Busy;update_button_ui_locked(State::Busy);
        if(xTaskCreate(worker_task,"ai_img_gen",32768,request,5,&s_worker_task)!=pdPASS){
            delete request;s_worker_task=0;s_state=State::Idle;update_button_ui_locked(State::Idle);return;
        }
        close_text_composer();
    },LV_EVENT_CLICKED,field);
    HiInputMethod_Show(field,true);
}

void on_record_released(lv_event_t* /*e*/) {
    if (s_state.load() == State::Recording) {
        s_stop_requested.store(true);
    }
}

void on_swipe_back()
{
#if defined(HI_SERVICES_PROBE) && HI_SERVICES_PROBE
    printf("AI_IMAGE_BACK_REQUEST\n");fflush(stdout);
#endif
    HomeScreen::SwitchToHome();
}

void on_back_clicked(lv_event_t* /*e*/) { on_swipe_back(); }

void on_screen_unloaded(lv_event_t* e) {
    lv_obj_t* scr = lv_event_get_target_obj(e);
    // 若本屏已被更新的 Create() 替换，静态指针属于新实例，不能在这里清掉。
    if (scr != s_screen) {
        ESP_LOGI(TAG, "unload: stale screen instance ignored");
        return;
    }
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Closing, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);

    if (s_tick_timer != nullptr) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = nullptr;
    }
    close_text_composer();
    clear_gallery_locked();
    s_screen = nullptr;
    s_prompt_lbl = nullptr;
    s_gallery = nullptr;
    s_record_btn = nullptr;
    s_record_lbl = nullptr;
    s_n_dd = nullptr;
}

void align_count_value(lv_obj_t* dd,lv_obj_t* label){
    if(!dd||!label)return;char text[2]={char('1'+lv_dropdown_get_selected(dd)),0};lv_label_set_text(label,text);
    lv_obj_update_layout(label);const auto* font=&font_puhui_20_4;lv_font_glyph_dsc_t glyph{};
    if(!lv_font_get_glyph_dsc(font,&glyph,text[0],0)){lv_obj_center(label);return;}
    const int dx=(lv_obj_get_width(label)-glyph.box_w)/2-glyph.ofs_x;
    const int dy=(lv_obj_get_height(label)+glyph.box_h)/2-font->line_height+font->base_line+glyph.ofs_y;
    lv_obj_align(label,LV_ALIGN_CENTER,dx,dy);
}
void on_dropdown_ready(lv_event_t* e) {
    lv_obj_t* dd = lv_event_get_target_obj(e);
    if (dd == nullptr) {
        return;
    }
    lv_obj_t* list = lv_dropdown_get_list(dd);
    if (list == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_opa(list,LV_OPA_COVER,LV_PART_MAIN);
    lv_obj_set_style_radius(list, 10, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 280, LV_PART_MAIN);
    screen_swipe_back_ignore(list, true);
}

void build_header(lv_obj_t* parent) {
    lv_obj_t* top = lv_obj_create(parent);
    screen_strip_obj_chrome(top);
    lv_obj_set_size(top, kPanelW, kHeaderH);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(top, lv_color_hex(kColorHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* divider = lv_obj_create(top);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

    lv_obj_t* back = lv_button_create(top);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, kHeaderSidePad, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(back, on_back_clicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(top);
    lv_label_set_text(title, I18n::T("AI生图"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, kHeaderSidePad + kBackBtnSize + 8,
                 0);
    screen_make_input_passive(title);

    lv_obj_t* n_lbl = lv_label_create(top);
    lv_label_set_text(n_lbl, I18n::T("生成图片数量"));
    lv_obj_set_style_text_font(n_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(n_lbl, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_align(n_lbl, LV_ALIGN_RIGHT_MID, -140, 0);
    screen_make_input_passive(n_lbl);

    s_n_dd = lv_dropdown_create(top);
    lv_obj_set_size(s_n_dd, 100, 48);
    lv_obj_align(s_n_dd, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_radius(s_n_dd, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_n_dd, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_n_dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_n_dd, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_n_dd, lv_color_hex(0x4B5563),
                                  LV_PART_MAIN);
    lv_obj_set_style_text_color(s_n_dd, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_n_dd, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_n_dd,LV_TEXT_ALIGN_CENTER,LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_n_dd,12,LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_n_dd,12,LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_n_dd,0,LV_PART_MAIN);
    lv_dropdown_set_symbol(s_n_dd, nullptr);
    static const lv_point_precise_t arrow_points[]={{0,0},{7,7},{14,0}};
    auto* arrow=lv_line_create(s_n_dd);lv_line_set_points(arrow,arrow_points,3);
    lv_obj_set_style_line_width(arrow,3,0);lv_obj_set_style_line_color(arrow,lv_color_hex(kColorText),0);
    lv_obj_align(arrow,LV_ALIGN_RIGHT_MID,-4,0);screen_make_input_passive(arrow);
    lv_obj_set_style_text_font(s_n_dd, &font_awesome_20_4, LV_PART_INDICATOR);
    lv_dropdown_set_options(s_n_dd, "1\n2\n3\n4");
    lv_dropdown_set_selected(s_n_dd, 0);
    // Centre the visible digit bounds; the dropdown's font baseline otherwise
    // leaves numerals lower than the frame's optical centre.
    lv_obj_set_style_text_opa(s_n_dd,LV_OPA_TRANSP,LV_PART_MAIN);
    auto* value=lv_label_create(s_n_dd);lv_obj_remove_style_all(value);
    lv_obj_set_style_text_font(value,&font_puhui_20_4,0);lv_obj_set_style_text_color(value,lv_color_hex(kColorText),0);
    lv_obj_set_style_text_opa(value,LV_OPA_COVER,0);screen_make_input_passive(value);
    lv_obj_set_user_data(s_n_dd,value);align_count_value(s_n_dd,value);
    lv_obj_add_event_cb(s_n_dd,[](lv_event_t* e){align_count_value(lv_event_get_target_obj(e),static_cast<lv_obj_t*>(lv_event_get_user_data(e)));},LV_EVENT_VALUE_CHANGED,value);
    lv_obj_add_event_cb(s_n_dd, on_dropdown_ready, LV_EVENT_READY, nullptr);
    screen_swipe_back_ignore(s_n_dd, true);
}

void build_body(lv_obj_t* parent) {
    lv_obj_t* body = lv_obj_create(parent);
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kPanelW, kBodyH);
    lv_obj_set_pos(body, 0, kHeaderH);
    lv_obj_set_style_bg_color(body, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_right(body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_top(body, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(body, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, LV_PART_MAIN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    s_prompt_lbl = lv_label_create(body);
    lv_obj_set_width(s_prompt_lbl, LV_PCT(100));
    lv_obj_set_height(s_prompt_lbl, kPromptAreaH);
    lv_label_set_long_mode(s_prompt_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_prompt_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_prompt_lbl, lv_color_hex(kColorPromptText),
                                LV_PART_MAIN);
    set_prompt_locked("");
    lv_obj_add_flag(s_prompt_lbl,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_prompt_lbl,open_text_composer,LV_EVENT_CLICKED,nullptr);
    screen_swipe_back_ignore(s_prompt_lbl,true);

    s_gallery = lv_obj_create(body);
    screen_strip_obj_chrome(s_gallery);
    lv_obj_set_width(s_gallery, LV_PCT(100));
    lv_obj_set_height(s_gallery, kGalleryH);
    lv_obj_set_style_bg_opa(s_gallery, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(s_gallery, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(s_gallery, true);
}

void build_footer(lv_obj_t* parent) {
    lv_obj_t* footer = lv_obj_create(parent);
    screen_strip_obj_chrome(footer);
    lv_obj_set_size(footer, kPanelW, kFooterH);
    lv_obj_set_pos(footer, 0, kPanelH - kFooterH);
    lv_obj_set_style_bg_color(footer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int32_t kBtnW = 400;
    constexpr int32_t kBtnH = 72;
    s_record_btn = lv_button_create(footer);
    lv_obj_set_size(s_record_btn, kBtnW, kBtnH);
    lv_obj_align(s_record_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_record_btn, kBtnH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_record_btn, lv_color_hex(kColorRecordBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_record_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_record_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_record_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_record_btn,
                              lv_color_hex(kColorRecordBtnActive),
                              LV_PART_MAIN | LV_STATE_PRESSED);

    s_record_lbl = lv_label_create(s_record_btn);
    lv_label_set_text(s_record_lbl, I18n::T(idle_button_text()));
    lv_obj_set_style_text_color(s_record_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_record_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(s_record_lbl);

    lv_obj_add_event_cb(s_record_btn, on_record_pressed, LV_EVENT_PRESSED,
                        nullptr);
    lv_obj_add_event_cb(s_record_btn, on_record_released, LV_EVENT_RELEASED,
                        nullptr);
    screen_swipe_back_ignore(s_record_btn, true);
    auto* text_btn=lv_button_create(footer);lv_obj_set_size(text_btn,112,kBtnH);
    lv_obj_align(text_btn,LV_ALIGN_RIGHT_MID,-24,0);
    lv_obj_set_style_bg_color(text_btn,lv_color_hex(kColorCard),0);
    lv_obj_set_style_text_font(text_btn,&font_puhui_20_4,0);
    auto* text_lbl=lv_label_create(text_btn);lv_label_set_text(text_lbl,"文字");lv_obj_center(text_lbl);
    lv_obj_add_event_cb(text_btn,open_text_composer,LV_EVENT_CLICKED,nullptr);
    screen_swipe_back_ignore(text_btn,true);
}

}  // namespace

// ---- Screen base class interface ----

lv_obj_t* AiImageGenScreen::CreateStatic() {
    // 递增 session，让可能仍在跑的旧 worker 自行退出；不要强行清空
    // s_worker_task，否则可能与仍存活的旧任务交错启动第二个 worker。
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Idle, std::memory_order_release);
    s_stop_requested.store(false, std::memory_order_release);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);
    build_body(scr);
    build_footer(scr);

    s_tick_timer = lv_timer_create(tick_timer_cb, 100, nullptr);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    screen_attach_swipe_back(scr, on_swipe_back);
    return scr;
}

lv_obj_t* AiImageGenScreen::Create() {
    root_ = CreateStatic();
    return root_;
}

#if defined(HI_SERVICES_PROBE) && HI_SERVICES_PROBE
extern "C" bool hi_image_probe_count_centered(){
    if(!s_n_dd)return false;auto* value=static_cast<lv_obj_t*>(lv_obj_get_user_data(s_n_dd));if(!value)return false;
    const auto previous=lv_dropdown_get_selected(s_n_dd);bool ok=true;
    for(unsigned i=0;i<4;++i){lv_dropdown_set_selected(s_n_dd,i);lv_obj_send_event(s_n_dd,LV_EVENT_VALUE_CHANGED,nullptr);lv_obj_update_layout(s_n_dd);
        lv_area_t box,label;lv_obj_get_coords(s_n_dd,&box);lv_obj_get_coords(value,&label);lv_font_glyph_dsc_t g{};
        if(!lv_font_get_glyph_dsc(&font_puhui_20_4,&g,'1'+i,0)){ok=false;continue;}
        const int x1=label.x1+g.ofs_x,y1=label.y1+font_puhui_20_4.line_height-font_puhui_20_4.base_line-g.box_h-g.ofs_y;
        const int dx=2*x1+g.box_w-1-box.x1-box.x2,dy=2*y1+g.box_h-1-box.y1-box.y2;
        printf("IMAGE_COUNT_INK value=%u dx2=%d dy2=%d\n",i+1,dx,dy);fflush(stdout);ok=ok&&std::abs(dx)<=2&&std::abs(dy)<=2;
    }lv_dropdown_set_selected(s_n_dd,previous);lv_obj_send_event(s_n_dd,LV_EVENT_VALUE_CHANGED,nullptr);return ok;
}
extern "C" bool hi_image_probe_record_ready(){return s_record_start_us.load()>0&&s_state.load()==State::Recording;}
extern "C" const char* hi_image_probe_prompt(){return s_prompt_lbl?lv_label_get_text(s_prompt_lbl):"";}
extern "C" int hi_image_probe_status() {
    if(!s_screen||lv_screen_active()!=s_screen)return -1;
    if(s_state.load()==State::Busy)return 1;
    return s_image_holders.empty()?0:2;
}
extern "C" const char* hi_image_probe_text() {
    return s_record_lbl?lv_label_get_text(s_record_lbl):"no image page";
}
#endif

void AiImageGenScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: ai_image_gen_screen");
        return;
    }
    ESP_LOGI(TAG, "unload: ai_image_gen_screen");
    s_state.store(State::Closing, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);
    // 生图页不持有语音 UI 会话；只确保不误开唤醒词。
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}
