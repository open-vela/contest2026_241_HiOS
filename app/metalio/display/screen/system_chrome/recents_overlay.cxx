/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Recents overlay — HyperOS-style background app cards.
 * Fullscreen apps are single-shot on this port, so cards relaunch by id.
 */

#include "recents_overlay.h"
#include "navigation_index.h"

#include "home_screen/home_screen.h"
#include "i18n.h"
#include "screen_util.h"
#include "idle_power_policy/idle_power_policy.h"
#include "esp_log_shim.h"

#include <cstdio>
#include <cstring>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char *TAG = "Recents";
constexpr int kPanel = 720;
constexpr int kMaxRecents = 8;
constexpr uint32_t kCardBg = 0x1C1F28;
constexpr uint32_t kText = 0xF2F3F5;
constexpr uint32_t kMuted = 0x9AA0A8;

struct RecentEntry {
    char id[32];
    char name[40];
    char icon[56];
    bool used;
};

RecentEntry s_entries[kMaxRecents];
int s_count = 0;
char s_switch_order[kMaxRecents][32] = {};
int s_switch_count = 0;
int s_switch_cursor = 0;
char s_switch_expected[32] = {};
bool s_switch_pending = false;
lv_obj_t *s_root = nullptr;

int FindIndex(const char *id)
{
    if (id == nullptr) return -1;
    for (int i = 0; i < s_count; ++i) {
        if (std::strcmp(s_entries[i].id, id) == 0) return i;
    }
    return -1;
}

void OnMaskClick(lv_event_t *e)
{
    IdlePower_NotifyActivity();
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    Recents_Hide();
}

void OnClearAll(lv_event_t * /*e*/)
{
    Recents_Clear();
    Recents_Hide();
}

char s_pending_launch_id[32] = {};

void OnLaunchAppAsync(void * /*user_data*/)
{
    if (s_pending_launch_id[0] == '\0') return;
    char id[32];
    std::snprintf(id, sizeof(id), "%s", s_pending_launch_id);
    s_pending_launch_id[0] = '\0';
    ESP_LOGI(TAG, "reopen app '%s'", id);
    if (s_switch_pending)
        std::snprintf(s_switch_expected, sizeof(s_switch_expected), "%s", id);
    s_switch_pending = false;
    if (!HomeScreen::LaunchApp(id)) {
        s_switch_expected[0] = '\0';
        s_switch_count = 0;
    }
}

void OnCardClick(lv_event_t *e)
{
    const intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (idx < 0 || idx >= s_count) return;
    std::snprintf(s_pending_launch_id, sizeof(s_pending_launch_id), "%s",
                  s_entries[idx].id);
    Recents_Hide();
    if (lv_async_call(OnLaunchAppAsync, nullptr) != LV_RESULT_OK) {
        OnLaunchAppAsync(nullptr);
    }
}

void OnHomeClick(lv_event_t * /*e*/)
{
    Recents_Hide();
    HomeScreen::SwitchToHome();
}

void BuildUi(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, kPanel, kPanel);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x0A0B10), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(s_root, OnMaskClick, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(s_root, true);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, I18n::T("后台管理"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    lv_obj_t *clear_btn = lv_obj_create(s_root);
    lv_obj_remove_style_all(clear_btn);
    lv_obj_set_size(clear_btn, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(0x2A2E38), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clear_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(clear_btn, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(clear_btn, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(clear_btn, 10, LV_PART_MAIN);
    lv_obj_align(clear_btn, LV_ALIGN_TOP_RIGHT, -28, 36);
    lv_obj_add_flag(clear_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clear_btn, OnClearAll, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(clear_btn, true);

    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, I18n::T("全部清除"));
    lv_obj_set_style_text_font(clear_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(clear_lbl, lv_color_hex(kMuted), LV_PART_MAIN);
    lv_obj_center(clear_lbl);
    lv_obj_remove_flag(clear_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *list = lv_obj_create(s_root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, kPanel - 48, 520);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 16, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(list, true);

    if (s_count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, I18n::T("暂无后台应用"));
        lv_obj_set_style_text_font(empty, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(empty, lv_color_hex(kMuted), LV_PART_MAIN);
    } else {
        for (int i = 0; i < s_count; ++i) {
            lv_obj_t *card = lv_obj_create(list);
            lv_obj_remove_style_all(card);
            lv_obj_set_size(card, kPanel - 64, 112);
            lv_obj_set_style_bg_color(card, lv_color_hex(kCardBg), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(card, 28, LV_PART_MAIN);
            lv_obj_set_style_pad_hor(card, 24, LV_PART_MAIN);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(card, OnCardClick, LV_EVENT_CLICKED,
                                reinterpret_cast<void *>(static_cast<intptr_t>(i)));
            screen_swipe_back_ignore(card, true);

            {
                lv_obj_t *icon_box = lv_obj_create(card);
                lv_obj_remove_style_all(icon_box);
                lv_obj_set_size(icon_box, 72, 72);
                lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x2A2E38),
                                          LV_PART_MAIN);
                lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_radius(icon_box, 18, LV_PART_MAIN);
                lv_obj_set_style_clip_corner(icon_box, true, LV_PART_MAIN);
                lv_obj_set_style_border_width(icon_box, 0, LV_PART_MAIN);
                lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);

                // Package icons and several built-in icons have no file path.
                // Always resolve by application id, just as the launcher does.
                lv_obj_t *icon = HomeScreen::CreateAppIcon(icon_box, s_entries[i].id, 72);
                if (icon) lv_obj_center(icon);
            }

            lv_obj_t *name = lv_label_create(card);
            lv_label_set_text(name, s_entries[i].name);
            lv_obj_set_style_text_font(name, &font_puhui_30_4, LV_PART_MAIN);
            lv_obj_set_style_text_color(name, lv_color_hex(kText), LV_PART_MAIN);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 96, 0);
            lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    lv_obj_t *home_btn = lv_obj_create(s_root);
    lv_obj_remove_style_all(home_btn);
    lv_obj_set_size(home_btn, 200, 56);
    lv_obj_set_style_bg_color(home_btn, lv_color_hex(0x3482FF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(home_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(home_btn, 28, LV_PART_MAIN);
    lv_obj_align(home_btn, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_obj_add_flag(home_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(home_btn, OnHomeClick, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(home_btn, true);

    lv_obj_t *home_lbl = lv_label_create(home_btn);
    lv_label_set_text(home_lbl, I18n::T("返回桌面"));
    lv_obj_set_style_text_font(home_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(home_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(home_lbl);
    lv_obj_remove_flag(home_lbl, LV_OBJ_FLAG_CLICKABLE);

    ESP_LOGI(TAG, "show recents count=%d", s_count);
}

}  // namespace

void Recents_NotifyOpened(const char *app_id, const char *app_name,
                          const char *icon_path)
{
    if (app_id == nullptr || app_id[0] == '\0') return;
    // "home" is the smart-home application id (Hi 家), not the launcher.

    // LaunchApp queues another LVGL callback; preserve order until its
    // matching open notification arrives, rather than until the call returns.
    if (!s_switch_expected[0] || std::strcmp(s_switch_expected, app_id) != 0)
        s_switch_count = 0;
    s_switch_expected[0] = '\0';
    const int existing = FindIndex(app_id);
    RecentEntry entry{};
    entry.used = true;
    std::snprintf(entry.id, sizeof(entry.id), "%s", app_id);
    std::snprintf(entry.name, sizeof(entry.name), "%s",
                  app_name != nullptr ? app_name : app_id);
    if (icon_path != nullptr) {
        std::snprintf(entry.icon, sizeof(entry.icon), "%s", icon_path);
    }

    if (existing >= 0) {
        for (int i = existing; i > 0; --i) {
            s_entries[i] = s_entries[i - 1];
        }
        s_entries[0] = entry;
        return;
    }

    if (s_count < kMaxRecents) {
        for (int i = s_count; i > 0; --i) {
            s_entries[i] = s_entries[i - 1];
        }
        s_entries[0] = entry;
        ++s_count;
    } else {
        for (int i = kMaxRecents - 1; i > 0; --i) {
            s_entries[i] = s_entries[i - 1];
        }
        s_entries[0] = entry;
    }
}

void Recents_Show(lv_obj_t *parent)
{
    if (parent == nullptr) parent = lv_screen_active();
    if (parent == nullptr) return;
    if (s_root != nullptr) Recents_Hide();
    BuildUi(parent);
    IdlePower_Attach(IdlePowerSession::Home, true);
}

void Recents_Hide(void)
{
    if (s_root == nullptr) return;
    lv_obj_delete(s_root);
    s_root = nullptr;
    if (HomeScreen::IsActive())
        IdlePower_Attach(IdlePowerSession::Home, false);
    else
        IdlePower_Detach(IdlePowerSession::Home);
}

bool Recents_IsOpen(void)
{
    return s_root != nullptr;
}

void Recents_Clear(void)
{
    s_count = 0;
    s_switch_count = 0;
    std::memset(s_entries, 0, sizeof(s_entries));
}

bool Recents_SwitchAdjacent(int direction, bool from_home)
{
    if (!s_count || !direction || s_pending_launch_id[0] || s_switch_expected[0]) return false;
    // Keep the navigation order stable while launch notifications reorder MRU.
    if (!s_switch_count || from_home) {
        s_switch_count = s_count;
        for (int i = 0; i < s_count; ++i)
            std::snprintf(s_switch_order[i], sizeof(s_switch_order[i]), "%s", s_entries[i].id);
        s_switch_cursor = 0;
    }
    if(s_switch_count<=1 && !from_home) return false;
    int next = from_home ? (direction>0?0:s_switch_count-1) :
        HiNavigation::Adjacent(s_switch_cursor,direction,s_switch_count);
    if(next<0) return false;
    const int previous = s_switch_cursor;
    s_switch_cursor = next;
    std::snprintf(s_pending_launch_id, sizeof(s_pending_launch_id), "%s", s_switch_order[next]);
    s_switch_pending = true;
    if (lv_async_call(OnLaunchAppAsync, nullptr) != LV_RESULT_OK) {
        s_switch_cursor = previous;
        s_switch_pending = false;
        s_pending_launch_id[0] = '\0';
        return false;
    }
    return true;
}
