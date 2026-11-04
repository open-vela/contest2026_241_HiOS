#include "hi_task_policy.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * CallScreen — ported from MetalioClaw4
 * main/display/screen/call_screen/call_screen.cc.
 *
 * Calls use the NuttX NT26 modem driver on a serialized background worker.
 * PA_SWITCH selects the modem audio path while this screen is active.
 * Results return to LVGL asynchronously and retain the call epoch guard.
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
#include "sms_service.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "esp_err_shim.h"
#include "freertos_shim.h"
#include "home_screen/home_screen.h"
#include "application.h"
#include "phone_service.h"
#include "font_awesome.h"
#include "idle_power_policy/idle_power_policy.h"
#include "input_method/hi_input_method.h"
#include "system_chrome/system_chrome.h"
#include "audio_service.h"
#include "standby_screen/standby_screen.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>

#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <memory>
#include <mutex>
#include <atomic>
#include <new>
#include <cstdlib>
#include <cerrno>
#include <metalio/metalio.h>

#include "lvgl.h"

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_number_50_4);
LV_FONT_DECLARE(font_awesome_30_4);

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
constexpr uint32_t kColorBg            = 0xF3F4F3;
constexpr uint32_t kColorTextPrimary   = 0x3B4144;
constexpr uint32_t kColorTextSecondary = 0x848B8E;
constexpr uint32_t kColorHintText      = 0x6E6E70;

constexpr uint32_t kKeyBg          = 0xF0F0EF;
constexpr uint32_t kKeyBgPress     = 0xD5D9D7;
constexpr uint32_t kKeyText        = 0x303638;
constexpr uint32_t kKeySubText     = 0x858B8D;

constexpr uint32_t kCallBg         = 0x82AD53;   // iOS green
constexpr uint32_t kCallBgPress    = 0x6E9844;
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
    {"*", "",     3, 0}, {"0", "+", 3, 1}, {"#", "", 3, 2},
};

// ----- dialer state ---------------------------------------------------------
enum class CallState { kIdle, kCalling, kRinging };
lv_timer_t *s_call_poll_timer = nullptr;
bool s_phone_session_enqueued = false;

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
std::atomic<bool> s_screen_active{false};

// Bumped on every call-state transition. Carried by the AT task; on completion
// we drop the result if the epoch no longer matches (e.g. user swiped back, or
// hung up before ATD returned). This avoids a stale "dial OK" overwriting a
// freshly-idle UI.
uint32_t s_call_epoch = 0;
void IncomingCommandResult(const CallScreen::CallResult &result);

// ---------------------------------------------------------------------------
// CallDriver — pluggable call-control interface (see header).
// ---------------------------------------------------------------------------

// The modem operations run in one FIFO worker. A quick hangup cannot race
// ahead of a pending dial, and neither operation blocks the LVGL thread.
class Nt26CallDriver : public CallScreen::CallDriver
{
    struct Job {
        CallScreen::JobKind kind;
        uint32_t epoch;
        std::string number;
        Job *next = nullptr;
        unsigned generation = 0;
    };
    std::mutex mutex_;
    Job *head_ = nullptr;
    Job *tail_ = nullptr;
    bool running_ = false;
    std::atomic<bool> poll_pending_{false};
    std::atomic<unsigned> dial_generation_{0};
    std::atomic<bool> hangup_running_{false};
    std::atomic<bool> call_request_busy_{false};

    esp_err_t Enqueue(CallScreen::JobKind kind, const std::string &number, uint32_t epoch)
    {
        void *storage = std::malloc(sizeof(Job));
        if (storage == nullptr) return ESP_FAIL;
        auto *job = new (storage) Job{kind, epoch, number};
        job->generation=dial_generation_.load();
        std::lock_guard<std::mutex> lock(mutex_);
        if (tail_ != nullptr) tail_->next = job;
        else head_ = job;
        tail_ = job;
        if (!running_) {
            running_ = true;
            if (xTaskCreateLegacy(Worker, "call_at", 8192, this, 4, nullptr) != pdPASS) {
                running_ = false;
                head_ = tail_ = nullptr;
                job->~Job();
                std::free(job);
                return ESP_FAIL;
            }
        }
        return ESP_OK;
    }

    static void ApplyResult(void *data)
    {
        auto *result = static_cast<CallScreen::CallResult *>(data);
        IncomingCommandResult(*result);
        CallScreen::DispatchResult(*result);
        std::free(result);
    }

    static void Worker(void *data)
    {
        auto *self = static_cast<Nt26CallDriver *>(data);
        for (;;) {
            Job *job;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                job = self->head_;
                if (job == nullptr) {
                    self->running_ = false;
                    break;
                }
                self->head_ = job->next;
                if (self->head_ == nullptr) self->tail_ = nullptr;
            }
            int ret = 0;
            switch (job->kind) {
            case CallScreen::JobKind::kDial: ret = job->generation==self->dial_generation_?metalio_app_call_dial(job->number.c_str()):-ECANCELED; break;
            case CallScreen::JobKind::kAnswer: ret = job->generation==self->dial_generation_?metalio_app_call_answer():-ECANCELED; break;
            case CallScreen::JobKind::kHangup: ret = metalio_app_call_hangup(); break;
            case CallScreen::JobKind::kPoll: ret = metalio_app_call_poll(); self->poll_pending_.store(false); break;
            case CallScreen::JobKind::kOpenSession: ret = metalio_app_call_session(true); break;
            case CallScreen::JobKind::kMute:
                ret=job->generation==self->dial_generation_?
                    metalio_nt26_call_control(1,atoi(job->number.c_str())):-ECANCELED;
                break;
            case CallScreen::JobKind::kSpeaker: {
                bool on = atoi(job->number.c_str()) != 0;
                ret = BluetoothScreen::SelectPhoneOutput(!on);
                break;
            }
            case CallScreen::JobKind::kVolume:
                ret = job->generation == self->dial_generation_ &&
                      metalio_nt26_get_call_state() != METALIO_CALL_IDLE ?
                      metalio_nt26_call_control(4, atoi(job->number.c_str())) : -ECANCELED;
                break;
            case CallScreen::JobKind::kReadVolume:
                ret = job->generation == self->dial_generation_ &&
                      metalio_nt26_get_call_state() != METALIO_CALL_IDLE ?
                      metalio_nt26_call_control(5, 0) : -ECANCELED;
                break;
            case CallScreen::JobKind::kDtmf: ret=metalio_nt26_call_control(2,job->number.empty()?0:job->number[0]);break;
            case CallScreen::JobKind::kCloseSession:
                ret = metalio_nt26_get_call_state()==METALIO_CALL_IDLE?metalio_app_call_session(false):0; break;
            }
            if(job->kind==CallScreen::JobKind::kDial||job->kind==CallScreen::JobKind::kAnswer)self->call_request_busy_=false;
            auto outcome = CallScreen::Outcome::kHangupDone;
            if (job->kind == CallScreen::JobKind::kDial || job->kind == CallScreen::JobKind::kAnswer) {
                if (ret >= 0) outcome = CallScreen::Outcome::kDialOk;
                else if (ret == -ENODEV || ret == -ENOSYS) outcome = CallScreen::Outcome::kNo4G;
                else if (ret == -EOPNOTSUPP) outcome = CallScreen::Outcome::kInternalSim;
                else if (ret == -EACCES || ret == -EPERM || ret == -ENETDOWN) outcome = CallScreen::Outcome::kSimNotReady;
                else outcome = CallScreen::Outcome::kDialFailed;
            } else if (job->kind == CallScreen::JobKind::kPoll) {
                outcome = ret < 0 ? CallScreen::Outcome::kPollFailed :
                          ret == METALIO_CALL_INCOMING ? CallScreen::Outcome::kIncoming :
                          ret == METALIO_CALL_ACTIVE || ret == METALIO_CALL_HELD ? CallScreen::Outcome::kConnected :
                          ret == METALIO_CALL_DIALING ? CallScreen::Outcome::kDialing : CallScreen::Outcome::kEnded;
            }
            auto *result = static_cast<CallScreen::CallResult *>(std::malloc(sizeof(CallScreen::CallResult)));
            if (result) *result = CallScreen::CallResult{job->kind, outcome, job->epoch,ret,job->kind==CallScreen::JobKind::kDtmf?(job->number.empty()?0:job->number[0]):atoi(job->number.c_str())};
            job->~Job();
                std::free(job);
            if (result != nullptr && lv_async_call(ApplyResult, result) != LV_RESULT_OK) std::free(result);
        }
    }

public:
    esp_err_t Feature(CallScreen::JobKind kind,int value,uint32_t epoch){
        char text[16];if(kind==CallScreen::JobKind::kDtmf){text[0]=(char)value;text[1]=0;}else snprintf(text,sizeof(text),"%d",value);
        return Enqueue(kind,text,epoch);
    }
    void Session(bool enabled, uint32_t epoch) {
        (void)Enqueue(enabled ? CallScreen::JobKind::kOpenSession : CallScreen::JobKind::kCloseSession, "", epoch);
    }
    void Poll(uint32_t epoch) {
        if (!poll_pending_.exchange(true) && Enqueue(CallScreen::JobKind::kPoll, "", epoch) != ESP_OK)
            poll_pending_.store(false);
    }
    esp_err_t Answer(uint32_t epoch) override {
        if(hangup_running_||call_request_busy_.exchange(true))return ESP_FAIL;
        ++dial_generation_;metalio_nt26_call_begin_dial();
        auto ret=Enqueue(CallScreen::JobKind::kAnswer, "", epoch);if(ret!=ESP_OK)call_request_busy_=false;return ret;
    }
    esp_err_t Dial(const std::string &number, uint32_t epoch) override
    {
        if(hangup_running_||call_request_busy_.exchange(true))return ESP_FAIL;
        ++dial_generation_;metalio_nt26_call_begin_dial();
        auto ret=Enqueue(CallScreen::JobKind::kDial, number, epoch);if(ret!=ESP_OK)call_request_busy_=false;return ret;
    }
    esp_err_t Hangup(uint32_t epoch) override
    {
        if(hangup_running_.exchange(true))return ESP_FAIL;
        ++dial_generation_;BluetoothScreen::CancelPhoneOutputRequest();metalio_nt26_call_request_hangup();PhoneService::CancelRingtone();
        struct Request{Nt26CallDriver *driver;uint32_t epoch;};
        auto *request=static_cast<Request*>(std::malloc(sizeof(Request)));
        if(!request){hangup_running_=false;return ESP_FAIL;}
        *request={this,epoch};
        pthread_t worker;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,16384);
        int rc=hi_task_create_legacy(&worker,&attr,[](void *arg)->void*{
            auto *request=static_cast<Request*>(arg);int ret=metalio_app_call_hangup();
            auto *result=static_cast<CallScreen::CallResult*>(std::malloc(sizeof(CallScreen::CallResult)));
            if(result)*result={CallScreen::JobKind::kHangup,CallScreen::Outcome::kHangupDone,request->epoch,ret,0};
            request->driver->hangup_running_=false;std::free(request);
            if(result&&lv_async_call(ApplyResult,result)!=LV_RESULT_OK)std::free(result);return nullptr;
        },request);pthread_attr_destroy(&attr);
        if(rc){std::free(request);hangup_running_=false;return ESP_FAIL;}pthread_detach(worker);return ESP_OK;
    }
};

CallScreen::CallDriver *s_driver = nullptr;
Nt26CallDriver s_default_driver;

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
        lv_label_set_text(s_number_lbl, "请输入号码");
    } else {
        lv_label_set_text(s_number_lbl, formatted);
    }
    // Backspace only shown when there is something to delete and we are
    // not currently in a "call in progress" state.
    bool show_back = (s_number[0] != '\0' && s_call_state == CallState::kIdle);
    lv_obj_remove_flag(s_backspace_btn, LV_OBJ_FLAG_HIDDEN);
    if(show_back)lv_obj_remove_state(s_backspace_btn,LV_STATE_DISABLED);
    else lv_obj_add_state(s_backspace_btn,LV_STATE_DISABLED);
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
    if (s_call_state != CallState::kIdle) return;
    size_t len = std::strlen(s_number);
    if (len >= kMaxDigits) return;
    s_number[len]     = d[0];
    s_number[len + 1] = '\0';
    RefreshNumberDisplay();
}

void Backspace() {
    if (s_call_state != CallState::kIdle) return;
    size_t len = std::strlen(s_number);
    if (len == 0) return;
    s_number[len - 1] = '\0';
    RefreshNumberDisplay();
}

CallScreen::CallDriver *ActiveDriver() {
    return (s_driver != nullptr) ? s_driver : &s_default_driver;
}

bool DispatchDial(const std::string& number) {
    esp_err_t err = ActiveDriver()->Dial(number, s_call_epoch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Dial dispatch failed: %d", (int)err);
        SetStatusText(I18n::T("系统忙"));
    }
    return err == ESP_OK;
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

    if (!DispatchDial(s_number)) {
        s_call_state = CallState::kIdle; ++s_call_epoch;
        RefreshActionButton(); RefreshNumberDisplay();
    }
}

char s_voice_dial[kMaxDigits + 1] = {};
bool s_voice_dial_armed = false;

void ShowPhoneTab(int tab);

void BeginVoiceDial() {
    if (!s_voice_dial_armed || !s_screen_active || s_call_state != CallState::kIdle)
        return;
    ShowPhoneTab(0);
    std::snprintf(s_number, sizeof(s_number), "%s", s_voice_dial);
    s_voice_dial_armed = false;
    s_voice_dial[0] = '\0';
    RefreshNumberDisplay();
    StartCall();
}

bool QueueVoiceDial(const char *number) {
    if (number == nullptr || number[0] == '\0') return false;
    const size_t len = std::strlen(number);
    if (len < 3 || len > kMaxDigits) return false;
    for (size_t i = 0; i < len; ++i) {
        const bool digit = number[i] >= '0' && number[i] <= '9';
        const bool plus = i == 0 && number[i] == '+';
        if (!digit && !plus) return false;
    }
    if (s_voice_dial_armed && std::strcmp(s_voice_dial, number) == 0) return true;
    if (s_screen_active && s_call_state != CallState::kIdle)
        return std::strcmp(s_number, number) == 0;
    std::snprintf(s_voice_dial, sizeof(s_voice_dial), "%s", number);
    s_voice_dial_armed = true;
    if (!s_screen_active) {
        if (!HomeScreen::LaunchApp("call")) {
            s_voice_dial_armed = false;
            s_voice_dial[0] = '\0';
            return false;
        }
        return true;
    }
    BeginVoiceDial();
    return true;
}

void HangupCall() {
    const bool was_calling = (s_call_state != CallState::kIdle);
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

bool s_backspace_long=false;
void BackspaceEventCb(lv_event_t* /*e*/) {
    if(s_backspace_long){s_backspace_long=false;return;}
    Backspace();
}

// Long-press on backspace clears the whole number, matching iPhone behaviour.
void BackspaceLongPressCb(lv_event_t* /*e*/) {
    s_backspace_long=true;
    if (s_call_state != CallState::kIdle) return;
    s_number[0] = '\0';
    RefreshNumberDisplay();
}

void ActionEventCb(lv_event_t* /*e*/) {
    if (s_call_state == CallState::kCalling) HangupCall();
    else if (s_call_state == CallState::kRinging) {
        s_call_state = CallState::kCalling;
        ++s_call_epoch; RefreshActionButton();
        SetStatusText(I18n::T("正在接听…"));
        if (ActiveDriver()->Answer(s_call_epoch) != ESP_OK) {
            s_call_state = CallState::kRinging; RefreshActionButton();
            SetStatusText(I18n::T("系统忙"));
        }
    } else StartCall();
}

#include "delete_icon.inc"
#include "contact_avatar.inc"
std::string CallMatchNumber(const std::string &number);
#include "phone_book_ui.inc"
#include "call_recording.inc"
#include "incoming_call_ui.inc"

void OnSwipeBack() {
    // 函数名保留 OnSwipeBack 是历史叫法；现在只由左上角返回按钮触发。
    // 如果用户在通话中点返回，主动给模组挂断一下，避免通话还挂着。
    if (s_call_state != CallState::kIdle) {
        DispatchHangup();
    }
    s_call_state = CallState::kIdle;
    ++s_call_epoch;
    s_screen_active = false;
    HomeScreen::SwitchToHome();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    CloseContactEditor();
    s_dial_body = s_phone_list = nullptr;
    s_contact_rows = s_contact_search = nullptr;
    for(auto &tab:s_phone_tabs)tab=nullptr;
    if (s_call_poll_timer) { lv_timer_delete(s_call_poll_timer); s_call_poll_timer = nullptr; }
    if (s_phone_session_enqueued) {
        s_phone_session_enqueued = false;
        if(metalio_nt26_get_call_state()==METALIO_CALL_IDLE)s_default_driver.Session(false, s_call_epoch);
    }
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
    auto *header=lv_obj_create(parent);screen_strip_obj_chrome(header);
    lv_obj_set_pos(header,0,0);lv_obj_set_size(header,720,64);
    lv_obj_remove_flag(header,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(header,lv_color_hex(0xFCFCFB),0);
    lv_obj_set_style_bg_opa(header,LV_OPA_COVER,0);
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
    lv_obj_set_style_image_recolor(back_icon,lv_color_hex(0x646B6E),0);
    lv_obj_set_style_image_recolor_opa(back_icon,LV_OPA_COVER,0);
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
    lv_obj_add_event_cb(s_backspace_btn,[](lv_event_t*){s_backspace_long=false;},LV_EVENT_PRESSED,nullptr);
    lv_obj_add_event_cb(s_backspace_btn, BackspaceEventCb, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_add_event_cb(s_backspace_btn, BackspaceLongPressCb,
                        LV_EVENT_LONG_PRESSED, nullptr);

    lv_obj_t* back_lbl = lv_image_create(s_backspace_btn);
    lv_image_set_src(back_lbl, &kDeleteIcon);
    lv_image_set_scale(back_lbl,512);
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
    lv_obj_align(s_number_lbl,LV_ALIGN_TOP_LEFT,24,88);lv_obj_set_size(s_number_lbl,672,64);
    lv_obj_align(s_status_lbl,LV_ALIGN_TOP_LEFT,24,165);lv_obj_set_size(s_status_lbl,672,30);
    for(const auto &k:kKeys){
        auto *key=PhoneButton(parent,"",k.col*240,204+k.row*85,240,85,KeyEventCb,const_cast<KeyDef*>(&k));
        lv_obj_set_style_bg_color(key,lv_color_hex(kKeyBg),0);
        auto *digit=lv_obj_get_child(key,0);lv_label_set_text(digit,k.digit);
        lv_obj_set_style_text_font(digit,&font_puhui_30_4,0);lv_obj_align(digit,LV_ALIGN_CENTER,0,k.sub[0]?-10:0);
        if(k.sub[0]){auto *sub=lv_label_create(key);lv_label_set_text(sub,k.sub);
            lv_obj_set_style_text_font(sub,&font_puhui_20_4,0);lv_obj_set_style_text_color(sub,lv_color_hex(kKeySubText),0);lv_obj_align(sub,LV_ALIGN_BOTTOM_MID,0,-8);}
    }
    PhoneButton(parent,"保存联系人",0,544,240,92,[](lv_event_t*){if(s_call_state==CallState::kIdle)EditContact({"","",s_number});});
    s_action_btn=PhoneButton(parent,"",240,544,240,92,ActionEventCb);
    lv_obj_set_style_bg_color(s_action_btn,lv_color_hex(kCallBg),0);
    s_action_icon=lv_image_create(s_action_btn);lv_image_set_src(s_action_icon,"A:ic_s_call_diall.spng");lv_obj_center(s_action_icon);lv_obj_remove_flag(s_action_icon,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_backspace_btn,LV_ALIGN_TOP_LEFT,480,544);lv_obj_set_size(s_backspace_btn,240,92);
    lv_obj_set_ext_click_area(s_backspace_btn,0);
    lv_obj_set_style_radius(s_backspace_btn,0,0);lv_obj_set_style_bg_color(s_backspace_btn,lv_color_hex(kKeyBg),0);lv_obj_set_style_bg_opa(s_backspace_btn,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(s_backspace_btn,1,0);lv_obj_set_style_border_color(s_backspace_btn,lv_color_hex(0xD3D5D5),0);
    auto *icon=lv_obj_get_child(s_backspace_btn,0);lv_obj_set_style_image_recolor(icon,lv_color_hex(0x596164),0);lv_obj_set_style_image_recolor_opa(icon,LV_OPA_TRANSP,0);
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
    if(res_in.kind==JobKind::kMute||res_in.kind==JobKind::kSpeaker||res_in.kind==JobKind::kDtmf||
       res_in.kind==JobKind::kVolume||res_in.kind==JobKind::kReadVolume)return;
    // Copy fields we need before any potential early-return so callers can
    // pass a temporary.
    const CallScreen::JobKind kind    = res_in.kind;
    const CallScreen::Outcome outcome = res_in.outcome;
    const uint32_t            epoch   = res_in.epoch;

    // 屏幕已销毁，直接丢弃。
    if (!s_screen_active) return;
    // 状态已经变了（比如拨号还没回 OK 用户就按了挂断/返回），结果作废。
    if (epoch != s_call_epoch) return;

    if (kind == CallScreen::JobKind::kPoll) {
        if (outcome == CallScreen::Outcome::kPollFailed) return;
        if (outcome == CallScreen::Outcome::kIncoming) {
            s_call_state = CallState::kRinging;
            ShowPhoneTab(0);
            auto number=PhoneService::IncomingNumber();
            snprintf(s_number,sizeof(s_number),"%s",number.c_str());
            SetStatusText(I18n::T("来电，点击接听"));
        } else if (outcome == CallScreen::Outcome::kConnected) {
            s_call_state = CallState::kCalling;
            SetStatusText(I18n::T("通话中"));
        } else if (outcome == CallScreen::Outcome::kDialing) {
            s_call_state = CallState::kCalling;
            SetStatusText(I18n::T("拨号中..."));
        } else if (s_call_state != CallState::kIdle) {
            s_call_state = CallState::kIdle; ++s_call_epoch;
            SetStatusText(I18n::T("通话结束"));
        }
        RefreshActionButton(); RefreshNumberDisplay();
        return;
    }
    if (kind == CallScreen::JobKind::kOpenSession || kind == CallScreen::JobKind::kCloseSession) return;
    if (kind == CallScreen::JobKind::kDial || kind == CallScreen::JobKind::kAnswer) {
        switch (outcome) {
            case CallScreen::Outcome::kDialOk:
                SetStatusText(kind == CallScreen::JobKind::kAnswer ? I18n::T("正在接听…") : I18n::T("拨号中..."));
                break;
            case CallScreen::Outcome::kInternalSim:
                s_call_state = CallState::kIdle;
                ++s_call_epoch;
                RefreshActionButton();
                RefreshNumberDisplay();
                SetStatusText(I18n::T("当前 SIM 或模块未支持语音服务"));
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

bool CallScreen::DialVoice(const char *number)
{
    return QueueVoiceDial(number);
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

    s_dial_body=lv_obj_create(scr);screen_strip_obj_chrome(s_dial_body);
    lv_obj_set_size(s_dial_body,720,720);lv_obj_set_pos(s_dial_body,0,0);
    lv_obj_remove_flag(s_dial_body,LV_OBJ_FLAG_SCROLLABLE);
    BuildNumberArea(s_dial_body);
    BuildKeypad(s_dial_body);
    BuildHeader(scr);
    BuildPhoneTabs(scr);

    RefreshNumberDisplay();
    RefreshActionButton();
    RefreshStatus();

    // 屏幕被卸载时清理 active 标志，确保 lv_async_call 不会回到野指针。
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    if (s_driver == nullptr) {
        s_phone_session_enqueued = true;
        if (s_call_poll_timer) lv_timer_delete(s_call_poll_timer);
        s_call_poll_timer = lv_timer_create([](lv_timer_t *) {
            if (s_screen_active && s_driver == nullptr) s_default_driver.Poll(s_call_epoch);
        }, 1500, nullptr);
        /* Let the dialer paint before the modem transaction. Starting it
         * inside the screen load is what blanks a new board. */
        lv_timer_t *arm = lv_timer_create([](lv_timer_t *t) {
            lv_timer_delete(t);
            if (!s_screen_active || s_driver != nullptr) return;
            s_default_driver.Session(true, s_call_epoch);
            s_default_driver.Poll(s_call_epoch);
        }, 600, nullptr);
        lv_timer_set_repeat_count(arm, 1);
    }

    BeginVoiceDial();

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
        s_screen_active = true;
        // Resume after a temporary screen transition without recreating the UI.
        if (s_driver == nullptr && !s_phone_session_enqueued) {
            s_phone_session_enqueued = true;
            s_default_driver.Session(true, s_call_epoch);
            if (!s_call_poll_timer) s_call_poll_timer = lv_timer_create([](lv_timer_t *) {
                if (s_screen_active && s_driver == nullptr) s_default_driver.Poll(s_call_epoch);
            }, 1500, nullptr);
        }
        // Local media stays selected until dial/answer actually starts.
    } else {
        if (s_phone_session_enqueued) {
            s_phone_session_enqueued = false;
            if(metalio_nt26_get_call_state()==METALIO_CALL_IDLE)s_default_driver.Session(false, s_call_epoch);
        }

        // 兜底：如果离开时仍处于通话态（理论上 OnSwipeBack 已经处理过，
        // 但如果是别的路径触发的卸载，这里保险一下），主动挂断。
        // The global in-call surface owns an ongoing modem session.
        s_call_state = CallState::kIdle;
        ++s_call_epoch;
        s_screen_active = false;
    }
}

void CallScreen::InitGlobal(){
 PhoneService::Init();
 SmsService::Init();
 static lv_timer_t *watch=nullptr;if(watch)return;
 watch=lv_timer_create([](lv_timer_t*){
  static uint32_t polled=0;
  static int reported=-1;
  int state=metalio_nt26_get_call_state();
  if(state!=METALIO_CALL_IDLE||s_incoming_card)UpdateIncoming(state);
  else if(!s_call_powering_off&&lv_tick_elaps(polled)>=3000){polled=lv_tick_get();s_default_driver.Poll(s_call_epoch);}
  if(state!=reported&&s_screen_active){
   auto outcome=state==METALIO_CALL_INCOMING?Outcome::kIncoming:
      state==METALIO_CALL_ACTIVE||state==METALIO_CALL_HELD?Outcome::kConnected:
      state==METALIO_CALL_DIALING?Outcome::kDialing:Outcome::kEnded;
   DispatchResult({JobKind::kPoll,outcome,s_call_epoch});
  }reported=state;
 },250,nullptr);
}

void CallScreen::PrepareSystemPowerOff(){
 s_call_powering_off=true;PhoneService::CancelRingtone();
 if(metalio_nt26_get_call_state()!=METALIO_CALL_IDLE)s_default_driver.Hangup(s_call_epoch);
 if(s_incoming_card)CloseIncoming();
}

// RX never takes LVGL's render lock: the global UI timer observes modem state.
extern "C" void metalio_app_call_state_changed(int state)
{
    PhoneService::PostStateChanged(state);
}

extern "C" void metalio_app_call_number_changed(const char *number)
{
    PhoneService::PostCallerNumber(number);
}
