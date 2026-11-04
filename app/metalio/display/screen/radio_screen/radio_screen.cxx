/* SPDX-License-Identifier: Apache-2.0 */
/* MetalioClaw4 radio UI, using the NuttX RadioPlayer backend. */

#include "radio_screen.h"
#include "radio_stations.h"
#include "radio_player.h"

#include "esp_log_shim.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "screen_util.h"
#include "standby_screen/standby_screen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr uint32_t kBg = 0x0E1116;
constexpr uint32_t kBgGrad = 0x161A22;
constexpr uint32_t kText = 0xFFFFFF;
constexpr uint32_t kMuted = 0x9AA3B2;
constexpr uint32_t kAccent = 0xE0FB3C;
constexpr uint32_t kButton = 0x232732;
constexpr uint32_t kButtonPressed = 0x303644;
constexpr uint32_t kPlayButton = 0x3A4150;
constexpr uint32_t kPlayPressed = 0x4A5260;
constexpr uint32_t kVizFloor = 0x1C2230;
constexpr uint32_t kListItem = 0x1A1F28;
constexpr uint32_t kListPressed = 0x2A3140;
constexpr uint32_t kListActive = 0x2A3320;
constexpr uint32_t kListBorder = 0x2E3542;
constexpr int32_t kPanel = 720;
constexpr int32_t kVizW = 520;
constexpr int32_t kVizH = 280;
constexpr int32_t kBarMaxH = kVizH - 28;
constexpr int32_t kBarMinH = 12;
constexpr int32_t kBarGap = 18;
constexpr int32_t kControlSide = 80;
constexpr int32_t kControlPlay = 112;
constexpr int32_t kVolStep = RadioPlayer::kVolumeStep;
constexpr int kBandCount = RadioPlayer::kBandCount;

constexpr uint32_t kRainbow[kBandCount] = {
    0xFF3B5C, 0xFF6B2D, 0xFFB020, 0xFFE84A, 0x7CFF4A, 0x2DFFB0,
    0x2DE8FF, 0x3D8BFF, 0x6B5CFF, 0xB24DFF, 0xFF4DC8, 0xFF4D7A,
};

struct RadioUi {
    lv_obj_t *title = nullptr;
    lv_obj_t *status = nullptr;
    lv_obj_t *viz = nullptr;
    lv_obj_t *bars[kBandCount] = {};
    lv_obj_t *play_icon = nullptr;
    lv_obj_t *volume = nullptr;
    lv_obj_t *list_overlay = nullptr;
    lv_obj_t *list_rows[kRadioStationCount] = {};
    lv_timer_t *poll = nullptr;
    lv_timer_t *viz_timer = nullptr;
    int shown_station = -1;
    int shown_status = -1;
    int shown_volume = -1;
    int shown_heights[kBandCount] = {};
    bool shown_lit[kBandCount] = {};
};

RadioUi s_ui{};
lv_obj_t *s_screen = nullptr;
std::atomic<bool> s_active{false};
int s_connect_ticks = 0;

void SyncUi();

lv_style_selector_t Sel(lv_part_t part, lv_state_t state)
{
    return static_cast<lv_style_selector_t>(part | state);
}

void SetStatus(const char *text)
{
    if (s_ui.status != nullptr)
        lv_label_set_text(s_ui.status, text);
}

void RefreshStationRows()
{
    const int selected = RadioPlayer::Instance().station_index();
    const bool playing = RadioPlayer::Instance().status() == RadioStatus::Playing;
    for (size_t i = 0; i < kRadioStationCount; ++i) {
        lv_obj_t *row = s_ui.list_rows[i];
        if (row == nullptr)
            continue;
        const bool active = static_cast<int>(i) == selected;
        lv_obj_set_style_bg_color(row,
            lv_color_hex(active ? kListActive : kListItem), LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(kAccent), LV_PART_MAIN);
        lv_obj_set_style_border_width(row, active ? 2 : 1, LV_PART_MAIN);
        lv_obj_set_style_border_opa(row, active ? LV_OPA_COVER : LV_OPA_40,
                                    LV_PART_MAIN);
        lv_obj_t *name = lv_obj_get_child(row, 0);
        if (name != nullptr)
            lv_obj_set_style_text_color(name,
                lv_color_hex(active ? kAccent : kText), LV_PART_MAIN);
        lv_obj_t *dot = lv_obj_get_child(row, 1);
        if (dot != nullptr)
            lv_obj_set_style_bg_opa(dot,
                active && playing ? LV_OPA_COVER : LV_OPA_0, LV_PART_MAIN);
    }
}

void SyncVisualizer()
{
    // The standby clock covers this screen without unloading playback.
    // Hidden bars must not invalidate the display every 100 ms beneath it.
    if (!s_active.load(std::memory_order_relaxed) || StandbyScreen::IsVisible())
        return;
    uint8_t levels[kBandCount] = {};
    RadioPlayer::Instance().CopyBands(levels);
    for (int i = 0; i < kBandCount; ++i) {
        if (s_ui.bars[i] == nullptr)
            continue;
        const int32_t height = kBarMinH +
            static_cast<int32_t>(levels[i]) * (kBarMaxH - kBarMinH) / 255;
        if (s_ui.shown_heights[i] != height) {
            lv_obj_set_height(s_ui.bars[i], height);
            s_ui.shown_heights[i] = height;
        }
        const bool lit = levels[i] > 0;
        if (s_ui.shown_lit[i] != lit) {
            lv_obj_set_style_bg_opa(s_ui.bars[i],
                lit ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
            s_ui.shown_lit[i] = lit;
        }
    }
}

void SyncUi()
{
    if (!s_active.load(std::memory_order_relaxed) || StandbyScreen::IsVisible())
        return;
    auto &player = RadioPlayer::Instance();
    const RadioStation &station = player.station();
    const int index = player.station_index();
    const int status = static_cast<int>(player.status());
    const int volume = player.volume();
    const bool station_changed = index != s_ui.shown_station;
    const bool status_changed = status != s_ui.shown_status;
    if (station_changed && s_ui.title != nullptr)
        lv_label_set_text(s_ui.title, station.name);
    if (status_changed) {
    switch (player.status()) {
    case RadioStatus::Connecting: SetStatus(I18n::T("连接中")); break;
    case RadioStatus::Playing: SetStatus(I18n::T("正在播放")); break;
    case RadioStatus::Paused: SetStatus(I18n::T("已暂停")); break;
    case RadioStatus::Failed: SetStatus(I18n::T("播放失败，请检查网络或更换电台")); break;
    default: SetStatus(I18n::T("点击播放开始收听，点击标题或列表选择电台")); break;
    }
    if (s_ui.play_icon != nullptr) {
        const auto status = player.status();
        lv_image_set_src(s_ui.play_icon,
            status == RadioStatus::Playing || status == RadioStatus::Connecting
                ? "A:ic_s_player_pause.spng" : "A:ic_s_player_play.spng");
    }
    }
    if (volume != s_ui.shown_volume && s_ui.volume != nullptr) {
        char text[32];
        std::snprintf(text, sizeof(text), "%s  %d%%", I18n::T("音量"),
                      volume);
        lv_label_set_text(s_ui.volume, text);
    }
    if (station_changed || status_changed) RefreshStationRows();
    s_ui.shown_station = index;
    s_ui.shown_status = status;
    s_ui.shown_volume = volume;
}

void OnPoll(lv_timer_t *)
{
    RadioPlayer &player = RadioPlayer::Instance();
    player.AbortConnectTimeout();
    SyncUi();
}

void OnPlay(lv_event_t *)
{
    RadioPlayer::Instance().TogglePlayPause();
    SyncUi();
}

void OnVolumeDown(lv_event_t *)
{
    RadioPlayer::Instance().AdjustVolume(-kVolStep);
    SyncUi();
}

void OnVolumeUp(lv_event_t *)
{
    RadioPlayer::Instance().AdjustVolume(kVolStep);
    SyncUi();
}

void HideStationList()
{
    if (s_ui.list_overlay != nullptr)
        lv_obj_add_flag(s_ui.list_overlay, LV_OBJ_FLAG_HIDDEN);
}

void ShowStationList()
{
    if (s_ui.list_overlay != nullptr) {
        RefreshStationRows();
        lv_obj_clear_flag(s_ui.list_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void OnStation(lv_event_t *event)
{
    const intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (index < 0 || index >= static_cast<intptr_t>(kRadioStationCount))
        return;
    RadioPlayer::Instance().SelectStation(static_cast<int>(index));
    HideStationList();
    SyncUi();
}

void OnBack(lv_event_t *)
{
    HomeScreen::SwitchToHome();
}

void OnSwipeBack()
{
    HomeScreen::SwitchToHome();
}

void OnUnloaded(lv_event_t *)
{
    s_active.store(false, std::memory_order_relaxed);
    if (s_ui.poll != nullptr) {
        lv_timer_delete(s_ui.poll);
        s_ui.poll = nullptr;
    }
    if (s_ui.viz_timer != nullptr) {
        lv_timer_delete(s_ui.viz_timer);
        s_ui.viz_timer = nullptr;
    }
    RadioPlayer::Instance().Stop();
    s_screen = nullptr;
    s_ui = RadioUi{};
}

lv_obj_t *MakeRoundButton(lv_obj_t *parent, int size, uint32_t bg,
                          uint32_t pressed, const char *icon,
                          lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(pressed),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(button, true);
    lv_obj_t *image = lv_image_create(button);
    lv_image_set_src(image, icon);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(image);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    return image;
}

void BuildBackAndListButtons(lv_obj_t *screen)
{
    lv_obj_t *back = lv_button_create(screen);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 72, 72);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 32, 36);
    lv_obj_add_event_cb(back, OnBack, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);
    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_center(back_icon);
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *list = lv_button_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 96, 72);
    lv_obj_align(list, LV_ALIGN_TOP_RIGHT, -24, 36);
    lv_obj_set_style_bg_color(list, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(list, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(list, 20, LV_PART_MAIN);
    lv_obj_add_event_cb(list, [](lv_event_t *) { ShowStationList(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(list, true);
    lv_obj_t *label = lv_label_create(list);
    lv_label_set_text(label, I18n::T("列表"));
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_center(label);
}

void BuildStationList(lv_obj_t *screen)
{
    s_ui.list_overlay = lv_obj_create(screen);
    lv_obj_set_size(s_ui.list_overlay, kPanel, kPanel);
    lv_obj_align(s_ui.list_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    screen_strip_obj_chrome(s_ui.list_overlay);
    lv_obj_clear_flag(s_ui.list_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_ui.list_overlay, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_ui.list_overlay, lv_color_hex(kBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_ui.list_overlay, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.list_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.list_overlay, LV_OBJ_FLAG_HIDDEN);
    screen_swipe_back_ignore(s_ui.list_overlay, true);

    lv_obj_t *header = lv_obj_create(s_ui.list_overlay);
    lv_obj_set_size(header, kPanel, 88);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    screen_strip_obj_chrome(header);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *close = lv_button_create(header);
    lv_obj_remove_style_all(close);
    lv_obj_set_size(close, 72, 72);
    lv_obj_align(close, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_add_event_cb(close, [](lv_event_t *) { HideStationList(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(close, true);
    lv_obj_t *icon = lv_image_create(close);
    lv_image_set_src(icon, "A:ic_app_back.spng");
    lv_obj_center(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("选择电台"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(title);

    lv_obj_t *list = lv_obj_create(s_ui.list_overlay);
    // Keep the final station above the persistent home gesture area.
    lv_obj_set_size(list, kPanel - 48, kPanel - 168);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 96);
    screen_strip_obj_chrome(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(list, true);

    for (size_t i = 0; i < kRadioStationCount; ++i) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, kPanel - 64, 64);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(kListItem), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kListPressed),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(kListBorder), LV_PART_MAIN);
        lv_obj_set_style_border_opa(row, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 20, LV_PART_MAIN);
        lv_obj_add_event_cb(row, OnStation, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        screen_swipe_back_ignore(row, true);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, kRadioStations[i].name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, kPanel - 120);
        lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, lv_color_hex(kText), LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(kAccent), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_0, LV_PART_MAIN);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        s_ui.list_rows[i] = row;
    }
    RefreshStationRows();
}

}  // namespace

lv_obj_t *RadioScreen::CreateStatic()
{
    s_ui = RadioUi{};
    s_connect_ticks = 0;
    lv_obj_t *screen = lv_obj_create(nullptr);
    screen_strip_obj_chrome(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(kBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    BuildBackAndListButtons(screen);

    s_ui.title = lv_label_create(screen);
    lv_label_set_text(s_ui.title, RadioPlayer::Instance().station().name);
    lv_obj_set_style_text_font(s_ui.title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.title, lv_color_hex(kText), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.title, kPanel - 220);
    lv_obj_align(s_ui.title, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_add_flag(s_ui.title, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(s_ui.title, true);
    lv_obj_add_event_cb(s_ui.title, [](lv_event_t *) { ShowStationList(); },
                        LV_EVENT_CLICKED, nullptr);

    s_ui.status = lv_label_create(screen);
    lv_obj_set_style_text_font(s_ui.status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_ui.status, kPanel - 80);
    lv_obj_align(s_ui.status, LV_ALIGN_TOP_MID, 0, 100);
    screen_make_input_passive(s_ui.status);

    s_ui.viz = lv_obj_create(screen);
    lv_obj_set_size(s_ui.viz, kVizW, kVizH);
    lv_obj_align(s_ui.viz, LV_ALIGN_TOP_MID, 0, 150);
    screen_strip_obj_chrome(s_ui.viz);
    lv_obj_remove_flag(s_ui.viz, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ui.viz, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_ui.viz, lv_color_hex(kVizFloor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.viz, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.viz, 28, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.viz, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.viz, 0, LV_PART_MAIN);
    const int32_t bar_w = (kVizW - 48 - (kBandCount - 1) * kBarGap) / kBandCount;
    const int32_t used_w = kBandCount * bar_w + (kBandCount - 1) * kBarGap;
    const int32_t x0 = (kVizW - used_w) / 2;
    for (int i = 0; i < kBandCount; ++i) {
        lv_obj_t *bar = lv_obj_create(s_ui.viz);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, bar_w, kBarMinH);
        lv_obj_set_style_radius(bar, bar_w / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(kRainbow[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_40, LV_PART_MAIN);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT,
                     x0 + i * (bar_w + kBarGap), -10);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        s_ui.bars[i] = bar;
    }

    s_ui.volume = lv_label_create(screen);
    lv_obj_set_style_text_font(s_ui.volume, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.volume, lv_color_hex(kMuted), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.volume, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_ui.volume, kPanel - 80);
    lv_obj_align(s_ui.volume, LV_ALIGN_TOP_MID, 0, 454);

    lv_obj_t *controls = lv_obj_create(screen);
    lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls, 520, 120);
    lv_obj_align(controls, LV_ALIGN_TOP_MID, 0, 530);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(controls, LV_OBJ_FLAG_SCROLLABLE);
    MakeRoundButton(controls, kControlSide, kButton, kButtonPressed,
                    "A:ic_s_music_volume_down.spng", OnVolumeDown);
    s_ui.play_icon = MakeRoundButton(controls, kControlPlay, kPlayButton,
                    kPlayPressed, "A:ic_s_player_play.spng", OnPlay);
    MakeRoundButton(controls, kControlSide, kButton, kButtonPressed,
                    "A:ic_s_music_volume_up.spng", OnVolumeUp);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, I18n::T("支持 WiFi 或 4G 网络收听在线电台"));
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kMuted), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, kPanel - 64);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);
    screen_make_input_passive(hint);

    BuildStationList(screen);
    lv_obj_add_event_cb(screen, OnUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(screen, OnSwipeBack);

    s_screen = screen;
    s_active.store(true, std::memory_order_relaxed);
    s_ui.poll = lv_timer_create(OnPoll, 250, nullptr);
    // 10 Hz is sufficient for a radio spectrum display and avoids forcing
    // high-rate full-screen redraws while audio is draining.
    s_ui.viz_timer = lv_timer_create([](lv_timer_t *) { SyncVisualizer(); },
                                     100, nullptr);
    SyncUi();
    write(1, "RADIO_UI_READY\n", 15);
    return screen;
}

lv_obj_t *RadioScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void RadioScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        s_connect_ticks = 0;
        s_active.store(true, std::memory_order_relaxed);
        SyncUi();
    } else {
        s_active.store(false, std::memory_order_relaxed);
        RadioPlayer::Instance().Stop();
    }
}
