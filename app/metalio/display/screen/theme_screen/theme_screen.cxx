/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ThemeScreen — ported from MetalioClaw4
 * main/display/screen/theme_screen/theme_screen.cc.
 *
 * ThemeManager on openvela persists home icon pack id via theme_manager.h
 * (NVS "ui"/"theme_id"), matching MetalioClaw4.
 */

#include "theme_screen.h"
#include "theme_manager.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "home_screen/home_screen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char *TAG = "ThemeScreen";

constexpr int kPanelSize = 720;
constexpr int kHeaderH = 90;
constexpr int kPad = 16;
constexpr int kBackBtnSize = 72;

constexpr int kCardSize = 180;
constexpr int kCardColGap = 60;
constexpr int kCardRowGap = 40;
constexpr int kCardRadius = 40;
constexpr int kCardBorder = 3;
constexpr int kCardLabelH = 36;
constexpr int kCardLabelPad = 12;
constexpr int kGridCols = 2;

constexpr uint32_t kColorBg = 0x000000;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorSubtle = 0x9CA3AF;
constexpr uint32_t kColorActive = 0x3B82F6;
constexpr uint32_t kColorDialogBg = 0x1B2030;
constexpr uint32_t kColorBtnPrimaryBg = 0x3B82F6;
constexpr uint32_t kColorBtnCancelBg = 0x2A2F3A;

struct UiState {
    lv_obj_t *screen = nullptr;
    int current_theme = ThemeManager::kDefaultThemeId;
};
UiState s_ui;

struct DialogState {
    lv_obj_t *mask = nullptr;
    int target_theme = 0;
};
DialogState s_dlg;

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state)
{
    return static_cast<lv_style_selector_t>(part | state);
}

void OnSwipeBack();
void OpenConfirmDialog(int theme_id);
void CloseDialog();
void GoHomeWithCurrentTheme();

void OnCardClicked(lv_event_t *e)
{
    const int target = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (target < ThemeManager::kMinThemeId ||
        target > ThemeManager::kMaxThemeId) {
        return;
    }
    if (target == s_ui.current_theme) {
        ESP_LOGI(TAG, "theme%d already active, skip", target);
        return;
    }
    OpenConfirmDialog(target);
}

lv_obj_t *BuildThemeCard(lv_obj_t *parent, int theme_id, int x, int y_top)
{
    const bool active = (theme_id == s_ui.current_theme);

    lv_obj_t *slot = lv_obj_create(parent);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, kCardSize, kCardSize + kCardLabelH + kCardLabelPad);
    lv_obj_set_pos(slot, x, y_top);
    lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(slot);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardSize, kCardSize);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    const int icon_size = kCardSize - kCardBorder * 2;
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(card, kCardRadius, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, kCardBorder, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(kColorActive),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, active ? LV_OPA_COVER : LV_OPA_TRANSP,
                                LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(card, true, LV_PART_MAIN);

    char src[64];
    std::snprintf(src, sizeof(src), "A:ic_app_home_theme%d_theme.spng",
                  theme_id);
    lv_obj_t *preview = lv_image_create(card);
    lv_image_set_src(preview, src);
    lv_obj_set_size(preview, icon_size, icon_size);
    lv_obj_center(preview);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, OnCardClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(
                            static_cast<intptr_t>(theme_id)));

    lv_obj_t *lbl = lv_label_create(slot);
    char buf[32];
    if (active) {
        std::snprintf(buf, sizeof(buf), I18n::T("主题 %d  当前"), theme_id);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorActive),
                                    LV_PART_MAIN);
    } else {
        std::snprintf(buf, sizeof(buf), I18n::T("主题 %d"), theme_id);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
    }
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    return slot;
}

void BuildHeader(lv_obj_t *parent)
{
    lv_obj_t *back_btn = lv_button_create(parent);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, kBackBtnSize, kBackBtnSize);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, kPad + 8, kPad + 8);
    screen_swipe_back_ignore(back_btn, true);

    lv_obj_t *back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);
    lv_obj_add_event_cb(
        back_btn, [](lv_event_t *) { OnSwipeBack(); }, LV_EVENT_CLICKED,
        nullptr);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, I18n::T("主题"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kPad + 20);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);
}

void BuildContent(lv_obj_t *parent)
{
    constexpr int kCellH = kCardSize + kCardLabelH + kCardLabelPad;
    constexpr int kHintReservedH = 60;

    const int cols = std::min(kGridCols, ThemeManager::kThemeCount);
    const int rows = (ThemeManager::kThemeCount + cols - 1) / cols;

    const int grid_w = cols * kCardSize + (cols - 1) * kCardColGap;
    const int grid_h = rows * kCellH + (rows - 1) * kCardRowGap;

    const int avail_top = kHeaderH;
    const int avail_bottom = kPanelSize - kHintReservedH;
    const int avail_h = avail_bottom - avail_top;

    const int origin_x = (kPanelSize - grid_w) / 2;
    int origin_y = avail_top + (avail_h - grid_h) / 2;
    if (origin_y < avail_top + 16) {
        origin_y = avail_top + 16;
    }

    for (int i = 0; i < ThemeManager::kThemeCount; ++i) {
        const int theme_id = ThemeManager::kMinThemeId + i;
        const int col = i % cols;
        const int row = i / cols;
        const int x = origin_x + col * (kCardSize + kCardColGap);
        const int y = origin_y + row * (kCellH + kCardRowGap);
        BuildThemeCard(parent, theme_id, x, y);
    }

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, I18n::T("切换后将立即应用，并返回主页查看新图标"));
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

void OnDialogMaskClicked(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    CloseDialog();
}

void CloseDialog()
{
    if (s_dlg.mask != nullptr) {
        lv_obj_delete(s_dlg.mask);
    }
    s_dlg = DialogState{};
}

void OnCancelClicked(lv_event_t * /*e*/) { CloseDialog(); }

void OnConfirmClicked(lv_event_t * /*e*/)
{
    const int target = s_dlg.target_theme;
    s_dlg = DialogState{};
    ESP_LOGI(TAG, "switch theme -> theme%d (hot apply)", target);
    ThemeManager::SetCurrentThemeId(target);
    s_ui.current_theme = target;
    HomeScreen::ResetToFirstPage();
    GoHomeWithCurrentTheme();
}

void GoHomeWithCurrentTheme()
{    HomeScreen::SwitchToHome();
}

void OpenConfirmDialog(int theme_id)
{
    if (s_dlg.mask != nullptr || s_ui.screen == nullptr) {
        return;
    }
    s_dlg.target_theme = theme_id;

    constexpr int kCardW = 480;
    constexpr int kCardH = 280;
    constexpr int kBtnW = 200;
    constexpr int kBtnH = 80;

    lv_obj_t *mask = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    lv_obj_add_event_cb(mask, OnDialogMaskClicked, LV_EVENT_CLICKED, nullptr);
    s_dlg.mask = mask;

    lv_obj_t *card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorDialogBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    char title_buf[48];
    std::snprintf(title_buf, sizeof(title_buf), I18n::T("切换到主题 %d ?"),
                  theme_id);
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, title_buf);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *desc = lv_label_create(card);
    lv_label_set_text(desc, I18n::T("确定后将立即应用并返回主页"));
    lv_obj_set_style_text_color(desc, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(desc, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, -10);
    lv_obj_remove_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_remove_style_all(cancel);
    lv_obj_set_size(cancel, kBtnW, kBtnH);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(kColorBtnCancelBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(cancel, 16, LV_PART_MAIN);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(cancel, OnCancelClicked, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t *lbl = lv_label_create(cancel);
        lv_label_set_text(lbl, I18n::T("取消"));
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_center(lbl);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *ok = lv_button_create(card);
    lv_obj_remove_style_all(ok);
    lv_obj_set_size(ok, kBtnW, kBtnH);
    lv_obj_set_style_bg_color(ok, lv_color_hex(kColorBtnPrimaryBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ok, 16, LV_PART_MAIN);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(ok, OnConfirmClicked, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t *lbl = lv_label_create(ok);
        lv_label_set_text(lbl, I18n::T("切换"));
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_center(lbl);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }
}

void OnScreenUnloaded(lv_event_t * /*e*/)
{
    s_ui = UiState{};
    s_dlg = DialogState{};
}

void OnSwipeBack() { GoHomeWithCurrentTheme(); }

}  // namespace

lv_obj_t *ThemeScreen::CreateStatic()
{
    ESP_LOGI(TAG, "create theme screen");

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    s_ui.current_theme = ThemeManager::GetCurrentThemeId();

    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildContent(scr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    return scr;
}

lv_obj_t *ThemeScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void ThemeScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load");
    } else {
        ESP_LOGI(TAG, "unload");
    }
}
