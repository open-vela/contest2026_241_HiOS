/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * PinTestScreen — ported from MetalioClaw4
 * main/display/screen/pin_test_screen/pin_test_screen.cc.
 *
 * Adaptations:
 *   - driver/gpio.h → thin inline GPIO shim (can be connected to NuttX
 *     esp32p4 GPIO driver later)
 *   - esp_log.h → esp_log_shim.h
 *   - esp_err_t/ESP_OK/esp_err_to_name → esp_err_shim.h
 *   - HomeScreen::Create() navigation kept (resolved at link time)
 *   - "A:ic_app_back.spng" → text back button (asset may not be available)
 *   - All LVGL UI code preserved verbatim
 */

#include "pin_test_screen.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "esp_err_shim.h"

#include <cstdio>
#include <cstring>
#include <utility>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

/* ---------------------------------------------------------------------------
 * GPIO shim — replaces ESP-IDF driver/gpio.h.
 * TODO(openvela): connect to NuttX esp32p4 GPIO driver
 * (nuttx/arch/risc-v/src/esp32p4/esp32p4_gpio.c) for real hardware access.
 * ------------------------------------------------------------------------- */
namespace gpio_shim
{
enum gpio_mode_t
{
    GPIO_MODE_INPUT = 0,
    GPIO_MODE_OUTPUT = 1,
};

struct gpio_config_t
{
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
};

inline int gpio_config(const gpio_config_t *cfg)
{
    (void)cfg;
    /* TODO: call NuttX esp32p4_gpioinit() */
    return 0; /* ESP_OK */
}

inline int gpio_set_level(int pin, int level)
{
    (void)pin;
    (void)level;
    /* TODO: call NuttX esp32p4_gpiowrite() */
    return 0;
}

inline int gpio_get_level(int pin)
{
    (void)pin;
    /* TODO: call NuttX esp32p4_gpioread() */
    return 0;
}
} // namespace gpio_shim

namespace
{
constexpr const char *TAG = "PinTestScreen";

constexpr int kPanelW = 720;
constexpr int kPanelH = 720;
constexpr int kHeaderH = 96;
constexpr int kFooterH = 96;
constexpr int kBodyY = kHeaderH;
constexpr int kBodyH = kPanelH - kHeaderH - kFooterH;

constexpr int kRowH = 84;
constexpr int kRowGap = 10;
constexpr int kSideMargin = 20;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorCardBg = 0x1B2030;
constexpr uint32_t kColorAccent = 0x3B82F6;
constexpr uint32_t kColorAccentHi = 0x60A5FA;
constexpr uint32_t kColorMuted = 0x2A2F3A;
constexpr uint32_t kColorMutedHi = 0x3A4050;
constexpr uint32_t kColorHigh = 0x10B981;
constexpr uint32_t kColorLow = 0x6B7280;
constexpr uint32_t kColorTextDim = 0x9AA3B2;

enum class Dir : uint8_t
{
    kInput = 0,
    kOutput = 1,
};

struct PinDef
{
    int index;
    const char *label;
};

constexpr PinDef kPins[] = {
    {5, "GPIO5"}, {14, "GPIO14"}, {15, "GPIO15"}, {16, "GPIO16"},
    {17, "GPIO17"}, {18, "GPIO18"}, {19, "GPIO19"}, {20, "GPIO20"},
    {21, "GPIO21"}, {23, "GPIO23"}, {35, "GPIO35"}, {46, "GPIO46"},
    {47, "GPIO47"}, {48, "GPIO48"},
};
constexpr int kPinCount = static_cast<int>(sizeof(kPins) / sizeof(kPins[0]));

struct PinRowUi
{
    lv_obj_t *row = nullptr;
    lv_obj_t *dir_in_btn = nullptr;
    lv_obj_t *dir_in_lbl = nullptr;
    lv_obj_t *dir_out_btn = nullptr;
    lv_obj_t *dir_out_lbl = nullptr;
    lv_obj_t *out_switch = nullptr;
    lv_obj_t *out_state_lbl = nullptr;
    lv_obj_t *in_level_dot = nullptr;
    lv_obj_t *in_level_lbl = nullptr;
};

struct PinState
{
    Dir dir = Dir::kInput;
    bool current_lvl = false;
};

PinRowUi s_rows[kPinCount];
PinState s_state[kPinCount];

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_cycle_btn = nullptr;
lv_obj_t *s_cycle_lbl = nullptr;

lv_timer_t *s_input_poll_timer = nullptr;
lv_timer_t *s_cycle_timer = nullptr;
bool s_cycle_active = false;
bool s_cycle_level = false;

void ApplyPinDir(int i)
{
    const PinDef &p = kPins[i];
    const PinState &s = s_state[i];
    gpio_shim::gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << static_cast<uint64_t>(p.index));
    cfg.mode = (s.dir == Dir::kOutput) ? gpio_shim::GPIO_MODE_OUTPUT : gpio_shim::GPIO_MODE_INPUT;
    cfg.pull_up_en = 0;
    cfg.pull_down_en = 0;
    cfg.intr_type = 0;
    int err = gpio_shim::gpio_config(&cfg);
    if (err != 0)
    {
        ESP_LOGW(TAG, "gpio_config(GPIO%d, %s) failed: %d", p.index,
                 s.dir == Dir::kOutput ? "OUT" : "IN", err);
        return;
    }
    if (s.dir == Dir::kOutput)
    {
        gpio_shim::gpio_set_level(p.index, s.current_lvl ? 1 : 0);
    }
}

void WritePinLevel(int i, bool level)
{
    PinState &s = s_state[i];
    if (s.dir != Dir::kOutput)
        return;
    s.current_lvl = level;
    gpio_shim::gpio_set_level(kPins[i].index, level ? 1 : 0);
}

bool ReadPinLevel(int i)
{
    return gpio_shim::gpio_get_level(kPins[i].index) != 0;
}

void RefreshDirButtons(int i)
{
    const PinRowUi &r = s_rows[i];
    if (r.dir_in_btn == nullptr || r.dir_out_btn == nullptr)
        return;
    const bool in_active = (s_state[i].dir == Dir::kInput);

    lv_obj_set_style_bg_color(r.dir_in_btn,
                              lv_color_hex(in_active ? kColorAccent : kColorMuted), LV_PART_MAIN);
    lv_obj_set_style_border_width(r.dir_in_btn, in_active ? 2 : 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(r.dir_out_btn,
                              lv_color_hex(!in_active ? kColorAccent : kColorMuted), LV_PART_MAIN);
    lv_obj_set_style_border_width(r.dir_out_btn, !in_active ? 2 : 0, LV_PART_MAIN);
}

void RefreshCtrl(int i)
{
    const PinRowUi &r = s_rows[i];
    if (s_state[i].dir == Dir::kOutput)
    {
        if (r.out_switch != nullptr)
        {
            lv_obj_remove_flag(r.out_switch, LV_OBJ_FLAG_HIDDEN);
            if (s_state[i].current_lvl)
                lv_obj_add_state(r.out_switch, LV_STATE_CHECKED);
            else
                lv_obj_remove_state(r.out_switch, LV_STATE_CHECKED);
        }
        if (r.out_state_lbl != nullptr)
        {
            lv_obj_remove_flag(r.out_state_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(r.out_state_lbl,
                              s_state[i].current_lvl ? I18n::T("高") : I18n::T("低"));
            lv_obj_set_style_text_color(
                r.out_state_lbl,
                lv_color_hex(s_state[i].current_lvl ? kColorHigh : kColorLow),
                LV_PART_MAIN);
        }
        if (r.in_level_dot != nullptr)
            lv_obj_add_flag(r.in_level_dot, LV_OBJ_FLAG_HIDDEN);
        if (r.in_level_lbl != nullptr)
            lv_obj_add_flag(r.in_level_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        if (r.out_switch != nullptr)
            lv_obj_add_flag(r.out_switch, LV_OBJ_FLAG_HIDDEN);
        if (r.out_state_lbl != nullptr)
            lv_obj_add_flag(r.out_state_lbl, LV_OBJ_FLAG_HIDDEN);
        if (r.in_level_dot != nullptr)
            lv_obj_remove_flag(r.in_level_dot, LV_OBJ_FLAG_HIDDEN);
        if (r.in_level_lbl != nullptr)
            lv_obj_remove_flag(r.in_level_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

void RefreshInputDisplay(int i, bool lvl)
{
    s_state[i].current_lvl = lvl;
    const PinRowUi &r = s_rows[i];
    if (r.in_level_dot != nullptr)
        lv_obj_set_style_bg_color(r.in_level_dot,
                                  lv_color_hex(lvl ? kColorHigh : kColorLow), LV_PART_MAIN);
    if (r.in_level_lbl != nullptr)
    {
        lv_label_set_text(r.in_level_lbl, lvl ? I18n::T("高") : I18n::T("低"));
        lv_obj_set_style_text_color(r.in_level_lbl,
                                    lv_color_hex(lvl ? kColorHigh : kColorLow), LV_PART_MAIN);
    }
}

void RefreshAllInputs()
{
    for (int i = 0; i < kPinCount; ++i)
    {
        if (s_state[i].dir != Dir::kInput)
            continue;
        RefreshInputDisplay(i, ReadPinLevel(i));
    }
}

void OnCycleTimer(lv_timer_t * /*t*/)
{
    s_cycle_level = !s_cycle_level;
    for (int i = 0; i < kPinCount; ++i)
    {
        if (s_state[i].dir != Dir::kOutput)
            continue;
        WritePinLevel(i, s_cycle_level);
        const PinRowUi &r = s_rows[i];
        if (r.out_switch != nullptr)
        {
            if (s_cycle_level)
                lv_obj_add_state(r.out_switch, LV_STATE_CHECKED);
            else
                lv_obj_remove_state(r.out_switch, LV_STATE_CHECKED);
        }
        if (r.out_state_lbl != nullptr)
        {
            lv_label_set_text(r.out_state_lbl, s_cycle_level ? I18n::T("高") : I18n::T("低"));
            lv_obj_set_style_text_color(r.out_state_lbl,
                                        lv_color_hex(s_cycle_level ? kColorHigh : kColorLow),
                                        LV_PART_MAIN);
        }
    }
}

void StopCycle()
{
    if (s_cycle_timer != nullptr)
    {
        lv_timer_delete(s_cycle_timer);
        s_cycle_timer = nullptr;
    }
    s_cycle_active = false;
    if (s_cycle_btn != nullptr)
    {
        lv_obj_set_style_bg_color(s_cycle_btn, lv_color_hex(kColorMuted),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_width(s_cycle_btn, 0, LV_PART_MAIN);
    }
    if (s_cycle_lbl != nullptr)
        lv_label_set_text(s_cycle_lbl, I18n::T("全部输出 · 周期方波"));
}

void StartCycle()
{
    for (int i = 0; i < kPinCount; ++i)
    {
        s_state[i].dir = Dir::kOutput;
        s_state[i].current_lvl = false;
        ApplyPinDir(i);
        RefreshDirButtons(i);
        RefreshCtrl(i);
    }
    s_cycle_level = false;
    s_cycle_active = true;
    if (s_cycle_btn != nullptr)
    {
        lv_obj_set_style_bg_color(s_cycle_btn, lv_color_hex(kColorAccent),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_color(s_cycle_btn,
                                      lv_color_hex(kColorAccentHi),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_width(s_cycle_btn, 2, LV_PART_MAIN);
    }
    if (s_cycle_lbl != nullptr)
        lv_label_set_text(s_cycle_lbl, I18n::T("周期方波运行中 · 点击停止"));
    if (s_cycle_timer == nullptr)
        s_cycle_timer = lv_timer_create(OnCycleTimer, 1000, nullptr);
}

void OnDirButtonClicked(lv_event_t *e)
{
    const intptr_t encoded =
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    const int pin_idx = static_cast<int>(encoded >> 1);
    const Dir new_dir = (encoded & 1) ? Dir::kOutput : Dir::kInput;
    if (pin_idx < 0 || pin_idx >= kPinCount)
        return;
    if (s_state[pin_idx].dir == new_dir)
        return;
    if (s_cycle_active)
        StopCycle();

    s_state[pin_idx].dir = new_dir;
    s_state[pin_idx].current_lvl = false;
    ApplyPinDir(pin_idx);
    RefreshDirButtons(pin_idx);
    RefreshCtrl(pin_idx);
    if (new_dir == Dir::kInput)
        RefreshInputDisplay(pin_idx, ReadPinLevel(pin_idx));
}

void OnOutSwitchChanged(lv_event_t *e)
{
    const int pin_idx =
        static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (pin_idx < 0 || pin_idx >= kPinCount)
        return;
    if (s_cycle_active)
        StopCycle();
    lv_obj_t *sw = lv_event_get_target_obj(e);
    const bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    WritePinLevel(pin_idx, checked);
    const PinRowUi &r = s_rows[pin_idx];
    if (r.out_state_lbl != nullptr)
    {
        lv_label_set_text(r.out_state_lbl, checked ? I18n::T("高") : I18n::T("低"));
        lv_obj_set_style_text_color(r.out_state_lbl,
                                    lv_color_hex(checked ? kColorHigh : kColorLow), LV_PART_MAIN);
    }
}

void OnCycleBtnClicked(lv_event_t * /*e*/)
{
    if (s_cycle_active)
        StopCycle();
    else
        StartCycle();
}

void OnInputPollTimer(lv_timer_t * /*t*/)
{
    RefreshAllInputs();
}

lv_obj_t *CreatePinRow(lv_obj_t *parent, int idx)
{
    constexpr int kRowInnerPad = 16;
    constexpr int kLabelW = 120;
    constexpr int kDirSegW = 180;
    constexpr int kDirBtnW = (kDirSegW - 6) / 2;
    constexpr int kCtrlW = 320;

    lv_obj_t *row = lv_obj_create(parent);
    screen_strip_obj_chrome(row);
    lv_obj_set_size(row, kPanelW - 2 * kSideMargin, kRowH);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorCardBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, kRowInnerPad, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, kPins[idx].label);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_width(label, kLabelW);

    lv_obj_t *seg = lv_obj_create(row);
    screen_strip_obj_chrome(seg);
    lv_obj_set_size(seg, kDirSegW, kRowH - 24);
    lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(seg, 6, LV_PART_MAIN);

    auto make_dir_btn = [&](const char *text, bool is_out) {
        lv_obj_t *b = lv_button_create(seg);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, kDirBtnW, kRowH - 28);
        lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(kColorMuted), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(b, lv_color_hex(kColorAccentHi),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);

        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, text);
        lv_obj_center(lbl);
        lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);

        const intptr_t enc =
            static_cast<intptr_t>((idx << 1) | (is_out ? 1 : 0));
        lv_obj_add_event_cb(b, OnDirButtonClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(enc));
        return std::make_pair(b, lbl);
    };

    auto in_pair = make_dir_btn(I18n::T("输入"), false);
    auto out_pair = make_dir_btn(I18n::T("输出"), true);

    lv_obj_t *ctrl = lv_obj_create(row);
    screen_strip_obj_chrome(ctrl);
    lv_obj_set_size(ctrl, kCtrlW, kRowH - 16);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl, 14, LV_PART_MAIN);

    lv_obj_t *out_state = lv_label_create(ctrl);
    lv_label_set_text(out_state, I18n::T("低"));
    lv_obj_set_style_text_color(out_state, lv_color_hex(kColorLow),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(out_state, &font_puhui_30_4, LV_PART_MAIN);

    lv_obj_t *out_sw = lv_switch_create(ctrl);
    lv_obj_set_size(out_sw, 88, 44);
    lv_obj_set_style_bg_color(out_sw, lv_color_hex(kColorMuted), LV_PART_MAIN);
    lv_obj_set_style_bg_color(out_sw, lv_color_hex(kColorHigh),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(out_sw, OnOutSwitchChanged, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void *>(
                            static_cast<intptr_t>(idx)));

    lv_obj_t *in_dot = lv_obj_create(ctrl);
    lv_obj_remove_style_all(in_dot);
    lv_obj_set_size(in_dot, 26, 26);
    lv_obj_set_style_radius(in_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(in_dot, lv_color_hex(kColorLow), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(in_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(in_dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *in_lvl_lbl = lv_label_create(ctrl);
    lv_label_set_text(in_lvl_lbl, I18n::T("低"));
    lv_obj_set_style_text_color(in_lvl_lbl, lv_color_hex(kColorLow),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(in_lvl_lbl, &font_puhui_30_4, LV_PART_MAIN);

    PinRowUi &ui = s_rows[idx];
    ui.row = row;
    ui.dir_in_btn = in_pair.first;
    ui.dir_in_lbl = in_pair.second;
    ui.dir_out_btn = out_pair.first;
    ui.dir_out_lbl = out_pair.second;
    ui.out_switch = out_sw;
    ui.out_state_lbl = out_state;
    ui.in_level_dot = in_dot;
    ui.in_level_lbl = in_lvl_lbl;

    return row;
}

void OnSwipeBack()
{
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr)
        lv_indev_wait_release(indev);
    HomeScreen::SwitchToHome();
}

void OnBackBtnClicked(lv_event_t * /*e*/) { OnSwipeBack(); }

void OnScreenUnloaded(lv_event_t * /*e*/)
{
    if (s_input_poll_timer != nullptr)
    {
        lv_timer_delete(s_input_poll_timer);
        s_input_poll_timer = nullptr;
    }
    if (s_cycle_timer != nullptr)
    {
        lv_timer_delete(s_cycle_timer);
        s_cycle_timer = nullptr;
    }
    s_cycle_active = false;
    s_cycle_level = false;
    s_screen = nullptr;
    s_cycle_btn = nullptr;
    s_cycle_lbl = nullptr;
    for (int i = 0; i < kPinCount; ++i)
        s_rows[i] = PinRowUi{};
}

} // namespace

lv_obj_t *PinTestScreen::CreateStatic()
{
    ESP_LOGI(TAG, "create pin test screen");

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(scr);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int kBackBtnSize = 72;
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
    lv_obj_add_event_cb(back, OnBackBtnClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    /* Use text label instead of image asset */
    lv_obj_t *back_icon = lv_label_create(back);
    lv_label_set_text(back_icon, "<");
    lv_obj_set_style_text_color(back_icon, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(back_icon, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("引脚测试"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    lv_obj_t *hint = lv_label_create(header);
    lv_label_set_text(hint, I18n::T("切换方向 · 控制电平"));
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -20, 0);

    lv_obj_t *body = lv_obj_create(scr);
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kPanelW, kBodyH);
    lv_obj_set_pos(body, 0, kBodyY);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(body, kSideMargin, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(body, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, kRowGap, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < kPinCount; ++i)
    {
        s_state[i] = PinState{};
        s_rows[i] = PinRowUi{};
    }
    s_cycle_active = false;
    s_cycle_level = false;

    for (int i = 0; i < kPinCount; ++i)
    {
        CreatePinRow(body, i);
        ApplyPinDir(i);
        RefreshDirButtons(i);
        RefreshCtrl(i);
    }
    RefreshAllInputs();

    lv_obj_t *footer = lv_obj_create(scr);
    screen_strip_obj_chrome(footer);
    lv_obj_set_size(footer, kPanelW, kFooterH);
    lv_obj_set_pos(footer, 0, kPanelH - kFooterH);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cycle_btn = lv_button_create(footer);
    s_cycle_btn = cycle_btn;
    lv_obj_remove_style_all(cycle_btn);
    lv_obj_set_size(cycle_btn, kPanelW - 2 * kSideMargin, kFooterH - 20);
    lv_obj_align(cycle_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(cycle_btn, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cycle_btn, lv_color_hex(kColorMuted),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cycle_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(cycle_btn, lv_color_hex(kColorAccentHi),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(cycle_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cycle_btn, lv_color_hex(kColorMutedHi),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(cycle_btn, OnCycleBtnClicked, LV_EVENT_CLICKED,
                        nullptr);

    lv_obj_t *cycle_lbl = lv_label_create(cycle_btn);
    s_cycle_lbl = cycle_lbl;
    lv_label_set_text(cycle_lbl, I18n::T("全部输出 · 周期方波"));
    lv_obj_set_style_text_color(cycle_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(cycle_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(cycle_lbl);

    s_input_poll_timer = lv_timer_create(OnInputPollTimer, 200, nullptr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}

lv_obj_t *PinTestScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void PinTestScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD)
    {
        ESP_LOGI(TAG, "load: pin_test_screen");
    }
    else
    {
        ESP_LOGI(TAG, "unload: pin_test_screen");
        if (s_cycle_timer != nullptr)
        {
            lv_timer_delete(s_cycle_timer);
            s_cycle_timer = nullptr;
        }
        if (s_input_poll_timer != nullptr)
        {
            lv_timer_delete(s_input_poll_timer);
            s_input_poll_timer = nullptr;
        }
        s_cycle_active = false;
    }
}
