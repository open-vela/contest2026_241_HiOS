/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ChatScreen — ported from MetalioClaw4
 * main/display/screen/chat_screen/chat_screen.cc (1255 lines).
 *
 * openvela / NuttX adaptations:
 *   - ESP-IDF headers → shim headers (esp_log_shim).
 *   - lv_eaf.h (EAF vector animation widget) → not available on NuttX;
 *     the emotion animation is stubbed with a large centered text label
 *     showing the emotion name. The caption bubble, chat bubbles, header
 *     menu, device-state polling, and activation-block dialog are all
 *     ported verbatim.
 *   - Application / DeviceState / SdCardManager → available via the
 *     openvela port (application.h, device_state.h, port/SdCardManager.hpp).
 *   - LVGL widget construction code is ported verbatim; only the EAF
 *     widget creation / src-loading is replaced.
 */

#include "chat_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "application.h"
#include "device_state.h"
#include "settings.h"
#include "SdCardManager.hpp"
#include "home_screen/home_screen.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <cstdio>
#include <cstring>

LV_FONT_DECLARE(font_chat_cjk_30);

namespace {

constexpr const char *TAG = "ChatScreen";

// ---------------------------------------------------------------------------
// 720x720 visual parameters
// ---------------------------------------------------------------------------
constexpr int32_t kPanelW          = 720;
constexpr int32_t kPanelH          = 720;
constexpr int32_t kHeaderH         = 88;
constexpr int32_t kBackBtnSize     = 72;
constexpr int32_t kListPadH        = 18;
constexpr int32_t kListPadTop      = 14;
constexpr int32_t kListPadBottom   = 32;
constexpr int32_t kRowGap          = 12;
constexpr int32_t kBubblePadX      = 18;
constexpr int32_t kBubblePadY      = 14;
constexpr int32_t kBubbleRadius    = 18;
constexpr int32_t kSideMargin      = 8;
constexpr int32_t kMaxMessages     = 12;
constexpr int32_t kHeaderRightPad  = 16;
constexpr int32_t kMenuBtnSize     = 72;
constexpr int32_t kMenuIconSize    = 40;
constexpr int32_t kMenuPanelW      = 220;
constexpr int32_t kMenuItemH       = 64;
constexpr int32_t kMenuPanelRadius = 16;
constexpr int32_t kMenuPanelGap    = 4;

// Emotion: server emotion name → display label. On ESP-IDF this was
// S:/sdcard/system/chat/{emotion}.eaf played by lv_eaf. On openvela we
// show the emotion name as large centered text.
constexpr const char *kDefaultEmotion = "neutral";
constexpr size_t kEmotionNameMax      = 39;

constexpr int32_t kEmotionBubbleBorder = 0;
constexpr int32_t kEmotionBubbleSide   = 24;
constexpr int32_t kCaptionBottom       = 36;
constexpr int32_t kEmotionBubbleMaxW   = kPanelW - kEmotionBubbleSide * 2;

constexpr uint32_t kColorBg                 = 0x0E1116;
constexpr uint32_t kColorHeaderBg           = 0x12151C;
constexpr uint32_t kColorDivider            = 0x2A2F3A;
constexpr uint32_t kColorHeaderText         = 0xFFFFFF;
constexpr uint32_t kColorHeaderBtn          = 0x2A2F3A;
constexpr uint32_t kColorHeaderBtnBorder    = 0x3B4556;
constexpr uint32_t kColorHeaderBtnText      = 0xE5E7EB;
constexpr uint32_t kColorModeSelectedBg     = 0x1E3A2F;
constexpr uint32_t kColorModeSelectedBorder = 0x34D399;
constexpr uint32_t kColorMenuPanelBg        = 0x1B2030;
constexpr uint32_t kColorLeftBubble         = 0x202736;
constexpr uint32_t kColorRightBubble        = 0x1E3A2F;
constexpr uint32_t kColorRightBubbleText    = 0xE8F5E9;
constexpr uint32_t kColorLeftBubbleText     = 0xE5E7EB;
constexpr uint32_t kColorHintText           = 0x9AA3B2;
constexpr uint32_t kColorStateIdle          = 0x9AA3B2;
constexpr uint32_t kColorStateListening     = 0x34D399;
constexpr uint32_t kColorStateSpeaking      = 0x60A5FA;
constexpr uint32_t kColorStateConnecting    = 0xFBBF24;
constexpr uint32_t kColorEmotionBubbleBg    = 0x000000;
constexpr uint32_t kColorEmotionBubbleText  = 0xFFFFFF;
constexpr lv_opa_t kEmotionBubbleBgOpa      = LV_OPA_40;

constexpr const char kEmptyHint[] =
    "快来和我聊天吧\n用 \"Hi openvela\" 唤醒我";

enum class ViewMode : uint8_t { Chat, Emotion };

struct UiState {
    lv_obj_t *screen           = nullptr;
    lv_obj_t *header           = nullptr;
    lv_obj_t *msg_list         = nullptr;
    lv_obj_t *empty_hint       = nullptr;
    lv_obj_t *status_state_lbl = nullptr;
    lv_obj_t *menu_btn         = nullptr;
    lv_obj_t *menu_mask        = nullptr;
    lv_obj_t *menu_panel       = nullptr;
    lv_obj_t *menu_item_emotion = nullptr;
    lv_obj_t *menu_item_chat    = nullptr;
    lv_obj_t *menu_item_interrupt = nullptr;
    lv_obj_t *menu_item_interrupt_lbl = nullptr;
    lv_obj_t *menu_item_clear   = nullptr;
    lv_obj_t *emotion_panel    = nullptr;
    lv_obj_t *emotion_label    = nullptr;  // stub for lv_eaf (text label)
    lv_obj_t *caption_bubble   = nullptr;
    lv_obj_t *caption_label    = nullptr;
    lv_obj_t *activation_mask  = nullptr;
    lv_timer_t *state_timer    = nullptr;
    lv_timer_t *activation_guard_timer = nullptr;
};

UiState s_ui;
DeviceState s_last_device_state = kDeviceStateUnknown;
ViewMode s_view_mode = ViewMode::Chat;
bool s_activation_blocked = false;
bool s_activation_dialog_shows_code = false;
bool s_header_visible = true;
bool s_voice_ui_held = false;

char s_current_emotion[kEmotionNameMax + 1] = "neutral";
char s_applied_emotion[kEmotionNameMax + 1] = "";

const lv_font_t *chat_font() { return &font_chat_cjk_30; }
const lv_font_t *chat_font_sm() { return &font_chat_cjk_30; }

/* Drop codepoints missing from font_chat_cjk_30 so LVGL does not paint □. */
void FilterChatTextToFontInPlace(char *s)
{
    if (s == nullptr)
        return;
    const lv_font_t *font = chat_font();
    unsigned char *p = reinterpret_cast<unsigned char *>(s);
    unsigned char *w = p;
    while (*p)
    {
        uint32_t cp = 0;
        int len = 0;
        if (*p < 0x80)
        {
            cp = *p;
            len = 1;
        }
        else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
        {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            len = 2;
        }
        else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
                 (p[2] & 0xC0) == 0x80)
        {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) |
                 ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            len = 3;
        }
        else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
                 (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
        {
            cp = ((uint32_t)(p[0] & 0x07) << 18) |
                 ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            len = 4;
        }
        else
        {
            ++p;
            continue;
        }

        bool keep = false;
        if (cp == '\n' || cp == '\r' || cp == '\t')
            keep = true;
        else
        {
            lv_font_glyph_dsc_t gdsc;
            std::memset(&gdsc, 0, sizeof(gdsc));
            keep = lv_font_get_glyph_dsc(font, &gdsc, cp, 0);
            /* Missing glyphs: API returns false but may fill a □ placeholder. */
            if (!keep || gdsc.is_placeholder)
                keep = false;
            else if (gdsc.box_w == 0 && cp > 0x20)
                keep = false;
        }
        if (keep)
        {
            for (int i = 0; i < len; ++i)
                *w++ = *p++;
        }
        else
        {
            p += len;
        }
    }
    *w = '\0';
}

// ---------------------------------------------------------------------------
// Emotion name validation (ported verbatim)
// ---------------------------------------------------------------------------
bool IsSafeEmotionName(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    size_t len = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name);
         *p != '\0'; ++p, ++len) {
        if (len > kEmotionNameMax) {
            return false;
        }
        if (!(std::isalnum(*p) || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return len > 0;
}

const char *NormalizeEmotionName(const char *emotion)
{
    if (!IsSafeEmotionName(emotion)) {
        if (emotion != nullptr && emotion[0] != '\0') {
            ESP_LOGW(TAG, "reject unsafe emotion name: %s", emotion);
        }
        return kDefaultEmotion;
    }
    return emotion;
}

void CopyEmotionName(char *dst, size_t dst_size, const char *emotion)
{
    const char *name = NormalizeEmotionName(emotion);
    std::strncpy(dst, name, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

// ---------------------------------------------------------------------------
// EAF stub — on openvela the EAF vector animation widget is not available.
// We render a large centered text label showing the emotion name instead.
// The control flow (ensure / apply / pause / resume / stop) is preserved
// so that switching to a real animation decoder later is a drop-in change.
// ---------------------------------------------------------------------------
void ensure_emotion_label()
{
    if (s_ui.emotion_label != nullptr || s_ui.emotion_panel == nullptr) {
        return;
    }
    s_ui.emotion_label = lv_label_create(s_ui.emotion_panel);
    lv_label_set_text(s_ui.emotion_label, kDefaultEmotion);
    lv_obj_set_style_text_font(s_ui.emotion_label, chat_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.emotion_label,
                                lv_color_hex(kColorEmotionBubbleText), LV_PART_MAIN);
    lv_obj_center(s_ui.emotion_label);
    screen_make_input_passive(s_ui.emotion_label);
}

void ApplyEmotionSrc(const char *emotion)
{
    if (s_ui.emotion_label == nullptr) {
        return;
    }
    const char *name = NormalizeEmotionName(emotion);
    if (s_applied_emotion[0] != '\0' &&
        std::strcmp(s_applied_emotion, name) == 0) {
        return;
    }
    lv_label_set_text(s_ui.emotion_label, name);
    lv_obj_center(s_ui.emotion_label);
    std::strncpy(s_applied_emotion, name, sizeof(s_applied_emotion) - 1);
    s_applied_emotion[sizeof(s_applied_emotion) - 1] = '\0';
}

void pause_emotion_eaf()
{
    // No-op: text label has no animation to pause.
}

void resume_emotion_eaf()
{
    // No-op: text label has no animation to resume.
}

void stop_emotion_eaf()
{
    if (s_ui.emotion_label == nullptr) {
        return;
    }
    lv_obj_delete(s_ui.emotion_label);
    s_ui.emotion_label = nullptr;
    s_applied_emotion[0] = '\0';
}

void ensure_emotion_eaf()
{
    ensure_emotion_label();
}

// ---------------------------------------------------------------------------
// Voice UI session (ported verbatim)
// ---------------------------------------------------------------------------
bool is_device_activated();  // forward decl
void delete_timer(lv_timer_t *&timer);
void chat_scroll_to_latest(bool anim);

void schedule_voice_ui_start()
{
    /* Claw4: enter chat → RequestVoiceUiDesired(true) on app thread.
     * Never call SetVoiceUiDesired/Schedule from an LVGL timer — that path
     * deadlocks (LVGL lock vs heap) and hard-hangs after VUI_ON.
     * Wait for a few LVGL frames so chat paints before WW/mic arming.
     * Do NOT auto-RequestWakeSession: only "Hi openvela" (WakeNet) or an
     * explicit tap on「待唤醒」may open the dialogue. */
    s_voice_ui_held = true;
    lv_timer_t *t = lv_timer_create(
        [](lv_timer_t *timer) {
            lv_timer_delete(timer);
            if (!s_voice_ui_held || !ChatScreen::IsActive())
                return;
            write(1, "VUI_ON\n", 7);
            Application::GetInstance().RequestVoiceUiDesired(true);
        },
        300, nullptr);
    if (t != nullptr)
        lv_timer_set_repeat_count(t, 1);
}

void schedule_voice_ui_stop()
{
    if (!s_voice_ui_held)
        return;
    s_voice_ui_held = false;
    Application::GetInstance().RequestVoiceUiDesired(false);
}

void chat_page_leave()
{
    delete_timer(s_ui.state_timer);
    delete_timer(s_ui.activation_guard_timer);
    stop_emotion_eaf();
    schedule_voice_ui_stop();
}

// ---------------------------------------------------------------------------
// Header: device chat state (ported verbatim)
// ---------------------------------------------------------------------------
bool chat_status_for_state(DeviceState state, const char **text, uint32_t *color)
{
    switch (state) {
        case kDeviceStateIdle:
            *text = "待唤醒";
            *color = kColorStateIdle;
            return true;
        case kDeviceStateListening:
            *text = "聆听中";
            *color = kColorStateListening;
            return true;
        case kDeviceStateSpeaking:
            *text = "讲话中";
            *color = kColorStateSpeaking;
            return true;
        case kDeviceStateConnecting:
            *text = "连接中";
            *color = kColorStateConnecting;
            return true;
        default:
            /* Never hide the status chip — Activating/Starting/Unknown used
             * to return false and LV_OBJ_FLAG_HIDDEN the label permanently. */
            *text = "待唤醒";
            *color = kColorStateIdle;
            return true;
    }
}

void chat_update_device_state_label()
{
    if (s_ui.status_state_lbl == nullptr) {
        return;
    }

    const DeviceState state = Application::GetInstance().GetDeviceState();
    const char *text = "待唤醒";
    uint32_t color = kColorStateIdle;
    (void)chat_status_for_state(state, &text, &color);
    const char *shown = I18n::T(text);
    const char *cur = lv_label_get_text(s_ui.status_state_lbl);

    if (state == s_last_device_state &&
        !lv_obj_has_flag(s_ui.status_state_lbl, LV_OBJ_FLAG_HIDDEN) &&
        cur != nullptr && std::strcmp(cur, shown) == 0) {
        return;
    }
    s_last_device_state = state;

    lv_label_set_text(s_ui.status_state_lbl, shown);
    lv_obj_set_style_text_color(s_ui.status_state_lbl, lv_color_hex(color),
                                LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.status_state_lbl, LV_OBJ_FLAG_HIDDEN);
}

void on_chat_status_timer(lv_timer_t * /*timer*/)
{
    chat_update_device_state_label();
}

void on_refresh_device_state_async(void * /*user_data*/)
{
    chat_update_device_state_label();
}

// ---------------------------------------------------------------------------
// List helpers (ported verbatim)
// ---------------------------------------------------------------------------
void chat_update_empty_hint()
{
    if (s_ui.empty_hint == nullptr || s_ui.msg_list == nullptr) {
        return;
    }
    const bool show = (s_view_mode == ViewMode::Chat) &&
                      (lv_obj_get_child_count(s_ui.msg_list) == 0);
    if (show) {
        lv_obj_remove_flag(s_ui.empty_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.empty_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void chat_scroll_to_latest(bool anim)
{
    if (s_ui.msg_list == nullptr) {
        return;
    }
    const uint32_t count = lv_obj_get_child_count(s_ui.msg_list);
    if (count == 0) {
        return;
    }
    lv_obj_t *latest = lv_obj_get_child(s_ui.msg_list, count - 1);
    if (latest != nullptr) {
        lv_obj_scroll_to_view_recursive(latest, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    }
}

void chat_trim_old_msgs()
{
    if (s_ui.msg_list == nullptr) {
        return;
    }
    while (static_cast<int32_t>(lv_obj_get_child_count(s_ui.msg_list)) >
           kMaxMessages) {
        lv_obj_t *oldest = lv_obj_get_child(s_ui.msg_list, 0);
        if (oldest == nullptr) {
            break;
        }
        lv_obj_delete(oldest);
    }
}

void chat_clear_msg_list()
{
    if (s_ui.msg_list == nullptr) {
        return;
    }
    const uint32_t count = lv_obj_get_child_count(s_ui.msg_list);
    for (int32_t i = static_cast<int32_t>(count) - 1; i >= 0; --i) {
        lv_obj_delete(lv_obj_get_child(s_ui.msg_list, i));
    }
}

// ---------------------------------------------------------------------------
// Bubble construction (ported verbatim)
// ---------------------------------------------------------------------------
void chat_create_bubble_row(const char *text, ChatMsgDir dir)
{
    if (s_ui.msg_list == nullptr || text == nullptr) {
        return;
    }

    const bool is_right = (dir == ChatMsgDir::Right);
    const lv_font_t *font = chat_font();

    lv_obj_t *row = lv_obj_create(s_ui.msg_list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, kRowGap, LV_PART_MAIN);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, kBubbleRadius, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bubble, kBubblePadX, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bubble, kBubblePadY, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t max_bubble_w = kPanelW * 72 / 100;
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t text_w = size.x;
    if (text_w < 24) {
        text_w = 24;
    }
    int32_t bubble_w = text_w + kBubblePadX * 2;
    if (bubble_w > max_bubble_w) {
        bubble_w = max_bubble_w;
    }
    lv_obj_set_width(bubble, bubble_w);

    if (is_right) {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(kColorRightBubble),
                                  LV_PART_MAIN);
        lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, -kSideMargin, 0);
    } else {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(kColorLeftBubble),
                                  LV_PART_MAIN);
        lv_obj_align(bubble, LV_ALIGN_TOP_LEFT, kSideMargin, 0);
    }
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, bubble_w - kBubblePadX * 2);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(
        label,
        lv_color_hex(is_right ? kColorRightBubbleText : kColorLeftBubbleText),
        LV_PART_MAIN);
    lv_obj_update_layout(label);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    screen_make_input_passive(row);
}

// ---------------------------------------------------------------------------
// Emotion-mode caption bubble (ported verbatim)
// ---------------------------------------------------------------------------
void StyleCaptionBubble(lv_obj_t *bubble)
{
    screen_strip_obj_chrome(bubble);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(kColorEmotionBubbleBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, kEmotionBubbleBgOpa, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, kBubbleRadius, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, kEmotionBubbleBorder, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bubble, kBubblePadX, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bubble, kBubblePadY, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
}

void BuildCaption(lv_obj_t *parent)
{
    lv_obj_t *bubble = lv_obj_create(parent);
    StyleCaptionBubble(bubble);
    lv_obj_set_width(bubble, 100);
    lv_obj_align(bubble, LV_ALIGN_BOTTOM_MID, 0, -kCaptionBottom);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, chat_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorEmotionBubbleText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    screen_make_input_passive(bubble);

    s_ui.caption_bubble = bubble;
    s_ui.caption_label = label;
}

void ClearEmotionCaption()
{
    if (s_ui.caption_label != nullptr) {
        lv_label_set_text(s_ui.caption_label, "");
    }
    if (s_ui.caption_bubble != nullptr) {
        lv_obj_add_flag(s_ui.caption_bubble, LV_OBJ_FLAG_HIDDEN);
    }
}

void ShowEmotionCaption(const char *text)
{
    if (s_view_mode != ViewMode::Emotion || text == nullptr || text[0] == '\0' ||
        s_ui.caption_bubble == nullptr || s_ui.caption_label == nullptr) {
        return;
    }

    const lv_font_t *font = chat_font();
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t text_w = size.x;
    if (text_w < 32) {
        text_w = 32;
    }
    int32_t bubble_w = text_w + kBubblePadX * 2 + kEmotionBubbleBorder * 2;
    if (bubble_w > kEmotionBubbleMaxW) {
        bubble_w = kEmotionBubbleMaxW;
    }

    lv_obj_set_width(s_ui.caption_bubble, bubble_w);
    lv_obj_set_width(s_ui.caption_label,
                     bubble_w - kBubblePadX * 2 - kEmotionBubbleBorder * 2);
    lv_label_set_text(s_ui.caption_label, text);
    lv_obj_set_style_text_align(s_ui.caption_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_update_layout(s_ui.caption_label);
    lv_obj_set_height(s_ui.caption_bubble, LV_SIZE_CONTENT);
    lv_obj_remove_flag(s_ui.caption_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_ui.caption_bubble, LV_ALIGN_BOTTOM_MID, 0, -kCaptionBottom);
    lv_obj_update_layout(s_ui.caption_bubble);
}

// ---------------------------------------------------------------------------
// Device activation check (ported verbatim)
// ---------------------------------------------------------------------------
bool is_device_activated()
{
    return Application::GetInstance().IsDeviceActivated();
}

void log_activation_blocked()
{
    auto &app = Application::GetInstance();
    ESP_LOGW(TAG, "Chat blocked: device not activated (boot_ready=%d pending=%d state=%d)",
             app.IsBootReady() ? 1 : 0,
             app.HasPendingActivation() ? 1 : 0,
             static_cast<int>(app.GetDeviceState()));
    if (app.HasPendingActivation()) {
        ESP_LOGW(TAG, "pending activation code: %s",
                 app.GetPendingActivationCode().c_str());
    }
}

void on_swipe_back();

void close_activation_blocked_dialog()
{
    if (s_ui.activation_mask != nullptr) {
        lv_obj_delete(s_ui.activation_mask);
        s_ui.activation_mask = nullptr;
    }
    s_activation_dialog_shows_code = false;
}

void open_activation_blocked_dialog()
{
    if (s_ui.screen == nullptr || s_ui.activation_mask != nullptr) {
        return;
    }

    auto &app = Application::GetInstance();
    const bool has_code = app.HasPendingActivation();
    s_activation_dialog_shows_code = has_code;

    constexpr int32_t kCardW = 520;
    const int32_t kCardH = has_code ? 460 : 340;
    constexpr int32_t kBackBtnW = 200;
    constexpr int32_t kBackBtnH = 72;

    lv_obj_t *mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.activation_mask = mask;

    lv_obj_t *card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("设备未激活"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, chat_font(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *desc = lv_label_create(card);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, kCardW - 56);
    lv_label_set_text(desc, has_code
        ? I18n::T("请打开 xiaozhi.me，输入下方 6 位验证码绑定本设备。")
        : I18n::T("正在连接 xiaozhi.me 获取验证码，请稍候。"));
    lv_obj_set_style_text_color(desc, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(desc, chat_font_sm(), LV_PART_MAIN);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, has_code ? -48 : -10);
    lv_obj_remove_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    if (has_code) {
        char code_buf[64];
        std::snprintf(code_buf, sizeof(code_buf), I18n::T("验证码: %s"),
                      app.GetPendingActivationCode().c_str());
        lv_obj_t *code_lbl = lv_label_create(card);
        lv_label_set_text(code_lbl, code_buf);
        lv_obj_set_style_text_color(code_lbl, lv_color_hex(0xFBBF24), LV_PART_MAIN);
        lv_obj_set_style_text_font(code_lbl, chat_font(), LV_PART_MAIN);
        lv_obj_align(code_lbl, LV_ALIGN_BOTTOM_MID, 0, -(kBackBtnH + 36));
        lv_obj_remove_flag(code_lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *back = lv_button_create(card);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnW, kBackBtnH);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 16, LV_PART_MAIN);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(back, [](lv_event_t * /*e*/) { on_swipe_back(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, I18n::T("返回"));
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_font(back_lbl, chat_font(), LV_PART_MAIN);
    lv_obj_center(back_lbl);
    lv_obj_remove_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void ensure_activation_blocked_dialog()
{
    if (s_activation_blocked && s_ui.activation_mask == nullptr) {
        open_activation_blocked_dialog();
    }
}

void sync_activation_block_state()
{
    auto &app = Application::GetInstance();
    const bool should_block = !is_device_activated();
    const bool has_code = app.HasPendingActivation();
    if (should_block == s_activation_blocked) {
        if (should_block) {
            if (has_code && !s_activation_dialog_shows_code) {
                close_activation_blocked_dialog();
                open_activation_blocked_dialog();
            } else {
                ensure_activation_blocked_dialog();
            }
        }
        return;
    }
    s_activation_blocked = should_block;
    if (should_block) {
        log_activation_blocked();
        open_activation_blocked_dialog();
    } else {
        ESP_LOGI(TAG, "Chat unblocked: device activated/ready");
        close_activation_blocked_dialog();
        schedule_voice_ui_start();
    }
}

void on_activation_guard_timer(lv_timer_t * /*timer*/)
{
    sync_activation_block_state();
}

bool reject_if_blocked()
{
    sync_activation_block_state();
    if (!s_activation_blocked) {
        return false;
    }
    ensure_activation_blocked_dialog();
    return true;
}

void on_status_wake_clicked(lv_event_t * /*e*/)
{
    if (reject_if_blocked())
        return;
    /* Xiaozhi-style: tap status to start the same session as wake word. */
    Application::GetInstance().RequestWakeSession();
}

// ---------------------------------------------------------------------------
// View mode + header dropdown menu (ported verbatim)
// ---------------------------------------------------------------------------
enum class MenuAction : uintptr_t {
    Emotion   = 0,
    Chat      = 1,
    Clear     = 2,
    Interrupt = 3,
};

void style_menu_item(lv_obj_t *btn, bool selected)
{
    if (btn == nullptr) {
        return;
    }
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, selected ? 1 : 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        btn, lv_color_hex(selected ? kColorModeSelectedBg : kColorHeaderBtn),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorModeSelectedBorder),
                                  LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3B4556),
                              LV_PART_MAIN | LV_STATE_PRESSED);
}

void set_obj_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj == nullptr) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

bool is_header_menu_open()
{
    return s_ui.menu_panel != nullptr &&
           !lv_obj_has_flag(s_ui.menu_panel, LV_OBJ_FLAG_HIDDEN);
}

bool is_interrupt_pref_enabled()
{
    Settings settings("audio");
    return settings.GetBool("interrupt", false);
}

void refresh_header_menu_selection()
{
    const bool chat = (s_view_mode == ViewMode::Chat);
    style_menu_item(s_ui.menu_item_chat, chat);
    style_menu_item(s_ui.menu_item_emotion, !chat);
    set_obj_hidden(s_ui.menu_item_clear, !chat);

    const bool interrupt_on = is_interrupt_pref_enabled();
    style_menu_item(s_ui.menu_item_interrupt, interrupt_on);
    if (s_ui.menu_item_interrupt_lbl != nullptr) {
        lv_label_set_text(s_ui.menu_item_interrupt_lbl,
                          interrupt_on ? I18n::T("打断：开")
                                       : I18n::T("打断：关"));
    }
}

void close_header_menu()
{
    set_obj_hidden(s_ui.menu_mask, true);
    set_obj_hidden(s_ui.menu_panel, true);
}

void open_header_menu()
{
    refresh_header_menu_selection();
    set_obj_hidden(s_ui.menu_mask, false);
    set_obj_hidden(s_ui.menu_panel, false);
    if (s_ui.menu_mask != nullptr) {
        lv_obj_move_foreground(s_ui.menu_mask);
    }
    if (s_ui.menu_panel != nullptr) {
        lv_obj_move_foreground(s_ui.menu_panel);
    }
}

void set_header_visible(bool visible)
{
    s_header_visible = visible;
    set_obj_hidden(s_ui.header, !visible);
    if (!visible) {
        close_header_menu();
    } else if (s_ui.header != nullptr) {
        lv_obj_move_foreground(s_ui.header);
    }
}

void on_emotion_panel_clicked(lv_event_t * /*e*/)
{
    if (s_view_mode != ViewMode::Emotion || reject_if_blocked()) {
        return;
    }
    if (is_header_menu_open()) {
        close_header_menu();
        return;
    }
    set_header_visible(!s_header_visible);
}

void apply_view_mode(ViewMode mode)
{
    const bool changed = (mode != s_view_mode);
    s_view_mode = mode;
    const bool chat = (mode == ViewMode::Chat);

    set_obj_hidden(s_ui.msg_list, !chat);
    set_obj_hidden(s_ui.emotion_panel, chat);

    set_header_visible(true);

    if (chat) {
        pause_emotion_eaf();
    } else {
        ensure_emotion_eaf();
        if (changed || s_applied_emotion[0] == '\0') {
            ApplyEmotionSrc(s_current_emotion);
        }
        resume_emotion_eaf();
    }

    if (is_header_menu_open()) {
        refresh_header_menu_selection();
    }
    chat_update_empty_hint();
}

void on_clear_clicked()
{
    if (reject_if_blocked()) {
        return;
    }
    ChatScreen::ClearMessages();
}

void on_interrupt_clicked()
{
    if (reject_if_blocked()) {
        return;
    }
    auto &app = Application::GetInstance();
    /* Toggle interrupt / device AEC preference. */
    const bool currently_on = is_interrupt_pref_enabled();
    const AecMode next = currently_on ? kAecOff : kAecOnDeviceSide;
    ESP_LOGI(TAG, "interrupt pref -> %s", currently_on ? "off" : "on");
    app.SetAecMode(next);
    refresh_header_menu_selection();
}

void on_menu_item_clicked(lv_event_t *e)
{
    if (reject_if_blocked()) {
        close_header_menu();
        return;
    }
    const auto action = static_cast<MenuAction>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    close_header_menu();
    switch (action) {
    case MenuAction::Emotion:
        apply_view_mode(ViewMode::Emotion);
        break;
    case MenuAction::Chat:
        apply_view_mode(ViewMode::Chat);
        break;
    case MenuAction::Clear:
        on_clear_clicked();
        break;
    case MenuAction::Interrupt:
        on_interrupt_clicked();
        break;
    }
}

void on_menu_btn_clicked(lv_event_t * /*e*/)
{
    if (reject_if_blocked()) {
        return;
    }
    if (is_header_menu_open()) {
        close_header_menu();
    } else {
        open_header_menu();
    }
}

void on_menu_mask_clicked(lv_event_t * /*e*/)
{
    close_header_menu();
}

lv_obj_t *create_menu_item(lv_obj_t *parent, const char *text, MenuAction action,
                           lv_obj_t **out_label = nullptr)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, kMenuItemH);
    style_menu_item(btn, false);
    lv_obj_add_event_cb(btn, on_menu_item_clicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
    screen_swipe_back_ignore(btn, true);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorHeaderBtnText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, chat_font_sm(), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 20, 0);
    if (out_label != nullptr) {
        *out_label = lbl;
    }
    return btn;
}

void build_header_menu(lv_obj_t *parent)
{
    s_ui.menu_mask = lv_obj_create(parent);
    screen_strip_obj_chrome(s_ui.menu_mask);
    lv_obj_add_flag(s_ui.menu_mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_ui.menu_mask, kPanelW, kPanelH);
    lv_obj_set_pos(s_ui.menu_mask, 0, 0);
    lv_obj_set_style_bg_opa(s_ui.menu_mask, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.menu_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.menu_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.menu_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ui.menu_mask, on_menu_mask_clicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(s_ui.menu_mask, true);

    s_ui.menu_panel = lv_obj_create(parent);
    screen_strip_obj_chrome(s_ui.menu_panel);
    lv_obj_add_flag(s_ui.menu_panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_ui.menu_panel, kMenuPanelW, LV_SIZE_CONTENT);
    lv_obj_align(s_ui.menu_panel, LV_ALIGN_TOP_RIGHT, -kHeaderRightPad,
                 kHeaderH + kMenuPanelGap);
    lv_obj_set_style_bg_color(s_ui.menu_panel, lv_color_hex(kColorMenuPanelBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.menu_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.menu_panel, kMenuPanelRadius, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.menu_panel,
                                  lv_color_hex(kColorHeaderBtnBorder), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.menu_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.menu_panel, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.menu_panel, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.menu_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.menu_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_ui.menu_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.menu_panel, LV_OBJ_FLAG_HIDDEN);
    screen_swipe_back_ignore(s_ui.menu_panel, true);

    s_ui.menu_item_emotion =
        create_menu_item(s_ui.menu_panel, I18n::T("表情"), MenuAction::Emotion);
    s_ui.menu_item_chat =
        create_menu_item(s_ui.menu_panel, I18n::T("聊天"), MenuAction::Chat);
    s_ui.menu_item_interrupt =
        create_menu_item(s_ui.menu_panel, I18n::T("打断：关"), MenuAction::Interrupt,
                         &s_ui.menu_item_interrupt_lbl);
    s_ui.menu_item_clear =
        create_menu_item(s_ui.menu_panel, I18n::T("清空"), MenuAction::Clear);
    refresh_header_menu_selection();
}

// ---------------------------------------------------------------------------
// Screen navigation (ported verbatim)
// ---------------------------------------------------------------------------
void on_swipe_back()
{
    write(1, "CHAT_BACK\n", 10);
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    /* Create+load Home first (SwitchToHome), then chat delete fires
     * on_screen_unloaded → deferred VUI stop. Never tear down audio/UDP
     * on this LVGL tick — that pairing solid-blues the default theme. */
    HomeScreen::SwitchToHome();
}

void delete_timer(lv_timer_t *&timer)
{
    if (timer != nullptr) {
        lv_timer_delete(timer);
        timer = nullptr;
    }
}

void on_screen_unloaded(lv_event_t *e)
{
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target_obj(e));
    if (target != s_ui.screen) {
        return;
    }
    /* UI-only teardown. Voice stop is deferred (~1.2s hard release) so
     * SwitchToHome can finish without closing UDP on the same LVGL tick. */
    delete_timer(s_ui.state_timer);
    delete_timer(s_ui.activation_guard_timer);
    stop_emotion_eaf();
    s_ui = UiState{};
    s_activation_blocked = false;
    s_activation_dialog_shows_code = false;
    s_header_visible = true;
    if (s_voice_ui_held) {
        s_voice_ui_held = false;
        Application::GetInstance().RequestVoiceUiDesired(false);
    }
    s_last_device_state = kDeviceStateUnknown;
    s_applied_emotion[0] = '\0';
}

// ---------------------------------------------------------------------------
// UI assembly (ported verbatim, except EAF → text label)
// ---------------------------------------------------------------------------
void build_header(lv_obj_t *parent)
{
    lv_obj_t *header = lv_obj_create(parent);
    s_ui.header = header;
    screen_strip_obj_chrome(header);
    lv_obj_add_flag(header, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kColorHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *divider = lv_obj_create(header);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

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
    lv_obj_add_event_cb(back, [](lv_event_t * /*e*/) { on_swipe_back(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("聊天"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorHeaderText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, chat_font(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 12, 0);

    s_ui.status_state_lbl = lv_label_create(header);
    lv_label_set_text(s_ui.status_state_lbl, I18n::T("待唤醒"));
    lv_obj_set_style_text_font(s_ui.status_state_lbl, chat_font_sm(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.status_state_lbl,
                                lv_color_hex(kColorStateIdle), LV_PART_MAIN);
    lv_obj_align_to(s_ui.status_state_lbl, title, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    lv_obj_add_flag(s_ui.status_state_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.status_state_lbl, on_status_wake_clicked,
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(s_ui.status_state_lbl, true);

    s_last_device_state = kDeviceStateUnknown;
    chat_update_device_state_label();
    /* Fast status poll — Speaking/Listening felt lagged at 400ms. */
    s_ui.state_timer = lv_timer_create(on_chat_status_timer, 250, nullptr);

    s_ui.menu_btn = lv_button_create(header);
    lv_obj_remove_style_all(s_ui.menu_btn);
    lv_obj_set_size(s_ui.menu_btn, kMenuBtnSize, kMenuBtnSize);
    lv_obj_align(s_ui.menu_btn, LV_ALIGN_RIGHT_MID, -kHeaderRightPad, 0);
    lv_obj_set_style_bg_opa(s_ui.menu_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.menu_btn, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_ui.menu_btn, LV_OPA_20,
                            LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_ui.menu_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_ui.menu_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_ui.menu_btn, on_menu_btn_clicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(s_ui.menu_btn, true);

    lv_obj_t *menu_icon = lv_image_create(s_ui.menu_btn);
    lv_image_set_src(menu_icon, "A:ic_app_chat_menu.spng");
    lv_image_set_inner_align(menu_icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(menu_icon, kMenuIconSize, kMenuIconSize);
    lv_obj_remove_flag(menu_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(menu_icon);
}

void build_message_list(lv_obj_t *parent)
{
    s_ui.msg_list = lv_obj_create(parent);
    lv_obj_set_size(s_ui.msg_list, kPanelW, kPanelH - kHeaderH);
    lv_obj_set_pos(s_ui.msg_list, 0, kHeaderH);
    screen_strip_obj_chrome(s_ui.msg_list);
    lv_obj_set_style_bg_color(s_ui.msg_list, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.msg_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_ui.msg_list, kListPadH, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_ui.msg_list, kListPadH, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_ui.msg_list, kListPadTop, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_ui.msg_list, kListPadBottom, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_ui.msg_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_ui.msg_list, LV_DIR_VER);
    lv_obj_set_flex_flow(s_ui.msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.msg_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    s_ui.empty_hint = lv_label_create(parent);
    lv_label_set_text(s_ui.empty_hint, I18n::T(kEmptyHint));
    lv_obj_set_width(s_ui.empty_hint, kPanelW * 80 / 100);
    lv_label_set_long_mode(s_ui.empty_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_ui.empty_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.empty_hint, chat_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.empty_hint, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_align(s_ui.empty_hint, LV_ALIGN_TOP_MID, 0,
                 kHeaderH + (kPanelH - kHeaderH) / 2 - 50);
    screen_make_input_passive(s_ui.empty_hint);
}

void build_emotion_panel(lv_obj_t *parent)
{
    s_ui.emotion_panel = lv_obj_create(parent);
    screen_strip_obj_chrome(s_ui.emotion_panel);
    lv_obj_set_size(s_ui.emotion_panel, kPanelW, kPanelH);
    lv_obj_set_pos(s_ui.emotion_panel, 0, 0);
    lv_obj_set_style_bg_color(s_ui.emotion_panel, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.emotion_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.emotion_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.emotion_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ui.emotion_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ui.emotion_panel, on_emotion_panel_clicked,
                        LV_EVENT_CLICKED, nullptr);

    if (!SdCardManager::GetInstance().IsMounted()) {
        ESP_LOGW(TAG, "chat emotion: SD card not mounted");
        lv_obj_t *hint = lv_label_create(s_ui.emotion_panel);
        lv_label_set_text(
            hint, I18n::T("未检测到 SD 卡\n\n请将聊天表情资源放入 SD 卡\n"
                          "system/chat/ 目录"));
        lv_obj_set_width(hint, kPanelW - 80);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorHintText), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, chat_font(), LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
        screen_make_input_passive(hint);
    }
    // EAF widget creation is deferred to apply_view_mode() → ensure_emotion_eaf().
    // On openvela the EAF is stubbed as a text label (see ensure_emotion_label()).

    BuildCaption(s_ui.emotion_panel);
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================

lv_obj_t *ChatScreen::CreateStatic()
{
    // Defensive: if re-entered while previous screen still active, reset
    // static refs (the old screen object is async-deleted by LVGL).
    if (s_ui.screen != nullptr) {
        ESP_LOGW(TAG, "Create while previous screen still active, resetting refs");
        chat_page_leave();
        delete_timer(s_ui.state_timer);
        delete_timer(s_ui.activation_guard_timer);
        s_ui = UiState{};
        s_applied_emotion[0] = '\0';
        s_header_visible = true;
    }

    s_activation_blocked = !is_device_activated();
    if (s_activation_blocked) {
        log_activation_blocked();
    }

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    write(1, "CHAT0\n", 6);
    /* Strip default theme before first refresh — unstyled screens show as
     * solid blue (LVGL light primary) on this port. */
    lv_obj_remove_style_all(scr);
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);
    build_message_list(scr);
    build_emotion_panel(scr);
    build_header_menu(scr);

    /* No edge swipe-back on chat: mid-gesture noise was stopping voice wake
     * (CHAT_BACK). Leave via the explicit header back button only. */
    apply_view_mode(s_view_mode);

    if (s_activation_blocked) {
        open_activation_blocked_dialog();
    }
    s_ui.activation_guard_timer =
        lv_timer_create(on_activation_guard_timer, 500, nullptr);

    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    lv_obj_add_event_cb(
        scr,
        [](lv_event_t *e) {
            if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
                sync_activation_block_state();
            }
        },
        LV_EVENT_SCREEN_LOADED, nullptr);
    write(1, "CHAT1\n", 6);
    lv_obj_invalidate(scr);
    return scr;
}

lv_obj_t *ChatScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void ChatScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        /* Text bubbles are the primary dialogue surface (Claw4 default).
         * Do not RefreshDeviceState here — status timer + create-time label
         * are enough; an async refresh on the enter tick has raced home
         * teardown into a solid-blue hang. */
        apply_view_mode(ViewMode::Chat);
        if (!is_device_activated()) {
            ESP_LOGW(TAG, "load: chat_screen blocked (device not activated)");
            log_activation_blocked();
        } else {
            ESP_LOGI(TAG, "load: chat_screen -> schedule voice UI start");
            schedule_voice_ui_start();
        }
        write(1, "CHAT_LOAD\n", 10);
    } else {
        ESP_LOGI(TAG, "unload: chat_screen -> defer voice UI stop");
        /* UNLOAD fires while SwitchToHome is deleting this screen.
         * Do not CloseAudioChannel here — UI teardown only. Voice stop
         * is armed by on_screen_unloaded via SetVoiceUiDesired(false). */
        delete_timer(s_ui.state_timer);
        delete_timer(s_ui.activation_guard_timer);
        stop_emotion_eaf();
    }
}

bool ChatScreen::IsActive()
{
    return s_ui.screen != nullptr && s_ui.msg_list != nullptr;
}

void ChatScreen::RefreshDeviceState()
{
    if (!IsActive()) {
        return;
    }
    lv_async_call(on_refresh_device_state_async, nullptr);
}

void ChatScreen::AddMessage(const char *text, ChatMsgDir dir)
{
    if (text == nullptr || text[0] == '\0' || !IsActive() ||
        s_activation_blocked) {
        return;
    }

    /* Copy + drop glyphs not in font_chat_cjk_30 (cloud emoji / rare CJK
     * otherwise render as □). */
    const size_t n = std::strlen(text);
    char *filtered = static_cast<char *>(std::malloc(n + 1));
    if (filtered == nullptr)
        return;
    std::memcpy(filtered, text, n + 1);
    FilterChatTextToFontInPlace(filtered);
    if (filtered[0] == '\0') {
        std::free(filtered);
        return;
    }

    /* Keep scroll animation off — anim under voice load has blue-flashed.
     * Skip a second full-list update_layout; the bubble label is already
     * laid out (reduces full-plane flush flicker on RGB888 DIRECT). */
    chat_create_bubble_row(filtered, dir);
    chat_trim_old_msgs();
    chat_update_empty_hint();
    if (s_view_mode == ViewMode::Chat) {
        chat_scroll_to_latest(false);
    } else {
        ShowEmotionCaption(filtered);
    }
    std::free(filtered);
}

void ChatScreen::ClearMessages()
{
    chat_clear_msg_list();
    ClearEmotionCaption();
    chat_update_empty_hint();
}

void on_set_emotion_async(void *user_data)
{
    auto *name = static_cast<char *>(user_data);
    if (name == nullptr) {
        return;
    }
    if (ChatScreen::IsActive() && s_view_mode == ViewMode::Emotion) {
        ApplyEmotionSrc(name);
    }
    free(name);
}

void ChatScreen::SetEmotion(const char *emotion)
{
    CopyEmotionName(s_current_emotion, sizeof(s_current_emotion), emotion);

    /* Never touch LVGL labels from Application/MQTT threads — that races
     * the render thread and has caused solid-blue wake frames. */
    if (!IsActive() || s_view_mode != ViewMode::Emotion) {
        return;
    }

    const size_t n = std::strlen(s_current_emotion) + 1;
    char *copy = static_cast<char *>(malloc(n));
    if (copy == nullptr) {
        return;
    }
    std::memcpy(copy, s_current_emotion, n);
    if (lv_async_call(on_set_emotion_async, copy) != LV_RESULT_OK) {
        free(copy);
    }
}
