/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Idle power policy — ported from MetalioClaw4
 * main/display/screen/idle_power_policy.cc.
 *
 * Standby / shutdown thresholds persist through the Settings helper
 * (system/settings.cxx). StandbyScreen::Show / HomeScreen::RequestSystemShutdown
 * are forwarded to via the screen headers; on openvela the actual power
 * pulse is stubbed (TODO) inside HomeScreen::RequestSystemShutdown.
 */

#include "idle_power_policy.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "lvgl.h"
#include "home_screen/home_screen.h"
#include "standby_screen/standby_screen.h"
#include "settings.h"
#include "system_chrome/system_chrome.h"
#include "system_chrome/control_center.h"
#include "call_screen/call_screen.h"
#include "camera_screen/camera_screen.h"
#include "recording_screen/recording_screen.h"
#include "application.h"
#include "device_state.h"
#include "usb_virtual_disk.h"
#include "video_player.h"

#include <cstdio>
#include <metalio/metalio.h>
#include <unistd.h>

namespace {

constexpr const char *TAG = "IdlePower";

constexpr int kDefaultShutdownMin = 5;
constexpr int kDefaultStandbyMin = 2;
constexpr int kMaxMinutes = 60;
constexpr const char *kShutdownNvsKey = "idle_off_min";
constexpr const char *kStandbyNvsKey = "idle_stby_min";
constexpr uint32_t kTickPeriodMs = 1000;
constexpr uint32_t kStatusLogIntervalMs = 60 * 1000;

struct State {
    lv_timer_t *timer = nullptr;
    IdlePowerSession session = IdlePowerSession::None;
    uint32_t last_activity_tick = 0;
    uint32_t last_status_log_tick = 0;
    bool preserve_activity_on_detach = false;
    bool standby_triggered = false;
    bool shutdown_triggered = false;

    // Cached thresholds.  OnIdleTick runs every second; reading these back
    // from Settings each tick (std::ifstream + std::string churn on uClibc++)
    // corrupts the heap over time and blue-screens the idle display.
    int standby_minutes = kDefaultStandbyMin;
    int shutdown_minutes = kDefaultShutdownMin;
    bool thresholds_cached = false;
};

State s;

int ClampMinutes(int minutes)
{
    if (minutes < 0) return 0;
    if (minutes > kMaxMinutes) return kMaxMinutes;
    return minutes;
}

uint32_t MinutesToMs(int minutes)
{
    if (minutes <= 0) return 0;
    return static_cast<uint32_t>(minutes) * 60U * 1000U;
}

// Read the persisted thresholds once and cache them.  Called lazily so the
// per-second OnIdleTick never touches Settings/iostreams/std::string.
void LoadThresholds()
{
    Settings settings("display", false);
    s.standby_minutes =
        ClampMinutes(settings.GetInt(kStandbyNvsKey, kDefaultStandbyMin));
    s.shutdown_minutes =
        ClampMinutes(settings.GetInt(kShutdownNvsKey, kDefaultShutdownMin));
    s.thresholds_cached = true;
}

void EnsureTimer();
void StopTimer();

void OnIdleTick(lv_timer_t * /*timer*/)
{
    if (s.session == IdlePowerSession::None) {
        return;
    }

    // Work is activity even when the user does not touch the glass. Merely
    // leaving a chat open (Listening with no detected speech) is not activity.
    auto &app=Application::GetInstance();
    const auto voice=app.GetDeviceState();
    const bool conversation=app.IsVoiceUiActive() &&
        (voice==kDeviceStateSpeaking || (voice==kDeviceStateListening && app.IsVoiceDetected()));
    // An active external-display session is real use without local touch,
    // including a paused/static desktop. Ending/disconnecting it restores
    // the normal idle timeout; a USB storage session is not exempted.
    const bool external_display=UsbVirtualDisk::GetInstance().IsDisplayActive();
    if(conversation || CameraScreen::IsCapturing() || RecordingScreen::IsCapturing() || external_display || HiApp::VideoPlayer::KeepsAwake()) {
        IdlePower_NotifyActivity();
        return;
    }

    // LVGL tracks input for every screen, including apps without a swipe
    // binding. Redrawing clocks, playback and AI animation is not input.
    uint32_t idle_ms = lv_tick_elaps(s.last_activity_tick);
    if(auto *display=lv_display_get_default()){
        const uint32_t input_idle=lv_display_get_inactive_time(display);
        if(input_idle<idle_ms){s.last_activity_tick=lv_tick_get()-input_idle;idle_ms=input_idle;s.standby_triggered=false;s.shutdown_triggered=false;}
    }
    const uint32_t standby_ms = MinutesToMs(IdlePower_GetStandbyMinutes());
    const uint32_t shutdown_ms = MinutesToMs(IdlePower_GetShutdownMinutes());

    static uint32_t last_dbg = 0;
    if (idle_ms - last_dbg >= 10000) {
        last_dbg = idle_ms;
        char dbg[64];
        int n = snprintf(dbg, sizeof(dbg), "IDLE %u s session=%d stby=%u off=%u\n",
                         idle_ms / 1000, static_cast<int>(s.session),
                         standby_ms / 1000, shutdown_ms / 1000);
        if (n > 0) write(1, dbg, static_cast<size_t>(n));
    }

    if (shutdown_ms > 0 && idle_ms >= shutdown_ms) {
        if (!s.shutdown_triggered) {
            s.shutdown_triggered = true;
            ESP_LOGW(TAG,
                     "idle shutdown: idle=%u s, limit=%u s, session=%d",
                     idle_ms / 1000, shutdown_ms / 1000,
                     static_cast<int>(s.session));
            char reason[72];
            std::snprintf(reason, sizeof(reason),
                          I18n::T("无操作 %u 分钟自动关机"),
                          static_cast<unsigned>(shutdown_ms / (60U * 1000U)));
            SystemChrome_DismissOverlays();
            CallScreen::PrepareSystemPowerOff();
            HomeScreen::RequestSystemShutdown(reason);
        }
        return;
    }

    if (s.session != IdlePowerSession::Standby && !StandbyScreen::IsVisible() && standby_ms > 0 &&
        idle_ms >= standby_ms) {
        if (!s.standby_triggered) {
            s.standby_triggered = true;
            ESP_LOGI(TAG, "idle enter standby: idle=%u s, limit=%u s",
                     idle_ms / 1000, standby_ms / 1000);
            write(1, "IDLE_STANDBY_TRIGGER\n",
                  sizeof("IDLE_STANDBY_TRIGGER\n") - 1);
            IdlePower_PrepareAutoStandby();
            SystemChrome_DismissOverlays();
            StandbyScreen::Show();
        }
        return;
    }

    if (lv_tick_elaps(s.last_status_log_tick) < kStatusLogIntervalMs) {
        return;
    }
    s.last_status_log_tick = lv_tick_get();

    uint32_t next_ms = 0;
    const char *next_action = "none";
    if (s.session != IdlePowerSession::Standby && !StandbyScreen::IsVisible() && standby_ms > 0 &&
        (shutdown_ms == 0 || standby_ms < shutdown_ms)) {
        next_ms = standby_ms;
        next_action = "standby";
    } else if (shutdown_ms > 0) {
        next_ms = shutdown_ms;
        next_action = "shutdown";
    }
    if (next_ms > idle_ms) {
        ESP_LOGI(TAG, "idle=%u s, remaining=%u s until %s (session=%d)",
                 idle_ms / 1000, (next_ms - idle_ms) / 1000, next_action,
                 static_cast<int>(s.session));
    }
}

void StopTimer()
{
    if (s.timer != nullptr) {
        lv_timer_delete(s.timer);
        s.timer = nullptr;
    }
}

void EnsureTimer()
{
    if (s.timer != nullptr) {
        return;
    }
    s.timer = lv_timer_create(OnIdleTick, kTickPeriodMs, nullptr);
}

}  // namespace

void IdlePower_NotifyActivity()
{
    s.last_activity_tick = lv_tick_get();
    s.last_status_log_tick = lv_tick_get();
    s.standby_triggered = false;
    s.shutdown_triggered = false;
}

void IdlePower_PrepareAutoStandby()
{
    s.preserve_activity_on_detach = true;
}

void IdlePower_Attach(IdlePowerSession session, bool reset_activity)
{
    if (session == IdlePowerSession::None) {
        return;
    }

    const bool keep_activity = s.preserve_activity_on_detach &&
                               session == IdlePowerSession::Standby;
    if (session == IdlePowerSession::Standby || reset_activity)
        s.preserve_activity_on_detach = false;
    s.session = session;

    if (reset_activity && !keep_activity) {
        IdlePower_NotifyActivity();
    } else if (s.last_activity_tick == 0) {
        IdlePower_NotifyActivity();
    } else {
        if (session == IdlePowerSession::Standby) {
            s.standby_triggered = true;
        } else {
            s.standby_triggered = false;
        }
        s.shutdown_triggered = false;
        s.last_status_log_tick = lv_tick_get();
    }

    EnsureTimer();
    ESP_LOGI(TAG, "attach session=%d reset=%d keep=%d",
             static_cast<int>(session), reset_activity ? 1 : 0,
             keep_activity ? 1 : 0);
}

void IdlePower_Detach(IdlePowerSession session)
{
    if (s.session != session) {
        return;
    }
    s.session = IdlePowerSession::App;

    if (s.preserve_activity_on_detach) {
        ESP_LOGI(TAG, "detach home -> preserving idle for standby");
        return;
    }

    EnsureTimer();
    ESP_LOGI(TAG, "detach session=%d", static_cast<int>(session));
}

void IdlePower_Stop()
{
    s.preserve_activity_on_detach = false;
    s.session = IdlePowerSession::None;
    s.standby_triggered = false;
    s.shutdown_triggered = false;
    StopTimer();
}

int IdlePower_GetStandbyMinutes()
{
    if (!s.thresholds_cached) {
        LoadThresholds();
    }
    return s.standby_minutes;
}

void IdlePower_SetStandbyMinutes(int minutes)
{
    minutes = ClampMinutes(minutes);
    Settings settings("display", true);
    settings.SetInt(kStandbyNvsKey, minutes);
    s.standby_minutes = minutes;
    s.thresholds_cached = true;
    ESP_LOGI(TAG, "standby timeout updated to %d min", minutes);
}

int IdlePower_GetShutdownMinutes()
{
    if (!s.thresholds_cached) {
        LoadThresholds();
    }
    return s.shutdown_minutes;
}

void IdlePower_SetShutdownMinutes(int minutes)
{
    minutes = ClampMinutes(minutes);
    Settings settings("display", true);
    settings.SetInt(kShutdownNvsKey, minutes);
    s.shutdown_minutes = minutes;
    s.thresholds_cached = true;
    ESP_LOGI(TAG, "shutdown timeout updated to %d min", minutes);
}

IdlePowerSession IdlePower_CurrentSession() { return s.session; }

#ifdef HI_VIDEO_TARGET_TEST
// Called under the display lock. No settings are persisted by this check.
bool IdlePower_TestVideoHold(){
 if(!HiApp::VideoPlayer::KeepsAwake())return false;
 const State original=s;s.session=IdlePowerSession::App;s.thresholds_cached=true;
 s.standby_minutes=1;s.shutdown_minutes=1;s.last_activity_tick=lv_tick_get()-10*60*1000;
 s.standby_triggered=false;s.shutdown_triggered=false;OnIdleTick(nullptr);
 bool held=lv_tick_elaps(s.last_activity_tick)<1000&&!s.standby_triggered&&!s.shutdown_triggered;
 s=original;s.last_activity_tick=lv_tick_get();return held;
}
#endif
