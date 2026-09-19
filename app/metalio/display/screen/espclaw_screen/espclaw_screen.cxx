/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * EspClawScreen — local "edge agent" emote digital-human for openvela.
 *
 * The reference ESPClaw (edge_agent) is a separate ESP-IDF binary that
 * renders an animated emote character (eye / listening animations, clock,
 * toast, status icons) via the esp_emote_gfx engine and drives a local
 * conversation agent.  That binary and engine are not shipped here, so we
 * reimplement the observable behaviour in LVGL:
 *
 *   - central emotion face loaded from the SD bundle
 *     (/sdcard/system/emotion/{category}.sjpg), same as digital_people;
 *   - top status bar: clock + WiFi / battery icons;
 *   - transient status / toast label (SetStatus);
 *   - mic / speaker indicator driven by Application device state
 *     (listening / speaking);
 *   - user / system chat bubbles (SetChatMessage).
 *
 * The conversation itself reuses the existing cloud link (OpenClaw /
 * MQTT / WebSocket) and AudioService wake-word + ASR by requesting the
 * voice-UI session while the screen is on stage.
 */

#include "espclaw_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "font_awesome.h"
#include "board_shim.h"
#include "application.h"
#include "device_state.h"
#include "home_screen/home_screen.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);

namespace {

constexpr const char *TAG = "EspClawScreen";

constexpr int32_t  kPanelSize    = 720;
constexpr int32_t  kStatusBarH   = 48;
constexpr uint32_t kColorBg      = 0x000000;

// Emotion resources live on the SD card, sharing the same bundle the
// digital-human page uses. Only .sjpg is decoded by this port; the .eaf
// animated format is not available on NuttX.
constexpr const char *kEmotionDir      = "S:/sdcard/system/emotion/";
constexpr const char *kEmotionPosixDir = "/sdcard/system/emotion/";
constexpr const char *kDefaultEmotion  = "neutral";

// Chat-bubble visuals (kept in sync with digital_people_screen).
constexpr int32_t  kBubbleRadius   = 18;
constexpr int32_t  kBubblePadX     = 18;
constexpr int32_t  kBubblePadY     = 14;
constexpr int32_t  kBubbleBorder   = 2;
constexpr int32_t  kSideMargin     = 16;
constexpr int32_t  kSysBubbleTop   = kStatusBarH + 8;
constexpr int32_t  kUserBubbleBottom = 24;
constexpr int32_t  kBubbleMaxW     = kPanelSize - kSideMargin * 2;
constexpr uint32_t kColorBubbleBg  = 0xFFFFFF;
constexpr uint32_t kColorBubbleText = 0x1F2937;

struct UiState {
    lv_obj_t *screen         = nullptr;
    lv_obj_t *face           = nullptr;   // lv_image (sjpg)
    lv_obj_t *face_hint      = nullptr;   // label shown when face missing
    lv_obj_t *clock_lbl      = nullptr;
    lv_obj_t *net_icon_lbl   = nullptr;
    lv_obj_t *battery_icon_lbl = nullptr;
    lv_obj_t *status_lbl     = nullptr;   // toast / status text
    lv_obj_t *listen_icon    = nullptr;   // mic / speaker glyph
    lv_obj_t *listen_lbl     = nullptr;
    lv_obj_t *system_bubble  = nullptr;
    lv_obj_t *system_label   = nullptr;
    lv_obj_t *user_bubble    = nullptr;
    lv_obj_t *user_label     = nullptr;
    lv_timer_t *poll_timer   = nullptr;
};
UiState s_ui;

// Cached emotion category so SetEmotion works while the screen is off stage.
char s_current_emotion[24] = "neutral";
char s_current_status[64]   = "";
DeviceState s_last_state    = kDeviceStateUnknown;
bool s_voice_ui_held        = false;

const char *EmotionCategoryName(const char *category)
{
    return (category != nullptr && category[0] != '\0') ? category
                                                        : kDefaultEmotion;
}

bool EmotionFileExists(const char *category)
{
    char path[96];
    std::snprintf(path, sizeof(path), "%s%s.sjpg", kEmotionPosixDir,
                  EmotionCategoryName(category));
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// ---- face ----
void SetFaceSrc(const char *category)
{
    if (s_ui.face == nullptr) {
        return;
    }
    const char *name = EmotionCategoryName(category);
    if (EmotionFileExists(name)) {
        char lvpath[96];
        std::snprintf(lvpath, sizeof(lvpath), "%s%s.sjpg", kEmotionDir, name);
        lv_image_set_src(s_ui.face, lvpath);
        lv_obj_remove_flag(s_ui.face, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.face_hint != nullptr) {
            lv_obj_add_flag(s_ui.face_hint, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(s_ui.face, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.face_hint != nullptr) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "[%s]", name);
            lv_label_set_text(s_ui.face_hint, buf);
            lv_obj_remove_flag(s_ui.face_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---- status / toast ----
void UpdateStatusLabel(const char *text)
{
    if (s_ui.status_lbl != nullptr) {
        lv_label_set_text(s_ui.status_lbl,
                          text != nullptr ? text : "");
    }
}

// ---- listen / speak indicator: mode < 0 hide, 0 = mic, 1 = speaker ----
void ShowListenIndicator(int mode)
{
    if (s_ui.listen_icon == nullptr || s_ui.listen_lbl == nullptr) {
        return;
    }
    if (mode < 0) {
        lv_obj_add_flag(s_ui.listen_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.listen_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_ui.listen_icon,
                      mode == 0 ? FONT_AWESOME_MICROPHONE : FONT_AWESOME_PLAY);
    lv_label_set_text(s_ui.listen_lbl,
                      mode == 0 ? I18n::T("聆听中...") : I18n::T("说话中..."));
    lv_obj_remove_flag(s_ui.listen_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ui.listen_lbl, LV_OBJ_FLAG_HIDDEN);
}

void OnDeviceStateChanged(DeviceState state)
{
    if (state == s_last_state) {
        return;
    }
    s_last_state = state;
    switch (state) {
    case kDeviceStateConnecting:
        UpdateStatusLabel(I18n::T("连接中..."));
        ShowListenIndicator(-1);
        break;
    case kDeviceStateListening:
        UpdateStatusLabel(I18n::T("聆听中..."));
        ShowListenIndicator(0);
        break;
    case kDeviceStateSpeaking:
        UpdateStatusLabel(I18n::T("说话中..."));
        ShowListenIndicator(1);
        break;
    case kDeviceStateIdle:
    case kDeviceStateUnknown:
    default:
        UpdateStatusLabel(s_current_status[0] != '\0'
                              ? s_current_status
                              : I18n::T("待机"));
        ShowListenIndicator(-1);
        break;
    }
}

// ---- chat bubbles ----
void StyleBubble(lv_obj_t *bubble)
{
    screen_strip_obj_chrome(bubble);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(kColorBubbleBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, kBubbleRadius, LV_PART_MAIN);
    lv_obj_set_style_border_color(bubble, lv_color_hex(kColorBubbleBg),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, kBubbleBorder, LV_PART_MAIN);
    lv_obj_set_style_border_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bubble, kBubblePadX, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bubble, kBubblePadY, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
}

struct BubbleHandles {
    lv_obj_t *bubble;
    lv_obj_t *label;
};

BubbleHandles BuildBubble(lv_obj_t *parent, lv_align_t align, int32_t x_ofs,
                          int32_t y_ofs)
{
    lv_obj_t *bubble = lv_obj_create(parent);
    StyleBubble(bubble);
    lv_obj_set_width(bubble, 100);
    lv_obj_align(bubble, align, x_ofs, y_ofs);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorBubbleText),
                                LV_PART_MAIN);
    screen_make_input_passive(bubble);
    return {bubble, label};
}

void UpdateBubble(lv_obj_t *bubble, lv_obj_t *label, const char *text)
{
    if (bubble == nullptr || label == nullptr || text == nullptr) {
        return;
    }
    lv_point_t size;
    lv_text_get_size(&size, text, &font_puhui_30_4, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    int32_t text_w = size.x;
    if (text_w < 32) text_w = 32;
    int32_t bubble_w = text_w + kBubblePadX * 2 + kBubbleBorder * 2;
    if (bubble_w > kBubbleMaxW) bubble_w = kBubbleMaxW;

    lv_obj_set_width(bubble, bubble_w);
    lv_obj_set_width(label, bubble_w - kBubblePadX * 2 - kBubbleBorder * 2);
    lv_label_set_text(label, text);
    lv_obj_update_layout(label);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(bubble);
}

// ---- status bar ----
void UpdateStatusBar()
{
    if (s_ui.clock_lbl != nullptr) {
        time_t now = time(nullptr);
        struct tm tm_info = {};
        if (localtime_r(&now, &tm_info) != nullptr &&
            tm_info.tm_year >= 2025 - 1900) {
            char buf[32];
            strftime(buf, sizeof(buf), "%H:%M", &tm_info);
            lv_label_set_text(s_ui.clock_lbl, buf);
        } else {
            lv_label_set_text(s_ui.clock_lbl, "--:--");
        }
    }

    if (s_ui.net_icon_lbl != nullptr) {
        const char *icon = metalio_board_get_network_icon();
        if (icon != nullptr) {
            lv_label_set_text(s_ui.net_icon_lbl, icon);
        }
    }

    if (s_ui.battery_icon_lbl != nullptr) {
        int level = 0, charging = 0, discharging = 0;
        if (metalio_board_get_battery(&level, &charging, &discharging)) {
            const char *icon = FONT_AWESOME_BATTERY_EMPTY;
            if (charging) {
                icon = FONT_AWESOME_BATTERY_BOLT;
            } else if (level >= 80) {
                icon = FONT_AWESOME_BATTERY_FULL;
            } else if (level >= 60) {
                icon = FONT_AWESOME_BATTERY_THREE_QUARTERS;
            } else if (level >= 40) {
                icon = FONT_AWESOME_BATTERY_HALF;
            } else if (level >= 20) {
                icon = FONT_AWESOME_BATTERY_QUARTER;
            }
            lv_label_set_text(s_ui.battery_icon_lbl, icon);
        }
    }
}

void OnPollTimer(lv_timer_t * /*timer*/)
{
    if (s_ui.screen == nullptr) {
        return;
    }
    UpdateStatusBar();
    OnDeviceStateChanged(Application::GetInstance().GetDeviceState());
}

// ---- navigation / voice session ----
void schedule_voice_ui_start()
{
    if (s_voice_ui_held) return;
    s_voice_ui_held = true;
    Application::GetInstance().SetVoiceUiDesired(true);
}

void schedule_voice_ui_stop()
{
    if (!s_voice_ui_held) return;
    s_voice_ui_held = false;
    Application::GetInstance().SetVoiceUiDesired(false);
}

void OnSwipeBack()
{
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    schedule_voice_ui_stop();
    HomeScreen::SwitchToHome();
}

void OnBackClicked(lv_event_t * /*e*/) { OnSwipeBack(); }

void OnScreenUnloaded(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_ui.screen) {
        return;
    }
    schedule_voice_ui_stop();
    if (s_ui.poll_timer != nullptr) {
        lv_timer_delete(s_ui.poll_timer);
        s_ui.poll_timer = nullptr;
    }
    s_ui = UiState{};
}

}  // namespace

// ---- Screen base class interface ----

lv_obj_t *EspClawScreen::CreateStatic()
{
    if (s_ui.screen != nullptr) {
        schedule_voice_ui_stop();
        if (s_ui.poll_timer != nullptr) {
            lv_timer_delete(s_ui.poll_timer);
            s_ui.poll_timer = nullptr;
        }
        s_ui = UiState{};
    }

    ESP_LOGI(TAG, "create espclaw (emote) screen");

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    // Header: back button + title
    lv_obj_t *header = lv_obj_create(scr);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kStatusBarH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 44, 44);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);
    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    s_ui.clock_lbl = lv_label_create(header);
    lv_label_set_text(s_ui.clock_lbl, "--:--");
    lv_obj_set_style_text_color(s_ui.clock_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.clock_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(s_ui.clock_lbl, LV_ALIGN_LEFT_MID, 64, 0);

    s_ui.battery_icon_lbl = lv_label_create(header);
    lv_label_set_text(s_ui.battery_icon_lbl, FONT_AWESOME_BATTERY_EMPTY);
    lv_obj_set_style_text_font(s_ui.battery_icon_lbl, &font_awesome_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.battery_icon_lbl, lv_color_white(),
                                LV_PART_MAIN);
    lv_obj_align(s_ui.battery_icon_lbl, LV_ALIGN_RIGHT_MID, -16, 0);

    s_ui.net_icon_lbl = lv_label_create(header);
    lv_label_set_text(s_ui.net_icon_lbl, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(s_ui.net_icon_lbl, &font_awesome_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.net_icon_lbl, lv_color_white(),
                                LV_PART_MAIN);
    lv_obj_align(s_ui.net_icon_lbl, LV_ALIGN_RIGHT_MID, -64, 0);

    // Status / toast text below the header
    s_ui.status_lbl = lv_label_create(scr);
    lv_label_set_text(s_ui.status_lbl, "");
    lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(0x9AA3B2),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.status_lbl, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_align(s_ui.status_lbl, LV_ALIGN_TOP_MID, 0, kStatusBarH + 8);
    screen_make_input_passive(s_ui.status_lbl);

    // Central emotion face + fallback hint
    s_ui.face = lv_image_create(scr);
    lv_image_set_inner_align(s_ui.face, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(s_ui.face, kPanelSize, kPanelSize - kStatusBarH);
    lv_obj_align(s_ui.face, LV_ALIGN_TOP_MID, 0, kStatusBarH);
    lv_obj_remove_flag(s_ui.face, LV_OBJ_FLAG_CLICKABLE);
    screen_make_input_passive(s_ui.face);

    s_ui.face_hint = lv_label_create(scr);
    lv_label_set_text(s_ui.face_hint, "");
    lv_obj_set_style_text_color(s_ui.face_hint, lv_color_hex(0x6B7280),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.face_hint, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(s_ui.face_hint, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(s_ui.face_hint, LV_OBJ_FLAG_HIDDEN);
    screen_make_input_passive(s_ui.face_hint);

    SetFaceSrc(s_current_emotion);

    // Listen / speak indicator near the bottom of the face
    lv_obj_t *ind = lv_obj_create(scr);
    screen_strip_obj_chrome(ind);
    lv_obj_set_size(ind, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(ind, LV_ALIGN_BOTTOM_MID, 0, -kUserBubbleBottom - 70);
    lv_obj_set_style_bg_opa(ind, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(ind, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ind, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ind, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ind, 10, LV_PART_MAIN);
    screen_make_input_passive(ind);

    s_ui.listen_icon = lv_label_create(ind);
    lv_label_set_text(s_ui.listen_icon, FONT_AWESOME_MICROPHONE);
    lv_obj_set_style_text_font(s_ui.listen_icon, &font_awesome_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.listen_icon, lv_color_hex(0x60A5FA),
                                LV_PART_MAIN);
    s_ui.listen_lbl = lv_label_create(ind);
    lv_label_set_text(s_ui.listen_lbl, "");
    lv_obj_set_style_text_font(s_ui.listen_lbl, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.listen_lbl, lv_color_white(),
                                LV_PART_MAIN);
    ShowListenIndicator(-1);

    // Chat bubbles
    {
        BubbleHandles sys = BuildBubble(scr, LV_ALIGN_TOP_LEFT, kSideMargin,
                                        kSysBubbleTop);
        s_ui.system_bubble = sys.bubble;
        s_ui.system_label  = sys.label;

        BubbleHandles usr = BuildBubble(scr, LV_ALIGN_BOTTOM_MID, 0,
                                        -kUserBubbleBottom);
        s_ui.user_bubble = usr.bubble;
        s_ui.user_label  = usr.label;
    }

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    s_last_state = kDeviceStateUnknown;
    UpdateStatusBar();
    s_ui.poll_timer = lv_timer_create(OnPollTimer, 1000, nullptr);

    return scr;
}

lv_obj_t *EspClawScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

bool EspClawScreen::IsActive()
{
    return s_ui.screen != nullptr;
}

void EspClawScreen::SetEmotion(const char *category)
{
    const char *name = EmotionCategoryName(category);
    std::strncpy(s_current_emotion, name, sizeof(s_current_emotion) - 1);
    s_current_emotion[sizeof(s_current_emotion) - 1] = '\0';
    ESP_LOGI(TAG, "SetEmotion -> %s", s_current_emotion);
    if (IsActive()) {
        SetFaceSrc(s_current_emotion);
    }
}

void EspClawScreen::SetStatus(const char *text)
{
    std::strncpy(s_current_status, text != nullptr ? text : "",
                 sizeof(s_current_status) - 1);
    s_current_status[sizeof(s_current_status) - 1] = '\0';
    if (IsActive()) {
        UpdateStatusLabel(s_current_status);
    }
}

void EspClawScreen::SetChatMessage(const char *role, const char *content)
{
    if (!IsActive()) {
        return;
    }
    if (role == nullptr || role[0] == '\0') {
        UpdateBubble(s_ui.system_bubble, s_ui.system_label, content);
        lv_obj_align(s_ui.system_bubble, LV_ALIGN_TOP_LEFT, kSideMargin,
                     kSysBubbleTop);
        return;
    }
    if (std::strcmp(role, "user") == 0) {
        UpdateBubble(s_ui.user_bubble, s_ui.user_label, content);
        lv_obj_align(s_ui.user_bubble, LV_ALIGN_BOTTOM_MID, 0,
                     -kUserBubbleBottom);
    } else {
        UpdateBubble(s_ui.system_bubble, s_ui.system_label, content);
        lv_obj_align(s_ui.system_bubble, LV_ALIGN_TOP_LEFT, kSideMargin,
                     kSysBubbleTop);
    }
}

void EspClawScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: espclaw_screen -> start voice UI");
        schedule_voice_ui_start();
    } else {
        ESP_LOGI(TAG, "unload: espclaw_screen -> stop voice UI");
        schedule_voice_ui_stop();
    }
}
