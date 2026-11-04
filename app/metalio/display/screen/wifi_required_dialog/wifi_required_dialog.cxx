/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiFi-required dialog — ported from MetalioClaw4
 * main/display/screen/wifi_required_dialog.cc.
 *
 * Keep the legacy entry-point names, but accept either live data transport.
 */

#include "wifi_required_dialog.h"

#include "i18n.h"
#include "screen_util.h"
#include "esp_log_shim.h"
#include "network_service.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr int kPanelSize = 720;

lv_obj_t *s_overlay = nullptr;
lv_timer_t *s_connection_timer = nullptr;

void CloseDialog()
{
    if (s_overlay != nullptr) {
        lv_obj_delete(s_overlay);
        s_overlay = nullptr;
    }
}

void OnOkClicked(lv_event_t * /*e*/)
{
    CloseDialog();
}

}  // namespace

bool WifiRequired_ShouldBlock()
{
    return !NetworkService::Available();
}

void WifiRequired_ShowDialog(const char *hint_msgid)
{
    // A connection can complete between the initial guard and this call.
    if (!WifiRequired_ShouldBlock()) return;
    lv_obj_t *scr = lv_screen_active();
    if (scr == nullptr) {
        return;
    }
    CloseDialog();

    constexpr int kCardW = 520;
    constexpr int kCardH = 300;
    constexpr int kBtnW = 200;
    constexpr int kBtnH = 72;

    lv_obj_t *mask = lv_obj_create(scr);
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_overlay = mask;
    lv_obj_add_event_cb(mask, [](lv_event_t *event) {
        if (lv_event_get_target_obj(event) == s_overlay) {
            s_overlay = nullptr;
            if (s_connection_timer) {
                lv_timer_delete(s_connection_timer);
                s_connection_timer = nullptr;
            }
        }
    }, LV_EVENT_DELETE, nullptr);
    s_connection_timer = lv_timer_create([](lv_timer_t *) {
        if (!WifiRequired_ShouldBlock()) CloseDialog();
    }, 1000, nullptr);

    lv_obj_t *card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("未连接网络"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    const char *hint = (hint_msgid != nullptr && hint_msgid[0] != '\0')
                            ? hint_msgid
                            : "请先连接 WiFi 或 4G 后再使用该应用";
    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, I18n::T(hint));
    lv_obj_set_width(body, kCardW - 56);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, -10);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ok = lv_button_create(card);
    lv_obj_remove_style_all(ok);
    lv_obj_set_size(ok, kBtnW, kBtnH);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ok, 16, LV_PART_MAIN);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(ok, OnOkClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(ok, true);

    lv_obj_t *ok_lbl = lv_label_create(ok);
    lv_label_set_text(ok_lbl, I18n::T("确定"));
    lv_obj_set_style_text_color(ok_lbl, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_font(ok_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(ok_lbl);
    lv_obj_remove_flag(ok_lbl, LV_OBJ_FLAG_CLICKABLE);
}
