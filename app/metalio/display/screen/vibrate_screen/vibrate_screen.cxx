/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * VibrateScreen — ported from MetalioClaw4
 * main/display/screen/vibrate_screen/vibrate_screen.cc.
 *
 * Faithful port of the reference vibration App:
 *   - 3 modes (manual / pulse / heartbeat); the slider is the single source
 *     of truth for amplitude (0..100%).
 *   - Dragging the slider always returns to "manual constant" mode and stops
 *     the pattern timer, so the slider and the pattern never fight each other.
 *   - "关闭" = manual + slider 0 => guaranteed off (duty 0 + timer stopped).
 *
 * The motor is driven by board LEDC PWM on GPIO 22 exposed as /dev/pwm1
 * (LEDC timer1). apply_duty_pct() keeps the PWM running and only changes the
 * duty (0% = off), matching the ESP-IDF ledc_set_duty semantics.
 */

#include "vibrate_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "esp_timer_shim.h"
#include "home_screen/home_screen.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#include <nuttx/timers/pwm.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_50_4);

namespace {

constexpr const char *TAG = "VibrateScreen";

constexpr int kPanelSize = 720;
constexpr int kHeaderH = 90;
constexpr int kBackBtnSize = 72;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorSubtle = 0x9AA3B2;
constexpr uint32_t kColorCard = 0x1B2030;
constexpr uint32_t kColorAccent = 0x3B82F6;
constexpr uint32_t kColorBtn = 0x2A2F3A;
constexpr uint32_t kColorBtnBorder = 0x3A4050;
constexpr uint32_t kColorBtnActiveBorder = 0x60A5FA;
constexpr uint32_t kColorSliderTrack = 0x2A2F3A;

// ---------------------------------------------------------------------------
// 模式定义（与参考固件一致）。振幅只由 slider（0..100）决定，模式决定
// 如何把振幅作用到马达上。
// ---------------------------------------------------------------------------
enum class Mode : uint8_t {
    kManual = 0,    // duty 直接 = slider 当前值
    kPulse,         // 500ms 周期方波（半亮半暗）
    kHeartbeat,     // 1s 周期：100ms on / 100ms off / 100ms on / 700ms off
};

// 预设按钮：target_pct == -1 表示“不改 slider，仅切模式”，给脉冲 / 心跳用。
struct PresetEntry {
    const char *label;
    int8_t target_pct;
    Mode target_mode;
};

constexpr PresetEntry kPresets[] = {
    { "关闭", 0,   Mode::kManual    },
    { "弱",   30,  Mode::kManual    },
    { "中",   60,  Mode::kManual    },
    { "强",   100, Mode::kManual    },
    { "脉冲", -1,  Mode::kPulse     },
    { "心跳", -1,  Mode::kHeartbeat },
};
constexpr int kPresetCount =
    static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

struct UiState {
    lv_obj_t *screen = nullptr;
    lv_obj_t *slider = nullptr;
    lv_obj_t *pct_label = nullptr;
    lv_obj_t *mode_label = nullptr;
    lv_obj_t *preset_btns[kPresetCount] = {};
};
UiState s_ui;

Mode s_mode = Mode::kManual;
int s_slider_pct = 0;
int s_active_preset = 0;

lv_timer_t *s_pattern_timer = nullptr;
int64_t s_pattern_t0_us = 0;

constexpr uint32_t kMotorFreqHz = 5000;
constexpr uint32_t kPatternTickMs = 30;

int g_pwm_fd = -1;
bool g_pwm_started = false;

// ---------------------------------------------------------------------------
// 马达 PWM（/dev/pwm1, GPIO22, LEDC timer1）
// ---------------------------------------------------------------------------
void MotorInitOnce()
{
    if (g_pwm_fd >= 0) return;
    g_pwm_fd = open("/dev/pwm1", O_WRONLY);
    if (g_pwm_fd < 0) {
        ESP_LOGW(TAG, "open /dev/pwm1 failed: %d (vibrate motor unavailable)",
                 errno);
    }
}

// 把 0..100 的百分比写到 PWM。保持 PWM 常开，只改 duty（0% = 马达停）。
void apply_duty_pct(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (g_pwm_fd < 0) MotorInitOnce();
    if (g_pwm_fd < 0) return;

    struct pwm_info_s info;
    memset(&info, 0, sizeof(info));
    info.frequency = kMotorFreqHz;
    info.duty = static_cast<uint32_t>((pct * 65535u) / 100u);
    info.cpol = PWM_CPOL_NDEF;
    info.dcpol = PWM_DCPOL_NDEF;

    if (ioctl(g_pwm_fd, PWMIOC_SETCHARACTERISTICS,
              (unsigned long)&info) < 0) {
        ESP_LOGW(TAG, "PWMIOC_SETCHARACTERISTICS failed: %d", errno);
        return;
    }

    if (!g_pwm_started) {
        if (ioctl(g_pwm_fd, PWMIOC_START, 0) < 0) {
            ESP_LOGW(TAG, "PWMIOC_START failed: %d", errno);
            return;
        }
        g_pwm_started = true;
    }
}

// ---------------------------------------------------------------------------
// 模式 / pattern timer
// ---------------------------------------------------------------------------
void on_pattern_tick(lv_timer_t * /*t*/);

void stop_pattern_timer()
{
    if (s_pattern_timer != nullptr) {
        lv_timer_delete(s_pattern_timer);
        s_pattern_timer = nullptr;
    }
}

void start_pattern_timer()
{
    if (s_pattern_timer != nullptr) return;
    s_pattern_t0_us = esp_timer_get_time();
    s_pattern_timer =
        lv_timer_create(on_pattern_tick, kPatternTickMs, nullptr);
}

// 把 (mode, pct) 应用到马达。手动恒定直接拉 duty；脉冲 / 心跳由 timer 周期切换。
void apply_state()
{
    if (s_mode == Mode::kManual) {
        stop_pattern_timer();
        apply_duty_pct(s_slider_pct);
    } else {
        if (s_pattern_timer == nullptr) {
            start_pattern_timer();
        } else {
            s_pattern_t0_us = esp_timer_get_time();
        }
        on_pattern_tick(nullptr);
    }
}

void on_pattern_tick(lv_timer_t * /*t*/)
{
    int64_t now = esp_timer_get_time();
    uint32_t elapsed_ms =
        static_cast<uint32_t>((now - s_pattern_t0_us) / 1000);

    bool on = false;
    if (s_mode == Mode::kPulse) {
        on = (elapsed_ms % 500U) < 250U;
    } else if (s_mode == Mode::kHeartbeat) {
        uint32_t p = elapsed_ms % 1000U;
        on = (p < 100U) || (p >= 200U && p < 300U);
    }
    apply_duty_pct(on ? s_slider_pct : 0);
}

void shutdown_pwm()
{
    stop_pattern_timer();
    apply_duty_pct(0);
}

// ---------------------------------------------------------------------------
// UI 同步
// ---------------------------------------------------------------------------
const char *mode_name(Mode m)
{
    switch (m) {
    case Mode::kManual:
        return s_slider_pct == 0 ? I18n::T("关闭") : I18n::T("恒定");
    case Mode::kPulse:     return I18n::T("脉冲");
    case Mode::kHeartbeat: return I18n::T("心跳");
    }
    return "?";
}

void refresh_pct_label()
{
    if (s_ui.pct_label == nullptr) return;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d%%", s_slider_pct);
    lv_label_set_text(s_ui.pct_label, buf);
}

void refresh_mode_label()
{
    if (s_ui.mode_label == nullptr) return;
    lv_label_set_text(s_ui.mode_label, mode_name(s_mode));
}

void refresh_preset_buttons()
{
    for (int i = 0; i < kPresetCount; i++) {
        lv_obj_t *btn = s_ui.preset_btns[i];
        if (btn == nullptr) continue;
        bool active = (i == s_active_preset);
        lv_obj_set_style_bg_color(
            btn, lv_color_hex(active ? kColorAccent : kColorBtn),
            LV_PART_MAIN);
        lv_obj_set_style_border_color(
            btn, lv_color_hex(active ? kColorBtnActiveBorder
                                      : kColorBtnBorder),
            LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, active ? 2 : 0, LV_PART_MAIN);
    }
}

// ---------------------------------------------------------------------------
// 事件回调
// ---------------------------------------------------------------------------
void on_slider_value_changed(lv_event_t *e)
{
    auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    s_slider_pct = static_cast<int>(lv_slider_get_value(slider));

    // 用户拖滑条 -> 回到“手动恒定”，停掉 pattern timer，避免相互干扰。
    s_mode = Mode::kManual;
    s_active_preset = -1;
    for (int i = 0; i < kPresetCount; i++) {
        if (kPresets[i].target_mode == Mode::kManual &&
            kPresets[i].target_pct == s_slider_pct) {
            s_active_preset = i;
            break;
        }
    }

    refresh_pct_label();
    refresh_mode_label();
    refresh_preset_buttons();
    apply_state();
}

void on_preset_clicked(lv_event_t *e)
{
    int idx = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= kPresetCount) return;
    const PresetEntry &p = kPresets[idx];

    if (p.target_pct >= 0) {
        s_slider_pct = p.target_pct;
        if (s_ui.slider != nullptr) {
            lv_slider_set_value(s_ui.slider, s_slider_pct, LV_ANIM_ON);
        }
    }
    s_mode = p.target_mode;
    s_active_preset = idx;

    refresh_pct_label();
    refresh_mode_label();
    refresh_preset_buttons();
    apply_state();
}

// ---------------------------------------------------------------------------
// 屏幕导航
// ---------------------------------------------------------------------------
void OnSwipeBack()
{
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    HomeScreen::SwitchToHome();
}

void on_back_btn_clicked(lv_event_t * /*e*/) { OnSwipeBack(); }

void on_screen_unloaded(lv_event_t * /*e*/)
{
    s_ui.screen = nullptr;
    s_ui.slider = nullptr;
    s_ui.pct_label = nullptr;
    s_ui.mode_label = nullptr;
    for (int i = 0; i < kPresetCount; i++) {
        s_ui.preset_btns[i] = nullptr;
    }
    // pattern timer 建在 LVGL 线程里，屏幕没了必须关掉；离开页面一律静默。
    shutdown_pwm();
}

void BuildHeader(lv_obj_t *scr)
{
    lv_obj_t *header = lv_obj_create(scr);
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
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, on_back_btn_clicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *icon = lv_image_create(back);
    lv_image_set_src(icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("震动"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);
}

}  // namespace

lv_obj_t *VibrateScreen::CreateStatic()
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);

    // ---------------- 大数显（百分比 + 当前模式名） ----------------
    lv_obj_t *card = lv_obj_create(scr);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 660, 220);
    lv_obj_set_pos(card, 30, 100);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(card);

    s_ui.pct_label = lv_label_create(card);
    lv_obj_set_width(s_ui.pct_label, LV_PCT(100));
    lv_label_set_long_mode(s_ui.pct_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_ui.pct_label, lv_color_hex(0x60A5FA),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.pct_label, &font_puhui_number_50_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.pct_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_align(s_ui.pct_label, LV_ALIGN_CENTER, 0, -20);

    s_ui.mode_label = lv_label_create(card);
    lv_obj_set_style_text_color(s_ui.mode_label, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.mode_label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.mode_label, LV_ALIGN_BOTTOM_MID, 0, -20);

    // ---------------- 滑条 ----------------
    constexpr int kSliderY = 350;
    lv_obj_t *slider = lv_slider_create(scr);
    s_ui.slider = slider;
    lv_obj_set_size(slider, 600, 36);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, kSliderY);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorSliderTrack),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorAccent),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 14, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 18, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 18, LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider, on_slider_value_changed, LV_EVENT_VALUE_CHANGED,
                        nullptr);
    screen_swipe_back_ignore(slider, true);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, I18n::T("拖动滑条调整振幅"));
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, kSliderY + 50);

    // ---------------- 预设模式按钮（2 行 x 3 列） ----------------
    constexpr int kButtonsRowH = 80;
    constexpr int kButtonsGap = 16;
    constexpr int kButtonsCols = 3;
    constexpr int kButtonW =
        (kPanelSize - 60 - (kButtonsCols - 1) * kButtonsGap) / kButtonsCols;
    constexpr int kButtonsTop = 460;

    for (int i = 0; i < kPresetCount; i++) {
        const int row = i / kButtonsCols;
        const int col = i % kButtonsCols;
        const int x = 30 + col * (kButtonW + kButtonsGap);
        const int y = kButtonsTop + row * (kButtonsRowH + kButtonsGap);

        lv_obj_t *btn = lv_button_create(scr);
        s_ui.preset_btns[i] = btn;
        lv_obj_set_size(btn, kButtonW, kButtonsRowH);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_radius(btn, 24, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(kColorBtn), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, on_preset_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(
                                static_cast<intptr_t>(i)));
        screen_swipe_back_ignore(btn, true);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, I18n::T(kPresets[i].label));
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_center(lbl);
    }

    // ---------------- 默认状态：关闭 ----------------
    s_slider_pct = 0;
    s_mode = Mode::kManual;
    s_active_preset = 0;
    refresh_pct_label();
    refresh_mode_label();
    refresh_preset_buttons();
    apply_duty_pct(0);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    return scr;
}

lv_obj_t *VibrateScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void VibrateScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: vibrate_screen");
        MotorInitOnce();
        apply_duty_pct(0);
    } else {
        ESP_LOGI(TAG, "unload: vibrate_screen");
        shutdown_pwm();
    }
}
