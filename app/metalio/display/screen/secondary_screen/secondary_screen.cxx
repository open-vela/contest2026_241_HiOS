/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * SecondaryScreen — ported from MetalioClaw4
 * main/display/screen/secondary_screen/secondary_screen.cc.
 *
 * Adaptations:
 *   - usb_extend_screen_* → stubbed (NuttX has no USB extend screen yet)
 *   - usb_extend_prefs_* → Settings class (file-based)
 *   - esp_lv_adapter_lock/unlock → lv_lock()/lv_unlock()
 *   - esp_lcd_touch/touch_feed → stubbed
 *   - DISPLAY_WIDTH/HEIGHT → 720
 *   - xTaskCreate → std::thread
 *   - All LVGL UI code preserved
 */

#include "secondary_screen.h"
#include "screen_util.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "esp_err_shim.h"
#include "settings.h"
#include "home_screen/home_screen.h"

#include <cstdio>
#include <thread>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

/* ---------------------------------------------------------------------------
 * USB extend screen stubs — TODO(openvela): implement with NuttX USB device
 * ------------------------------------------------------------------------- */
namespace usb_extend
{
static bool s_running = false;
static void (*s_stopped_cb)(void *) = nullptr;
static void *s_stopped_ctx = nullptr;

inline bool is_running() { return s_running; }
inline void start() { s_running = true; }
inline void stop()
{
    s_running = false;
    if (s_stopped_cb)
        s_stopped_cb(s_stopped_ctx);
}
inline void set_stopped_cb(void (*cb)(void *), void *ctx)
{
    s_stopped_cb = cb;
    s_stopped_ctx = ctx;
}

/* Preferences — backed by Settings class */
static Settings &prefs()
{
    static Settings s("usb_extend", true);
    return s;
}
inline int get_jpeg_quality() { return prefs().GetInt("jpeg_quality", 5); }
inline void set_jpeg_quality(int v) { prefs().SetInt("jpeg_quality", v); }
inline int get_max_fps() { return prefs().GetInt("max_fps", 30); }
inline void set_max_fps(int v) { prefs().SetInt("max_fps", v); }
inline int get_frame_limit_b() { return prefs().GetInt("frame_limit_kb", 100) * 1024; }
inline void set_frame_limit_b(int b) { prefs().SetInt("frame_limit_kb", b / 1024); }
inline void load() { /* Settings auto-loads from file */ }
} // namespace usb_extend

namespace
{
constexpr const char *TAG = "SecondaryScreen";
constexpr int kPanelSize = 720;
constexpr int kHeaderH = 90;
constexpr int kBackBtnSize = 72;
constexpr int kHeaderSidePad = 16;
constexpr int kTabBarH = 56;
constexpr int kSidePad = 24;
constexpr int kBtnH = 72;
constexpr int kFooterPad = 16;
constexpr int kStatusAreaH = 48;
constexpr int kFooterH = kFooterPad + kStatusAreaH + 8 + kBtnH + kFooterPad;
constexpr int kBodyH = kPanelSize - kHeaderH;
constexpr int kTabViewH = kBodyH - kFooterH;
constexpr int kContentW = kPanelSize - kSidePad * 2;

constexpr uint32_t kColorBg = 0x101418;
constexpr uint32_t kColorTabBar = 0x12151C;
constexpr uint32_t kColorText = 0xF2F4F7;
constexpr uint32_t kColorSubtle = 0x98A2B3;
constexpr uint32_t kColorBody = 0xD0D5DD;
constexpr uint32_t kColorAccent = 0x2E90FA;

struct UiState
{
    lv_obj_t *screen = nullptr;
    lv_obj_t *tabview = nullptr;
    lv_obj_t *status_lbl = nullptr;
    lv_obj_t *start_btn = nullptr;
    lv_obj_t *start_lbl = nullptr;
    lv_obj_t *jpg_lbl = nullptr;
    lv_obj_t *fps_lbl = nullptr;
    lv_obj_t *frame_lbl = nullptr;
    lv_obj_t *jpg_slider = nullptr;
    lv_obj_t *fps_slider = nullptr;
    lv_obj_t *frame_slider = nullptr;
    bool starting = false;
};

UiState s_ui;

void NavigateHome()
{
    HomeScreen::SwitchToHome();
}

void OnBackClicked(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    NavigateHome();
}

void FormatJpgLabel(char *buf, size_t n, int v)
{
    std::snprintf(buf, n, "%s: %d", I18n::T("JPEG 画质"), v);
}

void FormatFpsLabel(char *buf, size_t n, int v)
{
    std::snprintf(buf, n, "%s: %d", I18n::T("最大帧率"), v);
}

void FormatFrameLabel(char *buf, size_t n, int bytes)
{
    std::snprintf(buf, n, "%s: %d KB", I18n::T("单帧上限"), bytes / 1024);
}

void RefreshSettingsLabels()
{
    char buf[64];
    if (s_ui.jpg_lbl)
    {
        FormatJpgLabel(buf, sizeof(buf), usb_extend::get_jpeg_quality());
        lv_label_set_text(s_ui.jpg_lbl, buf);
    }
    if (s_ui.fps_lbl)
    {
        FormatFpsLabel(buf, sizeof(buf), usb_extend::get_max_fps());
        lv_label_set_text(s_ui.fps_lbl, buf);
    }
    if (s_ui.frame_lbl)
    {
        FormatFrameLabel(buf, sizeof(buf), usb_extend::get_frame_limit_b());
        lv_label_set_text(s_ui.frame_lbl, buf);
    }
}

void SetStatusVisible(bool visible)
{
    if (s_ui.status_lbl == nullptr)
        return;
    if (visible)
        lv_obj_remove_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);
}

void RefreshUi()
{
    if (s_ui.status_lbl == nullptr || s_ui.start_lbl == nullptr)
        return;
    const bool running = usb_extend::is_running();
    if (running)
    {
        lv_label_set_text(s_ui.status_lbl,
                          I18n::T("副屏运行中。短按电源键可退出；请在电脑上将显示模式设为「扩展」。"));
        SetStatusVisible(true);
        lv_label_set_text(s_ui.start_lbl, I18n::T("关闭副屏"));
    }
    else if (s_ui.starting)
    {
        lv_label_set_text(s_ui.status_lbl, I18n::T("正在开启副屏…"));
        SetStatusVisible(true);
        lv_label_set_text(s_ui.start_lbl, I18n::T("开启中…"));
    }
    else
    {
        lv_label_set_text(s_ui.status_lbl, "");
        SetStatusVisible(false);
        lv_label_set_text(s_ui.start_lbl, I18n::T("开启副屏"));
    }
    RefreshSettingsLabels();
}

void OnStoppedUi(void * /*ctx*/)
{
    auto fn = [](void *) {
        lv_lock();
        s_ui.starting = false;
        RefreshUi();
        lv_unlock();
    };
    lv_async_call(fn, nullptr);
}

void StartWorker()
{
    usb_extend::start();
    auto done = [](void *p) {
        (void)p;
        lv_lock();
        s_ui.starting = false;
        RefreshUi();
        lv_unlock();
    };
    lv_async_call(done, nullptr);
}

void OnStartBtn(lv_event_t *e)
{
    (void)e;
    if (s_ui.starting)
        return;
    if (usb_extend::is_running())
    {
        usb_extend::stop();
        RefreshUi();
        return;
    }
    s_ui.starting = true;
    RefreshUi();
    std::thread(StartWorker).detach();
}

void OnJpgSlider(lv_event_t *e)
{
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    int v = static_cast<int>(lv_slider_get_value(slider));
    usb_extend::set_jpeg_quality(v);
    RefreshSettingsLabels();
}

void OnFpsSlider(lv_event_t *e)
{
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    int v = static_cast<int>(lv_slider_get_value(slider));
    usb_extend::set_max_fps(v);
    RefreshSettingsLabels();
}

void OnFrameSlider(lv_event_t *e)
{
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    int kb = static_cast<int>(lv_slider_get_value(slider));
    usb_extend::set_frame_limit_b(kb * 1024);
    RefreshSettingsLabels();
}

lv_obj_t *MakeSliderRow(lv_obj_t *parent, int min_v, int max_v, int cur_v,
                        lv_event_cb_t cb, lv_obj_t **out_lbl,
                        lv_obj_t **out_slider)
{
    lv_obj_t *row = lv_obj_create(parent);
    screen_strip_obj_chrome(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xE4E7EC), LV_PART_MAIN);
    *out_lbl = lbl;

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_slider_set_range(slider, min_v, max_v);
    lv_slider_set_value(slider, cur_v, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    screen_swipe_back_ignore(slider, true);
    *out_slider = slider;
    return row;
}

void BuildMainTab(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, kSidePad, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);

    auto add_step = [&](const char *index, const char *msgid, bool emphasize) {
        lv_obj_t *row = lv_obj_create(tab);
        screen_strip_obj_chrome(row);
        lv_obj_set_width(row, kContentW);
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *idx = lv_label_create(row);
        lv_label_set_text(idx, index);
        lv_obj_set_style_text_font(idx, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(idx, lv_color_hex(kColorAccent), LV_PART_MAIN);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, kContentW - 36);
        lv_label_set_text(lbl, I18n::T(msgid));
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            lbl, lv_color_hex(emphasize ? kColorText : kColorBody), LV_PART_MAIN);
        lv_obj_set_style_text_line_space(lbl, 6, LV_PART_MAIN);
    };

    add_step("1", "使用前请先在 Windows 安装 USB 扩展屏驱动。", false);
    add_step("2",
             "请到本项目 GitHub 仓库的 secondary_screen 文件夹下载：\n"
             "xfz1986_usb_graphic_250224_rc_sign.exe",
             true);
    add_step("3",
             "安装驱动后用 USB 连接电脑，点击下方开启；在 Windows「显示设置」中选择"
             "「扩展」。支持触摸，支持播放电脑声音。",
             false);
    add_step("4", "分辨率 720×720。开启后短按电源键可退出副屏。", true);
}

void BuildSettingsTab(lv_obj_t *tab)
{
    usb_extend::load();

    lv_obj_set_style_pad_all(tab, kSidePad, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);

    lv_obj_t *hint = lv_label_create(tab);
    lv_label_set_text(hint, I18n::T("修改后下次「开启副屏」生效"));
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);

    MakeSliderRow(tab, 1, 10, usb_extend::get_jpeg_quality(), OnJpgSlider,
                  &s_ui.jpg_lbl, &s_ui.jpg_slider);
    MakeSliderRow(tab, 1, 60, usb_extend::get_max_fps(), OnFpsSlider,
                  &s_ui.fps_lbl, &s_ui.fps_slider);
    MakeSliderRow(tab, 32, 300, usb_extend::get_frame_limit_b() / 1024,
                  OnFrameSlider, &s_ui.frame_lbl, &s_ui.frame_slider);

    RefreshSettingsLabels();
}

void BuildHeader(lv_obj_t *parent)
{
    lv_obj_t *header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, kHeaderSidePad, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        back, lv_color_hex(0xFFFFFF),
        static_cast<lv_style_selector_t>(LV_PART_MAIN | LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(
        back, LV_OPA_20,
        static_cast<lv_style_selector_t>(LV_PART_MAIN | LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("副屏"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID,
                 kHeaderSidePad + kBackBtnSize + kHeaderSidePad, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);
}

void BuildFooter(lv_obj_t *parent)
{
    lv_obj_t *footer = lv_obj_create(parent);
    screen_strip_obj_chrome(footer);
    lv_obj_set_size(footer, kPanelSize, kFooterH);
    lv_obj_set_pos(footer, 0, kHeaderH + kTabViewH);
    lv_obj_set_style_bg_color(footer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(footer, kSidePad, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(footer, kFooterPad, LV_PART_MAIN);
    lv_obj_set_style_pad_row(footer, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_CLICKABLE);

    s_ui.status_lbl = lv_label_create(footer);
    lv_obj_set_width(s_ui.status_lbl, kContentW);
    lv_label_set_long_mode(s_ui.status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.status_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.status_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_ui.status_lbl, 4, LV_PART_MAIN);
    lv_label_set_text(s_ui.status_lbl, "");
    lv_obj_add_flag(s_ui.status_lbl, LV_OBJ_FLAG_HIDDEN);

    s_ui.start_btn = lv_button_create(footer);
    lv_obj_set_size(s_ui.start_btn, kContentW, kBtnH);
    lv_obj_set_style_radius(s_ui.start_btn, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.start_btn, lv_color_hex(kColorAccent),
                              LV_PART_MAIN);
    s_ui.start_lbl = lv_label_create(s_ui.start_btn);
    lv_obj_set_style_text_font(s_ui.start_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(s_ui.start_lbl);
    lv_obj_add_event_cb(s_ui.start_btn, OnStartBtn, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(s_ui.start_btn, true);
}

void BuildTabView(lv_obj_t *parent)
{
    lv_obj_t *tv = lv_tabview_create(parent);
    s_ui.tabview = tv;
    lv_obj_set_size(tv, kPanelSize, kTabViewH);
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
                              static_cast<lv_style_selector_t>(LV_PART_ITEMS |
                                                               LV_STATE_CHECKED));
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER,
                            static_cast<lv_style_selector_t>(LV_PART_ITEMS |
                                                             LV_STATE_CHECKED));
    lv_obj_set_style_text_color(
        bar, lv_color_hex(kColorText),
        static_cast<lv_style_selector_t>(LV_PART_ITEMS | LV_STATE_CHECKED));

    lv_obj_t *content = lv_tabview_get_content(tv);
    screen_swipe_back_ignore(content, true);

    lv_obj_t *tab_main = lv_tabview_add_tab(tv, I18n::T("副屏"));
    BuildMainTab(tab_main);

    lv_obj_t *tab_settings = lv_tabview_add_tab(tv, I18n::T("设置"));
    BuildSettingsTab(tab_settings);
}

void OnUnload(lv_event_t *e)
{
    (void)e;
    if (usb_extend::is_running())
        usb_extend::stop();
    s_ui = {};
}

} // namespace

lv_obj_t *SecondaryScreen::CreateStatic()
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildTabView(scr);
    BuildFooter(scr);

    s_ui.screen = scr;
    s_ui.starting = false;
    usb_extend::set_stopped_cb(OnStoppedUi, nullptr);
    RefreshUi();

    screen_attach_swipe_back(scr, NavigateHome);
    lv_obj_add_event_cb(scr, OnUnload, LV_EVENT_SCREEN_UNLOADED, nullptr);
    return scr;
}

lv_obj_t *SecondaryScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void SecondaryScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD)
    {
        ESP_LOGI(TAG, "load");
    }
    else
    {
        ESP_LOGI(TAG, "unload");
        if (usb_extend::is_running())
            usb_extend::stop();
    }
}
