/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Application orchestrator — ported from MetalioClaw4 main/application.h.
 *
 * This is the main Application singleton that owns the protocol, audio
 * service, OTA flow, MCP server and the chat state machine. The public
 * interface matches the upstream implementation so ported boards and
 * tools can call into it unchanged.
 */

#ifndef METALIO_APPLICATION_H
#define METALIO_APPLICATION_H

#include "esp_timer_shim.h"
#include "freertos_shim.h"
#include "esp_err_shim.h"

#include <syslog.h>
#include <semaphore.h>

#include "device_state.h"
#include "device_state_event.h"
#include "protocol.h"
#include "ota.h"
#include "audio_service.h"

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>
#include <atomic>

struct cJSON;

#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)
#define MAIN_EVENT_VOICE_UI (1 << 7)
#define MAIN_EVENT_WAKE_DONE (1 << 8)
#define MAIN_EVENT_XIAOZHI_JSON (1 << 9)
#define MAIN_EVENT_TTS_PROMOTE (1 << 10)
#define MAIN_EVENT_BARGE_IN (1 << 11)

enum AecMode
{
    kAecOff,
    kAecOnDeviceSide,
};

class Application
{
public:
    static Application &GetInstance()
    {
        /* Uses a global storage buffer with placement-new to avoid
         * __cxa_guard_acquire which may hang during early CRT startup. */
        static char storage[sizeof(Application)] alignas(Application);
        static Application *ptr = nullptr;
        if (!ptr)
        {
            ptr = new (storage) Application();
        }
        return *ptr;
    }
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    void Start();
    void MainEventLoop();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_->IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char *status, const char *message,
               const char *emotion = "", const std::string_view &sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    /* LVGL-safe: posts MAIN_EVENT_WAKE_WORD_DETECTED (same path as "Hi openvela"). */
    void RequestWakeSession();
    void Reboot();
    /* After UI STA+DHCP succeeds: restart MQTT/WebSocket so xiaozhi.me
     * binds to the new IP/DNS without a full chip reboot. */
    void OnWifiLinkReady();
    void WakeWordInvoke(const std::string &wake_word);
    bool UpgradeFirmware(Ota &ota, const std::string &url = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string &payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view &sound);
    AudioService &GetAudioService() { return *audio_service_; }

    /* Voice UI session control — wake word is enabled only inside a chat /
     * digital-human session to save memory and CPU on the desktop. */
    void SetVoiceUiDesired(bool desired);
    /* LVGL-thread safe: posts MAIN_EVENT_VOICE_UI (no heap / no mutex). */
    void RequestVoiceUiDesired(bool desired);
    bool IsVoiceUiActive() const { return voice_ui_active_; }
    bool IsVoiceUiDesired() const { return voice_ui_desired_; }

    bool HasPendingActivation() const
    {
        if (activation_suspended_ || pending_activation_code_.empty())
            return false;
        const std::string &c = pending_activation_code_;
        if (c.size() < 4 || c.size() > 8)
            return false;
        for (unsigned char ch : c)
        {
            if (ch < '0' || ch > '9')
                return false;
        }
        return true;
    }
    const std::string &GetPendingActivationCode() const { return pending_activation_code_; }
    bool IsDeviceActivated() const;
    bool IsBootReady() const { return boot_ready_; }
    /* Called as soon as OTA JSON contains a 6-digit code so chat/home can
     * show it without waiting for the activate poll to finish. */
    void ShowActivationCode(const std::string &code, const std::string &message);
    /* Clear pending code when xiaozhi.me reports the device is bound. */
    void ClearPendingActivation();
    /* Re-query OTA activation section so bind/unbind tracks xiaozhi.me. */
    void SyncXiaozhiBindState();
    void SetActivationSuspended(bool suspended);
    bool IsActivationSuspended() const { return activation_suspended_; }
    void StopSystemAudioForStressTest();
    void RestoreSystemAudioAfterStressTest();

    /* Wake worker completion — called via MAIN_EVENT_WAKE_DONE (no heap). */
    void CompleteWakeFromWorker(bool channel_ok);
    void NotifyWakeOpenDone(bool channel_ok);
    bool IsWakeConnectInflight() const { return wake_connect_inflight_; }

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    esp_timer_handle_t voice_ui_release_timer_ = nullptr;
    esp_timer_handle_t voice_ui_start_retry_timer_ = nullptr;
    esp_timer_handle_t esp_wake_upgrade_timer_ = nullptr;
    esp_timer_handle_t esp_wake_preload_timer_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    std::unique_ptr<AudioService> audio_service_;
    std::string pending_activation_code_;
    volatile bool activation_suspended_ = false;
    volatile bool boot_ready_ = false;
    /* Set by OnWifiLinkReady while OTA boot worker is still in CheckNewVersion
     * sleeps — wake early and reset HTTPS retry backoff. */
    volatile bool wifi_recheck_requested_ = false;
    volatile bool ota_recheck_inflight_ = false;
    volatile bool voice_ui_desired_ = false;
    volatile bool voice_ui_active_ = false;
    volatile bool wake_connect_inflight_ = false;
    /* -1 = none, 0 = fail, 1 = ok. Set by wake worker, consumed on app thread. */
    std::atomic<int8_t> wake_open_result_{-1};
    volatile uint32_t voice_ui_epoch_ = 0;
    /* -1 = none, 0 = stop, 1 = start. Written from LVGL, consumed on app thread. */
    std::atomic<int8_t> voice_ui_request_{-1};
    /* UDP TTS arrived while still Listening (JSON tts/start delayed). */
    std::atomic<bool> pending_tts_promote_{false};
    /* tts/stop seen but PCM still draining — finish on clock tick. */
    bool tts_stop_pending_ = false;
    uint32_t voice_ui_pending_release_epoch_ = 0;
    uint32_t voice_ui_pending_retry_epoch_ = 0;

    void SyncVoiceUiSession();
    void TearDownVoiceAudioPaths(bool release_wake_word);
    void SoftStopVoiceAudioPaths();
    void ParkVoiceUiProtocol();
    void ApplyVoiceUiStart();
    void ApplyVoiceUiStop();
    void ScheduleVoiceUiHardRelease(uint32_t epoch);
    void CancelVoiceUiHardRelease();
    void ScheduleVoiceUiStartRetry(uint32_t epoch);
    void ScheduleEspWakeWordUpgrade();
    void CancelEspWakeWordUpgrade();
    void ScheduleEspWakeWordPreload();
    void CancelEspWakeWordPreload();
    bool TryEnableWakeWordForVoiceUi();
    void EnterListeningAfterWake();
    void HandleIncomingXiaozhiJson(class Display *display, const cJSON *root);
    void FinishTtsStop();
    void EnsureWakeOpenWorker();
    bool QueueWakeOpenAudioChannel();
    bool EnqueueXiaozhiJson(const cJSON *root);
    void DrainXiaozhiJsonQueue();

    bool has_server_time_ = false;
    bool aborted_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = 0;
    TaskHandle_t main_event_loop_task_handle_ = 0;

    void OnWakeWordDetected();
    void CheckNewVersion(Ota &ota);
    void InitializeXiaozhiProtocol(Ota &ota);
    void OtaBootWorker(Ota *ota);
    /* Re-run CheckVersion/activation after a mid-boot WiFi change when the
     * first OTA worker already exited (MAX_RETRY) or left a pending code. */
    void StartXiaozhiOtaRecheck();
    void OtaRecheckWorker(Ota *ota);
    void SetListeningMode(ListeningMode mode);
    void ApplyInterruptPreferenceFromNvs();
    void ArmXiaozhiBindSyncTimer();

    esp_timer_handle_t xiaozhi_bind_sync_timer_ = nullptr;
};

/* Helper that temporarily lowers the current task's priority (used by the
 * reference code for camera capture). On NuttX this is a best-effort stub. */
class TaskPriorityReset
{
public:
    TaskPriorityReset(BaseType_t priority)
    {
        (void)priority;
        /* NuttX: lowering priority would require task_setpriority(getpid(), ...).
         * Stubbed for now — camera capture is not yet ported. */
    }
    ~TaskPriorityReset() {}
};

#endif /* METALIO_APPLICATION_H */
