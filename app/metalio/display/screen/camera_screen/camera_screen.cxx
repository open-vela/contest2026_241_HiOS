#include "hi_task_policy.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * CameraScreen — ported from MetalioClaw4
 * main/display/screen/camera_screen/camera_screen.cc.
 *
 * UI is ported 1:1 (camera_panel / gallery_panel / viewer_panel /
 * bottom_strip with capture + gallery buttons, gallery header with back
 * button, photo viewer with delete button, status label, freeze-on-
 * capture). All layout constants, colors, and event handlers are
 * preserved verbatim.
 *
 * Hardware dependencies sit behind two interfaces declared in the header:
 *
 *   - CameraDriver: live preview. The default is NuttxV4l2CameraDriver
 *     (/dev/video0, OV2710 RAW10 -> BGR888). There is no placeholder
 *     gradient; if the sensor fails to stream, the preview stays black
 *     and the status line reports init failure.
 *   - JpegEncoder: replaces image_to_jpeg. Default StubJpegEncoder
 *     returns false, matching the original's "编码失败，未保存" path.
 *
 * Other porting changes:
 *   - heap_caps_aligned_alloc -> memalign (NuttX libc provides it in stdlib.h).
 *   - esp_lv_adapter_lock/unlock -> direct calls. The V4L2 worker only
 *     calls lv_obj_invalidate() through a lv_async_call trampoline
 *     (OnFrameReadyAsync), so LVGL mutations happen on the LVGL thread.
 *   - xTaskCreatePinnedToCore -> freertos_shim provides it (delegates
 *     to xTaskCreate).
 *   - vTaskDelay(pdMS_TO_TICKS(ms)) -> vTaskDelayMs(ms) (shim).
 *   - HomeScreen::Create() -> HomeScreen::CreateStatic().
 */

#include "camera_screen.h"
#include "camera_v4l2_driver.h"
#include "camera_jpeg_encoder.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "esp_timer_shim.h"
#include "esp_err_shim.h"
#include "freertos_shim.h"

#ifndef tskIDLE_PRIORITY
#define tskIDLE_PRIORITY 0
#endif
#include "home_screen/home_screen.h"
#include "system_chrome/system_chrome.h"
#include "pwr_key_handler/pwr_key_handler.h"
#include "SdCardManager.hpp"
#include "application.h"
#include "device_state.h"
#include "board_shim.h"
#include "system_info.h"
#include "ota.h"

#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <malloc.h>
#include <memory>
#include <initializer_list>
#include <atomic>
#include <string>
#include <vector>
#include <deque>
#include <mutex>

#include "lvgl.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <pthread.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "CameraScreen";

// ---------- 屏幕分区 (preserved verbatim) ----------
// 720x720 屏幕被纵向切成两条：上 720x600 摄像头预览，下 720x120 按钮区。
constexpr int kPanelW       = 720;
constexpr int kPanelH       = 720;
constexpr int kCameraAreaW  = 720;
constexpr int kCameraAreaH  = 600;
constexpr int kButtonStripH = kPanelH - kCameraAreaH;     // 120
constexpr int kButtonStripY = kCameraAreaH;               // 600
constexpr int kGalleryHeaderH = 90;
constexpr int kPad = 16;

// 保留预览的完整分辨率保存，避免再次缩小造成细节丢失。
#ifndef CAMERA_JPEG_QUALITY
#define CAMERA_JPEG_QUALITY 85
#endif
constexpr int      kSaveW         = kCameraAreaW;
constexpr int      kSaveH         = kCameraAreaH;
constexpr uint8_t  kJpegQuality   = CAMERA_JPEG_QUALITY;
constexpr int      kGalleryThumbW   = 220;
constexpr int      kGalleryThumbH   = 165;
constexpr int      kMaxGalleryItems = 48;
constexpr size_t   kMaxNameLen      = 256;
constexpr const char* kPosixSdRoot  = "/sdcard/DCIM/Camera";
constexpr const char* kLegacyPhotoRoot = "/sdcard";
constexpr const char* kGalleryIconPath = "A:ic_s_camera_picture.spng";

enum class ViewMode {
    kCamera,
    kGallery,
    kViewer,
};

// Worker control (preserved semantics; only the body of the worker task
// differs — it now drives the configured CameraDriver instead of V4L2).
std::atomic<bool>   s_camera_running{false};
std::atomic<unsigned> s_camera_generation{0};
pthread_mutex_t s_camera_state_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t s_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
TaskHandle_t        s_camera_task_handle = 0;

// Render each valid frame; the pending-frame slot coalesces UI updates.
// Pace conversion to the LVGL consumer instead of dropping every other frame.
// Camera and Hi Vision use the same sensor, full-size frames and UI cadence.
constexpr int kPreviewFrameInterval = 1;
constexpr int kPreviewRefreshMs = 16;

// camera_out_buf 与 LVGL canvas 绑定 —— 只分配一次、永不释放，避免重复进入摄像头屏幕时
// 反复申请 1.3 MB 的 PSRAM。摄像头采集任务直接覆写这个缓冲。
uint8_t* s_canvas_buf = nullptr;
uint8_t* s_capture_buf = nullptr;
uint8_t* s_pending_buf = nullptr;
unsigned s_pending_generation = 0;
constexpr size_t kPreviewBytes = static_cast<size_t>(kCameraAreaW) * kCameraAreaH * 3;
lv_obj_t* s_external_canvas = nullptr;
std::atomic<CameraScreen::PreviewObserver> s_preview_observer{nullptr};

// 拍照冻结标志：true 时摄像头任务暂停刷新画面（屏幕显示最后一帧 = 照片）
std::atomic<bool> s_photo_frozen{false};

// True while a save_photo_task is running. Prevents the capture button
// from kicking off concurrent saves.
std::atomic<bool> s_save_in_progress{false};
std::atomic<bool> s_look_and_see_mode{false};
enum class VisionReturn : uint8_t { Home, Chat, DigitalPeople };
std::atomic<VisionReturn> s_vision_return{VisionReturn::Home};
pthread_mutex_t s_vision_config_mutex = PTHREAD_MUTEX_INITIALIZER;
std::string s_vision_url;
std::string s_vision_token;
lv_timer_t* s_look_and_see_listen_timer = nullptr;
bool s_look_and_see_owns_voice = false;
bool s_look_and_see_return_to_camera = false;
std::atomic<unsigned> s_vision_visit{0}, s_snapshot_jobs{0};
pthread_mutex_t s_snapshot_write_mutex = PTHREAD_MUTEX_INITIALIZER;
// Bound encoder contention; SD and HTTP never run under this mutex.
pthread_mutex_t s_photo_encode_mutex = PTHREAD_MUTEX_INITIALIZER;

lv_timer_t* s_status_clear_timer = nullptr;
lv_timer_t* s_frame_timer = nullptr;
lv_timer_t* s_gallery_build_timer = nullptr;
lv_timer_t* s_gallery_visible_timer = nullptr;
std::atomic<unsigned> s_gallery_generation{0};
char s_viewer_posix_path[kMaxNameLen + 16] = {};

// Pluggable drivers. Camera binds NuttxV4l2CameraDriver on first use.
// JPEG falls back to StubJpegEncoder until an encoder is installed.
CameraScreen::CameraDriver  *s_driver  = nullptr;
CameraScreen::JpegEncoder   *s_encoder = nullptr;

// Forward decls.
void RebuildGallery();
void SetViewMode(ViewMode mode);
void on_tab_back_clicked(lv_event_t* e);
esp_err_t start_camera_worker();
void stop_camera_worker();
bool QueueVisionSnapshot(uint8_t *jpeg, size_t size, unsigned generation);

// ---------------------------------------------------------------------------
// 全局 UI 状态（CameraScreen 是单例，一次只能存在一个实例）
// ---------------------------------------------------------------------------
struct UiState {
    lv_obj_t* screen         = nullptr;
    lv_obj_t* camera_panel   = nullptr;
    lv_obj_t* canvas         = nullptr;
    lv_obj_t* gallery_panel   = nullptr;
    lv_obj_t* gallery_header  = nullptr;
    lv_obj_t* gallery_scroll  = nullptr;
    lv_obj_t* gallery_empty   = nullptr;
    lv_obj_t* btn_gallery_back = nullptr;
    lv_obj_t* viewer_panel    = nullptr;
    lv_obj_t* viewer_img      = nullptr;
    lv_obj_t* btn_viewer_del  = nullptr;
    lv_obj_t* bottom_strip    = nullptr;
    lv_obj_t* btn_capture     = nullptr;
    lv_obj_t* btn_label       = nullptr;
    lv_obj_t* btn_tab_gallery = nullptr;
    lv_obj_t* status_lbl      = nullptr;
};

UiState s_ui;
ViewMode s_view_mode = ViewMode::kCamera;

// Tracks whether the camera screen is currently mounted. The frame-ready
// trampoline (which can fire from the driver's worker thread) checks this
// before touching LVGL objects.
std::atomic<bool> s_screen_active{false};
std::atomic<bool> s_frame_async_pending{false};
std::atomic<int64_t> s_last_capture_us{0};

// Set on the LVGL thread once a real preview frame has been painted.
std::atomic<bool> s_got_preview_frame{false};

// ---------------------------------------------------------------------------
// LVGL UI 辅助：仅在 LVGL 线程持锁状态下调用。
// ---------------------------------------------------------------------------
void update_status_label(const char* text, lv_color_t color) {
    if (s_ui.status_lbl == nullptr) return;
    if (text == nullptr) {
        lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ui.status_lbl, text);
        lv_obj_set_style_text_color(s_ui.status_lbl, color, LV_PART_MAIN);
    }
}

// 摄像头任务可在任意线程调用 —— 内部用 lv_async_call 切回 LVGL 线程。
void post_status_text(const char* text, uint32_t color_hex, unsigned generation = 0) {
    // Pack the text + color into a small heap struct so the async
    // callback can read them on the LVGL thread.
    struct StatusMsg {
        char text[64];
        uint32_t color_hex;
        unsigned generation;
    };
    auto *msg = static_cast<StatusMsg *>(std::calloc(1, sizeof(StatusMsg)));
    if (msg == nullptr) {
        return;
    }
    std::snprintf(msg->text, sizeof(msg->text), "%s", text ? text : "");
    msg->color_hex = color_hex;
    msg->generation = generation;
    lv_lock();
    if (lv_async_call([](void *user_data) {
        auto *msg = static_cast<StatusMsg *>(user_data);
        if (msg == nullptr || !s_screen_active || s_ui.status_lbl == nullptr ||
            (msg->generation && msg->generation != s_camera_generation.load())) {
            std::free(msg);
            return;
        }
        update_status_label(msg->text, lv_color_hex(msg->color_hex));
        std::free(msg);
    }, msg) != LV_RESULT_OK) {
        std::free(msg);
    }
    lv_unlock();
}

void post_status_clear() {
    lv_lock();
    lv_async_call([](void * /*user_data*/) {
        if (!s_screen_active || s_ui.status_lbl == nullptr) return;
        update_status_label(nullptr, lv_color_white());
    }, nullptr);
    lv_unlock();
}

void schedule_status_clear(uint32_t delay_ms) {
    // May be called from a worker thread (save_photo_task) or from the
    // LVGL thread. lv_timer_create/delete mutate the timer list, so take
    // the LVGL lock to avoid racing the render thread.
    lv_lock();
    if (s_status_clear_timer != nullptr) {
        lv_timer_delete(s_status_clear_timer);
        s_status_clear_timer = nullptr;
    }
    s_status_clear_timer = lv_timer_create(
        [](lv_timer_t* t) {
            post_status_clear();
            lv_timer_delete(t);
            s_status_clear_timer = nullptr;
        },
        delay_ms, nullptr);
    if (s_status_clear_timer != nullptr) {
        lv_timer_set_repeat_count(s_status_clear_timer, 1);
    }
    lv_unlock();
}

// ---------------------------------------------------------------------------
// Frame-ready trampoline. The CameraDriver calls this (potentially from
// its own worker thread) after writing a new BGR888 frame into
// the capture buffer. A UI timer takes ownership of the latest complete
// frame under the frame mutex, then updates the canvas on the LVGL thread.
// ---------------------------------------------------------------------------
CameraScreen::CameraDriver *ActiveDriver();
void OnFrameReadyAsync(void *user_data) {
    const unsigned generation = reinterpret_cast<uintptr_t>(user_data);
    if (generation != s_camera_generation.load() || s_photo_frozen) return;
    lv_obj_t *canvas = s_external_canvas ? s_external_canvas : s_ui.canvas;
    if (!canvas) return;
    pthread_mutex_lock(&s_frame_mutex);
    const bool copied = generation == s_camera_generation.load() &&
        s_pending_generation == generation &&
        s_frame_async_pending.exchange(false, std::memory_order_acq_rel);
    if (copied)
        std::swap(s_canvas_buf, s_pending_buf);
    pthread_mutex_unlock(&s_frame_mutex);
    if (!copied) return;
    // Geometry/format are unchanged between preview frames. Rebinding the
    // whole canvas through lv_canvas_set_buffer also re-sets the image source
    // and its layout/cache twice per frame. Swap only the owned pixel pointer.
    auto *draw_buf = lv_canvas_get_draw_buf(canvas);
    if (draw_buf && draw_buf->header.w==kCameraAreaW &&
        draw_buf->header.h==kCameraAreaH && draw_buf->header.cf==LV_COLOR_FORMAT_RGB888) {
        lv_image_cache_drop(draw_buf);
        draw_buf->data=s_canvas_buf;
        draw_buf->unaligned_data=s_canvas_buf;
    } else {
        lv_canvas_set_buffer(canvas, s_canvas_buf, kCameraAreaW, kCameraAreaH, LV_COLOR_FORMAT_RGB888);
    }
    lv_obj_invalidate(canvas);
    if (s_screen_active && !s_got_preview_frame.exchange(true))
        update_status_label(nullptr, lv_color_white());
}
void OnFrameReady(void *user_data) {
    const unsigned generation = reinterpret_cast<uintptr_t>(user_data);
    if (generation != s_camera_generation.load()) return;
    s_last_capture_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    auto observer = s_preview_observer.load();
    if (observer) observer(s_capture_buf, kCameraAreaW, kCameraAreaH);
    if (s_photo_frozen) return;
    pthread_mutex_lock(&s_frame_mutex);
    // A gallery switch may have retired this stream after the first check.
    // Never rotate an old session's buffer into a restarted live preview.
    if (generation != s_camera_generation.load() || s_photo_frozen) {
        pthread_mutex_unlock(&s_frame_mutex);
        return;
    }
    // Three fixed buffers: capture, newest pending frame, and displayed frame.
    // Rotate ownership instead of copying 1.3 MB twice for every frame.
    if (ActiveDriver()->SetFrameBuffer(s_pending_buf))
        std::swap(s_capture_buf, s_pending_buf);
    else
        std::memcpy(s_pending_buf, s_capture_buf, kPreviewBytes);
    s_pending_generation = generation;
    s_frame_async_pending.store(true, std::memory_order_release);
    pthread_mutex_unlock(&s_frame_mutex);
}

// ---------------------------------------------------------------------------
// Stub JpegEncoder — always returns false. This matches the original
// "编码失败，未保存" status path. Replace with a real encoder when
// NuttX/openvela has one (e.g. via libjpeg-turbo or the ESP P4 JPEG
// codec once its driver is wired up).
// ---------------------------------------------------------------------------
class StubJpegEncoder : public CameraScreen::JpegEncoder
{
public:
    bool Encode(const uint8_t * /*bgr*/, size_t /*bgr_size*/,
                int /*w*/, int /*h*/, int /*quality*/,
                uint8_t ** /*out*/, size_t * /*out_len*/) override
    {
        ESP_LOGW(TAG, "StubJpegEncoder: no real encoder wired up");
        return false;
    }
};
StubJpegEncoder s_default_encoder;

CameraScreen::JpegEncoder *ActiveEncoder() {
    if (s_encoder == nullptr) {
        s_encoder = GetCameraJpegEncoder();
    }
    return s_encoder;
}

CameraScreen::CameraDriver *ActiveDriver() {
    // Bind on first use. The V4L2 driver's static registrar may not run
    // (C++ init array), which previously left s_driver null and fell
    // through to the placeholder gradient.
    if (s_driver == nullptr) {
        s_driver = GetNuttxV4l2CameraDriver();
        ESP_LOGI(TAG, "camera driver: /dev/video0");
    }
    return s_driver;
}

// ---------------------------------------------------------------------------
// SD card / gallery helpers (preserved verbatim — pure POSIX)
// ---------------------------------------------------------------------------
bool IsJpgFilename(const char* name) {
    if (name == nullptr) {
        return false;
    }
    size_t len = std::strlen(name);
    if (len < 5) {
        return false;
    }
    return strcasecmp(name + len - 4, ".jpg") == 0;
}

void DownscaleCanvas(uint8_t* dst, int dst_w, int dst_h) {
    if (s_canvas_buf == nullptr || dst == nullptr) {
        return;
    }
    pthread_mutex_lock(&s_frame_mutex);
    if (dst_w == kCameraAreaW && dst_h == kCameraAreaH) {
        std::memcpy(dst, s_canvas_buf, kPreviewBytes);
        pthread_mutex_unlock(&s_frame_mutex);
        return;
    }
    for (int y = 0; y < dst_h; y++) {
        const int src_y = y * kCameraAreaH / dst_h;
        for (int x = 0; x < dst_w; x++) {
            const int src_x = x * kCameraAreaW / dst_w;
            const uint8_t* src_px = s_canvas_buf + (src_y * kCameraAreaW + src_x) * 3;
            uint8_t* dst_px = dst + (y * dst_w + x) * 3;
            // 保持 B-G-R 字节序（与预览 canvas / P4 硬件 JPEG 输入一致）。
            dst_px[0] = src_px[0];
            dst_px[1] = src_px[1];
            dst_px[2] = src_px[2];
        }
    }
    pthread_mutex_unlock(&s_frame_mutex);
}

// All still-image paths use the manual camera's geometry and JPEG quality.
// Keep the copied pixels immutable until encoding completes. Allocation
// failure is recoverable and cannot throw from a background camera worker.
bool VisionVisitActive(unsigned visit) {
    return s_look_and_see_mode.load(std::memory_order_acquire) &&
        s_vision_visit.load(std::memory_order_acquire) == visit;
}

bool HasVisionFrame() {
    pthread_mutex_lock(&s_frame_mutex);
    const bool ready = s_screen_active.load() && s_camera_running.load() &&
        (s_got_preview_frame.load(std::memory_order_acquire) ||
         (s_pending_generation == s_camera_generation.load() &&
          s_frame_async_pending.load(std::memory_order_acquire)));
    pthread_mutex_unlock(&s_frame_mutex);
    return ready;
}

bool CapturePhotoJpeg(uint8_t **jpeg, size_t *jpeg_len, unsigned visit) {
    *jpeg = nullptr;
    *jpeg_len = 0;
    const int64_t lock_deadline = esp_timer_get_time() + 3000000;
    while (pthread_mutex_trylock(&s_photo_encode_mutex) != 0) {
        if (!VisionVisitActive(visit) || esp_timer_get_time() >= lock_deadline) return false;
        vTaskDelayMs(10);
    }
    struct EncodeUnlock { ~EncodeUnlock() {pthread_mutex_unlock(&s_photo_encode_mutex);} } unlock;
    if (!VisionVisitActive(visit) || !HasVisionFrame()) return false;
    const size_t bytes = static_cast<size_t>(kSaveW) * kSaveH * 3;
    auto *bgr = static_cast<uint8_t *>(std::malloc(bytes));
    if (!bgr) return false;
    // Snapshot the newest COMPLETE owned buffer, independent of whether the
    // LVGL timer has painted it yet. Never copy from the DMA/capture buffer.
    pthread_mutex_lock(&s_frame_mutex);
    const bool pending = s_pending_generation == s_camera_generation.load() &&
        s_frame_async_pending.load(std::memory_order_acquire);
    const bool valid = VisionVisitActive(visit) && s_screen_active.load() &&
        s_camera_running.load() && (pending || s_got_preview_frame.load());
    if (valid) std::memcpy(bgr, pending ? s_pending_buf : s_canvas_buf, bytes);
    pthread_mutex_unlock(&s_frame_mutex);
    if (!valid) { std::free(bgr); return false; }
    const bool ok = ActiveEncoder()->Encode(bgr, bytes, kSaveW, kSaveH,
                                            kJpegQuality, jpeg, jpeg_len);
    std::free(bgr);
    return ok;
}

bool EnsurePhotoDirectory() {
    if(!SdCardManager::GetInstance().EnsureWritable()) return false;
    for(const char *path:{"/sdcard/DCIM",kPosixSdRoot}) {
        if(mkdir(path,0775)<0 && errno!=EEXIST) return false;
        struct stat info{};
        if(stat(path,&info)<0 || !S_ISDIR(info.st_mode)) return false;
    }
    return true;
}

bool SaveLookAndSeeJpeg(const uint8_t* jpeg, size_t jpeg_len, char* out_path,
                        size_t out_path_size) {
    if (jpeg == nullptr || jpeg_len == 0 || out_path == nullptr ||
        out_path_size == 0 || !EnsurePhotoDirectory()) {
        return false;
    }

    const time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    char stamp[24] = {};
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);

    int fd = -1;
    char pending[112]{};
    for (unsigned suffix = 0; suffix < 10000; ++suffix) {
        if (suffix == 0) {
            std::snprintf(out_path, out_path_size, "%s/%s.jpg",
                          kPosixSdRoot, stamp);
        } else {
            std::snprintf(out_path, out_path_size, "%s/%s_%03u.jpg",
                          kPosixSdRoot, stamp, suffix);
        }
        if (access(out_path, F_OK) == 0) continue;
        std::snprintf(pending, sizeof(pending), "%s.partial", out_path);
        fd = open(pending, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return false;

    size_t written = 0;
    while (written < jpeg_len) {
        const ssize_t n = write(fd, jpeg + written, jpeg_len - written);
        if (n <= 0) break;
        written += static_cast<size_t>(n);
    }
    const bool flushed = fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    if (written != jpeg_len || !flushed || !closed || rename(pending, out_path) < 0) {
        unlink(pending);
        return false;
    }
    return true;
}

bool QueueVisionSnapshot(uint8_t *jpeg, size_t size, unsigned generation)
{
    // One exact uploaded JPEG per successful recognition, no second capture
    // or encode. Keep at most two bounded jobs, with serialized SD writes.
    if (s_snapshot_jobs.fetch_add(1) >= 2) {--s_snapshot_jobs;return false;}
    struct Snapshot {uint8_t *jpeg;size_t size;unsigned generation;};
    auto *snapshot=static_cast<Snapshot *>(std::malloc(sizeof(Snapshot)));
    if (!snapshot) {--s_snapshot_jobs;return false;}
    *snapshot={jpeg,size,generation};
    pthread_t worker;pthread_attr_t attr;pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr,32768);
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    sched_param priority{};priority.sched_priority=CONFIG_METALIO_APP_PRIORITY-3;
    pthread_attr_setschedparam(&attr,&priority);pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
    int rc=hi_task_create_legacy(&worker,&attr,[](void *arg)->void* {
        auto *snapshot=static_cast<Snapshot *>(arg);
        char path[96]{};
        pthread_mutex_lock(&s_snapshot_write_mutex);
        const bool saved=SaveLookAndSeeJpeg(snapshot->jpeg,snapshot->size,path,sizeof(path));
        pthread_mutex_unlock(&s_snapshot_write_mutex);
        std::free(snapshot->jpeg);
        printf("VISION_PHOTO saved=%d bytes=%u\n",saved,static_cast<unsigned>(snapshot->size));
        if (saved) {
            lv_lock();
            lv_async_call([](void *) {
                if (s_screen_active && s_view_mode==ViewMode::kGallery) RebuildGallery();
            },nullptr);
            lv_unlock();
        } else {
            post_status_text("识别完成，照片保存失败，请检查SD卡",0xFF8855,snapshot->generation);
        }
        std::free(snapshot);
        --s_snapshot_jobs;
        return nullptr;
    },snapshot);
    pthread_attr_destroy(&attr);
    if (rc!=0) {std::free(snapshot);--s_snapshot_jobs;return false;}
    return true; // JPEG ownership transferred only after successful creation.
}

void CollectRootJpgFiles(std::vector<std::string>& out) {
    out.clear();
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }

    std::vector<std::pair<time_t, std::string>> photos;
    // New photos live under DCIM/Camera. Legacy root photos remain visible
    // without moving or rewriting the user's existing files.
    for(const char *directory:{kPosixSdRoot,kLegacyPhotoRoot}) {
    DIR* dir=opendir(directory);
    if(!dir) continue;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue;
        }
        if (ent->d_type == DT_DIR) {
            continue;
        }
        if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) {
            continue;
        }
        if (!IsJpgFilename(ent->d_name)) {
            continue;
        }
        struct stat st = {};
        const std::string path = std::string(directory) + "/" + ent->d_name;
        if(stat(path.c_str(), &st)<0 || !S_ISREG(st.st_mode)) continue;
        photos.emplace_back(st.st_mtime, path);
    }
    closedir(dir);
    }

    std::sort(photos.begin(), photos.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    for (const auto& photo : photos) out.push_back(photo.second);
    if (out.size() > static_cast<size_t>(kMaxGalleryItems)) {
        out.resize(static_cast<size_t>(kMaxGalleryItems));
    }
}

// ---------------------------------------------------------------------------
// 拍照保存任务（preserved structure; JPEG encoding is delegated to the
// configured JpegEncoder, which by default is the stub that returns
// false -> "编码失败，未保存" status path, exactly matching the original
// behaviour when image_to_jpeg failed).
// ---------------------------------------------------------------------------
void save_photo_task(void* arg) {
    // JPEG/FAT work must yield to preview, display and microphone workers.
    sched_param priority{};priority.sched_priority=CONFIG_METALIO_APP_PRIORITY-3;
    (void)pthread_setschedparam(pthread_self(),SCHED_RR,&priority);
    // The UI took this snapshot at the shutter event; a later preview or
    // screen recreation cannot replace the photo while storage is busy.
    uint8_t* bgr = static_cast<uint8_t*>(arg);
    const time_t captured_at = time(nullptr);
    const size_t bgr_size = static_cast<size_t>(kSaveW) * kSaveH * 3;
    if (!EnsurePhotoDirectory()) {
        std::free(bgr);
        post_status_text(I18n::T("无 SD 卡，未保存"), 0xFFAA00);
        s_save_in_progress = false;
        return;
    }

    uint8_t* jpeg_data = nullptr;
    size_t jpeg_len = 0;
    bool ok = false;
    {
        pthread_mutex_lock(&s_photo_encode_mutex);
        ok = ActiveEncoder()->Encode(bgr, bgr_size, kSaveW, kSaveH,
                                     kJpegQuality, &jpeg_data, &jpeg_len);
        pthread_mutex_unlock(&s_photo_encode_mutex);
    }
    std::free(bgr);
    if (!ok || jpeg_data == nullptr || jpeg_len == 0) {
        if (jpeg_data != nullptr) {
            std::free(jpeg_data);
        }
        post_status_text(I18n::T("编码失败，未保存"), 0xFF5555);
        s_save_in_progress = false;
        vTaskDelete(0);
        return;
    }

    char path[96],pending[112];
    // FAT long filenames are enabled. Preserve existing photos on collisions.
    time_t now = captured_at;
    struct tm local = {};
    localtime_r(&now, &local);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
    int fd = -1;
    for (unsigned suffix = 0; suffix < 10000; ++suffix) {
        if (suffix == 0)
            std::snprintf(path, sizeof(path), "%s/%s.jpg", kPosixSdRoot, stamp);
        else
            std::snprintf(path, sizeof(path), "%s/%s_%03u.jpg", kPosixSdRoot, stamp, suffix);
        if(access(path,F_OK)==0)continue;
        snprintf(pending,sizeof(pending),"%s.partial",path);
        fd = open(pending, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0 || errno != EEXIST) break;
    }
    FILE* file = fd >= 0 ? fdopen(fd, "wb") : nullptr;
    if (fd >= 0 && file == nullptr) { close(fd); unlink(pending); }
    if (file == nullptr) {
        char errbuf[48];
        std::snprintf(errbuf, sizeof(errbuf), "CAM_FOPEN errno=%d\n", errno);
        write(1, errbuf, std::strlen(errbuf));
        std::free(jpeg_data);
        post_status_text(I18n::T("写入失败，未保存"), 0xFF5555);
        s_save_in_progress = false;
        vTaskDelete(0);
        return;
    }

    size_t written = 0;
    while (written < jpeg_len) {
        const size_t chunk = jpeg_len - written > 4096 ? 4096
                                                        : jpeg_len - written;
        const size_t n = fwrite(jpeg_data + written, 1, chunk, file);
        if (n == 0) {
            break;
        }
        written += n;
    }
    const bool flushed=fflush(file)==0&&fsync(fileno(file))==0;
    const bool closed = fclose(file) == 0;
    std::free(jpeg_data);

    if (written != jpeg_len || !closed || !flushed || rename(pending,path)<0) {
        unlink(pending);
        post_status_text(I18n::T("写入失败，未保存"), 0xFF5555);
    } else {
        ESP_LOGI(TAG, "saved photo %s (%u bytes)", path, static_cast<unsigned>(jpeg_len));
        post_status_text(I18n::T("已保存到 SD 卡"), 0x66FF66);
        schedule_status_clear(2500);
        lv_lock();
        lv_async_call([](void * /*user_data*/) {
            if (s_view_mode == ViewMode::kGallery) {
                RebuildGallery();
            }
        }, nullptr);
        lv_unlock();
    }
    s_save_in_progress = false;
    vTaskDelete(0);
}

void start_save_photo_task() {
    if (s_save_in_progress) {
        return;
    }
    auto* snapshot = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(kSaveW) * kSaveH * 3));
    if (!snapshot) {
        post_status_text(I18n::T("内存不足，保存失败"), 0xFF5555);
        return;
    }
    DownscaleCanvas(snapshot, kSaveW, kSaveH);
    s_save_in_progress = true;
    post_status_text(I18n::T("正在保存…"), 0xFFFFFF);
    if (xTaskCreateLegacy(save_photo_task, "cam_save", 32 * 1024, snapshot,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
        std::free(snapshot);
        s_save_in_progress = false;
        post_status_text(I18n::T("保存任务启动失败"), 0xFF5555);
    }
}

// ---------------------------------------------------------------------------
// Gallery view-mode helpers (preserved verbatim)
// ---------------------------------------------------------------------------
static inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

void RaiseGalleryHeader() {
    if (s_ui.gallery_header != nullptr) {
        lv_obj_move_foreground(s_ui.gallery_header);
    }
}

void BuildGalleryHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    s_ui.gallery_header = header;
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kGalleryHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back_btn = lv_button_create(header);
    s_ui.btn_gallery_back = back_btn;
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 72, 72);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, kPad + 8, 0);
    screen_swipe_back_ignore(back_btn, true);

    lv_obj_t* back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);
    lv_obj_add_event_cb(back_btn, on_tab_back_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("相册"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(title);
}

void ApplyGalleryLayout(bool gallery_active) {
    if (s_ui.gallery_panel == nullptr || s_ui.gallery_scroll == nullptr) {
        return;
    }
    if (gallery_active) {
        lv_obj_set_size(s_ui.gallery_panel, kPanelW, kPanelH);
        lv_obj_set_pos(s_ui.gallery_panel, 0, 0);
        lv_obj_set_size(s_ui.gallery_scroll, kPanelW, kPanelH - kGalleryHeaderH);
        lv_obj_set_pos(s_ui.gallery_scroll, 0, kGalleryHeaderH);
        if (s_ui.gallery_empty != nullptr) {
            lv_obj_align(s_ui.gallery_empty, LV_ALIGN_CENTER, 0, kGalleryHeaderH / 2);
        }
        if (s_ui.viewer_panel != nullptr) {
            lv_obj_set_size(s_ui.viewer_panel, kPanelW, kPanelH - kGalleryHeaderH);
            lv_obj_set_pos(s_ui.viewer_panel, 0, kGalleryHeaderH);
        }
    } else {
        lv_obj_set_size(s_ui.gallery_panel, kCameraAreaW, kCameraAreaH);
        lv_obj_set_pos(s_ui.gallery_panel, 0, 0);
        lv_obj_set_size(s_ui.gallery_scroll, kCameraAreaW, kCameraAreaH - kGalleryHeaderH);
        lv_obj_set_pos(s_ui.gallery_scroll, 0, kGalleryHeaderH);
        if (s_ui.viewer_panel != nullptr) {
            lv_obj_set_size(s_ui.viewer_panel, kCameraAreaW, kCameraAreaH - kGalleryHeaderH);
            lv_obj_set_pos(s_ui.viewer_panel, 0, kGalleryHeaderH);
        }
    }
}

// Immutable after publication. Cache and widgets share the same checked heap
// allocation instead of copying a second display-sized image under pressure.
struct PhotoPixels {
    std::shared_ptr<uint16_t> storage;
    size_t count=0;
    bool Allocate(size_t n){auto*p=static_cast<uint16_t*>(std::calloc(n,sizeof(uint16_t)));if(!p)return false;storage=std::shared_ptr<uint16_t>(p,[](uint16_t*v){std::free(v);});count=n;return true;}
    uint16_t*data(){return storage.get();}const uint16_t*data()const{return storage.get();}
    size_t size()const{return count;}bool empty()const{return !storage;}
    void Clear(){storage.reset();count=0;}
    uint16_t&operator[](size_t i){return storage.get()[i];}
};
struct ThumbCtx {
    char lv_path[kMaxNameLen + 16];
    char posix_path[kMaxNameLen + 16];
    std::atomic<bool> cancelled{false};
    std::atomic<bool> queued{false},failed{false};
    lv_obj_t *image = nullptr; // accessed only with the LVGL lock held
    lv_image_dsc_t descriptor{};
    PhotoPixels pixels;
    int target_width = kGalleryThumbW - 8;
    int target_height = kGalleryThumbH;
};

using ThumbRef = std::shared_ptr<ThumbCtx>;
ThumbRef s_viewer_job;
ThumbRef s_viewer_preview; // retain the visible thumbnail until full decode replaces it
std::vector<ThumbRef> s_gallery_refs; // Metadata only; pixels are viewport-bounded.
struct ThumbDecode {
    FILE *file;
    ThumbCtx *thumb;
    int width, height, x, y;
    struct Average { uint32_t r,g,b,n; };
    Average*average=nullptr;
    ~ThumbDecode(){std::free(average);}
    int average_rows=0,flushed_rows=0;
};

size_t ThumbRead(JDEC *decoder, uint8_t *bytes, size_t count) {
    auto *ctx = static_cast<ThumbDecode *>(decoder->device);
    if (ctx->thumb->cancelled.load()) return 0;
    if (bytes) return fread(bytes, 1, count, ctx->file);
    return fseek(ctx->file, static_cast<long>(count), SEEK_CUR) == 0 ? count : 0;
}

int ThumbWrite(JDEC *decoder, void *rgb, JRECT *rect) {
    auto *ctx = static_cast<ThumbDecode *>(decoder->device);
    if (ctx->thumb->cancelled.load()) return 0;
    const auto *source = static_cast<const uint8_t *>(rgb);
    const unsigned block_width = rect->right - rect->left + 1;
    const int stride = ctx->thumb->target_width;
    const unsigned source_w = std::max<unsigned>(1, decoder->width >> decoder->scale);
    const unsigned source_h = std::max<unsigned>(1, decoder->height >> decoder->scale);
    if(rect->left>rect->right || rect->top>rect->bottom ||
       rect->right>=source_w || rect->bottom>=source_h || ctx->average_rows<=0)
        return 0;
    for (unsigned y = rect->top; y <= rect->bottom; ++y) {
        const int dy0 = ctx->y + y * ctx->height / source_h;
        const int dy1 = ctx->y + ((y + 1) * ctx->height + source_h-1) / source_h;
        for (unsigned x = rect->left; x <= rect->right; ++x) {
            const int dx0 = ctx->x + x * ctx->width / source_w;
            const int dx1 = ctx->x + ((x + 1) * ctx->width + source_w-1) / source_w;
            const auto *p = source + ((y - rect->top) * block_width + x - rect->left) * 3;
            if (ctx->average) {
                // Fractional source-pixel coverage, rather than assigning
                // entire pixels to unequal integer bins. Smooth both edges
                // and detail when the source/target ratio is non-integral.
                for (int dy=dy0;dy<dy1;++dy) {
                    if(dy<ctx->flushed_rows || dy>=ctx->flushed_rows+ctx->average_rows ||
                       dy<0 || dy>=ctx->thumb->target_height) return 0;
                    const unsigned oy=dy-ctx->y;
                    const unsigned overlap_y=std::min<unsigned>((y+1)*ctx->height,(oy+1)*source_h)-
                        std::max<unsigned>(y*ctx->height,oy*source_h);
                    const unsigned wy=std::max(1u,(overlap_y*256+source_h/2)/source_h);
                    for (int dx=dx0;dx<dx1;++dx) {
                        if(dx<0 || dx>=stride) return 0;
                        const unsigned ox=dx-ctx->x;
                        const unsigned overlap_x=std::min<unsigned>((x+1)*ctx->width,(ox+1)*source_w)-
                            std::max<unsigned>(x*ctx->width,ox*source_w);
                        const unsigned wx=std::max(1u,(overlap_x*256+source_w/2)/source_w);
                        const unsigned weight=wx*wy;
                        auto &a=ctx->average[(dy%ctx->average_rows)*stride+dx];
                        a.r+=p[2]*weight;a.g+=p[1]*weight;a.b+=p[0]*weight;a.n+=weight;
                    }
                }
                continue;
            }
            const uint16_t color = ((p[2] & 0xf8) << 8) | ((p[1] & 0xfc) << 3) | (p[0] >> 3);
            for (int dy = dy0; dy < dy1; ++dy)
                for (int dx = dx0; dx < dx1; ++dx)
                    ctx->thumb->pixels[dy * stride + dx] = color;
        }
    }
    // MCUs arrive left-to-right. Once a full MCU row is complete, publish
    // its finished output rows and reuse their accumulators. Keep only a
    // small rolling band, never a width*height*16-byte full-photo matrix.
    if(rect->right+1==source_w) {
        const int complete=ctx->y+(rect->bottom+1)*ctx->height/source_h;
        while(ctx->flushed_rows<complete) {
            const int dy=ctx->flushed_rows++;
            for(int dx=0;dx<stride;++dx) {
                auto &a=ctx->average[(dy%ctx->average_rows)*stride+dx];
                if(a.n)ctx->thumb->pixels[dy*stride+dx]=
                    (((a.r/a.n)&0xf8)<<8)|(((a.g/a.n)&0xfc)<<3)|((a.b/a.n)>>3);
                a=ThumbDecode::Average{};
            }
        }
    }
    return 1;
}

struct CachedThumb { std::string path; off_t bytes; time_t modified; int width,height; PhotoPixels pixels; };
std::mutex s_thumb_cache_mutex;
std::deque<CachedThumb> s_thumb_cache; // At most 12 thumbnails and 2 display-sized photos.
void PublishThumbnail(const ThumbRef &thumb) {
        lv_lock();
        if (!thumb->cancelled.load() && s_screen_active && thumb->image) {
            auto &d = thumb->descriptor;
            d.header.magic = LV_IMAGE_HEADER_MAGIC;
            d.header.cf = LV_COLOR_FORMAT_RGB565;
            d.header.w = thumb->target_width; d.header.h = thumb->target_height; d.header.stride = thumb->target_width * 2;
            d.data_size = thumb->pixels.size() * sizeof(uint16_t);
            d.data = reinterpret_cast<const uint8_t *>(thumb->pixels.data());
            lv_image_set_src(thumb->image, &d);
            if (thumb == s_viewer_job && s_view_mode == ViewMode::kViewer) {
                // Publish pixels before exposing the overlay; never render an empty viewer.
                lv_obj_remove_flag(s_ui.viewer_panel, LV_OBJ_FLAG_HIDDEN);
                s_viewer_preview.reset();
            }
        }
        lv_unlock();
}

void *LoadGalleryThumbnails(void *arg) {
    std::unique_ptr<std::vector<ThumbRef>> jobs(static_cast<std::vector<ThumbRef> *>(arg));
    for (auto &thumb : *jobs) {
        struct JobDone {ThumbRef ref;~JobDone(){ref->queued=false;}} done{thumb};
        if (thumb->cancelled.load()) continue;
        struct stat signature{};
        const bool cacheable=stat(thumb->posix_path,&signature)==0;
        bool cached=false;
        if(cacheable){std::lock_guard<std::mutex> lock(s_thumb_cache_mutex);
            for(const auto &entry:s_thumb_cache)if(entry.path==thumb->posix_path &&
                entry.bytes==signature.st_size && entry.modified==signature.st_mtime &&
                entry.width==thumb->target_width && entry.height==thumb->target_height){
                thumb->pixels=entry.pixels;cached=true;break;
            }
        }
        if(cached){PublishThumbnail(thumb);continue;}
        FILE *file = fopen(thumb->posix_path, "rb");
        if (!file) {thumb->failed=true;continue;}
        setvbuf(file, nullptr, _IOFBF, 8192);
        struct Work{uint8_t*data=static_cast<uint8_t*>(std::malloc(16384));~Work(){std::free(data);}} work;
        if(!work.data){fclose(file);thumb->failed=true;continue;}
        JDEC decoder{};
        ThumbDecode ctx{file, thumb.get(), 0, 0, 0, 0};
        auto result = jd_prepare(&decoder, ThumbRead, work.data, 16384, &ctx);
        const int width = thumb->target_width, height = thumb->target_height;
        if (result == JDR_OK && decoder.width && decoder.height &&
            static_cast<uint64_t>(decoder.width) * decoder.height <= 24000000) {
            // This port's native scaled MCU path truncates coordinates
            // without resampling its pixel block. Decode at 1:1 here and
            // use our streamed area filter; do not change the shared decoder.
            uint8_t scale = 0;
            ctx.width = width;
            ctx.height = std::max(1, width * decoder.height / decoder.width);
            if (ctx.height > height) {
                ctx.height = height;
                ctx.width = std::max(1, height * decoder.width / decoder.height);
            }
            // Do not enlarge tiny imported images or grow the filter band.
            if(ctx.width>decoder.width || ctx.height>decoder.height) {
                ctx.width=decoder.width;ctx.height=decoder.height;
            }
            ctx.x = (width - ctx.width) / 2;
            ctx.y = (height - ctx.height) / 2;
            if(!thumb->pixels.Allocate(static_cast<size_t>(width)*height)){fclose(file);thumb->failed=true;continue;}
            ctx.average_rows=std::min(height,
                (16*ctx.height+decoder.height-1)/decoder.height+2);
            ctx.flushed_rows=ctx.y;
            ctx.average=static_cast<ThumbDecode::Average*>(std::calloc(static_cast<size_t>(width)*ctx.average_rows,sizeof(ThumbDecode::Average)));
            if(!ctx.average){fclose(file);thumb->pixels.Clear();thumb->failed=true;continue;}
            result = jd_decomp(&decoder, ThumbWrite, scale);
            if(result==JDR_OK && ctx.flushed_rows!=ctx.y+ctx.height) result=JDR_INP;
        } else result = JDR_FMT1;
        fclose(file);
        if (result != JDR_OK) {printf("PHOTO_DECODE_FAILED result=%d path=%s\n",(int)result,thumb->posix_path);thumb->pixels.Clear();thumb->failed=true;continue;}
        if (thumb->cancelled.load()) continue;
        if(cacheable){std::lock_guard<std::mutex> lock(s_thumb_cache_mutex);
            for(auto i=s_thumb_cache.begin();i!=s_thumb_cache.end();) {
                if(i->path==thumb->posix_path && i->width==width && i->height==height)i=s_thumb_cache.erase(i);else ++i;
            }
            const bool full=width>kGalleryThumbW;
            size_t count=0;
            for(const auto &entry:s_thumb_cache) if((entry.width>kGalleryThumbW)==full) ++count;
            if(count>=(full?2u:12u))for(auto i=s_thumb_cache.begin();i!=s_thumb_cache.end();++i)
                if((i->width>kGalleryThumbW)==full){s_thumb_cache.erase(i);break;}
            s_thumb_cache.push_back({thumb->posix_path,signature.st_size,signature.st_mtime,width,height,thumb->pixels});
        }
        PublishThumbnail(thumb);
    }
    return nullptr;
}

std::mutex s_thumb_jobs_mutex;
std::deque<ThumbRef> s_thumb_jobs;
bool s_thumb_worker_running=false;
void QueueThumbnails(std::vector<ThumbRef> &jobs) {
    std::lock_guard<std::mutex> lock(s_thumb_jobs_mutex);
    for(auto &job:jobs) {
        if(job->cancelled.load() || job->queued.exchange(true)) continue;
        if (job->target_width>kGalleryThumbW) s_thumb_jobs.push_front(std::move(job));
        else s_thumb_jobs.push_back(std::move(job));
    }
    jobs.clear();
    if(s_thumb_worker_running)return;
    pthread_t worker;pthread_attr_t attr;pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr,32768);
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    sched_param priority{};priority.sched_priority=CONFIG_METALIO_APP_PRIORITY-2;
    pthread_attr_setschedparam(&attr,&priority);pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
    s_thumb_worker_running=true;
    int result=hi_task_create_legacy(&worker,&attr,[](void*)->void*{
        for(;;){auto one=new std::vector<ThumbRef>;
            {std::lock_guard<std::mutex> lock(s_thumb_jobs_mutex);
                if(s_thumb_jobs.empty()){s_thumb_worker_running=false;delete one;return nullptr;}
                one->push_back(std::move(s_thumb_jobs.front()));s_thumb_jobs.pop_front();
            }
            int policy=0;sched_param priority{};
            if(pthread_getschedparam(pthread_self(),&policy,&priority)==0) {
                priority.sched_priority=CONFIG_METALIO_APP_PRIORITY-
                    (one->front()->target_width>kGalleryThumbW?1:2);
                (void)pthread_setschedparam(pthread_self(),SCHED_RR,&priority);
            }
            LoadGalleryThumbnails(one);
        }
    },nullptr);
    pthread_attr_destroy(&attr);
    if(result!=0){s_thumb_worker_running=false;
        for(auto &job:s_thumb_jobs){job->queued=false;job->failed=true;}
        s_thumb_jobs.clear();}
}

// Called only by LVGL. A large SD library must not retain a decoded bitmap
// for every off-screen photo. Never release pixels while its worker owns it.
void RefreshVisibleGallery(lv_timer_t *) {
    if(!s_screen_active || s_view_mode!=ViewMode::kGallery || !s_ui.gallery_scroll) return;
    lv_area_t viewport{};lv_obj_get_coords(s_ui.gallery_scroll,&viewport);
    const int margin=kGalleryThumbH+36;
    std::vector<ThumbRef> jobs;
    for(const auto &ref:s_gallery_refs) {
        if(ref->cancelled.load() || ref->queued.load() || !ref->image) continue;
        lv_area_t area{};lv_obj_get_coords(ref->image,&area);
        const bool near=area.y2>=viewport.y1-margin && area.y1<=viewport.y2+margin;
        if(near) {
            if(ref->pixels.empty() && !ref->failed.load()) jobs.push_back(ref);
        } else if(!ref->pixels.empty() && ref!=s_viewer_preview) {
            lv_image_set_src(ref->image,nullptr);
            lv_image_cache_drop(&ref->descriptor);
            ref->descriptor.data=nullptr;ref->descriptor.data_size=0;
            ref->pixels.Clear();
        }
    }
    if(!jobs.empty()) QueueThumbnails(jobs);
}

void CancelViewerJob() {
    if (!s_viewer_job) { s_viewer_preview.reset(); return; }
    s_viewer_job->cancelled.store(true);
    if (s_ui.viewer_img) lv_image_set_src(s_ui.viewer_img, nullptr);
    s_viewer_job->image = nullptr;
    lv_image_cache_drop(&s_viewer_job->descriptor);
    s_viewer_job.reset();
    s_viewer_preview.reset();
}

void ClosePhotoViewer() {
    CancelViewerJob();
    if (s_ui.viewer_panel != nullptr) {
        lv_obj_add_flag(s_ui.viewer_panel, LV_OBJ_FLAG_HIDDEN);
    }
    s_viewer_posix_path[0] = '\0';
    if (s_view_mode == ViewMode::kViewer) {
        s_view_mode = ViewMode::kGallery;
    }
}

void ShowPhotoViewer(const ThumbRef& ctx) {
    if (s_ui.viewer_panel == nullptr || s_ui.viewer_img == nullptr || ctx == nullptr) {
        return;
    }
    CancelViewerJob();
    auto job = std::make_shared<ThumbCtx>();
    std::snprintf(job->posix_path, sizeof(job->posix_path), "%s", ctx->posix_path);
    job->image = s_ui.viewer_img;
    job->target_width = kPanelW - 20;
    job->target_height = kPanelH - kGalleryHeaderH - 100;
    s_viewer_job = job;
    s_view_mode = ViewMode::kViewer;
    // Show the viewer immediately; its thumbnail or icon is replaced once
    // the full JPEG has finished decoding off-thread.
    lv_obj_remove_flag(s_ui.viewer_panel, LV_OBJ_FLAG_HIDDEN);
    if (ctx->descriptor.data && !ctx->pixels.empty()) {
        s_viewer_preview = ctx;
        lv_image_set_src(s_ui.viewer_img, &ctx->descriptor);
    } else {
        lv_image_set_src(s_ui.viewer_img, kGalleryIconPath);
    }
    std::vector<ThumbRef> jobs;
    jobs.push_back(job);
    QueueThumbnails(jobs);
    // strlcpy is available on NuttX libc.
    snprintf(s_viewer_posix_path, sizeof(s_viewer_posix_path), "%s", ctx->posix_path);
}

void on_thumbnail_clicked(lv_event_t* e) {
    auto* ref = static_cast<ThumbRef*>(lv_event_get_user_data(e));
    if (ref == nullptr) {
        return;
    }
    ShowPhotoViewer(*ref);
}

void on_viewer_panel_clicked(lv_event_t* e) {
    if (lv_event_get_target(e) == s_ui.btn_viewer_del) {
        return;
    }
    ClosePhotoViewer();
}

void on_viewer_delete_clicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (s_viewer_posix_path[0] == '\0') {
        return;
    }
    if (unlink(s_viewer_posix_path) != 0) {
        ESP_LOGE(TAG, "delete failed: %s", s_viewer_posix_path);
        post_status_text(I18n::T("删除失败"), 0xFF5555);
        schedule_status_clear(2500);
        return;
    }
    ESP_LOGI(TAG, "deleted photo: %s", s_viewer_posix_path);
    ClosePhotoViewer();
    RebuildGallery();
}

void SetViewMode(ViewMode mode) {
    if (mode == ViewMode::kGallery &&
        !SdCardManager::GetInstance().EnsureWritable()) {
        post_status_text(I18n::T("请插入 SD 卡"), 0xFFAA00);
        schedule_status_clear(2500);
        return;
    }

    s_view_mode = mode;
    ClosePhotoViewer();

    const bool show_camera = (mode == ViewMode::kCamera);
    if (s_ui.camera_panel != nullptr) {
        if (show_camera) {
            lv_obj_remove_flag(s_ui.camera_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.camera_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ui.gallery_panel != nullptr) {
        if (show_camera) {
            lv_obj_add_flag(s_ui.gallery_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_ui.gallery_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ui.bottom_strip != nullptr) {
        if (show_camera) {
            lv_obj_remove_flag(s_ui.bottom_strip, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.bottom_strip, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ui.btn_capture != nullptr) {
        if (show_camera) {
            lv_obj_remove_flag(s_ui.btn_capture, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.btn_capture, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ui.btn_tab_gallery != nullptr) {
        if (show_camera) {
            lv_obj_remove_flag(s_ui.btn_tab_gallery, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.btn_tab_gallery, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Release capture bandwidth while browsing photos. Restart asynchronously
    // when returning to preview; never join the camera worker under LVGL.
    if (show_camera) {
        start_camera_worker();
        if (s_ui.btn_label) lv_label_set_text(s_ui.btn_label, I18n::T("拍照"));
        if (s_ui.btn_capture) lv_obj_set_style_bg_color(s_ui.btn_capture, lv_color_hex(0x222222), LV_PART_MAIN);
    }
    else stop_camera_worker();
    ApplyGalleryLayout(!show_camera);
    if (mode == ViewMode::kGallery) {
        RaiseGalleryHeader();
        RebuildGallery();
    }
}

struct GalleryBuildState {
    std::vector<std::string> files;
    size_t next = 0;
};
std::unique_ptr<GalleryBuildState> s_gallery_build;

void BuildGalleryBatch(lv_timer_t *) {
    if (!s_gallery_build || !s_ui.gallery_scroll || !s_screen_active) return;
    auto &build = *s_gallery_build;
    const size_t end = std::min(build.next + 6, build.files.size());
    while (build.next < end) {
        const auto &path = build.files[build.next++];
        const std::string name=path.substr(path.find_last_of('/')+1);
        auto ref = std::make_shared<ThumbCtx>();
        auto* ctx = ref.get();
        std::snprintf(ctx->lv_path, sizeof(ctx->lv_path), "S:%s", path.c_str());
        std::snprintf(ctx->posix_path, sizeof(ctx->posix_path), "%s", path.c_str());

        lv_obj_t* item = lv_button_create(s_ui.gallery_scroll);
        lv_obj_set_size(item, kGalleryThumbW, kGalleryThumbH + 36);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(item, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(item, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(item, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_pad_all(item, 4, LV_PART_MAIN);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t* img = lv_image_create(item);
        lv_obj_set_size(img, kGalleryThumbW - 8, kGalleryThumbH);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
        ctx->image = img;

        lv_obj_t* cap = lv_label_create(item);
        lv_label_set_text(cap, name.c_str());
        lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
        lv_obj_set_width(cap, kGalleryThumbW - 8);
        lv_obj_set_style_text_color(cap, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
        lv_obj_set_style_text_font(cap, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

        auto *owned_ref = new ThumbRef(ref);
        lv_obj_add_event_cb(item, on_thumbnail_clicked, LV_EVENT_CLICKED, owned_ref);
        lv_obj_add_event_cb(
            item,
            [](lv_event_t* ev) {
                auto *ref = static_cast<ThumbRef*>(lv_event_get_user_data(ev));
                (*ref)->cancelled.store(true);
                if ((*ref)->image) lv_image_set_src((*ref)->image, nullptr);
                (*ref)->image = nullptr;
                lv_image_cache_drop(&(*ref)->descriptor);
                delete ref;
            }, LV_EVENT_DELETE, owned_ref);
        s_gallery_refs.push_back(std::move(ref));
    }
    lv_obj_update_layout(s_ui.gallery_scroll);
    RefreshVisibleGallery(nullptr);
    if (build.next < build.files.size()) return;

    if (s_gallery_build_timer) {
        lv_timer_delete(s_gallery_build_timer);
        s_gallery_build_timer = nullptr;
    }
    s_gallery_build.reset();
}

void RebuildGallery() {
    if (s_ui.gallery_scroll == nullptr) return;
    const unsigned generation = ++s_gallery_generation;
    if (s_gallery_build_timer) {
        lv_timer_delete(s_gallery_build_timer);
        s_gallery_build_timer = nullptr;
    }
    s_gallery_build.reset();
    for(auto&ref:s_gallery_refs){ref->cancelled=true;if(ref->image){lv_image_set_src(ref->image,nullptr);ref->image=nullptr;}lv_image_cache_drop(&ref->descriptor);}
    s_gallery_refs.clear();
    lv_obj_clean(s_ui.gallery_scroll);
    if(!s_gallery_visible_timer)
        s_gallery_visible_timer=lv_timer_create(RefreshVisibleGallery,150,nullptr);

    if (!SdCardManager::GetInstance().IsMounted()) {
        if (s_ui.gallery_empty != nullptr) {
            lv_label_set_text(s_ui.gallery_empty, I18n::T("请插入 SD 卡"));
            lv_obj_remove_flag(s_ui.gallery_empty, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (s_ui.gallery_empty != nullptr) {
        lv_label_set_text(s_ui.gallery_empty, I18n::T("加载中"));
        lv_obj_remove_flag(s_ui.gallery_empty, LV_OBJ_FLAG_HIDDEN);
    }
    struct ScanResult {
        unsigned generation;
        std::vector<std::string> files;
    };
    auto result = std::make_unique<ScanResult>();
    result->generation = generation;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16384);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    sched_param priority{}; priority.sched_priority = CONFIG_METALIO_APP_PRIORITY-2;
    pthread_attr_setschedparam(&attr, &priority);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    const int started = hi_task_create_legacy(&thread, &attr, [](void *arg) -> void * {
        std::unique_ptr<ScanResult> result(static_cast<ScanResult *>(arg));
        CollectRootJpgFiles(result->files);
        ScanResult *pending = result.release();
        // lv_async_call creates an LVGL timer; it is not thread-safe itself.
        lv_lock();
        const auto scheduled=lv_async_call([](void *arg) {
            std::unique_ptr<ScanResult> result(static_cast<ScanResult *>(arg));
            if (!s_screen_active || result->generation != s_gallery_generation.load() ||
                s_view_mode != ViewMode::kGallery || s_ui.gallery_scroll == nullptr)
                return;
            if (result->files.empty()) {
                if (s_ui.gallery_empty != nullptr)
                    lv_label_set_text(s_ui.gallery_empty, I18n::T("暂无照片"));
                return;
            }
            if (s_ui.gallery_empty != nullptr)
                lv_obj_add_flag(s_ui.gallery_empty, LV_OBJ_FLAG_HIDDEN);
            s_gallery_build = std::make_unique<GalleryBuildState>();
            s_gallery_build->files = std::move(result->files);
            BuildGalleryBatch(nullptr);
            if (s_gallery_build)
                s_gallery_build_timer = lv_timer_create(BuildGalleryBatch, 16, nullptr);
        }, pending);
        lv_unlock();
        if(scheduled!=LV_RESULT_OK)
            delete pending;
        return nullptr;
    }, result.get());
    pthread_attr_destroy(&attr);
    if (started == 0) {
        result.release();
    } else if (s_ui.gallery_empty != nullptr) {
        lv_label_set_text(s_ui.gallery_empty, I18n::T("加载失败"));
    }
}

void on_tab_back_clicked(lv_event_t* /*e*/) {
    // 无论是否在预览大图，左上角返回都直接回到待拍照页。
    SetViewMode(ViewMode::kCamera);
}

void on_tab_gallery_clicked(lv_event_t* /*e*/) {
    SetViewMode(ViewMode::kGallery);
}

// ---------------------------------------------------------------------------
// Camera worker — drives the configured CameraDriver. The original
// ran the V4L2 pipeline here; openvela keeps the same task structure
// (slot mutex, non-blocking stop, async LVGL updates) but delegates
// the actual frame production to CameraDriver::StartStreaming.
//
// The driver is expected to call OnFrameReady() from its own worker
// after each frame; OnFrameReady hops into the LVGL thread via
// a UI timer and invalidates the canvas. s_photo_frozen skips that
// invalidate so the screen keeps the frame that was visible at capture.
// ---------------------------------------------------------------------------
void camera_worker_task(void *) {
    for (;;) {
        pthread_mutex_lock(&s_camera_state_mutex);
        if (!s_camera_running.load()) {
            s_camera_task_handle = 0;
            pthread_mutex_unlock(&s_camera_state_mutex);
            return;
        }
        const unsigned generation = s_camera_generation.load();
        pthread_mutex_unlock(&s_camera_state_mutex);
        esp_err_t err = ESP_FAIL;
        // A transient sensor/bus startup failure must be fully unwound before
        // retrying. Remain on this owner thread; never overlap two stream tasks.
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            if (!s_camera_running.load() || generation != s_camera_generation.load()) break;
            err = ActiveDriver()->StartStreaming(s_capture_buf,
                kCameraAreaW, kCameraAreaH, OnFrameReady,
                reinterpret_cast<void *>(static_cast<uintptr_t>(generation)));
            if (err == ESP_OK) break;
            ActiveDriver()->StopStreaming();
            ESP_LOGW(TAG, "camera startup attempt %u failed: %d", attempt + 1, err);
            if (attempt < 2) {
                for (unsigned wait = 0; wait < (attempt + 1) * 10; ++wait) {
                    if (!s_camera_running.load() || generation != s_camera_generation.load()) break;
                    vTaskDelayMs(20);
                }
            }
        }
        if (err == ESP_OK) {
            s_last_capture_us.store(esp_timer_get_time());
            while (s_camera_running.load() && generation == s_camera_generation.load()) {
                vTaskDelayMs(100);
                if (!s_photo_frozen && !s_frame_async_pending &&
                    esp_timer_get_time()-s_last_capture_us.load() > 5000000) {
                    ESP_LOGW(TAG, "stalled preview: reopen the owned V4L2 session");
                    ActiveDriver()->StopStreaming();
                    s_last_capture_us.store(esp_timer_get_time());
                    err=ActiveDriver()->StartStreaming(s_capture_buf,
                        kCameraAreaW,kCameraAreaH,OnFrameReady,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(generation)));
                    if (err!=ESP_OK) {
                        post_status_text(I18n::T("摄像头初始化失败"),0xFF5555,generation);
                        break;
                    }
                }
            }
        } else if (s_camera_running.load() && generation == s_camera_generation.load()) {
            post_status_text(I18n::T("摄像头初始化失败"), 0xFF5555, generation);
        }
        ActiveDriver()->StopStreaming();
        pthread_mutex_lock(&s_camera_state_mutex);
        if (generation == s_camera_generation.load()) s_camera_running = false;
        pthread_mutex_unlock(&s_camera_state_mutex);
        // A new screen may have requested a preview during cleanup. Service
        // that generation only after the old capture thread has joined.
    }
}
esp_err_t start_camera_worker() {
    // The UI pulls the latest frame. The capture worker must never wait for
    // lv_async_call's LVGL lock while holding a dequeued camera buffer.
    lv_lock();
    if (!s_frame_timer) {
        s_frame_timer = lv_timer_create([](lv_timer_t *) {
            if (!s_frame_async_pending.load(std::memory_order_acquire)) return;
            OnFrameReadyAsync(reinterpret_cast<void *>(static_cast<uintptr_t>(s_camera_generation.load())));
        }, kPreviewRefreshMs, nullptr);
    }
    const bool frame_timer_ready = s_frame_timer != nullptr;
    lv_unlock();
    if (!frame_timer_ready) return ESP_ERR_NO_MEM;
    pthread_mutex_lock(&s_camera_state_mutex);
    // Create starts sensor bring-up alongside UI construction. LOAD is idempotent.
    if(s_camera_running.load()){pthread_mutex_unlock(&s_camera_state_mutex);return ESP_OK;}
    if (!s_capture_buf) s_capture_buf = static_cast<uint8_t *>(memalign(64, kPreviewBytes));
    if (!s_pending_buf) s_pending_buf = static_cast<uint8_t *>(memalign(64, kPreviewBytes));
    if (!s_capture_buf || !s_pending_buf) {
        pthread_mutex_unlock(&s_camera_state_mutex);
        return ESP_ERR_NO_MEM;
    }
    ++s_camera_generation;
    pthread_mutex_lock(&s_frame_mutex);
    s_frame_async_pending.store(false, std::memory_order_release);
    s_pending_generation = 0;
    pthread_mutex_unlock(&s_frame_mutex);
    s_camera_running = true;
    s_photo_frozen = false;
    s_got_preview_frame = false;
    ActiveDriver()->SetFrameInterval(kPreviewFrameInterval);
    int ret = ESP_OK;
    if (!s_camera_task_handle && xTaskCreateLegacy(camera_worker_task, "cam_screen",
            8 * 1024, nullptr, tskIDLE_PRIORITY + 4, &s_camera_task_handle) != pdPASS) {
        s_camera_running = false;
        ret = ESP_FAIL;
    }
    pthread_mutex_unlock(&s_camera_state_mutex);
    return ret;
}
void stop_camera_worker() {
    pthread_mutex_lock(&s_camera_state_mutex);
    s_camera_running = false;
    ++s_camera_generation;
    pthread_mutex_unlock(&s_camera_state_mutex);
    s_frame_async_pending.store(false);
    lv_lock();
    if (s_frame_timer) { lv_timer_delete(s_frame_timer); s_frame_timer = nullptr; }
    lv_unlock();
}

// ---------------------------------------------------------------------------
// LVGL 事件 (preserved verbatim)
// ---------------------------------------------------------------------------
void on_capture_btn_clicked(lv_event_t* /*e*/) {
    if (!s_got_preview_frame.load()) {
        post_status_text(I18n::T("摄像头无画面"), 0xFF5555);
        return;
    }
    const bool was_frozen = s_photo_frozen;
    s_photo_frozen = !s_photo_frozen;
    if (s_photo_frozen)
        s_frame_async_pending.store(false, std::memory_order_release);

    if (s_ui.btn_label != nullptr) {
        lv_label_set_text(s_ui.btn_label, s_photo_frozen ? I18n::T("实时预览") : I18n::T("拍照"));
    }
    if (s_ui.btn_capture != nullptr) {
        lv_obj_set_style_bg_color(
            s_ui.btn_capture,
            s_photo_frozen ? lv_color_hex(0xC62828) : lv_color_hex(0x222222),
            LV_PART_MAIN);
    }

    if (s_photo_frozen && !was_frozen) {
        start_save_photo_task();
    } else if (!s_photo_frozen && was_frozen) {
        post_status_clear();
    }

    ESP_LOGI(TAG, "capture button: %s", s_photo_frozen ? "FROZEN (photo)" : "LIVE preview");
}

void LeaveCameraVision()
{
    s_look_and_see_mode.store(false, std::memory_order_release);
    s_look_and_see_return_to_camera=false;
    auto &app=Application::GetInstance();
    app.GetAudioService().SetVisualConversation(false);
    if (s_look_and_see_owns_voice) app.RequestVoiceUiDesired(false);
    s_look_and_see_owns_voice=false;
    SystemChrome_CloseFloatAi();
}

void OnSwipeBack() {
    // 让 LVGL 在加载新屏前等当前手指 release，否则按 “返回” 按钮的 PRESSED
    // 事件会被新加载的主屏当成一次新 click —— 落在主屏的 “相机” 图标上时
    // 就会立刻又 LaunchCamera 一次。
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    if (s_look_and_see_mode.load(std::memory_order_acquire)) {
        if (s_look_and_see_return_to_camera) {
            LeaveCameraVision();
            return;
        }
        const VisionReturn origin=s_vision_return.load(std::memory_order_acquire);
        if(origin==VisionReturn::Home){
            SystemChrome_CloseFloatAi();
            HomeScreen::SwitchToHome();
        }else HomeScreen::LaunchApp(origin==VisionReturn::DigitalPeople?"digital_people":"chat");
        return;
    }
    HomeScreen::SwitchToHome();
}

void on_screen_unloaded(lv_event_t* e) {
    if (lv_event_get_current_target_obj(e) != s_ui.screen) return;
    ++s_gallery_generation;
    // 屏幕被切走 -> 抹掉对 LVGL 对象的引用。worker 任务停止由
    // CameraScreen::LifecycleCallback 在同一时机负责，等 stop 完成后
    // 屏幕对象才会被 lv_obj_delete_async 真正释放，所以这里的清理仅是“标记”。
    if (s_status_clear_timer != nullptr) {
        lv_timer_delete(s_status_clear_timer);
        s_status_clear_timer = nullptr;
    }
    if (s_gallery_build_timer != nullptr) {
        lv_timer_delete(s_gallery_build_timer);
        s_gallery_build_timer = nullptr;
    }
    if(s_gallery_visible_timer) {
        lv_timer_delete(s_gallery_visible_timer);
        s_gallery_visible_timer=nullptr;
    }
    s_gallery_build.reset();
    for(auto &ref:s_gallery_refs) ref->cancelled=true;
    s_gallery_refs.clear();
    CancelViewerJob();
    s_ui.screen          = nullptr;
    s_ui.camera_panel    = nullptr;
    s_ui.canvas          = nullptr;
    s_ui.gallery_panel   = nullptr;
    s_ui.gallery_header  = nullptr;
    s_ui.gallery_scroll  = nullptr;
    s_ui.gallery_empty   = nullptr;
    s_ui.btn_gallery_back = nullptr;
    s_ui.viewer_panel    = nullptr;
    s_ui.viewer_img      = nullptr;
    s_ui.btn_viewer_del  = nullptr;
    s_ui.bottom_strip    = nullptr;
    s_ui.btn_capture     = nullptr;
    s_ui.btn_label       = nullptr;
    s_ui.btn_tab_gallery = nullptr;
    s_ui.status_lbl      = nullptr;
    s_photo_frozen       = false;
    s_view_mode          = ViewMode::kCamera;
    s_viewer_posix_path[0] = '\0';
    s_screen_active      = false;
}

}  // namespace

// ---------------------------------------------------------------------------
// CameraScreen 公共接口
// ---------------------------------------------------------------------------
lv_obj_t* CameraScreen::CreateStatic() {
    // 每个有效帧都交给预览，异步显示仅保留最新一帧。
    s_view_mode = ViewMode::kCamera;
    s_photo_frozen = false;
    s_screen_active = true;

    // canvas 缓冲：720x600 RGB888，1.296 MB。一次申请、永不释放（重复进入摄像头屏幕
    // 时复用），避免反复 1.3MB 大块内存分配抖动。
    const size_t out_size = static_cast<size_t>(kCameraAreaW) * kCameraAreaH * 3;
    if (s_canvas_buf == nullptr) {
        // heap_caps_aligned_alloc(64, ...) -> memalign(64, ...) on NuttX.
        // NuttX libc provides memalign in stdlib.h; no size rounding needed.
        
        s_canvas_buf = static_cast<uint8_t*>(
            memalign(64, out_size));
        if (s_canvas_buf == nullptr) {
            ESP_LOGE(TAG, "alloc canvas buf %u failed", static_cast<unsigned>(out_size));
            return nullptr;
        }
    }
    // 每次进入相机屏先把画布抹黑：上一次退出时 canvas 里残留的最后一帧
    // 在 worker 出第一帧前会一直可见，看上去像 “上次的画面又出现了”。
    pthread_mutex_lock(&s_frame_mutex);
    std::memset(s_canvas_buf, 0, out_size);
    pthread_mutex_unlock(&s_frame_mutex);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen   = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ----- 相机预览区（720x600） -----
    lv_obj_t* camera_panel = lv_obj_create(scr);
    s_ui.camera_panel = camera_panel;
    screen_strip_obj_chrome(camera_panel);
    lv_obj_set_size(camera_panel, kCameraAreaW, kCameraAreaH);
    lv_obj_set_pos(camera_panel, 0, 0);
    lv_obj_set_style_bg_color(camera_panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(camera_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(camera_panel, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(camera_panel);

    lv_obj_t* canvas = lv_canvas_create(camera_panel);
    s_ui.canvas      = canvas;
    lv_canvas_set_buffer(canvas, s_canvas_buf, kCameraAreaW, kCameraAreaH,
                         LV_COLOR_FORMAT_RGB888);
    lv_obj_set_style_bg_color(canvas, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(canvas, 0, 0);
    screen_make_input_passive(canvas);

    lv_obj_t* status = lv_label_create(camera_panel);
    s_ui.status_lbl  = status;
    lv_label_set_text(status, I18n::T("正在启动摄像头…"));
    lv_obj_set_style_text_color(status, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(status);

    // Start cold sensor initialization while the rest of the screen is built.
    // No hidden camera at boot; same full-resolution driver and exposure settings.
    s_external_canvas=nullptr;
    if(start_camera_worker()!=ESP_OK)update_status_label(I18n::T("摄像头初始化失败"),lv_color_hex(0xFF5555));

    // ----- 相册区（默认隐藏，进入后全屏 + 顶部 header） -----
    lv_obj_t* gallery_panel = lv_obj_create(scr);
    s_ui.gallery_panel = gallery_panel;
    screen_strip_obj_chrome(gallery_panel);
    lv_obj_set_size(gallery_panel, kPanelW, kPanelH);
    lv_obj_set_pos(gallery_panel, 0, 0);
    lv_obj_set_style_bg_color(gallery_panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_add_flag(gallery_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(gallery_panel, LV_OBJ_FLAG_SCROLLABLE);

    BuildGalleryHeader(gallery_panel);

    lv_obj_t* gallery_scroll = lv_obj_create(gallery_panel);
    s_ui.gallery_scroll = gallery_scroll;
    screen_strip_obj_chrome(gallery_scroll);
    lv_obj_set_size(gallery_scroll, kPanelW, kPanelH - kGalleryHeaderH);
    lv_obj_set_pos(gallery_scroll, 0, kGalleryHeaderH);
    lv_obj_set_style_bg_color(gallery_scroll, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_flex_flow(gallery_scroll, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(gallery_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(gallery_scroll, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(gallery_scroll, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(gallery_scroll, 10, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(gallery_scroll, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(gallery_scroll);

    lv_obj_t* gallery_empty = lv_label_create(gallery_panel);
    s_ui.gallery_empty = gallery_empty;
    lv_label_set_text(gallery_empty, I18n::T("暂无照片"));
    lv_obj_set_style_text_color(gallery_empty, lv_color_hex(0x9A9A9A), LV_PART_MAIN);
    lv_obj_set_style_text_font(gallery_empty, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(gallery_empty, LV_ALIGN_CENTER, 0, kGalleryHeaderH / 2);
    lv_obj_add_flag(gallery_empty, LV_OBJ_FLAG_HIDDEN);
    screen_make_input_passive(gallery_empty);

    RaiseGalleryHeader();

    // ----- 大图查看层（点击缩略图后显示） -----
    lv_obj_t* viewer_panel = lv_obj_create(scr);
    s_ui.viewer_panel = viewer_panel;
    screen_strip_obj_chrome(viewer_panel);
    lv_obj_set_size(viewer_panel, kPanelW, kPanelH - kGalleryHeaderH);
    lv_obj_set_pos(viewer_panel, 0, kGalleryHeaderH);
    lv_obj_set_style_bg_color(viewer_panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(viewer_panel, LV_OPA_90, LV_PART_MAIN);
    lv_obj_add_flag(viewer_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(viewer_panel, on_viewer_panel_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* viewer_img = lv_image_create(viewer_panel);
    s_ui.viewer_img = viewer_img;
    lv_obj_set_size(viewer_img, kPanelW - 20, kPanelH - kGalleryHeaderH - 100);
    lv_image_set_inner_align(viewer_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(viewer_img, LV_ALIGN_CENTER, 0, -20);
    screen_make_input_passive(viewer_img);

    lv_obj_t* viewer_del = lv_button_create(viewer_panel);
    s_ui.btn_viewer_del = viewer_del;
    lv_obj_set_size(viewer_del, 160, 56);
    lv_obj_align(viewer_del, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_radius(viewer_del, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(viewer_del, lv_color_hex(0xE74C3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(viewer_del, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(viewer_del, on_viewer_delete_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* viewer_del_lbl = lv_label_create(viewer_del);
    lv_label_set_text(viewer_del_lbl, I18n::T("删除"));
    lv_obj_set_style_text_color(viewer_del_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(viewer_del_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(viewer_del_lbl);

    // ----- 底部按钮区（720x120 黑底） -----
    lv_obj_t* strip = lv_obj_create(scr);
    s_ui.bottom_strip = strip;
    screen_strip_obj_chrome(strip);
    lv_obj_set_size(strip, kPanelW, kButtonStripH);
    lv_obj_set_pos(strip, 0, kButtonStripY);
    lv_obj_set_style_bg_color(strip, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    // Separate from the shutter and bottom gesture strip; native shapes
    // avoid missing icon-font glyphs and preserve the existing camera style.
    lv_obj_t *vision=lv_button_create(strip);
    screen_strip_obj_chrome(vision);
    lv_obj_set_size(vision,180,72);
    lv_obj_align(vision,LV_ALIGN_LEFT_MID,24,0);
    lv_obj_set_style_radius(vision,28,LV_PART_MAIN);
    lv_obj_set_style_bg_color(vision,lv_color_hex(0x132C48),LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vision,LV_OPA_COVER,LV_PART_MAIN);
    lv_obj_set_style_border_width(vision,1,LV_PART_MAIN);
    lv_obj_set_style_border_color(vision,lv_color_hex(0x4E91DB),LV_PART_MAIN);
    lv_obj_set_style_bg_color(vision,lv_color_hex(0x24568C),LV_STATE_PRESSED);
    lv_obj_t *lens=lv_obj_create(vision);
    screen_strip_obj_chrome(lens);
    lv_obj_set_size(lens,28,28);
    lv_obj_align(lens,LV_ALIGN_LEFT_MID,14,0);
    lv_obj_set_style_radius(lens,14,LV_PART_MAIN);
    lv_obj_set_style_bg_color(lens,lv_color_hex(0x7FC9FF),LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lens,LV_OPA_COVER,LV_PART_MAIN);
    screen_make_input_passive(lens);
    lv_obj_t *pupil=lv_obj_create(lens);
    screen_strip_obj_chrome(pupil);
    lv_obj_set_size(pupil,12,12);lv_obj_center(pupil);
    lv_obj_set_style_radius(pupil,6,LV_PART_MAIN);
    lv_obj_set_style_bg_color(pupil,lv_color_hex(0x163F65),LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pupil,LV_OPA_COVER,LV_PART_MAIN);
    screen_make_input_passive(pupil);
    lv_obj_t *vision_label=lv_label_create(vision);
    lv_label_set_text(vision_label,"Hi 视界");
    lv_obj_set_style_text_font(vision_label,&font_puhui_20_4,LV_PART_MAIN);
    lv_obj_set_style_text_color(vision_label,lv_color_hex(0xE1F3FF),LV_PART_MAIN);
    lv_obj_align(vision_label,LV_ALIGN_RIGHT_MID,-12,0);
    screen_make_input_passive(vision_label);
    lv_obj_add_event_cb(vision,[](lv_event_t *) {
        if (s_look_and_see_return_to_camera && CameraScreen::IsLookAndSeeMode())
            LeaveCameraVision();
        else CameraScreen::OpenLookAndSee(false);
    },LV_EVENT_SHORT_CLICKED,nullptr);

    lv_obj_t* tab_gallery = lv_button_create(strip);
    s_ui.btn_tab_gallery = tab_gallery;
    lv_obj_set_size(tab_gallery, 80, 80);
    lv_obj_align(tab_gallery, LV_ALIGN_RIGHT_MID, -24, 0);
    lv_obj_set_style_radius(tab_gallery, 40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tab_gallery, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tab_gallery, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tab_gallery, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab_gallery, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(tab_gallery, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(tab_gallery, on_tab_gallery_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* gallery_icon = lv_image_create(tab_gallery);
    lv_image_set_src(gallery_icon, kGalleryIconPath);
    lv_image_set_inner_align(gallery_icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(gallery_icon);
    lv_obj_remove_flag(gallery_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* btn = lv_button_create(strip);
    s_ui.btn_capture = btn;
    lv_obj_set_size(btn, 200, 80);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(btn, 40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 3, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, on_capture_btn_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(btn);
    s_ui.btn_label = lbl;
    lv_label_set_text(lbl, I18n::T("拍照"));
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(lbl);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}

namespace {

}  // namespace

lv_obj_t* CameraScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void CameraScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: camera_screen -> start worker");
        // CAM_PWDN 由单一 worker 任务串行处理。这里如果
        // 直接动 IOExpander，会和上一个 worker 还没完成的 cleanup（也在改
        // CAM_PWDN）撞车。
        s_external_canvas = nullptr;
        if (start_camera_worker() != ESP_OK) {
            ESP_LOGE(TAG, "start_camera_worker failed");
        }
        if (s_look_and_see_mode.load(std::memory_order_acquire)) {
            auto &app=Application::GetInstance();
            s_look_and_see_owns_voice=true;
            app.GetAudioService().SetVisualConversation(true);
            app.RequestVoiceUiDesired(true);
            // Entering look-and-see is an explicit voice action. Use the
            // same deferred microphone/session path as the assistant pill,
            // after the dialog has painted; the camera never opens a second
            // microphone or changes the audio sample clock.
            SystemChrome_EnsureFloatAi(true);
        }
    } else {
        ESP_LOGI(TAG, "unload: camera_screen -> signal worker stop (non-blocking)");
        // 只发停止信号；worker 在自己的 task 里 cleanup（关流、断电）。
        // LVGL 主线程不阻塞，避免 adapter 后台任务长时间拿不到锁。
        stop_camera_worker();
        s_external_canvas = nullptr;
        if (s_look_and_see_listen_timer != nullptr) {
            lv_timer_delete(s_look_and_see_listen_timer);
            s_look_and_see_listen_timer = nullptr;
        }
        if (s_look_and_see_mode.exchange(false, std::memory_order_acq_rel)) {
            Application::GetInstance().GetAudioService().SetVisualConversation(false);
            if(s_look_and_see_owns_voice)Application::GetInstance().RequestVoiceUiDesired(false);
            s_look_and_see_owns_voice=false;
        }
    }
}

bool CameraScreen::PreparePreviewBuffer(PreviewBuffer* out) {
    if (out == nullptr) {
        return false;
    }
    const size_t out_size =
        static_cast<size_t>(kCameraAreaW) * kCameraAreaH * 3;
    if (s_canvas_buf == nullptr) {
        s_canvas_buf = static_cast<uint8_t*>(memalign(64, out_size));
        if (s_canvas_buf == nullptr) {
            ESP_LOGE(TAG, "alloc preview buffer failed");
            return false;
        }
    }
    pthread_mutex_lock(&s_frame_mutex);
    std::memset(s_canvas_buf, 0, out_size);
    pthread_mutex_unlock(&s_frame_mutex);
    out->data   = s_canvas_buf;
    out->width  = kCameraAreaW;
    out->height = kCameraAreaH;
    return true;
}

esp_err_t CameraScreen::StartExternalPreview(lv_obj_t* canvas) {
    if (canvas == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    s_external_canvas = canvas;
    s_photo_frozen    = false;
    return start_camera_worker();
}

void CameraScreen::StopExternalPreview() {
    stop_camera_worker();
    s_external_canvas = nullptr;
}

void CameraScreen::SetPreviewObserver(PreviewObserver observer) {
    s_preview_observer = observer;
}

bool CameraScreen::CanAcceptPreviewFrame() {
    // Three buffers have separate owners. Capture may replace an obsolete
    // pending frame under s_frame_mutex while LVGL owns the displayed one.
    // Waiting for LVGL here serialized capture and display despite the ring.
    return !s_photo_frozen.load(std::memory_order_relaxed);
}

void CameraScreen::SetCameraDriver(CameraDriver *driver) {
    s_driver = driver;
}

void CameraScreen::SetJpegEncoder(JpegEncoder *encoder) {
    s_encoder = encoder;
}

bool CameraScreen::OpenLookAndSee(bool return_to_digital_people) {
    if (s_ui.screen && s_ui.screen==lv_screen_active() && s_screen_active) {
        // Reuse this exact live sensor/frame; no camera close/reopen or
        // full-screen transition before the user can ask a scene question.
        const bool already=IsLookAndSeeMode();
        s_look_and_see_mode.store(true,std::memory_order_release);
        if (!already) {s_look_and_see_return_to_camera=true;++s_vision_visit;}
        s_photo_frozen=false;
        if (s_view_mode!=ViewMode::kCamera) SetViewMode(ViewMode::kCamera);
        else if (!s_camera_running.load()) start_camera_worker();
        if (s_ui.btn_label) lv_label_set_text(s_ui.btn_label,I18n::T("拍照"));
        if (s_ui.btn_capture) lv_obj_set_style_bg_color(s_ui.btn_capture,lv_color_hex(0x222222),LV_PART_MAIN);
        post_status_clear();
        auto &app=Application::GetInstance();
        s_look_and_see_owns_voice=true;
        app.GetAudioService().SetVisualConversation(true);
        app.RequestVoiceUiDesired(true);
        SystemChrome_EnsureFloatAi(true);
        return true;
    }
    s_look_and_see_return_to_camera=false;
    ++s_vision_visit;
    // The false argument historically meant both "chat" and every popup
    // entry, which incorrectly sent home-origin conversations into Chat.
    // Capture the foreground owner before LaunchApp replaces that screen.
    const char *origin=PwrKey_ActiveScreen();
    s_vision_return.store(return_to_digital_people || !strcmp(origin,"digital_people")?
        VisionReturn::DigitalPeople:!strcmp(origin,"chat")?VisionReturn::Chat:VisionReturn::Home,
        std::memory_order_release);
    s_look_and_see_mode.store(true, std::memory_order_release);
    if (HomeScreen::LaunchApp("camera")) {
        return true;
    }
    s_look_and_see_mode.store(false, std::memory_order_release);
    return false;
}

bool CameraScreen::IsLookAndSeeMode() {
    return s_look_and_see_mode.load(std::memory_order_acquire);
}

unsigned CameraScreen::VisionVisit() {
    return s_vision_visit.load(std::memory_order_acquire);
}

void CameraScreen::SetVisionService(const std::string &url,
                                    const std::string &token) {
    pthread_mutex_lock(&s_vision_config_mutex);
    s_vision_url = url;
    s_vision_token = token;
    pthread_mutex_unlock(&s_vision_config_mutex);
}

bool CameraScreen::CaptureAndExplain(const std::string &question,
                                     std::string &answer,
                                     std::string &error) {
    answer.clear();
    error.clear();
    const int64_t started_us=esp_timer_get_time();
    const unsigned visit=VisionVisit();
    if (!IsLookAndSeeMode()) {
        error = "请先打开Hi 视界页面";
        return false;
    }
    // App launch and sensor bring-up are asynchronous. A tool request can
    // arrive before LOAD or before the UI has consumed its first frame.
    // Wait off the UI/audio thread; do not turn that race into camera failure.
    const int64_t ready_deadline=started_us+10000000;
    while (VisionVisitActive(visit) && !HasVisionFrame() &&
           esp_timer_get_time()<ready_deadline) vTaskDelayMs(20);
    if (!VisionVisitActive(visit)) {error="本次Hi 视界识别已取消";return false;}
    if (!HasVisionFrame()) {
        error = "相机首帧尚未就绪，请稍后重试画面识别";
        return false;
    }

    // Upload the same full-resolution, full-quality pixels as a manual
    // photo, including ordinary questions. Never silently downscale text.
    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    const bool encoded = CapturePhotoJpeg(&jpeg, &jpeg_len, visit);
    if (!encoded || jpeg == nullptr || jpeg_len == 0) {
        std::free(jpeg);
        error = VisionVisitActive(visit) ? "画面编码未完成，请稍后重试" : "本次Hi 视界识别已取消";
        return false;
    }
    const unsigned camera_generation=s_camera_generation.load();

    // Keep the compressed request in memory; a spoken visual question must
    // not wait on FAT I/O before upload.
    struct FreeJpeg {void operator()(uint8_t *p) const {std::free(p);}};
    std::unique_ptr<uint8_t, FreeJpeg> jpeg_owner(jpeg);
    std::string vision_url;
    std::string vision_token;
    const int64_t config_deadline=esp_timer_get_time()+2000000;
    do {
        pthread_mutex_lock(&s_vision_config_mutex);
        vision_url = s_vision_url;
        vision_token = s_vision_token;
        pthread_mutex_unlock(&s_vision_config_mutex);
        if (!vision_url.empty() || !VisionVisitActive(visit)) break;
        vTaskDelayMs(20);
    } while (esp_timer_get_time()<config_deadline);
    if (!VisionVisitActive(visit)) {error="本次Hi 视界识别已取消";return false;}
    if (vision_url.empty()) {
        error = "相机已有画面，视觉服务尚未就绪，请稍后重试";
        return false;
    }

    auto http = std::make_unique<Http>();
    if (!http) {
        error = "无法创建视觉分析连接";
        return false;
    }
    const std::string boundary = "----HIOS_LOOK_AND_SEE_BOUNDARY";
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid());
    if (!vision_token.empty()) {
        http->SetHeader("Authorization", "Bearer " + vision_token);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    const std::string prompt = question.empty()
        ? std::string("请用一两句话直接描述当前画面的主要内容。") :
          question+" 请直接简要回答，不要复述问题。";
    const std::string prefix = "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"question\"\r\n\r\n" +
        prompt + "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n";
    const std::string suffix = "\r\n--" + boundary + "--\r\n";
    std::string body; body.reserve(prefix.size()+jpeg_len+suffix.size());
    body=prefix; body.append(reinterpret_cast<const char *>(jpeg),jpeg_len); body+=suffix;
    http->SetContent(std::move(body));
    // Five seconds is a latency target, not proof of camera failure. DNS,
    // TLS, full-quality upload and remote inference get their own bounded
    // budget, independent of camera startup and JPEG encoding.
    const int64_t deadline_us=esp_timer_get_time()+20000000;
    const int64_t prepared_us=esp_timer_get_time();
    http->SetDeadlineMs(20000);
    if (!http->Open("POST", vision_url)) {
        error = esp_timer_get_time()>=deadline_us || http->GetLastError()==ETIMEDOUT ?
            "相机已取图，视觉服务响应超时，请稍后重试" : "相机已取图，视觉服务连接失败，请检查网络";
        return false;
    }
    const int status = http->GetStatusCode();
    if (status != 200) {
        error = "相机已取图，视觉服务返回 HTTP " + std::to_string(status);
        http->Close();
        return false;
    }
    answer = http->ReadAll();
    const bool incomplete=http->GetLastError()!=0 ||
        (http->GetBodyLength()>0 && answer.size()!=http->GetBodyLength());
    http->Close();
    printf("VISION_REQUEST elapsed_ms=%lld prepare_ms=%lld bytes=%u complete=%d\n",
        static_cast<long long>((esp_timer_get_time()-started_us)/1000),
        static_cast<long long>((prepared_us-started_us)/1000),
        static_cast<unsigned>(jpeg_len),!incomplete);
    if (incomplete) {
        answer.clear();error=esp_timer_get_time()>=deadline_us?"相机已取图，视觉服务响应超时，请稍后重试":"画面识别响应不完整，请重试";
        return false;
    }
    if (answer.empty()) {
        error = "小智视觉服务没有返回画面描述";
        return false;
    }
    if (!VisionVisitActive(visit)) {
        answer.clear();error="本次Hi 视界识别已取消";return false;
    }
    // Transfer the exact full-quality recognized image to an independent
    // bounded SD worker. No second exposure, encode, or SD wait on the reply.
    if (QueueVisionSnapshot(jpeg_owner.get(),jpeg_len,camera_generation)) {
        jpeg_owner.release();
    } else {
        answer += "\n本次画面已识别，但照片保存任务未能启动，请稍后重试保存。";
        post_status_text("照片保存任务未能启动",0xFF8855,camera_generation);
    }
    return true;
}

bool CameraScreen::IsCapturing() { return s_camera_running.load() && !s_photo_frozen.load(); }
bool CameraScreen::PreviewStopped(){pthread_mutex_lock(&s_camera_state_mutex);bool stopped=!s_camera_running.load()&&!s_camera_task_handle;pthread_mutex_unlock(&s_camera_state_mutex);return stopped;}
