#include "hi_task_policy.h"
#include "notification_sound.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Control center — Xiaomi HyperOS-inspired quick settings.
 * Soft dark glass panel, rounded toggle tiles, brightness / volume sliders.
 */

#include "control_center.h"

#include "application.h"
#include "phone_service.h"
#include "board_shim.h"
#include "metalio_claw_4_board.h"
#include "esp_log_shim.h"
#include "font_awesome.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "network_screen/network_screen.h"
#include "screen_util.h"
#include "settings.h"
#include "idle_power_policy/idle_power_policy.h"
#include "simple_uart.h"

#include <metalio/metalio.h>
#include <arch/board/board.h>

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <sys/ioctl.h>
#include <nuttx/arch.h>
#include <nuttx/timers/pwm.h>
#include <nuttx/video/fb.h>
#include <netutils/netlib.h>

#include "control_center_ring_pcm.inc"

extern "C" int metalio_app_active_data_path(void);

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

/* ---------------------------------------------------------------------------
 * Shared motor pulse helper (used by control-center tiles and PWR_KEY press).
 * Keeps one persistent /dev/pwm1 fd; closing it can black the backlight.
 * --------------------------------------------------------------------------- */

static int s_motor_fd = -1;

static bool MotorFdOpen(void)
{
    if (s_motor_fd >= 0) return true;
    s_motor_fd = open("/dev/pwm1", O_WRONLY);
    if (s_motor_fd < 0) {
        ESP_LOGW("Motor", "open /dev/pwm1 failed: %d", errno);
        return false;
    }
    return true;
}

static bool s_motor_started = false;

static void MotorStart(uint32_t duty_pct)
{
    /* Same frequency as the backlight. A different rate rewrites the
     * shared LEDC clock and the panel flashes for the whole pulse. */
    constexpr uint32_t kMotorFreqHz = BACKLIGHT_FREQ_HZ;
    if (!MotorFdOpen()) return;
    struct pwm_info_s info;
    std::memset(&info, 0, sizeof(info));
    info.frequency = kMotorFreqHz;
    info.duty = static_cast<uint32_t>((duty_pct * 65535u) / 100u);
    info.cpol = PWM_CPOL_NDEF;
    info.dcpol = PWM_DCPOL_NDEF;
    (void)ioctl(s_motor_fd, PWMIOC_SETCHARACTERISTICS, (unsigned long)&info);
    if (!s_motor_started) {
        (void)ioctl(s_motor_fd, PWMIOC_START, 0);
        s_motor_started = true;
    }
}

static void MotorStop()
{
    if (s_motor_fd < 0) return;
    struct pwm_info_s info;
    std::memset(&info, 0, sizeof(info));
    info.frequency = BACKLIGHT_FREQ_HZ;
    info.duty = 0;
    info.cpol = PWM_CPOL_NDEF;
    info.dcpol = PWM_DCPOL_NDEF;
    /* Duty only. Do not START again and do not rewrite backlight PWM —
     * that second ioctl is the flash after every buzz. */
    (void)ioctl(s_motor_fd, PWMIOC_SETCHARACTERISTICS, (unsigned long)&info);
}

static void PulseVibrateOnce(uint32_t pulse_us)
{
    /* Same frequency as backlight timer path; duty-only updates after open. */
    constexpr int kDutyPct = 60;
    MotorStart(kDutyPct);
    usleep(pulse_us);
    MotorStop();
}

static void *VibratePulseWorker(void *arg)
{
    uint32_t pulse_us = 160 * 1000;
    if (arg != nullptr) {
        pulse_us = *static_cast<uint32_t *>(arg);
        delete static_cast<uint32_t *>(arg);
    }
    PulseVibrateOnce(pulse_us);
    return nullptr;
}

void PulseVibrateAsync(uint32_t pulse_us)
{
    uint32_t *value = new uint32_t(pulse_us);
    pthread_t tid{};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4096);
    if (hi_task_create_legacy(&tid, &attr, VibratePulseWorker, value) != 0) {
        ESP_LOGW("Motor", "vibrate pulse spawn failed");
        delete value;
    }
    pthread_attr_destroy(&attr);
}

/* Worker that only performs the trailing stop + backlight restore.  The
 * motor START has already been issued synchronously by the caller so the
 * haptic fires the instant the key edge is detected. */
static void *VibrateStopWorker(void *arg)
{
    uint32_t pulse_us = 160 * 1000;
    if (arg != nullptr) {
        pulse_us = *static_cast<uint32_t *>(arg);
        delete static_cast<uint32_t *>(arg);
    }
    usleep(pulse_us);
    MotorStop();
    return nullptr;
}

/* Fire the haptic NOW (synchronous motor start, no thread-spawn latency) and
 * schedule the trailing stop + backlight restore on a worker.  Used by the
 * PWR_KEY press edge so the buzz is felt the instant the key goes down. */
void PulseVibrateInstant(uint32_t pulse_us)
{
    constexpr int kDutyPct = 60;
    MotorStart(kDutyPct);
    uint32_t *value = new uint32_t(pulse_us);
    pthread_t tid{};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4096);
    if (hi_task_create_legacy(&tid, &attr, VibrateStopWorker, value) != 0) {
        ESP_LOGW("Motor", "vibrate stop spawn failed");
        delete value;
        /* Still stop synchronously so the motor does not stay on. */
        usleep(pulse_us);
        MotorStop();
    }
    pthread_attr_destroy(&attr);
}

namespace {

constexpr const char *TAG = "ControlCenter";
constexpr int kPanel = 720;
/* HyperOS-inspired palette: deeper glassy background, vivid blue accent. */
constexpr uint32_t kShadeBg  = 0x14161C;
constexpr uint32_t kTileOff  = 0x23262F;
constexpr uint32_t kTileOn   = 0x3D7EFF;
constexpr uint32_t kTileWarn = 0xFF8A3D;
constexpr uint32_t kTileGreen = 0x34C759;
constexpr uint32_t kText     = 0xF5F6F8;
constexpr uint32_t kMuted    = 0x8B919A;
constexpr uint32_t kStateOn  = 0xB8C7FF;
constexpr uint32_t kStateOff = 0x6E7480;
constexpr uint32_t kTrack    = 0x2A2E38;

constexpr const char *kFaWifiOn = FONT_AWESOME_WIFI;
constexpr const char *kFaWifiOff = FONT_AWESOME_WIFI;
constexpr const char *kFaBt = FONT_AWESOME_BLUETOOTH;
/* Control-center off states keep the normal icon and use the gray tile. */
constexpr const char *kFa4g = FONT_AWESOME_SIGNAL;
/* Current airplane glyph → location control. */
constexpr const char *kFaLoc = "\xEF\x84\xA4"; /* U+F124 location-arrow */
constexpr const char *kFaBell = FONT_AWESOME_BELL;
constexpr const char *kFaMute = FONT_AWESOME_VOLUME_XMARK;
constexpr const char *kFaGear = FONT_AWESOME_GEAR;
constexpr const char *kFaCam = FONT_AWESOME_CAMERA;

enum class SoundMode { Ring = 0, Vibrate = 1, Mute = 2 };

lv_obj_t *s_root = nullptr;
lv_obj_t *s_panel = nullptr;
lv_obj_t *s_tile_wifi = nullptr;
lv_obj_t *s_tile_bt = nullptr;
lv_obj_t *s_tile_4g = nullptr;
lv_obj_t *s_tile_air = nullptr;
lv_obj_t *s_tile_loc = nullptr;
lv_obj_t *s_tile_sound = nullptr;
lv_obj_t *s_tile_settings = nullptr;
lv_obj_t *s_tile_camera = nullptr;
lv_obj_t *s_bri_icon = nullptr;
lv_obj_t *s_bri_pct = nullptr;
lv_obj_t *s_vol_icon = nullptr;
lv_obj_t *s_vol_pct = nullptr;
bool s_wifi_on = true;
bool s_bt_on = false;
bool s_4g_on = false;
bool s_air_on = false;
bool s_loc_on = false;
bool s_wifi_long_press = false;
SoundMode s_sound = SoundMode::Ring;

lv_image_dsc_t s_plane_dsc = {};
bool s_plane_dsc_ready = false;
bool s_sound_prefs_dirty = false;
bool s_network_prefs_dirty = false;
bool s_shade_persist_running = false;
lv_timer_t *s_save_timer = nullptr;
pthread_mutex_t s_save_mu = PTHREAD_MUTEX_INITIALIZER;
std::atomic<bool> s_final_save{false};
void ScheduleFlashSave();
int s_ring_volume = 70; /* last non-zero ring volume for mode restore */

lv_obj_t *TileIcon(lv_obj_t *tile)
{
    /* The icon is stored in user_data by MakeBigTile / MakeSmallTile. */
    if (tile == nullptr) return nullptr;
    void *ud = lv_obj_get_user_data(tile);
    if (ud != nullptr) return static_cast<lv_obj_t *>(ud);
    /* Fallback for plane tile (image) — first child. */
    return lv_obj_get_child(tile, 0);
}

lv_obj_t *TileStateLabel(lv_obj_t *tile)
{
    /* Big tiles store their state subtitle as the 3rd child (index 2). */
    if (tile == nullptr) return nullptr;
    const uint32_t cnt = lv_obj_get_child_count(tile);
    if (cnt < 3) return nullptr;
    return lv_obj_get_child(tile, 2);
}

void StyleTile(lv_obj_t *tile, bool on, uint32_t on_color = kTileOn)
{
    if (tile == nullptr) return;
    lv_obj_set_style_bg_color(tile, lv_color_hex(on ? on_color : kTileOff),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
}

void SetTileIcon(lv_obj_t *tile, const char *fa)
{
    lv_obj_t *icon = TileIcon(tile);
    if (icon != nullptr && fa != nullptr && lv_obj_check_type(icon, &lv_label_class)) {
        lv_label_set_text(icon, fa);
    }
}

void SetTileState(lv_obj_t *tile, const char *text, bool on)
{
    lv_obj_t *state = TileStateLabel(tile);
    if (state == nullptr || text == nullptr) return;
    lv_label_set_text(state, text);
    lv_obj_set_style_text_color(state, lv_color_hex(on ? kStateOn : kStateOff),
                                LV_PART_MAIN);
}

/* Small tiles have only icon (child 0) + label (child 1). Update the label. */
void SetSmallTileLabel(lv_obj_t *tile, const char *text)
{
    if (tile == nullptr || text == nullptr) return;
    const uint32_t cnt = lv_obj_get_child_count(tile);
    if (cnt < 2) return;
    lv_obj_t *lbl = lv_obj_get_child(tile, 1);
    if (lbl != nullptr && lv_obj_check_type(lbl, &lv_label_class)) {
        lv_label_set_text(lbl, text);
    }
}

/* Android quick-settings airplane: Material "flight" silhouette rotated so
 * the nose points northeast.  Filled once into a static ARGB8888 buffer. */
constexpr int kPlaneN = 48;
static uint8_t s_plane_px[kPlaneN * kPlaneN * 4];

bool PlaneInside(float x, float y, const float *poly, int n)
{
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float xi = poly[i * 2];
        const float yi = poly[i * 2 + 1];
        const float xj = poly[j * 2];
        const float yj = poly[j * 2 + 1];
        if ((yi > y) != (yj > y) &&
            x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
            inside = !inside;
        }
    }
    return inside;
}

void EnsurePlaneDsc()
{
    if (s_plane_dsc_ready) return;
    std::memset(s_plane_px, 0, sizeof(s_plane_px));

    /* Material flight outline in a 24px box, nose at the top. */
    float poly[32 * 2];
    int n = 0;
    auto add = [&](float x, float y) {
        poly[n * 2] = x;
        poly[n * 2 + 1] = y;
        ++n;
    };
    add(21.f, 16.f);
    add(21.f, 14.f);
    add(13.f, 9.f);
    add(13.f, 3.5f);
    for (int i = 1; i < 8; ++i) {
        const float a = 3.14159265f * static_cast<float>(i) / 8.f;
        add(11.5f + 1.5f * std::cos(a), 3.5f - 1.5f * std::sin(a));
    }
    add(10.f, 3.5f);
    add(10.f, 9.f);
    add(2.f, 14.f);
    add(2.f, 16.f);
    add(10.f, 13.5f);
    add(10.f, 19.f);
    add(8.f, 20.5f);
    add(8.f, 22.f);
    add(11.5f, 21.f);
    add(15.f, 22.f);
    add(15.f, 20.5f);
    add(13.f, 19.f);
    add(13.f, 13.5f);

    /* Clockwise 45° on screen turns the nose from north to northeast. */
    constexpr float kCos = 0.70710678f;
    constexpr float kScale = 2.0f;
    const float cx = (kPlaneN - 1) * 0.5f;
    for (int py = 0; py < kPlaneN; ++py) {
        for (int px = 0; px < kPlaneN; ++px) {
            int cover = 0;
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 2; ++sx) {
                    const float fx = static_cast<float>(px) + (sx + 0.5f) * 0.5f;
                    const float fy = static_cast<float>(py) + (sy + 0.5f) * 0.5f;
                    const float dxs = (fx - cx) / kScale;
                    const float dys = (fy - cx) / kScale;
                    const float ix = 12.f + dxs * kCos + dys * kCos;
                    const float iy = 12.f - dxs * kCos + dys * kCos;
                    if (PlaneInside(ix, iy, poly, n)) ++cover;
                }
            }
            if (cover == 0) continue;
            uint8_t *p = s_plane_px + (py * kPlaneN + px) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<uint8_t>(cover * 255 / 4);
        }
    }

    s_plane_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_plane_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s_plane_dsc.header.w = kPlaneN;
    s_plane_dsc.header.h = kPlaneN;
    s_plane_dsc.header.stride = kPlaneN * 4;
    s_plane_dsc.data_size = sizeof(s_plane_px);
    s_plane_dsc.data = s_plane_px;
    s_plane_dsc_ready = true;
}

/* Big connectivity tile (310×120): icon left, title + state subtitle right.
 * Stores the icon in user_data so TileIcon() can find it after creation. */
lv_obj_t *MakeBigTile(lv_obj_t *parent, const char *fa_icon, const char *label,
                      lv_event_cb_t on_click, lv_event_cb_t on_long = nullptr)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 310, 120);
    lv_obj_set_style_radius(tile, 36, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tile, lv_color_hex(kTileOff), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(tile, true);

    lv_obj_t *icon = lv_label_create(tile);
    lv_label_set_text(icon, fa_icon);
    lv_obj_set_style_text_font(icon, &font_awesome_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 28, -8);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(tile, icon);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 78, 30);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *state = lv_label_create(tile);
    lv_label_set_text(state, "");
    lv_obj_set_style_text_font(state, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(state, lv_color_hex(kStateOff), LV_PART_MAIN);
    lv_obj_align(state, LV_ALIGN_TOP_LEFT, 78, 68);
    lv_obj_remove_flag(state, LV_OBJ_FLAG_CLICKABLE);

    if (on_click != nullptr) {
        lv_obj_add_event_cb(tile, on_click, LV_EVENT_CLICKED, nullptr);
    }
    if (on_long != nullptr) {
        lv_obj_add_event_cb(tile, on_long, LV_EVENT_LONG_PRESSED, nullptr);
    }
    return tile;
}

/* Small shortcut tile (148×80): icon top-left, label bottom-left. */
lv_obj_t *MakeSmallTile(lv_obj_t *parent, const char *fa_icon, const char *label,
                        lv_event_cb_t on_click, lv_event_cb_t on_long = nullptr)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 148, 80);
    lv_obj_set_style_radius(tile, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tile, lv_color_hex(kTileOff), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(tile, true);

    lv_obj_t *icon = lv_label_create(tile);
    lv_label_set_text(icon, fa_icon);
    lv_obj_set_style_text_font(icon, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 14, 14);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(tile, icon);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 14, -12);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    if (on_click != nullptr) {
        lv_obj_add_event_cb(tile, on_click, LV_EVENT_CLICKED, nullptr);
    }
    if (on_long != nullptr) {
        lv_obj_add_event_cb(tile, on_long, LV_EVENT_LONG_PRESSED, nullptr);
    }
    return tile;
}

/* Plane (airplane) big tile uses an embedded bitmap instead of a font glyph. */
lv_obj_t *MakePlaneTile(lv_obj_t *parent, const char *label, lv_event_cb_t on_click)
{
    EnsurePlaneDsc();
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 310, 120);
    lv_obj_set_style_radius(tile, 36, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tile, lv_color_hex(kTileOff), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(tile, true);

    lv_obj_t *icon = lv_image_create(tile);
    lv_image_set_src(icon, &s_plane_dsc);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 26, -8);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(tile, icon);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 78, 30);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *state = lv_label_create(tile);
    lv_label_set_text(state, "");
    lv_obj_set_style_text_font(state, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(state, lv_color_hex(kStateOff), LV_PART_MAIN);
    lv_obj_align(state, LV_ALIGN_TOP_LEFT, 78, 68);
    lv_obj_remove_flag(state, LV_OBJ_FLAG_CLICKABLE);

    if (on_click != nullptr) {
        lv_obj_add_event_cb(tile, on_click, LV_EVENT_CLICKED, nullptr);
    }
    return tile;
}

void WifiReconnectSaved()
{
    /* Mirror NetworkService / Claw4 WifiStation::Start for CC wifi-on. */
    Settings wifi("wifi", false);
    const std::string ssid = wifi.GetString("ssid");
    const std::string pass = wifi.GetString("password");

    int ret = metalio_esp_hosted_ensure_wifi_ready();
    if (ret < 0) {
        ESP_LOGW(TAG, "wifi ensure ready ret=%d", ret);
        return;
    }
    if (ssid.empty()) {
        ESP_LOGI(TAG, "wifi on: radio up, no saved ssid");
        return;
    }
    if (metalio_esp_hosted_is_connected()) {
        ESP_LOGI(TAG, "wifi on: already associated");
        return;
    }

    ret = metalio_esp_hosted_connect(
        ssid.c_str(), pass.empty() ? nullptr : pass.c_str());
    ESP_LOGI(TAG, "wifi reconnect '%s' ret=%d", ssid.c_str(), ret);
    if (ret < 0) return;

    for (int i = 0; i < 100; i++) {
        if (metalio_esp_hosted_is_connected()) break;
        usleep(100 * 1000);
    }
    if (!metalio_esp_hosted_is_connected()) {
        ESP_LOGW(TAG, "wifi reconnect associate timeout");
        return;
    }
    if (netlib_ifup("eth0") < 0) return;
    (void)netlib_obtain_ipv4addr("eth0");
}

pthread_mutex_t s_radio_kick_mu = PTHREAD_MUTEX_INITIALIZER;
bool s_radio_worker_running = false;
bool s_radio_retrigger = false;

bool s_hw_known = false;
bool s_hw_wifi = false;
bool s_hw_bt = false;
bool s_hw_4g = false;

void RefreshRadioTiles();
bool s_radio_pending = false;
int s_radio_error = 0;
uint32_t s_radio_generation = 0;

void ApplyRadiosToHardware()
{
    lv_lock();
    const auto generation = s_radio_generation;
    const bool air = s_air_on;
    const bool want_wifi = s_wifi_on && !air;
    const bool want_bt = s_bt_on && !air;
    const bool want_4g = s_4g_on && !air;
    const int active_path = metalio_dual_network_path();
    const int wanted_path = want_4g ? 1 : (want_wifi ? 0 : -1);
    const bool network_changed = !s_hw_known || active_path != wanted_path ||
                                 want_wifi != s_hw_wifi || want_4g != s_hw_4g;
    lv_unlock();

    int bt_ret = metalio_bt_power(want_bt);
    int net_ret = network_changed ? metalio_app_apply_radios(want_wifi, want_4g) : 0;
    const bool actual_bt = metalio_bt_is_powered();
    const int actual_path = metalio_dual_network_path();
    const bool actual_4g = actual_path == 1;

    lv_lock();
    s_hw_bt = actual_bt;
    if (net_ret == 0) {
        s_hw_wifi = want_wifi;
        s_hw_4g = want_4g;
    }
    s_hw_known = net_ret == 0 && bt_ret == 0;
    if (generation == s_radio_generation) {
        s_radio_pending = false;
        s_radio_error = bt_ret < 0 ? bt_ret : net_ret;
        if (!air) {
            s_bt_on = actual_bt;
            if (net_ret < 0) {
                s_4g_on = actual_4g;
                s_wifi_on = actual_path == 0;
            }
        }
        s_network_prefs_dirty = true;
        RefreshRadioTiles();
        ScheduleFlashSave();
    }
    lv_unlock();
    ESP_LOGI(TAG, "radio apply wifi=%d 4g=%d bt=%d network=%d bluetooth=%d",
             want_wifi, want_4g, want_bt, net_ret, bt_ret);
}

void *RadioApplyWorker(void * /*arg*/)
{
    for (;;) {
        ApplyRadiosToHardware();

        pthread_mutex_lock(&s_radio_kick_mu);
        if (!s_radio_retrigger) {
            s_radio_worker_running = false;
            pthread_mutex_unlock(&s_radio_kick_mu);
            break;
        }
        s_radio_retrigger = false;
        pthread_mutex_unlock(&s_radio_kick_mu);
    }
    return nullptr;
}

void KickRadioApply()
{
    // Persist only after the hardware transaction completes. A pending target
    // must not become the next boot's configuration if switching fails.
    ++s_radio_generation;
    s_radio_pending = true;
    s_radio_error = 0;
    RefreshRadioTiles();
    pthread_mutex_lock(&s_radio_kick_mu);
    if (s_radio_worker_running) {
        s_radio_retrigger = true;
        pthread_mutex_unlock(&s_radio_kick_mu);
        return;
    }
    s_radio_worker_running = true;
    s_radio_retrigger = false;
    pthread_mutex_unlock(&s_radio_kick_mu);

    pthread_t tid{};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 8192);
    const int spawn_ret = hi_task_create_legacy(&tid, &attr, RadioApplyWorker, nullptr);
    if (spawn_ret != 0) {
        s_radio_pending = false;
        s_radio_error = -spawn_ret;
        RefreshRadioTiles();
        pthread_mutex_lock(&s_radio_kick_mu);
        s_radio_worker_running = false;
        pthread_mutex_unlock(&s_radio_kick_mu);
        ESP_LOGW(TAG, "radio apply worker spawn failed");
    }
    pthread_attr_destroy(&attr);
}

/* Coalesce changes off the LVGL thread, including while the shade stays open. */
void PersistSoundUi()
{
    NotificationSound::SetSystemMode(static_cast<int>(s_sound));
    s_sound_prefs_dirty = true;
    ScheduleFlashSave();
}

int s_bri_save = -1;
void NoteBrightness(int brightness)
{
    s_bri_save = brightness;
    ScheduleFlashSave();
}

void UpdateVolUi(int pct); /* defined below */

/* Label / tile updates invalidate, and every invalidate ends in a
 * full-frame cache writeback plus FBIOPAN.  That pan on finger-up is the
 * release blue screen.  Apply the widget state with invalidation
 * held off; the pixels already on screen stay until a later refresh. */
struct InvalidationPause {
    lv_display_t *disp;
    explicit InvalidationPause(lv_display_t *d) : disp(d)
    {
        if (disp != nullptr) lv_display_enable_invalidation(disp, false);
    }
    ~InvalidationPause()
    {
        if (disp != nullptr) lv_display_enable_invalidation(disp, true);
    }
};

/* Brightness / volume bars (条).
 *
 * Any lv_obj_invalidate() while a finger is down ends in flush_cb(),
 * which cache-writebacks a full 720x720 frame and FBIOPAN-switches the
 * triple buffer.  Doing that on every PRESSING sample starves the MIPI
 * scan and the panel drops to solid blue.
 *
 * While dragging, the bar is painted straight into all three framebuffers
 * and only those rows are cache-flushed.  LVGL is not asked to refresh.
 * A normal redraw (open animation, release, external refresh) still goes
 * through DRAW_POST, which is a single radius-0 fill inside a refresh
 * that was already going to happen.
 */
struct Bar {
    lv_obj_t *track = nullptr;
    lv_obj_t *label = nullptr;   /* pct label, painted live during drag */
    int lo = 0, hi = 100, val = 0;
    int painted_x = -1;
    int painted_val = -1;
};
Bar s_bri;
Bar s_vol;

constexpr int kBarH = 36;
constexpr int kStepRowH = 64;  /* HyperOS-style card height */

struct FbPages {
    uint8_t *base = nullptr;
    uint32_t stride = 0;
    uint32_t page = 0;
    uint32_t yres = 0;
    int pages = 0;
    int fd = -1;
    bool ready = false;
};
FbPages s_fb;

bool FbReady()
{
    if (s_fb.ready) return true;
    s_fb.fd = open("/dev/fb0", O_RDWR);
    if (s_fb.fd < 0) return false;
    struct fb_videoinfo_s vinfo;
    struct fb_planeinfo_s pinfo;
    memset(&vinfo, 0, sizeof(vinfo));
    memset(&pinfo, 0, sizeof(pinfo));
    pinfo.display = 0;
    if (ioctl(s_fb.fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0 ||
        ioctl(s_fb.fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0 ||
        pinfo.fbmem == nullptr || pinfo.stride < 720 * 3 || vinfo.yres < 1) {
        close(s_fb.fd);
        s_fb.fd = -1;
        return false;
    }
    s_fb.base = static_cast<uint8_t *>(pinfo.fbmem);
    s_fb.stride = pinfo.stride;
    s_fb.yres = vinfo.yres;
    s_fb.page = pinfo.stride * vinfo.yres;
    s_fb.pages = (s_fb.page > 0 && pinfo.fblen >= s_fb.page * 3) ? 3 : 1;
    s_fb.ready = true;
    return true;
}

/* DMA scans one page. Cache-writeback of the other two just fights it. */
int VisibleFbPage()
{
    if (!s_fb.ready || s_fb.yres == 0) return 0;
    struct fb_planeinfo_s pinfo;
    memset(&pinfo, 0, sizeof(pinfo));
    pinfo.display = 0;
    if (ioctl(s_fb.fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0) return 0;
    const int p = static_cast<int>(pinfo.yoffset / s_fb.yres);
    if (p < 0 || p >= s_fb.pages) return 0;
    return p;
}

int BarFillX(const Bar &b, const lv_area_t &a)
{
    const int w = a.x2 - a.x1 + 1;
    const int range = b.hi - b.lo;
    if (w <= 1 || range <= 0 || b.val <= b.lo) return a.x1;
    if (b.val >= b.hi) return a.x2 + 1;
    return a.x1 + (w * (b.val - b.lo)) / range;
}

/* Horizontal inset (px) for one row of a pill so the direct-fb fill respects
 * the rounded corners.  row is 0..h-1 within the bar; r is the corner radius.
 * Returns the number of px to clip from each side at that row. */
int PillRowInset(int row, int h, int r)
{
    if (r <= 0 || h <= 0) return 0;
    if (r > h / 2) r = h / 2;
    int dy;  /* distance from the nearest horizontal center line */
    int mid = h / 2;
    if (row < mid) dy = mid - row;
    else if (row > mid) dy = row - mid;
    else dy = 0;
    if (dy >= r) return 0;
    const int d2 = r * r - dy * dy;
    if (d2 <= 0) return r;
    return r - static_cast<int>(sqrtf(static_cast<float>(d2)));
}

int BarPctFromX(const Bar &b, int x)
{
    if (b.track == nullptr) return 0;
    lv_area_t a;
    lv_obj_get_coords(b.track, &a);
    const int w = a.x2 - a.x1 + 1;
    if (w <= 0) return 0;
    int pct = (x - a.x1) * 100 / w;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* Blend one A4 (0..15) alpha pixel onto an RGB888 dst in-place. */
inline void FbBlendA4(uint8_t *dst, uint32_t bg, uint32_t fg, int a4)
{
    if (a4 <= 0) {
        dst[0] = static_cast<uint8_t>(bg & 0xFF);
        dst[1] = static_cast<uint8_t>((bg >> 8) & 0xFF);
        dst[2] = static_cast<uint8_t>((bg >> 16) & 0xFF);
        return;
    }
    const int a = a4 * 17;
    const int br = bg & 0xFF, bgc = (bg >> 8) & 0xFF, bb = (bg >> 16) & 0xFF;
    const int fr = fg & 0xFF, fgc = (fg >> 8) & 0xFF, fb = (fg >> 16) & 0xFF;
    dst[0] = static_cast<uint8_t>(br + (fr - br) * a / 255);
    dst[1] = static_cast<uint8_t>(bgc + (fgc - bgc) * a / 255);
    dst[2] = static_cast<uint8_t>(bb + (fb - bb) * a / 255);
}

void FbBlitGlyphA4(uint8_t *page, uint32_t stride, int x0, int y0,
                   const uint8_t *bmp, int gw, int gh, int gstride,
                   uint32_t bg, uint32_t fg)
{
    for (int gy = 0; gy < gh; ++gy) {
        const int dy = y0 + gy;
        if (dy < 0 || dy > 719) continue;
        for (int gx = 0; gx < gw; ++gx) {
            const int dx = x0 + gx;
            if (dx < 0 || dx > 719) continue;
            const uint8_t byte = bmp[gy * gstride + (gx >> 1)];
            const int a4 = (gx & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
            uint8_t *px = page + static_cast<size_t>(dy) * stride +
                          static_cast<size_t>(dx) * 3;
            FbBlendA4(px, bg, fg, a4);
        }
    }
}

void FbPaintPctLabel(Bar &b, uint8_t *page, uint32_t stride,
                     int *ly1, int *ly2)
{
    *ly1 = -1; *ly2 = -1;
    if (b.label == nullptr) return;
    lv_area_t la;
    lv_obj_get_coords(b.label, &la);
    if (la.x2 < la.x1 || la.y2 < la.y1) return;
    const lv_font_t *font = lv_obj_get_style_text_font(b.label, LV_PART_MAIN);
    if (font == nullptr) return;
    const int box_h = static_cast<int>(la.y2) - static_cast<int>(la.y1);
    if (box_h < 0 || box_h > 80) return;
    for (int y = la.y1; y <= la.y2; ++y) {
        if (y < 0 || y > 719) continue;
        const int xa = static_cast<int>(la.x1) < 0 ? 0 : static_cast<int>(la.x1);
        uint8_t *p = page + static_cast<size_t>(y) * stride +
                     static_cast<size_t>(xa) * 3;
        for (int x = xa; x <= static_cast<int>(la.x2) && x <= 719; ++x) {
            p[0] = static_cast<uint8_t>(kShadeBg & 0xFF);
            p[1] = static_cast<uint8_t>((kShadeBg >> 8) & 0xFF);
            p[2] = static_cast<uint8_t>((kShadeBg >> 16) & 0xFF);
            p += 3;
        }
        up_udelay(20);
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", b.val);
    int total_w = 0;
    for (int i = 0; buf[i]; ++i) {
        lv_font_glyph_dsc_t dsc;
        if (lv_font_get_glyph_dsc(font, &dsc, (uint8_t)buf[i],
                                  (uint8_t)buf[i + 1])) {
            total_w += dsc.adv_w;
        }
    }
    const int lw = la.x2 - la.x1 + 1;
    const int lh = la.y2 - la.y1 + 1;
    int pos_x = la.x1 + (lw - total_w) / 2;
    int pos_y = la.y1 + (lh - font->line_height) / 2;
    if (pos_y < 0) pos_y = 0;
    const int baseline = pos_y + (font->line_height - font->base_line);
    for (int i = 0; buf[i]; ++i) {
        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(font, &dsc, (uint8_t)buf[i],
                                   (uint8_t)buf[i + 1])) {
            continue;
        }
        dsc.req_raw_bitmap = 1;
        const uint8_t *bmp = static_cast<const uint8_t *>(
            lv_font_get_glyph_static_bitmap(&dsc));
        if (bmp != nullptr) {
            const int gx = pos_x + dsc.ofs_x;
            const int gy = baseline - static_cast<int>(dsc.box_h) - dsc.ofs_y;
            const int gstride = dsc.stride != 0 ? dsc.stride
                                : (dsc.box_w + 1) / 2;
            FbBlitGlyphA4(page, stride, gx, gy, bmp, dsc.box_w, dsc.box_h,
                          gstride, kShadeBg, kText);
        }
        pos_x += dsc.adv_w;
    }
    *ly1 = static_cast<int>(la.y1) < 0 ? 0 : static_cast<int>(la.y1);
    *ly2 = static_cast<int>(la.y2) > 719 ? 719 : static_cast<int>(la.y2);
}

/* The bar is copied onto the page the scan is reading. A whole row is
 * about 1.5 KB and thirty-six rows back to back hold the PSRAM bus longer
 * than the panel FIFO. Same 512-byte gap as the partial flush. */
void FbCopyYield(uint8_t *dst, const uint8_t *src, size_t n)
{
    while (n > 0) {
        const size_t chunk = n > 512 ? 512 : n;
        memcpy(dst, src, chunk);
        dst += chunk;
        src += chunk;
        n -= chunk;
        up_udelay(20);
    }
}

void BarPaintFb(Bar &b, bool force)
{
    if (b.track == nullptr || !FbReady()) return;
    lv_area_t a;
    lv_obj_get_coords(b.track, &a);
    if (a.x2 < a.x1 || a.y2 < a.y1) return;
    if (a.y2 < 0 || a.y1 > 719 || a.x2 < 0 || a.x1 > 719) return;
    const int fill_x = BarFillX(b, a);
    const bool bar_changed = (fill_x != b.painted_x);
    const bool val_changed = (b.val != b.painted_val);
    if (!force && !bar_changed && !val_changed) return;
    /* A rounded fill rewrites every bar row into all 3 pages.  Doing that
     * on every PRESSING sample starves the DSI DMA (blue).  30 Hz is enough
     * for the fill and the number to track the finger. */
    static uint32_t s_last_paint_ms = 0;
    const uint32_t now_ms = lv_tick_get();
    if (!force && s_last_paint_ms != 0 && now_ms - s_last_paint_ms < 32) return;
    s_last_paint_ms = now_ms;
    b.painted_x = fill_x;
    b.painted_val = b.val;

    int x1 = a.x1 < 0 ? 0 : a.x1;
    int y1 = a.y1 < 0 ? 0 : a.y1;
    int x2 = a.x2 > 719 ? 719 : a.x2;
    int y2 = a.y2 > 719 ? 719 : a.y2;
    if (x2 < x1 || y2 < y1) return;
    int fx = fill_x;
    if (fx < x1) fx = x1;
    if (fx > x2 + 1) fx = x2 + 1;

    const int w = x2 - x1 + 1;
    if (w > 720) return;
    const int h = y2 - y1 + 1;
    const int r = kBarH / 2;
    const int vis = VisibleFbPage();
    uint8_t *page = s_fb.base + static_cast<size_t>(vis) * s_fb.page;
    uint8_t row[720 * 3];
    for (int y = y1; y <= y2; ++y) {
        const int inset = PillRowInset(y - y1, h, r);
        const int lx = x1 + inset;
        const int rx = x2 - inset;
        uint8_t *rp = row;
        for (int x = x1; x <= x2; ++x) {
            uint32_t rgb;
            if (x < lx || x > rx) {
                rgb = kShadeBg;            /* rounded corner: card bg */
            } else if (x < fx) {
                rgb = kTileOn;             /* filled portion */
            } else {
                rgb = kTrack;              /* empty track */
            }
            *rp++ = static_cast<uint8_t>(rgb & 0xFF);
            *rp++ = static_cast<uint8_t>((rgb >> 8) & 0xFF);
            *rp++ = static_cast<uint8_t>((rgb >> 16) & 0xFF);
        }
        const size_t nbytes = static_cast<size_t>(w) * 3;
        FbCopyYield(page + static_cast<size_t>(y) * s_fb.stride +
                        static_cast<size_t>(x1) * 3,
                    row, nbytes);
    }

    int ly1 = -1, ly2 = -1;
    if (val_changed || force) {
        FbPaintPctLabel(b, page, s_fb.stride, &ly1, &ly2);
    }

    /* Bar and number are separate. One box around both also writebacks the
     * cards that sit in the gap, which is what tears those tiles. */
    struct fb_area_s area;
    memset(&area, 0, sizeof(area));
    area.x = static_cast<uint16_t>(x1);
    area.y = static_cast<uint16_t>(vis * static_cast<int>(s_fb.yres) + y1);
    area.w = static_cast<uint16_t>(w);
    area.h = static_cast<uint16_t>(h);
    ioctl(s_fb.fd, FBIO_UPDATE, (unsigned long)&area);
    if (ly1 >= 0 && b.label != nullptr) {
        lv_area_t la;
        lv_obj_get_coords(b.label, &la);
        int lx1 = static_cast<int>(la.x1);
        int lx2 = static_cast<int>(la.x2);
        if (lx1 < 0) lx1 = 0;
        if (ly1 < 0) ly1 = 0;
        if (lx2 > 719) lx2 = 719;
        if (ly2 > 719) ly2 = 719;
        if (lx2 >= lx1 && ly2 >= ly1) {
            area.x = static_cast<uint16_t>(lx1);
            area.y = static_cast<uint16_t>(
                vis * static_cast<int>(s_fb.yres) + ly1);
            area.w = static_cast<uint16_t>(lx2 - lx1 + 1);
            area.h = static_cast<uint16_t>(ly2 - ly1 + 1);
            ioctl(s_fb.fd, FBIO_UPDATE, (unsigned long)&area);
        }
    }
}

void BarStore(Bar &b, int v)
{
    if (v < b.lo) v = b.lo;
    if (v > b.hi) v = b.hi;
    b.val = v;
}

/* Finger is down: pixels only.  No invalidate, no flush_cb, no pan. */
void BarLive(Bar &b, int v)
{
    BarStore(b, v);
    BarPaintFb(b, false);
}

/* Panel is idle: one invalidate so the next refresh matches the value. */
void BarSync(Bar &b, int v)
{
    if (v < b.lo) v = b.lo;
    if (v > b.hi) v = b.hi;
    if (b.val == v) return;
    b.val = v;
    b.painted_x = -1;
    b.painted_val = -1;
    if (b.track != nullptr) lv_obj_invalidate(b.track);
}

void SyncVolSlider(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    BarSync(s_vol, pct);
    UpdateVolUi(pct);
}

/* Map sound mode → volume bar + codec. Never persist 0 as ring_volume. */
std::atomic<bool> s_ring_preview_busy{false};
std::atomic<bool> s_ring_play_again{false};

void *RingPreviewWorker(void * /*arg*/)
{
    auto &audio = Application::GetInstance().GetAudioService();
    audio.MuteWakeWordForMs(3500);
    for (;;) {
        lv_lock();
        bool ring_enabled = s_sound == SoundMode::Ring;
        int volume = std::max(1, std::min(100, s_ring_volume));
        s_ring_play_again = false;
        lv_unlock();
        if (ring_enabled) {
            // Shared transaction is invalidated by Bluetooth power/music
            // changes; a per-panel one-shot cache cannot track these.
            if (metalio_bt_ensure_mode1() < 0)
                ESP_LOGW("ControlCenter", "Ring preview: audio mode not acknowledged");
            metalio_tca9555_write_pin(BOARD_IOEXP_PA_ENABLE, true);
            // Initialization may take time. Respect a newer mute/vibrate tap.
            lv_lock();
            ring_enabled = s_sound == SoundMode::Ring;
            volume = std::max(1, std::min(100, s_ring_volume));
            audio.SetVolume(ring_enabled ? volume : 0, false);
            lv_unlock();
            if (ring_enabled)
                audio.PlayLocalPcm(kControlCenterRingPcm, kControlCenterRingPcmSamples);
        } else {
            audio.SetVolume(0, false);
        }
        // Serialize completion with the LVGL click handler so a final tap
        // cannot be lost between checking the pending flag and clearing busy.
        lv_lock();
        const bool again = s_ring_play_again.exchange(false);
        if (!again) s_ring_preview_busy = false;
        lv_unlock();
        if (!again) break;
    }
    return nullptr;
}

void StartRingPreviewWorker(void *arg)
{
    pthread_t tid{};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 48 * 1024);
    if (hi_task_create_legacy(&tid, &attr, RingPreviewWorker, arg) != 0) {
        s_ring_preview_busy = false;
    }
    pthread_attr_destroy(&attr);
}

/* Mute ioctl on the LVGL thread races LCDC (solid blue). Apply once
 * after the finger is up, never on PRESSING. */
volatile int s_vol_apply_pct = 70;
volatile bool s_vol_apply_busy = false;
volatile bool s_vol_apply_again = false;

void *VolumeApplyWorker(void * /*arg*/)
{
    auto &audio = Application::GetInstance().GetAudioService();
    for (;;) {
        int pct = s_vol_apply_pct;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        /* Software gain only. AUDIOIOC mute/volume on this port
         * underruns MIPI (solid blue) even off the LVGL thread. */
        audio.SetVolume(pct, false);
        if (!s_vol_apply_again)
            break;
        s_vol_apply_again = false;
    }
    s_vol_apply_busy = false;
    return nullptr;
}

void KickVolumeApply(int pct)
{
    s_vol_apply_pct = pct;
    if (s_vol_apply_busy) {
        s_vol_apply_again = true;
        return;
    }
    s_vol_apply_busy = true;
    s_vol_apply_again = false;
    pthread_t tid{};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 8192);
    if (hi_task_create_legacy(&tid, &attr, VolumeApplyWorker, nullptr) != 0)
        s_vol_apply_busy = false;
    pthread_attr_destroy(&attr);
}

void OnRingPreviewTimer(lv_timer_t * /*timer*/)
{
    /* repeat_count==1 already auto-deletes this timer. lv_timer_delete
     * here double-freed the timer and solid-blued after switching to 响铃. */
    if (!s_ring_preview_busy)
        return;
    StartRingPreviewWorker(nullptr);
}

void ApplySoundMode(bool play_preview, bool sync_bar = true)
{
    if (s_sound == SoundMode::Ring) {
        int v = s_ring_volume;
        if (v < 1) v = 70;
        if (sync_bar) SyncVolSlider(v);
        // This setter only changes software gain. Restore it immediately,
        // including when opening the panel without requesting a preview.
        Application::GetInstance().GetAudioService().SetVolume(v, false);
        if (play_preview && !s_ring_preview_busy) {
            s_ring_preview_busy = true;
            s_ring_play_again = false;
            lv_timer_t *t = lv_timer_create(OnRingPreviewTimer, 420, nullptr);
            if (t != nullptr)
                lv_timer_set_repeat_count(t, 1);
            else
                StartRingPreviewWorker(nullptr);
        } else if (play_preview && s_ring_preview_busy) {
            s_ring_play_again = true;
        }
    } else {
        /* Software gain takes effect immediately even while a preview is
         * queued. Leave the codec hardware running and keep ring_volume. */
        if (sync_bar) SyncVolSlider(0);
        Application::GetInstance().GetAudioService().SetVolume(0, false);
        if (s_ring_preview_busy) {
            s_ring_play_again = true;
        }
    }
}

/* Persistent motor fd — NEVER close(/dev/pwm1).  esp_ledc pwm_shutdown()
 * frees the shared p_ledc_obj and disables the LEDC bus clock, which
 * blacks the backlight on /dev/pwm0 (same peripheral). */
void UpdateSoundTile()
{
    if (s_tile_sound == nullptr) return;
    const char *ic = kFaBell;
    const char *st = I18n::T("响铃");
    uint32_t col = kTileOn;
    if (s_sound == SoundMode::Vibrate) {
        ic = FONT_AWESOME_PHONE;
        st = I18n::T("震动");
        col = kTileWarn;
    } else if (s_sound == SoundMode::Mute) {
        ic = kFaMute;
        st = I18n::T("静音");
        col = kTileOff;
    }
    SetTileIcon(s_tile_sound, ic);
    StyleTile(s_tile_sound, s_sound != SoundMode::Mute, col);
    SetSmallTileLabel(s_tile_sound, st);
}

/* Style changes invalidate the tile. Paint it now so LVGL cannot join
 * every tile into one tall rect (that rect pans a full frame: slow, and
 * the panel flashes). */
void PaintTileNow(lv_obj_t *tile)
{
    if (tile == nullptr) return;
    lv_obj_invalidate(tile);
    if (lv_display_t *d = lv_display_get_default()) {
        lv_refr_now(d);
    }
}

bool s_paint_each_tile = false;

// Present confirmed hardware state. Desired state remains separate so rapid
// taps are coalesced by the existing worker without optimistic success UI.
void StyleRadioTiles()
{
    const bool wifi = metalio_esp_hosted_radio_enabled();
    const bool cellular_rf = metalio_nt26_radio_enabled();
    // The 4G tile selects cellular DATA. WiFi may keep RF registered solely
    // for incoming calls/SMS without selecting the cellular data route.
    const bool cell = cellular_rf && metalio_dual_network_path() == 1;
    const bool bt = metalio_bt_is_powered();
    const bool air = s_air_on && !wifi && !cellular_rf && !bt;
    if (s_tile_wifi) {
        StyleTile(s_tile_wifi, wifi);
        SetTileIcon(s_tile_wifi, kFaWifiOn);
        SetTileState(s_tile_wifi, wifi ? (metalio_esp_hosted_is_connected() ?
                     I18n::T("已连接") : I18n::T("已开启")) : I18n::T("关闭"), wifi);
    }
    if (s_tile_bt) {
        StyleTile(s_tile_bt, bt);
        SetTileIcon(s_tile_bt, kFaBt);
        SetTileState(s_tile_bt, bt ? I18n::T("已开启") : I18n::T("关闭"), bt);
    }
    if (s_tile_4g) {
        StyleTile(s_tile_4g, cell);
        SetTileIcon(s_tile_4g, kFa4g);
        SetTileState(s_tile_4g, cell ? I18n::T("已开启") : I18n::T("关闭"), cell);
    }
    if (s_tile_air) {
        StyleTile(s_tile_air, air, kTileWarn);
        SetTileState(s_tile_air, air ? I18n::T("已开启") : I18n::T("关闭"), air);
    }
}

void RefreshRadioTiles()
{
    {
        InvalidationPause pause(lv_display_get_default());
        StyleRadioTiles();
    }
    if (s_tile_wifi) lv_obj_invalidate(s_tile_wifi);
    if (s_tile_bt) lv_obj_invalidate(s_tile_bt);
    if (s_tile_4g) lv_obj_invalidate(s_tile_4g);
    if (s_tile_air) lv_obj_invalidate(s_tile_air);
}

void RefreshTiles()
{
    StyleRadioTiles();
    if (s_tile_loc != nullptr) {
        StyleTile(s_tile_loc, s_loc_on, kTileGreen);
        SetTileIcon(s_tile_loc, kFaLoc);
        SetTileState(s_tile_loc, s_loc_on ? I18n::T("已开启") : I18n::T("关闭"),
                     s_loc_on);
        if (s_paint_each_tile) PaintTileNow(s_tile_loc);
    }
    if (s_tile_sound != nullptr) {
        UpdateSoundTile();
        if (s_paint_each_tile) PaintTileNow(s_tile_sound);
    }
    if (s_tile_settings != nullptr) {
        StyleTile(s_tile_settings, false);
        SetTileIcon(s_tile_settings, kFaGear);
        if (s_paint_each_tile) PaintTileNow(s_tile_settings);
    }
    if (s_tile_camera != nullptr) {
        StyleTile(s_tile_camera, false);
        SetTileIcon(s_tile_camera, kFaCam);
        if (s_paint_each_tile) PaintTileNow(s_tile_camera);
    }
}

int SnapStep(int pct)
{
    if (pct <= 15) return 10;
    if (pct <= 40) return 30;
    if (pct <= 65) return 50;
    if (pct <= 90) return 80;
    return 100;
}

void UpdateBriUi(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    const int step = SnapStep(pct);
    if (s_bri_icon != nullptr) {
        const char *want = step <= 30 ? FONT_AWESOME_MOON : FONT_AWESOME_SUN;
        if (std::strcmp(lv_label_get_text(s_bri_icon), want) != 0) {
            lv_label_set_text(s_bri_icon, want);
        }
    }
    if (s_bri_pct != nullptr) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", pct);
        if (std::strcmp(lv_label_get_text(s_bri_pct), buf) != 0) {
            lv_label_set_text(s_bri_pct, buf);
        }
    }
}

void UpdateVolUi(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const int step = SnapStep(pct == 0 ? 10 : pct);
    if (s_vol_icon != nullptr) {
        const char *ic = FONT_AWESOME_VOLUME_HIGH;
        if (pct == 0 || step <= 10) {
            ic = FONT_AWESOME_VOLUME_XMARK;
        } else if (step <= 50) {
            ic = FONT_AWESOME_VOLUME_LOW;
        }
        if (std::strcmp(lv_label_get_text(s_vol_icon), ic) != 0) {
            lv_label_set_text(s_vol_icon, ic);
        }
    }
    if (s_vol_pct != nullptr) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", pct);
        if (std::strcmp(lv_label_get_text(s_vol_pct), buf) != 0) {
            lv_label_set_text(s_vol_pct, buf);
        }
    }
}

void LoadState()
{
    if (s_network_prefs_dirty || s_sound_prefs_dirty || s_bri_save >= 0 ||
        s_radio_pending || s_shade_persist_running) return;
    Settings settings("network", false);
    s_air_on = settings.GetBool("airplane", false);
    s_wifi_on = settings.GetBool("wifi_enabled",
        !settings.GetBool("cellular", settings.GetInt("type", 0) == 1));
    s_bt_on = settings.GetBool("bt_enabled", true);
    s_4g_on = settings.GetBool("cellular", settings.GetInt("type", 0) == 1);
    s_loc_on = settings.GetBool("location", false);
    s_sound = static_cast<SoundMode>(settings.GetInt("sound_mode", 0));
    if (s_sound < SoundMode::Ring || s_sound > SoundMode::Mute) {
        s_sound = SoundMode::Ring;
    }
    s_ring_volume = settings.GetInt("ring_volume", 70);
    if (s_ring_volume < 1) s_ring_volume = 70;
    if (s_ring_volume > 100) s_ring_volume = 100;
    const int active_path = metalio_dual_network_path();
    if (!s_air_on && active_path >= 0) {
        s_wifi_on = active_path == 0;
        s_4g_on = active_path == 1;
    }
    if (!s_air_on) s_bt_on = metalio_bt_is_powered();
}

void OnWifi(lv_event_t * /*e*/)
{
    if (s_air_on) return;
    if (s_wifi_long_press) {
        /* LONG_PRESSED already handled open — swallow the trailing click. */
        s_wifi_long_press = false;
        return;
    }
    s_wifi_on = !s_wifi_on;
    if (s_wifi_on) s_4g_on = false;
    RefreshRadioTiles();
    KickRadioApply();
}

void OnWifiLongPress(lv_event_t *e)
{
    s_wifi_long_press = true;
    lv_event_stop_bubbling(e);
    /* Hide shade first, then open network config with auto-scan. */
    ControlCenter_Hide();
    NetworkScreen::RequestAutoScanOnNextLoad();
    HomeScreen::LaunchApp("wifi");
}

void OnBt(lv_event_t * /*e*/)
{
    if (s_air_on) return;
    s_bt_on = !s_bt_on;
    RefreshRadioTiles();
    KickRadioApply();
}

void On4g(lv_event_t * /*e*/)
{
    if (s_air_on) return;
    s_4g_on = !s_4g_on;
    if (s_4g_on) s_wifi_on = false;
    RefreshRadioTiles();
    KickRadioApply();
}

void OnAir(lv_event_t * /*e*/)
{
    s_air_on = !s_air_on;
    if (s_air_on) {
        /* Radios are gated by airplane in ApplyRadiosToHardware. Keep the
         * WiFi and 4G switches so closing airplane mode brings back whichever
         * of them was on. */
    }
    RefreshRadioTiles();
    KickRadioApply();
}

void OnLoc(lv_event_t * /*e*/)
{
    s_loc_on = !s_loc_on;
    RefreshTiles();
    PersistSoundUi();
    if (s_loc_on) {
        /* Kick the GPS reader so a fix is being acquired while the toggle
         * is on.  The board reader thread runs from boot, but this also
         * serves as a user-visible "I want location" signal. */
        metalio_gps_initialize();
        PulseVibrateAsync(80 * 1000);
    }
}

void OnLocLongPress(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    /* Open the GPS screen for a real fix + map. */
    ControlCenter_Hide();
    HomeScreen::LaunchApp("gps");
}

/* One later frame paints the tile and the volume bar together. Painting
 * them into the page being scanned is what tore the cards and then went
 * blue. */
void ShowSoundChange(bool play_preview)
{
    ApplySoundMode(play_preview, true);
    UpdateSoundTile();
}

/* Single cycling sound-mode tile: Ring → Vibrate → Mute → Ring … */
void OnSound(lv_event_t * /*e*/)
{
    if (s_sound == SoundMode::Ring) {
        s_sound = SoundMode::Vibrate;
        PersistSoundUi();
        ShowSoundChange(false);
        PulseVibrateAsync();
    } else if (s_sound == SoundMode::Vibrate) {
        s_sound = SoundMode::Mute;
        PersistSoundUi();
        ShowSoundChange(false);
    } else {
        s_sound = SoundMode::Ring;
        PersistSoundUi();
        ShowSoundChange(true);
    }
}

void OnSettings(lv_event_t * /*e*/)
{
    ControlCenter_Hide();
    HomeScreen::LaunchApp("settings");
}

void OnCamera(lv_event_t * /*e*/)
{
    ControlCenter_Hide();
    HomeScreen::LaunchApp("camera");
}

void StopGestureBubble(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

/* Finger state only. The bar is an LVGL object; the next frame flips a
 * finished back buffer. Do not paint the page the panel is scanning. */
bool s_slider_pressed = false;
void OnHypSliderPressed(lv_event_t * /*e*/)
{
    s_slider_pressed = true;
}
void OnHypSliderReleased(lv_event_t * /*e*/)
{
    s_slider_pressed = false;
}

/* Brightness: store the value and let the next full frame redraw the bar
 * and the number. The backlight updates on the sample. */
void OnBriPressing(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (in == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    int pct = BarPctFromX(s_bri, p.x);
    if (pct < s_bri.lo) pct = s_bri.lo;
    if (pct == s_bri.val) return;
    BarStore(s_bri, pct);
    if (s_bri.track != nullptr) lv_obj_invalidate(s_bri.track);
    UpdateBriUi(pct);
    if (auto *bl = Board::GetInstance().GetBacklight()) {
        bl->SetBrightness(static_cast<uint8_t>(pct), false);
    }
    NoteBrightness(pct);
}

void OnBriRelease(lv_event_t * /*e*/)
{
    UpdateBriUi(s_bri.val);
    if (s_bri.track != nullptr) lv_obj_invalidate(s_bri.track);
}

/* Same as brightness: store, invalidate the track (DRAW_POST in the
 * current refresh), update the number. Software gain only — never
 * AUDIOIOC, never FBIO_UPDATE, never a worker on PRESSING (those raced
 * LCDC). */
void ApplyVolFromPointer()
{
    lv_indev_t *in = lv_indev_active();
    if (in == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    int pct = BarPctFromX(s_vol, p.x);
    if (pct < s_vol.lo) pct = s_vol.lo;
    if (pct > s_vol.hi) pct = s_vol.hi;
    if (pct == s_vol.val) return;
    BarStore(s_vol, pct);
    if (s_vol.track != nullptr) lv_obj_invalidate(s_vol.track);
    UpdateVolUi(pct);
    if (pct > 0) {
        if (s_sound != SoundMode::Ring) s_sound = SoundMode::Ring;
        s_ring_volume = pct;
    } else if (s_sound == SoundMode::Ring) {
        s_sound = SoundMode::Mute;
    }
    Application::GetInstance().GetAudioService().SetVolume(pct, false);
    PersistSoundUi();
}

void OnVolPressing(lv_event_t * /*e*/)
{
    ApplyVolFromPointer();
}

void OnVolRelease(lv_event_t * /*e*/)
{
    ApplyVolFromPointer();
    UpdateVolUi(s_vol.val);
    if (s_vol.track != nullptr) lv_obj_invalidate(s_vol.track);
    PersistSoundUi();
}

/* DRAW_POST fill: only runs inside a refresh that was already going to happen
 * (open animation, release, external refresh).  A single radius-0 fill. */
void OnBarDraw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int w = a.x2 - a.x1 + 1;
    const int h = a.y2 - a.y1 + 1;
    if (w <= 0 || h <= 0) return;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = kBarH / 2;
    dsc.bg_color = lv_color_hex(kTrack);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 0;
    dsc.shadow_width = 0;
    lv_area_t full = a;
    lv_draw_rect(lv_event_get_layer(e), &dsc, &full);
    Bar *b = static_cast<Bar *>(lv_event_get_user_data(e));
    if (b == nullptr) return;
    int fx = BarFillX(*b, a);
    if (fx <= a.x1) return;
    if (fx > a.x2 + 1) fx = a.x2 + 1;
    lv_area_t fill = a;
    fill.x2 = static_cast<lv_coord_t>(fx - 1);
    dsc.bg_color = lv_color_hex(kTileOn);
    lv_draw_rect(lv_event_get_layer(e), &dsc, &fill);
    b->painted_x = fx;
}

lv_obj_t *MakeBar(Bar *slot, lv_obj_t *parent, int lo, int hi, int val,
                  lv_event_cb_t on_pressing, lv_event_cb_t on_release)
{
    slot->lo = lo;
    slot->hi = hi;
    slot->val = val;
    slot->painted_x = -1;

    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_flex_grow(track, 1);
    lv_obj_set_height(track, kBarH);
    lv_obj_set_style_radius(track, kBarH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(track, lv_color_hex(kTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    /* Rounded outline (圆角边框) so the bar reads as a framed pill. */
    lv_obj_set_style_border_width(track, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(track, lv_color_hex(0x4A4F5C), LV_PART_MAIN);
    lv_obj_set_style_border_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(track, 0, LV_PART_MAIN);
    lv_obj_add_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(track, true);
    slot->track = track;

    lv_obj_add_event_cb(track, OnBarDraw, LV_EVENT_DRAW_POST, slot);
    lv_obj_add_event_cb(track, on_pressing, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(track, on_pressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(track, on_release, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(track, OnHypSliderPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(track, OnHypSliderReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(track, StopGestureBubble, LV_EVENT_GESTURE, nullptr);
    return track;
}

/* HyperOS-style rounded card: icon chip + bar (flex-grow) + pct label.
 * *out_pct points at the pct label so UpdateBriUi / UpdateVolUi work. */
lv_obj_t *MakeSliderRow(lv_obj_t *parent, int y, const char *fa_icon,
                        lv_obj_t **out_icon, lv_obj_t **out_pct,
                        Bar *slot, int lo, int hi, int val,
                        lv_event_cb_t on_pressing, lv_event_cb_t on_release)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), kStepRowH);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(kShadeBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_chip = lv_obj_create(row);
    lv_obj_remove_style_all(icon_chip);
    lv_obj_set_size(icon_chip, 40, 40);
    lv_obj_set_style_radius(icon_chip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon_chip, lv_color_hex(kTileOff), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon_chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon_chip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(icon_chip, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t *icon = lv_label_create(icon_chip);
    lv_label_set_text(icon, fa_icon);
    lv_obj_set_style_text_font(icon, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_center(icon);
    *out_icon = icon;

    MakeBar(slot, row, lo, hi, val, on_pressing, on_release);

    lv_obj_t *pct = lv_label_create(row);
    lv_label_set_text(pct, "0");
    lv_obj_set_style_text_font(pct, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(pct, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(pct, 56);
    lv_obj_remove_flag(pct, LV_OBJ_FLAG_CLICKABLE);
    *out_pct = pct;
    slot->label = pct;

    return row;
}

/* Hide from inside an event handler targeting s_root/s_panel.  Deleting the
 * overlay synchronously while it is still the event target corrupts the LVGL
 * heap on this port (solid blue).  Null the state immediately and let LVGL
 * delete the object asynchronously after the event returns. */
struct ShadePersistJob {
    int bri;
    bool sound;
    bool network;
    int32_t values[8];
};

void CapturePrefs(ShadePersistJob &job)
{
    job.bri = s_bri_save;
    job.sound = s_sound_prefs_dirty;
    job.network = s_network_prefs_dirty && !s_radio_pending;
    job.values[0] = s_air_on;
    job.values[1] = s_wifi_on;
    job.values[2] = s_bt_on;
    job.values[3] = s_4g_on;
    job.values[4] = s_4g_on ? 1 : 0;
    job.values[5] = s_loc_on;
    job.values[6] = static_cast<int>(s_sound);
    job.values[7] = std::max(1, std::min(100, s_ring_volume));
}

bool SavePrefs(const ShadePersistJob &job)
{
    bool ok = true;
    if (job.bri >= 0) {
        Settings settings("display", true);
        const char *keys[] = {"brightness"};
        const int32_t value = job.bri;
        ok = settings.SetInts(keys, &value, 1);
    }
    if (job.network || job.sound) {
        Settings settings("network", true);
        const char *keys[] = {"airplane", "wifi_enabled", "bt_enabled", "cellular",
                              "type", "location", "sound_mode", "ring_volume"};
        // A sound-only save must not overwrite a network selection made
        // in the network app after this snapshot was queued (and vice versa).
        const size_t first = job.network ? 0 : 5;
        const size_t count = job.network ? (job.sound ? 8 : 5) : 3;
        int32_t values[8];
        std::copy(job.values, job.values + 8, values);
        if (job.network) {
            // A queued shade/shutdown snapshot may predate a selection in
            // Network Settings. Save the confirmed live data mode instead.
            // -1 keeps genuine airplane/all-radios-off intent unchanged.
            const int path = metalio_app_active_data_path();
            if (path == 0 || path == 1) {
                values[0] = 0;
                values[1] = path == 0;
                values[3] = path == 1;
                values[4] = path;
            }
        }
        ok = settings.SetInts(keys + first, values + first, count) && ok;
    }
    return ok;
}

void *ShadeClosePersistWorker(void *arg)
{
    auto *job = static_cast<ShadePersistJob *>(arg);
    pthread_mutex_lock(&s_save_mu);
    // A queued older snapshot must never overwrite the shutdown snapshot.
    const bool ok = s_final_save || SavePrefs(*job);
    pthread_mutex_unlock(&s_save_mu);
    lv_lock();
    if (!ok) {
        if (s_bri_save < 0) s_bri_save = job->bri;
        s_network_prefs_dirty |= job->network;
        s_sound_prefs_dirty |= job->sound;
    }
    s_shade_persist_running = false;
    ScheduleFlashSave();
    lv_unlock();
    delete job;
    return nullptr;
}

void FlashSaveLater(lv_timer_t *t)
{
    s_save_timer = nullptr;
    lv_timer_delete(t);
    if (s_final_save || s_shade_persist_running) return;
    if (!s_sound_prefs_dirty && !s_network_prefs_dirty && s_bri_save < 0) return;
    auto *job = new ShadePersistJob;
    if (job == nullptr) { ScheduleFlashSave(); return; }
    CapturePrefs(*job);
    s_bri_save = -1;
    if (job->network) s_network_prefs_dirty = false;
    s_sound_prefs_dirty = false;
    s_shade_persist_running = true;
    pthread_t tid{};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 8192);
    if (hi_task_create_legacy(&tid, &attr, ShadeClosePersistWorker, job) != 0) {
        s_bri_save = job->bri;
        s_network_prefs_dirty = job->network;
        s_sound_prefs_dirty = job->sound;
        s_shade_persist_running = false;
        delete job;
        ScheduleFlashSave();
    }
    pthread_attr_destroy(&attr);
}

void ScheduleFlashSave()
{
    if (s_final_save) return;
    if (!s_sound_prefs_dirty && !s_network_prefs_dirty && s_bri_save < 0) return;
    // One debounce timer: dragging must not create an unbounded timer queue.
    if (s_save_timer) lv_timer_reset(s_save_timer);
    else s_save_timer = lv_timer_create(FlashSaveLater, 500, nullptr);
}

void ClearControlCenterObjects()
{
    s_root = nullptr;
    s_panel = nullptr;
    s_tile_wifi = s_tile_bt = s_tile_4g = s_tile_air = nullptr;
    s_tile_loc = s_tile_sound = s_tile_settings = s_tile_camera = nullptr;
    s_bri = Bar{};
    s_vol = Bar{};
    s_bri_icon = s_bri_pct = s_vol_icon = s_vol_pct = nullptr;
    s_slider_pressed = false;
}

IdlePowerSession s_shade_previous_session = IdlePowerSession::None;
void RestoreShadeIdleSession()
{
    if (IdlePower_CurrentSession() != IdlePowerSession::Home) return;
    if (s_shade_previous_session == IdlePowerSession::None)
        IdlePower_Detach(IdlePowerSession::Home);
    else
        IdlePower_Attach(s_shade_previous_session, false);
}

void OnControlCenterDeleted(lv_event_t *e)
{
    // Parent screens can destroy the shade without calling Hide. Never
    // retain pointers into that subtree, or clear a newer shade's state.
    if (lv_event_get_target_obj(e) != s_root) return;
    RestoreShadeIdleSession();
    ScheduleFlashSave();
    ClearControlCenterObjects();
}

void HideControlCenterAsync()
{
    if (s_root == nullptr) return;
    RestoreShadeIdleSession();
    ScheduleFlashSave();
    lv_obj_t *root = s_root;
    ClearControlCenterObjects();
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(root);
}

void OnMaskClick(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    HideControlCenterAsync();
}

void OnPanelGesture(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    if (lv_event_get_target_obj(e) != s_panel) return;
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        HideControlCenterAsync();
    }
}

lv_obj_t *MakeTileRow(lv_obj_t *parent, int y, int h, int gap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), h);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void BuildUi(lv_obj_t *parent)
{
    LoadState();

    s_root = lv_obj_create(parent);
    lv_obj_add_event_cb(s_root, OnControlCenterDeleted, LV_EVENT_DELETE, nullptr);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, kPanel, kPanel);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), LV_PART_MAIN);
    /* Full-opacity mask — the 60 % transparent blend caused the LCDC/DBI
     * path to underrun (solid blue) whenever the sliders invalidated. */
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_root, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_root, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(s_root, OnMaskClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_root, [](lv_event_t * /*e*/) {
        IdlePower_NotifyActivity();
    }, LV_EVENT_PRESSED, nullptr);
    screen_swipe_back_ignore(s_root, true);

    s_panel = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, kPanel - 40, 560);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(kShadeBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, LV_PART_MAIN);
    /* Rounded panel (HyperOS / HarmonyOS style). Safe now: slider redraws
     * stay inside their opaque row bbox and never touch the panel corners,
     * so the rounded corner mask is only evaluated when the panel itself
     * is invalidated (open/close), not on every value change. */
    lv_obj_set_style_radius(s_panel, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_panel, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_panel, 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_panel, OnPanelGesture, LV_EVENT_GESTURE, nullptr);
    screen_swipe_back_ignore(s_panel, true);

    lv_obj_t *handle = lv_obj_create(s_panel);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 56, 5);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0x5A6070), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(handle, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, I18n::T("控制中心"));
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kMuted), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 16);

    /* Keep the system clock visible in the shade, as on the reference UI. */
    lv_obj_t *clock = lv_label_create(s_panel);
    lv_obj_set_style_text_font(clock, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(clock, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_set_style_text_align(clock, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_width(clock, 112);
    lv_obj_align(clock, LV_ALIGN_TOP_RIGHT, -4, 10);
    screen_make_input_passive(clock);
    auto update_clock = [](lv_obj_t *label) {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        char text[8] = "--:--";
        if (localtime_r(&now, &local) != nullptr)
            std::strftime(text, sizeof(text), "%H:%M", &local);
        lv_label_set_text(label, text);
    };
    update_clock(clock);
    lv_timer_t *clock_timer = lv_timer_create(
        [](lv_timer_t *timer) {
            auto *label = static_cast<lv_obj_t *>(lv_timer_get_user_data(timer));
            if (label == nullptr || !lv_obj_is_valid(label))
                return;
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            char text[8] = "--:--";
            if (localtime_r(&now, &local) != nullptr)
                std::strftime(text, sizeof(text), "%H:%M", &local);
            lv_label_set_text(label, text);
        }, 1000, clock);
    if (clock_timer != nullptr) {
        lv_obj_add_event_cb(clock, [](lv_event_t *event) {
            auto *timer = static_cast<lv_timer_t *>(lv_event_get_user_data(event));
            if (timer != nullptr)
                lv_timer_delete(timer);
        }, LV_EVENT_DELETE, clock_timer);
    }

    /* Row 1: 2 big connectivity tiles (WiFi + Bluetooth). */
    lv_obj_t *row1 = MakeTileRow(s_panel, 44, 120, 12);
    s_tile_wifi = MakeBigTile(row1, kFaWifiOn, I18n::T("WiFi"), OnWifi,
                              OnWifiLongPress);
    s_tile_bt = MakeBigTile(row1, kFaBt, I18n::T("蓝牙"), OnBt);

    /* Row 2: 2 big connectivity tiles (4G + Airplane). */
    lv_obj_t *row2 = MakeTileRow(s_panel, 172, 120, 12);
    s_tile_4g = MakeBigTile(row2, kFa4g, I18n::T("4G"), On4g);
    s_tile_air = MakePlaneTile(row2, I18n::T("飞行模式"), OnAir);

    /* Row 3: 4 small shortcut tiles (Location, Sound, Settings, Camera). */
    lv_obj_t *row3 = MakeTileRow(s_panel, 300, 80, 12);
    s_tile_loc = MakeSmallTile(row3, kFaLoc, I18n::T("定位"), OnLoc,
                               OnLocLongPress);
    s_tile_sound = MakeSmallTile(row3, kFaBell, I18n::T("响铃"), OnSound);
    s_tile_camera = MakeSmallTile(row3, kFaCam, I18n::T("相机"), OnCamera);
    s_tile_settings = MakeSmallTile(row3, kFaGear, I18n::T("设置"), OnSettings);

    /* icon | stepper (− value +) */
    int bri = 50;
    if (auto *bl = Board::GetInstance().GetBacklight()) {
        bri = static_cast<int>(bl->GetBrightness());
    } else {
        Settings ds("display", false);
        bri = ds.GetInt("brightness", 50);
    }
    MakeSliderRow(s_panel, 388, FONT_AWESOME_SUN, &s_bri_icon, &s_bri_pct,
                  &s_bri, 5, 100, bri, OnBriPressing, OnBriRelease);
    UpdateBriUi(bri);

    int vol = Application::GetInstance().GetAudioService().GetVolume();
    if (s_sound != SoundMode::Ring) {
        vol = 0;
    } else if (vol < 1) {
        vol = s_ring_volume;
    } else {
        s_ring_volume = vol;
    }
    MakeSliderRow(s_panel, 460, FONT_AWESOME_VOLUME_HIGH,
                  &s_vol_icon, &s_vol_pct,
                  &s_vol, 0, 100, vol, OnVolPressing, OnVolRelease);
    UpdateVolUi(vol);
    ApplySoundMode(false);

    s_paint_each_tile = false;
    RefreshTiles();

    /* Keep the shade stationary on open.  Moving this large panel invalidates
     * nearly the full display on every animation frame and can leave the
     * display controller in a blue-screen state on this board. */
    lv_obj_set_y(s_panel, 0);
}

}  // namespace

void ControlCenter_SaveBeforePowerOff()
{
    PhoneService::FlushHistory();
    lv_lock();
    ShadePersistJob job{};
    CapturePrefs(job);
    if (s_shade_persist_running) {
        job.network = job.sound = true;
        if (job.bri < 0) {
            if (auto *bl = Board::GetInstance().GetBacklight())
                job.bri = bl->GetBrightness();
        }
    }
    s_final_save = true;
    if (s_save_timer) { lv_timer_delete(s_save_timer); s_save_timer = nullptr; }
    // Serialize with a worker's write, but never wait for its LVGL cleanup.
    pthread_mutex_lock(&s_save_mu);
    const bool ok = SavePrefs(job);
    pthread_mutex_unlock(&s_save_mu);
    lv_unlock();
    if (!ok) ESP_LOGE(TAG, "final control-center save failed");
}

const lv_image_dsc_t *ControlCenter_GetStatusPlaneIcon(void)
{
    static uint8_t pixels[24 * 24 * 4];
    static lv_image_dsc_t icon = {};
    if (icon.data == nullptr) {
        EnsurePlaneDsc();
        for (int y = 0; y < 24; ++y) {
            for (int x = 0; x < 24; ++x) {
                unsigned alpha = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        alpha += s_plane_px[((y * 2 + dy) * kPlaneN + x * 2 + dx) * 4 + 3];
                uint8_t *p = pixels + (y * 24 + x) * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = static_cast<uint8_t>((alpha + 2) / 4);
            }
        }
        icon.header.magic = LV_IMAGE_HEADER_MAGIC;
        icon.header.cf = LV_COLOR_FORMAT_ARGB8888;
        icon.header.w = icon.header.h = 24;
        icon.header.stride = 24 * 4;
        icon.data_size = sizeof(pixels);
        icon.data = pixels;
    }
    return &icon;
}

void ControlCenter_Show(lv_obj_t *parent)
{
    if (parent == nullptr) parent = lv_screen_active();
    if (parent == nullptr) return;
    if (s_root != nullptr) {
        ControlCenter_Hide();
    }
    s_shade_previous_session = IdlePower_CurrentSession();
    IdlePower_Attach(IdlePowerSession::Home, true);
    BuildUi(parent);
}

void ControlCenter_Hide(void)
{
    HideControlCenterAsync();
}

bool ControlCenter_IsOpen(void)
{
    return s_root != nullptr;
}

void ControlCenter_Refresh(void)
{
    if (s_root == nullptr) return;
    LoadState();
    RefreshTiles();
    if (s_bri.track != nullptr) {
        int bri = 50;
        if (auto *bl = Board::GetInstance().GetBacklight()) {
            bri = static_cast<int>(bl->GetBrightness());
        }
        BarSync(s_bri, bri);
        UpdateBriUi(bri);
    }
    if (s_vol.track != nullptr) {
        int vol = 0;
        if (s_sound == SoundMode::Ring) {
            vol = Application::GetInstance().GetAudioService().GetVolume();
            if (vol < 1) vol = s_ring_volume;
        }
        BarSync(s_vol, vol);
        UpdateVolUi(vol);
    }
}
