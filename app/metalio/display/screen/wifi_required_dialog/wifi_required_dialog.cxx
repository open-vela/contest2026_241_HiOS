/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiFi-required dialog — ported from MetalioClaw4
 * main/display/screen/wifi_required_dialog.cc.
 *
 * The original reads the saved network type (WiFi vs 4G) via
 * DualNetworkBoard::LoadNetworkTypeFromSettings and checks
 * WifiStation::GetInstance().IsConnected(). openvela does not yet have
 * those classes; the blocker falls back to a Settings key
 * ("network/type": 0=WiFi, 1=4G) and an always-connected stub.
 */

#include "wifi_required_dialog.h"

#include "i18n.h"
#include "screen_util.h"
#include "esp_log_shim.h"
#include "settings.h"
#include <metalio/metalio.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr int kPanelSize = 720;

lv_obj_t *s_overlay = nullptr;

// Real STA state from the esp-hosted netdev.  The C5 sets priv->sta_connected
// on the Event_StaConnected(775) RPC, so this reflects the actual association
// instead of a Settings stub.
bool WifiStationConnected()
{
    return metalio_esp_hosted_is_connected();
}

// This openvela port is WiFi-only (NT26 4G is not wired up yet), so the
// effective network type is always WiFi(0).  Any app that requires network
// therefore blocks until the STA has actually connected.
int SavedNetworkType()
{
    return 0;
}

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
    if (SavedNetworkType() != 0) {  // not WiFi mode
        return false;
    }
    return !WifiStationConnected();
}

void WifiRequired_ShowDialog(const char *hint_msgid)
{
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
    lv_label_set_text(title, I18n::T("未连接 WiFi"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    const char *hint = (hint_msgid != nullptr && hint_msgid[0] != '\0')
                            ? hint_msgid
                            : "请先连接 WiFi 后再使用该应用";
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
