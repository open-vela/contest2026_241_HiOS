/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * RadioScreen — UI ported 1:1 from MetalioClaw4 radio_screen.cc;
 * playback/spectrum delegated to RadioPlayer (radio_player.cxx).
 */

#include "radio_screen.h"
#include "radio_stations.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "home_screen/home_screen.h"
#include "radio_player.h"
#include "screen_util.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char *TAG = "RadioScreen";

constexpr int32_t kPanelSize = 720;
constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorBgGrad = 0x161A22;
constexpr uint32_t kColorTextPrimary = 0xFFFFFF;
constexpr uint32_t kColorSubtle = 0x9AA3B2;
constexpr uint32_t kColorAccent = 0xE0FB3C;
constexpr uint32_t kColorCtrlBtnBg = 0x232732;
constexpr uint32_t kColorCtrlBtnBgPressed = 0x303644;
constexpr uint32_t kColorPlayBtnBg = 0x3A4150;
constexpr uint32_t kColorPlayBtnBgPressed = 0x4A5260;
constexpr uint32_t kColorVizFloor = 0x1C2230;
constexpr uint32_t kColorListItemBg = 0x1A1F28;
constexpr uint32_t kColorListItemBgPressed = 0x2A3140;
constexpr uint32_t kColorListItemActive = 0x2A3320;
constexpr uint32_t kColorListBorder = 0x2E3542;

constexpr int32_t kTitleY = 48;
constexpr int32_t kStatusY = 100;
constexpr int32_t kVizY = 150;
constexpr int32_t kVizW = 520;
constexpr int32_t kVizH = 280;
constexpr int32_t kCtrlRowY = 530;
constexpr int32_t kCtrlRowWidth = 520;
constexpr int32_t kCtrlRowHeight = 120;
constexpr int32_t kCtrlSideBtnSize = 80;
constexpr int32_t kCtrlPlayBtnSize = 112;
constexpr int32_t kHintBottomMargin = 16;

constexpr int kBarCount = RadioPlayer::kBandCount;
constexpr int kBarGap = 18;
constexpr int kBarMinH = 12;
constexpr int kBarMaxH = kVizH - 28;

constexpr uint32_t kRainbowBase[kBarCount] = {
    0xFF3B5C, 0xFF6B2D, 0xFFB020, 0xFFE84A, 0x7CFF4A, 0x2DFFB0,
    0x2DE8FF, 0x3D8BFF, 0x6B5CFF, 0xB24DFF, 0xFF4DC8, 0xFF4D7A,
};

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state)
{
    return static_cast<lv_style_selector_t>(part | state);
}

enum class StatusKind : uint8_t { Idle, Connecting, Playing, Paused, Failed };

struct RadioUi {
    lv_obj_t *lbl_title = nullptr;
    lv_obj_t *lbl_status = nullptr;
    lv_obj_t *img_play_icon = nullptr;
    lv_obj_t *viz_host = nullptr;
    lv_obj_t *bars[kBarCount] = {};
    lv_obj_t *lbl_volume = nullptr;
    lv_timer_t *viz_timer = nullptr;
    lv_timer_t *auto_play_timer = nullptr;
    lv_obj_t *list_overlay = nullptr;
    lv_obj_t *list_scroll = nullptr;
    lv_obj_t *list_rows[kRadioStationCount] = {};
};

RadioUi s_ui;
lv_obj_t *s_bound_scr = nullptr;
std::atomic<bool> s_screen_active{false};
int s_connect_ticks = 0;

StatusKind RadioStatusToKind(RadioStatus st)
{
    switch (st) {
    case RadioStatus::Idle:
        return StatusKind::Idle;
    case RadioStatus::Connecting:
        return StatusKind::Connecting;
    case RadioStatus::Playing:
        return StatusKind::Playing;
    case RadioStatus::Paused:
        return StatusKind::Paused;
    case RadioStatus::Failed:
        return StatusKind::Failed;
    default:
        return StatusKind::Idle;
    }
}

uint32_t LerpColor(uint32_t a, uint32_t b, float t)
{
    if (t <= 0.f) return a;
    if (t >= 1.f) return b;
    const auto ch = [&](int shift) {
        const int ca = static_cast<int>((a >> shift) & 0xFF);
        const int cb = static_cast<int>((b >> shift) & 0xFF);
        return static_cast<uint32_t>(ca + (cb - ca) * t);
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

uint32_t BarColor(int index, float level)
{
    const uint32_t base = kRainbowBase[index % kBarCount];
    const float boost = 0.25f + 0.75f * std::min(1.f, std::max(0.f, level));
    return LerpColor(0x2A2F3A, LerpColor(base, 0xFFFFFF, boost * 0.35f), boost);
}

void SetStatusDirect(StatusKind kind);
void SetPlayIconDirect(bool playing);
void SyncStatusToUi();
void UpdateVolumeLabel();
void UpdateTitleLabel();
void ShowStationList();
void HideStationList();
void RefreshStationListHighlight();
void SwitchStation(int index);
void OnStationRowClicked(lv_event_t *e);
void BuildStationListOverlay(lv_obj_t *scr);

void SetStatusDirect(StatusKind kind)
{
    if (s_ui.lbl_status == nullptr) return;
    const char *text = I18n::T("点击播放");
    switch (kind) {
    case StatusKind::Idle:
        text = I18n::T("点击播放");
        break;
    case StatusKind::Connecting:
        text = I18n::T("连接中");
        break;
    case StatusKind::Playing:
        text = I18n::T("正在播放");
        break;
    case StatusKind::Paused:
        text = I18n::T("已暂停");
        break;
    case StatusKind::Failed:
        text = I18n::T("播放失败");
        break;
    }
    lv_label_set_text(s_ui.lbl_status, text);
}

void SetPlayIconDirect(bool playing)
{
    if (s_ui.img_play_icon == nullptr) return;
    lv_image_set_src(s_ui.img_play_icon,
                     playing ? "A:ic_s_player_pause.spng" : "A:ic_s_player_play.spng");
}

void SyncStatusToUi()
{
    if (!s_screen_active.load(std::memory_order_relaxed)) return;
    auto &rp = RadioPlayer::Instance();
    SetStatusDirect(RadioStatusToKind(rp.status()));
    SetPlayIconDirect((rp.want_play() && rp.status() != RadioStatus::Paused) ||
                      rp.status() == RadioStatus::Connecting);
    UpdateVolumeLabel();
    UpdateTitleLabel();
}

void UpdateVolumeLabel()
{
    if (s_ui.lbl_volume == nullptr) return;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%s %d%%", I18n::T("音量"),
                  RadioPlayer::Instance().volume());
    lv_label_set_text(s_ui.lbl_volume, buf);
}

void UpdateTitleLabel()
{
    if (s_ui.lbl_title == nullptr) return;
    lv_label_set_text(s_ui.lbl_title, RadioPlayer::Instance().station().name);
}

void ApplyVizFrame()
{
    if (!s_screen_active.load(std::memory_order_relaxed) || s_ui.viz_host == nullptr) {
        return;
    }

    uint8_t levels[kBarCount];
    RadioPlayer::Instance().CopyBands(levels);

    const int32_t bar_w = (kVizW - 48 - (kBarCount - 1) * kBarGap) / kBarCount;
    const int32_t usable_w = kBarCount * bar_w + (kBarCount - 1) * kBarGap;
    const int32_t x0 = (kVizW - usable_w) / 2;

    for (int i = 0; i < kBarCount; ++i) {
        lv_obj_t *bar = s_ui.bars[i];
        if (bar == nullptr) continue;
        const float t = levels[i] / 255.f;
        const int32_t h = kBarMinH + static_cast<int32_t>((kBarMaxH - kBarMinH) * t);
        lv_obj_set_height(bar, h);
        lv_obj_set_style_bg_color(bar, lv_color_hex(BarColor(i, t)), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar,
                                static_cast<lv_opa_t>(140 + static_cast<int>(115 * t)),
                                LV_PART_MAIN);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, x0 + i * (bar_w + kBarGap), -10);
    }
}

void VizTimerCb(lv_timer_t * /*t*/)
{
    ApplyVizFrame();
    if (!s_screen_active.load(std::memory_order_relaxed)) {
        return;
    }
    auto &rp = RadioPlayer::Instance();
    const RadioStatus st = rp.status();
    if (st == RadioStatus::Connecting && rp.want_play()) {
        ++s_connect_ticks;
        /* ~15 s @ 33 ms — stuck Connecting with no PCM → fail once. */
        if (s_connect_ticks >= 450) {
            s_connect_ticks = 0;
            rp.AbortConnectTimeout();
            SyncStatusToUi();
        }
    } else {
        s_connect_ticks = 0;
    }
    if ((s_connect_ticks % 15) == 0) {
        SyncStatusToUi();
    }
}

void HideStationList()
{
    if (s_ui.list_overlay == nullptr) return;
    lv_obj_add_flag(s_ui.list_overlay, LV_OBJ_FLAG_HIDDEN);
}

void RefreshStationListHighlight()
{
    if (s_ui.list_overlay == nullptr) return;
    const int cur = RadioPlayer::Instance().station_index();
    for (size_t i = 0; i < kRadioStationCount; ++i) {
        lv_obj_t *row = s_ui.list_rows[i];
        if (row == nullptr) continue;
        const bool active = (static_cast<int>(i) == cur);
        lv_obj_set_style_bg_color(row,
                                  lv_color_hex(active ? kColorListItemActive : kColorListItemBg),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_color(row,
                                      lv_color_hex(active ? kColorAccent : kColorListBorder),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_opa(row, active ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
        if (lv_obj_get_child_count(row) > 0) {
            lv_obj_t *lbl = lv_obj_get_child(row, 0);
            if (lbl != nullptr) {
                lv_obj_set_style_text_color(lbl,
                                            lv_color_hex(active ? kColorAccent : kColorTextPrimary),
                                            LV_PART_MAIN);
            }
        }
    }
}

void ShowStationList()
{
    if (s_bound_scr == nullptr) {
        return;
    }
    /* Lazy list — building all rows on create spikes heap. */
    if (s_ui.list_overlay == nullptr) {
        BuildStationListOverlay(s_bound_scr);
    }
    if (s_ui.list_overlay == nullptr) {
        return;
    }
    RefreshStationListHighlight();
    lv_obj_remove_flag(s_ui.list_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.list_overlay);

    const int cur = RadioPlayer::Instance().station_index();
    if (cur >= 0 && cur < static_cast<int>(kRadioStationCount) &&
        s_ui.list_rows[static_cast<size_t>(cur)] != nullptr && s_ui.list_scroll != nullptr) {
        lv_obj_scroll_to_view(s_ui.list_rows[static_cast<size_t>(cur)], LV_ANIM_OFF);
    }
}

void SwitchStation(int index)
{
    if (index < 0 || index >= static_cast<int>(kRadioStationCount)) return;
    RadioPlayer::Instance().SelectStation(index);
    UpdateTitleLabel();
    HideStationList();
    SyncStatusToUi();
    ESP_LOGI(TAG, "switch station -> %d (%s)", index,
             RadioPlayer::Instance().station().name);
}

void OnStationRowClicked(lv_event_t *e)
{
    const intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    SwitchStation(static_cast<int>(idx));
}

void OnSwipeBack()
{
    if (s_ui.list_overlay != nullptr &&
        !lv_obj_has_flag(s_ui.list_overlay, LV_OBJ_FLAG_HIDDEN)) {
        HideStationList();
        return;
    }
    RadioPlayer::Instance().Stop();
    HomeScreen::SwitchToHome();
}

void OnScreenUnloaded(lv_event_t *e)
{
    auto *target = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if (target == nullptr || target != s_bound_scr) return;
    s_bound_scr = nullptr;
    s_screen_active.store(false, std::memory_order_relaxed);
    if (s_ui.auto_play_timer != nullptr) {
        lv_timer_delete(s_ui.auto_play_timer);
        s_ui.auto_play_timer = nullptr;
    }
    if (s_ui.viz_timer != nullptr) {
        lv_timer_delete(s_ui.viz_timer);
        s_ui.viz_timer = nullptr;
    }
    s_ui = RadioUi{};
    s_connect_ticks = 0;
}

void OnAutoPlayTick(lv_timer_t * /*t*/)
{
    s_ui.auto_play_timer = nullptr;
    if (!s_screen_active.load(std::memory_order_relaxed)) {
        return;
    }
    write(1, "RADIO_AUTO\n", 11);
    SetStatusDirect(StatusKind::Connecting);
    RadioPlayer::Instance().Start();
    SyncStatusToUi();
}

void CancelAutoPlayTimer()
{
    if (s_ui.auto_play_timer != nullptr) {
        lv_timer_delete(s_ui.auto_play_timer);
        s_ui.auto_play_timer = nullptr;
    }
}

void ArmDeferredAutoPlay()
{
    CancelAutoPlayTimer();
    /* Claw4 auto-plays on LOAD; defer so first paint + audio occupy do not race. */
    s_ui.auto_play_timer = lv_timer_create(OnAutoPlayTick, 500, nullptr);
    if (s_ui.auto_play_timer != nullptr) {
        lv_timer_set_repeat_count(s_ui.auto_play_timer, 1);
    }
}

void OnPlayClicked(lv_event_t * /*e*/)
{
    RadioPlayer::Instance().TogglePlayPause();
    SyncStatusToUi();
}

void OnVolDownClicked(lv_event_t * /*e*/)
{
    RadioPlayer::Instance().AdjustVolume(-RadioPlayer::kVolumeStep);
    UpdateVolumeLabel();
}

void OnVolUpClicked(lv_event_t * /*e*/)
{
    RadioPlayer::Instance().AdjustVolume(RadioPlayer::kVolumeStep);
    UpdateVolumeLabel();
}

lv_obj_t *CreateRoundButton(lv_obj_t *parent, int32_t size, uint32_t bg_color,
                            uint32_t bg_pressed, const char *icon_path, lv_event_cb_t cb)
{
    /* Square — LV_RADIUS_CIRCLE softmasks blue on this NuttX flush path. */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_pressed),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(btn, 12);
    screen_swipe_back_ignore(btn, true);

    lv_obj_t *img = lv_image_create(btn);
    lv_image_set_src(img, icon_path);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return img;
}

void BuildBackButton(lv_obj_t *scr)
{
    lv_obj_t *back_btn = lv_button_create(scr);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 72, 72);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 32, 36);
    screen_swipe_back_ignore(back_btn, true);

    lv_obj_t *back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);
    lv_obj_add_event_cb(back_btn, [](lv_event_t * /*e*/) { OnSwipeBack(); },
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *list_btn = lv_button_create(scr);
    lv_obj_remove_style_all(list_btn);
    lv_obj_set_size(list_btn, 96, 72);
    lv_obj_set_style_bg_opa(list_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(list_btn, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(list_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(list_btn, 0, LV_PART_MAIN);
    lv_obj_align(list_btn, LV_ALIGN_TOP_RIGHT, -24, 36);
    screen_swipe_back_ignore(list_btn, true);

    lv_obj_t *list_lbl = lv_label_create(list_btn);
    lv_label_set_text(list_lbl, I18n::T("列表"));
    lv_obj_set_style_text_font(list_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(list_lbl, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_center(list_lbl);
    lv_obj_add_event_cb(list_btn, [](lv_event_t * /*e*/) { ShowStationList(); },
                        LV_EVENT_CLICKED, nullptr);
}

void BuildTitle(lv_obj_t *scr)
{
    s_ui.lbl_title = lv_label_create(scr);
    lv_label_set_text(s_ui.lbl_title, RadioPlayer::Instance().station().name);
    lv_obj_set_style_text_font(s_ui.lbl_title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_title, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.lbl_title, kPanelSize - 220);
    lv_obj_align(s_ui.lbl_title, LV_ALIGN_TOP_MID, 0, kTitleY);
    lv_obj_add_flag(s_ui.lbl_title, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(s_ui.lbl_title, true);
    lv_obj_add_event_cb(s_ui.lbl_title, [](lv_event_t * /*e*/) { ShowStationList(); },
                        LV_EVENT_CLICKED, nullptr);

    s_ui.lbl_status = lv_label_create(scr);
    lv_label_set_text(s_ui.lbl_status, I18n::T("连接中"));
    lv_obj_set_style_text_font(s_ui.lbl_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_status, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_ui.lbl_status, kPanelSize - 80);
    lv_obj_align(s_ui.lbl_status, LV_ALIGN_TOP_MID, 0, kStatusY);
    screen_make_input_passive(s_ui.lbl_status);
}

void BuildStationListOverlay(lv_obj_t *scr)
{
    s_ui.list_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_ui.list_overlay, kPanelSize, kPanelSize);
    lv_obj_align(s_ui.list_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    screen_strip_obj_chrome(s_ui.list_overlay);
    lv_obj_remove_flag(s_ui.list_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_ui.list_overlay, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_ui.list_overlay, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_ui.list_overlay, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.list_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.list_overlay, LV_OBJ_FLAG_HIDDEN);
    screen_swipe_back_ignore(s_ui.list_overlay, true);

    lv_obj_t *header = lv_obj_create(s_ui.list_overlay);
    lv_obj_set_size(header, kPanelSize, 88);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    screen_strip_obj_chrome(header);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *close_btn = lv_button_create(header);
    lv_obj_remove_style_all(close_btn);
    lv_obj_set_size(close_btn, 72, 72);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(close_btn, 0, LV_PART_MAIN);
    lv_obj_align(close_btn, LV_ALIGN_LEFT_MID, 24, 0);
    screen_swipe_back_ignore(close_btn, true);

    lv_obj_t *close_icon = lv_image_create(close_btn);
    lv_image_set_src(close_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(close_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close_btn, [](lv_event_t * /*e*/) { HideStationList(); },
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("选择电台"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(title);

    s_ui.list_scroll = lv_obj_create(s_ui.list_overlay);
    lv_obj_set_size(s_ui.list_scroll, kPanelSize - 48, kPanelSize - 108);
    lv_obj_align(s_ui.list_scroll, LV_ALIGN_TOP_MID, 0, 96);
    screen_strip_obj_chrome(s_ui.list_scroll);
    lv_obj_add_flag(s_ui.list_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ui.list_scroll, LV_DIR_VER);
    lv_obj_set_style_bg_opa(s_ui.list_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.list_scroll, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.list_scroll, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.list_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.list_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_ui.list_scroll, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(s_ui.list_scroll, true);

    for (size_t i = 0; i < kRadioStationCount; ++i) {
        lv_obj_t *row = lv_obj_create(s_ui.list_scroll);
        lv_obj_set_size(row, kPanelSize - 64, 64);
        screen_strip_obj_chrome(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorListItemBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorListItemBgPressed),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(kColorListBorder), LV_PART_MAIN);
        lv_obj_set_style_border_opa(row, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 20, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(row, 0, LV_PART_MAIN);
        screen_swipe_back_ignore(row, true);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, kRadioStations[i].name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, kPanelSize - 120);
        lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(row, OnStationRowClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        s_ui.list_rows[i] = row;
    }

    RefreshStationListHighlight();
}

void BuildVisualizer(lv_obj_t *scr)
{
    s_ui.viz_host = lv_obj_create(scr);
    lv_obj_set_size(s_ui.viz_host, kVizW, kVizH);
    lv_obj_align(s_ui.viz_host, LV_ALIGN_TOP_MID, 0, kVizY);
    screen_strip_obj_chrome(s_ui.viz_host);
    lv_obj_remove_flag(s_ui.viz_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.viz_host, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_ui.viz_host, lv_color_hex(kColorVizFloor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.viz_host, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.viz_host, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.viz_host, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.viz_host, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_ui.viz_host, false, LV_PART_MAIN);
    screen_make_input_passive(s_ui.viz_host);

    const int32_t bar_w = (kVizW - 48 - (kBarCount - 1) * kBarGap) / kBarCount;
    const int32_t usable_w = kBarCount * bar_w + (kBarCount - 1) * kBarGap;
    const int32_t x0 = (kVizW - usable_w) / 2;

    for (int i = 0; i < kBarCount; ++i) {
        lv_obj_t *bar = lv_obj_create(s_ui.viz_host);
        lv_obj_set_size(bar, bar_w, kBarMinH);
        screen_strip_obj_chrome(bar);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(kRainbowBase[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, x0 + i * (bar_w + kBarGap), -10);
        s_ui.bars[i] = bar;
    }

    s_ui.viz_timer = lv_timer_create(VizTimerCb, 33, nullptr);
}

void BuildVolumeLabel(lv_obj_t *scr)
{
    s_ui.lbl_volume = lv_label_create(scr);
    UpdateVolumeLabel();
    lv_obj_set_style_text_font(s_ui.lbl_volume, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_volume, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_volume, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_ui.lbl_volume, kPanelSize - 80);
    lv_obj_align(s_ui.lbl_volume, LV_ALIGN_TOP_MID, 0, kVizY + kVizH + 24);
    screen_make_input_passive(s_ui.lbl_volume);
}

void BuildControls(lv_obj_t *scr)
{
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_size(row, kCtrlRowWidth, kCtrlRowHeight);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, kCtrlRowY);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg, kColorCtrlBtnBgPressed,
                      "A:ic_s_music_volume_down.spng", OnVolDownClicked);
    s_ui.img_play_icon = CreateRoundButton(row, kCtrlPlayBtnSize, kColorPlayBtnBg,
                                           kColorPlayBtnBgPressed, "A:ic_s_player_play.spng",
                                           OnPlayClicked);
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg, kColorCtrlBtnBgPressed,
                      "A:ic_s_music_volume_up.spng", OnVolUpClicked);
}

void BuildUsageHint(lv_obj_t *scr)
{
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint,
                      I18n::T("请勿在 4G 模式下使用电台，非常消耗流量，请在 WiFi 下使用"));
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, kPanelSize - 64);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -kHintBottomMargin);
    screen_make_input_passive(hint);
}

}  // namespace

lv_obj_t *RadioScreen::CreateStatic()
{
    write(1, "RADIO_CR\n", 9);
    if (s_ui.viz_timer != nullptr) {
        lv_timer_delete(s_ui.viz_timer);
        s_ui.viz_timer = nullptr;
    }
    CancelAutoPlayTimer();
    s_ui = RadioUi{};
    s_connect_ticks = 0;

    lv_obj_t *scr = lv_obj_create(nullptr);
    screen_strip_obj_chrome(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Full UI — LaunchHomeApp reclaims Home before create(). */
    BuildTitle(scr);
    BuildVisualizer(scr);
    BuildVolumeLabel(scr);
    BuildControls(scr);
    BuildUsageHint(scr);
    BuildBackButton(scr);
    /* Station list lazy on first open. */

    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(scr, OnSwipeBack);

    s_bound_scr = scr;
    s_screen_active.store(true, std::memory_order_relaxed);
    SetStatusDirect(StatusKind::Connecting);
    SetPlayIconDirect(false);
    SyncStatusToUi();
    write(1, "RADIO_CR_OK\n", 12);
    return scr;
}

lv_obj_t *RadioScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void RadioScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: radio_screen");
        s_connect_ticks = 0;
        SetStatusDirect(StatusKind::Connecting);
        SetPlayIconDirect(true);
        ArmDeferredAutoPlay();
    } else {
        ESP_LOGI(TAG, "unload: radio_screen");
        s_connect_ticks = 0;
        CancelAutoPlayTimer();
        RadioPlayer::Instance().Stop();
    }
}
