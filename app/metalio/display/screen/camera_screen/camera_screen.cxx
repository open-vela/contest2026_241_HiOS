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
 * Hardware dependencies are stubbed behind two clean interfaces
 * declared in the header:
 *
 *   - CameraDriver: replaces the V4L2 / esp_video_init / esp_cam_sensor
 *     / IOExpander::CAM_PWDN pipeline. Default StubCameraDriver paints
 *     a slowly-animated placeholder gradient into the canvas buffer so
 *     the preview visibly updates and the freeze-on-capture flow can
 *     be exercised end-to-end.
 *   - JpegEncoder: replaces image_to_jpeg. Default StubJpegEncoder
 *     returns false, matching the original's "编码失败，未保存" path.
 *
 * Other porting changes:
 *   - heap_caps_aligned_alloc -> memalign (NuttX libc provides it in stdlib.h).
 *   - esp_lv_adapter_lock/unlock -> direct calls. The original needed
 *     them because the V4L2 worker ran on a separate task and touched
 *     LVGL objects. The stub driver only calls lv_obj_invalidate()
 *     through a lv_async_call trampoline (OnFrameReadyAsync), so all
 *     LVGL mutations happen on the LVGL thread and no lock is needed.
 *   - xTaskCreatePinnedToCore -> freertos_shim provides it (delegates
 *     to xTaskCreate).
 *   - vTaskDelay(pdMS_TO_TICKS(ms)) -> vTaskDelayMs(ms) (shim).
 *   - HomeScreen::Create() -> HomeScreen::CreateStatic().
 */

#include "camera_screen.h"
#include "camera_v4l2_driver.h"
#include "i18n.h"
#include <nuttx/cache.h>
#include "esp_log_shim.h"
#include "esp_timer_shim.h"
#include "esp_err_shim.h"
#include "freertos_shim.h"

#ifndef tskIDLE_PRIORITY
#define tskIDLE_PRIORITY 0
#endif
#include "home_screen/home_screen.h"
#include "SdCardManager.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <malloc.h>
#include <memory>
#include <string>
#include <vector>

#include "lvgl.h"

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

// 拍照保存：缩小分辨率 + JPEG 质量（见 camera_screen.h 中 CAMERA_JPEG_QUALITY）。
#ifndef CAMERA_JPEG_QUALITY
#define CAMERA_JPEG_QUALITY 55
#endif
constexpr int      kSaveW         = 360;
constexpr int      kSaveH         = 300;
constexpr uint8_t  kJpegQuality   = CAMERA_JPEG_QUALITY;
constexpr int      kGalleryThumbW   = 220;
constexpr int      kGalleryThumbH   = 165;
constexpr int      kMaxGalleryItems = 48;
constexpr size_t   kMaxNameLen      = 256;
constexpr const char* kLvSdRoot     = "S:/sdcard";
constexpr const char* kPosixSdRoot  = "/sdcard";
constexpr const char* kGalleryIconPath = "A:ic_s_camera_picture.spng";

enum class ViewMode {
    kCamera,
    kGallery,
    kViewer,
};

// Worker control (preserved semantics; only the body of the worker task
// differs — it now drives the configured CameraDriver instead of V4L2).
volatile bool       s_camera_running    = false;
TaskHandle_t        s_camera_task_handle = 0;
SemaphoreHandle_t   s_worker_slot        = nullptr;

// Frame interval (1 = high quality, 2 = medium (default), 4 = low).
volatile int s_frame_interval = 2;

// camera_out_buf 与 LVGL canvas 绑定 —— 只分配一次、永不释放，避免重复进入摄像头屏幕时
// 反复申请 1.3 MB 的 PSRAM。摄像头采集任务直接覆写这个缓冲。
uint8_t* s_canvas_buf = nullptr;
lv_obj_t* s_external_canvas = nullptr;

// 拍照冻结标志：true 时摄像头任务暂停刷新画面（屏幕显示最后一帧 = 照片）
volatile bool s_photo_frozen = false;

// True while a save_photo_task is running. Prevents the capture button
// from kicking off concurrent saves.
volatile bool s_save_in_progress = false;

lv_timer_t* s_status_clear_timer = nullptr;
char s_viewer_posix_path[kMaxNameLen + 16] = {};

// Pluggable drivers. Defaults are the inline stubs declared below.
CameraScreen::CameraDriver  *s_driver  = nullptr;
CameraScreen::JpegEncoder   *s_encoder = nullptr;

// Forward decls.
void RebuildGallery();
void SetViewMode(ViewMode mode);
void on_tab_back_clicked(lv_event_t* e);
esp_err_t start_camera_worker();
void stop_camera_worker();

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
bool s_screen_active = false;

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
void post_status_text(const char* text, uint32_t color_hex) {
    // Pack the text + color into a small heap struct so the async
    // callback can read them on the LVGL thread.
    struct StatusMsg {
        char text[64];
        uint32_t color_hex;
    };
    auto *msg = new StatusMsg{};
    std::snprintf(msg->text, sizeof(msg->text), "%s", text ? text : "");
    msg->color_hex = color_hex;
    lv_async_call([](void *user_data) {
        std::unique_ptr<StatusMsg> msg(static_cast<StatusMsg *>(user_data));
        if (msg == nullptr || !s_screen_active || s_ui.status_lbl == nullptr) {
            return;
        }
        update_status_label(msg->text, lv_color_hex(msg->color_hex));
    }, msg);
}

void post_status_clear() {
    lv_async_call([](void * /*user_data*/) {
        if (!s_screen_active || s_ui.status_lbl == nullptr) return;
        update_status_label(nullptr, lv_color_white());
    }, nullptr);
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
    lv_timer_set_repeat_count(s_status_clear_timer, 1);
    lv_unlock();
}

// ---------------------------------------------------------------------------
// Frame-ready trampoline. The CameraDriver calls this (potentially from
// its own worker thread) after writing a new BGR888 frame into
// s_canvas_buf. We hop into the LVGL thread via lv_async_call and
// invalidate the canvas so LVGL repaints on the next refresh.
// ---------------------------------------------------------------------------
void OnFrameReadyAsync(void * /*user_data*/) {
    if (!s_screen_active) return;
    // 拍照冻结时跳过 invalidate —— 屏幕保留最后一帧作为照片。
    // The stub driver still paints into s_canvas_buf, but since we do
    // not invalidate the canvas the displayed image stays put until
    // the user unfreezes.
    if (s_photo_frozen) return;
    lv_obj_t* canvas = (s_external_canvas != nullptr) ? s_external_canvas
                                                      : s_ui.canvas;
    if (canvas != nullptr) {
        lv_obj_invalidate(canvas);
    }
}

void OnFrameReady(void * /*user_data*/) {
    lv_async_call(OnFrameReadyAsync, nullptr);
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
    return (s_encoder != nullptr) ? s_encoder : &s_default_encoder;
}

// ---------------------------------------------------------------------------
// Stub CameraDriver — paints a slowly-animated placeholder gradient
// into the canvas buffer so the preview visibly updates and the
// freeze-on-capture flow can be exercised end-to-end without any
// camera hardware.
// ---------------------------------------------------------------------------
class StubCameraDriver : public CameraScreen::CameraDriver
{
public:
    esp_err_t StartStreaming(uint8_t *bgr_buf, int w, int h,
                             CameraScreen::FrameReadyCallback on_frame_ready,
                             void *user_data) override
    {
        if (bgr_buf == nullptr || w <= 0 || h <= 0) {
            return ESP_ERR_INVALID_ARG;
        }
        StopStreaming();
        ctx_.buf            = bgr_buf;
        ctx_.w              = w;
        ctx_.h              = h;
        ctx_.on_frame_ready = on_frame_ready;
        ctx_.user_data      = user_data;
        ctx_.running        = true;
        // Render ~8 FPS — fast enough to look alive, slow enough to not
        // hammer the LVGL thread with invalidations.
        BaseType_t ok = xTaskCreate(StubTask, "cam_stub", 4096, this,
                                    tskIDLE_PRIORITY + 2, &ctx_.task);
        if (ok != pdPASS) {
            ctx_.running = false;
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    esp_err_t StopStreaming() override
    {
        if (!ctx_.running) {
            return ESP_OK;
        }
        ctx_.running = false;
        // The stub task checks `running` each iteration and self-deletes.
        // Don't wait for it here — matches the original's non-blocking
        // stop semantics.
        ctx_.task = 0;
        return ESP_OK;
    }

    void SetFrameInterval(int interval) override
    {
        if (interval < 1) interval = 1;
        if (interval > 4) interval = 4;
        ctx_.interval = interval;
    }

private:
    struct Ctx {
        uint8_t *buf = nullptr;
        int w = 0;
        int h = 0;
        CameraScreen::FrameReadyCallback on_frame_ready = nullptr;
        void *user_data = nullptr;
        volatile bool running = false;
        TaskHandle_t task = 0;
        int interval = 2;
        uint32_t frame = 0;
    } ctx_;

    static void StubTask(void *arg) {
        auto *self = static_cast<StubCameraDriver *>(arg);
        Ctx &c = self->ctx_;
        while (c.running) {
            // Paint a moving diagonal gradient so the preview visibly
            // changes from frame to frame. B-G-R byte order to match
            // the original canvas format.
            const uint32_t t = c.frame;
            for (int y = 0; y < c.h; ++y) {
                for (int x = 0; x < c.w; ++x) {
                    uint8_t b = static_cast<uint8_t>((x + t) & 0xFF);
                    uint8_t g = static_cast<uint8_t>((y + (t >> 1)) & 0xFF);
                    uint8_t r = static_cast<uint8_t>((x + y + t) & 0xFF);
                    uint8_t *px = c.buf + (y * c.w + x) * 3;
                    px[0] = b;
                    px[1] = g;
                    px[2] = r;
                }
            }
            const size_t frame_bytes =
                static_cast<size_t>(c.w) * c.h * 3;
            up_flush_dcache(reinterpret_cast<uintptr_t>(c.buf),
                            reinterpret_cast<uintptr_t>(c.buf + frame_bytes));
            if (c.on_frame_ready != nullptr) {
                c.on_frame_ready(c.user_data);
            }
            ++c.frame;
            // ~8 FPS
            vTaskDelayMs(120);
        }
        c.task = 0;
        vTaskDelete(0);
    }
};
StubCameraDriver s_default_driver;

CameraScreen::CameraDriver *ActiveDriver() {
    return (s_driver != nullptr) ? s_driver : &s_default_driver;
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
}

void CollectRootJpgFiles(std::vector<std::string>& out) {
    out.clear();
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }

    DIR* dir = opendir(kPosixSdRoot);
    if (dir == nullptr) {
        return;
    }

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
        out.emplace_back(ent->d_name);
    }
    closedir(dir);

    std::sort(out.begin(), out.end(), std::greater<std::string>());
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
void save_photo_task(void* /*arg*/) {
    const size_t bgr_size = static_cast<size_t>(kSaveW) * kSaveH * 3;
    uint8_t* bgr = static_cast<uint8_t*>(
        std::malloc(bgr_size));
    if (bgr == nullptr) {
        post_status_text(I18n::T("内存不足，保存失败"), 0xFF5555);
        s_save_in_progress = false;
        vTaskDelete(0);
        return;
    }

    DownscaleCanvas(bgr, kSaveW, kSaveH);

    uint8_t* jpeg_data = nullptr;
    size_t jpeg_len = 0;
    const bool ok = ActiveEncoder()->Encode(bgr, bgr_size, kSaveW, kSaveH,
                                            kJpegQuality, &jpeg_data, &jpeg_len);
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

    char path[96];
    const unsigned long ts_ms =
        static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
    std::snprintf(path, sizeof(path), "%s/IMG_%lu.jpg", kPosixSdRoot, ts_ms);

    FILE* file = fopen(path, "wb");
    if (file == nullptr) {
        std::free(jpeg_data);
        post_status_text(I18n::T("写入失败，未保存"), 0xFF5555);
        s_save_in_progress = false;
        vTaskDelete(0);
        return;
    }

    const size_t written = fwrite(jpeg_data, 1, jpeg_len, file);
    fclose(file);
    std::free(jpeg_data);

    if (written != jpeg_len) {
        post_status_text(I18n::T("写入失败，未保存"), 0xFF5555);
    } else {
        ESP_LOGI(TAG, "saved photo %s (%u bytes)", path, static_cast<unsigned>(jpeg_len));
        post_status_text(I18n::T("已保存到 SD 卡"), 0x66FF66);
        schedule_status_clear(2500);
        lv_async_call([](void * /*user_data*/) {
            if (s_view_mode == ViewMode::kGallery) {
                RebuildGallery();
            }
        }, nullptr);
    }
    s_save_in_progress = false;
    vTaskDelete(0);
}

void start_save_photo_task() {
    if (s_save_in_progress) {
        return;
    }
    s_save_in_progress = true;
    post_status_text(I18n::T("正在保存…"), 0xFFFFFF);
    if (xTaskCreate(save_photo_task, "cam_save", 12 * 1024, nullptr,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
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

struct ThumbCtx {
    char lv_path[kMaxNameLen + 16];
    char posix_path[kMaxNameLen + 16];
};

void ClosePhotoViewer() {
    if (s_ui.viewer_panel != nullptr) {
        lv_obj_add_flag(s_ui.viewer_panel, LV_OBJ_FLAG_HIDDEN);
    }
    s_viewer_posix_path[0] = '\0';
    if (s_view_mode == ViewMode::kViewer) {
        s_view_mode = ViewMode::kGallery;
    }
}

void ShowPhotoViewer(const ThumbCtx* ctx) {
    if (s_ui.viewer_panel == nullptr || s_ui.viewer_img == nullptr || ctx == nullptr) {
        return;
    }
    lv_image_set_src(s_ui.viewer_img, ctx->lv_path);
    // strlcpy is available on NuttX libc.
    snprintf(s_viewer_posix_path, sizeof(s_viewer_posix_path), "%s", ctx->posix_path);
    lv_obj_remove_flag(s_ui.viewer_panel, LV_OBJ_FLAG_HIDDEN);
    s_view_mode = ViewMode::kViewer;
}

void on_thumbnail_clicked(lv_event_t* e) {
    auto* ctx = static_cast<ThumbCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    ShowPhotoViewer(ctx);
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
    if (mode == ViewMode::kGallery && !SdCardManager::GetInstance().IsMounted()) {
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

    ApplyGalleryLayout(!show_camera);
    if (mode == ViewMode::kGallery) {
        RaiseGalleryHeader();
        RebuildGallery();
    }
}

void RebuildGallery() {
    if (s_ui.gallery_scroll == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.gallery_scroll);

    if (!SdCardManager::GetInstance().IsMounted()) {
        if (s_ui.gallery_empty != nullptr) {
            lv_label_set_text(s_ui.gallery_empty, I18n::T("请插入 SD 卡"));
            lv_obj_remove_flag(s_ui.gallery_empty, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    std::vector<std::string> files;
    CollectRootJpgFiles(files);
    if (files.empty()) {
        if (s_ui.gallery_empty != nullptr) {
            lv_label_set_text(s_ui.gallery_empty, I18n::T("暂无照片"));
            lv_obj_remove_flag(s_ui.gallery_empty, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (s_ui.gallery_empty != nullptr) {
        lv_obj_add_flag(s_ui.gallery_empty, LV_OBJ_FLAG_HIDDEN);
    }

    for (const auto& name : files) {
        auto* ctx = new ThumbCtx;
        std::snprintf(ctx->lv_path, sizeof(ctx->lv_path), "%s/%s", kLvSdRoot, name.c_str());
        std::snprintf(ctx->posix_path, sizeof(ctx->posix_path), "%s/%s", kPosixSdRoot,
                      name.c_str());

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
        lv_image_set_src(img, ctx->lv_path);

        lv_obj_t* cap = lv_label_create(item);
        lv_label_set_text(cap, name.c_str());
        lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
        lv_obj_set_width(cap, kGalleryThumbW - 8);
        lv_obj_set_style_text_color(cap, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
        lv_obj_set_style_text_font(cap, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

        lv_obj_add_event_cb(item, on_thumbnail_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(
            item,
            [](lv_event_t* ev) { delete static_cast<ThumbCtx*>(lv_event_get_user_data(ev)); },
            LV_EVENT_DELETE, ctx);
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
// lv_async_call and invalidates the canvas. s_photo_frozen gates
// whether the driver's frame-ready callback actually repaints (the
// stub driver still paints into the buffer but the canvas isn't
// invalidated while frozen, so the screen shows the last frame).
// ---------------------------------------------------------------------------
void camera_worker_task(void* /*arg*/) {
    // ----- 0) 排队：等上一轮 worker 把清理做完 -----
    xSemaphoreTake(s_worker_slot, portMAX_DELAY);

    // ----- 1) 期间用户可能已经又切走相机屏，停止信号已经下来了 -----
    if (!s_camera_running) {
        ESP_LOGI(TAG, "worker started but already cancelled, exit");
        xSemaphoreGive(s_worker_slot);
        s_camera_task_handle = 0;
        vTaskDelete(0);
        return;
    }

    // ----- 2) 启动 CameraDriver 流水线（V4L2 驱动内部负责 CAM_PWDN） -----
    esp_err_t err = ActiveDriver()->StartStreaming(
        s_canvas_buf, kCameraAreaW, kCameraAreaH,
        OnFrameReady, nullptr);
    if (err != ESP_OK) {
        post_status_text(I18n::T("摄像头初始化失败"), 0xFF5555);
        goto cleanup_power;
    }

    post_status_clear();

    // ----- 3) 等待停止信号 -----
    // The driver's own task produces frames; here we just wait until
    // s_camera_running goes false. The original did the same — its
    // main loop blocked on VIDIOC_DQBUF and checked s_camera_running
    // each iteration.
    while (s_camera_running) {
        vTaskDelayMs(100);
    }

    // ----- 4) 停止驱动 / 断电 -----
    ActiveDriver()->StopStreaming();

cleanup_power:
    // ----- 5) 让出 worker slot 给下一个 worker，并自我销毁 -----
    s_camera_task_handle = 0;
    xSemaphoreGive(s_worker_slot);
    vTaskDelete(0);
}

esp_err_t start_camera_worker() {
    if (s_worker_slot == nullptr) {
        s_worker_slot = xSemaphoreCreateMutex();
        if (s_worker_slot == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_camera_task_handle != 0) {
        // 上一个 worker 还在跑（用户来回快速切换屏幕）。把 running 重新置 1，
        // 让它继续保持 stream，不再额外起 task。
        s_camera_running = true;
        s_photo_frozen   = false;
        ESP_LOGW(TAG, "camera worker already exists, reuse");
        return ESP_OK;
    }
    s_camera_running = true;
    s_photo_frozen   = false;

    ActiveDriver()->SetFrameInterval(s_frame_interval);

    BaseType_t ok = xTaskCreate(camera_worker_task, "cam_screen",
                                8 * 1024, nullptr,
                                tskIDLE_PRIORITY + 4,
                                &s_camera_task_handle);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create camera worker task failed");
        s_camera_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void stop_camera_worker() {
    // 只发信号，不等待。worker 自己会在主循环里看到 s_camera_running == false
    // 并退出，cleanup 在 worker 自己的 task 上下文中完成；下次 LOAD 时新 worker
    // 会在 take(s_worker_slot) 处自动排队，等待这次的 cleanup 结束。
    s_camera_running = false;
}

// ---------------------------------------------------------------------------
// LVGL 事件 (preserved verbatim)
// ---------------------------------------------------------------------------
void on_capture_btn_clicked(lv_event_t* /*e*/) {
    const bool was_frozen = s_photo_frozen;
    s_photo_frozen = !s_photo_frozen;

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
        if (SdCardManager::GetInstance().IsMounted()) {
            start_save_photo_task();
        } else {
            post_status_text(I18n::T("无 SD 卡，未保存"), 0xFFAA00);
            schedule_status_clear(3000);
        }
    } else if (!s_photo_frozen && was_frozen) {
        post_status_clear();
    }

    ESP_LOGI(TAG, "capture button: %s", s_photo_frozen ? "FROZEN (photo)" : "LIVE preview");
}

void OnSwipeBack() {
    // 让 LVGL 在加载新屏前等当前手指 release，否则按 “返回” 按钮的 PRESSED
    // 事件会被新加载的主屏当成一次新 click —— 落在主屏的 “相机” 图标上时
    // 就会立刻又 LaunchCamera 一次。
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    HomeScreen::SwitchToHome();
}

void on_screen_unloaded(lv_event_t* /*e*/) {
    // 屏幕被切走 -> 抹掉对 LVGL 对象的引用。worker 任务停止由
    // CameraScreen::LifecycleCallback 在同一时机负责，等 stop 完成后
    // 屏幕对象才会被 lv_obj_delete_async 真正释放，所以这里的清理仅是“标记”。
    if (s_status_clear_timer != nullptr) {
        lv_timer_delete(s_status_clear_timer);
        s_status_clear_timer = nullptr;
    }
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
    s_save_in_progress   = false;
    s_viewer_posix_path[0] = '\0';
    s_screen_active      = false;
}

}  // namespace

// ---------------------------------------------------------------------------
// CameraScreen 公共接口
// ---------------------------------------------------------------------------
lv_obj_t* CameraScreen::CreateStatic() {
    // 固定中画质：隔一帧渲染一次，平衡流畅度与画面质量。
    s_frame_interval = 2;
    s_view_mode = ViewMode::kCamera;
    s_photo_frozen = false;
    s_save_in_progress = false;
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
    std::memset(s_canvas_buf, 0, out_size);

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
    lv_obj_clear_flag(camera_panel, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(camera_panel);

    lv_obj_t* canvas = lv_canvas_create(camera_panel);
    s_ui.canvas      = canvas;
    lv_canvas_set_buffer(canvas, s_canvas_buf, kCameraAreaW, kCameraAreaH,
                         LV_COLOR_FORMAT_RGB888);
    lv_obj_set_pos(canvas, 0, 0);
    screen_make_input_passive(canvas);

    lv_obj_t* status = lv_label_create(camera_panel);
    s_ui.status_lbl  = status;
    lv_label_set_text(status, I18n::T("正在启动摄像头…"));
    lv_obj_set_style_text_color(status, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(status);

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
    lv_image_set_inner_align(viewer_img, LV_IMAGE_ALIGN_CENTER);
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

lv_obj_t* CameraScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void CameraScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: camera_screen -> start worker");
        // CAM_PWDN 由 worker 任务在拿到 s_worker_slot 后串行处理。这里如果
        // 直接动 IOExpander，会和上一个 worker 还没完成的 cleanup（也在改
        // CAM_PWDN）撞车。
        s_external_canvas = nullptr;
        if (start_camera_worker() != ESP_OK) {
            ESP_LOGE(TAG, "start_camera_worker failed");
        }
    } else {
        ESP_LOGI(TAG, "unload: camera_screen -> signal worker stop (non-blocking)");
        // 只发停止信号；worker 在自己的 task 里 cleanup（关流、断电）。
        // LVGL 主线程不阻塞，避免 adapter 后台任务长时间拿不到锁。
        stop_camera_worker();
        s_external_canvas = nullptr;
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
    std::memset(s_canvas_buf, 0, out_size);
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
    s_frame_interval  = 2;
    return start_camera_worker();
}

void CameraScreen::StopExternalPreview() {
    stop_camera_worker();
    s_external_canvas = nullptr;
}

void CameraScreen::SetCameraDriver(CameraDriver *driver) {
    s_driver = driver;
}

void CameraScreen::SetJpegEncoder(JpegEncoder *encoder) {
    s_encoder = encoder;
}
