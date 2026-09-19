/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * CallScreen — ported from MetalioClaw4
 * main/display/screen/call_screen/call_screen.cc.
 *
 * The original drives a Neoway NT26 4G modem via AT commands
 * (AT+CPIN? / ATD<number> / ATH) and toggles IOExpander::PA_SWITCH to
 * route audio between WIFI and 4G call paths. openvela has neither
 * SimpleUart nor IOExpander yet, so:
 *
 *   - The AT-command dispatch is replaced by a CallDriver interface
 *     (declared in the header). The default driver is an inline stub
 *     that immediately posts kDialOk / kHangupDone through
 *     lv_async_call() so the UI flow is fully exercised.
 *   - IOExpander::PA_SWITCH toggling collapses to ESP_LOGI calls in
 *     LifecycleCallback().
 *
 * Everything else is ported 1:1 from the ESP-IDF source:
 *
 *   - Layout constants (header / number area / keypad geometry /
 *     backspace extended click area / action button diameter).
 *   - iOS-style dark palette (key bg 0x303030, call green 0x34C759,
 *     hangup red 0xFF3B30).
 *   - 3x4 keypad table with sub-letters (ABC, DEF, ..., WXYZ, +).
 *   - Number formatter (11-digit mobile -> "138 1234 5678", otherwise
 *     groups of 4).
 *   - CallState epoch guard so stale lv_async_call results don't
 *     overwrite a fresh idle UI.
 *   - Long-press on backspace clears the whole number (iPhone parity).
 *   - No right-swipe gesture (intentional in the source — too easy to
 *     mis-detect horizontal drags on the keypad); back navigation is
 *     the top-left button only.
 *   - Hanging up on swipe-back / unload if a call was in progress.
 */

#include "call_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "esp_err_shim.h"
#include "freertos_shim.h"
#include "home_screen/home_screen.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <memory>

#include "lvgl.h"

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_number_50_4);

// ---------------------------------------------------------------------------
// 720x720 layout (preserved verbatim from the original)
//
//  +-----------------------------------------------+ y=0
//  |  [<-]  电话                                    |  header (h=88)
//  +-----------------------------------------------+ y=88
//  |              138 1234 5678          [backsp]  |  number display (h=100)
//  |              (status line)                     |
//  +-----------------------------------------------+ y=188
//  |   [1]   [2 ABC]   [3 DEF]                      |
//  |   [4 GHI] [5 JKL] [6 MNO]                      |  digit grid 3x3
//  |   [7 PQRS][8 TUV] [9 WXYZ]                     |  + 0 centered
//  |            [0 +]              [ call / hangup ]|  call button: bottom-right
//  +-----------------------------------------------+ y=720
// ---------------------------------------------------------------------------

namespace {

constexpr const char* TAG = "CallScreen";

/* number_50 only has 0-9 and '%'; phone formatting inserts spaces, so
 * fall back to puhui_30 for the missing glyphs. */
lv_font_t s_dial_number_font;
bool s_dial_number_font_ready = false;

const lv_font_t *DialNumberFont()
{
    if (!s_dial_number_font_ready) {
        s_dial_number_font = font_puhui_number_50_4;
        s_dial_number_font.fallback = &font_puhui_30_4;
        s_dial_number_font_ready = true;
    }
    return &s_dial_number_font;
}

constexpr int kPanelSize    = 720;
constexpr int kPad          = 16;
constexpr int kBackBtnSize  = 72;   // 与其他页面一致的返回按钮点击区域
constexpr int kHeaderH      = 88;   // 容纳 72px 返回按钮 + 上下留白
constexpr int kNumberAreaH  = 100;  // 容纳 50px 号码字 + 状态行
constexpr int kKeypadY      = kHeaderH + kNumberAreaH;
constexpr int kKeypadH      = kPanelSize - kKeypadY;
constexpr int kKeypadBottomPad = 16;  // 数字盘与底边留白（拨打键另计）

// ----- number area sub-layout ----------------------------------------------
// We hand-place the number label, the status line, and the backspace button
// so the backspace's vertical center sits exactly on the number's vertical
// center -- making the trio read as a single horizontal row.
constexpr int kNumberLblTop = kHeaderH + 6;
constexpr int kNumberLblH   = 56;                  // hugs the 50px number font
constexpr int kStatusLblTop = kNumberLblTop + kNumberLblH + 4;
constexpr int kStatusLblH   = 28;
constexpr int kBackspaceBtnSize  = 80;             // visible diameter
constexpr int kBackspaceClickExt = 16;             // adds 16px to every side
                                                   // -> 112x112 effective tap
constexpr int kBackspaceY   =
    kNumberLblTop + (kNumberLblH - kBackspaceBtnSize) / 2;  // 64

// 数字区：3 列 × 4 行（末行仅 0 居中）；拨打键贴屏幕右下角放大。
constexpr int kDigitRows    = 4;
constexpr int kKeypadCols   = 3;
constexpr int kKeypadColGap = 48;
constexpr int kKeypadRowGap = 16;
constexpr int kKeyDiameter  = 96;   // circular digit buttons

// Action button (call / hangup) — 屏幕右下角，明显大于数字键。
constexpr int kActionBtnD      = 128;
constexpr int kActionEdgePad   = 28;  // 距右/底边

// ----- iOS-inspired dark phone palette -------------------------------------
constexpr uint32_t kColorBg            = 0x000000;
constexpr uint32_t kColorTextPrimary   = 0xFFFFFF;
constexpr uint32_t kColorTextSecondary = 0x9A9A9A;
constexpr uint32_t kColorHintText      = 0x6E6E70;

constexpr uint32_t kKeyBg          = 0x303030;
constexpr uint32_t kKeyBgPress     = 0x595959;
constexpr uint32_t kKeyText        = 0xFFFFFF;
constexpr uint32_t kKeySubText     = 0xB0B0B0;

constexpr uint32_t kCallBg         = 0x34C759;   // iOS green
constexpr uint32_t kCallBgPress    = 0x66D684;
constexpr uint32_t kHangupBg       = 0xFF3B30;   // iOS red
constexpr uint32_t kHangupBgPress  = 0xFF6F66;

// ----- keypad table ---------------------------------------------------------
struct KeyDef {
    const char* digit;
    const char* sub;     // letters (e.g. "ABC"), may be empty
    int row, col;
};

// 末行仅保留 0（中间列）；拨打键固定在屏幕右下角（见 BuildKeypad）。
const KeyDef kKeys[] = {
    {"1", "",     0, 0}, {"2", "ABC",  0, 1}, {"3", "DEF",  0, 2},
    {"4", "GHI",  1, 0}, {"5", "JKL",  1, 1}, {"6", "MNO",  1, 2},
    {"7", "PQRS", 2, 0}, {"8", "TUV",  2, 1}, {"9", "WXYZ", 2, 2},
    {"0", "+",    3, 1},
};

// ----- dialer state ---------------------------------------------------------
enum class CallState { kIdle, kCalling };

constexpr int  kMaxDigits   = 24;
char           s_number[kMaxDigits + 1];
CallState      s_call_state;

lv_obj_t* s_number_lbl;
lv_obj_t* s_status_lbl;
lv_obj_t* s_backspace_btn;
lv_obj_t* s_action_btn;        // 右下角：拨打 / 挂断
lv_obj_t* s_action_icon;       // image inside action_btn (dial / hangup)

// Tracks whether the call screen is currently mounted. lv_async_call() trampolines
// from the AT-task thread back to the LVGL thread; if the user already swiped back
// we MUST NOT touch the now-deleted UI objects.
bool s_screen_active = false;

// Bumped on every call-state transition. Carried by the AT task; on completion
// we drop the result if the epoch no longer matches (e.g. user swiped back, or
// hung up before ATD returned). This avoids a stale "dial OK" overwriting a
// freshly-idle UI.
uint32_t s_call_epoch = 0;

// ---------------------------------------------------------------------------
// CallDriver — pluggable call-control interface (see header).
// ---------------------------------------------------------------------------

// Default stub driver: posts an immediate kDialOk / kHangupDone result so
// the UI transitions can be tested without a real modem. Runs the post on
// a short FreeRTOS task to preserve the original async semantics (the LVGL
// thread never blocks on the dial path).
class StubCallDriver : public CallScreen::CallDriver
{
public:
    esp_err_t Dial(const std::string & /*number*/, uint32_t epoch) override
    {
        ESP_LOGI(TAG, "stub dial (epoch=%u)", (unsigned)epoch);
        return PostResult(CallScreen::JobKind::kDial,
                          CallScreen::Outcome::kDialOk, epoch);
    }

    esp_err_t Hangup(uint32_t epoch) override
    {
        ESP_LOGI(TAG, "stub hangup (epoch=%u)", (unsigned)epoch);
        return PostResult(CallScreen::JobKind::kHangup,
                          CallScreen::Outcome::kHangupDone, epoch);
    }

private:
    static esp_err_t PostResult(CallScreen::JobKind kind,
                                CallScreen::Outcome outcome, uint32_t epoch)
    {
        auto *res = new CallScreen::CallResult{};
        res->kind     = kind;
        res->outcome  = outcome;
        res->epoch    = epoch;
        // Trampoline into the LVGL thread. lv_async_call is available on
        // NuttX's LVGL port (already used by pwr_key_handler / sd_card).
        lv_async_call(OnCallResultAsync, res);
        return ESP_OK;
    }

    static void OnCallResultAsync(void *user_data)
    {
        std::unique_ptr<CallScreen::CallResult> res(
            static_cast<CallScreen::CallResult *>(user_data));
        if (res == nullptr) return;
        CallScreen::DispatchResult(*res);
    }
};

CallScreen::CallDriver *s_driver = nullptr;
StubCallDriver s_default_driver;

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

// Format a Chinese-style phone number: "13812345678" -> "138 1234 5678".
// For non-11-digit input we just show it as typed (still grouped lightly).
void FormatNumberForDisplay(const char* in, char* out, size_t out_sz) {
    size_t len = std::strlen(in);
    if (len == 0) {
        if (out_sz > 0) out[0] = '\0';
        return;
    }
    // Mobile-style: 3-4-4 grouping when exactly 11 digits.
    if (len == 11) {
        snprintf(out, out_sz, "%.3s %.4s %.4s", in, in + 3, in + 7);
        return;
    }
    // Otherwise insert a space every 4 chars from the left for readability.
    size_t out_len = 0;
    for (size_t i = 0; i < len && out_len + 1 < out_sz; ++i) {
        if (i > 0 && i % 4 == 0 && out_len + 1 < out_sz) {
            out[out_len++] = ' ';
        }
        out[out_len++] = in[i];
    }
    if (out_len < out_sz) out[out_len] = '\0';
}

void RefreshNumberDisplay() {
    char formatted[40];
    FormatNumberForDisplay(s_number, formatted, sizeof(formatted));
    if (formatted[0] == '\0') {
        // Placeholder hint when nothing has been typed.
        lv_label_set_text(s_number_lbl, "");
    } else {
        lv_label_set_text(s_number_lbl, formatted);
    }
    // Backspace only shown when there is something to delete and we are
    // not currently in a "call in progress" state.
    bool show_back = (s_number[0] != '\0' && s_call_state == CallState::kIdle);
    if (show_back) {
        lv_obj_remove_flag(s_backspace_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_backspace_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void RefreshActionButton() {
    if (s_call_state == CallState::kCalling) {
        lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kHangupBg),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kHangupBgPress),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
    } else {
        lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kCallBg),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kCallBgPress),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
    }
}

void RefreshStatus() {
    if (s_call_state == CallState::kCalling) {
        if (s_number[0] != '\0') {
            lv_label_set_text(s_status_lbl, I18n::T("拨号中..."));
        } else {
            lv_label_set_text(s_status_lbl, "");
        }
    } else {
        lv_label_set_text(s_status_lbl, "");
    }
}

void SetStatusText(const char* txt) {
    if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, txt);
    }
}

// ---------------------------------------------------------------------------
// State mutations
// ---------------------------------------------------------------------------

void AppendDigit(const char* d) {
    if (s_call_state == CallState::kCalling) return;
    size_t len = std::strlen(s_number);
    if (len >= kMaxDigits) return;
    s_number[len]     = d[0];
    s_number[len + 1] = '\0';
    RefreshNumberDisplay();
}

void Backspace() {
    if (s_call_state == CallState::kCalling) return;
    size_t len = std::strlen(s_number);
    if (len == 0) return;
    s_number[len - 1] = '\0';
    RefreshNumberDisplay();
}

CallScreen::CallDriver *ActiveDriver() {
    return (s_driver != nullptr) ? s_driver : &s_default_driver;
}

void DispatchDial(const std::string& number) {
    esp_err_t err = ActiveDriver()->Dial(number, s_call_epoch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Dial dispatch failed: %d", (int)err);
        SetStatusText(I18n::T("系统忙"));
    }
}

void DispatchHangup() {
    esp_err_t err = ActiveDriver()->Hangup(s_call_epoch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Hangup dispatch failed: %d", (int)err);
    }
}

void StartCall() {
    if (s_number[0] == '\0') return;

    s_call_state = CallState::kCalling;
    ++s_call_epoch;
    RefreshActionButton();
    RefreshNumberDisplay();
    // 拨号前要先体检 SIM，UI 上先告诉用户在做什么，避免好像"按下没反应"。
    SetStatusText(I18n::T("正在检查网络..."));

    DispatchDial(s_number);
}

void HangupCall() {
    const bool was_calling = (s_call_state == CallState::kCalling);
    s_call_state = CallState::kIdle;
    ++s_call_epoch;
    RefreshActionButton();
    RefreshStatus();
    RefreshNumberDisplay();

    if (was_calling) {
        // 主动挂断: 通知模组释放当前通话。即便 ATD 还没回 OK，
        // 这条 ATH 也能被串行化进 modem 的 AT 队列（at_mutex_）。
        DispatchHangup();
    }
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

void KeyEventCb(lv_event_t* e) {
    const KeyDef* k = static_cast<const KeyDef*>(lv_event_get_user_data(e));
    if (k == nullptr) return;
    AppendDigit(k->digit);
}

void BackspaceEventCb(lv_event_t* /*e*/) {
    Backspace();
}

// Long-press on backspace clears the whole number, matching iPhone behaviour.
void BackspaceLongPressCb(lv_event_t* /*e*/) {
    if (s_call_state == CallState::kCalling) return;
    s_number[0] = '\0';
    RefreshNumberDisplay();
}

void ActionEventCb(lv_event_t* /*e*/) {
    if (s_call_state == CallState::kIdle) {
        StartCall();
    } else {
        HangupCall();
    }
}

void OnSwipeBack() {
    // 函数名保留 OnSwipeBack 是历史叫法；现在只由左上角返回按钮触发。
    // 如果用户在通话中点返回，主动给模组挂断一下，避免通话还挂着。
    if (s_call_state == CallState::kCalling) {
        DispatchHangup();
    }
    s_call_state = CallState::kIdle;
    ++s_call_epoch;
    s_screen_active = false;
    HomeScreen::SwitchToHome();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    // 屏幕被 LVGL 卸载（无论是 swipe-back 还是别的路径），统一在这里把
    // active 标记关掉。AT 任务回来时会看到 false 并丢弃 UI 更新。
    s_screen_active = false;
    s_number_lbl     = nullptr;
    s_status_lbl     = nullptr;
    s_backspace_btn  = nullptr;
    s_action_btn     = nullptr;
    s_action_icon    = nullptr;
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

void BuildHeader(lv_obj_t* parent) {
    // 左上角返回按钮：72x72 透明圆形按钮 + ic_app_back 图标（与其它页面一致）。
    // 点击复用 OnSwipeBack —— 它已经处理了"通话中点返回先挂断再退出"。
    lv_obj_t* back_btn = lv_button_create(parent);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, kBackBtnSize, kBackBtnSize);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, kPad,
                 (kHeaderH - kBackBtnSize) / 2);
    lv_obj_add_event_cb(
        back_btn, [](lv_event_t*) { OnSwipeBack(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    // 标题放在按钮右侧，与按钮垂直居中对齐。
    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, I18n::T("电话"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorTextPrimary),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, kPad + kBackBtnSize + 16,
                 (kHeaderH - 30) / 2);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    // 注：本屏不挂右滑返回手势——拨号过程中横滑数字键太容易被误判成返
    // 回手势，所以只保留左上角的明确按钮作为唯一返回入口。
}

void BuildNumberArea(lv_obj_t* parent) {
    // Big number label, centered.  Sized to hug the font height so the
    // backspace button can vertically align with the actual text rather
    // than a tall padded box.
    s_number_lbl = lv_label_create(parent);
    lv_label_set_text(s_number_lbl, "");
    lv_obj_set_style_text_color(s_number_lbl,
                                lv_color_hex(kColorTextPrimary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_number_lbl, DialNumberFont(), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_number_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_number_lbl, LV_LABEL_LONG_SCROLL);
    lv_obj_set_size(s_number_lbl,
                    kPanelSize - 2 * kPad - kBackspaceBtnSize - 24,
                    kNumberLblH);
    lv_obj_align(s_number_lbl, LV_ALIGN_TOP_MID, 0, kNumberLblTop);
    lv_obj_remove_flag(s_number_lbl, LV_OBJ_FLAG_CLICKABLE);

    // Status line under the number ("拨号中...", etc.)
    s_status_lbl = lv_label_create(parent);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl,
                                lv_color_hex(kColorTextSecondary), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_size(s_status_lbl, kPanelSize - 2 * kPad, kStatusLblH);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, kStatusLblTop);
    lv_obj_remove_flag(s_status_lbl, LV_OBJ_FLAG_CLICKABLE);

    // Backspace button on the right of the number, centered on the number's
    // vertical midline.  We keep the visual circle at 80px but extend the
    // hit-test area by 16px on every side so the effective tap target is
    // 112x112 -- much easier to land than the previous 64px region.
    s_backspace_btn = lv_button_create(parent);
    lv_obj_set_size(s_backspace_btn, kBackspaceBtnSize, kBackspaceBtnSize);
    lv_obj_align(s_backspace_btn, LV_ALIGN_TOP_RIGHT, -kPad - 4, kBackspaceY);
    lv_obj_set_ext_click_area(s_backspace_btn, kBackspaceClickExt);
    lv_obj_set_style_bg_opa(s_backspace_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_backspace_btn, LV_OPA_30,
                            LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_backspace_btn, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_backspace_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_backspace_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_backspace_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_backspace_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_backspace_btn, BackspaceEventCb, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_add_event_cb(s_backspace_btn, BackspaceLongPressCb,
                        LV_EVENT_LONG_PRESSED, nullptr);

    lv_obj_t* back_lbl = lv_image_create(s_backspace_btn);
    lv_image_set_src(back_lbl, "A:ic_s_call_delete.spng");
    lv_obj_remove_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_lbl);

    // Hidden until the user types a digit.
    lv_obj_add_flag(s_backspace_btn, LV_OBJ_FLAG_HIDDEN);
}

void StyleKeyButton(lv_obj_t* btn) {
    lv_obj_set_style_bg_color(btn, lv_color_hex(kKeyBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kKeyBgPress),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
}

void BuildKeypad(lv_obj_t* parent) {
    // 数字盘 3×4（0 居中），在号码区下方剩余高度内垂直居中。
    // 拨打键贴右下角放大，与居中的 0 水平错开，不占数字盘行高。
    // 宽：3*96 + 2*48 = 384；高：4*96 + 3*16 = 432
    const int row_w =
        kKeypadCols * kKeyDiameter + (kKeypadCols - 1) * kKeypadColGap;
    const int digit_h =
        kDigitRows * kKeyDiameter + (kDigitRows - 1) * kKeypadRowGap;
    const int x_origin = (kPanelSize - row_w) / 2;
    const int avail_h  = kKeypadH - kKeypadBottomPad;
    const int y_origin = kKeypadY + (avail_h - digit_h) / 2;
    const int cell_step_x = kKeyDiameter + kKeypadColGap;
    const int cell_step_y = kKeyDiameter + kKeypadRowGap;

    for (const auto& k : kKeys) {
        lv_obj_t* btn = lv_button_create(parent);
        lv_obj_set_size(btn, kKeyDiameter, kKeyDiameter);
        lv_obj_set_pos(btn,
                       x_origin + k.col * cell_step_x,
                       y_origin + k.row * cell_step_y);
        StyleKeyButton(btn);
        lv_obj_add_event_cb(btn, KeyEventCb, LV_EVENT_CLICKED,
                            const_cast<KeyDef*>(&k));

        // Inner column: digit on top, sub-letters below.
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t* digit = lv_label_create(btn);
        lv_label_set_text(digit, k.digit);
        lv_obj_set_style_text_color(digit, lv_color_hex(kKeyText), LV_PART_MAIN);
        lv_obj_set_style_text_font(digit, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_remove_flag(digit, LV_OBJ_FLAG_CLICKABLE);

        if (k.sub != nullptr && k.sub[0] != '\0') {
            lv_obj_t* sub = lv_label_create(btn);
            lv_label_set_text(sub, k.sub);
            lv_obj_set_style_text_color(sub, lv_color_hex(kKeySubText),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(sub, &font_puhui_20_4, LV_PART_MAIN);
            lv_obj_set_style_pad_top(sub, 2, LV_PART_MAIN);
            lv_obj_remove_flag(sub, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    // 拨打 / 挂断：贴屏幕右下角，直径 128 突出主操作。
    s_action_btn = lv_button_create(parent);
    lv_obj_set_size(s_action_btn, kActionBtnD, kActionBtnD);
    lv_obj_align(s_action_btn, LV_ALIGN_BOTTOM_RIGHT,
                 -kActionEdgePad, -kActionEdgePad);
    lv_obj_set_style_radius(s_action_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_action_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_action_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_action_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_action_btn, ActionEventCb, LV_EVENT_CLICKED, nullptr);

    s_action_icon = lv_image_create(s_action_btn);
    lv_image_set_src(s_action_icon, "A:ic_s_call_diall.spng");
    lv_image_set_inner_align(s_action_icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_remove_flag(s_action_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_action_icon);
}

}  // namespace

// ---------------------------------------------------------------------------
// CallScreen public interface
// ---------------------------------------------------------------------------

// DispatchResult lives in the anonymous-namespace-ish public surface so the
// StubCallDriver's lv_async_call trampoline can reach it. Implementation
// matches the original OnAtResult() handler verbatim, only renamed.
void CallScreen::DispatchResult(const CallScreen::CallResult &res_in)
{
    // Copy fields we need before any potential early-return so callers can
    // pass a temporary.
    const CallScreen::JobKind kind    = res_in.kind;
    const CallScreen::Outcome outcome = res_in.outcome;
    const uint32_t            epoch   = res_in.epoch;

    // 屏幕已销毁，直接丢弃。
    if (!s_screen_active) return;
    // 状态已经变了（比如拨号还没回 OK 用户就按了挂断/返回），结果作废。
    if (epoch != s_call_epoch) return;

    if (kind == CallScreen::JobKind::kDial) {
        switch (outcome) {
            case CallScreen::Outcome::kDialOk:
                // ATD 已经收到 OK，正在通话中。
                SetStatusText(I18n::T("通话中"));
                break;
            case CallScreen::Outcome::kSimNotReady:
                s_call_state = CallState::kIdle;
                ++s_call_epoch;
                RefreshActionButton();
                RefreshNumberDisplay();
                SetStatusText(I18n::T("请检查移动网络"));
                break;
            case CallScreen::Outcome::kNo4G:
                s_call_state = CallState::kIdle;
                ++s_call_epoch;
                RefreshActionButton();
                RefreshNumberDisplay();
                SetStatusText(I18n::T("无 4G 模块"));
                break;
            case CallScreen::Outcome::kDialFailed:
            default:
                s_call_state = CallState::kIdle;
                ++s_call_epoch;
                RefreshActionButton();
                RefreshNumberDisplay();
                SetStatusText(I18n::T("拨号失败"));
                break;
        }
    } else {
        // 挂断的反馈不是必须展示的，简单清空状态行即可。
        if (s_call_state == CallState::kIdle) {
            SetStatusText("");
        }
    }
}

void CallScreen::SetCallDriver(CallDriver *driver)
{
    s_driver = driver;
}

void CallScreen::BumpCallEpoch()
{
    ++s_call_epoch;
}

lv_obj_t* CallScreen::CreateStatic() {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);

    s_number[0] = '\0';
    s_call_state = CallState::kIdle;
    ++s_call_epoch;
    s_screen_active = true;

    BuildHeader(scr);
    BuildNumberArea(scr);
    BuildKeypad(scr);

    RefreshNumberDisplay();
    RefreshActionButton();
    RefreshStatus();

    // 屏幕被卸载时清理 active 标志，确保 lv_async_call 不会回到野指针。
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    // 注：本屏不再挂右滑返回手势——拨号过程中误触概率太高（横滑数字键
    // 容易扫成返回）。返回入口只保留左上角的明确按钮 OnSwipeBack()。

    return scr;
}

lv_obj_t* CallScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void CallScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        // 进入拨号界面：把功放切到 4G 通话路径。
        // TODO(openvela): replace with real audio-route call once the
        // board driver exposes IOExpander::PA_SWITCH.
        ESP_LOGI(TAG, "load: PA_SWITCH=false (route to 4G) [stub]");
    } else {
        // 退出拨号界面：恢复默认 WIFI/本地音频路径。
        ESP_LOGI(TAG, "unload: PA_SWITCH=true (route to WIFI) [stub]");

        // 兜底：如果离开时仍处于通话态（理论上 OnSwipeBack 已经处理过，
        // 但如果是别的路径触发的卸载，这里保险一下），主动挂断。
        if (s_call_state == CallState::kCalling) {
            DispatchHangup();
            s_call_state = CallState::kIdle;
            ++s_call_epoch;
        }
        s_screen_active = false;
    }
}
