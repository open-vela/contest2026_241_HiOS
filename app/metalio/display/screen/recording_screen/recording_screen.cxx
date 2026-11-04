#include "hi_task_policy.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * RecordingScreen — ported from MetalioClaw4
 * main/display/screen/recording_screen/recording_screen.cc (2330 lines).
 *
 * Adaptations for openvela/NuttX:
 *   - ESP-IDF headers (esp_log / esp_timer / freertos / esp_heap_caps /
 *     esp_lv_adapter) → NuttX shims (esp_log_shim.h / esp_timer_shim.h /
 *     freertos_shim.h / lv_lock()/lv_unlock()).
 *   - heap_caps_malloc(_, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM) → malloc.
 *     (PSRAM is uniformly available on ESP32-P4; on NuttX the libc heap
 *     is the unified allocator.)
 *   - HomeScreen::Create() → HomeScreen::CreateStatic() (openvela port
 *     convention — every screen returns through the static entry point).
 *   - esp_lv_adapter_lock(-1) / esp_lv_adapter_unlock() → lv_lock() /
 *     lv_unlock() (LVGL v9 native lock API; the openvela LVGL adapter
 *     routes these to the same mutex the ESP-IDF adapter used).
 *   - ASR uses the NuttX HTTP client with bounded, streaming uploads.
 *
 * Preserved verbatim:
 *   - Full LVGL widget tree: header (back + title), tabview (录音/列表),
 *     record card (timer + status + record button + tip), file list
 *     (rows with name/duration/size + delete), detail overlay (title +
 *     meta + play/transcribe buttons + status + result scroll box).
 *   - All event handlers (record start/stop, list row click, delete,
 *     detail play, detail ASR, detail back, tab change, swipe-back).
 *   - Helper functions: Ogg page writer, Opus header writer, duration
 *     sidecar read/write, opus tail parser, file size formatter,
 *     timer formatter, MAC-derived filename serial.
 *   - State machine (Idle/Recording/Saving/Playing) and worker tasks
 *     (RecordTask / PlayTask / AsrUploadTask / AsrQueryTask /
 *     DurationFillTask) with the same FreeRTOS-shaped lifecycle that
 *     the freertos_shim maps onto NuttX tasks.
 */

#include "recording_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "esp_timer_shim.h"
#include "esp_err_shim.h"
#include "freertos_shim.h"
#include "screen_util.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "application.h"
#include "api_endpoints.h"
#include "ota.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "hi_music_screen/local_music_player.h"
#include "radio_screen/radio_player.h"
#include "music_screen/music_screen.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include <sched.h>
#include "board_shim.h"
#include "home_screen/home_screen.h"
#include "system_info.h"
#include "wifi_required_dialog.h"
#include "SdCardManager.hpp"
#include <metalio/metalio.h>

#include "cJSON_compat.h"

/* FreeRTOS priority baseline — NuttX sched.h has SCHED_PRIORITY_MIN; the
 * ESP-IDF tskIDLE_PRIORITY symbol is not always defined through the shim
 * layer, so provide a sane fallback. */
#ifndef tskIDLE_PRIORITY
#define tskIDLE_PRIORITY 0
#endif

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
// 注意：font_puhui_number_50_4 只有 % 和 0-9，没有冒号，计时不能用它。

namespace {

constexpr const char *TAG = "RecordingScreen";

constexpr int32_t kPanelSize = 720;
constexpr int32_t kHeaderH = 90;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kTabBarH = 64;
constexpr int32_t kBodyH = kPanelSize - kHeaderH;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorTabBar = 0x12151C;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorSubtle = 0x9AA3B2;
constexpr uint32_t kColorAccent = 0x3B82F6;
constexpr uint32_t kColorRecord = 0xDC2626;
constexpr uint32_t kColorCard = 0x1B2030;
constexpr uint32_t kColorDanger = 0xEF4444;

constexpr const char *kPosixDir = "/sdcard/recordings";
constexpr int kSampleRate = 16000;
constexpr int kFrameDurationMs = OPUS_FRAME_DURATION_MS;  // 60
constexpr int kSamplesPerFrame = kSampleRate * kFrameDurationMs / 1000;  // 960
// Opus granule 固定按 48 kHz 计：60ms → 2880
constexpr int64_t kGranulePerFrame = 48000 * kFrameDurationMs / 1000;
constexpr int kMaxRecordSeconds = 30 * 60;
constexpr int kMinRecordMs = 300;
constexpr int kPlayChunkSamples = kSamplesPerFrame;
constexpr int kMaxListItems = 80;
constexpr uint32_t kOggSerial = 0x4F505553;  // 'OPUS'

enum class RecFormat : uint8_t {
    Opus = 0,
    Wav,
};

enum class RecState : uint8_t {
    Idle = 0,
    Recording,
    Saving,
    Playing,
};

struct UiState {
    lv_obj_t *screen = nullptr;
    lv_obj_t *tabview = nullptr;
    lv_obj_t *timer_lbl = nullptr;
    lv_obj_t *status_lbl = nullptr;
    lv_obj_t *record_btn = nullptr;
    lv_obj_t *record_btn_lbl = nullptr;
    lv_obj_t *list_scroll = nullptr;
    lv_obj_t *list_empty = nullptr;
    lv_obj_t *list_status = nullptr;

    // 录音详情覆盖层
    lv_obj_t *detail_panel = nullptr;
    lv_obj_t *detail_title = nullptr;
    lv_obj_t *detail_meta = nullptr;
    lv_obj_t *detail_play_btn = nullptr;
    lv_obj_t *detail_play_lbl = nullptr;
    lv_obj_t *detail_asr_btn = nullptr;
    lv_obj_t *detail_asr_lbl = nullptr;
    lv_obj_t *detail_status = nullptr;
    lv_obj_t *detail_result = nullptr;
};

UiState s_ui;
bool s_screen_alive = false;
bool s_sd_ready = false;
uint32_t s_screen_generation = 0;
uint32_t s_record_generation = 0;
bool s_detail_open = false;
int s_detail_idx = -1;

std::atomic<RecState> s_state{RecState::Idle};
std::atomic<bool> s_stop_record{false};
std::atomic<bool> s_stop_play{false};
std::atomic<bool> s_stop_asr{false};
std::atomic<int64_t> s_record_start_us{0};
std::atomic<uint32_t> s_recorded_frames{0};

std::atomic<TaskHandle_t> s_record_task{0};
std::atomic<TaskHandle_t> s_play_task{0};
TaskHandle_t s_asr_task = 0;        // 上传转写
TaskHandle_t s_asr_query_task = 0;  // 查询转写结果
TaskHandle_t s_meta_task = 0;
lv_timer_t *s_tick_timer = nullptr;
bool s_wake_disabled_by_us = false;
std::atomic<bool> s_stop_meta{false};

char s_playing_path[192] = {};
char s_detail_path[192] = {};
char s_detail_name[96] = {};

struct FileEntry {
    char name[96];
    char path[160];
    int duration_sec = 0;
    size_t size_bytes = 0;
    RecFormat format = RecFormat::Opus;
    bool duration_ready = false;  // sidecar / WAV 尺寸已就绪；否则后台补算
    time_t modified = 0;
};

std::vector<FileEntry> s_files;
// Workers own hardware/files; only the LVGL timer owns UI state transitions.
// Completion is published after every resource is released, including failures.
struct WorkerUpdates {
    std::mutex lock;
    std::string record_status,play_status;
    bool record_done=false,play_done=false,record_ok=false;
    uint32_t samples=0,record_generation=0,play_generation=0;
    bool files_ready=false;uint32_t files_generation=0;std::vector<FileEntry> files;
};
WorkerUpdates s_updates;
lv_timer_t *s_worker_ui_timer=nullptr;
uint32_t s_play_generation=0;
bool s_record_after_play=false;
bool s_scan_busy=false,s_refresh_again=false,s_fill_durations=true;


// ---------------------------------------------------------------------------
// Reference ASR endpoints over the board's socket/TLS client.
// ---------------------------------------------------------------------------
struct AsrUploadResponse {
    bool ok = false;
    int status = 0;
    std::string body;
};

struct AsrQueryResponse {
    bool ok = false;
    int status = 0;
    std::string body;
};

class RecordingHttpBackend {
public:
    virtual ~RecordingHttpBackend() = default;
    virtual AsrUploadResponse UploadTranscribe(const char *path,
                                               const char *filename) = 0;
    virtual AsrQueryResponse QueryAudioRecords(const char *filename) = 0;
};

class SocketRecordingHttpBackend : public RecordingHttpBackend {
    static bool ReadResponse(Http &http, std::string &body) {
        char chunk[1024];
        for (;;) {
            int n = http.Read(chunk, sizeof(chunk));
            if (n < 0) return false;
            if (n == 0) return true;
            if (body.size() + static_cast<size_t>(n) > 256 * 1024) return false;
            body.append(chunk, n);
        }
    }
    static void Headers(Http &http) {
        http.SetHeader("Accept", "*/*");
        http.SetHeader("X-Device-Id", SystemInfo::GetMacAddress());
    }
public:
    AsrUploadResponse UploadTranscribe(const char *path, const char *filename) override {
        AsrUploadResponse r;
        if (!path || !filename) return r;
        std::string name(filename);
        // File names form a quoted MIME parameter, never raw header text.
        for (char &c : name) if (c == '\r' || c == '\n' || c == '"' || c == '\\') c = '_';
        const std::string boundary = "----XingzhiAsrBoundary";
        const std::string prefix = "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + name +
            "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
        Http http;
        Headers(http);
        http.SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        http.SetContentFile(path, prefix, "\r\n--" + boundary + "--\r\n");
        if (!http.Open("POST", api::Url(api::kAsrTranscribe))) return r;
        r.status = http.GetStatusCode();
        r.ok = ReadResponse(http, r.body);
        return r;
    }
    AsrQueryResponse QueryAudioRecords(const char *filename) override {
        AsrQueryResponse r;
        if (!filename || !*filename) return r;
        Http http;
        Headers(http);
        if (!http.Open("GET", api::AsrAudioRecordsUrl(filename))) return r;
        r.status = http.GetStatusCode();
        r.ok = ReadResponse(http, r.body);
        return r;
    }
};

RecordingHttpBackend *g_recording_http = nullptr;

RecordingHttpBackend *GetRecordingHttp() {
    if (g_recording_http == nullptr) {
        static SocketRecordingHttpBackend s_http;
        g_recording_http = &s_http;
    }
    return g_recording_http;
}

// Public hook (declared in the header for tests / future wiring). Default
// linkage is internal to this TU.
extern "C" void recording_screen_set_http_backend(RecordingHttpBackend *b) {
    g_recording_http = b;
}

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

void OnSwipeBack();
void OnBackClicked(lv_event_t *e);
void UpdateRecordButtonUi();
void UpdateTimerLabel();
void RebuildFileList();
void RebuildFileList(bool schedule_fill);
void RenderFileList(bool schedule_fill);
void StopPlayback();
void ForceStopAll();
void ShowDetail(int idx);
void HideDetail();
void UpdateDetailPlayButton();
void SetDetailStatusText(const char *text);
void SetDetailResultText(const char *text);
void BuildDetailPanel(lv_obj_t *parent);
void ScheduleDurationFill();
void ScheduleAsrQuery();
bool WriteDurationSidecar(const char *opus_path, int duration_sec);
int ReadDurationSidecar(const char *opus_path);
void UnlinkDurationSidecar(const char *opus_path);
int DurationSecFromOpusFileTail(const char *path);
int EnsureOpusDuration(FileEntry &entry);

// ---------------------------------------------------------------------------
// LVGL lock helpers — replace esp_lv_adapter_lock(-1)/unlock with the LVGL
// v9 native API. lv_lock() returns bool (true on success). The original
// code compared esp_lv_adapter_lock(-1) == ESP_OK; we keep the same shape
// by returning ESP_OK / ESP_FAIL.
// ---------------------------------------------------------------------------
inline esp_err_t LvglLock() {
    lv_lock();
    return ESP_OK;
}
inline void LvglUnlock() {
    lv_unlock();
}

// heap_caps_* aliases — the original used heap_caps_malloc with
// MALLOC_CAP_8BIT / MALLOC_CAP_SPIRAM. NuttX's libc malloc is the unified
// allocator (PSRAM is part of the heap on ESP32-P4), so just route through.
inline void *HeapMalloc(size_t n) { return malloc(n); }
inline void HeapFree(void *p) { free(p); }

bool EnsureRecordingsDir() {
    struct stat st;
    if (stat(kPosixDir, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(kPosixDir, 0755) == 0) {
        return true;
    }
    const int error = errno;
    if (error == EEXIST && stat(kPosixDir, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    ESP_LOGE(TAG, "mkdir(%s) failed errno=%d", kPosixDir, error);
    return false;
}

void PutLe16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void PutLe32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void PutLe64(uint8_t *p, int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((u >> (8 * i)) & 0xFF);
    }
}

uint32_t ReadLe32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int64_t ReadLe64(const uint8_t *p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) {
        u |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return static_cast<int64_t>(u);
}

// Ogg CRC（多项式 0x04C11DB7，与 RFC 3533 一致）
uint32_t *OggCrcTable() {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int j = 0; j < 8; ++j) {
                r = (r & 0x80000000u) ? ((r << 1) ^ 0x04c11db7u) : (r << 1);
            }
            table[i] = r;
        }
        ready = true;
    }
    return table;
}

uint32_t OggCrc(const uint8_t *data, size_t len) {
    const uint32_t *table = OggCrcTable();
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc << 8) ^ table[((crc >> 24) & 0xFFu) ^ data[i]];
    }
    return crc;
}

bool WriteOggPage(FILE *f, uint8_t header_type, int64_t granule, uint32_t serial,
                  uint32_t page_seq, const uint8_t *body, size_t body_len) {
    if (f == nullptr) {
        return false;
    }

    std::vector<uint8_t> segs;
    if (body_len == 0) {
        segs.push_back(0);
    } else {
        size_t rem = body_len;
        while (rem >= 255) {
            segs.push_back(255);
            rem -= 255;
        }
        segs.push_back(static_cast<uint8_t>(rem));
    }
    if (segs.size() > 255) {
        ESP_LOGE(TAG, "ogg packet too large: %u", static_cast<unsigned>(body_len));
        return false;
    }

    const size_t header_len = 27 + segs.size();
    std::vector<uint8_t> page(header_len + body_len);
    std::memcpy(page.data(), "OggS", 4);
    page[4] = 0;
    page[5] = header_type;
    PutLe64(page.data() + 6, granule);
    PutLe32(page.data() + 14, serial);
    PutLe32(page.data() + 18, page_seq);
    PutLe32(page.data() + 22, 0);  // checksum placeholder
    page[26] = static_cast<uint8_t>(segs.size());
    std::memcpy(page.data() + 27, segs.data(), segs.size());
    if (body_len > 0) {
        std::memcpy(page.data() + header_len, body, body_len);
    }

    const uint32_t crc = OggCrc(page.data(), page.size());
    PutLe32(page.data() + 22, crc);

    return fwrite(page.data(), 1, page.size(), f) == page.size();
}

bool WriteOpusHeaders(FILE *f, uint32_t serial, uint32_t &page_seq) {
    uint8_t head[19] = {};
    std::memcpy(head, "OpusHead", 8);
    head[8] = 1;   // version
    head[9] = 1;   // mono
    PutLe16(head + 10, 0);  // pre_skip
    PutLe32(head + 12, static_cast<uint32_t>(kSampleRate));
    PutLe16(head + 16, 0);  // output gain
    head[18] = 0;  // mapping family
    if (!WriteOggPage(f, 0x02 /* BOS */, 0, serial, page_seq++, head, sizeof(head))) {
        return false;
    }

    const char *vendor = "xingzhi-ai";
    const uint32_t vendor_len = static_cast<uint32_t>(std::strlen(vendor));
    std::vector<uint8_t> tags(8 + 4 + vendor_len + 4);
    std::memcpy(tags.data(), "OpusTags", 8);
    PutLe32(tags.data() + 8, vendor_len);
    std::memcpy(tags.data() + 12, vendor, vendor_len);
    PutLe32(tags.data() + 12 + vendor_len, 0);  // user comment list count
    return WriteOggPage(f, 0x00, 0, serial, page_seq++, tags.data(), tags.size());
}

void KeepLeftChannelOnly(std::vector<int16_t> &samples) {
    if (samples.size() < 2) {
        return;
    }
    const size_t mono_count = samples.size() / 2;
    for (size_t i = 0, j = 0; i < mono_count; ++i, j += 2) {
        samples[i] = samples[j];
    }
    samples.resize(mono_count);
}

// A stateful DC/rumble filter, with no automatic gain on quiet recordings.
// Kept inside the recording path; never changes shared codec/I2S settings.
class RecordingPcmFilter {
public:
    void Apply(std::vector<int16_t> &pcm) {
        for (auto &sample : pcm) {
            const int32_t x=sample;
            y_=x-x_+static_cast<int32_t>(static_cast<int64_t>(y_)*31900/32768);
            x_=x;
            sample=static_cast<int16_t>(std::max(-30000,std::min(30000,static_cast<int>(y_))));
        }
    }
private:
    int32_t x_=0,y_=0;
};

void DisableWakeWordIfNeeded() {
    auto &as = Application::GetInstance().GetAudioService();
    if (!as.IsWakeWordRunning()) {
        return;
    }
    as.EnableWakeWordDetection(false);
    s_wake_disabled_by_us = true;
    vTaskDelayMs(100);
}

void RestoreWakeWordIfNeeded() {
    if (!s_wake_disabled_by_us) {
        return;
    }
    s_wake_disabled_by_us = false;
    // 仅语音 UI 会话内才恢复唤醒词。
    if (Application::GetInstance().IsVoiceUiActive()) {
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
    }
}

void FormatTimer(char *buf, size_t buf_size, int total_sec) {
    if (total_sec < 0) {
        total_sec = 0;
    }
    const int mm = total_sec / 60;
    const int ss = total_sec % 60;
    std::snprintf(buf, buf_size, "%02d:%02d", mm, ss);
}

void FormatFileSize(char *buf, size_t buf_size, size_t bytes) {
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, buf_size, I18n::T("%.2f MB"), bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        std::snprintf(buf, buf_size, I18n::T("%.1f KB"), bytes / 1024.0);
    } else {
        std::snprintf(buf, buf_size, I18n::T("%u B"), static_cast<unsigned>(bytes));
    }
}

void SetRecordStatusText(const char *text) {
    if (!s_screen_alive || text == nullptr || s_ui.status_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.status_lbl, text);
}

void SetIdleRecordHint() {
    char buf[96];
    std::snprintf(buf, sizeof(buf), I18n::T("点击下方按钮开始录音（最长 %d 分钟）"),
                  kMaxRecordSeconds / 60);
    SetRecordStatusText(buf);
}

void SetRecordingStatusHint() {
    char buf[96];
    std::snprintf(buf, sizeof(buf), I18n::T("正在录音…（最长 %d 分钟）"),
                  kMaxRecordSeconds / 60);
    SetRecordStatusText(buf);
}

void SetListStatusText(const char *text) {
    if (!s_screen_alive || text == nullptr || s_ui.list_status == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.list_status, text);
}

void SetDetailStatusText(const char *text) {
    if (!s_screen_alive || text == nullptr || s_ui.detail_status == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.detail_status, text);
}

void SetDetailResultText(const char *text) {
    if (!s_screen_alive || text == nullptr || s_ui.detail_result == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.detail_result, text);
}

void PostPlayStatusFromWorker(const char *text) {
    if(!text)return;
    std::lock_guard<std::mutex> lock(s_updates.lock);
    s_updates.play_status=text;s_updates.play_generation=s_play_generation;
}

void PostDetailStatusFromWorker(const char *text) {
    if (text == nullptr) {
        return;
    }
    auto *copy = static_cast<char *>(HeapMalloc(std::strlen(text) + 1));
    if (copy == nullptr) {
        return;
    }
    std::strcpy(copy, text);
    if (LvglLock() != ESP_OK) {
        HeapFree(copy);
        return;
    }
    if (s_screen_alive && s_detail_open && !s_stop_asr.load()) {
        SetDetailStatusText(copy);
    }
    LvglUnlock();
    HeapFree(copy);
}

void PostDetailResultFromWorker(const char *text) {
    if (text == nullptr) {
        return;
    }
    auto *copy = static_cast<char *>(HeapMalloc(std::strlen(text) + 1));
    if (copy == nullptr) {
        return;
    }
    std::strcpy(copy, text);
    if (LvglLock() != ESP_OK) {
        HeapFree(copy);
        return;
    }
    if (s_screen_alive && s_detail_open && !s_stop_asr.load()) {
        SetDetailResultText(copy);
    }
    LvglUnlock();
    HeapFree(copy);
}

void PostStatusFromWorker(bool to_list_tab, const char *text) {
    (void)to_list_tab;if(!text)return;
    std::lock_guard<std::mutex> lock(s_updates.lock);
    s_updates.record_status=text;s_updates.record_generation=s_record_generation;
}

void UpdateTimerLabel() {
    if (!s_screen_alive || s_ui.timer_lbl == nullptr) {
        return;
    }
    int sec = 0;
    if (s_state.load() == RecState::Recording) {
        // The timer follows captured samples, not delayed UI ticks.
        sec = static_cast<int>(s_recorded_frames.load() * kFrameDurationMs / 1000);
    }
    char buf[16];
    FormatTimer(buf, sizeof(buf), sec);
    if (std::strcmp(lv_label_get_text(s_ui.timer_lbl), buf) != 0)
        lv_label_set_text(s_ui.timer_lbl, buf);
}

void OnTickTimer(lv_timer_t * /*t*/) {
    if (s_state.load() == RecState::Recording) {
        UpdateTimerLabel();
    }
}

void StartTickTimer() {
    if (s_tick_timer != nullptr) {
        return;
    }
    s_tick_timer = lv_timer_create(OnTickTimer, 200, nullptr);
}

void StopTickTimer() {
    if (s_tick_timer != nullptr) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = nullptr;
    }
}

void UpdateRecordButtonUi() {
    if (!s_screen_alive || s_ui.record_btn == nullptr || s_ui.record_btn_lbl == nullptr) {
        return;
    }
    const RecState st = s_state.load();
    switch (st) {
    case RecState::Idle:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(kColorRecord), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("开始录音"));
        lv_obj_add_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case RecState::Recording:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(0x4B5563), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("结束录音"));
        lv_obj_add_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case RecState::Saving:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(0x4B5563), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("保存中…"));
        lv_obj_remove_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case RecState::Playing:
        lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(kColorRecord), LV_PART_MAIN);
        lv_label_set_text(s_ui.record_btn_lbl, I18n::T("开始录音"));
        lv_obj_add_flag(s_ui.record_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    }
}

void MakeNextRecordPath(char *path, size_t path_size) {
    // 设备序列：与配网 AP 相同取 MAC 末两字节 %02X%02X；MAC 来源统一 SystemInfo::GetMacAddress()
    char serial[8] = "0000";
    {
        const std::string mac = SystemInfo::GetMacAddress();
        unsigned b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
        if (std::sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &b0, &b1, &b2, &b3, &b4,
                        &b5) == 6) {
            std::snprintf(serial, sizeof(serial), "%02X%02X", b4 & 0xff, b5 & 0xff);
        }
    }

    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year > 70) {
        std::snprintf(path, path_size, "%s/REC_%s_%04d%02d%02d_%02d%02d%02d.opus",
                      kPosixDir, serial, tm_info.tm_year + 1900, tm_info.tm_mon + 1,
                      tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        return;
    }
    const unsigned long ts_ms =
        static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
    std::snprintf(path, path_size, "%s/REC_%s_%lu.opus", kPosixDir, serial, ts_ms);
}

// Fixed allocation ring: the microphone reader never encodes or touches FAT.
struct WaveWriter {
 FILE*file;std::mutex lock;std::vector<int16_t> ring;size_t read=0,write=0,count=0;bool done=false,failed=false;uint32_t written=0;
 explicit WaveWriter(FILE*f):file(f),ring(64000){}
 bool Push(const std::vector<int16_t>&pcm){std::lock_guard<std::mutex>l(lock);if(failed||pcm.size()>ring.size()-count)return false;for(auto x:pcm){ring[write]=x;write=(write+1)%ring.size();}count+=pcm.size();return true;}
 static void*Run(void*arg){auto*w=(WaveWriter*)arg;sched_param p{};p.sched_priority=CONFIG_METALIO_APP_PRIORITY+10;pthread_setschedparam(pthread_self(),SCHED_RR,&p);std::vector<int16_t>batch(3840);
  for(;;){size_t n;bool done;{std::lock_guard<std::mutex>l(w->lock);n=std::min(batch.size(),w->count);for(size_t i=0;i<n;++i){batch[i]=w->ring[w->read];w->read=(w->read+1)%w->ring.size();}w->count-=n;done=w->done;}
   if(n){size_t at=0;while(at<n){size_t got=fwrite(batch.data()+at,sizeof(int16_t),n-at,w->file);if(!got){std::lock_guard<std::mutex>l(w->lock);w->failed=true;return nullptr;}at+=got;}w->written+=n;}else if(done)break;else usleep(5000);
  }return nullptr;
 }
};
bool WaveHeader(FILE*f,uint32_t samples){uint8_t h[44]={};auto put=[&](int at,uint32_t v,int n){for(int i=0;i<n;++i)h[at+i]=(v>>(8*i))&255;};memcpy(h,"RIFF",4);put(4,36+samples*2,4);memcpy(h+8,"WAVEfmt ",8);put(16,16,4);put(20,1,2);put(22,1,2);put(24,16000,4);put(28,32000,4);put(32,2,2);put(34,16,2);memcpy(h+36,"data",4);put(40,samples*2,4);return fseek(f,0,SEEK_SET)==0&&fwrite(h,1,44,f)==44;}
bool VerifyRecordedWave(const char *path,uint32_t samples) {
    FILE *f=fopen(path,"rb");if(!f)return false;
    uint8_t h[44];struct stat st{};bool ok=fstat(fileno(f),&st)==0&&
        st.st_size==(off_t)(44+samples*2)&&fread(h,1,44,f)==44&&
        !memcmp(h,"RIFF",4)&&!memcmp(h+8,"WAVEfmt ",8)&&!memcmp(h+36,"data",4)&&
        ReadLe32(h+4)==36+samples*2&&ReadLe32(h+40)==samples*2;
    fclose(f);return ok;
}
// Quiesce previous local sessions before claiming a new one. A late radio or
// music worker must not replace the recording's session after prebuffering.
bool PrepareRecorderAudio(bool input) {
    // The modem may have missed the final idle URC. A read-only CLCC refresh
    // distinguishes a stale cached call from a real call without resetting RF.
    if(metalio_nt26_get_call_state()!=METALIO_CALL_IDLE)
        (void)metalio_nt26_call_poll();
    if(metalio_nt26_get_call_state()!=METALIO_CALL_IDLE)return false;
    auto &app=Application::GetInstance();auto &audio=app.GetAudioService();
    // The recorder worker owns the input lease BEFORE changing routes.
    // A globally re-armed wake detector must not switch A2DP back to the
    // microphone route halfway through preparing a file playback session.
    if(!audio.HasExclusiveCapture())return false;
    app.RequestVoiceUiDesired(false);
    audio.EnableWakeWordDetection(false);audio.EnableVoiceProcessing(false);
    audio.SetTtsUplinkGate(false);
    LocalMusicPlayer::Instance().Stop();RadioPlayer::Instance().Pause();MusicScreen::PauseForVoice();
    for(int i=0;i<300&&!audio.LocalPlaybackSettled();++i){
        if(input?s_stop_record.load():s_stop_play.load())return false;
        usleep(10000);
    }
    if(!audio.LocalPlaybackSettled())return false;
    // Retire old cloud PCM and wait for an already executing DMA write.
    // ResetDecoder preserves an owned local session; no I2S stop/reconfigure.
    audio.ResetDecoder();
    for(int i=0;i<300&&!audio.VoicePlaybackSettled();++i){
        if(input?s_stop_record.load():s_stop_play.load())return false;
        usleep(10000);
    }
    if(!audio.VoicePlaybackSettled())return false;
    int route=input?BluetoothScreen::PrepareVoiceRoute():BluetoothScreen::PreparePlaybackRoute();
    if(route<0||metalio_nt26_get_call_state()!=METALIO_CALL_IDLE)return false;
    // Same idle/local source selection used by working Hi Music playback.
    // Never change this mux while the modem owns a real telephone call.
    if(metalio_audio_select_local()<0||
       metalio_nt26_call_control(3,BluetoothScreen::IsHeadsetConnected()?0:1)<0)return false;
    printf("REC_AUDIO_ROUTE direction=%s headset=%d sco=%d\n",input?"capture":"playback",
           BluetoothScreen::IsHeadsetConnected(),BluetoothScreen::IsHeadsetCaptureReady());
    return true;
}
void RecordTask(void*) {
    auto&as=Application::GetInstance().GetAudioService();char path[192]={};
    std::string pending;FILE*file=nullptr;bool ok=false,owned=false;
    uint32_t samples=0;const char*error="录音未开始";bool headset=false;
    printf("REC_CAPTURE_BEGIN generation=%lu\n",(unsigned long)s_record_generation);
    do {
        if(!as.BeginExclusiveCapture()){error="麦克风忙，请重试";break;}
        owned=true;DisableWakeWordIfNeeded();
        if(!PrepareRecorderAudio(true)){error="录音音频通路未就绪，请重试";break;}
        headset=BluetoothScreen::IsHeadsetCaptureReady();
        if(s_stop_record)break;
        if(as.GetCodec()){as.GetCodec()->EnableInput(true);as.GetCodec()->SetRecordingInput(true);}
        // Settle only before the recording starts. Never discard words after
        // the sample clock/UI has started, and never reset shared I2S/DMA.
        bool warmed=true;{std::vector<int16_t>settle;for(int i=0;i<2&&!s_stop_record;++i)
            if(!as.ReadAudioData(settle,16000,320)){warmed=false;break;}}
        if(!warmed){error="麦克风未就绪，请重试";break;}
        if(!EnsureRecordingsDir()){error="无法创建录音目录";break;}
        MakeNextRecordPath(path,sizeof(path));std::string base(path);base.resize(base.rfind('.'));
        for(unsigned suffix=0;suffix<1000;++suffix){
            std::string candidate=base+(suffix?"_"+std::to_string(suffix):"")+".wav";
            if(access(candidate.c_str(),F_OK)==0)continue;
            snprintf(path,sizeof(path),"%s",candidate.c_str());pending=candidate+".partial";
            int fd=open(pending.c_str(),O_WRONLY|O_CREAT|O_EXCL,0644);
            if(fd>=0){file=fdopen(fd,"wb");if(!file){close(fd);unlink(pending.c_str());}break;}
            if(errno!=EEXIST)break;
        }
        if(!file){error="写入失败，未保存";break;}
        setvbuf(file,nullptr,_IOFBF,16384);
        if(!WaveHeader(file,0)){error="无法写入录音头";break;}
        WaveWriter writer(file);pthread_attr_t a;pthread_attr_init(&a);
        pthread_attr_setstacksize(&a,16384);pthread_t task;
        int rc=hi_task_create_legacy(&task,&a,WaveWriter::Run,&writer);pthread_attr_destroy(&a);
        if(rc){error="录音存储任务启动失败";break;}
        sched_param priority{};priority.sched_priority=CONFIG_METALIO_APP_PRIORITY+12;
        pthread_setschedparam(pthread_self(),SCHED_RR,&priority);
        s_record_start_us=esp_timer_get_time();s_recorded_frames=0;
        RecordingPcmFilter filter;std::vector<int16_t>pcm;pcm.reserve(640);
        int64_t last=esp_timer_get_time();bool capture_ok=true;
        PostStatusFromWorker(false,"正在录音…");
        while(!s_stop_record&&samples<(uint32_t)kMaxRecordSeconds*16000){
            if(metalio_nt26_get_call_state()!=METALIO_CALL_IDLE){error="来电已停止录音并保存";capture_ok=false;break;}
            if(headset&&!BluetoothScreen::IsHeadsetCaptureReady()){error="耳机已断开，已保存录音";capture_ok=false;break;}
            if(!as.ReadAudioData(pcm,16000,320)){
                if(esp_timer_get_time()-last>3000000){error="麦克风读取中断";capture_ok=false;break;}
                usleep(2000);continue;
            }
            // Stop belongs to this capture session. Do not enqueue a final
            // in-flight microphone block after the user has ended it.
            if(s_stop_record.load())break;
            last=esp_timer_get_time();
            if(pcm.size()==640)KeepLeftChannelOnly(pcm);
            if(pcm.size()!=320){error="麦克风采样不完整";capture_ok=false;break;}
            filter.Apply(pcm);
            if(!writer.Push(pcm)){error="SD 卡写入速度不足，录音已停止";capture_ok=false;break;}
            samples+=pcm.size();s_recorded_frames=samples/960;
        }
        // Release the microphone before waiting for storage drain/sync.
        if(as.GetCodec())as.GetCodec()->SetRecordingInput(false);
        as.EndExclusiveCapture();owned=false;
        {std::lock_guard<std::mutex>l(writer.lock);writer.done=true;}
        pthread_join(task,nullptr);samples=writer.written;
        printf("REC_CAPTURE_DRAIN samples=%lu write_error=%d\n",(unsigned long)samples,writer.failed);
        if(writer.failed){error="SD 卡写入失败，已保留可恢复文件";capture_ok=false;}
        if(samples<4800){error="录音太短，再试一次";break;}
        bool complete=WaveHeader(file,samples)&&fflush(file)==0&&fsync(fileno(file))==0;
        bool closed=fclose(file)==0;file=nullptr;
        if(!complete||!closed||!VerifyRecordedWave(pending.c_str(),samples)){
            error="录音保存未完成，已保留可恢复文件";break;
        }
        if(rename(pending.c_str(),path)<0){error="录音重命名失败，已保留文件";break;}
        WriteDurationSidecar(path,samples/16000);ok=true;
        if(!capture_ok)printf("REC_CAPTURE_PARTIAL samples=%lu\n",(unsigned long)samples);
    }while(false);
    if(file){WaveHeader(file,samples);fflush(file);fsync(fileno(file));fclose(file);}
    if(!pending.empty()&&samples<4800)unlink(pending.c_str());
    if(owned){if(as.GetCodec())as.GetCodec()->SetRecordingInput(false);as.EndExclusiveCapture();}
    RestoreWakeWordIfNeeded();
    {std::lock_guard<std::mutex>lock(s_updates.lock);
        s_updates.record_status=ok?"已保存到 SD 卡（WAV 无损录音）":error;
        s_updates.record_generation=s_record_generation;s_updates.samples=samples;
        s_updates.record_ok=ok;s_updates.record_done=true;
    }
    printf("REC_CAPTURE_RELEASED saved=%d samples=%lu\n",ok,(unsigned long)samples);
}

void StartRecording() {
    if (!s_sd_ready) {
        SetRecordStatusText(I18n::T("请插入 SD 卡"));
        return;
    }
    if(s_play_task!=0||s_state.load()==RecState::Playing){
        s_record_after_play=true;StopPlayback();
        SetRecordStatusText("正在停止回放，随后开始录音…");return;
    }
    if(s_record_task!=0||s_state.load()!=RecState::Idle){
        SetRecordStatusText("录音正在收尾，请稍候…");return;
    }

    StopPlayback();

    auto &as = Application::GetInstance().GetAudioService();
    if (!as.IsStarted()) {
        SetRecordStatusText(I18n::T("音频未就绪"));
        return;
    }

    s_stop_record.store(false);s_stop_play.store(false);s_record_after_play=false;
    s_record_start_us=0;s_recorded_frames=0;s_playing_path[0]=0;
    {std::lock_guard<std::mutex>lock(s_updates.lock);
        s_updates.record_done=false;s_updates.record_status.clear();s_updates.record_ok=false;s_updates.samples=0;
        s_updates.play_done=false;s_updates.play_status.clear();
    }
    s_record_generation = s_screen_generation;
    s_state.store(RecState::Recording);
    if (s_ui.timer_lbl != nullptr) {
        lv_label_set_text(s_ui.timer_lbl, "00:00");
    }
    SetRecordStatusText("正在准备麦克风…");
    UpdateRecordButtonUi();
    StartTickTimer();

    // Opus/SILK 编码栈很深：AudioService opus_codec 用 2048*13，
    // wake_word encode 用 4096*7；16KB 会 Stack protection fault。
    constexpr uint32_t kRecordTaskStack = 2048 * 14;
    s_record_task=1;
    if (xTaskCreatePinnedToCore(RecordTask, "rec_save", kRecordTaskStack, nullptr,
                                tskIDLE_PRIORITY + 3, nullptr, 0) != pdPASS) {
        s_record_task = 0;
        s_state.store(RecState::Idle);
        StopTickTimer();
        UpdateRecordButtonUi();
        SetRecordStatusText(I18n::T("录音任务启动失败"));
    }
}

void ApplyWorkerUpdates(lv_timer_t*) {
    bool record_done,play_done,ok,files_ready;uint32_t rg,pg,fg;std::string record,play;std::vector<FileEntry>files;
    {std::lock_guard<std::mutex>lock(s_updates.lock);
        record_done=s_updates.record_done;play_done=s_updates.play_done;
        s_updates.record_done=s_updates.play_done=false;ok=s_updates.record_ok;
        rg=s_updates.record_generation;pg=s_updates.play_generation;
        if(record_done){s_updates.samples=0;s_updates.record_ok=false;}
        record.swap(s_updates.record_status);play.swap(s_updates.play_status);
        files_ready=s_updates.files_ready;s_updates.files_ready=false;fg=s_updates.files_generation;
        if(files_ready)files.swap(s_updates.files);
    }
    if(files_ready){
        s_scan_busy=false;
        if(s_screen_alive&&fg==s_screen_generation){s_files=std::move(files);RenderFileList(s_fill_durations);s_fill_durations=false;}
        if(s_refresh_again){s_refresh_again=false;RebuildFileList();}
    }
    if(record_done){
        // The worker publishes done only after PCM/filter/writer destruction,
        // file finalization and microphone release. Clear session state here,
        // never the shared audio hardware while that worker is still active.
        s_record_task=0;s_state=RecState::Idle;StopTickTimer();
        s_record_start_us=0;s_recorded_frames=0;s_stop_record=false;
        if(s_play_task==0){s_stop_play=false;s_playing_path[0]=0;}
        UpdateTimerLabel();
    }
    if(play_done){s_play_task=0;s_state=RecState::Idle;s_playing_path[0]=0;}
    if(s_screen_alive){
        if(rg==s_screen_generation&&!record.empty())SetRecordStatusText(record.c_str());
        if(pg==s_screen_generation&&!play.empty()){
            if(s_detail_open)SetDetailStatusText(play.c_str());else SetListStatusText(play.c_str());
        }
        if(record_done||play_done){UpdateRecordButtonUi();UpdateDetailPlayButton();}
        if(record_done&&rg==s_screen_generation&&ok){
            // Saved duration remains in the file/sidecar and list. The
            // recording panel is a fresh 00:00 session after Stop.
            RebuildFileList();
        }
        if(play_done&&s_record_after_play){s_record_after_play=false;StartRecording();}
    }else s_record_after_play=false;
}

void StopRecording() {
    if (s_state.load() != RecState::Recording) {
        return;
    }
    s_stop_record.store(true);
    s_state.store(RecState::Saving);
    StopTickTimer();
    if(s_ui.timer_lbl)lv_label_set_text(s_ui.timer_lbl,"00:00");
    UpdateRecordButtonUi();
    SetRecordStatusText(I18n::T("保存中…"));
}

void OnRecordBtnClicked(lv_event_t * /*e*/) {
    const RecState st = s_state.load();
    if (st == RecState::Idle || st == RecState::Playing) {
        StartRecording();
    } else if (st == RecState::Recording) {
        StopRecording();
    }
}

bool HasExt(const char *name, const char *ext4) {
    // ext4 like ".wav" / ".opus" (lowercase), case-insensitive match on last 4 or 5 chars
    if (name == nullptr || ext4 == nullptr) {
        return false;
    }
    const size_t len = std::strlen(name);
    const size_t elen = std::strlen(ext4);
    if (len < elen) {
        return false;
    }
    for (size_t i = 0; i < elen; ++i) {
        char a = name[len - elen + i];
        char b = ext4[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool IsRecordingFilename(const char *name) {
    return HasExt(name, ".opus") || HasExt(name, ".wav");
}

void MakeDurationSidecarPath(char *out, size_t out_size, const char *opus_path) {
    std::snprintf(out, out_size, "%s.dur", opus_path);
}

bool WriteDurationSidecar(const char *opus_path, int duration_sec) {
    if (opus_path == nullptr || duration_sec < 0) {
        return false;
    }
    char dur_path[192];
    MakeDurationSidecarPath(dur_path, sizeof(dur_path), opus_path);
    FILE *f = fopen(dur_path, "wb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "write dur sidecar failed: %s", dur_path);
        return false;
    }
    const int n = std::fprintf(f, "%d\n", duration_sec);
    fclose(f);
    return n > 0;
}

int ReadDurationSidecar(const char *opus_path) {
    if (opus_path == nullptr) {
        return -1;
    }
    char dur_path[192];
    MakeDurationSidecarPath(dur_path, sizeof(dur_path), opus_path);
    FILE *f = fopen(dur_path, "rb");
    if (f == nullptr) {
        return -1;
    }
    int sec = -1;
    if (fscanf(f, "%d", &sec) != 1 || sec < 0) {
        sec = -1;
    }
    fclose(f);
    return sec;
}

void UnlinkDurationSidecar(const char *opus_path) {
    if (opus_path == nullptr) {
        return;
    }
    char dur_path[192];
    MakeDurationSidecarPath(dur_path, sizeof(dur_path), opus_path);
    unlink(dur_path);
}

// 从文件尾读最后若干 KB，解析 Ogg 页 granule（避免整文件逐页扫描）。
int DurationSecFromOpusFileTail(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    const long file_size = ftell(f);
    if (file_size <= 27) {
        fclose(f);
        return 0;
    }

    constexpr long kTailMax = 64 * 1024;
    const long start = (file_size > kTailMax) ? (file_size - kTailMax) : 0;
    const size_t len = static_cast<size_t>(file_size - start);
    auto *buf = static_cast<uint8_t *>(HeapMalloc(len));
    if (buf == nullptr) {
        fclose(f);
        return 0;
    }
    if (fseek(f, start, SEEK_SET) != 0 || fread(buf, 1, len, f) != len) {
        HeapFree(buf);
        fclose(f);
        return 0;
    }
    fclose(f);

    int64_t last_granule = 0;
    for (size_t i = 0; i + 27 <= len; ++i) {
        if (std::memcmp(buf + i, "OggS", 4) != 0 || buf[i + 4] != 0) {
            continue;
        }
        const uint8_t nseg = buf[i + 26];
        if (nseg == 0 || i + 27 + nseg > len) {
            continue;
        }
        size_t body = 0;
        for (uint8_t s = 0; s < nseg; ++s) {
            body += buf[i + 27 + s];
        }
        if (i + 27 + nseg + body > len) {
            continue;  // 不完整页（缓冲区开头可能截断）
        }
        const int64_t granule = ReadLe64(buf + i + 6);
        if (granule > 0) {
            last_granule = granule;
        }
    }
    HeapFree(buf);
    if (last_granule <= 0) {
        return 0;
    }
    return static_cast<int>(last_granule / 48000);
}

int EnsureOpusDuration(FileEntry &entry) {
    if (entry.format != RecFormat::Opus) {
        return entry.duration_sec;
    }
    if (entry.duration_ready && entry.duration_sec >= 0) {
        return entry.duration_sec;
    }
    const int from_side = ReadDurationSidecar(entry.path);
    if (from_side >= 0) {
        entry.duration_sec = from_side;
        entry.duration_ready = true;
        return entry.duration_sec;
    }
    const int from_tail = DurationSecFromOpusFileTail(entry.path);
    entry.duration_sec = from_tail;
    entry.duration_ready = true;
    if (from_tail > 0) {
        WriteDurationSidecar(entry.path, from_tail);
    }
    return entry.duration_sec;
}

void DurationFillTask(void * /*arg*/) {
    std::vector<std::string> pending;
    pending.reserve(16);
    lv_lock();
    if (!s_stop_meta.load() && s_screen_alive) {
    for (const auto &e : s_files) {
        if (e.format == RecFormat::Opus && !e.duration_ready) {
            pending.emplace_back(e.path);
        }
    }
    }
    lv_unlock();

    for (const auto &path : pending) {
        if (s_stop_meta.load() || !s_screen_alive) {
            break;
        }
        const int from_side = ReadDurationSidecar(path.c_str());
        int sec = from_side;
        if (sec < 0) {
            sec = DurationSecFromOpusFileTail(path.c_str());
            if (sec > 0) {
                WriteDurationSidecar(path.c_str(), sec);
            } else if (sec == 0) {
                // 仍写入 0，避免反复扫损坏文件
                WriteDurationSidecar(path.c_str(), 0);
            }
        }
        if (LvglLock() == ESP_OK) {
            if (s_screen_alive && !s_stop_meta.load()) {
                for (auto &e : s_files) {
                    if (e.path == path) {
                        e.duration_sec = (sec >= 0) ? sec : 0;
                        e.duration_ready = true;
                        break;
                    }
                }
            }
            LvglUnlock();
        }
        vTaskDelayMs(10);
    }

    if (!s_stop_meta.load() && s_screen_alive) {
        if (LvglLock() == ESP_OK) {
            if (s_screen_alive && !s_stop_meta.load() && s_ui.list_scroll != nullptr && !s_detail_open) {
                // Preserve rows while a pointer is down. Re-collecting also
                // discarded duration_ready for damaged files and retried forever.
                for (size_t i = 0; i < s_files.size() &&
                     i < lv_obj_get_child_count(s_ui.list_scroll); ++i) {
                    auto *row = lv_obj_get_child(s_ui.list_scroll, i);
                    auto *hint = lv_obj_get_child(row, 1);
                    if (!hint || !lv_obj_check_type(hint, &lv_label_class)) continue;
                    char dur[16], size_buf[24], text[72];
                    FormatTimer(dur, sizeof(dur), s_files[i].duration_sec);
                    FormatFileSize(size_buf, sizeof(size_buf), s_files[i].size_bytes);
                    std::snprintf(text, sizeof(text), I18n::T("时长 %s · %s · 点击查看"), dur, size_buf);
                    lv_label_set_text(hint, text);
                }
            } else if (s_screen_alive && !s_stop_meta.load() && s_detail_open && s_detail_idx >= 0 &&
                       s_detail_idx < static_cast<int>(s_files.size())) {
                FileEntry &e = s_files[static_cast<size_t>(s_detail_idx)];
                EnsureOpusDuration(e);
                if (s_ui.detail_meta != nullptr) {
                    char dur[16];
                    char size_buf[24];
                    FormatTimer(dur, sizeof(dur), e.duration_sec);
                    FormatFileSize(size_buf, sizeof(size_buf), e.size_bytes);
                    char meta[80];
                    std::snprintf(meta, sizeof(meta), I18n::T("时长 %s · %s"), dur,
                                  size_buf);
                    lv_label_set_text(s_ui.detail_meta, meta);
                }
            }
            LvglUnlock();
        }
    }

    s_meta_task = 0;
    if (!s_stop_meta.load() && s_screen_alive) {
        ScheduleDurationFill();  // sidecar 写入失败时再补一轮
    }
    return;
}

void ScheduleDurationFill() {
    if (!s_screen_alive || s_meta_task != 0) {
        return;
    }
    bool need = false;
    for (const auto &e : s_files) {
        if (e.format == RecFormat::Opus && !e.duration_ready) {
            need = true;
            break;
        }
    }
    if (!need) {
        return;
    }
    s_stop_meta.store(false);
    if (xTaskCreatePinnedToCore(DurationFillTask, "rec_meta", 8 * 1024, nullptr,
                                tskIDLE_PRIORITY + 1, &s_meta_task, 0) != pdPASS) {
        s_meta_task = 0;
        ESP_LOGW(TAG, "duration fill task start failed");
    }
}

std::vector<FileEntry> CollectRecordings() {
    std::vector<FileEntry> files;
    if (!SdCardManager::GetInstance().IsMounted()) {
        return files;
    }
    DIR *dir = opendir(kPosixDir);
    if (dir == nullptr) {
        return files;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (ent->d_type == DT_DIR) {
            continue;
        }
        if (!IsRecordingFilename(ent->d_name)) {
            continue;
        }
        // d_name 最长 255，直接 snprintf 会触发 -Werror=format-truncation
        if (std::strlen(ent->d_name) >= sizeof(FileEntry{}.name)) {
            continue;
        }
        FileEntry entry = {};
        strlcpy(entry.name, ent->d_name, sizeof(entry.name));
        const int n = std::snprintf(entry.path, sizeof(entry.path), "%s/%s",
                                    kPosixDir, entry.name);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(entry.path)) {
            continue;
        }
        entry.format = HasExt(entry.name, ".opus") ? RecFormat::Opus : RecFormat::Wav;
        struct stat st;
        if (stat(entry.path, &st) == 0 && st.st_size > 0) {
            entry.modified = st.st_mtime;
            entry.size_bytes = static_cast<size_t>(st.st_size);
            if (entry.format == RecFormat::Opus) {
                // 进页只读 sidecar；缺失则留给后台 / 详情懒加载
                const int side = ReadDurationSidecar(entry.path);
                if (side >= 0) {
                    entry.duration_sec = side;
                    entry.duration_ready = true;
                } else {
                    entry.duration_sec = 0;
                    entry.duration_ready = false;
                }
            } else if (st.st_size > 44) {
                entry.duration_sec =
                    static_cast<int>((st.st_size - 44) / (kSampleRate * 2));
                if (entry.duration_sec < 0) {
                    entry.duration_sec = 0;
                }
                entry.duration_ready = true;
            }
        }
        // FAT directory order and device prefixes are not chronological.
        // Keep the newest 80 across the entire directory, including calls.
        auto newer=[](const FileEntry &a,const FileEntry &b) {
            if(a.modified!=b.modified)return a.modified>b.modified;
            return std::strcmp(a.name,b.name)>0;
        };
        files.insert(std::lower_bound(files.begin(),files.end(),entry,newer),entry);
        if(files.size()>static_cast<size_t>(kMaxListItems))files.pop_back();
    }
    closedir(dir);

    return files;
}

// Historical saved captures contain predominantly 65-110 Hz rumble. A
// recording-only 140 Hz Butterworth high-pass removes that low-frequency
// energy without boosting hiss or gating away quiet syllables. Q30 state
// spans every Opus packet/WAV chunk; the hardware PCM format is unchanged.
class RecordingPlaybackLevel {
public:
    void Apply(std::vector<int16_t> &pcm) {
        for(auto &sample:pcm) {
            const int64_t x=sample;
            int64_t y=(1032800534LL*x-2065601067LL*x1_+1032800534LL*x2_+
                2064039427LL*y1_-993420884LL*y2_)/(1LL<<30);
            y=std::max<int64_t>(-65536,std::min<int64_t>(65536,y));
            x2_=x1_;x1_=x;y2_=y1_;y1_=y;
            sample=static_cast<int16_t>(std::max<int64_t>(-30000,std::min<int64_t>(30000,y)));
        }
    }
private:
    int64_t x1_=0,x2_=0,y1_=0,y2_=0;
};

// File playback holds the capture/route lease and writes its own PCM directly.
// It does not wait in the shared cloud/music prebuffer queue. The previous
// output has drained before this object is constructed; other producers are
// rejected for this entire session, including amplifier lead-in and DMA tail.
class RecordingOutput {
public:
    explicit RecordingOutput(AudioService &audio) : audio_(audio), session_(audio.BeginLocalPlayback(5760,true,true)) {}
    ~RecordingOutput() {
        // Stop/error also leaves accepted PCM in the hardware DMA queue.
        // Retain the lease until that tail is silent before the next record,
        // route change or wake detector can start reading microphone frames.
        if(wrote_&&!drained_)vTaskDelayMs(500);
        audio_.EndLocalPlayback(session_);
    }
    bool Prepare() {
        if (!session_) { PostPlayStatusFromWorker("录音回放通路被占用，请稍后重试"); return false; }
        if (auto *codec=audio_.GetCodec()) codec->EnableOutput(true);
        // Let the I2S/BT amplifier settle on silence before submitting the
        // FIRST file sample. These samples are not read from or counted in
        // the recording, so neither its beginning nor its duration is lost.
        const int16_t silence[kSamplesPerFrame]{};
        for (int i=0;i<6;++i) if (!Push(silence,kSamplesPerFrame)) return false;
        return true;
    }
    bool Push(const int16_t *pcm, size_t count) {
        if(s_stop_play.load() || !audio_.HasExclusiveCapture() ||
           audio_.LocalPlaybackWriteFailed(session_) ||
           metalio_nt26_get_call_state()!=METALIO_CALL_IDLE)return false;
        auto *codec=audio_.GetCodec();
        if(!codec || !codec->OutputDataChecked(pcm,count)) {
            PostPlayStatusFromWorker(I18n::T("音频输出未就绪"));
            return false;
        }
        wrote_=true;
        return !s_stop_play.load();
    }
    bool Finish() {
        // Push the complete file through the lower-half DMA queue before
        // releasing the session. Padding is not part of the recording.
        const int16_t silence[kSamplesPerFrame]{};
        for(int i=0;i<8;++i)if(!Push(silence,kSamplesPerFrame))return false;
        audio_.FinishLocalPlayback(session_);
        for (int waited=0; !s_stop_play.load() && waited<400; ++waited) {
            if(audio_.LocalPlaybackWriteFailed(session_)||metalio_nt26_get_call_state()!=METALIO_CALL_IDLE)return false;
            if (audio_.LocalPlaybackDrained(session_)) {
                // Include the I2S lower-half's final queued DMA blocks.
                for (int i=0;i<24&&!s_stop_play.load();++i) vTaskDelayMs(20);
                drained_=!s_stop_play.load();
                return !s_stop_play.load();
            }
            vTaskDelayMs(20);
        }
        return false;
    }
private:
    AudioService &audio_;
    uint32_t session_;
    bool wrote_=false,drained_=false;
};

bool PlayOpusFile(FILE *file, AudioService &as) {
    RecordingOutput output(as);
    if (!output.Prepare()) return false;
    PostPlayStatusFromWorker(I18n::T("播放中…"));

    bool seen_head = false;
    bool seen_tags = false;
    unsigned queued_frames = 0;
    OpusDecoderWrapper recording_decoder(kSampleRate, 1, kFrameDurationMs);
    RecordingPlaybackLevel playback_level;
    std::vector<int16_t> decoded_pcm;
    const int64_t play_start_us = esp_timer_get_time();
    uint8_t hdr[27];
    std::vector<uint8_t> packet;
    bool truncated=false;
    bool decode_failed=false;
    while (!s_stop_play.load()) {
        const size_t header_size=fread(hdr,1,sizeof(hdr),file);
        if (!header_size) break;
        if (header_size!=sizeof(hdr)||std::memcmp(hdr,"OggS",4)||hdr[4]!=0) {truncated=true;break;}
        const size_t nseg=hdr[26];
        uint8_t segs[255];
        if (nseg&&fread(segs,1,nseg,file)!=nseg) {truncated=true;break;}
        size_t body_size=0;
        for (size_t i=0;i<nseg;++i) body_size+=segs[i];
        std::vector<uint8_t> body(body_size);
        if (body_size&&fread(body.data(),1,body_size,file)!=body_size) {truncated=true;break;}
        size_t offset=0;
        for (size_t i=0;i<nseg&&!s_stop_play.load();++i) {
            const size_t len=segs[i];
            if (packet.size()+len>65536) {truncated=true;break;}
            if (len) packet.insert(packet.end(),body.data()+offset,body.data()+offset+len);
            offset+=len;
            // A 255 lacing value continues the SAME packet, even over an
            // Ogg page boundary. Decoding each page fragment truncated files.
            if (len==255) continue;
            if (packet.empty()) continue;
            if (!seen_head) {
                if (packet.size()<19||std::memcmp(packet.data(),"OpusHead",8)) {truncated=true;break;}
                if (packet[9]!=1 || packet[18]!=0) {
                    PostPlayStatusFromWorker(I18n::T("不支持此录音格式"));
                    return false;
                }
                seen_head=true;packet.clear();continue;
            }
            if (!seen_tags) {
                seen_tags=true;
                if (packet.size()>=8&&!std::memcmp(packet.data(),"OpusTags",8)) {packet.clear();continue;}
                // Do not drop the first audio packet if tags are absent.
            }
            if (!recording_decoder.Decode(std::move(packet),decoded_pcm)) {decode_failed=true;break;}
            packet.clear();
            playback_level.Apply(decoded_pcm);
            if (!output.Push(decoded_pcm.data(),decoded_pcm.size())) return false;
            ++queued_frames;
        }
        if (truncated||decode_failed||(hdr[5]&0x04)) break;
    }

    const bool read_failed = ferror(file) != 0 || truncated || (!decode_failed&&!packet.empty());
    const bool drained = output.Finish();
    printf("REC_PLAY frames=%u elapsed_ms=%lld cancelled=%d read_error=%d\n",
           queued_frames, static_cast<long long>((esp_timer_get_time()-play_start_us)/1000),
           s_stop_play.load(), read_failed);
    if (s_stop_play.load()) return false;
    if (ferror(file)) {
        PostPlayStatusFromWorker(I18n::T("读取录音失败"));
        return false;
    }
    if (decode_failed) {
        PostPlayStatusFromWorker(I18n::T("录音解码失败"));
        return false;
    }
    if (read_failed || !seen_head || !seen_tags) {
        PostPlayStatusFromWorker(I18n::T("录音数据不完整"));
        return false;
    }
    if (queued_frames == 0) {
        PostPlayStatusFromWorker(I18n::T("录音中没有音频"));
        return false;
    }
    if (!drained) {
        PostPlayStatusFromWorker(I18n::T("音频输出未就绪"));
        return false;
    }
    return true;
}

// WAV recordings use the bounded 16 kHz PCM playback queue.
bool PlayWavFile(FILE *file, AudioService &as) {
    uint8_t hdr[12] = {};
    struct stat st{};
    if (fstat(fileno(file), &st)!=0 || fread(hdr,1,sizeof(hdr),file)!=sizeof(hdr) ||
        std::memcmp(hdr,"RIFF",4) || std::memcmp(hdr+8,"WAVE",4)) {
        PostPlayStatusFromWorker(I18n::T("读取录音失败"));
        return false;
    }
    // RIFF permits extended fmt, LIST/JUNK/fact chunks and odd-size padding.
    // A valid WAV does not necessarily put its data header at byte 36.
    const uint64_t riff_end=8ULL+ReadLe32(hdr+4);
    uint32_t remaining=0;
    long data_position=-1;
    bool have_format=false;
    bool supported=false;
    if (st.st_size<12 || riff_end<12 || riff_end>static_cast<uint64_t>(st.st_size)) {
        PostPlayStatusFromWorker(I18n::T("录音数据不完整"));
        return false;
    }
    for (unsigned chunk=0;chunk<128;++chunk) {
        const long position=ftell(file);
        if (position<0 || static_cast<uint64_t>(position)+8>riff_end) break;
        uint8_t chunk_header[8];
        if (fread(chunk_header,1,8,file)!=8) break;
        const uint32_t size=ReadLe32(chunk_header+4);
        const uint64_t payload=static_cast<uint64_t>(position)+8;
        const uint64_t next=payload+size+(size&1u);
        if (payload>0x7fffffffULL || payload+size>riff_end ||
            next>static_cast<uint64_t>(st.st_size)) break;
        if (!std::memcmp(chunk_header,"fmt ",4)) {
            uint8_t fmt[16];
            if (size<16 || fread(fmt,1,sizeof(fmt),file)!=sizeof(fmt)) break;
            have_format=true;
            supported=fmt[0]==1 && fmt[1]==0 && fmt[2]==1 && fmt[3]==0 &&
                ReadLe32(fmt+4)==kSampleRate && fmt[12]==2 && fmt[13]==0 &&
                fmt[14]==16 && fmt[15]==0;
        } else if (!std::memcmp(chunk_header,"data",4)) {
            data_position=static_cast<long>(payload);
            remaining=size;
        }
        if (have_format && data_position>=0) break;
        if (next>0x7fffffffULL || fseek(file,static_cast<long>(next),SEEK_SET)!=0) break;
    }
    if (!have_format || data_position<0 || (remaining&1u) ||
        fseek(file,data_position,SEEK_SET)!=0) {
        PostPlayStatusFromWorker(I18n::T("录音数据不完整"));
        return false;
    }
    if (!supported) {
        PostPlayStatusFromWorker(I18n::T("不支持此录音格式"));
        return false;
    }
    if (!remaining) {
        PostPlayStatusFromWorker(I18n::T("录音中没有音频"));
        return false;
    }
    PostPlayStatusFromWorker(I18n::T("播放中…"));

    std::vector<int16_t> pcm(static_cast<size_t>(kPlayChunkSamples));
    RecordingPlaybackLevel playback_level;
    bool ok=true;
    RecordingOutput output(as);
    if (!output.Prepare()) return false;
    while (ok&&remaining&&!s_stop_play.load()) {
        const size_t want = std::min<size_t>(remaining,pcm.size() * sizeof(int16_t));
        const size_t got = fread(pcm.data(), 1, want, file);
        if (got != want || (got&1u)) {
            ok=false;
            break;
        }
        const size_t samples = got / sizeof(int16_t);
        remaining-=samples*sizeof(int16_t);
        pcm.resize(samples);
        playback_level.Apply(pcm);

        if (!output.Push(pcm.data(),samples)) {ok=false;break;}
        pcm.resize(static_cast<size_t>(kPlayChunkSamples));
    }
    // WAV must wait for the SAME lower-half DMA tail as Opus. An empty
    // software queue does not mean the last ~480ms have reached the speaker.
    if (!output.Finish()) ok=false;
    if(!ok&&!s_stop_play.load())PostPlayStatusFromWorker(I18n::T("录音数据不完整或音频输出未就绪"));
    return ok&&!ferror(file);
}

void PlayTask(void *arg) {
    auto*path_copy=static_cast<char*>(arg);FILE*file=fopen(path_copy,"rb");
    HeapFree(path_copy);bool played_ok=false,owned=false;
    auto &as=Application::GetInstance().GetAudioService();
    sched_param priority{};priority.sched_priority=CONFIG_METALIO_APP_PRIORITY+12;
    pthread_setschedparam(pthread_self(),SCHED_RR,&priority);
    do{
        if(!file){PostPlayStatusFromWorker("无法打开录音文件");break;}
        // Hold the input lease through playback and its DMA tail. This
        // prevents wake/AI/notifications from consuming samples or changing
        // the shared Bluetooth/I2S route, without stopping either DMA engine.
        if(!as.BeginExclusiveCapture()){PostPlayStatusFromWorker("音频正在使用，请稍后播放");break;}
        owned=true;
        if(!PrepareRecorderAudio(false)){PostPlayStatusFromWorker("音频设备尚未就绪，请重试");break;}
        if(s_stop_play)break;
        std::vector<char>buffer(16384);setvbuf(file,buffer.data(),_IOFBF,buffer.size());
        uint8_t signature[12]{};size_t n=fread(signature,1,sizeof(signature),file);
        if(fseek(file,0,SEEK_SET)!=0||ferror(file))PostPlayStatusFromWorker("读取录音失败");
        else if(n>=4&&!memcmp(signature,"OggS",4))played_ok=PlayOpusFile(file,as);
        else if(n==12&&!memcmp(signature,"RIFF",4)&&!memcmp(signature+8,"WAVE",4))played_ok=PlayWavFile(file,as);
        else PostPlayStatusFromWorker("不支持此录音格式");
        // fclose must run before the caller-provided stdio buffer is freed.
        fclose(file);file=nullptr;
    }while(false);
    if(file)fclose(file);
    if(owned)as.EndExclusiveCapture();
    {std::lock_guard<std::mutex>lock(s_updates.lock);
        if(played_ok&&!s_stop_play)s_updates.play_status="播放结束";
        else if(s_stop_play)s_updates.play_status="已停止播放";
        s_updates.play_generation=s_play_generation;s_updates.play_done=true;
    }
    printf("REC_PLAY_RELEASED complete=%d cancelled=%d\n",played_ok,s_stop_play.load());
}

void StopPlayback() {
    if (s_play_task == 0 && s_state.load() != RecState::Playing) {
        return;
    }
    s_stop_play.store(true);
    // The worker observes cancellation after this UI callback releases LVGL.
    s_playing_path[0] = '\0';
    if (s_state.load() == RecState::Playing && s_play_task == 0) {
        s_state.store(RecState::Idle);
    }
    UpdateDetailPlayButton();
}

void StartPlayback(const char *path) {
    if (!s_sd_ready || path == nullptr || path[0] == '\0') {
        return;
    }
    auto set_status = [](const char *text) {
        if (s_detail_open) {
            SetDetailStatusText(text);
        } else {
            SetListStatusText(text);
        }
    };
    if (s_record_task != 0 || s_state.load() == RecState::Recording || s_state.load() == RecState::Saving) {
        set_status(I18n::T("请先结束录音"));
        return;
    }

    StopPlayback();
    if (s_play_task != 0) {
        set_status(I18n::T("请稍候"));
        return;
    }

    auto *path_copy = static_cast<char *>(HeapMalloc(std::strlen(path) + 1));
    if (path_copy == nullptr) {
        set_status(I18n::T("内存不足"));
        return;
    }
    std::strcpy(path_copy, path);
    std::snprintf(s_playing_path, sizeof(s_playing_path), "%s", path);

    s_play_generation=s_screen_generation;
    s_record_after_play=false;
    s_stop_play.store(false);
    {std::lock_guard<std::mutex>lock(s_updates.lock);s_updates.play_done=false;s_updates.play_status.clear();}
    s_state.store(RecState::Playing);
    UpdateRecordButtonUi();
    UpdateDetailPlayButton();

    s_play_task=1;
    if (xTaskCreatePinnedToCore(PlayTask, "rec_play", 24 * 1024, path_copy,
                                tskIDLE_PRIORITY + 2, nullptr, 0) != pdPASS) {
        HeapFree(path_copy);
        s_play_task = 0;
        s_playing_path[0] = '\0';
        s_state.store(RecState::Idle);
        UpdateRecordButtonUi();
        UpdateDetailPlayButton();
        set_status(I18n::T("播放任务启动失败"));
    }
}

int EventIndex(lv_event_t *e) {
    return static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
}

void UpdateDetailPlayButton() {
    if (!s_screen_alive || s_ui.detail_play_lbl == nullptr) {
        return;
    }
    const bool playing_this =
        s_state.load() == RecState::Playing && s_detail_path[0] != '\0' &&
        std::strcmp(s_playing_path, s_detail_path) == 0;
    lv_label_set_text(s_ui.detail_play_lbl,
                      playing_this ? I18n::T("停止播放") : I18n::T("播放"));
}

void OnDetailPlayClicked(lv_event_t * /*e*/) {
    if (s_detail_path[0] == '\0') {
        return;
    }
    if (s_state.load() == RecState::Playing &&
        std::strcmp(s_playing_path, s_detail_path) == 0) {
        StopPlayback();
        SetDetailStatusText(I18n::T("已停止播放"));
        UpdateDetailPlayButton();
        UpdateRecordButtonUi();
        return;
    }
    StartPlayback(s_detail_path);
    UpdateDetailPlayButton();
}

void AppendJsonStringArray(std::string &out, cJSON *arr, const char *title) {
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) <= 0) {
        return;
    }
    out += title;
    out += "\n";
    const int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; ++i) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsString(item) && item->valuestring != nullptr &&
            item->valuestring[0] != '\0') {
            out += item->valuestring;
            out += "\n";
        }
    }
    out += "\n";
}

std::string FormatCompletedAsrText(cJSON *data) {
    std::string out;
    cJSON *text = cJSON_GetObjectItemCaseSensitive(data, "asrText");
    if (cJSON_IsString(text) && text->valuestring != nullptr &&
        text->valuestring[0] != '\0') {
        out = text->valuestring;
    }

    cJSON *duration = cJSON_GetObjectItemCaseSensitive(data, "durationMs");
    if (cJSON_IsNumber(duration) && duration->valuedouble > 0) {
        char line[64];
        std::snprintf(line, sizeof(line), I18n::T("音频时长：%.1f 秒"),
                      duration->valuedouble / 1000.0);
        if (!out.empty()) {
            out += "\n\n";
        }
        out += line;
    }

    AppendJsonStringArray(out, cJSON_GetObjectItemCaseSensitive(data, "summary"),
                          I18n::T("摘要"));

    if (out.empty()) {
        out = I18n::T("转写成功，但没有可显示的内容");
    }
    return out;
}

void ApplyAsrRecordData(cJSON *data) {
    if (!cJSON_IsObject(data)) {
        return;
    }
    cJSON *status = cJSON_GetObjectItemCaseSensitive(data, "status");
    if (!cJSON_IsString(status) || status->valuestring == nullptr) {
        return;
    }
    const char *st = status->valuestring;
    if (std::strcmp(st, "transcribing") == 0) {
        PostDetailResultFromWorker(I18n::T("转写处理中，请稍后"));
        PostDetailStatusFromWorker(I18n::T("转写处理中，请稍后"));
        return;
    }
    if (std::strcmp(st, "completed") == 0) {
        const std::string formatted = FormatCompletedAsrText(data);
        PostDetailResultFromWorker(formatted.c_str());
        PostDetailStatusFromWorker(I18n::T("转写完成"));
        return;
    }
    if (std::strcmp(st, "failed") == 0) {
        cJSON *err = cJSON_GetObjectItemCaseSensitive(data, "errorMessage");
        if (cJSON_IsString(err) && err->valuestring != nullptr &&
            err->valuestring[0] != '\0') {
            PostDetailResultFromWorker(err->valuestring);
            PostDetailStatusFromWorker(I18n::T("转写失败"));
        } else {
            PostDetailStatusFromWorker(I18n::T("转写失败"));
        }
    }
    // 其它状态：静默忽略
}

void AsrQueryTask(void * /*arg*/) {
    char name_snapshot[96] = {};
    strlcpy(name_snapshot, s_detail_name, sizeof(name_snapshot));
    if (name_snapshot[0] == '\0') {
        s_asr_query_task = 0;
        return;
        return;
    }

    AsrQueryResponse res =
        GetRecordingHttp()->QueryAudioRecords(name_snapshot);

    if (s_stop_asr.load() || !s_detail_open ||
        std::strcmp(name_snapshot, s_detail_name) != 0) {
        s_asr_query_task = 0;
        return;
        return;
    }

    // 无记录 / 请求失败：不改动 UI（「没有就不管」）
    if (!res.ok || res.status != 200 || res.body.empty()) {
        s_asr_query_task = 0;
        return;
        return;
    }

    cJSON *root = cJSON_Parse(res.body.c_str());
    if (root == nullptr) {
        s_asr_query_task = 0;
        return;
        return;
    }
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsNumber(code) && code->valueint == 200 && cJSON_IsObject(data)) {
        ApplyAsrRecordData(data);
    }
    cJSON_Delete(root);

    const bool need_again =
        !s_stop_asr.load() && s_detail_open && s_detail_name[0] != '\0' &&
        std::strcmp(name_snapshot, s_detail_name) != 0;
    s_asr_query_task = 0;
    if (need_again) {
        ScheduleAsrQuery();
    }
    return;
}

void ScheduleAsrQuery() {
    if (!s_screen_alive || !s_detail_open || s_detail_name[0] == '\0') {
        return;
    }
    if (s_asr_query_task != 0 || s_asr_task != 0) {
        return;
    }
    // 查询结果需联网；WiFi 和 4G 均不可用时静默跳过（不弹窗，本地详情仍可用）。
    if (WifiRequired_ShouldBlock()) {
        return;
    }
    s_stop_asr.store(false);
    if (xTaskCreatePinnedToCore(AsrQueryTask, "rec_asr_q", 8 * 1024, nullptr,
                                tskIDLE_PRIORITY + 1, &s_asr_query_task,
                                0) != pdPASS) {
        s_asr_query_task = 0;
        ESP_LOGW(TAG, "asr query task start failed");
    }
}

void AsrUploadTask(void * /*arg*/) {
    FILE *file = fopen(s_detail_path, "rb");
    if (file == nullptr) {
        PostDetailStatusFromWorker(I18n::T("无法打开录音文件"));
        s_asr_task = 0;
        return;
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        PostDetailStatusFromWorker(I18n::T("读取录音失败"));
        s_asr_task = 0;
        return;
        return;
    }
    const long file_size = ftell(file);
    if (file_size <= 0 || file_size > 8 * 1024 * 1024) {
        fclose(file);
        PostDetailStatusFromWorker(I18n::T("录音文件过大或无效"));
        s_asr_task = 0;
        return;
        return;
    }
    fclose(file);
    if (s_stop_asr.load()) {
        s_asr_task = 0;
        return;
        return;
    }

    const std::string path_snapshot(s_detail_path);
    const std::string name_snapshot(s_detail_name[0] ? s_detail_name : "recording.opus");
    PostDetailStatusFromWorker(I18n::T("正在上传…"));
    AsrUploadResponse res = GetRecordingHttp()->UploadTranscribe(
        path_snapshot.c_str(), name_snapshot.c_str());

    if (s_stop_asr.load() || !s_detail_open || path_snapshot != s_detail_path) {
        s_asr_task = 0;
        return;
        return;
    }

    if (!res.ok || res.status != 200) {
        char err[64];
        std::snprintf(err, sizeof(err), I18n::T("转写失败（HTTP %d）"), res.status);
        PostDetailStatusFromWorker(err);
        s_asr_task = 0;
        return;
        return;
    }

    cJSON *root = cJSON_Parse(res.body.c_str());
    if (root == nullptr) {
        PostDetailStatusFromWorker(I18n::T("转写结果解析失败"));
        s_asr_task = 0;
        return;
        return;
    }

    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsNumber(code) || code->valueint != 200 || !cJSON_IsObject(data)) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(msg) && msg->valuestring != nullptr) {
            PostDetailStatusFromWorker(msg->valuestring);
        } else {
            PostDetailStatusFromWorker(I18n::T("转写失败"));
        }
        cJSON_Delete(root);
        s_asr_task = 0;
        return;
        return;
    }

    cJSON_Delete(root);
    PostDetailResultFromWorker(I18n::T("转写处理中，请稍后"));
    PostDetailStatusFromWorker(
        I18n::T("上传成功，请稍后来查看转写结果，勿反复上传"));
    s_asr_task = 0;
    return;
}

void OnDetailAsrClicked(lv_event_t * /*e*/) {
    if (s_detail_path[0] == '\0') {
        return;
    }
    if (s_asr_task != 0 || s_asr_query_task != 0) {
        SetDetailStatusText(I18n::T("正在上传…"));
        return;
    }
    if (s_record_task != 0 || s_state.load() == RecState::Recording || s_state.load() == RecState::Saving) {
        SetDetailStatusText(I18n::T("请先结束录音"));
        return;
    }
    // 转写需联网：WiFi 和 4G 均不可用时弹窗拦截（本地录音/播放不受影响）。
    if (WifiRequired_ShouldBlock()) {
        WifiRequired_ShowDialog("请先连接 WiFi 或 4G 后再使用转写");
        return;
    }

    s_stop_asr.store(false);
    SetDetailStatusText(I18n::T("正在上传…"));
    if (xTaskCreatePinnedToCore(AsrUploadTask, "rec_asr", 16 * 1024, nullptr,
                                tskIDLE_PRIORITY + 2, &s_asr_task, 0) != pdPASS) {
        s_asr_task = 0;
        SetDetailStatusText(I18n::T("转写任务启动失败"));
    }
}

void OnDetailBackClicked(lv_event_t * /*e*/) { HideDetail(); }

void HideDetail() {
    if (!s_detail_open) {
        return;
    }
    // Returning to the list is navigation, not a stop command. The playback
    // worker owns its file and stays alive until EOF or an explicit stop.
    s_stop_asr.store(true);
    s_detail_open = false;
    s_detail_idx = -1;
    s_detail_path[0] = '\0';
    s_detail_name[0] = '\0';
    if (s_ui.detail_panel != nullptr) {
        lv_obj_add_flag(s_ui.detail_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void ShowDetail(int idx) {
    if (idx < 0 || idx >= static_cast<int>(s_files.size()) ||
        s_ui.detail_panel == nullptr) {
        return;
    }
    FileEntry &entry = s_files[static_cast<size_t>(idx)];
    // 详情打开时同步补算一次（尾部扫描，通常很快）
    EnsureOpusDuration(entry);
    s_detail_idx = idx;
    s_detail_open = true;
    strlcpy(s_detail_path, entry.path, sizeof(s_detail_path));
    strlcpy(s_detail_name, entry.name, sizeof(s_detail_name));

    if (s_ui.detail_title != nullptr) {
        lv_label_set_text(s_ui.detail_title, entry.name);
    }
    if (s_ui.detail_meta != nullptr) {
        char dur[16];
        char size_buf[24];
        FormatTimer(dur, sizeof(dur), entry.duration_sec);
        FormatFileSize(size_buf, sizeof(size_buf), entry.size_bytes);
        char meta[80];
        std::snprintf(meta, sizeof(meta), I18n::T("时长 %s · %s"), dur, size_buf);
        lv_label_set_text(s_ui.detail_meta, meta);
    }
    SetDetailStatusText("");
    SetDetailResultText(I18n::T("点击「录音转写」上传，稍后再进详情查看结果"));
    UpdateDetailPlayButton();
    lv_obj_remove_flag(s_ui.detail_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.detail_panel);
    ScheduleAsrQuery();
}

void OnRowClicked(lv_event_t *e) {
    const int idx = EventIndex(e);
    if (idx < 0 || idx >= static_cast<int>(s_files.size())) {
        return;
    }
    ShowDetail(idx);
}

void OnDeleteClicked(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    const int idx = EventIndex(e);
    if (idx < 0 || idx >= static_cast<int>(s_files.size())) {
        return;
    }
    const FileEntry &entry = s_files[static_cast<size_t>(idx)];
    const char *path = entry.path;
    if (s_state.load() == RecState::Playing &&
        std::strcmp(s_playing_path, path) == 0) {
        StopPlayback();
    }
    if (unlink(path) != 0) {
        ESP_LOGE(TAG, "delete failed: %s", path);
        SetListStatusText(I18n::T("删除失败"));
        return;
    }
    if (entry.format == RecFormat::Opus) {
        UnlinkDurationSidecar(path);
    }
    ESP_LOGI(TAG, "deleted: %s", path);
    SetListStatusText(I18n::T("已删除"));
    RebuildFileList();
}

void ClearListChildren() {
    if (s_ui.list_scroll == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.list_scroll);
}

void RebuildFileList() { RebuildFileList(true); }

void *ScanRecordingFiles(void *arg){
    uint32_t generation=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    auto files=CollectRecordings();
    std::lock_guard<std::mutex>lock(s_updates.lock);
    s_updates.files=std::move(files);s_updates.files_generation=generation;s_updates.files_ready=true;
    return nullptr;
}
void RebuildFileList(bool schedule_fill){
    if(!s_screen_alive)return;
    s_fill_durations=s_fill_durations||schedule_fill;
    if(s_scan_busy){s_refresh_again=true;return;}
    s_scan_busy=true;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,16384);
    pthread_attr_setdetachstate(&a,PTHREAD_CREATE_DETACHED);pthread_t t;
    int rc=hi_task_create_legacy(&t,&a,ScanRecordingFiles,reinterpret_cast<void*>(static_cast<uintptr_t>(s_screen_generation)));
    pthread_attr_destroy(&a);
    if(rc){s_scan_busy=false;SetListStatusText("录音列表读取失败，请重试");}
}
void RenderFileList(bool schedule_fill) {
    if (!s_screen_alive || s_ui.list_scroll == nullptr) {
        return;
    }
    ClearListChildren();

    if (s_files.empty()) {
        if (s_ui.list_empty != nullptr) {
            lv_obj_remove_flag(s_ui.list_empty, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (s_ui.list_empty != nullptr) {
        lv_obj_add_flag(s_ui.list_empty, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < s_files.size(); ++i) {
        void *idx_ud = reinterpret_cast<void *>(static_cast<intptr_t>(i));

        lv_obj_t *row = lv_obj_create(s_ui.list_scroll);
        screen_strip_obj_chrome(row);
        lv_obj_set_size(row, lv_pct(100), 88);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(row, 8, LV_PART_MAIN);
        lv_obj_set_style_margin_bottom(row, 12, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, OnRowClicked, LV_EVENT_CLICKED, idx_ud);
        screen_swipe_back_ignore(row, true);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, s_files[i].name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 420);
        lv_obj_set_style_text_color(name, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, -12);

        lv_obj_t *hint = lv_label_create(row);
        char dur[16];
        char size_buf[24];
        if (s_files[i].duration_ready) {
            FormatTimer(dur, sizeof(dur), s_files[i].duration_sec);
        } else {
            std::snprintf(dur, sizeof(dur), "--:--");
        }
        FormatFileSize(size_buf, sizeof(size_buf), s_files[i].size_bytes);
        char hint_buf[72];
        std::snprintf(hint_buf, sizeof(hint_buf), I18n::T("时长 %s · %s · 点击查看"),
                      dur, size_buf);
        lv_label_set_text(hint, hint_buf);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
        lv_obj_set_width(hint, 420);
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_LEFT_MID, 0, 16);

        lv_obj_t *del = lv_button_create(row);
        lv_obj_remove_style_all(del);
        lv_obj_set_size(del, 96, 56);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(del, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del, lv_color_hex(kColorDanger), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(del, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del, lv_color_hex(0xB91C1C),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_add_event_cb(del, OnDeleteClicked, LV_EVENT_CLICKED, idx_ud);
        screen_swipe_back_ignore(del, true);

        lv_obj_t *del_lbl = lv_label_create(del);
        lv_label_set_text(del_lbl, I18n::T("删除"));
        lv_obj_set_style_text_color(del_lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(del_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(del_lbl);
    }

    if (schedule_fill) {
        ScheduleDurationFill();
    }
}

void OnTabChanged(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    if (s_ui.tabview == nullptr) {
        return;
    }
    const uint32_t idx = lv_tabview_get_tab_active(s_ui.tabview);
    if (idx == 1) {
        RebuildFileList();
    }
}

void ForceStopAll() {
    s_record_after_play=false;
    s_stop_record.store(true);
    s_stop_play.store(true);
    s_stop_asr.store(true);
    s_stop_meta.store(true);
    StopTickTimer();
    // Leave cancellation set until the corresponding worker has exited.
    s_playing_path[0] = '\0';
    s_detail_open = false;
    s_detail_idx = -1;
    s_detail_path[0] = '\0';
    s_detail_name[0] = '\0';
    // The capture worker still owns the microphone until it closes the
    // file and restores wake-word detection. Do not start a competing
    // reader while its final I2S request is being cancelled.
    if (s_record_task == 0 && s_play_task == 0) {
        s_state.store(RecState::Idle);
        RestoreWakeWordIfNeeded();
    }
}

void BuildDetailPanel(lv_obj_t *parent) {
    lv_obj_t *panel = lv_obj_create(parent);
    s_ui.detail_panel = panel;
    screen_strip_obj_chrome(panel);
    lv_obj_set_size(panel, kPanelSize, kPanelSize);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    screen_swipe_back_ignore(panel, true);

    lv_obj_t *header = lv_obj_create(panel);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnDetailBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    s_ui.detail_title = lv_label_create(header);
    lv_label_set_text(s_ui.detail_title, "");
    lv_label_set_long_mode(s_ui.detail_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.detail_title, kPanelSize - 16 - kBackBtnSize - 40);
    lv_obj_set_style_text_color(s_ui.detail_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(s_ui.detail_title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 8, 0);

    s_ui.detail_meta = lv_label_create(panel);
    lv_label_set_text(s_ui.detail_meta, "");
    lv_obj_set_style_text_color(s_ui.detail_meta, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_meta, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.detail_meta, LV_ALIGN_TOP_MID, 0, kHeaderH + 8);

    // 操作按钮行
    lv_obj_t *play = lv_button_create(panel);
    s_ui.detail_play_btn = play;
    lv_obj_remove_style_all(play);
    lv_obj_set_size(play, 300, 72);
    lv_obj_align(play, LV_ALIGN_TOP_LEFT, 40, kHeaderH + 48);
    lv_obj_set_style_radius(play, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(play, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(play, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(play, lv_color_hex(0x2563EB),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_add_event_cb(play, OnDetailPlayClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(play, true);

    s_ui.detail_play_lbl = lv_label_create(play);
    lv_label_set_text(s_ui.detail_play_lbl, I18n::T("播放"));
    lv_obj_set_style_text_color(s_ui.detail_play_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_play_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(s_ui.detail_play_lbl);

    lv_obj_t *asr = lv_button_create(panel);
    s_ui.detail_asr_btn = asr;
    lv_obj_remove_style_all(asr);
    lv_obj_set_size(asr, 300, 72);
    lv_obj_align(asr, LV_ALIGN_TOP_RIGHT, -40, kHeaderH + 48);
    lv_obj_set_style_radius(asr, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(asr, lv_color_hex(0x059669), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(asr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(asr, lv_color_hex(0x047857),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_add_event_cb(asr, OnDetailAsrClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(asr, true);

    s_ui.detail_asr_lbl = lv_label_create(asr);
    lv_label_set_text(s_ui.detail_asr_lbl, I18n::T("录音转写"));
    lv_obj_set_style_text_color(s_ui.detail_asr_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_asr_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(s_ui.detail_asr_lbl);

    s_ui.detail_status = lv_label_create(panel);
    lv_label_set_text(s_ui.detail_status, "");
    lv_obj_set_width(s_ui.detail_status, kPanelSize - 80);
    lv_label_set_long_mode(s_ui.detail_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_ui.detail_status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.detail_status, LV_ALIGN_TOP_MID, 0, kHeaderH + 136);
    screen_make_input_passive(s_ui.detail_status);

    lv_obj_t *result_box = lv_obj_create(panel);
    screen_strip_obj_chrome(result_box);
    lv_obj_set_size(result_box, kPanelSize - 48, kPanelSize - (kHeaderH + 180));
    lv_obj_align(result_box, LV_ALIGN_TOP_MID, 0, kHeaderH + 168);
    lv_obj_set_style_bg_color(result_box, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(result_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(result_box, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(result_box, 20, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(result_box, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(result_box, true);

    s_ui.detail_result = lv_label_create(result_box);
    lv_label_set_text(s_ui.detail_result, "");
    lv_obj_set_width(s_ui.detail_result, kPanelSize - 48 - 40);
    lv_label_set_long_mode(s_ui.detail_result, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_ui.detail_result, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.detail_result, &font_puhui_20_4, LV_PART_MAIN);
}

void BuildHeader(lv_obj_t *parent) {
    lv_obj_t *header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("录音"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);
}

void BuildNoSdHint(lv_obj_t *parent) {
    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, I18n::T("未检测到 SD 卡\n\n请插入 SD 卡后再使用录音功能"));
    lv_obj_set_width(hint, kPanelSize - 80);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 20);
    screen_make_input_passive(hint);
}

void BuildRecordTab(lv_obj_t *tab) {
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(tab);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 640, 220);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(card);

    s_ui.timer_lbl = lv_label_create(card);
    lv_label_set_text(s_ui.timer_lbl, "00:00");
    lv_obj_set_style_text_color(s_ui.timer_lbl, lv_color_hex(0xF87171), LV_PART_MAIN);
    // 用含冒号字形的字体；纯数字字库会把 "00:07" 渲成 "0007"
    lv_obj_set_style_text_font(s_ui.timer_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(s_ui.timer_lbl, LV_ALIGN_CENTER, 0, -16);

    s_ui.status_lbl = lv_label_create(card);
    SetIdleRecordHint();
    lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.status_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ui.status_lbl, 560);
    lv_obj_set_style_text_align(s_ui.status_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_ui.status_lbl, LV_ALIGN_BOTTOM_MID, 0, -24);

    s_ui.record_btn = lv_button_create(tab);
    lv_obj_remove_style_all(s_ui.record_btn);
    lv_obj_set_size(s_ui.record_btn, 360, 88);
    lv_obj_align(s_ui.record_btn, LV_ALIGN_TOP_MID, 0, 320);
    lv_obj_set_style_radius(s_ui.record_btn, 44, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(kColorRecord), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.record_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.record_btn, lv_color_hex(0xB91C1C),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_add_event_cb(s_ui.record_btn, OnRecordBtnClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(s_ui.record_btn, true);

    s_ui.record_btn_lbl = lv_label_create(s_ui.record_btn);
    lv_label_set_text(s_ui.record_btn_lbl, I18n::T("开始录音"));
    lv_obj_set_style_text_color(s_ui.record_btn_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.record_btn_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(s_ui.record_btn_lbl);

    lv_obj_t *tip = lv_label_create(tab);
    {
        char tip_buf[96];
        std::snprintf(tip_buf, sizeof(tip_buf),
                      I18n::T("单次最长 %d 分钟 · Opus：/sdcard/recordings"),
                      kMaxRecordSeconds / 60);
        lv_label_set_text(tip, tip_buf);
    }
    lv_obj_set_style_text_color(tip, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(tip, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, 440);
    screen_make_input_passive(tip);
}

void BuildListTab(lv_obj_t *tab) {
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.list_status = lv_label_create(tab);
    lv_label_set_text(s_ui.list_status, "");
    lv_obj_set_width(s_ui.list_status, kPanelSize - 48);
    lv_label_set_long_mode(s_ui.list_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ui.list_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.list_status, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.list_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.list_status, LV_ALIGN_BOTTOM_MID, 0, -12);
    screen_make_input_passive(s_ui.list_status);

    s_ui.list_empty = lv_label_create(tab);
    lv_label_set_text(s_ui.list_empty, I18n::T("暂无录音"));
    lv_obj_set_style_text_color(s_ui.list_empty, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.list_empty, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.list_empty, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(s_ui.list_empty);

    s_ui.list_scroll = lv_obj_create(tab);
    screen_strip_obj_chrome(s_ui.list_scroll);
    lv_obj_set_size(s_ui.list_scroll, kPanelSize - 40, kBodyH - kTabBarH - 56);
    lv_obj_align(s_ui.list_scroll, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_opa(s_ui.list_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.list_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.list_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_ui.list_scroll, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_ui.list_scroll, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(s_ui.list_scroll, true);
}

void BuildTabView(lv_obj_t *parent) {
    lv_obj_t *tv = lv_tabview_create(parent);
    s_ui.tabview = tv;
    lv_obj_set_size(tv, kPanelSize, kBodyH);
    lv_obj_set_pos(tv, 0, kHeaderH);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, kTabBarH);

    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorTabBar), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorAccent),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText),
                                LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t *content = lv_tabview_get_content(tv);
    screen_swipe_back_ignore(content, true);
    lv_obj_add_event_cb(tv, OnTabChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *tab_rec = lv_tabview_add_tab(tv, I18n::T("录音"));
    BuildRecordTab(tab_rec);

    lv_obj_t *tab_list = lv_tabview_add_tab(tv, I18n::T("列表"));
    BuildListTab(tab_list);
}

void OnSwipeBack() {
    if (s_detail_open) {
        HideDetail();
        return;
    }
    if (s_state.load() == RecState::Recording) {
        StopRecording();
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    HomeScreen::SwitchToHome();
}

void OnBackClicked(lv_event_t * /*e*/) { OnSwipeBack(); }

void OnScreenUnloaded(lv_event_t *e) {
    if(lv_event_get_target_obj(e)!=s_ui.screen)return;
    ForceStopAll();
    s_screen_alive = false;
    s_detail_open = false;
    s_ui = {};
    s_files.clear();
}

void *PrepareRecordingStorage(void *arg) {
    const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    const bool ready = SdCardManager::GetInstance().EnsureWritable() && EnsureRecordingsDir();
    lv_lock();
    if (s_screen_alive && generation == s_screen_generation) {
        s_sd_ready = ready;
        if (ready) {
            RebuildFileList();
            SetRecordStatusText(I18n::T("点击开始录音"));
        } else {
            SetRecordStatusText(I18n::T("请插入 SD 卡"));
        }
    }
    lv_unlock();
    return nullptr;
}

}  // namespace

lv_obj_t *RecordingScreen::CreateStatic() {
    if(!s_worker_ui_timer)s_worker_ui_timer=lv_timer_create(ApplyWorkerUpdates,60,nullptr);
    s_ui = {};
    s_screen_alive = true;
    ++s_screen_generation;
    if (s_record_task == 0 && s_play_task == 0) s_state.store(RecState::Idle);
    s_sd_ready = false;

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);

    BuildTabView(scr);
    BuildDetailPanel(scr);
    SetRecordStatusText(I18n::T("正在读取 SD 卡…"));
    pthread_t storage_thread;
    pthread_attr_t storage_attr;
    pthread_attr_init(&storage_attr);
    pthread_attr_setstacksize(&storage_attr, 8192);
    pthread_attr_setdetachstate(&storage_attr, PTHREAD_CREATE_DETACHED);
    if (hi_task_create_legacy(&storage_thread, &storage_attr, PrepareRecordingStorage,
                       reinterpret_cast<void *>(static_cast<uintptr_t>(s_screen_generation))) != 0)
        SetRecordStatusText(I18n::T("SD 卡读取失败"));
    pthread_attr_destroy(&storage_attr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    return scr;
}

lv_obj_t *RecordingScreen::Create() {
    root_ = CreateStatic();
    return root_;
}

void RecordingScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: recording_screen sd=%d", s_sd_ready ? 1 : 0);
    } else {
        ESP_LOGI(TAG, "unload: recording_screen");
        ForceStopAll();
    }
}

bool RecordingScreen::IsCapturing() { return s_state.load()==RecState::Recording || s_state.load()==RecState::Saving; }
