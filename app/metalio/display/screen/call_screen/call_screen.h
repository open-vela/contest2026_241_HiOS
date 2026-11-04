/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * CallScreen — ported from MetalioClaw4
 * main/display/screen/call_screen/call_screen.h.
 *
 * 720x720 fullscreen phone dialer. The original drives a Neoway NT26
 * 4G modem over AT commands (AT+CPIN? / ATD<number> / ATH) and toggles
 * IOExpander::PA_SWITCH to route audio to the 4G call path. The NuttX
 * port uses the board NT26 driver through a serialized worker queue.
 *
 * The UI is preserved verbatim:
 *   - 88px header with [back] button + "电话" title
 *   - 100px number area: big 50px-font number display, status line
 *     ("拨号中..." / "通话中" / "拨号失败" / "请检查移动网络" / etc.),
 *     and a circular backspace button (80px visual + 16px extended
 *     click area, long-press clears the whole number)
 *   - 3x4 circular keypad (1-9, 0 centered) with sub-letters (ABC, ...)
 *   - Bottom-right 128px action button (green call / red hangup)
 *
 * State machine (preserved):
 *   kIdle      -> user types digits, backspace available
 *   kCalling   -> action button turns red; back button issues ATH on exit
 *
 * Dial and hangup results reflect the modem response; epochs reject
 * late responses after leaving or recreating the screen.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"
#include "esp_err_shim.h"

#include <string>
#include <cstdint>

class CallScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "call"; }

    // Legacy static entry point — kept for callers that still use the
    // original MetalioClaw4 signature (e.g. home_screen).
    static lv_obj_t *CreateStatic();

    // Route the shared amplifier between local media and cellular audio.
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // -----------------------------------------------------------------
    // CallDriver — pluggable call-control interface.
    //
    // The default driver runs board AT commands on a worker and posts
    // results through lv_async_call. SetCallDriver permits a replacement.
    // -----------------------------------------------------------------
    enum class Outcome : uint8_t {
        kDialOk,         // ATD-equivalent succeeded, call is up
        kInternalSim,    // internal data-only SIM cannot make calls
        kSimNotReady,    // modem / VoIP stack not ready
        kDialFailed,     // dial command came back with an error
        kHangupDone,     // hangup finished (success or failure)
        kNo4G,           // no cellular / VoIP transport available
        kIncoming,
        kConnected,
        kDialing,
        kEnded,
        kPollFailed,
    };

    enum class JobKind : uint8_t {
        kDial,
        kHangup,
        kAnswer,
        kPoll,
        kOpenSession,
        kCloseSession,
        kMute, kSpeaker, kDtmf, kVolume, kReadVolume,
    };

    // Async result posted by a CallDriver implementation back to the UI
    // thread (typically via lv_async_call). The epoch field guards
    // against stale results overwriting a fresher UI state.
    struct CallResult {
        JobKind kind;
        Outcome outcome;
        uint32_t epoch;
        int status = 0;
        int value = 0;
    };

    class CallDriver
    {
    public:
        virtual ~CallDriver() = default;
        virtual esp_err_t Dial(const std::string &number, uint32_t epoch) = 0;
        virtual esp_err_t Hangup(uint32_t epoch) = 0;
        virtual esp_err_t Answer(uint32_t) { return ESP_FAIL; }
    };

    // Install a custom call driver. Pass nullptr to revert to the default
    // stub driver (immediate kDialOk / kHangupDone). Must be called before
    // Create() for the change to take effect on the next screen instance.
    static void SetCallDriver(CallDriver *driver);

    // DispatchResult is called from the LVGL thread (typically inside an
    // lv_async_call trampoline) to apply a CallResult to the UI. Stale
    // results (screen inactive or epoch mismatch) are silently dropped.
    static void DispatchResult(const CallResult &res);

    // For unit tests / external callers: bump the call epoch so any
    // in-flight async results become stale. The screen does this itself
    // on every state transition and on swipe-back.
    static void BumpCallEpoch();
    static void InitGlobal();
    static void PrepareSystemPowerOff();

    // Open the dialer if needed and start a call. Safe to call from the
    // LVGL thread only. A second request for the same number while that
    // call is already starting is ignored. Returns false when another
    // call is already active or the screen cannot be opened.
    static bool DialVoice(const char *number);
};
