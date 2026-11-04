#include "hi_task_policy.h"
#include "network_service.h"
#include <net/if.h>
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Application orchestrator implementation — ported from
 * MetalioClaw4 main/application.cc.
 *
 * The state machine, event loop, voice-UI session handling, OTA flow and
 * protocol wiring are preserved from the upstream implementation. ESP-IDF
 * specifics are replaced as follows:
 *
 *   - esp_timer/xEventGroup/xTaskCreate  → NuttX shims (work_queue, nxevent,
 *                                          task_create) via freertos_shim.h /
 *                                          esp_timer_shim.h.
 *   - Board::GetInstance()               → board_shim.h (returns the same
 *                                          singleton; subsystem getters may
 *                                          return nullptr until the matching
 *                                          HAL is wired up).
 *   - Lang::Strings::X / Lang::Sounds::X → literal strings (i18n is wired up
 *                                          separately; the openvela port keeps
 *                                          the catalog under I18n::Str).
 *   - esp_app_get_description()          → compiled-in version string.
 *   - esp_restart()                      → boardctl(BOARDIOC_RESET) or exit().
 *   - heap_caps_get_free_size()          → mallinfo() via SystemInfo.
 *   - LVGL screens (OtaScreen/HomeScreen) → no-op stubs (HAVE_LVGL not defined
 *                                          in this port yet).
 */

#include "application.h"
#ifdef HI_VIDEO_TARGET_TEST
namespace HiApp { void StartVideoTargetTests(); }
#endif
#include "radio_screen/radio_player.h"
#include "app_dns.h"
#include "board_shim.h"
#include "metalio_claw_4_board.h"
#include "display.h"
#include "system_info.h"
#include "settings.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "mcp_server.h"
#include "camera_screen/camera_screen.h"
#include "digital_people_screen/digital_people_screen.h"
#include "home_screen/home_screen.h"
#include "panel_screen/panel_screen.h"
#include "smart_home/smart_home.h"
#include "chat_screen/chat_screen.h"
#include "system_chrome/system_chrome.h"
#include "system_chrome/control_center.h"
#include "energy_wake_word.h"
#include "audio/wake_words/hi_openvela_command.h"
#include "chat_local_cmd.h"
#include "audio_codec.h"
#include "hi_music_screen/local_music_player.h"
#include "music_screen/music_screen.h"
#include "bluetooth_screen/bluetooth_screen.h"

#include <metalio/metalio.h>

#include <pthread.h>
#include <sched.h>
#include <nuttx/config.h>

#include "esp_log_shim.h"
#include "cJSON_compat.h"
#include "esp_timer_shim.h"
#include "freertos_shim.h"

#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <thread>
#include <chrono>
#include <algorithm>
#include <array>
#include <mutex>
#include <atomic>
#include <pthread.h>

#include <unistd.h>
#include <malloc.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <cerrno>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netutils/netlib.h>

#include "esp_system.h"

#define TAG "Application"

namespace {
struct TypedChatState {
    std::atomic<bool> active{false};
    std::string pending,last;
};
// Initialized on Start before any app/audio/UI thread, without an early
// function-local C++ guard or a change to the core Application storage ABI.
TypedChatState *s_typed_chat=nullptr;
TypedChatState &TypedChat() { return *s_typed_chat; }
}

bool Application::IsTypedChatMode() const
{
    return s_typed_chat && s_typed_chat->active.load();
}

static bool ActiveNetworkHasIpv4();

static void *NetworkBootWorker(void * /*arg*/)
{
    Board::GetInstance().InitializeNetwork();
    write(1, "NET1\n", 5);
    if (ActiveNetworkHasIpv4())
        Application::GetInstance().OnWifiLinkReady();
    return nullptr;
}

/* Diagnostic: set to 1 right before the main event loop first blocks on
 * nanosleep().  The arch idle hook (up_idle) uses this to start printing
 * WFI/tick/irq probes only at the failure point, not during early boot. */
extern "C" volatile int g_app_entered_sleep = 0;

/* Claw4-aligned wake open on a *persistent* large-stack worker.
 * Creating a fresh 96KB pthread on every wake (while Chat LVGL is live)
 * exhausts the shared heap and paints a solid-blue frame. Allocate the
 * worker once at boot, then only sem_post for each wake. */
static sem_t s_wake_open_sem;
static std::atomic<bool> s_wake_worker_ready{false};
static std::atomic<Protocol *> s_wake_open_proto{nullptr};

static void *WakeOpenWorkerLoop(void *arg)
{
    auto *app = static_cast<Application *>(arg);
    write(1, "WAKE_WRK\n", 9);
    for (;;)
    {
        while (sem_wait(&s_wake_open_sem) != 0)
        {
            if (errno != EINTR)
                break;
        }
        Protocol *proto = s_wake_open_proto.exchange(nullptr,
                                                     std::memory_order_acq_rel);
        write(1, "WAKE_THR\n", 9);
        /* Spurious sem wake with no proto must not NotifyWakeOpenDone(false)
         * — that ClearOpeningAudioGate'd at boot and confused wake state. */
        if (!proto)
        {
            write(1, "WAKE_THR_NOP\n", 13);
            continue;
        }
        /* Only open the audio channel here. listen/detect+start MUST run on
         * the app thread after io_mutex serializes TLS — doing Publish from
         * this worker while RxLoop ssl_reads caused WAKE_START hang + blue. */
        // A late local-player cleanup used to erase/reject the first cloud
        // packets. Wait for its private session and DMA tail on this existing
        // worker, never on the UI/app thread, before opening the AI channel.
        auto &audio=app->GetAudioService();
        for (int waited=0;waited<300 && !audio.VoicePlaybackSettled() &&
             app->GetDeviceState()==kDeviceStateConnecting &&
             metalio_nt26_get_call_state()==METALIO_CALL_IDLE && !audio.HasExclusiveCapture();++waited)
            usleep(10000);
        bool ok=app->GetDeviceState()==kDeviceStateConnecting &&
            audio.VoicePlaybackSettled() && !audio.HasExclusiveCapture() &&
            metalio_nt26_get_call_state()==METALIO_CALL_IDLE;
        if (ok) ok=BluetoothScreen::PrepareVoiceRoute()>=0;
        if (ok) {
            audio.BeginWakeGreetingPlayback();
            if(auto *codec=audio.GetCodec())codec->EnableOutput(true);
            ok=proto->OpenAudioChannel();
        }
        write(1, ok ? "WAKE_CH_OK\n" : "WAKE_CH_FAIL\n", ok ? 11 : 13);
        app->NotifyWakeOpenDone(ok);
    }
    return nullptr;
}

/* MQTT RX must not Schedule(std::function) — that hung after MQTT_IN stt on
 * this uClibc++ port. Print JSON to a small lock-free queue instead. */
static constexpr int kXiaozhiJsonQ = 32;
static char *s_xiaozhi_json_q[kXiaozhiJsonQ];
static std::atomic<uint32_t> s_xiaozhi_json_head{0};
static std::atomic<uint32_t> s_xiaozhi_json_tail{0};


/* ------------------------------------------------------------------ */
/* i18n shorthands — the openvela port keeps strings as literals until
 * the LVGL screen layer is wired up to I18n::Tr().                  */
/* ------------------------------------------------------------------ */
namespace Lang
{
namespace Strings
{
constexpr const char *STANDBY = "Standby";
constexpr const char *LOADING_PROTOCOL = "Loading protocol";
constexpr const char *LOADING_ASSETS = "Loading assets";
constexpr const char *CHECKING_NEW_VERSION = "Checking new version";
constexpr const char *ACTIVATION = "Activating";
constexpr const char *CONNECTING = "Connecting";
constexpr const char *LISTENING = "Listening";
constexpr const char *SPEAKING = "Speaking";
constexpr const char *ERROR = "Error";
constexpr const char *PLEASE_WAIT = "Please wait...";
constexpr const char *VERSION = "Version: ";
constexpr const char *NEW_VERSION = "New version: ";
constexpr const char *OTA_UPGRADE = "OTA upgrade";
constexpr const char *UPGRADING = "Upgrading...";
constexpr const char *UPGRADE_FAILED = "Upgrade failed";
constexpr const char *RTC_MODE_ON = "RTC mode on";
constexpr const char *RTC_MODE_OFF = "RTC mode off";
} /* namespace Strings */

namespace Sounds
{
#include "popup_ogg.inc"
/* Real OGG from MetalioClaw4; stubs below are no-ops until assets land. */
static const std::string_view OGG_POPUP{
    reinterpret_cast<const char *>(kPopupOgg), kPopupOggLen};
constexpr const char *OGG_UPGRADE = "upgrade";
constexpr const char *OGG_EXCLAMATION = "exclamation";
constexpr const char *OGG_VIBRATION = "vibration";
constexpr const char *OGG_SUCCESS = "success";
constexpr const char *OGG_ERR_REG = "err_reg";
} /* namespace Sounds */
} /* namespace Lang */

/* ------------------------------------------------------------------ */
/* State strings                                                       */
/* ------------------------------------------------------------------ */

static const char *const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"};

/* ------------------------------------------------------------------ */
/* Construction / destruction                                          */
/* ------------------------------------------------------------------ */

Application::Application()
{
    event_group_ = xEventGroupCreate();
    aec_mode_ = kAecOff;

    esp_timer_create_args_t clock_timer_args = {
        .callback =
            [](void *arg) {
                auto *app = static_cast<Application *>(arg);
                if (app->event_group_)
                    xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
            },
        .arg = this,
        .name = "clock_timer",
        .dispatch_method = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application()
{
    CancelVoiceUiHardRelease();
    CancelEspWakeWordUpgrade();
    CancelEspWakeWordPreload();
    if (voice_ui_start_retry_timer_ != nullptr)
    {
        esp_timer_stop(voice_ui_start_retry_timer_);
        esp_timer_delete(voice_ui_start_retry_timer_);
        voice_ui_start_retry_timer_ = nullptr;
    }
    if (esp_wake_upgrade_timer_ != nullptr)
    {
        esp_timer_delete(esp_wake_upgrade_timer_);
        esp_wake_upgrade_timer_ = nullptr;
    }
    if (esp_wake_preload_timer_ != nullptr)
    {
        esp_timer_delete(esp_wake_preload_timer_);
        esp_wake_preload_timer_ = nullptr;
    }
    if (voice_ui_release_timer_ != nullptr)
    {
        esp_timer_delete(voice_ui_release_timer_);
        voice_ui_release_timer_ = nullptr;
    }
    if (clock_timer_handle_ != nullptr)
    {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (event_group_)
        vEventGroupDelete(event_group_);
}

/* ------------------------------------------------------------------ */
/* Activation / version check                                          */
/* ------------------------------------------------------------------ */

static bool HasPersistedXiaozhiProtocol()
{
    Settings mqtt("mqtt", false);
    if (!mqtt.GetString("endpoint").empty())
        return true;
    Settings ws("websocket", false);
    return !ws.GetString("url").empty();
}

void Application::ShowActivationCode(const std::string &code,
                                     const std::string &message)
{
    if (activation_suspended_)
        return;
    /* Ignore stale activation blobs once the device has MQTT/WS or bound NVS. */
    Settings wifi("wifi", false);
    if (wifi.GetInt("xiaozhi_bound", 0) != 0 || HasPersistedXiaozhiProtocol())
        return;
    if (code.empty())
        return;
    /* Only accept a clean 4–8 digit bind code. Junk / UTF-8 blobs used to
     * paint as 乱码 beside the home clock after the device was already bound. */
    if (code.size() < 4 || code.size() > 8)
        return;
    for (unsigned char c : code)
    {
        if (c < '0' || c > '9')
            return;
    }
    pending_activation_code_ = code;
    {
        Settings settings("wifi", true);
        settings.SetInt("xiaozhi_bound", 0);
    }
    HomeScreen::RefreshStatusBar();
    {
        char mark[64];
        int n = snprintf(mark, sizeof(mark), "ACT_CODE:%s\n", code.c_str());
        if (n > 0)
            write(1, mark, (size_t)n);
    }
    /* LOG_LEVEL=1 compiles out ESP_LOGI — use E so the 6-digit code is
     * visible on the serial console while waiting for xiaozhi.me bind. */
    ESP_LOGE(TAG, "xiaozhi.me 验证码: %s (%s)", code.c_str(),
             message.empty() ? "bind at xiaozhi.me" : message.c_str());
}

void Application::ClearPendingActivation()
{
    if (pending_activation_code_.empty())
        return;
    write(1, "ACT_CLEAR\n", 10);
    pending_activation_code_.clear();
    {
        Settings settings("wifi", true);
        settings.SetInt("xiaozhi_bound", 1);
    }
    if (device_state_ == kDeviceStateActivating)
        SetDeviceState(kDeviceStateIdle);
    HomeScreen::RefreshStatusBar();
}

void Application::SyncXiaozhiBindState()
{
    /* Lightweight OTA poll: only sync activation ↔ xiaozhi.me bind state. */
    static std::atomic<bool> inflight{false};
    bool expected = false;
    if (!inflight.compare_exchange_strong(expected, true))
        return;

    struct Guard
    {
        std::atomic<bool> &f;
        ~Guard() { f.store(false); }
    } guard{inflight};

    if (!ActiveNetworkHasIpv4() || voice_network_preparing_.load())
        return;

    /* Never POST during a voice session. This TLS poll shares the SDIO
     * link with the wake worker — a colliding POST delayed UDP setup past
     * the connect watchdog (连接中 → 待唤醒, never 聆听中). */
    if (device_state_ != kDeviceStateIdle || wake_connect_inflight_)
        return;

    const bool was_pending = HasPendingActivation();
    Settings binding("wifi", false);
    // A bound device already has usable cloud credentials. Periodic HTTPS
    // activation polling competes with live LTE voice and can refresh cloud
    // configuration underneath an otherwise healthy MQTT/UDP session.
    // Boot refresh and an explicit activation flow still check the binding.
    if (!was_pending && binding.GetInt("xiaozhi_bound", 0) != 0) return;
    Ota ota;
    if (ota.CheckVersion() != ESP_OK)
        return;

    if (!ota.HasActivationCode() && !ota.HasActivationChallenge())
    {
        ClearPendingActivation();
        write(1, "ACT_BOUND\n", 10);
    }
    else if (ota.HasActivationCode())
    {
        ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        write(1, "ACT_UNBOUND\n", 12);
    }

    const bool now_pending = HasPendingActivation();
    if (was_pending != now_pending && boot_ready_ && !ota_recheck_inflight_)
    {
        /* Bind↔unbind flipped — refresh MQTT/protocol like a WiFi recheck. */
        StartXiaozhiOtaRecheck();
    }
}

void Application::ArmXiaozhiBindSyncTimer()
{
    if (xiaozhi_bind_sync_timer_ != nullptr)
        return;

    esp_timer_create_args_t args = {};
    args.callback = [](void *arg) {
        (void)arg;
        /* Off the main loop — CheckVersion is HTTP and must not stall UI. */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 65536);
        pthread_t th;
        int rc = hi_task_create_legacy(
            &th, &attr,
            [](void *) -> void * {
                Application::GetInstance().SyncXiaozhiBindState();
                return nullptr;
            },
            nullptr);
        pthread_attr_destroy(&attr);
        if (rc == 0)
            pthread_detach(th);
    };
    args.arg = this;
    args.name = "xz_bind_sync";
    if (esp_timer_create(&args, &xiaozhi_bind_sync_timer_) != 0)
        return;
    /* 20s — track xiaozhi.me console bind/unbind without flooding OTA. */
    esp_timer_start_periodic(xiaozhi_bind_sync_timer_, 20 * 1000 * 1000);
}

void Application::SetActivationSuspended(bool suspended)
{
    activation_suspended_ = suspended;
    if (suspended)
    {
        DismissAlert();
        ESP_LOGI(TAG, "Activation suspended for stress test");
    }
    else
    {
        ESP_LOGI(TAG, "Activation resumed after stress test");
    }
}

bool Application::IsDeviceActivated() const
{
    if (HasPendingActivation())
        return false;
    if (boot_ready_)
        return true;
    Settings settings("wifi", false);
    if (settings.GetInt("xiaozhi_bound", 0) != 0)
        return true;
    return HasPersistedXiaozhiProtocol();
}

void Application::StopSystemAudioForStressTest()
{
    /* Radio grabs the shared codec through the AudioService decode queue.
     * Only the voice-processing / wake-word feed paths contend for that
     * codec, so disabling those two and flushing stale decode/playback data
     * is sufficient.
     *
     * Deliberately NOT doing here (all of them break the radio path):
     *   - EnableAudioTesting(false): its std::deque move-assignment under
     *     uClibc++ hangs (audio_testing_queue_ -> audio_decode_queue_).
     *   - CloseAudioChannel()/AbortSpeaking(): block on MQTT/UDP.
     *   - SetDeviceState()/DismissAlert(): touch LVGL from this non-LVGL
     *     worker thread.
     */
    write(1, "SSA_0\n", 6);
    audio_service_->EnableVoiceProcessing(false);
    write(1, "SSA_5\n", 6);
    audio_service_->EnableWakeWordDetection(false);
    write(1, "SSA_6\n", 6);
    audio_service_->ResetDecoder();
    write(1, "SSA_7\n", 6);
    ESP_LOGI(TAG, "System audio stopped for stress test");
}

void Application::RestoreSystemAudioAfterStressTest()
{
    RearmGlobalWakeWord();
    ESP_LOGI(TAG, "System audio restored after stress test (voice_ui=%d)",
             voice_ui_active_ ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* Alerts                                                              */
/* ------------------------------------------------------------------ */

void Application::Alert(const char *status, const char *message,
                        const char *emotion, const std::string_view &sound)
{
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto *display = Board::GetInstance().GetDisplay();
    /* Chat owns the foreground UI during voice wake — never touch status /
     * emotion / DP widgets (wake blue-screen on this port). Bubbles only. */
    if (ChatScreen::IsActive())
    {
        if (display && message && message[0] != '\0')
            display->SetChatMessage("system", message);
        return;
    }
    if (display)
    {
        display->SetStatus(status);
        display->SetEmotion(emotion);
        display->SetChatMessage("system", message);
    }
    if (!sound.empty() && !activation_suspended_)
        audio_service_->PlaySound(sound);
}

void Application::DismissAlert()
{
    /* Chat header owns Idle/"待唤醒" — SetEmotion/SetStatus here races LVGL. */
    if (ChatScreen::IsActive())
        return;
    if (device_state_ == kDeviceStateIdle)
    {
        auto *display = Board::GetInstance().GetDisplay();
        if (display)
        {
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Chat state transitions                                              */
/* ------------------------------------------------------------------ */

void Application::ToggleChatState()
{
    if (device_state_ == kDeviceStateActivating)
    {
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    else if (device_state_ == kDeviceStateWifiConfiguring)
    {
        audio_service_->EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }
    else if (device_state_ == kDeviceStateAudioTesting)
    {
        audio_service_->EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!voice_ui_active_)
    {
        ESP_LOGW(TAG, "ToggleChatState ignored: voice UI session inactive");
        return;
    }
    if (!protocol_)
    {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle)
    {
        /* Same Xiaozhi path as wake word — OpenAudioChannel on 96KB worker. */
        RequestWakeSession();
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    }
    else if (device_state_ == kDeviceStateListening)
    {
        Schedule([this]() {
            if (protocol_)
                protocol_->CloseAudioChannel();
        });
    }
}

void Application::RequestWakeSession()
{
    write(1, "WAKE_REQ\n", 9);
    Schedule([this]() {
        if (!audio_service_ || audio_service_->HasExclusiveCapture() ||
            metalio_nt26_get_call_state() != METALIO_CALL_IDLE) return;
        if (device_state_ == kDeviceStateIdle) {
            LocalMusicPlayer::Instance().SetPaused(true);
            MusicScreen::PauseForVoice();
            RadioPlayer::Instance().Pause();
        }
        if (!boot_ready_)
        {
            write(1, "WAKE_NORDY\n", 11);
            return;
        }
        if (!protocol_)
        {
            write(1, "WAKE_NOPROTO\n", 13);
            return;
        }
        if (!voice_ui_active_)
        {
            write(1, "WAKE_ARM_VUI\n", 13);
            voice_ui_desired_ = true;
            ++voice_ui_epoch_;
            CancelVoiceUiHardRelease();
            ApplyVoiceUiStart();
        }
        if (!voice_ui_active_ && voice_ui_desired_ && boot_ready_ &&
            protocol_ != nullptr)
        {
            write(1, "WAKE_FORCE_VUI\n", 15);
            voice_ui_active_ = true;
        }
        if (!voice_ui_active_)
        {
            write(1, "WAKE_NOVUI\n", 11);
            return;
        }
        OnWakeWordDetected();
    });
}

void Application::EndFloatAiSession()
{
    pending_boot_wake_.store(false, std::memory_order_release);
    Schedule([this]() {
        write(1, "FLOAT_END\n", 10);
        float_listen_idle_sec_ = 0;
        /* Card is already gone (LVGL hide is synchronous). Idle arms
         * wake once. A second TryEnableWakeWord here restarted the engine
         * on the same tick and solid-blued the desktop. */
        ParkVoiceUiProtocol();
    });
}

void Application::BeginTypedChat()
{
    Schedule([this]() {
        if (!ChatScreen::IsForeground() || !audio_service_ ||
            audio_service_->HasExclusiveCapture() || wake_connect_inflight_ ||
            metalio_nt26_get_call_state()!=METALIO_CALL_IDLE) return;
        TypedChat().active.store(true);
        audio_service_->EnableBargeIn(false);
        TypedChat().pending.clear();TypedChat().last.clear();
        CancelVoiceUiHardRelease();
        voice_ui_desired_=true;voice_ui_active_=true;++voice_ui_epoch_;
        if (listen_vp_timer_) esp_timer_stop(listen_vp_timer_);
        listen_vp_deferred_arm_.store(false);
        if (protocol_ && protocol_->IsAudioChannelOpened() &&
            (device_state_==kDeviceStateListening || device_state_==kDeviceStateSpeaking))
            protocol_->SendAbortSpeaking(kAbortReasonNone);
        audio_service_->EnableVoiceProcessing(false);
        audio_service_->DiscardUplinkAudio();
        audio_service_->ResetDecoder();
        SetDeviceState(kDeviceStateIdle);
    });
}

bool Application::RequestTypedChatMessage(const std::string &text)
{
    if (text.empty() || text.size()>2048 || !boot_ready_ || !protocol_ ||
        !NetworkService::Available() || wake_connect_inflight_ ||
        !audio_service_ || audio_service_->HasExclusiveCapture() ||
        metalio_nt26_get_call_state()!=METALIO_CALL_IDLE) return false;
    Schedule([this,text]() {
        if (!ChatScreen::IsForeground() || !TypedChat().pending.empty() ||
            wake_connect_inflight_ || !protocol_ || audio_service_->HasExclusiveCapture() ||
            metalio_nt26_get_call_state()!=METALIO_CALL_IDLE) return;
        TypedChat().active.store(true);
        audio_service_->EnableBargeIn(false);
        voice_ui_desired_=true;voice_ui_active_=true;++voice_ui_epoch_;
        CancelVoiceUiHardRelease();
        if (listen_vp_timer_) esp_timer_stop(listen_vp_timer_);
        listen_vp_deferred_arm_.store(false);
        if (protocol_->IsAudioChannelOpened() &&
            (device_state_==kDeviceStateSpeaking || device_state_==kDeviceStateListening))
            protocol_->SendAbortSpeaking(kAbortReasonNone);
        audio_service_->EnableVoiceProcessing(false);
        audio_service_->EnableWakeWordDetection(false);
        audio_service_->DiscardUplinkAudio();audio_service_->ResetDecoder();
        LocalMusicPlayer::Instance().SetPaused(true);MusicScreen::PauseForVoice();
        RadioPlayer::Instance().Pause();
        TypedChat().pending=text;
        if (protocol_->IsAudioChannelOpened() && audio_service_->VoicePlaybackSettled()) { SendPendingTypedChat();return; }
        SetDeviceState(kDeviceStateConnecting);
        wake_connect_inflight_=true;
        if (!QueueWakeOpenAudioChannel()) {
            wake_connect_inflight_=false;TypedChat().pending.clear();
            SetDeviceState(kDeviceStateIdle);
            if(auto *display=Board::GetInstance().GetDisplay())
                display->SetChatMessage("system","连接暂未就绪，请重新发送");
        }
    });
    return true;
}

void Application::SendPendingTypedChat()
{
    if (!protocol_ || TypedChat().pending.empty()) return;
    protocol_->ClearOpeningAudioGate();wake_connect_inflight_=false;
    listening_mode_=kListeningModeManualStop;aborted_=false;
    SetDeviceState(kDeviceStateListening);
    audio_service_->BeginWakeGreetingPlayback();
    audio_service_->ArmDownlinkPlaybackProtect();
    const bool sent=protocol_->SendUserText(TypedChat().pending);
    if (auto *display=Board::GetInstance().GetDisplay()) {
        if (sent) display->SetChatMessage("user",TypedChat().pending.c_str());
        else display->SetChatMessage("system","发送未确认，请检查网络后重新发送");
    }
    TypedChat().last=sent?TypedChat().pending:std::string();
    TypedChat().pending.clear();
    if (!sent) SetDeviceState(kDeviceStateIdle);
}

void Application::ScheduleDeferredListenVp()
{
    if (listen_vp_timer_ == nullptr)
    {
        esp_timer_create_args_t args = {
            .callback =
                [](void *arg) {
                    auto *app = static_cast<Application *>(arg);
                    app->listen_vp_deferred_arm_.store(true,
                                                       std::memory_order_release);
                    if (app->event_group_)
                        xEventGroupSetBits(app->event_group_,
                                           MAIN_EVENT_LISTEN_VP);
                },
            .arg = this,
        };
        if (esp_timer_create(&args, &listen_vp_timer_) != 0)
            listen_vp_timer_ = nullptr;
    }
    if (listen_vp_timer_ == nullptr)
    {
        listen_vp_deferred_arm_.store(true, std::memory_order_release);
        if (event_group_)
            xEventGroupSetBits(event_group_, MAIN_EVENT_LISTEN_VP);
        return;
    }
    esp_timer_stop(listen_vp_timer_);
    listen_vp_deferred_arm_.store(false, std::memory_order_release);
    /* Let the float bubble finish painting before the mic graph starts. */
    esp_timer_start_once(listen_vp_timer_, 450 * 1000);
}

void Application::NotifyFloatAiDialogActivity()
{
    float_listen_idle_sec_ = 0;
}

void Application::ArmAssistantWakeWord(bool explicit_mode1_restore)
{
    Schedule([this,explicit_mode1_restore]() {
        if (!CanArmSpokenWake()) return;
        // Returning to home/standby must not renegotiate A2DP into SCO or
        // pause a live media session. Only an explicit voice action takes focus.
        if(!explicit_mode1_restore && (audio_service_->HasLocalPlaybackSession() ||
            RadioPlayer::Instance().want_play()))return;
        // Mode 2 keeps its scan/connection lease. Only acknowledged mode 1
        // or a voice surface may restore local clocks; never reset I2S/DMA.
        LocalMusicPlayer::Instance().SetPaused(true);
        MusicScreen::PauseForVoice();
        if(BluetoothScreen::PrepareVoiceRoute()<0)return;
        if(auto *codec=audio_service_->GetCodec()){codec->EnableInput(true);codec->EnableOutput(true);}
        write(1, "WW_ARM_AI\n", 10);
        if (PanelScreen::IsForeground())
            write(1, "WAKE_SURFACE_PANEL\n", 19);
        EnergyWakeWord::SetDesktopStrict(false);
        if (audio_service_ == nullptr)
            return;
        audio_service_->ClearWakeWordMute();
        if(device_state_!=kDeviceStateConnecting&&device_state_!=kDeviceStateListening&&device_state_!=kDeviceStateSpeaking)
            audio_service_->EnableVoiceProcessing(false);
        TryEnableWakeWordForVoiceUi();
    });
}

void Application::StartListening()
{
    if (device_state_ == kDeviceStateActivating)
    {
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    else if (device_state_ == kDeviceStateWifiConfiguring)
    {
        audio_service_->EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!voice_ui_active_)
    {
        ESP_LOGW(TAG, "StartListening ignored: voice UI session inactive");
        return;
    }
    if (!protocol_)
    {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle)
    {
        RequestWakeSession();
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening()
{
    if (device_state_ == kDeviceStateAudioTesting)
    {
        audio_service_->EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end())
        return;

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening)
        {
            if (protocol_)
                protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

/* ------------------------------------------------------------------ */
/* Boot-time wall-clock seed                                          */
/* ------------------------------------------------------------------ */

static void SeedSystemTimeFromBuild()
{
    /* Seed only for TLS date validation. UI remains --:-- until an actual
     * OTA/NTP synchronization marks the wall clock ready. */
    static const char *const kMonths[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    for (int i = 0; i < 12; ++i)
    {
        if (strncmp(__DATE__, kMonths[i], 3) == 0)
        {
            tm.tm_mon = i;
            break;
        }
    }
    tm.tm_mday = atoi(__DATE__ + 4);
    tm.tm_year = atoi(__DATE__ + 7) - 1900;
    tm.tm_hour = atoi(__TIME__);
    tm.tm_min  = atoi(__TIME__ + 3);
    tm.tm_sec  = atoi(__TIME__ + 6);
    tm.tm_isdst = 0;

    time_t epoch = 0;
#if defined(CONFIG_LIBC_LOCALTIME) || defined(HAVE_TIMEGM)
    epoch = timegm(&tm);
#else
    /* NuttX without LOCALTIME: mktime treats fields as wall time. */
    epoch = mktime(&tm);
#endif
    if (epoch <= 0)
        return;

    struct timeval tv;
    tv.tv_sec  = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
}

/* One-shot NTP (Claw4 relies on OTA server_time; NTP fills the gap when
 * OTA has not yet applied a clock). Uses UTC + 8h for CN local display
 * because CONFIG_LIBC_LOCALTIME is off on this port. */
static bool SyncWallClockFromNtpBlocking()
{
    static bool s_ntp_ok = false;
    if (s_ntp_ok)
        return true;

    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(123);
    if (metalio_voice_resolve_ipv4("ntp.aliyun.com", &address.sin_addr.s_addr) < 0)
    {
        write(1, "NTP_DNS_FAIL\n", 13);
        return false;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return false;
    }

    if (metalio_app_bind_active_interface(fd) < 0)
    {
        close(fd);
        write(1, "NTP_BIND_FAIL\n", 14);
        return false;
    }

    struct timeval so;
    so.tv_sec = 3;
    so.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &so, sizeof(so));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &so, sizeof(so));

    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1b; /* LI=0 VN=3 Mode=3 (client) */

    ssize_t n = sendto(fd, pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&address), sizeof(address));
    if (n != (ssize_t)sizeof(pkt))
    {
        close(fd);
        write(1, "NTP_TX_FAIL\n", 12);
        return false;
    }

    n = recv(fd, pkt, sizeof(pkt), 0);
    close(fd);
    if (n < 48)
    {
        write(1, "NTP_RX_FAIL\n", 12);
        return false;
    }

    uint32_t sec = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                   ((uint32_t)pkt[42] << 8) | (uint32_t)pkt[43];
    /* NTP epoch 1900 → Unix 1970 */
    if (sec < 2208988800u)
        return false;
    time_t unix_utc = (time_t)(sec - 2208988800u);

    struct timeval tv;
    tv.tv_sec = unix_utc + 8 * 3600; /* Asia/Shanghai without LOCALTIME */
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0)
        return false;

    metalio_board_clock_synced();
    s_ntp_ok = true;
    write(1, "NTP_OK\n", 7);
    return true;
}

// Network leases must never block the application event loop on NTP/DNS.
// The same resolver/binding follows both Wi-Fi and NT26's selected data path.
static bool SyncWallClockFromNtp()
{
    static std::atomic<bool> ready{false};
    static std::atomic<bool> running{false};
    if (ready.load()) return true;
    if (running.exchange(true)) return false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32768);
    pthread_t thread;
    int rc = hi_task_create_legacy(&thread, &attr, [](void *) -> void * {
        if (SyncWallClockFromNtpBlocking())
        {
            ready.store(true);
            Application::GetInstance().Schedule([](){HomeScreen::RefreshStatusBar();});
        }
        running.store(false);
        return nullptr;
    }, nullptr);
    pthread_attr_destroy(&attr);
    if (rc == 0) pthread_detach(thread);
    else running.store(false);
    return ready.load();
}

static void RefreshTimeUi()
{
    HomeScreen::RefreshStatusBar();
}

/* ------------------------------------------------------------------ */
/* Start — main entry point                                            */
/* ------------------------------------------------------------------ */

void Application::Start()
{
    if (!s_typed_chat) s_typed_chat=new TypedChatState;
    auto &board = Board::GetInstance();
    spoken_wake_enabled_.store(SmartHome_VoiceEnabled());
    printf("SPOKEN_WAKE_ENABLED %d\n",IsSpokenWakeEnabled());
    SeedSystemTimeFromBuild();
    SetDeviceState(kDeviceStateStarting);

    /* --- Step 1: AudioService ---
     * Created and started BEFORE the LVGL render thread and the network
     * stack.  board.InitializeNetwork() blocks for tens of seconds during
     * STA association + host-side DHCP; the home screen auto-launches the
     * radio app on the LVGL thread during that window, and its
     * SessionStartWorker() calls StopSystemAudioForStressTest() ->
     * AudioService::EnableVoiceProcessing().  Starting audio up front
     * guarantees audio_service_ / audio_processor_ are ready when the
     * radio session fires. */
    audio_service_ = std::make_unique<AudioService>();

    if (HasPersistedXiaozhiProtocol())
    {
        Settings settings("wifi", true);
        settings.SetInt("xiaozhi_bound", 1);
        pending_activation_code_.clear();
    }

    AudioServiceCallbacks audio_cbs;
    audio_cbs.on_send_queue_available = [this]() {
        if (event_group_)
            xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    audio_cbs.on_wake_word_detected = [this](const std::string &wake_word) {
        if (wake_word != HI_OPENVELA_PHRASE || !CanArmSpokenWake()) return;
        if (event_group_)
            xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    audio_cbs.on_vad_change = [this](bool /*speaking*/) {
        if (event_group_)
            xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_cbs.wake_word_allowed = [this]() {
        /* Idle: chat / desktop AI. Energy must not run during TTS. */
        const bool want_ww = CanArmSpokenWake();
        if (!want_ww)
            return false;
        if (device_state_ == kDeviceStateIdle)
            return true;
        // Local recognition must not wait for OTA, IP, DNS or cloud setup.
        if (!boot_ready_ && (device_state_ == kDeviceStateStarting ||
                            device_state_ == kDeviceStateActivating))
            return true;
        if (device_state_ == kDeviceStateSpeaking &&
            aec_mode_ == kAecOff &&
            audio_service_ && audio_service_->IsAfeWakeWord())
            return true;
        return false;
    };
    audio_cbs.on_barge_in = [this]() {
        /* Audio input thread → main loop; only while interrupt Speaking. */
        if (event_group_)
            xEventGroupSetBits(event_group_, MAIN_EVENT_BARGE_IN);
    };

    audio_cbs.on_audio_testing_queue_full = [this]()
    {
        SetDeviceState(kDeviceStateWifiConfiguring);
    };

    audio_service_->SetCallbacks(audio_cbs);

    auto *display = board.GetDisplay();
    write(1, "DISP0\n", 6);
    if (display && display->IsActive())
        display->StartDisplayThread();
    write(1, "DISP1\n", 6);
    if (auto *bl = board.GetBacklight())
        bl->RestoreBrightness();

    audio_service_->Start();
    write(1, "AS_DONE\n", 8);
    // Load MultiNet while boot UI is visible. The foreground screen arms
    // capture after its first frame, without blocking LVGL on model creation.
    const bool wake_prepared=audio_service_->PrepareWakeWordDetection();
    if (!wake_prepared) write(1, "WAKE_PREPARE_FAIL\n", 18);
    // The port already documents create/FST versus Home/LVGL startup as an
    // unsafe overlap. Network and OTA also allocate while create is running.
    // Finish this bounded initialization before arming Home or those workers;
    // the display thread continues rendering the boot guide while we yield.
    if (wake_prepared) {
        write(1,"WAKE_BOOT_WAIT\n",15);
        const int64_t deadline=esp_timer_get_time()+20000000;
        while (!audio_service_->IsWakeWordEngineReady() && esp_timer_get_time()<deadline)
            usleep(20000);
        if (audio_service_->IsWakeWordEngineReady()) write(1,"WAKE_BOOT_READY\n",16);
        else write(1,"WAKE_BOOT_TIMEOUT\n",18);
    }

    {
        Settings settings("wifi", false);
        if (settings.GetInt("xiaozhi_bound", 0) != 0)
            pending_activation_code_.clear();
    }

    /* Home after the boot guide — do not block on STA/DHCP (up to ~30s),
     * which left the guide screen up indefinitely. Wi-Fi continues on a
     * worker; SDIO init still runs early on that thread while LVGL shows
     * boot, then home at ~3s. */
    if (display && display->IsActive())
        display->ArmHomeTransition();
    write(1, "HOME_ARMD\n", 10);
#ifdef HI_VIDEO_TARGET_TEST
    HiApp::StartVideoTargetTests();
#endif

    write(1, "NET0\n", 5);
    pthread_attr_t net_attr;
    pthread_attr_init(&net_attr);
    pthread_attr_setstacksize(&net_attr, 65536);
    pthread_t net_thread;
    if (hi_task_create_legacy(&net_thread, &net_attr, NetworkBootWorker, nullptr) == 0)
        pthread_detach(net_thread);
    else
        NetworkBootWorker(nullptr);
    pthread_attr_destroy(&net_attr);
    /* Never EnsureWakeWordReady / Esp preload on the home path — WakeNet
     * create racing home LVGL solid-blues the panel (WW_PRE / EW_IA).
     * Esp upgrades only after chat Idle settles (ScheduleEspWakeWordUpgrade). */
    write(1, "WW_Q_SKIP\n", 10);

    /* Update the status bar immediately to show the network state
     * (mirrors original application.cc: board.StartNetwork() →
     * display->UpdateStatusBar(true)). */
    /* TODO: re-enable once boot→home render is stable; the timed lock /
     * status-bar path corrupts the LVGL draw heap on this uClibc++ port. */
    // if (auto *display = board.GetDisplay(); display && display->IsActive())
    //     display->UpdateStatusBar(true);

    /* Persist the official xiaozhi.me OTA URL when none is stored.
     * CheckVersion POSTs device identity and, if unbound, returns a
     * 6-digit activation code for https://xiaozhi.me. */
    {
        static constexpr const char *kDefaultOtaUrl =
            "https://api.tenclass.net/xiaozhi/ota/";
        Settings settings("wifi", true);
        std::string ota_url = settings.GetString("ota_url");
        if (ota_url.empty())
        {
            ota_url = kDefaultOtaUrl;
            settings.SetString("ota_url", ota_url);
            ESP_LOGI(TAG, "OTA URL empty, wrote default: %s", ota_url.c_str());
        }
        else
        {
            ESP_LOGI(TAG, "OTA URL from settings: %s", ota_url.c_str());
        }
    }

    write(1, "OTA0\n", 5);

    /* OTA + activate poll does blocking HTTPS.  Run it below LVGL so the
     * home screen stays tappable after the 6-digit code appears. */
    auto *ota = new Ota();
    ota->MarkCurrentVersionValid();

    pthread_attr_t ota_attr;
    pthread_attr_init(&ota_attr);
    pthread_attr_setstacksize(&ota_attr, 131072);
    struct sched_param ota_sp;
    memset(&ota_sp, 0, sizeof(ota_sp));
    ota_sp.sched_priority = CONFIG_METALIO_APP_PRIORITY - 10;
    if (ota_sp.sched_priority < 1)
        ota_sp.sched_priority = 1;
    pthread_attr_setschedpolicy(&ota_attr, SCHED_FIFO);
    pthread_attr_setschedparam(&ota_attr, &ota_sp);
    pthread_attr_setinheritsched(&ota_attr, PTHREAD_EXPLICIT_SCHED);

    pthread_t ota_thread;
    int ota_rc = hi_task_create_legacy(
        &ota_thread, &ota_attr,
        [](void *p) -> void * {
            auto *o = static_cast<Ota *>(p);
            Application::GetInstance().OtaBootWorker(o);
            delete o;
            return nullptr;
        },
        ota);
    pthread_attr_destroy(&ota_attr);
    if (ota_rc == 0)
    {
        pthread_detach(ota_thread);
        check_new_version_task_handle_ = (TaskHandle_t)ota_thread;
    }
    else
    {
        ESP_LOGE(TAG, "OTA worker create failed (%d); running inline", ota_rc);
        OtaBootWorker(ota);
        delete ota;
    }

    /* Main event loop — must not wait for activate poll / bind. */
    write(1, "M0\n", 3);

    if (clock_timer_handle_ != nullptr)
        esp_timer_start_periodic(clock_timer_handle_, 1000000);

    hi_task_enable_app_balance();
#ifdef HI_APP_SMP_PROBE
    extern void hi_task_probe_start();
    hi_task_probe_start();
#endif
    MainEventLoop();
}

void Application::ResumePendingBootWake()
{
    Schedule([this]() {
        if (!boot_ready_ || !protocol_ || voice_network_preparing_.load() ||
            !pending_boot_wake_.exchange(false, std::memory_order_acq_rel))
            return;
        // Closing the early popup cancels the pending conversation.
        if (ChatScreen::IsForeground() || DigitalPeopleScreen::IsForeground() || SystemChrome_FloatAiOpen())
        {
            write(1, "BOOT_WAKE_RESUME\n", 17);
            OnWakeWordDetected();
        }
    });
}

void Application::OtaBootWorker(Ota *ota)
{
    Settings wifi_settings("wifi", false);
    const bool cached_bound = wifi_settings.GetInt("xiaozhi_bound", 0) != 0;
    if (cached_bound)
        pending_activation_code_.clear();

    const bool fast_protocol =
        cached_bound && ota->LoadPersistedProtocolConfig();

    if (fast_protocol)
    {
        write(1, "OTA_FAST\n", 8);
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddCommonTools();
        mcp_server.AddUserOnlyTools();
        InitializeXiaozhiProtocol(*ota);
        ApplyInterruptPreferenceFromNvs();
        boot_ready_ = true;
        device_state_ = kDeviceStateIdle;
        ResumePendingBootWake();
        if (!ota->HasServerTime())
            SyncWallClockFromNtp();
        HomeScreen::RefreshStatusBar();
        ArmXiaozhiBindSyncTimer();
        if (voice_ui_desired_)
            Schedule([this]() { SyncVoiceUiSession(); });

        /* Already on the OTA worker: HTTPS must not run in MainEventLoop,
         * where it blocks touch wake and voice-session events on a weak link. */
        ota_check_silent_ = true;
        Ota refresh;
        CheckNewVersion(refresh);
        ota_check_silent_ = false;
        HomeScreen::RefreshStatusBar();
        ChatScreen::RefreshDeviceState();
        write(1, "OTA1\n", 5);
        write(1, "P1\n", 3);
        ESP_LOGE(TAG, "Xiaozhi fast boot ready (mqtt=%d websocket=%d)",
                 ota->HasMqttConfig() ? 1 : 0, ota->HasWebsocketConfig() ? 1 : 0);
        {
            Settings settings("wifi", true);
            settings.SetInt("xiaozhi_bound", 1);
        }
        return;
    }

    CheckNewVersion(*ota);
    write(1, "OTA1\n", 5);

    auto &mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    write(1, "P0\n", 3);
    InitializeXiaozhiProtocol(*ota);
    write(1, "P1\n", 3);
    /* Claw4: restore 打断 preference after audio/protocol are ready. */
    ApplyInterruptPreferenceFromNvs();

    boot_ready_ = true;
    device_state_ = kDeviceStateIdle;
    ResumePendingBootWake();
    has_server_time_ = ota->HasServerTime();
    if (!has_server_time_)
        SyncWallClockFromNtp();
    HomeScreen::RefreshStatusBar();
    ArmXiaozhiBindSyncTimer();
    ESP_LOGE(TAG, "Xiaozhi boot ready (mqtt=%d websocket=%d)",
             ota->HasMqttConfig() ? 1 : 0, ota->HasWebsocketConfig() ? 1 : 0);

    /* Chat may have been opened while OTA/MQTT was still starting — arm WW now. */
    if (voice_ui_desired_)
        Schedule([this]() { SyncVoiceUiSession(); });

    if (!HasPendingActivation())
    {
        Settings settings("wifi", true);
        settings.SetInt("xiaozhi_bound", 1);
    }
}

/* ------------------------------------------------------------------ */
/* Schedule / event loop                                               */
/* ------------------------------------------------------------------ */

void Application::Schedule(std::function<void()> callback)
{
    if (!callback)
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    if (event_group_)
        xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::MainEventLoop()
{
    while (true)
    {
        g_app_entered_sleep = 1;
        auto bits = xEventGroupWaitBits(
            event_group_,
            MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO |
                MAIN_EVENT_WAKE_WORD_DETECTED | MAIN_EVENT_VAD_CHANGE |
                MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR | MAIN_EVENT_VOICE_UI |
                MAIN_EVENT_WAKE_DONE | MAIN_EVENT_XIAOZHI_JSON |
                MAIN_EVENT_TTS_PROMOTE | MAIN_EVENT_BARGE_IN |
                MAIN_EVENT_LISTEN_VP | MAIN_EVENT_WAKE_PREFERENCE,
            pdTRUE, pdFALSE, portMAX_DELAY);
        g_app_entered_sleep = 0;

        /* WAKE_DONE before any usleep so chat does not sit on「连接中」. */
        if (bits & MAIN_EVENT_WAKE_DONE)
        {
            const int8_t r =
                wake_open_result_.exchange(-1, std::memory_order_acq_rel);
            if (r >= 0)
                CompleteWakeFromWorker(r != 0);
        }

        if (bits & MAIN_EVENT_BARGE_IN)
        {
            /* Interrupt: local energy barge while TTS uplink was gated. */
            if (!TypedChat().active.load() && device_state_ == kDeviceStateSpeaking &&
                audio_service_ && audio_service_->IsBargeInEnabled() && voice_ui_active_ &&
                !audio_service_->HasExclusiveCapture() &&
                metalio_nt26_get_call_state()==METALIO_CALL_IDLE &&
                voice_ui_request_.load(std::memory_order_acquire) != 0)
            {
                write(1, "BARGE_APP\n", 10);
                AbortSpeaking(kAbortReasonNone);
                if (audio_service_->IsBargeInEnabled() && voice_ui_active_)
                    SetListeningMode(kListeningModeAutoStop);
            }
        }

        if (bits & MAIN_EVENT_LISTEN_VP)
        {
            /* Next-loop VP Start after Speaking→Listening / wake relisten.
             * Same-tick EnableVP while LVGL paints the last bubble raced
             * the FB (solid blue) and left STT dead on「聆听中」.
             * Desktop float defers one more beat so the card can flip. */
            const bool float_open = SystemChrome_FloatAiOpen();
            const bool arm_now =
                listen_vp_deferred_arm_.exchange(false,
                                                 std::memory_order_acq_rel);
            if (TypedChat().active.load())
            {
                if (audio_service_) audio_service_->EnableVoiceProcessing(false);
            }
            else if (float_open && !arm_now)
            {
                ScheduleDeferredListenVp();
            }
            else if (device_state_ == kDeviceStateListening && audio_service_ &&
                     !audio_service_->IsAudioProcessorRunning())
            {
                write(1, "LISTEN_VP\n", 10);
                audio_service_->SetTtsUplinkGate(false);
                audio_service_->EnableVoiceProcessing(true);
                audio_service_->EnableWakeWordDetection(false);
            }
        }

        if (bits & MAIN_EVENT_TTS_PROMOTE)
        {
            pending_tts_promote_.store(false, std::memory_order_release);
            /* Early Opus beat tts/start — enter Speaking so UI/gate match.
             * Skip while aborted_ (barge-in): leftover UDP must not resume TTS. */
            if (!aborted_ && device_state_ == kDeviceStateListening &&
                voice_ui_desired_)
                SetDeviceState(kDeviceStateSpeaking);
        }

        /* Drain cloud JSON before uplink TX so STT/TTS bubbles are not
         * stuck behind a long SEND_AUDIO burst (felt like bubble delay). */
        if (bits & MAIN_EVENT_XIAOZHI_JSON)
            DrainXiaozhiJsonQueue();

        if (bits & MAIN_EVENT_VOICE_UI)
        {
            const int8_t req =
                voice_ui_request_.exchange(-1, std::memory_order_acq_rel);
            if (req >= 0)
            {
                write(1, req ? "VUI_APP1\n" : "VUI_APP0\n", 9);
                SetVoiceUiDesired(req != 0);
            }
        }

        if (bits & MAIN_EVENT_ERROR)
        {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(),
                  "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO)
        {
            /* While TTS is active, drop uplink so echo is not STT'd.
             * Do not Discard during the short post-TTS settle — that wiped
             * the first Opus frames of the user's next utterance. */
            if (audio_service_ && (TypedChat().active.load() || audio_service_->IsTtsUplinkGated()))
            {
                audio_service_->DiscardUplinkAudio();
            }
            else if (audio_service_)
            {
                while (auto packet = audio_service_->PopPacketFromSendQueue())
                {
                    if (!protocol_ || !protocol_->SendAudio(std::move(packet)))
                        break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_PREFERENCE)
        {
            if (!IsSpokenWakeEnabled() && audio_service_)
                audio_service_->EnableWakeWordDetection(false);
            // Manual listening/TTS is left intact by the wake switch.
        }
        if ((bits & MAIN_EVENT_VAD_CHANGE) && audio_service_ &&
            audio_service_->IsVoiceDetected())
            NotifyFloatAiDialogActivity();
        if ((bits & MAIN_EVENT_WAKE_WORD_DETECTED) && CanArmSpokenWake())
            OnWakeWordDetected();

        if (bits & MAIN_EVENT_SCHEDULE)
        {
            std::deque<std::function<void()>> tasks;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                tasks.swap(main_tasks_);
            }
            for (auto &task : tasks)
            {
                if (task)
                    task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK)
        {
            clock_ticks_++;
            if (TypedChat().active.load() && device_state_==kDeviceStateListening && clock_ticks_>=30) {
                if (protocol_) protocol_->SendAbortSpeaking(kAbortReasonNone);
                SetDeviceState(kDeviceStateIdle);
                if (auto *display=Board::GetInstance().GetDisplay())
                    display->SetChatMessage("system","暂未收到回复，请检查网络后重试");
            }
            /* Skip display->UpdateStatusBar(): corrupts LVGL heap on this port. */
            /* Wake OpenAudioChannel can hang on TLS/DNS; free the session. */
            /* Wake OpenAudioChannel runs on the wake worker and is bounded
             * (hello ≤8s + UDP/DNS a few s). Do NOT abort while it is
             * inflight — the old 6s kill reset to Idle, then the worker's
             * success arrived and the fresh channel was closed (连接中 loop).
             * Only a lost worker result (not inflight) or a genuine 20s
             * hang frees the session. */
            if (device_state_ == kDeviceStateConnecting &&
                clock_ticks_ >= 20 && !wake_connect_inflight_)
            {
                write(1, "WAKE_TO\n", 8);
                wake_connect_inflight_ = false;
                if (protocol_ && protocol_->IsAudioChannelOpened())
                    protocol_->CloseAudioChannel();
                SetDeviceState(kDeviceStateIdle);
            }
            else if (device_state_ == kDeviceStateConnecting &&
                     wake_connect_inflight_ && clock_ticks_ >= 28)
            {
                write(1, "WAKE_TO\n", 8);
                wake_connect_inflight_ = false;
                if (protocol_ && protocol_->IsAudioChannelOpened())
                    protocol_->CloseAudioChannel();
                SetDeviceState(kDeviceStateIdle);
            }
            else if (device_state_ == kDeviceStateConnecting &&
                     clock_ticks_ == 2 && protocol_ &&
                     !wake_connect_inflight_)
            {
                write(1, "WAKE_RETRY\n", 11);
                QueueWakeOpenAudioChannel();
            }
            /* Desktop float AI: after silence in「聆听中」, park the session and
             * dismiss the card (AutoStop otherwise keeps Listening forever).
             * One re-arm first — a wedged uplink left the chip on「聆听中」
             * with STT dead. Closing + wake-arm on that same tick then
             * solid-blued the panel. */
            constexpr int kFloatListenRearmSec = 12;
            constexpr int kFloatListenIdleSec = 30;
            if (!SystemChrome_ChatOwnsAssistantUi() && SystemChrome_FloatAiOpen() &&
                !CameraScreen::IsLookAndSeeMode() &&
                !McpServer::GetInstance().IsVisionAnalysisPending())
            {
                if (device_state_ == kDeviceStateListening)
                {
                    const int idle = float_listen_idle_sec_.fetch_add(
                                         1, std::memory_order_relaxed) +
                                     1;
                    if (idle == kFloatListenRearmSec &&
                        float_listen_idle_sec_.load(std::memory_order_relaxed) >=
                            kFloatListenRearmSec &&
                        protocol_ && audio_service_)
                    {
                        write(1, "LISTEN_REARM\n", 13);
                        audio_service_->SetTtsUplinkGate(false);
                        audio_service_->DiscardUplinkAudio();
                        protocol_->SendStartListening(listening_mode_);
                        audio_service_->EnableVoiceProcessing(false);
                        listen_vp_deferred_arm_.store(false,
                                                      std::memory_order_release);
                        if (event_group_)
                            xEventGroupSetBits(event_group_,
                                               MAIN_EVENT_LISTEN_VP);
                    }
                    if (idle >= kFloatListenIdleSec &&
                        float_listen_idle_sec_.load(std::memory_order_relaxed) >=
                            kFloatListenIdleSec)
                    {
                        float_listen_idle_sec_.store(0, std::memory_order_relaxed);
                        write(1, "FLOAT_IDLE\n", 11);
                        if (listen_vp_timer_ != nullptr)
                            esp_timer_stop(listen_vp_timer_);
                        listen_vp_deferred_arm_.store(false,
                                                      std::memory_order_release);
                        if (audio_service_)
                        {
                            audio_service_->EnableVoiceProcessing(false);
                            audio_service_->SetTtsUplinkGate(false);
                        }
                        if (protocol_)
                        {
                            protocol_->SendStopListening();
                            if (protocol_->IsAudioChannelOpened())
                                protocol_->CloseAudioChannel();
                        }
                        SystemChrome_CloseFloatAi();
                        Schedule([this]() {
                            if (ChatScreen::IsForeground())
                                return;
                            if (device_state_ != kDeviceStateListening &&
                                device_state_ != kDeviceStateSpeaking &&
                                device_state_ != kDeviceStateConnecting)
                                return;
                            /* Wake stays off while the card delete is still
                             * queued. The Idle WW_REARM tick picks it up. */
                            skip_idle_wake_arm_ = SystemChrome_FloatAiOpen();
                            SetDeviceState(kDeviceStateIdle);
                            skip_idle_wake_arm_ = false;
                        });
                    }
                }
                else if (device_state_ == kDeviceStateSpeaking ||
                         device_state_ == kDeviceStateConnecting)
                {
                    float_listen_idle_sec_ = 0;
                }
            }
            else
            {
                float_listen_idle_sec_ = 0;
            }
            /* Missed tts/stop left UI on「讲话中」forever with VP off.
             * Require ~2 s of empty playback (not wall time since Speaking)
             * so brief underruns mid-TTS do not false-trigger. */
            static int s_speak_idle_sec = 0;
            if (device_state_ == kDeviceStateSpeaking && audio_service_ &&
                !McpServer::GetInstance().IsVisionAnalysisPending())
            {
                if (tts_stop_pending_ && !audio_service_->HasPendingPlayback())
                {
                    s_speak_idle_sec = 0;
                    FinishTtsStop();
                }
                else if (!audio_service_->HasPendingPlayback())
                    s_speak_idle_sec++;
                else
                    s_speak_idle_sec = 0;
                if (s_speak_idle_sec >= 2)
                {
                    s_speak_idle_sec = 0;
                    write(1, "SPEAK_TO\n", 9);
                    tts_stop_pending_ = false;
                    /* Match Claw4: only ManualStop (or left chat) → Idle.
                     * AutoStop/Realtime → Listening so header shows「聆听中」. */
                    if (listening_mode_ == kListeningModeManualStop ||
                        voice_ui_request_.load(std::memory_order_acquire) == 0)
                        SetDeviceState(kDeviceStateIdle);
                    else
                        SetDeviceState(kDeviceStateListening);
                }
            }
            else if (McpServer::GetInstance().IsVisionAnalysisPending())
            {
                s_speak_idle_sec = 0;
            }
            /* Reconcile Idle wake state on every voice surface, including
             * a visible assistant card. A stale service bit or downlink gate
             * must not require leaving the page or rebooting to listen again.
             * Wait for a quiet tick with the already-prepared model (two
             * ticks if still preparing), preserve real
             * calls/recorders/media and never reset an active command window. */
            static unsigned wake_idle_quiet_ticks=0;
            const bool idle_wake_surface=(device_state_ == kDeviceStateIdle ||
                 (!boot_ready_ && (device_state_ == kDeviceStateStarting ||
                                   device_state_ == kDeviceStateActivating)));
            const bool idle_wake_eligible=idle_wake_surface &&
                !wake_connect_inflight_ &&
                audio_service_ &&
                CanArmSpokenWake() &&
                !audio_service_->HasLocalPlaybackSession() &&
                !RadioPlayer::Instance().want_play() &&
                !audio_service_->HasPendingPlayback() &&
                !McpServer::GetInstance().IsVisionAnalysisPending();
            if (!idle_wake_eligible) wake_idle_quiet_ticks=0;
            else if (wake_idle_quiet_ticks<3) ++wake_idle_quiet_ticks;
            const unsigned quiet_ticks_needed = audio_service_ &&
                audio_service_->IsWakeWordEngineReady() &&
                !SystemChrome_FloatAiOpen() ? 1 : 2;
            if (idle_wake_eligible && wake_idle_quiet_ticks>=quiet_ticks_needed &&
                (!SystemChrome_FloatAiOpen() ||
                 audio_service_->IsWakeWordEngineReady()))
            {
                EnergyWakeWord::SetDesktopStrict(
                    HomeScreen::IsActive() && !ChatScreen::IsForeground() &&
                    !SystemChrome_FloatAiOpen());
                if (audio_service_->IsAudioProcessorRunning())
                    audio_service_->EnableVoiceProcessing(false);
                if (audio_service_->IsTtsUplinkGated())
                    audio_service_->SetTtsUplinkGate(false);
                if (audio_service_->IsDownlinkPlaybackProtected())
                    audio_service_->ClearDownlinkPlaybackProtect();
                // Leave timed ringtone/notification echo suppression intact.
                // An already-running model's Start() does not clear its PCM.
                audio_service_->EnableWakeWordDetection(true);
            }
        }
    }
}

void Application::NotifyWakeOpenDone(bool channel_ok)
{
    wake_open_result_.store(channel_ok ? 1 : 0, std::memory_order_release);
    if (event_group_)
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_DONE);
}

void Application::EnsureWakeOpenWorker()
{
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true))
        return;

    if (sem_init(&s_wake_open_sem, 0, 0) != 0)
    {
        started.store(false);
        write(1, "WAKE_WRK_FAIL\n", 14);
        return;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* TLS + MQTT hello + DNS — allocate once at boot, not on each wake. */
    pthread_attr_setstacksize(&attr, 96 * 1024);
    pthread_t tid;
    const int rc = hi_task_create_legacy(&tid, &attr, WakeOpenWorkerLoop, this);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        started.store(false);
        write(1, "WAKE_WRK_FAIL\n", 14);
        return;
    }
    pthread_detach(tid);
    s_wake_worker_ready.store(true, std::memory_order_release);
    write(1, "WAKE_WRK_OK\n", 12);
}

bool Application::QueueWakeOpenAudioChannel()
{
    EnsureWakeOpenWorker();
    if (!s_wake_worker_ready.load(std::memory_order_acquire) || !protocol_)
        return false;
    s_wake_open_proto.store(protocol_.get(), std::memory_order_release);
    write(1, "WAKE_GO\n", 8);
    sem_post(&s_wake_open_sem);
    return true;
}

void Application::OnWakeWordDetected()
{
    /* MetalioClaw4 OnWakeWordDetected + CONFIG_SEND_WAKE_WORD_DATA.
     * OpenAudioChannel runs on the persistent wake worker (no per-wake
     * 96KB pthread — that blue-screened Chat when saying 你好小智). */
    // Native conversation belongs to the actual foreground chat/Xiaobai.
    // Every other screen gets the ordinary assistant card, never camera entry.
    const bool chat = SystemChrome_ChatOwnsAssistantUi();
    if (audio_service_)
        audio_service_->SetVisualConversation(CameraScreen::IsLookAndSeeMode());
    const bool float_ai = SystemChrome_FloatAiOpen();
    if (audio_service_->HasExclusiveCapture() ||
        metalio_nt26_get_call_state() != METALIO_CALL_IDLE)
    {
        write(1, "WAKE_SKIP\n", 10);
        return;
    }
    // A real wake/manual wake explicitly returns from typing to voice chat.
    if (TypedChat().active.exchange(false)) ApplyInterruptPreferenceFromNvs();
    TypedChat().pending.clear();TypedChat().last.clear();

    if (!protocol_ || !boot_ready_ || voice_network_preparing_.load())
    {
        // Respond to the real local command now. Only online conversation
        // waits for protocol setup; the popup can already acknowledge it.
        pending_boot_wake_.store(true, std::memory_order_release);
        if (audio_service_)
            audio_service_->EnableWakeWordDetection(false);
        if (!chat && !float_ai)
            SystemChrome_EnsureFloatAi(true);
        write(1, "BOOT_WAKE_LOCAL\n", 16);
        return;
    }

    /* Desktop float AI: paint the card first. Opening UDP/EnableVP on the
     * same tick as the overlay solid-blues home. */
    if (!chat && !SystemChrome_FloatAiOpen())
    {
        SystemChrome_EnsureFloatAi(true);
        write(1, "WAKE_FLOAT\n", 11);
        return;
    }

    if (!voice_ui_active_)
    {
        write(1, "WAKE_ARM_WW\n", 12);
        voice_ui_desired_ = true;
        ++voice_ui_epoch_;
        CancelVoiceUiHardRelease();
        ApplyVoiceUiStart();
        if (!voice_ui_active_ && voice_ui_desired_ && boot_ready_ && protocol_)
            voice_ui_active_ = true;
        if (!voice_ui_active_)
        {
            write(1, "WAKE_NOVUI\n", 11);
            return;
        }
    }

    if (wake_connect_inflight_)
    {
        pending_boot_wake_.store(true, std::memory_order_release);
        write(1, "WAKE_BUSY\n", 10);
        return;
    }

    /* Freeze Esp create/swap for the whole wake session — WakeNet install
     * racing Connecting/Listening solid-blues chat on this port. */
    CancelEspWakeWordUpgrade();
    CancelEspWakeWordPreload();
    if (audio_service_)
        audio_service_->SetWakeWordUpgradeAllowed(false);

    if (device_state_ == kDeviceStateIdle)
    {
        aborted_ = false;
        LocalMusicPlayer::Instance().SetPaused(true);MusicScreen::PauseForVoice();
        RadioPlayer::Instance().Pause();
        audio_service_->ResetDecoder();
        audio_service_->EnableWakeWordDetection(false);
        /* EnergyWakeWord has no wake PCM — Encode+Pop blocked the app thread
         * waiting on an empty opus queue and first tap never reached Listening. */
        if (audio_service_->IsAfeWakeWord())
            audio_service_->EncodeWakeWord();

        SetDeviceState(kDeviceStateConnecting);
        wake_connect_inflight_ = true;

        if (!QueueWakeOpenAudioChannel())
        {
            wake_connect_inflight_ = false;
            SetDeviceState(kDeviceStateIdle);
            if (voice_ui_active_)
                audio_service_->EnableWakeWordDetection(true);
            if (audio_service_)
                audio_service_->SetWakeWordUpgradeAllowed(true);
            ScheduleEspWakeWordUpgrade();
            return;
        }
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        /* Wake-to-abort during AutoStop TTS (AFE WW), or barge-in path. */
        AbortSpeaking(kAbortReasonWakeWordDetected);
    }
    else if (device_state_ == kDeviceStateListening)
    {
        /* Already listening — re-arm uplink so a "dead" first session recovers. */
        write(1, "WAKE_RELISTEN\n", 14);
        if (protocol_ && audio_service_)
        {
            protocol_->SendStartListening(listening_mode_);
            audio_service_->SetTtsUplinkGate(false);
            if (!audio_service_->IsAudioProcessorRunning() && event_group_)
                xEventGroupSetBits(event_group_, MAIN_EVENT_LISTEN_VP);
        }
        ChatScreen::RefreshDeviceState();
    }
    else if (device_state_ == kDeviceStateActivating)
    {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::CompleteWakeFromWorker(bool channel_ok)
{
    write(1, channel_ok ? "WAKE_DONE_OK\n" : "WAKE_DONE_FAIL\n",
          channel_ok ? 13 : 15);

    if (!voice_ui_active_ || !protocol_)
    {
        wake_connect_inflight_ = false;
        if (protocol_)
            protocol_->ClearOpeningAudioGate();
        if (channel_ok && protocol_)
            protocol_->CloseAudioChannel();
        if (device_state_ == kDeviceStateConnecting)
            SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (device_state_ != kDeviceStateConnecting)
    {
        wake_connect_inflight_ = false;
        protocol_->ClearOpeningAudioGate();
        if (channel_ok)
            protocol_->CloseAudioChannel();
        ResumePendingBootWake();
        return;
    }

    if (!channel_ok)
    {
        wake_connect_inflight_ = false;
        protocol_->ClearOpeningAudioGate();
        if (TypedChat().active.load() && !TypedChat().pending.empty()) {
            TypedChat().pending.clear();
            if (auto *display=Board::GetInstance().GetDisplay())
                display->SetChatMessage("system","连接未成功，请检查网络后重新发送");
        }
        SetDeviceState(kDeviceStateIdle);
        if (voice_ui_active_)
            audio_service_->EnableWakeWordDetection(true);
        return;
    }

    /* Channel is up. Clear gates BEFORE listen/detect TX so the cloud's
     * immediate tts/stt replies are not MQTT_DROP / JSON_SKIP'd — that left
     * the UI stuck on「聆听中」with no greeting and no dialogue. */
    protocol_->ClearOpeningAudioGate();
    wake_connect_inflight_ = false;
    if (TypedChat().active.load() && !TypedChat().pending.empty()) SendPendingTypedChat();
    else EnterListeningAfterWake();
}

void Application::EnterListeningAfterWake()
{
    write(1, "WAKE_LISTEN\n", 12);
    ESP_LOGI(TAG, "Wake word detected: Hi openvela");

    /* Only AFE/Esp has wake Opus. Energy mute Encode+Pop hung first wake. */
    if (audio_service_ && audio_service_->IsAfeWakeWord())
    {
        while (auto packet = audio_service_->PopWakeWordPacket())
            protocol_->SendAudio(std::move(packet));
    }

    /* Leave Connecting BEFORE detect/start TX. While Connecting:
     *  - SetChatMessage drops bubbles
     *  - tts/start does not enter Speaking
     *  - UDP TTS frames are discarded
     * so the UI stays on「聆听中」with no text/voice. */
    {
        DeviceState previous_state = device_state_;
        clock_ticks_ = 0;
        aborted_ = false;
        device_state_ = kDeviceStateListening;
        DeviceStateEventManager::GetInstance().PostStateChangeEvent(
            previous_state, kDeviceStateListening);
        write(1, "LISTEN_ON\n", 10);
        if (ChatScreen::IsForeground())
            ChatScreen::RefreshDeviceState();
    }

    /* Device AEC → Realtime barge-in; else AutoStop half-duplex. */
    listening_mode_ = (aec_mode_ == kAecOff) ? kListeningModeAutoStop
                                             : kListeningModeRealtime;

    /* Claw4 order: detect/start first, then arm uplink.  Protect downlink
     * so EnableVP cannot ResetDecoder the greeting, and prime speaker TX
     * before the first Opus frame (first wake was text-only / silent). */
    if (audio_service_)
    {
        audio_service_->EnableWakeWordDetection(false);
        audio_service_->ArmDownlinkPlaybackProtect();
    }

    write(1, "WAKE_DET\n", 9);
    if (!protocol_->SendWakeWordDetected(HI_OPENVELA_PHRASE))
    {
        write(1, "WAKE_DET_RETRY\n", 15);
        usleep(50 * 1000);
        if (!protocol_->SendWakeWordDetected(HI_OPENVELA_PHRASE))
            write(1, "WAKE_DET_FAIL\n", 14);
        else
            write(1, "WAKE_DET_OK\n", 12);
    }
    else
    {
        write(1, "WAKE_DET_OK\n", 12);
    }

    write(1, "WAKE_START\n", 11);
    protocol_->SendStartListening(listening_mode_);
    write(1, "WAKE_TX_OK\n", 11);

    if (event_group_)
        xEventGroupSetBits(event_group_, MAIN_EVENT_LISTEN_VP);
}

void Application::AbortSpeaking(AbortReason reason)
{
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    tts_stop_pending_ = false;
    pending_tts_promote_.store(false, std::memory_order_release);
    if (audio_service_)
    {
        audio_service_->ResetDecoder();
        audio_service_->ClearDownlinkPlaybackProtect();
        audio_service_->SetTtsUplinkGate(false);
        audio_service_->DiscardUplinkAudio();
    }
    if (protocol_)
        protocol_->SendAbortSpeaking(reason);

    /* Interrupt(Realtime): after barge stop TTS and go Listening so the
     * user can keep talking without saying "Hi openvela" again.
     * AutoStop: park Idle「待唤醒」until the next wake. */
    if (device_state_ == kDeviceStateSpeaking && voice_ui_active_ &&
        voice_ui_request_.load(std::memory_order_acquire) != 0)
    {
        if (aec_mode_ == kAecOnDeviceSide)
        {
            listening_mode_ = kListeningModeRealtime;
            SetDeviceState(kDeviceStateListening);
            aborted_ = true;
            write(1, "ABORT_LISTEN\n", 13);
        }
        else
        {
            SetDeviceState(kDeviceStateIdle);
            aborted_ = true;
            write(1, "ABORT_IDLE\n", 11);
        }
    }
}

void Application::SetListeningMode(ListeningMode mode)
{
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state)
{
    if (device_state_ == state)
        return;

    clock_ticks_ = 0;
    DeviceState previous_state = device_state_;
    device_state_ = state;
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    const bool chat_fg = SystemChrome_ChatOwnsAssistantUi();
    const bool popup_was_open = SystemChrome_FloatAiOpen();

    switch (state)
    {
    case kDeviceStateUnknown:
    case kDeviceStateIdle:
        wake_connect_inflight_ = false;
        aborted_ = false;
        float_listen_idle_sec_ = 0;
        if (!chat_fg && SystemChrome_FloatAiOpen() &&
            !CameraScreen::IsLookAndSeeMode())
            SystemChrome_CloseFloatAi();
        if (audio_service_)
        {
            audio_service_->ClearDownlinkPlaybackProtect();
            audio_service_->SetTtsUplinkGate(false);
        }
        /* Never SetStatus/SetEmotion from this thread when !chat_fg:
         * after voice-open→calendar, SPEAK_TO / hard-release Idle raced
         * SwitchToHome into a solid-blue LVGL frame. Chat uses
         * RefreshDeviceState() below. */
        if (audio_service_)
        {
            audio_service_->EnableVoiceProcessing(false);
            audio_service_->ResetDecoder();
            /* Skip the arm while the float card is still on the top layer.
             * EnableWakeWord in the same refresh as that delete solid-blued
             * the desktop after a stuck「聆听中」session. */
            EnergyWakeWord::SetDesktopStrict(
                HomeScreen::IsActive() && !ChatScreen::IsForeground() &&
                !SystemChrome_FloatAiOpen());
            if (!skip_idle_wake_arm_ && !popup_was_open)
                RearmGlobalWakeWord();
            else if (!CanArmSpokenWake())
            {
                audio_service_->EnableWakeWordDetection(false);
                /* Do not ReleaseWakeWordEngine() on Idle: voice-open MCP
                 * hard-release (~2s later) was freeing the engine while the
                 * new app screen was live → solid-blue LVGL heap. Keep the
                 * engine warm; SoftStop already stopped detection. */
            }
            /* Wake session ended — Esp upgrade may resume safely. */
            audio_service_->SetWakeWordUpgradeAllowed(true);
            if (voice_ui_active_ && ChatScreen::IsForeground())
                ScheduleEspWakeWordUpgrade();
        }
        break;
    case kDeviceStateConnecting:
        if (audio_service_)
        {
            audio_service_->EnableWakeWordDetection(false);
            /* Arm before hello/UDP so early greeting frames are kept. */
            audio_service_->ArmDownlinkPlaybackProtect();
            // The wake worker primes TX after the previous media DMA drains.
        }
        break;
    case kDeviceStateListening:
        if (TypedChat().active.load()) {
            if (audio_service_) {
                audio_service_->EnableVoiceProcessing(false);
                audio_service_->EnableWakeWordDetection(false);
                audio_service_->DiscardUplinkAudio();
            }
            break;
        }
        /* Do not clear aborted_ here. AbortSpeaking→Listening must keep
         * rejecting leftover assistant TTS text/audio until STT arrives. */
        write(1, "LISTEN_ON\n", 10);
        if (audio_service_ && protocol_)
        {
            /* After Speaking: clear protect so TX can drop immediately, and
             * flush echo Opus before mic re-arms. */
            if (previous_state == kDeviceStateSpeaking)
            {
                audio_service_->ClearDownlinkPlaybackProtect();
                audio_service_->DiscardUplinkAudio();
                /* Drop leftover TTS before opening the gate. Clearing the
                 * gate first let echo Opus occupy the send queue and the
                 * header stayed on「聆听中」. */
                audio_service_->ResetDecoder();
                audio_service_->SetTtsUplinkGate(false);
            }
            if (!audio_service_->IsAudioProcessorRunning())
            {
                write(1, "LISTEN_TX\n", 10);
                protocol_->SendStartListening(listening_mode_);
                audio_service_->SetTtsUplinkGate(false);
                audio_service_->EnableWakeWordDetection(false);
                if (event_group_)
                    xEventGroupSetBits(event_group_, MAIN_EVENT_LISTEN_VP);
            }
            else
            {
                audio_service_->SetTtsUplinkGate(false);
            }
        }
        break;
    case kDeviceStateSpeaking:
        write(1, "SPEAK_ON\n", 9);
        if (audio_service_)
        {
            /* Always gate uplink during TTS — prevents speaker→STT 自问自答.
             * Stop VP so continuous mic RX does not chop TTS on shared I2S.
             * Realtime keeps input enabled for infrequent barge peeks. */
            audio_service_->SetTtsUplinkGate(true);
            audio_service_->EnableVoiceProcessing(false);
            if (!TypedChat().active.load() && audio_service_->IsBargeInEnabled() && !audio_service_->HasExclusiveCapture() &&
                metalio_nt26_get_call_state()==METALIO_CALL_IDLE) {
                if (auto *c = audio_service_->GetCodec()) c->EnableInput(true);
            }
            /* No device AEC here. Realtime would keep the mic DMA reading
             * during TTS and chop playback on the shared I2S. */
            if (listening_mode_ == kListeningModeRealtime &&
                !audio_service_->SupportsDeviceAec())
                listening_mode_ = kListeningModeAutoStop;
            if (listening_mode_ != kListeningModeRealtime)
            {
                audio_service_->EnableWakeWordDetection(
                    voice_ui_active_ && audio_service_->IsAfeWakeWord() &&
                    audio_service_->SupportsDeviceAec());
            }
            else
            {
                audio_service_->EnableWakeWordDetection(false);
                /* Keep RX powered so MaybeDetectBargeIn need not flip
                 * EnableInput mid-TTS (that glitched playback). */
                if (auto *c = audio_service_->GetCodec())
                    c->EnableInput(true);
            }
            audio_service_->DiscardUplinkAudio();
            /* Never ResetDecoder here: early UDP greeting is often already
             * queued while still Listening — wiping it → first wake text-only. */
        }
        break;
    default:
        break;
    }

    /* Fast chat header update (连接中/聆听中/讲话中/待唤醒). Async-only —
     * never touch LVGL labels from this thread (that blue-flashed wake). */
    if (chat_fg &&
        (state == kDeviceStateConnecting || state == kDeviceStateListening ||
         state == kDeviceStateSpeaking || state == kDeviceStateIdle))
    {
        ChatScreen::RefreshDeviceState();
    }
    if (SystemChrome_FloatAiOpen() &&
        (state == kDeviceStateConnecting || state == kDeviceStateListening ||
         state == kDeviceStateSpeaking || state == kDeviceStateIdle))
    {
        SystemChrome_FloatAiSyncState();
    }
}

void Application::Reboot()
{
    ControlCenter_SaveBeforePowerOff();
    write(1, "REBOOT\n", 7);
    ESP_LOGI(TAG, "Rebooting...");
    /* Dim backlight before reset to avoid transitional 花屏; do not
     * persist 0% to NVS (fade/permanent=false). */
    if (Backlight *bl = Board::GetInstance().GetBacklight())
        bl->SetBrightness(0, false);

    /* Do NOT CloseAudioChannel / protocol_.reset / join threads here —
     * any hang leaves a black panel with no chip reset. Hardware reset
     * tears everything down. */
    usleep(80 * 1000);
    esp_restart();
}

void Application::OnWifiLinkReady()
{
    /* Run on the main event loop so we do not tear down MQTT from the
     * WiFi connect worker while Schedule/OTA workers may also touch it. */
    Schedule([this]() {
        write(1, "WIFI_UP\n", 8);
        ESP_LOGE(TAG, "OnWifiLinkReady: new STA+DHCP lease");
        if (protocol_)
            voice_network_preparing_.store(protocol_->OnNetworkReady());

        /* Prefer OTA server_time (Claw4); NTP fills in if OTA has not. */
        if (!has_server_time_)
            SyncWallClockFromNtp();
        RefreshTimeUi();

        /* Always poke the in-flight OTA worker (if any). */
        wifi_recheck_requested_ = true;

        if (!boot_ready_)
        {
            ESP_LOGE(TAG, "OnWifiLinkReady: wake in-flight OTA/activation");
            return;
        }

        /* Boot OTA already finished (often after DNS failures on the
         * Kconfig SSID). Always re-run CheckVersion so an unbound device
         * can mint the 6-digit xiaozhi.me code on the new network. */
        // A data-path switch does not invalidate an already bound device.
        // Keep activation polling from stealing state / bandwidth from the
        // voice connection while NT26 acquires its new route.
        Settings binding("wifi", false);
        if (binding.GetInt("xiaozhi_bound", 0) != 0 && !HasPendingActivation())
            return;

        ESP_LOGE(TAG, "OnWifiLinkReady: forcing OTA recheck for activation");
        StartXiaozhiOtaRecheck();
    });
}

void Application::StartXiaozhiOtaRecheck()
{
    if (ota_recheck_inflight_)
    {
        ESP_LOGW(TAG, "OTA recheck already in flight");
        return;
    }
    ota_recheck_inflight_ = true;

    auto *ota = new Ota();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 131072);
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = CONFIG_METALIO_APP_PRIORITY - 10;
    if (sp.sched_priority < 1)
        sp.sched_priority = 1;
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    pthread_t th;
    int rc = hi_task_create_legacy(
        &th, &attr,
        [](void *p) -> void * {
            Application::GetInstance().OtaRecheckWorker(static_cast<Ota *>(p));
            return nullptr;
        },
        ota);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "OTA recheck create failed (%d)", rc);
        delete ota;
        ota_recheck_inflight_ = false;
        return;
    }
    pthread_detach(th);
}

void Application::OtaRecheckWorker(Ota *ota)
{
    ESP_LOGE(TAG, "OTA recheck after WiFi change");
    write(1, "OTA_RE0\n", 8);
    CheckNewVersion(*ota);
    write(1, "OTA_RE1\n", 8);

    /* Keep `ota` alive until the main loop finishes InitializeXiaozhiProtocol. */
    Schedule([this, ota]() {
        ota_recheck_inflight_ = false;
        ESP_LOGE(TAG, "OTA recheck done (mqtt=%d ws=%d pending=%d)",
                 ota->HasMqttConfig() ? 1 : 0,
                 ota->HasWebsocketConfig() ? 1 : 0,
                 HasPendingActivation() ? 1 : 0);

        if (!HasPendingActivation())
        {
            // Configuration is already persisted. Keep an active dialogue's
            // protocol alive; replacing it races the wake worker and kills RX.
            if (protocol_ && (wake_connect_inflight_ ||
                device_state_ == kDeviceStateConnecting ||
                device_state_ == kDeviceStateListening ||
                device_state_ == kDeviceStateSpeaking))
            {
                delete ota;
                return;
            }
            if (protocol_ && ota->HasMqttConfig() && protocol_->OnNetworkReady())
            {
                boot_ready_ = true;
                has_server_time_ = ota->HasServerTime();
                ResumePendingBootWake();
                if (voice_ui_desired_ || ChatScreen::IsForeground()) SyncVoiceUiSession();
                delete ota;
                return;
            }
            if (protocol_ && protocol_->IsAudioChannelOpened())
                protocol_->CloseAudioChannel();
            protocol_.reset();
            InitializeXiaozhiProtocol(*ota);
            if (protocol_ && !protocol_->Start())
                ESP_LOGW(TAG, "OTA recheck: protocol Start() failed");
            boot_ready_ = true;
            device_state_ = kDeviceStateIdle;
            ResumePendingBootWake();
            has_server_time_ = ota->HasServerTime();
            if (!has_server_time_)
                SyncWallClockFromNtp();
            HomeScreen::RefreshStatusBar();
        }
        else
        {
            /* Still unbound — code is on the home bar; user must bind at
             * xiaozhi.me. Another WiFi change can call OnWifiLinkReady again. */
            ESP_LOGE(TAG, "OTA recheck: activation code still pending");
            if (ota->HasServerTime())
                has_server_time_ = true;
            else
                SyncWallClockFromNtp();
            HomeScreen::RefreshStatusBar();
        }
        if (voice_ui_desired_ || ChatScreen::IsForeground())
        {
            if (ChatScreen::IsForeground())
                voice_ui_desired_ = true;
            SyncVoiceUiSession();
        }
        delete ota;
    });
}

bool Application::UpgradeFirmware(Ota &ota, const std::string &url)
{
    auto &board = Board::GetInstance();
    auto *display = board.GetDisplay();

    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string version_info = url.empty() ? ota.GetFirmwareVersion() : "(Manual upgrade)";

    if (protocol_ && protocol_->IsAudioChannelOpened())
    {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    SetDeviceState(kDeviceStateUpgrading);

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING,
          "download", Lang::Sounds::OGG_UPGRADE);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (display)
    {
        std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
        display->SetChatMessage("system", message.c_str());
    }

    audio_service_->Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    bool upgrade_success = ota.StartUpgradeFromUrl(upgrade_url,
        [](int progress, size_t downloaded, size_t total, size_t speed) {
            ESP_LOGI(TAG, "Upgrade %d%% (%u/%u) %uB/s",
                     progress, (unsigned)downloaded, (unsigned)total, (unsigned)speed);
        });

    if (!upgrade_success)
    {
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service...");
        audio_service_->Start();
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED,
              "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
    if (display)
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    Reboot();
    return true;
}

void Application::WakeWordInvoke(const std::string &wake_word)
{
    if (!CanArmSpokenWake()) return;
    if (wake_word != HI_OPENVELA_PHRASE) return;
    if (!voice_ui_active_ || !protocol_)
        return;

    if (device_state_ == kDeviceStateIdle)
    {
        audio_service_->EncodeWakeWord();

        SetDeviceState(kDeviceStateConnecting);
        if (!protocol_->OpenAudioChannel())
        {
            SetDeviceState(kDeviceStateIdle);
            if (voice_ui_active_)
                audio_service_->EnableWakeWordDetection(true);
            return;
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        protocol_->SendWakeWordDetected(HI_OPENVELA_PHRASE);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop
                                              : kListeningModeRealtime);
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    }
    else if (device_state_ == kDeviceStateListening)
    {
        Schedule([this]() {
            if (protocol_)
                protocol_->CloseAudioChannel();
        });
    }
}

bool Application::CanEnterSleepMode()
{
    if (device_state_ != kDeviceStateIdle)
        return false;
    if (protocol_ && protocol_->IsAudioChannelOpened())
        return false;
    if (!audio_service_->IsIdle())
        return false;
    return true;
}

void Application::SendMcpMessage(const std::string &payload)
{
    if (protocol_ == nullptr)
        return;
    /* Publish immediately on the main loop — Schedule(std::function) hung on
     * uClibc++ and the cloud never received tool results for volume/蓝牙/开应用. */
    protocol_->SendMcpMessage(payload);
}

void Application::SetAecMode(AecMode mode)
{
    if (!audio_service_) return;
    const bool want_interrupt = mode == kAecOnDeviceSide;
    const bool actual_aec = want_interrupt && audio_service_->SupportsDeviceAec();
    if (audio_service_->IsBargeInEnabled() == want_interrupt &&
        aec_mode_ == (actual_aec ? kAecOnDeviceSide : kAecOff)) return;
    { Settings settings("audio", true); settings.SetBool("interrupt", want_interrupt); }
    // Do not pretend that NoAudioProcessor supplies AEC. Barge-in uses the
    // local gated-microphone path; server listening remains AutoStop.
    aec_mode_ = actual_aec ? kAecOnDeviceSide : kAecOff;
    audio_service_->EnableDeviceAec(actual_aec);
    audio_service_->EnableBargeIn(want_interrupt && !TypedChat().active.load());
    Schedule([want_interrupt]() {
        if (auto *display = Board::GetInstance().GetDisplay())
            display->ShowNotification(want_interrupt ? "打断已开启" : "打断已关闭");
    });
}

void Application::ApplyInterruptPreferenceFromNvs()
{
    Settings settings("audio");
    const bool interrupt_on = settings.GetBool("interrupt", false);
    const bool actual_aec = interrupt_on && audio_service_ && audio_service_->SupportsDeviceAec();
    aec_mode_ = actual_aec ? kAecOnDeviceSide : kAecOff;
    if (audio_service_) {
        audio_service_->EnableDeviceAec(actual_aec);
        audio_service_->EnableBargeIn(interrupt_on && !TypedChat().active.load());
    }
    ESP_LOGI(TAG, "interrupt preference: %s", interrupt_on ? "on" : "off");
}

void Application::PlaySound(const std::string_view &sound)
{
    if (activation_suspended_)
        return;
    audio_service_->PlaySound(sound);
}

/* ------------------------------------------------------------------ */
/* Voice UI session management                                         */
/* ------------------------------------------------------------------ */

void Application::SoftStopVoiceAudioPaths()
{
    if (audio_service_)
    {
        audio_service_->ClearDownlinkPlaybackProtect();
        audio_service_->EnableWakeWordDetection(false);
        audio_service_->EnableVoiceProcessing(false);
        if (!audio_service_->HasLocalPlaybackSession() && !audio_service_->HasExclusiveCapture())
            if (auto *c = audio_service_->GetCodec())c->EnableOutput(false);
    }
}

void Application::TearDownVoiceAudioPaths(bool release_wake_word)
{
    SoftStopVoiceAudioPaths();
    if (release_wake_word)
        audio_service_->ReleaseWakeWordEngine();
}

void Application::CancelVoiceUiHardRelease()
{
    if (voice_ui_release_timer_ != nullptr)
        esp_timer_stop(voice_ui_release_timer_);
}

void Application::ScheduleVoiceUiHardRelease(uint32_t epoch)
{
    CancelVoiceUiHardRelease();
    voice_ui_pending_release_epoch_ = epoch;

    if (voice_ui_release_timer_ == nullptr)
    {
        esp_timer_create_args_t args = {
            .callback =
                [](void *arg) {
                    auto *app = static_cast<Application *>(arg);
                    const uint32_t ep = app->voice_ui_pending_release_epoch_;
                    app->Schedule([app, ep]() {
                        if (app->voice_ui_desired_ || app->voice_ui_epoch_ != ep)
                            return;
                        ESP_LOGI(TAG, "voice UI delayed hard release (epoch=%" PRIu32 ")", ep);
                        app->ApplyVoiceUiStop();
                    });
                },
            .arg = this,
        };
        esp_timer_create(&args, &voice_ui_release_timer_);
    }
    constexpr uint64_t kReleaseDelayUs = 2000 * 1000;
    esp_timer_start_once(voice_ui_release_timer_, kReleaseDelayUs);
}

void Application::ScheduleVoiceUiStartRetry(uint32_t epoch)
{
    voice_ui_pending_retry_epoch_ = epoch;
    if (voice_ui_start_retry_timer_ == nullptr)
    {
        esp_timer_create_args_t args = {
            .callback =
                [](void *arg) {
                    auto *app = static_cast<Application *>(arg);
                    const uint32_t ep = app->voice_ui_pending_retry_epoch_;
                    app->Schedule([app, ep]() {
                        if (!app->voice_ui_desired_ || app->voice_ui_epoch_ != ep)
                            return;
                        app->ApplyVoiceUiStart();
                    });
                },
            .arg = this,
        };
        esp_timer_create(&args, &voice_ui_start_retry_timer_);
    }
    else
    {
        esp_timer_stop(voice_ui_start_retry_timer_);
    }
    esp_timer_start_once(voice_ui_start_retry_timer_, 200 * 1000);
}

bool Application::TryEnableWakeWordForVoiceUi()
{
    if (!CanArmSpokenWake())
    {
        if (audio_service_) audio_service_->EnableWakeWordDetection(false);
        return true; // User disabled recognition; do not retry indefinitely.
    }
    if (device_state_ == kDeviceStateConnecting ||
        device_state_ == kDeviceStateListening ||
        device_state_ == kDeviceStateSpeaking)
        return true;

    /* VP must be off — EnableWakeWordDetection returns WW_BLOCK otherwise,
     * while IsWakeWordEngineReady() stays true (false "success"). */
    if (audio_service_->IsAudioProcessorRunning())
        audio_service_->EnableVoiceProcessing(false);

    /* Idle chat has no TTS. Stale DL protect / ring mute used to eat every
     * Energy feed (ShouldSuppressWakeWord) so「Hi openvela」never fired. */
    audio_service_->ClearWakeWordMute();
    audio_service_->ClearDownlinkPlaybackProtect();

    /* Arm Energy immediately; EspWakeWord swaps in after chat paint settles
     * so the first「Hi openvela」works without racing CreateStatic (solid blue). */
    EnergyWakeWord::SetDesktopStrict(
        HomeScreen::IsActive() && !ChatScreen::IsForeground() &&
        !SystemChrome_FloatAiOpen());
    audio_service_->EnableWakeWordDetection(true);
    if (!audio_service_->IsAfeWakeWord())
        ScheduleEspWakeWordUpgrade();
    else
        write(1, "WW_ESP_HOT\n", 11);
    if (!audio_service_->IsWakeWordRunning())
    {
        ESP_LOGW(TAG, "Wake word not running after enable, will retry");
        return false;
    }
    return true;
}

void Application::CancelEspWakeWordUpgrade()
{
    if (esp_wake_upgrade_timer_ != nullptr)
        esp_timer_stop(esp_wake_upgrade_timer_);
}

void Application::CancelEspWakeWordPreload()
{
    if (esp_wake_preload_timer_ != nullptr)
        esp_timer_stop(esp_wake_preload_timer_);
}

void Application::ScheduleEspWakeWordPreload()
{
    /* Never preload Esp on home — WakeNet create racing home LVGL
     * solid-blues the panel. Chat ScheduleEspWakeWordUpgrade owns it. */
    CancelEspWakeWordPreload();
    write(1, "WW_PRE_OFF\n", 11);
}

void Application::ScheduleEspWakeWordUpgrade()
{
    /* Disabled on this NuttX port: Esp/WakeNet create (EW_CR1 / WW_ESP_TRY)
     * during chat — especially the first wake — solid-blues LVGL.
     * Voice wake uses EnergyWakeWord (reports「Hi openvela」); tap「待唤醒」
     * still opens the session via RequestWakeSession. */
    CancelEspWakeWordUpgrade();
    write(1, "WW_ESP_NOP\n", 11);
}

void Application::SetVoiceUiDesired(bool desired)
{
    if (voice_ui_desired_ == desired)
    {
        /* Re-enter chat while desired stuck true but WW not armed yet.
         * Inline — Schedule(std::function) has hung after chat enter. */
        if (desired && !voice_ui_active_)
        {
            write(1, "VUI_RESYNC\n", 11);
            SyncVoiceUiSession();
        }
        else if (desired && voice_ui_active_ &&
                 device_state_ == kDeviceStateIdle && audio_service_ &&
                 !audio_service_->IsWakeWordRunning())
        {
            write(1, "VUI_WW_FIX\n", 11);
            TryEnableWakeWordForVoiceUi();
        }
        return;
    }
    voice_ui_desired_ = desired;
    ++voice_ui_epoch_;
    const uint32_t epoch = voice_ui_epoch_;
    write(1, desired ? "VUI_DES1\n" : "VUI_DES0\n", 9);

    if (!desired)
    {
        if (TypedChat().active.exchange(false)) ApplyInterruptPreferenceFromNvs();
        TypedChat().pending.clear();TypedChat().last.clear();
        voice_ui_active_ = false;
        wake_connect_inflight_ = false;
        float_listen_idle_sec_ = 0;
        if (!ChatScreen::IsForeground() && SystemChrome_FloatAiOpen())
            SystemChrome_CloseFloatAi();
        /* Soft-stop immediately. Do NOT SetDeviceState(Idle) here:
         * Idle→ReleaseWakeWordEngine races LVGL chat→app swaps from
         * voice MCP open (slow / APP_NULL / solid blue). CloseAudioChannel
         * + Idle stay on the delayed hard release. */
        SoftStopVoiceAudioPaths();
        if (audio_service_)
            audio_service_->EnableWakeWordDetection(false);
        CancelEspWakeWordUpgrade();
        if (voice_ui_start_retry_timer_ != nullptr)
            esp_timer_stop(voice_ui_start_retry_timer_);
        if (device_state_ == kDeviceStateSpeaking)
            AbortSpeaking(kAbortReasonNone);
        ScheduleVoiceUiHardRelease(epoch);
        return;
    }

    CancelVoiceUiHardRelease();
    /* Run inline: SetVoiceUiDesired is only entered from the Application
     * thread after RequestVoiceUiDesired. Schedule(std::function) after chat
     * enter was hanging (no VUI_START/WW_ON) — likely heap alloc under load. */
    write(1, "VUI_SYNC\n", 9);
    SyncVoiceUiSession();
}

void Application::RequestVoiceUiDesired(bool desired)
{
    /* Safe on the LVGL thread: no mutex, no std::function, no heap. */
    voice_ui_request_.store(desired ? 1 : 0, std::memory_order_release);
    write(1, desired ? "VUI_REQ1\n" : "VUI_REQ0\n", 9);
    if (event_group_)
        xEventGroupSetBits(event_group_, MAIN_EVENT_VOICE_UI);
}

bool Application::CanArmSpokenWake() const
{
    return IsSpokenWakeEnabled() && audio_service_ &&
        !audio_service_->HasExclusiveCapture() &&
        metalio_nt26_get_call_state() == METALIO_CALL_IDLE;
}

void Application::RequestSpokenWakeEnabled(bool enabled)
{
    spoken_wake_enabled_.store(enabled, std::memory_order_release);
    if (event_group_) xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_PREFERENCE);
}

void Application::ParkVoiceUiProtocol()
{
    if (device_state_ == kDeviceStateListening && protocol_)
        protocol_->CloseAudioChannel();
    else if (device_state_ == kDeviceStateSpeaking)
    {
        AbortSpeaking(kAbortReasonNone);
        if (protocol_ && protocol_->IsAudioChannelOpened())
            protocol_->CloseAudioChannel();
    }
    else if (protocol_ && protocol_->IsAudioChannelOpened())
        protocol_->CloseAudioChannel();

    if (device_state_ == kDeviceStateListening ||
        device_state_ == kDeviceStateSpeaking ||
        device_state_ == kDeviceStateConnecting)
    {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::SyncVoiceUiSession()
{
    if (voice_ui_desired_)
    {
        ApplyVoiceUiStart();
        if (!voice_ui_desired_)
        {
            SoftStopVoiceAudioPaths();
            voice_ui_active_ = false;
            ScheduleVoiceUiHardRelease(voice_ui_epoch_);
        }
    }
    else
    {
        voice_ui_active_ = false;
        SoftStopVoiceAudioPaths();
        ParkVoiceUiProtocol();
    }
}

void Application::ApplyVoiceUiStart()
{
    write(1, "VUI_AS0\n", 8);
    /* Do not clear wake_connect_inflight_ during Connecting — chat VUI
     * re-arm used to drop the flag and leave the header on「连接中」. */
    if (device_state_ != kDeviceStateConnecting &&
        device_state_ != kDeviceStateListening &&
        device_state_ != kDeviceStateSpeaking)
        wake_connect_inflight_ = false;

    if (!voice_ui_desired_)
    {
        SoftStopVoiceAudioPaths();
        voice_ui_active_ = false;
        return;
    }

    const uint32_t epoch = voice_ui_epoch_;

    /* Do not clobber an active dialogue session. */
    if (device_state_ == kDeviceStateListening ||
        device_state_ == kDeviceStateSpeaking ||
        device_state_ == kDeviceStateConnecting)
    {
        voice_ui_active_ = true;
        write(1, "VUI_BUSY\n", 9);
        return;
    }

    const bool already_active = voice_ui_active_;
    voice_ui_active_ = true;

    if (device_state_ == kDeviceStateActivating ||
        device_state_ == kDeviceStateStarting ||
        device_state_ == kDeviceStateWifiConfiguring ||
        device_state_ == kDeviceStateAudioTesting ||
        device_state_ == kDeviceStateUpgrading)
    {
        write(1, "VUI_WAIT\n", 9);
        ScheduleVoiceUiStartRetry(epoch);
        return;
    }

    if (device_state_ != kDeviceStateIdle)
        SetDeviceState(kDeviceStateIdle);

    if (device_state_ != kDeviceStateIdle)
        return;

    if (!boot_ready_ || protocol_ == nullptr)
    {
        write(1, "VUI_WAIT\n", 9);
        ScheduleVoiceUiStartRetry(epoch);
        return;
    }

    write(1, "VUI_AS1\n", 8);
    /* Keep voice processor OFF while chat is Idle. Enabling it here used to
     * ReleaseWakeWordEngine() + AfeAudioProcessor::Initialize under LVGL
     * pressure (solid blue right after entering 聊天). Wake word only until
     * the first wake→Listening transition starts the uplink processor. */
    audio_service_->EnableVoiceProcessing(false);
    write(1, "VUI_AS2\n", 8);
    if (!TryEnableWakeWordForVoiceUi())
    {
        /* Keep voice_ui_active_ so tap「待唤醒」can still open the channel
         * on the first try (WW retry must not gate VUI). */
        write(1, "VUI_RETRY\n", 10);
        ScheduleVoiceUiStartRetry(epoch);
        return;
    }
    if (!voice_ui_desired_)
    {
        SoftStopVoiceAudioPaths();
        voice_ui_active_ = false;
        return;
    }

    if (!already_active)
    {
        write(1, "VUI_START\n", 10);
        ESP_LOGI(TAG, "ApplyVoiceUiStart");
    }
}

void Application::ApplyVoiceUiStop()
{
    wake_connect_inflight_ = false;
    voice_ui_active_ = false;
    float_listen_idle_sec_ = 0;
    const bool popup_was_open = SystemChrome_FloatAiOpen();
    if (!ChatScreen::IsForeground() && popup_was_open)
        SystemChrome_CloseFloatAi();
    /* Soft teardown only — keep EnergyWakeWord initialized so chat re-enter
     * does not re-allocate and blue-screen the LVGL heap. */
    TearDownVoiceAudioPaths(false);
    ParkVoiceUiProtocol();
    // This delayed cleanup belongs to the closed conversation, not to the
    // global recognizer. Do not stop it again after Idle has re-armed it.
    // If a popup was just deleted, the quiet clock tick re-arms it instead.
    if (!popup_was_open) RearmGlobalWakeWord();
    write(1, "VUI_STOP\n", 9);
    ESP_LOGI(TAG, "ApplyVoiceUiStop: voice paths parked (WW kept)");
}

void Application::RearmGlobalWakeWord()
{
    // Page-independent local recognition. Reuse the warm engine only; never
    // construct a model, reopen the cloud or renegotiate audio on a UI swap.
    if (device_state_ != kDeviceStateIdle || !CanArmSpokenWake() ||
        wake_connect_inflight_ || skip_idle_wake_arm_ ||
        SystemChrome_FloatAiOpen() || !audio_service_->IsWakeWordEngineReady() ||
        audio_service_->HasLocalPlaybackSession() ||
        RadioPlayer::Instance().want_play() || audio_service_->HasPendingPlayback() ||
        McpServer::GetInstance().IsVisionAnalysisPending()) return;
    EnergyWakeWord::SetDesktopStrict(
        HomeScreen::IsActive() && !ChatScreen::IsForeground());
    if (audio_service_->IsAudioProcessorRunning())
        audio_service_->EnableVoiceProcessing(false);
    audio_service_->SetTtsUplinkGate(false);
    audio_service_->ClearDownlinkPlaybackProtect();
    audio_service_->EnableWakeWordDetection(true);
}

/* ------------------------------------------------------------------ */
/* Xiaozhi protocol (MQTT or WebSocket, chosen by OTA config)          */
/* ------------------------------------------------------------------ */

void Application::InitializeXiaozhiProtocol(Ota &ota)
{
    const auto protocol_generation = ++protocol_generation_;
    if (ota.HasMqttConfig())
    {
        ESP_LOGI(TAG, "Using MQTT protocol from xiaozhi.me OTA config");
        protocol_ = std::make_unique<MqttProtocol>();
    }
    else if (ota.HasWebsocketConfig())
    {
        ESP_LOGI(TAG, "Using WebSocket protocol from xiaozhi.me OTA config");
        protocol_ = std::make_unique<WebsocketProtocol>();
    }
    else
    {
        ESP_LOGW(TAG, "No protocol in OTA config, falling back to MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this, protocol_generation]() {
        // Match OnDisconnected's event-loop ownership. A fast reconnect can
        // otherwise clear preparing here before the old disconnect's queued
        // task runs, which sets it back forever despite a live new socket.
        Schedule([this, protocol_generation]() {
            if (protocol_generation_.load() != protocol_generation) return;
            voice_network_preparing_.store(false);
            DismissAlert();
            ResumePendingBootWake();
        });
    });
    protocol_->OnDisconnected([this, protocol_generation]() {
        Schedule([this, protocol_generation]() {
            if (protocol_generation_.load() != protocol_generation) return;
            voice_network_preparing_.store(true);
            if (device_state_ == kDeviceStateListening ||
                device_state_ == kDeviceStateSpeaking ||
                device_state_ == kDeviceStateConnecting)
            {
                // Retain a real wake that arrived during network preparation.
                if (!TypedChat().active.load() && wake_connect_inflight_ && device_state_ == kDeviceStateConnecting)
                    pending_boot_wake_.store(true, std::memory_order_release);
                // A disconnected cloud socket must not leave a live-listen UI.
                SetDeviceState(kDeviceStateIdle);
            }
        });
    });
    protocol_->OnNetworkError([this](const std::string &message) {
        last_error_message_ = message;
        if (event_group_)
            xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        /* Speaking/Connecting: normal TTS. Listening: Opus often arrives
         * before tts/start JSON is drained (JSON_Q) — dropping left
         * text-only bubbles and felt like delayed/missing feedback. */
        if (aborted_ || !audio_service_)
            return;
        const DeviceState st = device_state_;
        if (st == kDeviceStateSpeaking || st == kDeviceStateConnecting)
        {
            audio_service_->PushPacketToDecodeQueue(std::move(packet));
            return;
        }
        if (st == kDeviceStateListening && voice_ui_desired_)
        {
            audio_service_->SetTtsUplinkGate(true);
            audio_service_->ArmDownlinkPlaybackProtect();
            audio_service_->PushPacketToDecodeQueue(std::move(packet));
            if (!pending_tts_promote_.exchange(true, std::memory_order_acq_rel) &&
                event_group_)
                xEventGroupSetBits(event_group_, MAIN_EVENT_TTS_PROMOTE);
        }
    });
    protocol_->OnAudioChannelClosed([this]() {
        /* Never tear down UI/state while a wake open is in flight — CH_CLOSE
         * mid-wake has raced Connecting and blue-screened chat. */
        if (wake_connect_inflight_ ||
            device_state_ == kDeviceStateConnecting)
        {
            write(1, "CH_CLOSE_IGN\n", 13);
            return;
        }
        Schedule([this]() {
            if (wake_connect_inflight_ ||
                device_state_ == kDeviceStateConnecting)
                return;
            auto *disp = Board::GetInstance().GetDisplay();
            if (disp)
                disp->SetChatMessage("system", "");
            wake_connect_inflight_ = false;
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this](const cJSON *root) {
        /* MQTT RX thread: never Schedule(std::function) here — that hung
         * the port right after MQTT_IN stt (wake → listen stuck).
         * Skip non-MCP while OpenAudioChannel is still running. MCP for
         * 音量/背光/蓝牙/开应用 must still enqueue (same as MQTT layer). */
        if (wake_connect_inflight_ &&
            device_state_ == kDeviceStateConnecting)
        {
            auto *type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type) &&
                strcmp(type->valuestring, "hello") == 0)
            {
                /* Server hello must reach the MQTT waiter — skipping it
                 * left the chat header on「连接中」until timeout. */
            }
            else if (!cJSON_IsString(type) ||
                strcmp(type->valuestring, "mcp") != 0)
            {
                write(1, "JSON_SKIP\n", 10);
                return;
            }
            else
                write(1, "JSON_MCP\n", 9);
        }
        if (!EnqueueXiaozhiJson(root))
            write(1, "JSON_DROP\n", 10);
    });

    bool started = protocol_->Start();
    ESP_LOGI(TAG, "Xiaozhi protocol Start() -> %s", started ? "ok" : "failed");

    /* Allocate the 96KB wake OpenAudioChannel worker now (heap still free),
     * before Chat LVGL objects fill the arena. */
    EnsureWakeOpenWorker();
}

bool Application::EnqueueXiaozhiJson(const cJSON *root)
{
    if (root == nullptr)
        return false;

    char *printed = cJSON_PrintUnformatted(root);
    if (printed == nullptr)
        return false;

    const uint32_t head = s_xiaozhi_json_head.load(std::memory_order_relaxed);
    const uint32_t tail = s_xiaozhi_json_tail.load(std::memory_order_acquire);
    if ((head - tail) >= (uint32_t)kXiaozhiJsonQ)
    {
        cJSON_free(printed);
        return false;
    }

    s_xiaozhi_json_q[head % (uint32_t)kXiaozhiJsonQ] = printed;
    s_xiaozhi_json_head.store(head + 1, std::memory_order_release);
    write(1, "JSON_Q\n", 7);
    if (event_group_)
        xEventGroupSetBits(event_group_, MAIN_EVENT_XIAOZHI_JSON);
    return true;
}

void Application::DrainXiaozhiJsonQueue()
{
    auto *display = Board::GetInstance().GetDisplay();
    for (;;)
    {
        const uint32_t tail = s_xiaozhi_json_tail.load(std::memory_order_relaxed);
        const uint32_t head = s_xiaozhi_json_head.load(std::memory_order_acquire);
        if (tail == head)
            break;

        char *printed = s_xiaozhi_json_q[tail % (uint32_t)kXiaozhiJsonQ];
        s_xiaozhi_json_q[tail % (uint32_t)kXiaozhiJsonQ] = nullptr;
        s_xiaozhi_json_tail.store(tail + 1, std::memory_order_release);

        if (printed == nullptr)
            continue;

        write(1, "JSON_DRV\n", 9);
        cJSON *root = cJSON_Parse(printed);
        cJSON_free(printed);
        if (root == nullptr)
            continue;
        HandleIncomingXiaozhiJson(display, root);
        cJSON_Delete(root);
    }
}

void Application::FinishTtsStop()
{
    tts_stop_pending_ = false;
    if (device_state_ != kDeviceStateSpeaking)
    {
        /* Late tts/stop outside Speaking: drop the gate. The Speaking →
         * Listening handoff clears it only after the decoder is reset. */
        if (audio_service_)
        {
            audio_service_->ClearDownlinkPlaybackProtect();
            audio_service_->SetTtsUplinkGate(false);
        }
        return;
    }
    if (audio_service_)
        audio_service_->ClearDownlinkPlaybackProtect();
    /* Match Claw4 tts/stop: ManualStop → Idle「待唤醒」; otherwise Listening
     * so「聆听中」returns after「讲话中」. Left-chat still parks Idle. */
    if (TypedChat().active.load() || listening_mode_ == kListeningModeManualStop ||
        voice_ui_request_.load(std::memory_order_acquire) == 0)
        SetDeviceState(kDeviceStateIdle);
    else
        SetDeviceState(kDeviceStateListening);
}

void Application::HandleIncomingXiaozhiJson(Display *display, const cJSON *root)
{
    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type))
        return;

    if (strcmp(type->valuestring, "tts") == 0)
    {
        auto state = cJSON_GetObjectItem(root, "state");
        if (!cJSON_IsString(state))
            return;
        if (strcmp(state->valuestring, "start") == 0)
        {
            /* New assistant turn (Claw4): clear barge abort so voice feedback
             * plays. Ignoring tts/start while aborted_ stuck UI on 聆听中. */
            write(1, "TTS_START\n", 10);
            aborted_ = false;
            tts_stop_pending_ = false;
            /* Re-arm gate for TTS (both AutoStop and Realtime). Realtime
             * barges in locally; uplink stays muted until Listening. */
            if (audio_service_)
            {
                if (!audio_service_->HasPendingPlayback())
                    audio_service_->BeginWakeGreetingPlayback();
                audio_service_->SetTtsUplinkGate(true);
                audio_service_->ArmDownlinkPlaybackProtect();
                /* Prime TX early — waiting for first PCM to EnableOutput
                 * added a noticeable gap before voice feedback. */
                if (auto *c = audio_service_->GetCodec())
                    c->EnableOutput(true);
            }
            if (device_state_ == kDeviceStateIdle ||
                device_state_ == kDeviceStateListening ||
                device_state_ == kDeviceStateConnecting)
            {
                SetDeviceState(kDeviceStateSpeaking);
            }
        }
        else if (strcmp(state->valuestring, "stop") == 0)
        {
            write(1, "TTS_STOP\n", 9);
            if (audio_service_ && !aborted_)
                audio_service_->NotifyTtsStreamEnded();
            if (device_state_ == kDeviceStateSpeaking)
            {
                /* PCM may still be draining; clearing protect/gate here
                 * cut the tail and raced the next sentence into text-only. */
                if (audio_service_ && audio_service_->HasPendingPlayback() &&
                    !aborted_)
                {
                    tts_stop_pending_ = true;
                }
                else
                {
                    FinishTtsStop();
                }
            }
            else if (!TypedChat().active.load() && device_state_ == kDeviceStateListening && audio_service_ &&
                     protocol_ && !aborted_ &&
                     voice_ui_request_.load(std::memory_order_acquire) != 0)
            {
                /* tts/stop after we already recovered via SPEAK_TO — still
                 * re-arm listen so the next user turn is not dropped. */
                write(1, "LISTEN_TX\n", 10);
                protocol_->SendStartListening(listening_mode_);
                audio_service_->SetTtsUplinkGate(false);
                if (!audio_service_->IsAudioProcessorRunning() && event_group_)
                    xEventGroupSetBits(event_group_, MAIN_EVENT_LISTEN_VP);
            }
        }
        else if (strcmp(state->valuestring, "sentence_start") == 0)
        {
            /* Drop late text from the aborted utterance only. */
            if (aborted_)
            {
                write(1, "TTS_IGN\n", 8);
                return;
            }
            tts_stop_pending_ = false;
            if (audio_service_)
            {
                audio_service_->SetTtsUplinkGate(true);
                audio_service_->ArmDownlinkPlaybackProtect();
            }
            if (device_state_ == kDeviceStateIdle ||
                device_state_ == kDeviceStateListening ||
                device_state_ == kDeviceStateConnecting)
            {
                SetDeviceState(kDeviceStateSpeaking);
            }
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text) && display)
            {
                write(1, "TTS_TXT\n", 8);
                ESP_LOGE(TAG, "<< %s", text->valuestring);
                /* Cloud sometimes leaks tool names into tts text
                 * (e.g. "% self.get_device_status"). Painting that bubble
                 * then running MCP_INLINE in the same tick raced LVGL. */
                const char *t = text->valuestring;
                while (*t == ' ' || *t == '\t' || *t == '%' || *t == '<' ||
                       *t == '>')
                    ++t;
                if (strncmp(t, "self.", 5) == 0)
                    write(1, "TTS_TOOL\n", 9);
                else
                    display->SetChatMessage("assistant", text->valuestring);
            }
        }
    }
    else if (strcmp(type->valuestring, "stt") == 0)
    {
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && display)
        {
            /* New user utterance after barge — allow the next assistant turn. */
            aborted_ = false;
            write(1, "STT_OK\n", 7);
            ESP_LOGE(TAG, ">> %s", text->valuestring);
            const std::string command_text =
                ChatLocalCmd_NormalizeVisionOpenCommand(text->valuestring);
            const bool typed_echo=TypedChat().active.load() && TypedChat().last==text->valuestring;
            if (!typed_echo) display->SetChatMessage("user", command_text.c_str());
            if (typed_echo) TypedChat().last.clear();

            /* Local device control (音量/背光/蓝牙/开应用) — do not wait
             * for cloud MCP. Abort cloud TTS for this turn if handled. */
            if ((ChatScreen::IsForeground() || SystemChrome_FloatAiOpen() ||
                 DigitalPeopleScreen::IsActive() ||
                 DigitalPeopleScreen::IsForeground()) &&
                ChatLocalCmd_TryHandle(command_text))
            {
                write(1, "LOC_CMD\n", 8);
                aborted_ = true;
                AbortSpeaking(kAbortReasonNone);
            }
        }
    }
    else if (strcmp(type->valuestring, "llm") == 0)
    {
        /* Chat text mode: skip emotion widget updates (wake blue). */
        if (ChatScreen::IsForeground())
            return;
        auto emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emotion) && display)
            display->SetEmotion(emotion->valuestring);
    }
    else if (strcmp(type->valuestring, "mcp") == 0)
    {
        /* Always answer MCP (tools/list, tools/call) — even mid-wake-connect.
         * Skipping broke 打开应用/调音量/背光/蓝牙 when the cloud replied
         * during AUD_OPEN. Device-control tools run inline and are safe. */
        auto payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload))
            McpServer::GetInstance().ParseMessage(payload);
    }
    else if (strcmp(type->valuestring, "system") == 0)
    {
        auto command = cJSON_GetObjectItem(root, "command");
        if (cJSON_IsString(command) &&
            strcmp(command->valuestring, "reboot") == 0)
        {
            Reboot();
        }
    }
    else if (strcmp(type->valuestring, "alert") == 0)
    {
        auto status = cJSON_GetObjectItem(root, "status");
        auto message = cJSON_GetObjectItem(root, "message");
        auto emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(status) && cJSON_IsString(message) &&
            cJSON_IsString(emotion))
        {
            Alert(status->valuestring, message->valuestring,
                  emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
        }
    }
    else if (strcmp(type->valuestring, "message") == 0)
    {
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && display)
            display->SetChatMessage("system", text->valuestring);
    }
    else
    {
        ESP_LOGW(TAG, "Unknown Xiaozhi message type: %s", type->valuestring);
    }
}

/* ------------------------------------------------------------------ */
/* OTA / version check loop                                            */
/* ------------------------------------------------------------------ */

static bool ActiveNetworkHasIpv4()
{
    return NetworkService::Available();
}

void Application::CheckNewVersion(Ota &ota)
{
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;

    Settings wifi_settings("wifi", false);
    const bool cached_bound = wifi_settings.GetInt("xiaozhi_bound", 0) != 0;
    const bool silent = ota_check_silent_ ||
        (boot_ready_ && cached_bound && !HasPendingActivation());

    auto &board = Board::GetInstance();
    while (true)
    {
        if (!silent)
            SetDeviceState(kDeviceStateActivating);
        auto *display = board.GetDisplay();
        if (display && !silent)
            display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        /* Boot often starts OTA before STA/DHCP succeeds (Kconfig SSID
         * timeout). Wait for an address — or a UI WiFi connect poke —
         * otherwise getaddrinfo(api.tenclass.net) fails and the 6-digit
         * code never appears. */
        if (!ActiveNetworkHasIpv4())
        {
            ESP_LOGE(TAG, "CheckNewVersion: waiting for active network IPv4…");
            write(1, "OTA_WAIT_IP\n", 12);
            bool have_ip = false;
            const int max_wait = (cached_bound || silent) ? 150 : 180;
            for (int i = 0; i < max_wait; ++i)
            {
                if (ActiveNetworkHasIpv4())
                {
                    have_ip = true;
                    break;
                }
                if (wifi_recheck_requested_)
                {
                    wifi_recheck_requested_ = false;
                    ESP_LOGE(TAG, "CheckNewVersion: WiFi poke while waiting for IP");
                    if (ActiveNetworkHasIpv4())
                        have_ip = true;
                    break;
                }
                if (cached_bound || silent)
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                else
                    std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!have_ip && !ActiveNetworkHasIpv4())
            {
                ESP_LOGW(TAG, "CheckNewVersion: still no IP, trying OTA anyway");
            }
            else
            {
                char ipbuf[32] = {};
                struct in_addr addr;
                memset(&addr, 0, sizeof(addr));
                if (netlib_get_ipv4addr(metalio_dual_network_path() == 1 ? "eth1" : "eth0", &addr) == 0)
                    snprintf(ipbuf, sizeof(ipbuf), "%s", inet_ntoa(addr));
                ESP_LOGE(TAG, "CheckNewVersion: active network ip=%s", ipbuf);
                write(1, "OTA_HAVE_IP\n", 12);
                /* Brief settle so DNS nameserver from DHCP is usable. */
                const int settle_ms = (cached_bound || silent) ? 80 : 500;
                std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
            }
        }

        esp_err_t err = ota.CheckVersion();
        if (err != ESP_OK)
        {
            retry_count++;
            if (retry_count >= MAX_RETRY)
            {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }
            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)",
                     retry_delay, retry_count, MAX_RETRY);
            bool woke_for_wifi = false;
            for (int i = 0; i < retry_delay; i++)
            {
                if (wifi_recheck_requested_)
                {
                    wifi_recheck_requested_ = false;
                    woke_for_wifi = true;
                    ESP_LOGE(TAG, "WiFi link ready — retry CheckVersion now");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (device_state_ == kDeviceStateIdle)
                    break;
            }
            if (woke_for_wifi)
            {
                retry_count = 0;
                retry_delay = 10;
            }
            else
            {
                retry_delay *= 2;
            }
            continue;
        }
        retry_count = 0;
        retry_delay = 10;

        if (ota.HasNewVersion())
        {
            /* Flash write is stubbed on this port — do not download or
             * reboot, otherwise the Xiaozhi bind flow never starts. */
            ESP_LOGW(TAG,
                     "Firmware %s advertised by OTA, skipped on openvela",
                     ota.GetFirmwareVersion().c_str());
        }

        if (!ota.HasActivationCode() && !ota.HasActivationChallenge())
        {
            /* Bound at xiaozhi.me (or never needed a code). Clear any stale
             * pending code so ChatScreen unblocks and arms wake. */
            ClearPendingActivation();
            if (event_group_)
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            break;
        }

        while (activation_suspended_)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (display)
            display->SetStatus(Lang::Strings::ACTIVATION);
        /* Publish immediately (CheckVersion may also have done this). */
        if (ota.HasActivationCode())
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());

        /* Xiaozhi.me bind is by 6-digit code in the console. NuttX cannot
         * compute efuse HMAC, so never block on Activate() with empty hmac —
         * poll CheckVersion until the activation section disappears. */
        if (ota.HasActivationCode())
        {
            ESP_LOGE(TAG, "waiting for xiaozhi.me bind (code=%s challenge=%d)",
                     ota.GetActivationCode().c_str(),
                     ota.HasActivationChallenge() ? 1 : 0);
            for (int i = 0; i < 10; ++i)
            {
                while (activation_suspended_)
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (wifi_recheck_requested_)
                {
                    wifi_recheck_requested_ = false;
                    ESP_LOGE(TAG, "WiFi link ready — re-CheckVersion for bind");
                    break;
                }
                if (ota.HasActivationChallenge())
                {
                    /* Best-effort; empty HMAC usually returns 202/fail. */
                    esp_err_t act_err = ota.Activate();
                    if (act_err == ESP_OK)
                    {
                        ClearPendingActivation();
                        if (event_group_)
                            xEventGroupSetBits(event_group_,
                                               MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                        return;
                    }
                }
                std::this_thread::sleep_for(std::chrono::seconds(3));
                /* Re-query OTA so xiaozhi.me bind clears pending promptly. */
                if (ota.CheckVersion() == ESP_OK &&
                    !ota.HasActivationCode() && !ota.HasActivationChallenge())
                {
                    ClearPendingActivation();
                    if (event_group_)
                        xEventGroupSetBits(event_group_,
                                           MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                    return;
                }
                if (ota.HasActivationCode())
                    ShowActivationCode(ota.GetActivationCode(),
                                       ota.GetActivationMessage());
                if (device_state_ == kDeviceStateIdle)
                    break;
            }
            continue;
        }

        /* Challenge-only (no display code) — rare; keep trying Activate. */
        for (int i = 0; i < 10; ++i)
        {
            while (activation_suspended_)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ESP_LOGE(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t act_err = ota.Activate();
            if (act_err == ESP_OK)
            {
                ClearPendingActivation();
                if (event_group_)
                    xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                return;
            }
            else if (act_err == ESP_ERR_TIMEOUT)
            {
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
            if (device_state_ == kDeviceStateIdle)
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* C-linkage entry point invoked by metalio_main.c                    */
/* ------------------------------------------------------------------ */

extern "C" int metalio_app_start(int argc, char *argv[]);

extern "C" int metalio_app_start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    write(1, "ENTRY0\n", 7);
    board_shim_initialize();
    write(1, "ENTRY1\n", 7);

    Application::GetInstance().Start();
    write(1, "ENTRY3\n", 7);

    return 0;
}
