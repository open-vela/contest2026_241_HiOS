/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * System chrome glue: bottom HyperOS gesture pill + top pull zone.
 *
 * Gestures on the pill (mutually exclusive):
 *   - Long press in place (≥550ms, little movement) → AI voice assistant
 *   - Swipe up → recents
 *   - Swipe horizontally → previous / next recent app
 */

#include "system_chrome.h"

#include "control_center.h"
#include "recents_overlay.h"

#include "application.h"
#include "device_state.h"
#include "esp_log_shim.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "idle_power_policy/idle_power_policy.h"
#include "standby_screen/standby_screen.h"
#include "screen_util.h"
#include "chat_screen/chat_screen.h"
#include "digital_people_screen/digital_people_screen.h"
#include "camera_screen/camera_screen.h"
#include "mcp_server.h"

LV_FONT_DECLARE(font_chat_cjk_30);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <atomic>

namespace {

constexpr const char *TAG = "SystemChrome";
constexpr int kPanel = 720;
constexpr int kPillW = 160;
constexpr int kPillH = 6;
constexpr int kHitW = 220;
constexpr int kHitH = 40;
constexpr int kTopPullH = 32;
constexpr uint32_t kHoldRecentsMs = 420;
constexpr uint32_t kLongPressVoiceMs = 780;
constexpr int kSwipeUpMin = 56;
constexpr int kVoiceCancelDy = 26; /* any committed upward move kills long-press */
constexpr int kStillMax = 16; /* max movement for "long press in place" */
constexpr uint32_t kRainbowMs = 4800;

struct ChromeState {
    lv_obj_t *scr = nullptr;
    lv_obj_t *pill_hit = nullptr;
    lv_obj_t *pill = nullptr;
    lv_obj_t *top_pull = nullptr;
    bool is_home = false;
    bool tracking = false;
    bool from_pill = false;
    bool from_top = false;
    bool swiping_up = false;
    bool moved = false;
    int horizontal_direction = 0;
    bool hold_fired = false;
    bool voice_fired = false;
    int16_t start_x = 0;
    int16_t start_y = 0;
    uint32_t press_tick = 0;
    uint32_t swipe_tick = 0;
    lv_timer_t *hold_timer = nullptr;
};

ChromeState s_chrome;
lv_obj_t *s_standby_pill_hit=nullptr;
lv_indev_t *s_observed_input = nullptr;
lv_indev_t *s_global_contact = nullptr;
lv_obj_t *s_global_target = nullptr;
lv_timer_t *s_global_pointer_timer = nullptr;
bool s_global_top = false;

lv_indev_t *GestureInput(lv_event_t *e)
{
    if (s_observed_input) return s_observed_input;
    auto *input = e ? lv_event_get_indev(e) : nullptr;
    return input ? input : lv_indev_active();
}

void KillChromeBorder(lv_obj_t *obj)
{
    if (obj == nullptr) return;
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
}

void CancelHoldTimer()
{
    if (s_chrome.hold_timer != nullptr) {
        lv_timer_delete(s_chrome.hold_timer);
        s_chrome.hold_timer = nullptr;
    }
}

void OnHoldTimer(lv_timer_t *timer)
{
    s_chrome.hold_timer = nullptr;
    lv_timer_delete(timer);
    if (!s_chrome.tracking || !s_chrome.from_pill || s_chrome.hold_fired ||
        s_chrome.voice_fired || !s_chrome.swiping_up) {
        return;
    }
    s_chrome.hold_fired = true;
    ESP_LOGI(TAG, "gesture: swipe-up hold → recents");
    SystemChrome_ShowRecents();
}

/* Four narrow, non-interactive LVGL overlays. Never invalidate a full-screen
 * transparent object or write into the scanout buffer for this effect. */
lv_obj_t *s_rb_edges[4] = {};
lv_timer_t *s_rb_timer = nullptr;
uint32_t s_rb_start = 0;
constexpr int kGlowWidth = 32;

void StopRainbow()
{
    if (s_rb_timer) { lv_timer_delete(s_rb_timer); s_rb_timer = nullptr; }
    for (auto &edge : s_rb_edges) {
        if (edge) lv_obj_delete(edge);
        edge = nullptr;
    }
}

lv_color_t GlowColor(int position)
{
    static const uint32_t colors[] = {
        0x57CEFF, 0x677CFF, 0xAC89FF, 0xD5AEF9, 0x80B9FF, 0x57CEFF
    };
    position = (position % 1280 + 1280) % 1280;
    const uint32_t a = colors[position / 256], b = colors[position / 256 + 1];
    const int f = position % 256;
    return lv_color_make((((a >> 16) & 255) * (256-f) + ((b >> 16) & 255) * f) / 256,
                         (((a >> 8) & 255) * (256-f) + ((b >> 8) & 255) * f) / 256,
                         ((a & 255) * (256-f) + (b & 255) * f) / 256);
}

void OnRainbowDraw(lv_event_t *e)
{
    const int edge = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const uint32_t age = lv_tick_elaps(s_rb_start);
    int fade = age < 450 ? age * 255 / 450 : 255;
    if (age > kRainbowMs - 800) fade = (kRainbowMs - std::min(age, kRainbowMs)) * 255 / 800;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t bounds;
    lv_obj_get_coords(static_cast<lv_obj_t *>(lv_event_get_target(e)), &bounds);
    const bool horizontal = edge < 2;
    const int length = horizontal ? lv_area_get_width(&bounds) : lv_area_get_height(&bounds);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.border_width = 0;
    /* Seven continuous gradient strips per edge: a broad inward falloff,
     * with 28 draw tasks total instead of hundreds of rectangular segments. */
    const int widths[] = {32, 26, 20, 14, 9, 5, 2};
    const int opacity[] = {8, 12, 18, 28, 42, 60, 100};
    const int begin = edge == 0 ? 0 : edge == 3 ? 720 + kGlowWidth :
                      edge == 1 ? 2160 : 2880 - kGlowWidth;
    const int direction = edge == 0 || edge == 3 ? 1 : -1;
    d.bg_grad.dir = horizontal ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[1].frac = 255;
    d.bg_grad.stops[0].opa = LV_OPA_COVER;
    d.bg_grad.stops[1].opa = LV_OPA_COVER;
    d.bg_grad.stops[0].color = GlowColor(begin * 1280 / 2880 - static_cast<int>(age / 12));
    d.bg_grad.stops[1].color = GlowColor((begin + direction * length) * 1280 / 2880 - static_cast<int>(age / 12));
    d.bg_color = d.bg_grad.stops[0].color;
    for (int band = 0; band < 7; ++band) {
        d.bg_opa = opacity[band] * fade / 255;
        lv_area_t area = bounds;
        if (horizontal) {
            if (edge == 0) area.y2 = area.y1 + widths[band] - 1;
            else area.y1 = area.y2 - widths[band] + 1;
        } else {
            if (edge == 2) area.x2 = area.x1 + widths[band] - 1;
            else area.x1 = area.x2 - widths[band] + 1;
        }
        lv_draw_rect(layer, &d, &area);
    }
}

void StartRainbow()
{
    StopRainbow();
    s_rb_start = lv_tick_get();
    for (int i = 0; i < 4; ++i) {
        auto *obj = lv_obj_create(lv_layer_top());
        s_rb_edges[i] = obj;
        lv_obj_remove_style_all(obj);
        lv_obj_set_pos(obj, i == 3 ? 720-kGlowWidth : 0,
                       i == 1 ? 720-kGlowWidth : i >= 2 ? kGlowWidth : 0);
        lv_obj_set_size(obj, i < 2 ? 720 : kGlowWidth,
                       i < 2 ? kGlowWidth : 720-2*kGlowWidth);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(obj, OnRainbowDraw, LV_EVENT_DRAW_MAIN,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
    s_rb_timer = lv_timer_create([](lv_timer_t *) {
        if (StandbyScreen::IsVisible() || lv_tick_elaps(s_rb_start) >= kRainbowMs) {
            StopRainbow(); return;
        }
        for (auto *edge : s_rb_edges) if (edge) lv_obj_invalidate(edge);
    }, 50, nullptr);
}

lv_obj_t *s_float = nullptr;
std::atomic<bool> s_float_visible{false};
unsigned s_float_generation=0; // LVGL thread only, retires delayed popup actions.
bool s_float_start_session = false;
bool s_vision_reply_waiting = false; // LVGL thread only, cleared by the actual reply.
uint32_t s_vision_reply_since = 0;
lv_obj_t *s_float_scroll = nullptr;
lv_obj_t *s_float_body = nullptr;
lv_obj_t *s_float_status = nullptr;
lv_obj_t *s_float_chip = nullptr;
lv_timer_t *s_float_scroll_timer = nullptr;

/* Keep the transcript short enough that a wrap+layout stays inside one
 * DSI frame. A 4K CJK label on the LVGL thread underran the panel (solid
 * blue) after a few turns, and scroll_to_view then pinned the thumb at
 * the top because the label was taller than the viewport. */
constexpr size_t kFloatTextMax = 1536;

void CancelFloatScrollTimer()
{
    if (s_float_scroll_timer != nullptr) {
        lv_timer_delete(s_float_scroll_timer);
        s_float_scroll_timer = nullptr;
    }
}

/* Thumb sits in the right padding, outside the label. Once the transcript
 * overflows, LVGL stops marking the scrollbar dirty, so the previous thumb
 * stays in the framebuffer while the text moves. Redraw the whole track. */
void InvalidateFloatScrollbar()
{
    if (s_float_scroll == nullptr)
        return;
    lv_area_t area;
    lv_obj_get_coords(s_float_scroll, &area);
    constexpr int32_t kTrack = 16;
    if (lv_area_get_width(&area) > kTrack)
        area.x1 = area.x2 - (kTrack - 1);
    lv_obj_invalidate_area(s_float_scroll, &area);
}

void OnFloatScrollChanged(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_float_scroll)
        return;
    InvalidateFloatScrollbar();
}

void ScrollFloatBodyToBottom()
{
    if (s_float_scroll == nullptr || s_float_body == nullptr)
        return;
    /* Layout after the current flush, then jump to the real bottom.
     * scroll_to_view() on an oversized child shows the top, so the thumb
     * stayed put while new lines grew underneath. */
    lv_obj_update_layout(s_float_scroll);
    lv_obj_scroll_to_y(s_float_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    InvalidateFloatScrollbar();
}

void ScheduleFloatScrollToBottom()
{
    if (s_float_scroll == nullptr)
        return;
    if (s_float_scroll_timer != nullptr)
        return;
    /* repeat_count 1 deletes the timer after the callback. Do not
     * lv_timer_delete it here — that double-free corrupts the LVGL heap
     * and the next frame comes up solid blue. */
    s_float_scroll_timer = lv_timer_create(
        [](lv_timer_t *timer) {
            if (s_float_scroll_timer == timer)
                s_float_scroll_timer = nullptr;
            ScrollFloatBodyToBottom();
        },
        32, nullptr);
    if (s_float_scroll_timer != nullptr)
        lv_timer_set_repeat_count(s_float_scroll_timer, 1);
}

void FilterFloatTextInPlace(char *s)
{
    if (s == nullptr) return;
    const lv_font_t *font = &font_chat_cjk_30;
    unsigned char *p = reinterpret_cast<unsigned char *>(s);
    unsigned char *w = p;
    while (*p) {
        uint32_t cp = 0;
        int len = 0;
        if (*p < 0x80) {
            cp = *p;
            len = 1;
        } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            len = 2;
        } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) |
                 ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            len = 3;
        } else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) |
                 ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            len = 4;
        } else {
            ++p;
            continue;
        }
        bool keep = (cp == '\n' || cp == '\r' || cp == ' ');
        if (!keep) {
            lv_font_glyph_dsc_t gdsc;
            std::memset(&gdsc, 0, sizeof(gdsc));
            keep = lv_font_get_glyph_dsc(font, &gdsc, cp, 0);
            if (!keep || gdsc.is_placeholder)
                keep = false;
            else if (gdsc.box_w == 0 && cp > 0x20)
                keep = false;
        }
        if (keep) {
            for (int i = 0; i < len; ++i)
                *w++ = *p++;
        } else {
            p += len;
        }
    }
    *w = '\0';
}

void ApplyFloatAiState()
{
    if (s_float_status == nullptr || s_float_chip == nullptr)
        return;
    const DeviceState st = Application::GetInstance().GetDeviceState();
    const char *text = "待唤醒";
    uint32_t color = 0x94A3B8;
    if(!CameraScreen::IsLookAndSeeMode()) s_vision_reply_waiting=false;
    const bool recognizing=CameraScreen::IsLookAndSeeMode() &&
        (s_vision_reply_waiting || McpServer::GetInstance().IsVisionAnalysisPending());
    if (recognizing) {
        text = "识别中";
        color = 0xFBBF24;
    } else if (st == kDeviceStateListening) {
        text = "聆听中";
        color = 0x34D399;
    } else if (st == kDeviceStateSpeaking) {
        text = "讲话中";
        color = 0x60A5FA;
    } else if (st == kDeviceStateConnecting) {
        text = "连接中";
        color = 0xFBBF24;
    } else {
        text = "待唤醒";
        color = 0x94A3B8;
    }
    lv_label_set_text(s_float_status, I18n::T(text));
    if (recognizing) lv_obj_remove_flag(s_float_chip,LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_add_flag(s_float_chip,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_float_chip, lv_color_hex(color), LV_PART_MAIN);
    if (!StandbyScreen::IsVisible()) IdlePower_NotifyActivity();
}

void HideFloatAi()
{
    s_float_visible.store(false,std::memory_order_release);
    ++s_float_generation;
    s_float_start_session=false;
    s_vision_reply_waiting=false;
    StopRainbow();
    CancelFloatScrollTimer();
    /* Drop the scroll pointer before delete so CHILD_CHANGED during
     * teardown does not invalidate a widget that is going away. */
    s_float_scroll = nullptr;
    s_float_body = nullptr;
    s_float_status = nullptr;
    s_float_chip = nullptr;
    if (s_float != nullptr) {
        lv_obj_delete(s_float);
    }
    s_float = nullptr;
}

void ShowFloatAi(bool start_session)
{
    // Chat already owns an AI interface. Hi 视界 is a separate foreground
    // screen even during the deferred Chat -> camera lifecycle transfer.
    if (SystemChrome_ChatOwnsAssistantUi() && !CameraScreen::IsLookAndSeeMode()) {
        if (s_float != nullptr) HideFloatAi();
        if (start_session) Application::GetInstance().RequestWakeSession();
        return;
    }
    if (s_float != nullptr) {
        ApplyFloatAiState();
        if (start_session) {
            /* RequestWakeSession schedules onto the app thread. Wait a
             * frame so this gesture's refresh is done before EnableVP. */
            lv_timer_t *wake_t = lv_timer_create(
                [](lv_timer_t *timer) {
                    if (s_float != nullptr && static_cast<unsigned>(reinterpret_cast<uintptr_t>(lv_timer_get_user_data(timer)))==s_float_generation)
                        Application::GetInstance().RequestWakeSession();
                },
                650, reinterpret_cast<void *>(static_cast<uintptr_t>(s_float_generation)));
            if (wake_t != nullptr)
                lv_timer_set_repeat_count(wake_t, 1);
        }
        return;
    }

    /* Keep overlay creation and edge animation on separate refreshes. */
    StopRainbow();

    lv_obj_t *card = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 600, 468);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2C3340), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_FLOATING);

    lv_obj_t *accent = lv_obj_create(card);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 8, 36);
    lv_obj_set_pos(accent, 22, 22);
    lv_obj_set_style_bg_color(accent, lv_color_hex(0x5B8CFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(accent, 4, LV_PART_MAIN);
    lv_obj_remove_flag(accent, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Hi 助手");
    lv_obj_set_style_text_font(title, &font_chat_cjk_30, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF4F6FA), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 42, 22);

    lv_obj_t *chip = lv_obj_create(card);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(chip, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(chip, 6, LV_PART_MAIN);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, -22, 20);
    lv_obj_set_style_bg_color(chip, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(chip, 20, LV_PART_MAIN);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        chip,
        [](lv_event_t * /*e*/) {
            Application::GetInstance().RequestWakeSession();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *status = lv_label_create(chip);
    lv_label_set_text(status, I18n::T("待唤醒"));
    lv_obj_set_style_text_font(status, &font_chat_cjk_30, LV_PART_MAIN);
    lv_obj_set_style_text_color(status, lv_color_hex(0x0E1116), LV_PART_MAIN);
    lv_obj_center(status);

    lv_obj_t *rule = lv_obj_create(card);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 556, 1);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_bg_color(rule, lv_color_hex(0x2A3140), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *scroll = lv_obj_create(card);
    lv_obj_remove_style_all(scroll);
    lv_obj_set_size(scroll, 556, 360);
    lv_obj_align(scroll, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_color(scroll, lv_color_hex(0x141820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(scroll, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(scroll, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(scroll, 4, LV_PART_MAIN);
    /* ON, not AUTO: AUTO hides the indicator between gestures, so a
     * content-only grow left the thumb at the previous offset. */
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_width(scroll, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_all(scroll, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll, lv_color_hex(0x5B8CFF), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(scroll, 3, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(scroll, OnFloatScrollChanged, LV_EVENT_SCROLL, nullptr);
    lv_obj_add_event_cb(scroll, OnFloatScrollChanged, LV_EVENT_CHILD_CHANGED,
                        nullptr);

    lv_obj_t *body = lv_label_create(scroll);
    lv_label_set_text(body, "");
    /* 556 - pad_left 8 - pad_right 16. No align: align re-pins the label
     * on every layout and the thumb stops tracking the scroll offset. */
    lv_obj_set_width(body, 532);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body, &font_chat_cjk_30, LV_PART_MAIN);
    lv_obj_set_style_text_color(body, lv_color_hex(0xDCE3EE), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(body, 8, LV_PART_MAIN);
    lv_obj_set_pos(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    s_float = card;
    ++s_float_generation;
    s_float_scroll = scroll;
    s_float_body = body;
    s_float_status = status;
    s_float_chip = chip;
    s_float_visible.store(true,std::memory_order_release);
    screen_kill_theme_chrome(card);
    ApplyFloatAiState();
    IdlePower_NotifyActivity();
    /* Card pixels before any audio. ArmAssistantWakeWord / RequestWakeSession
     * are unchanged; they just must not run on this same refresh. */
    lv_refr_now(nullptr);
    lv_timer_t *glow_t = lv_timer_create([](lv_timer_t *timer) {
        if (s_float != nullptr && static_cast<unsigned>(reinterpret_cast<uintptr_t>(lv_timer_get_user_data(timer)))==s_float_generation && !StandbyScreen::IsVisible()) StartRainbow();
    }, 150, reinterpret_cast<void *>(static_cast<uintptr_t>(s_float_generation)));
    if (glow_t) lv_timer_set_repeat_count(glow_t, 1);
    ESP_LOGI(TAG, "gesture: long-press → floating AI");
    s_float_start_session = start_session;
    lv_timer_t *wake_t = lv_timer_create(
        [](lv_timer_t *timer) {
            if (s_float == nullptr || static_cast<unsigned>(reinterpret_cast<uintptr_t>(lv_timer_get_user_data(timer)))!=s_float_generation)
                return;
            Application::GetInstance().ArmAssistantWakeWord();
            if (s_float_start_session)
                Application::GetInstance().RequestWakeSession();
            s_float_start_session = false;
        },
        650, reinterpret_cast<void *>(static_cast<uintptr_t>(s_float_generation)));
    if (wake_t != nullptr)
        lv_timer_set_repeat_count(wake_t, 1);
}

void InvokeVoiceAssistant()
{
    // A committed long hold toggles only the assistant card, including the
    // card above Hi Vision. It never launches or navigates to the camera.
    if (s_float != nullptr) {
        HideFloatAi();
        Application::GetInstance().EndFloatAiSession();
        return;
    }
    ShowFloatAi(true);
}

extern "C" bool SystemChrome_FloatAiOpen(void)
{
    return s_float_visible.load(std::memory_order_acquire);
}

extern "C" void SystemChrome_FloatAiSyncState(void)
{
    lv_lock();
    lv_async_call([](void *) { ApplyFloatAiState(); }, nullptr);
    lv_unlock();
}

extern "C" void SystemChrome_FloatAiAppend(const char *role, const char *text)
{
    if (s_float_body == nullptr || text == nullptr || text[0] == '\0')
        return;
    if(CameraScreen::IsLookAndSeeMode()) {
        if(role && std::strcmp(role,"user")==0) {
            // Start feedback at the recognized question, before cloud tool
            // dispatch, and retain it through the final cloud text response.
            s_vision_reply_waiting=true;s_vision_reply_since=lv_tick_get();
            auto *timer=lv_timer_create([](lv_timer_t *) {
                if(s_vision_reply_waiting && lv_tick_elaps(s_vision_reply_since)>=12000) {
                    s_vision_reply_waiting=false;ApplyFloatAiState();
                }
            },12000,nullptr);
            if(timer)lv_timer_set_repeat_count(timer,1);
        } else if(role && (std::strcmp(role,"assistant")==0 || std::strcmp(role,"system")==0))
            s_vision_reply_waiting=false;
    }
    const size_t n = std::strlen(text);
    char *filtered = static_cast<char *>(std::malloc(n + 1));
    if (filtered == nullptr)
        return;
    std::memcpy(filtered, text, n + 1);
    FilterFloatTextInPlace(filtered);
    if (filtered[0] == '\0') {
        std::free(filtered);
        return;
    }

    const char *cur = lv_label_get_text(s_float_body);
    std::string next;
    if (cur != nullptr && cur[0] != '\0')
        next = cur;
    if (!next.empty())
        next += "\n";
    next += (role != nullptr && std::strcmp(role, "user") == 0) ? "你: " : "AI: ";
    next += filtered;
    std::free(filtered);
    if (next.size() > kFloatTextMax) {
        size_t cut = next.size() - kFloatTextMax;
        const auto *bytes = reinterpret_cast<const unsigned char *>(next.data());
        while (cut < next.size() && (bytes[cut] & 0xC0) == 0x80)
            ++cut;
        next.erase(0, cut);
    }
    lv_label_set_text(s_float_body, next.c_str());
    /* Defer the layout+scroll so this callback can return and the panel
     * can flip before the wrap of the whole transcript. */
    ScheduleFloatScrollToBottom();
    Application::GetInstance().NotifyFloatAiDialogActivity();
    ApplyFloatAiState();
}

void GoHomeOrDismiss()
{
    if (s_float != nullptr) {
        HideFloatAi();
        Application::GetInstance().EndFloatAiSession();
    }
    if (ControlCenter_IsOpen()) {
        ControlCenter_Hide();
        return;
    }
    if (Recents_IsOpen()) {
        Recents_Hide();
        return;
    }
    if (!s_chrome.is_home) {
        HomeScreen::SwitchToHome();
    }
}

void OnPillPressed(lv_event_t *e)
{
    lv_indev_t *indev = GestureInput(e);
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    CancelHoldTimer();
    s_chrome.tracking = true;
    s_chrome.from_pill = true;
    s_chrome.from_top = false;
    s_chrome.swiping_up = false;
    s_chrome.moved = false;
    s_chrome.horizontal_direction = 0;
    s_chrome.hold_fired = false;
    s_chrome.voice_fired = false;
    s_chrome.start_x = static_cast<int16_t>(p.x);
    s_chrome.start_y = static_cast<int16_t>(p.y);
    s_chrome.press_tick = lv_tick_get();
    s_chrome.swipe_tick = 0;
    // Visual AI feedback starts only after a stationary long press wins.
    /* Do NOT start hold-recents timer here — only after upward swipe. */
}

void OnPillPressing(lv_event_t *e)
{
    if (!s_chrome.tracking || !s_chrome.from_pill) return;
    if (s_chrome.hold_fired || s_chrome.voice_fired) return;

    lv_indev_t *indev = GestureInput(e);
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int dy = s_chrome.start_y - p.y; /* up is positive */
    const int dx = std::abs(p.x - s_chrome.start_x);
    const int ady = std::abs(dy);

    // Movement cancels long-press permanently, even if the finger returns.
    if (dx >= kStillMax || ady >= kStillMax) s_chrome.moved = true;
    // Lock one axis for the complete contact; diagonal motion cannot change it.
    if (!s_chrome.swiping_up && s_chrome.horizontal_direction == 0) {
        if (dx >= kVoiceCancelDy && dx > ady * 2)
            s_chrome.horizontal_direction = p.x > s_chrome.start_x ? 1 : -1;
        else if (dy >= kVoiceCancelDy && dy > dx * 2)
            s_chrome.swiping_up = true;
    }
    if (s_chrome.moved) return;
    if (lv_tick_elaps(s_chrome.press_tick) >= kLongPressVoiceMs) {
        s_chrome.voice_fired = true;
        CancelHoldTimer();
        // Consume the original contact before creating a new overlay.
        // Its release must not click a newly exposed app/vision button.
        lv_indev_reset(indev, nullptr);
        lv_indev_wait_release(indev);
        InvokeVoiceAssistant();
    }
}

void OnPillCancelled(lv_event_t * /*e*/)
{
    CancelHoldTimer();
    s_chrome.tracking = false;
    s_chrome.from_pill = false;
}

void OnPillReleased(lv_event_t *e)
{
    if (!s_chrome.tracking || !s_chrome.from_pill) return;
    // Also classify the release sample (fast swipes may have few PRESSING events).
    OnPillPressing(e);
    auto *indev = GestureInput(e);
    lv_point_t p = {s_chrome.start_x, s_chrome.start_y};
    if (indev) lv_indev_get_point(indev, &p);
    const int dx = p.x - s_chrome.start_x;
    const int dy = s_chrome.start_y - p.y;
    const bool consumed = s_chrome.voice_fired || s_chrome.hold_fired;
    const bool up = s_chrome.swiping_up;
    const int horizontal = s_chrome.horizontal_direction;
    const bool home = s_chrome.is_home;
    OnPillCancelled(nullptr);
    if (consumed) return;
    if (up && dy >= kSwipeUpMin && dy > std::abs(dx) * 2) {
        ESP_LOGI(TAG, "gesture: swipe up -> recents");
        SystemChrome_ShowRecents();
    } else if (horizontal && dx * horizontal >= 64 && std::abs(dx) > std::abs(dy) * 2) {
        ESP_LOGI(TAG, "gesture: horizontal -> app direction=%d", horizontal);
        if (s_float) { HideFloatAi(); Application::GetInstance().EndFloatAiSession(); }
        if (ControlCenter_IsOpen()) ControlCenter_Hide();
        Recents_SwitchAdjacent(horizontal, home);
    }
    // Taps, cancelled holds, and ambiguous diagonals do not launch AI.
}

void OnTopPressed(lv_event_t *e)
{
    lv_indev_t *indev = GestureInput(e);
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_chrome.tracking = true;
    s_chrome.from_top = true;
    s_chrome.from_pill = false;
    s_chrome.start_x = static_cast<int16_t>(p.x);
    s_chrome.start_y = static_cast<int16_t>(p.y);
    s_chrome.press_tick = lv_tick_get();
}

void TryOpenControlCenterFromTop(int dy, int dx)
{
    if (dy < 28 || dy <= dx)
        return;
    s_chrome.tracking = false;
    s_chrome.from_top = false;
    ESP_LOGI(TAG, "gesture: pull down → control center");
    SystemChrome_ShowControlCenter();
}

void OnTopPressing(lv_event_t *e)
{
    if (!s_chrome.tracking || !s_chrome.from_top)
        return;
    lv_indev_t *indev = GestureInput(e);
    if (indev == nullptr)
        return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    TryOpenControlCenterFromTop(p.y - s_chrome.start_y,
                                std::abs(p.x - s_chrome.start_x));
}

void OnTopReleased(lv_event_t *e)
{
    if (!s_chrome.tracking || !s_chrome.from_top) {
        s_chrome.tracking = false;
        return;
    }
    lv_indev_t *indev = GestureInput(e);
    int dy = 0;
    int dx = 0;
    if (indev != nullptr) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        dy = p.y - s_chrome.start_y;
        dx = std::abs(p.x - s_chrome.start_x);
    }
    s_chrome.tracking = false;
    s_chrome.from_top = false;

    if (dy >= 28 && dy > dx) {
        ESP_LOGI(TAG, "gesture: pull down → control center");
        SystemChrome_ShowControlCenter();
    }
}

void OnScreenDeleted(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_current_target_obj(e);
    if (s_chrome.scr == scr) {
        CancelHoldTimer();
        HideFloatAi();
        ControlCenter_Hide();
        Recents_Hide();
        s_chrome = ChromeState{};
    }
}

void EnsurePill(lv_obj_t *scr)
{
    if (s_chrome.pill_hit != nullptr &&
        lv_obj_get_parent(s_chrome.pill_hit) == scr) {
        lv_obj_move_foreground(s_chrome.pill_hit);
        if (s_chrome.top_pull != nullptr) {
            lv_obj_set_size(s_chrome.top_pull, kPanel, kTopPullH);
            lv_obj_align(s_chrome.top_pull, LV_ALIGN_TOP_MID, 0, 0);
            lv_obj_move_foreground(s_chrome.top_pull);
        }
        return;
    }

    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, kPanel, kTopPullH);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, LV_PART_MAIN);
    KillChromeBorder(top);
    lv_obj_add_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(top, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(top, OnTopPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(top, OnTopPressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(top, OnTopReleased, LV_EVENT_RELEASED, nullptr);
    screen_swipe_back_ignore(top, true);
    s_chrome.top_pull = top;

    lv_obj_t *hit = lv_obj_create(scr);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, kHitW, kHitH);
    lv_obj_align(hit, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, LV_PART_MAIN);
    KillChromeBorder(hit);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit, OnPillPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(hit, OnPillPressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(hit, OnPillReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(hit, OnPillCancelled, LV_EVENT_PRESS_LOST, nullptr);
    screen_swipe_back_ignore(hit, true);
    s_chrome.pill_hit = hit;

    lv_obj_t *pill = lv_obj_create(hit);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, kPillW, kPillH);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    KillChromeBorder(pill);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    s_chrome.pill = pill;
}

}  // namespace

bool SystemChrome_ChatOwnsAssistantUi(void)
{
    // An underlying cached Chat page does not own a visible clock, shade
    // or recents overlay. Those surfaces still need the global AI popup.
    return (ChatScreen::IsForeground() || DigitalPeopleScreen::IsForeground()) && !StandbyScreen::IsVisible() &&
        !ControlCenter_IsOpen() && !Recents_IsOpen();
}

bool SystemChrome_ObservePointer(lv_indev_t *input, lv_event_code_t code,
                                lv_obj_t *target)
{
    if(HomeScreen::IsEditing()){OnPillCancelled(nullptr);s_global_contact=nullptr;return false;}
    lv_point_t p{};
    lv_indev_get_point(input, &p);
    if (code == LV_EVENT_PRESSED) {
        if (s_global_contact == input) { s_global_target=target; return false; }
        s_global_contact = nullptr;
        // Existing chrome owns its normal input. App buttons/overlays keep
        // their taps until a genuine edge swipe or stationary hold commits.
        for (auto *obj = target; obj; obj = lv_obj_get_parent(obj))
            if (obj == s_chrome.pill_hit || obj == s_chrome.top_pull || obj==s_standby_pill_hit) return false;
        const bool top = p.y >= 0 && p.y < kTopPullH;
        const bool pill = p.y >= kPanel-kHitH &&
            std::abs(p.x-kPanel/2) <= kHitW/2;
        if (!top && !pill) return false;
        s_global_contact = input;
        s_global_target = target;
        s_global_top = top;
        s_observed_input = input;
        if (top) OnTopPressed(nullptr); else OnPillPressed(nullptr);
        s_observed_input = nullptr;
        return false;
    }
    if (s_global_contact != input) return false;
    if (code == LV_EVENT_DELETE) {
        OnPillCancelled(nullptr);
        s_global_contact = nullptr;
        return false;
    }
    if (code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) return false;
    s_observed_input = input;
    const bool commit_top = s_global_top && s_chrome.from_top &&
        p.y-s_chrome.start_y >= 28 && p.y-s_chrome.start_y > std::abs(p.x-s_chrome.start_x);
    const bool commit_pill = !s_global_top && code == LV_EVENT_RELEASED &&
        (s_chrome.swiping_up || s_chrome.horizontal_direction);
    if (s_global_top) {
        if (code == LV_EVENT_RELEASED) OnTopReleased(nullptr); else OnTopPressing(nullptr);
    } else {
        if (code == LV_EVENT_RELEASED) OnPillReleased(nullptr); else OnPillPressing(nullptr);
    }
    const bool consumed = commit_top || commit_pill || s_chrome.voice_fired;
    s_observed_input = nullptr;
    if (code == LV_EVENT_RELEASED || consumed) s_global_contact = nullptr;
    if (consumed) {
        if (target && lv_obj_is_valid(target)) {
            lv_obj_remove_state(target, LV_STATE_PRESSED);
            lv_obj_send_event(target, LV_EVENT_PRESS_LOST, input);
        }
        lv_indev_reset(input, nullptr);
        lv_indev_wait_release(input);
        OnPillCancelled(nullptr);
    }
    return consumed;
}

void SystemChrome_Attach(lv_obj_t *scr, bool is_home)
{
    if (scr == nullptr) return;
    if (!s_global_pointer_timer) {
        // LVGL emits PRESSING only to the target widget, not to the input
        // device observer. Sample the existing real contact on the LVGL
        // thread so overlay edges commit while held, before release.
        s_global_pointer_timer=lv_timer_create([](lv_timer_t *) {
            if (!s_global_contact) return;
            if (lv_indev_get_state(s_global_contact)!=LV_INDEV_STATE_PRESSED) {
                s_global_contact=nullptr;
                OnPillCancelled(nullptr);
                return;
            }
            SystemChrome_ObservePointer(s_global_contact,LV_EVENT_PRESSING,s_global_target);
        },30,nullptr);
    }
    if (s_chrome.scr == scr)
        s_chrome.is_home = s_chrome.is_home || is_home;
    else {
        s_chrome.scr = scr;
        s_chrome.is_home = is_home;
    }
    EnsurePill(scr);
    lv_obj_add_event_cb(scr, OnScreenDeleted, LV_EVENT_DELETE, nullptr);
    if (s_chrome.top_pull != nullptr)
        lv_obj_move_foreground(s_chrome.top_pull);
    if (s_chrome.pill_hit != nullptr)
        lv_obj_move_foreground(s_chrome.pill_hit);
    ESP_LOGI(TAG, "attach chrome is_home=%d", s_chrome.is_home ? 1 : 0);
}

// The clock is an overlay: preserve the underlying app's chrome and create
// only this clock-owned hit area. Deleting it must not end the AI dialogue.
void SystemChrome_AttachStandbyPill(lv_obj_t *clock){
    if(!clock)return;
    if(s_standby_pill_hit&&lv_obj_is_valid(s_standby_pill_hit))lv_obj_delete(s_standby_pill_hit);
    auto *hit=lv_obj_create(clock);s_standby_pill_hit=hit;
    lv_obj_remove_style_all(hit);lv_obj_set_size(hit,kHitW,kHitH);
    lv_obj_align(hit,LV_ALIGN_BOTTOM_MID,0,0);lv_obj_set_style_bg_opa(hit,LV_OPA_TRANSP,0);
    lv_obj_add_flag(hit,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_flag(hit,LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_flag(hit,LV_OBJ_FLAG_FLOATING);lv_obj_remove_flag(hit,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit,OnPillPressed,LV_EVENT_PRESSED,nullptr);
    lv_obj_add_event_cb(hit,OnPillPressing,LV_EVENT_PRESSING,nullptr);
    lv_obj_add_event_cb(hit,[](lv_event_t*e){
        if(s_chrome.swiping_up||s_chrome.horizontal_direction)StandbyScreen::ReturnHome();
        OnPillReleased(e);
    },LV_EVENT_RELEASED,nullptr);
    lv_obj_add_event_cb(hit,OnPillCancelled,LV_EVENT_PRESS_LOST,nullptr);
    lv_obj_add_event_cb(hit,[](lv_event_t*e){if(lv_event_get_target_obj(e)==s_standby_pill_hit){s_standby_pill_hit=nullptr;OnPillCancelled(nullptr);}},LV_EVENT_DELETE,nullptr);
    screen_swipe_back_ignore(hit,true);
    auto*pill=lv_obj_create(hit);lv_obj_remove_style_all(pill);lv_obj_set_size(pill,kPillW,kPillH);
    lv_obj_set_style_bg_color(pill,lv_color_white(),0);lv_obj_set_style_bg_opa(pill,LV_OPA_80,0);
    lv_obj_set_style_radius(pill,LV_RADIUS_CIRCLE,0);lv_obj_align(pill,LV_ALIGN_BOTTOM_MID,0,-10);
    lv_obj_remove_flag(pill,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(pill,LV_OBJ_FLAG_SCROLLABLE);
}

void SystemChrome_EnsureFloatAi(bool start_session)
{
    // Both the Application thread and camera lifecycle call this entry.
    // lv_async_call allocates a timer and must share the renderer's lock.
    lv_lock();
    lv_async_call(
        [](void *p) { ShowFloatAi(p != nullptr); },
        start_session ? reinterpret_cast<void *>(1) : nullptr);
    lv_unlock();
}

void SystemChrome_CloseFloatAi(void)
{
    lv_lock();
    lv_async_call([](void *) { HideFloatAi(); }, nullptr);
    lv_unlock();
}

void SystemChrome_NotifyAppOpened(const char *app_id, const char *app_name,
                                  const char *icon_path)
{
    Recents_NotifyOpened(app_id, app_name, icon_path);
}

void SystemChrome_ShowControlCenter(void)
{
    if (Recents_IsOpen()) Recents_Hide();
    ControlCenter_Show(lv_layer_sys());
}

void SystemChrome_HideControlCenter(void)
{
    ControlCenter_Hide();
}

bool SystemChrome_IsControlCenterOpen(void)
{
    return ControlCenter_IsOpen();
}

void SystemChrome_ShowRecents(void)
{
    if (s_float) { HideFloatAi(); Application::GetInstance().EndFloatAiSession(); }
    if (ControlCenter_IsOpen()) ControlCenter_Hide();
    Recents_Show(lv_screen_active());
}

void SystemChrome_HideRecents(void)
{
    Recents_Hide();
}

bool SystemChrome_IsRecentsOpen(void)
{
    return Recents_IsOpen();
}

void SystemChrome_DismissOverlays(void)
{
    ControlCenter_Hide();
    Recents_Hide();
}
