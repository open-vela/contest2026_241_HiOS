#include "hi_task_cpp.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * BluetoothScreen — full port from MetalioClaw4
 * main/display/screen/bluetooth_screen/bluetooth_screen.cc.
 *
 * The UI is complete (reset / 3 mode buttons / mode-1 hint / mode-2 scan
 * panel with device list + music/call buttons). The original drives the
 * external BT audio codec over SimpleUart and resets it via the TCA9555
 * IOExpander BT_POWER pin.
 *
 * openvela wiring:
 *   - SimpleUart -> apps/metalio/boards/simple_uart.* (opens /dev/ttyS1).
 *   - BT_POWER  -> metalio_bt_power() (board library, TCA9555 P0.6).
 * AT-command send / RX parsing / scan / connect are ported from the
 * reference so mode 1/2/3, scanning, pairing and call/music modes work.
 */

#include "bluetooth_screen.h"
#include "application.h"
#include "settings.h"

#include "i18n.h"
#include "esp_log_shim.h"
#include "home_screen/home_screen.h"
#include "simple_uart.h"

#include <metalio/metalio.h>   /* metalio_bt_power() */

#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>            /* usleep() */

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char *TAG = "BluetoothScreen";

constexpr int kPanelSize   = 720;
constexpr int kHeaderH     = 90;
constexpr int kBackBtnSize = 72;
constexpr int kAddrHexLen  = 12;

constexpr uint32_t kColorBg        = 0x0E1116;
constexpr uint32_t kColorCard      = 0x1B2030;
constexpr uint32_t kColorBtn       = 0x2A2F3A;
constexpr uint32_t kColorBtnActive = 0x3B82F6;
constexpr uint32_t kColorText      = 0xFFFFFF;
constexpr uint32_t kColorSubtle    = 0x9AA3B2;
constexpr uint32_t kColorSuccess   = 0x34C759;
constexpr uint32_t kColorError     = 0xFF3B30;
constexpr uint32_t kColorScanning  = 0xF59E0B;

// BT module operating mode (matches the original enum).
enum class BtMode : uint8_t {
    kNone = 0,
    kMode1,
    kMode2,
    kMode3,
};

enum class ConnState : uint8_t {
    kIdle,
    kScanning,
    kConnecting,
    kConnected,
};

struct BtDevice {
    char address[kAddrHexLen + 1];
    char name[64];
};

struct UiState {
    lv_obj_t *screen      = nullptr;
    lv_obj_t *status_label = nullptr;
    lv_obj_t *mode_btns[3] = {};
    lv_obj_t *mode1_panel = nullptr;
    lv_obj_t *mode2_panel = nullptr;
    lv_obj_t *scan_btn    = nullptr;
    lv_obj_t *device_list = nullptr;
    lv_obj_t *music_btn   = nullptr;
    lv_obj_t *call_btn    = nullptr;
};

UiState  s_ui;
std::atomic<BtMode> s_active_mode{BtMode::kMode1};
std::atomic<ConnState> s_conn_state{ConnState::kIdle};
std::atomic<bool> s_screen_active{false};
std::atomic<bool> s_mode_busy{false};
std::atomic<int> s_ack_mode{0};
std::atomic<unsigned> s_scan_epoch{0},s_ui_epoch{0};
std::atomic<bool> s_request_busy{false};
std::mutex s_rx_mutex;
std::mutex s_phone_audio_mutex;
std::atomic<bool> s_phone_audio{false};
// Private route state stays on the heap; no Application layout changes.
struct PhoneRoutingState {
 std::mutex operation;
 std::atomic<unsigned> generation{1},request{1};
 std::atomic<int> preferred{-1},route{0}; // auto=-1; speaker=1, headset=2
 std::atomic<bool> worker{false},ending{false},boot_restore{false};
 BtMode before=BtMode::kMode1;bool before_sco=false;
};
PhoneRoutingState&PhoneRouting(){static auto*s=new PhoneRoutingState;return *s;}
bool CellularAudioActive(){int c=metalio_nt26_get_call_state();return c==METALIO_CALL_DIALING||c==METALIO_CALL_ACTIVE||c==METALIO_CALL_HELD;}

std::atomic<bool> s_sco_up{false},s_profile_busy{false},s_a2dp_selected{false},s_headset_linked{false};
// A profile selection is distinct from a running P4 PCM stream. An idle
// A2DP link need not drive I2S clocks until the first playback starts.
std::atomic<unsigned> s_link_epoch{0};
std::string s_observer_rx;

std::string           s_rx_buffer;
std::vector<BtDevice> s_devices;
std::mutex            s_devices_mutex;

// ---------------------------------------------------------------------------
// AT command TX — real /dev/ttyS1 write (was a stub).
// ---------------------------------------------------------------------------

void post_clear_list();  // defined below; used early by OnScanClicked()

bool SendAt(const char *cmd)
{
    if (cmd == nullptr) {
        return false;
    }
    if (!SimpleUart::getInstance().begin()) {
        ESP_LOGW(TAG, "TX dropped (uart unavailable): %s", cmd);
        return false;
    }

    std::string line(cmd);
    if (line.empty() || line.back() != '\n') {
        line += "\r\n";
    }
    bool ok = SimpleUart::getInstance().sendString(line);
    ESP_LOGI(TAG, "TX: %s", cmd);
    return ok;
}

void UpdateStatus(const char *text, uint32_t color = kColorText)
{
    if (s_ui.status_label == nullptr) {
        return;
    }
    lv_label_set_text(s_ui.status_label, text);
    lv_obj_set_style_text_color(s_ui.status_label, lv_color_hex(color),
                                LV_PART_MAIN);
}

// Async status update (RX thread / background task -> LVGL thread).
struct AsyncStatusMsg {
    char text[128];
    uint32_t color;
    unsigned epoch;
};

void async_update_status(void *user_data)
{
    auto *msg = static_cast<AsyncStatusMsg *>(user_data);
    if(s_screen_active&&msg->epoch==s_ui_epoch)UpdateStatus(msg->text, msg->color);
    delete msg;
}

void post_status(const char *text, uint32_t color = kColorText)
{
    if (!s_screen_active) {
        return;
    }
    auto *msg = new AsyncStatusMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text);
    msg->color = color;msg->epoch=s_ui_epoch;
    lv_lock();
    if(lv_async_call(async_update_status, msg)!=LV_RESULT_OK)delete msg;
    lv_unlock();
}

void RefreshModeButtons()
{
    for (int i = 0; i < 3; ++i) {
        if (s_ui.mode_btns[i] == nullptr) {
            continue;
        }
        const bool active = (static_cast<int>(s_active_mode.load()) == i + 1);
        lv_obj_set_style_bg_color(
            s_ui.mode_btns[i],
            lv_color_hex(active ? kColorBtnActive : kColorBtn),
            LV_PART_MAIN);
    }
}

void ShowMode1Panel(bool show)
{
    if (s_ui.mode1_panel == nullptr) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(s_ui.mode1_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.mode1_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void ShowMode2Panel(bool show)
{
    if (s_ui.mode2_panel == nullptr) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(s_ui.mode2_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.mode2_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void RestoreModeUi()
{
    if(s_headset_linked)s_conn_state=ConnState::kConnected;
    RefreshModeButtons();
    switch (s_active_mode) {
        case BtMode::kMode1:
            ShowMode1Panel(true);
            ShowMode2Panel(false);
            UpdateStatus(I18n::T("模式1 已设置"), kColorSuccess);
            break;
        case BtMode::kMode2:
            ShowMode1Panel(false);
            ShowMode2Panel(true);
            UpdateStatus(I18n::T("模式2 已设置，可扫描设备"), kColorSuccess);
            break;
        case BtMode::kMode3:
            ShowMode1Panel(false);
            ShowMode2Panel(false);
            UpdateStatus(I18n::T("模式3 已设置"), kColorSuccess);
            break;
        default:
            ShowMode1Panel(false);
            ShowMode2Panel(false);
            break;
    }
}

// ---------------------------------------------------------------------------
// Mode switching (700ms gap between the two AT commands, per protocol).
// ---------------------------------------------------------------------------

void RunModeCommand(BtMode mode)
{
    // Keep the prefix, gap, final command and ACK under one UART owner.
    // The background reader must not consume half of this transaction.
    auto valid=[](){return !s_phone_audio.load()&&metalio_nt26_get_call_state()==METALIO_CALL_IDLE;};
    BtMode previous=s_active_mode.load();s_ack_mode=0;
    {std::lock_guard<std::mutex> lock(s_phone_audio_mutex);if(s_phone_audio){s_mode_busy=false;return;}metalio_bt_set_scan_mode(true);}
    bool ok=metalio_bt_is_powered()||metalio_bt_power(true)==0;
    char text[48];snprintf(text,sizeof(text),"正在切换模式%d…",(int)mode);post_status(text,kColorScanning);
    ok=ok&&SimpleUart::getInstance().sendModeWait((int)mode,valid);
    ok=ok&&s_ack_mode.load()==(int)mode;
    if(ok){
        Settings settings("bluetooth",true);
        bool saved=settings.SetStringSaved("mode",std::to_string((int)mode));
        printf("BT_MODE_CONFIRMED mode=%d saved=%d\n",(int)mode,saved);
        if(!saved)post_status("模式已切换，设置保存失败，请重试",kColorError);
    }
    if(!ok&&!s_phone_audio&&metalio_nt26_get_call_state()==METALIO_CALL_IDLE){
        // RX/TX may have succeeded even when MODE did not. Restore the whole
        // previous transaction and require its ACK before advertising it.
        s_ack_mode=0;
        BtMode restore=previous==BtMode::kNone?BtMode::kMode1:previous;
        bool restored=SimpleUart::getInstance().sendModeWait((int)restore,valid)&&s_ack_mode==(int)restore;
        s_active_mode=restored?restore:BtMode::kNone;
        post_status(restored?"切换未完成，已恢复原模式":"模式未确认，请重新选择",kColorError);
    }
    {std::lock_guard<std::mutex> lock(s_phone_audio_mutex);if(!s_phone_audio)metalio_bt_set_scan_mode(s_active_mode==BtMode::kMode2||s_active_mode==BtMode::kNone);}
    s_mode_busy=false;
    // Restore existing microphone/speaker streams after a real mode-1 ACK.
    // InputData otherwise keeps its old enabled flag after clocks disappeared.
    if(s_active_mode==BtMode::kMode1&&!s_phone_audio)Application::GetInstance().ArmAssistantWakeWord(true);
}

void SendModeCommand(BtMode mode)
{
    printf("BT_MODE_REQUEST mode=%d active=%d busy=%d profile=%d request=%d phone=%d call=%d exclusive=%d\n",
        (int)mode,(int)s_active_mode.load(),s_mode_busy.load(),s_profile_busy.load(),s_request_busy.load(),s_phone_audio.load(),metalio_nt26_get_call_state(),Application::GetInstance().GetAudioService().HasExclusiveCapture());
    if(s_profile_busy||s_phone_audio||metalio_nt26_get_call_state()!=METALIO_CALL_IDLE||
       Application::GetInstance().GetAudioService().HasExclusiveCapture()){
        post_status("音频正在使用，请结束录音或通话后再切换",kColorScanning);return;
    }
    if(s_mode_busy.exchange(true))return;
    if (!SimpleUart::getInstance().begin()) {
        s_mode_busy=false;
        post_status(I18n::T("UART 未初始化"), kColorError);
        return;
    }
    pthread_t t;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,16384);
    int rc=hi_task_create_legacy(&t,&a,[](void *p)->void*{RunModeCommand((BtMode)(intptr_t)p);return nullptr;},(void*)(intptr_t)mode);
    pthread_attr_destroy(&a);if(!rc)pthread_detach(t);else{s_mode_busy=false;post_status("模式切换任务未能启动，请重试",kColorError);printf("BT_MODE_WORKER error=%d\n",rc);}
}

void ApplyMode(BtMode mode)
{
    PhoneRouting().boot_restore=false;
    if(s_mode_busy||s_request_busy||s_profile_busy){UpdateStatus("请等待当前蓝牙操作完成",kColorScanning);return;}
    ++s_scan_epoch;
    // Only SET MODE acknowledges the visible mode/panel. A busy or failed
    // command must not leave the UI claiming a mode the hardware never set.
    SendModeCommand(mode);
}

void OnModeClicked(lv_event_t *e)
{
    const int idx = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    ApplyMode(static_cast<BtMode>(idx + 1));
}

// ---------------------------------------------------------------------------
// BT power reset (TCA9555 P0.6 low -> 300ms -> high).
// ---------------------------------------------------------------------------

void async_after_bt_reset(void * /*user_data*/)
{
    s_active_mode = BtMode::kNone;
    s_conn_state  = ConnState::kIdle;
    RefreshModeButtons();
    ShowMode1Panel(false);
    ShowMode2Panel(false);
}

void ResetTask()
{
    s_headset_linked=false;s_sco_up=false;s_a2dp_selected=false;
    post_status(I18n::T("正在复位蓝牙..."), kColorScanning);
    metalio_bt_power(false);
    usleep(300 * 1000);
    metalio_bt_power(true);
    lv_lock();lv_async_call(async_after_bt_reset, nullptr);lv_unlock();
    post_status(I18n::T("蓝牙电源已复位"), kColorSuccess);
}

void OnResetClicked(lv_event_t * /*e*/)
{
    if(s_profile_busy||s_mode_busy||s_request_busy||s_phone_audio||Application::GetInstance().GetAudioService().HasExclusiveCapture())return;
    HiTask::LegacyThread(ResetTask).detach();
}

// ---------------------------------------------------------------------------
// Scan / connect / call / music.
// ---------------------------------------------------------------------------

struct BtRequest {unsigned epoch;std::string command;bool scan;};
void StartRequest(const std::string &command,bool scan) {
    if(s_phone_audio||Application::GetInstance().GetAudioService().HasExclusiveCapture()){UpdateStatus("音频正在使用，请稍后扫描或连接",kColorScanning);return;}
    if(s_profile_busy||s_mode_busy||s_request_busy.exchange(true)){UpdateStatus("请等待当前蓝牙操作完成",kColorScanning);return;}
    auto *job=new BtRequest{++s_scan_epoch,command,scan};
    s_conn_state=scan?ConnState::kScanning:ConnState::kConnecting;
    UpdateStatus(scan?"正在扫描附近设备…":"正在连接设备…",kColorScanning);
    pthread_t t;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,16384);
    int rc=hi_task_create_legacy(&t,&a,[](void *arg)->void*{
        auto *j=(BtRequest*)arg;
        auto valid=[j]{return !s_phone_audio&&j->epoch==s_scan_epoch&&metalio_nt26_get_call_state()==METALIO_CALL_IDLE;};
        {std::lock_guard<std::mutex> lock(s_phone_audio_mutex);if(valid())metalio_bt_set_scan_mode(true);}
        bool sent=valid()&&SimpleUart::getInstance().begin()&&SimpleUart::getInstance().sendStringWait(j->command+"\r\n",2000,valid);
        if(!sent&&j->epoch==s_scan_epoch){s_conn_state=ConnState::kIdle;post_status("蓝牙模块未响应命令，请重试",kColorError);}
        for(int n=0;sent&&n<(j->scan?220:300)&&j->epoch==s_scan_epoch&&s_screen_active;n++){
            if(s_conn_state!=(j->scan?ConnState::kScanning:ConnState::kConnecting))break;usleep(100000);
        }
        if(sent&&j->epoch==s_scan_epoch&&s_screen_active&&s_conn_state==(j->scan?ConnState::kScanning:ConnState::kConnecting)){
            s_conn_state=ConnState::kIdle;
            if(j->scan){size_t count;{std::lock_guard<std::mutex>l(s_devices_mutex);count=s_devices.size();}
                char status[96];snprintf(status,sizeof(status),count?"扫描结束，共 %u 个设备":"未发现设备，请让目标设备进入配对状态",(unsigned)count);post_status(status,count?kColorSuccess:kColorSubtle);}
            else post_status("连接超时，请确认设备仍处于配对状态",kColorError);
        }
        s_request_busy=false;delete j;return nullptr;
    },job);pthread_attr_destroy(&a);
    if(!rc)pthread_detach(t);else{delete job;s_request_busy=false;s_conn_state=ConnState::kIdle;UpdateStatus("系统忙，请重试",kColorError);}
}
void OnScanClicked(lv_event_t *) {
    if(s_mode_busy||s_request_busy){UpdateStatus("请等待当前蓝牙操作完成",kColorScanning);return;}
    if(s_active_mode!=BtMode::kMode2){UpdateStatus("请先切换到模式2",kColorError);return;}
    {std::lock_guard<std::mutex>l(s_devices_mutex);s_devices.clear();}
    post_clear_list();StartRequest("AT+INQUIRING",true);
}

bool ProfileCommand(bool call,bool cellular=false,unsigned phone_generation=0,unsigned request=0){
 if(s_profile_busy.exchange(true))return false;
 const unsigned epoch=s_link_epoch.load();
 auto valid=[epoch,cellular,phone_generation,request](){auto&r=PhoneRouting();
  if(phone_generation&&(r.generation!=phone_generation||r.request!=request))return false;
  bool ownership=cellular?(s_phone_audio&&CellularAudioActive()&&!r.ending):
      (!CellularAudioActive()&&(!s_phone_audio||metalio_nt26_get_call_state()==METALIO_CALL_INCOMING));
  return ownership&&s_link_epoch==epoch&&s_active_mode==BtMode::kMode2&&
      s_headset_linked&&!s_mode_busy&&!s_request_busy;
 };
 auto command=[&](const char *cmd){
  bool sent=valid()&&SimpleUart::getInstance().begin()&&
      SimpleUart::getInstance().sendStringWait(std::string(cmd)+"\r\n",2500,valid);
  ESP_LOGW(TAG,"profile %s: TX %s %s",call?"SCO":"A2DP",cmd,sent?"sent":"failed");
  return sent;
 };
 // Do not resend BTSCO=0 to an already closed SCO bearer: that is already
 // the music state, and an unnecessary command can add an unrelated error.
 // Nor may an unrelated generic ERROR overrule a later SETUP/DISC SCO.
 bool ok=valid();
 if(ok)metalio_bt_leave_voice();
 const char *failure="蓝牙连接已变化，请重新选择模式";
 if(ok&&s_sco_up.load()!=call){
  failure="蓝牙模式命令未发出，请重试";
  if(call){ok=command("AT+PP=1");if(ok)usleep(700000);}
  if(ok)ok=command(call?"AT+BTSCO=1":"AT+BTSCO=0");
  if(ok){
   failure=call?"耳机未建立通话音频，请确认支持通话模式":"耳机未退出通话音频，请重试";
   for(int n=0;valid()&&s_sco_up.load()!=call&&n<60;n++)usleep(100000);
   ok=valid()&&s_sco_up.load()==call;
  }
 }
 if(ok&&!call&&!cellular){
  // PP=1 starts A2DP, but not every supported module returns a separate
  // OK/MPLAY. Keep this as a selection, not proof of PCM or audible sound.
  failure="蓝牙音乐命令未发出，请重试";
  usleep(200000);ok=command("AT+PP=1");
 }
 ok=ok&&valid()&&s_sco_up.load()==call;
 s_a2dp_selected=ok&&!call&&!cellular;s_profile_busy=false;
 if(!ok)ESP_LOGW(TAG,"profile %s failed: mode=%d linked=%d sco=%d epoch=%u/%u",
     call?"SCO":"A2DP",(int)s_active_mode.load(),(int)s_headset_linked.load(),
     (int)s_sco_up.load(),epoch,s_link_epoch.load());
 // GPIO edges confirm only P4's external clock, not the wireless profile.
 // Check them at actual capture/playback time below, with a settle window.
 post_status(ok?(call?"耳机通话模式已建立":"已选择耳机音乐模式"):failure,ok?kColorSuccess:kColorError);
 return ok;
}

#include "phone_audio_route.inc"

int AcceptHeadsetStream(bool call){
 const unsigned epoch=s_link_epoch.load();
 for(int n=0;n<15;n++){
  if(s_profile_busy||s_mode_busy||s_request_busy)return -EBUSY;
  if(s_link_epoch!=epoch||s_active_mode!=BtMode::kMode2||!s_headset_linked||
     (s_phone_audio&&metalio_nt26_get_call_state()!=METALIO_CALL_INCOMING)||CellularAudioActive()||s_sco_up.load()!=call)return -ENOTCONN;
  if(metalio_bt_accept_headset_stream()==0)return 0;
  usleep(100000);
 }
 ESP_LOGW(TAG,"profile %s: P4 audio clock not present",call?"SCO":"A2DP");
 post_status("耳机已连接，但音频时钟未就绪",kColorError);
 return -EIO;
}
void MusicModeTask(){(void)ProfileCommand(false);}
void CallModeTask(){(void)ProfileCommand(true);}

void OnMusicModeClicked(lv_event_t * /*e*/)
{
    if(Application::GetInstance().GetAudioService().HasExclusiveCapture()){UpdateStatus("录音或回放中，请结束后再切换",kColorScanning);return;}
    if(s_profile_busy||s_mode_busy||s_request_busy){UpdateStatus("请等待当前蓝牙操作完成",kColorScanning);return;}
    if (s_conn_state != ConnState::kConnected) {
        UpdateStatus(I18n::T("请先连接蓝牙设备"), kColorError);
        return;
    }
    UpdateStatus("正在切换耳机音乐模式…",kColorScanning);
    HiTask::LegacyThread(MusicModeTask).detach();
}

void OnCallModeClicked(lv_event_t * /*e*/)
{
    if(Application::GetInstance().GetAudioService().HasExclusiveCapture()){UpdateStatus("录音或回放中，请结束后再切换",kColorScanning);return;}
    if(s_profile_busy||s_mode_busy||s_request_busy){UpdateStatus("请等待当前蓝牙操作完成",kColorScanning);return;}
    if (s_conn_state != ConnState::kConnected) {
        UpdateStatus(I18n::T("请先连接蓝牙设备"), kColorError);
        return;
    }
    UpdateStatus("正在切换耳机通话模式…",kColorScanning);
    HiTask::LegacyThread(CallModeTask).detach();
}

// ---------------------------------------------------------------------------
// RX parsing (AT+BT:<addr><name> scan lines + state keywords).
// ---------------------------------------------------------------------------

void clear_device_list_ui()
{
    if (s_ui.device_list != nullptr) {
        lv_obj_clean(s_ui.device_list);
    }
}

struct AsyncAddDeviceMsg {
    char address[kAddrHexLen + 1];
    char name[64];
    unsigned epoch;
};

void async_add_device_item(void *user_data)
{
    auto *msg = static_cast<AsyncAddDeviceMsg *>(user_data);
    if (!s_screen_active||msg->epoch!=s_ui_epoch||s_ui.device_list == nullptr) {
        delete msg;
        return;
    }

    lv_obj_t *item = lv_button_create(s_ui.device_list);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_height(item, 72);
    lv_obj_set_style_radius(item, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(item, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(item, 20, LV_PART_MAIN);

    char *addr_copy = static_cast<char *>(lv_malloc(kAddrHexLen + 1));
    if (addr_copy != nullptr) {
        memcpy(addr_copy, msg->address, kAddrHexLen + 1);
        lv_obj_add_event_cb(
            item,
            [](lv_event_t *e) {
                const char *addr =
                    static_cast<const char *>(lv_event_get_user_data(e));
                if (addr == nullptr) {
                    return;
                }
                char cmd[48];
                snprintf(cmd, sizeof(cmd), "AT+CONNECT=%s", addr);
                if(s_active_mode!=BtMode::kMode2)return;
                StartRequest(cmd,false);

            },
            LV_EVENT_CLICKED, addr_copy);
        lv_obj_add_event_cb(
            item,
            [](lv_event_t *e) {
                char *addr = static_cast<char *>(lv_event_get_user_data(e));
                lv_free(addr);
            },
            LV_EVENT_DELETE, addr_copy);
    }

    lv_obj_t *lbl = lv_label_create(item);
    char display[96];
    if (msg->name[0] != '\0') {
        snprintf(display, sizeof(display), "%s\n%s", msg->name, msg->address);
    } else {
        snprintf(display, sizeof(display), "%s", msg->address);
    }
    lv_label_set_text(lbl, display);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    delete msg;
}

void add_device_to_list(const char *address, const char *name)
{
    if (!s_screen_active) {
        return;
    }
    auto *msg = new AsyncAddDeviceMsg{};
    snprintf(msg->address, sizeof(msg->address), "%s", address);
    snprintf(msg->name, sizeof(msg->name), "%s", name);
    msg->epoch=s_ui_epoch;
    lv_lock();if(lv_async_call(async_add_device_item, msg)!=LV_RESULT_OK)delete msg;lv_unlock();
}

void async_clear_list(void *user_data)
{
    if(s_screen_active&&(unsigned)(uintptr_t)user_data==s_ui_epoch)clear_device_list_ui();
}

void post_clear_list()
{
    if (!s_screen_active) {
        return;
    }
    lv_lock();lv_async_call(async_clear_list,(void*)(uintptr_t)s_ui_epoch.load());lv_unlock();
}

void async_on_mode1_set(void * /*user_data*/)
{
    if(!s_screen_active)return;
    RefreshModeButtons();
    ShowMode1Panel(true);
    ShowMode2Panel(false);
}

void async_on_mode2_set(void * /*user_data*/)
{
    if(!s_screen_active)return;
    RefreshModeButtons();
    ShowMode1Panel(false);
    ShowMode2Panel(true);
}

void async_on_mode3_set(void * /*user_data*/)
{
    if(!s_screen_active)return;
    RefreshModeButtons();
    ShowMode1Panel(false);
    ShowMode2Panel(false);
}

bool is_hex_char(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

void trim_line(std::string &line)
{
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    size_t start = 0;
    while (start < line.size() && line[start] == ' ') {
        ++start;
    }
    if (start > 0) {
        line = line.substr(start);
    }
}

bool parse_bt_device_line(const std::string &line,
                          char *address, size_t addr_sz,
                          char *name, size_t name_sz)
{
    size_t prefix=line.size()>=6&&memcmp(line.data(),"AT+BT:",6)==0?6:line.size()>=4&&memcmp(line.data(),"+BT:",4)==0?4:line.size()>=3&&memcmp(line.data(),"BT:",3)==0?3:0;
    if(!prefix)return false;
    std::string payload=line.substr(prefix);size_t at=0,n=0;
    while(at<payload.size()&&(payload[at]==' '||payload[at]=='\"'))++at;
    char hex[kAddrHexLen+1]{};
    while(at<payload.size()&&n<kAddrHexLen){
        char c=payload[at++];if(is_hex_char(c))hex[n++]=c;
        else if(c!=':'&&c!='-'&&c!=' ')return false;
    }
    if(n!=kAddrHexLen)return false;
    while(at<payload.size()&&(payload[at]==' '||payload[at]=='\"'||payload[at]==','||payload[at]==':'))++at;
    std::string device_name=payload.substr(at);trim_line(device_name);
    if(!device_name.empty()&&device_name.back()=='\"')device_name.pop_back();
    if(device_name.size()>=name_sz){device_name.resize(name_sz-1);
        while(!device_name.empty()&&((unsigned char)device_name.back()&0xc0)==0x80)device_name.pop_back();
        if(!device_name.empty()&&(unsigned char)device_name.back()>=0xc0)device_name.pop_back();}
    snprintf(address,addr_sz,"%s",hex);snprintf(name,name_sz,"%s",device_name.c_str());return true;
#if 0
    if (payload.size() < static_cast<size_t>(kAddrHexLen)) {
        return false;
    }
    for (int i = 0; i < kAddrHexLen; ++i) {
        if (!is_hex_char(payload[i])) {
            return false;
        }
    }
    snprintf(address, addr_sz, "%.*s", kAddrHexLen, payload.c_str());
    snprintf(name, name_sz, "%s", payload.c_str() + kAddrHexLen);
    return true;
#endif
}

void handle_response_line(const std::string &raw_line)
{
    std::string line = raw_line;
    trim_line(line);
    if (line.empty()) {
        return;
    }

    ESP_LOGI(TAG, "RX: %s", line.c_str());

    if (line.find("SET MODE 1") != std::string::npos) {
        s_ack_mode=1;
        s_active_mode = BtMode::kMode1;
        s_conn_state  = ConnState::kIdle;
        post_status(I18n::T("模式1 已设置"), kColorSuccess);
        lv_lock();lv_async_call(async_on_mode1_set, nullptr);lv_unlock();
        return;
    }
    if (line.find("SET MODE 2") != std::string::npos) {
        s_ack_mode=2;
        s_active_mode = BtMode::kMode2;
        s_conn_state  = ConnState::kIdle;
        post_status(I18n::T("模式2 已设置，可扫描设备"), kColorSuccess);
        lv_lock();lv_async_call(async_on_mode2_set, nullptr);lv_unlock();
        return;
    }
    if (line.find("SET MODE 3") != std::string::npos) {
        s_ack_mode=3;
        s_active_mode = BtMode::kMode3;
        s_conn_state  = ConnState::kIdle;
        post_status(I18n::T("模式3 已设置"), kColorSuccess);
        lv_lock();lv_async_call(async_on_mode3_set, nullptr);lv_unlock();
        return;
    }

    if (line.find("RECONNECT") != std::string::npos) {
        post_status(line.c_str(), kColorSubtle);
        return;
    }

    if (line.find("INQUIRING START") != std::string::npos) {
        s_conn_state = ConnState::kScanning;
        {
            std::lock_guard<std::mutex> lock(s_devices_mutex);
            s_devices.clear();
        }
        post_clear_list();
        post_status(I18n::T("正在扫描..."), kColorScanning);
        return;
    }

    char address[kAddrHexLen + 1];
    char name[64];
    if (parse_bt_device_line(line, address, sizeof(address), name,
                             sizeof(name))) {
        BtDevice dev{};
        snprintf(dev.address, sizeof(dev.address), "%s", address);
        snprintf(dev.name, sizeof(dev.name), "%s", name);
        {
            std::lock_guard<std::mutex> lock(s_devices_mutex);
            for(const auto &known:s_devices)if(!strcasecmp(known.address,address))return;
            if(s_devices.size()>=64)return;
            s_devices.push_back(dev);
        }
        add_device_to_list(address, name);
        char status[96];
        snprintf(status, sizeof(status), I18n::T("发现设备: %s"),
                 name[0] ? name : address);
        post_status(status, kColorSubtle);
        return;
    }

    if (line.find("INQ COMPLETE") != std::string::npos) {
        s_conn_state = ConnState::kIdle;
        int count = 0;
        {
            std::lock_guard<std::mutex> lock(s_devices_mutex);
            count = static_cast<int>(s_devices.size());
        }
        char status[64];
        snprintf(status, sizeof(status), I18n::T("扫描完成，共 %d 个设备"), count);
        post_status(status, kColorSuccess);
        return;
    }

    if (line.find("DISCONNECT")!=std::string::npos||line.find("CONNECT FAIL")!=std::string::npos){s_conn_state=ConnState::kIdle;post_status("蓝牙连接已断开",kColorSubtle);return;}
    if (line.find("CONNECTING") != std::string::npos) {
        s_conn_state = ConnState::kConnecting;
        post_status(I18n::T("正在连接..."), kColorScanning);
        return;
    }

    if (line.find("CONNECT SUCCESS") != std::string::npos) {
        s_conn_state = ConnState::kConnected;
        post_status(I18n::T("连接成功"), kColorSuccess);
        return;
    }

    if (line.find("CONNECT TIMEOUT") != std::string::npos) {
        s_conn_state = ConnState::kIdle;
        post_status(I18n::T("连接失败 (超时)"), kColorError);
        return;
    }

    if (line.find("SETUP SCO") != std::string::npos) {
        post_status(I18n::T("通话模式 (SCO 已建立)"), kColorSuccess);
        return;
    }

    if (line.find("DISC SCO") != std::string::npos) {
        post_status(I18n::T("音乐模式 (SCO 已断开)"), kColorSuccess);
        return;
    }

    post_status(line.c_str(), kColorSubtle);
}

void on_uart_data(const std::vector<uint8_t> &data) {
    std::vector<std::string> lines;
    {std::lock_guard<std::mutex>lock(s_rx_mutex);
     if(!data.empty())s_rx_buffer.append((const char*)data.data(),data.size());
     size_t pos=0,nl;
     while((nl=s_rx_buffer.find_first_of("\r\n",pos))!=std::string::npos){if(nl>pos)lines.push_back(s_rx_buffer.substr(pos,nl-pos));pos=nl+1;}
     if(pos)s_rx_buffer.erase(0,pos);if(s_rx_buffer.size()>2048)s_rx_buffer.clear();}
    for(const auto &line:lines)handle_response_line(line);
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

lv_obj_t *MakeModeButton(lv_obj_t *parent, const char *label, int idx)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 56);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_add_event_cb(btn, OnModeClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(idx)));
    screen_swipe_back_ignore(btn, true);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

lv_obj_t *MakePanelButton(lv_obj_t *parent, const char *label, uint32_t bg,
                          lv_event_cb_t cb, lv_obj_t **out)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 48);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(btn, true);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(lbl);

    if (out != nullptr) {
        *out = btn;
    }
    return btn;
}

void BuildIntoInternal(lv_obj_t *parent)
{
    s_ui.screen = parent;

    // Transparent, scrollable vertical column — the body must NOT keep the
    // default LVGL light-theme card (white) background.
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(parent, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *desc = lv_label_create(parent);
    lv_label_set_text(desc, I18n::T("以下设置用于外置蓝牙音频解码芯片，非 ESP32-C5 内置蓝牙"));
    lv_obj_set_width(desc, LV_PCT(100));
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(desc, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(desc, &font_puhui_20_4, LV_PART_MAIN);

    // Reset row
    lv_obj_t *reset_row = lv_obj_create(parent);
    lv_obj_remove_style_all(reset_row);
    lv_obj_set_width(reset_row, LV_PCT(100));
    lv_obj_set_height(reset_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(reset_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reset_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(reset_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *reset_hint = lv_label_create(reset_row);
    lv_label_set_text(reset_hint, I18n::T("烧录蓝牙固件时使用"));
    lv_obj_set_style_text_color(reset_hint, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(reset_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_flex_grow(reset_hint, 1);

    lv_obj_t *reset = lv_button_create(reset_row);
    lv_obj_set_height(reset, 48);
    lv_obj_set_width(reset, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(reset, 20, LV_PART_MAIN);
    lv_obj_set_style_radius(reset, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(reset, lv_color_hex(0xC4761A), LV_PART_MAIN);
    lv_obj_add_event_cb(reset, OnResetClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(reset, true);

    lv_obj_t *reset_lbl = lv_label_create(reset);
    lv_label_set_text(reset_lbl, I18n::T("复位蓝牙"));
    lv_obj_set_style_text_color(reset_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(reset_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(reset_lbl);

    // Mode row
    lv_obj_t *mode_row = lv_obj_create(parent);
    lv_obj_remove_style_all(mode_row);
    lv_obj_set_width(mode_row, LV_PCT(100));
    lv_obj_set_height(mode_row, 56);
    lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(mode_row, 10, LV_PART_MAIN);
    lv_obj_remove_flag(mode_row, LV_OBJ_FLAG_SCROLLABLE);

    const char *mode_labels[] = {I18n::T("模式1"), I18n::T("模式2"),
                                 I18n::T("模式3")};
    for (int i = 0; i < 3; ++i) {
        s_ui.mode_btns[i] = MakeModeButton(mode_row, mode_labels[i], i);
    }

    // Status label
    lv_obj_t *status = lv_label_create(parent);
    s_ui.status_label = status;
    lv_label_set_text(status, I18n::T("请选择蓝牙模式"));
    lv_obj_set_style_text_color(status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_width(status, LV_PCT(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);

    // Mode 1 hint panel
    lv_obj_t *m1_panel = lv_obj_create(parent);
    s_ui.mode1_panel = m1_panel;
    lv_obj_remove_style_all(m1_panel);
    lv_obj_set_width(m1_panel, LV_PCT(100));
    lv_obj_set_height(m1_panel, 120);
    lv_obj_set_style_bg_color(m1_panel, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(m1_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(m1_panel, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(m1_panel, 16, LV_PART_MAIN);
    lv_obj_add_flag(m1_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(m1_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *m1_hint = lv_label_create(m1_panel);
    lv_label_set_text(m1_hint, I18n::T("模式1 已激活\n(AT+RX=2 / AT+MODE=1)"));
    lv_obj_set_style_text_color(m1_hint, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(m1_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(m1_hint);

    // Mode 2 scan/connect panel
    lv_obj_t *panel = lv_obj_create(parent);
    s_ui.mode2_panel = panel;
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_height(panel, 360);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *scan = lv_button_create(panel);
    s_ui.scan_btn = scan;
    lv_obj_set_size(scan, 180, 48);
    lv_obj_align(scan, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(scan, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scan, lv_color_hex(kColorBtnActive),
                              LV_PART_MAIN);
    lv_obj_add_event_cb(scan, OnScanClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(scan, true);

    lv_obj_t *scan_lbl = lv_label_create(scan);
    lv_label_set_text(scan_lbl, I18n::T("扫描设备"));
    lv_obj_set_style_text_color(scan_lbl, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(scan_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(scan_lbl);

    lv_obj_t *list = lv_obj_create(panel);
    s_ui.device_list = list;
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_PCT(100), 200);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x12151C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(list, true);

    lv_obj_t *hint = lv_label_create(list);
    lv_label_set_text(hint, I18n::T("暂无设备"));
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(hint);

    lv_obj_t *btn_row = lv_obj_create(panel);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, 48);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 12, LV_PART_MAIN);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    MakePanelButton(btn_row, I18n::T("音乐模式"), kColorBtn, OnMusicModeClicked,
                    &s_ui.music_btn);
    MakePanelButton(btn_row, I18n::T("通话模式"), kColorBtn, OnCallModeClicked,
                    &s_ui.call_btn);
}

void OnSwipeBack()
{
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    HomeScreen::SwitchToHome();
}

void OnBackClicked(lv_event_t * /*e*/)
{
    OnSwipeBack();
}

void OnScreenUnloaded(lv_event_t * /*e*/)
{
    ++s_ui_epoch;++s_scan_epoch;
    s_screen_active = false;
    s_ui.screen = nullptr;
    s_ui.status_label = nullptr;
    s_ui.mode1_panel = nullptr;
    s_ui.mode2_panel = nullptr;
    s_ui.scan_btn = nullptr;
    s_ui.device_list = nullptr;
    s_ui.music_btn = nullptr;
    s_ui.call_btn = nullptr;
    for (auto &b : s_ui.mode_btns) {
        b = nullptr;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void BluetoothScreen::BuildInto(lv_obj_t *parent)
{
    if (parent == nullptr) {
        return;
    }
    s_screen_active = true;
    {std::lock_guard<std::mutex>lock(s_rx_mutex);s_rx_buffer.clear();}
    {
        std::lock_guard<std::mutex> lock(s_devices_mutex);
        s_devices.clear();
    }
    BuildIntoInternal(parent);
    RestoreModeUi();
}

void BluetoothScreen::ResetUi()
{
    ++s_ui_epoch;++s_scan_epoch;
    s_screen_active = false;
    {std::lock_guard<std::mutex>lock(s_rx_mutex);s_rx_buffer.clear();}
    {
        std::lock_guard<std::mutex> lock(s_devices_mutex);
        s_devices.clear();
    }
    s_conn_state = ConnState::kIdle;
    s_ui.screen = nullptr;
    s_ui.status_label = nullptr;
    s_ui.mode1_panel = nullptr;
    s_ui.mode2_panel = nullptr;
    s_ui.scan_btn = nullptr;
    s_ui.device_list = nullptr;
    s_ui.music_btn = nullptr;
    s_ui.call_btn = nullptr;
    for (auto &b : s_ui.mode_btns) {
        b = nullptr;
    }
}

void BluetoothScreen::InitAudioObserver(){
 static std::atomic<bool> ready{false};if(ready.exchange(true))return;
 SimpleUart::getInstance().registerAudioObserver([](const std::vector<uint8_t>&data){
  s_observer_rx.append((const char*)data.data(),data.size());size_t end;
  while((end=s_observer_rx.find_first_of("\r\n"))!=std::string::npos){
   std::string line=s_observer_rx.substr(0,end);s_observer_rx.erase(0,end+1);if(line.empty())continue;
   if(s_profile_busy)ESP_LOGW(TAG,"profile RX: %.160s",line.c_str());
   if(line.find("SETUP SCO")!=std::string::npos){s_sco_up=true;s_a2dp_selected=false;metalio_bt_leave_voice();}
   if(line.find("DISC SCO")!=std::string::npos){s_sco_up=false;s_a2dp_selected=false;metalio_bt_leave_voice();}
   if(line.find("CONNECT SUCCESS")!=std::string::npos){if(!s_headset_linked.exchange(true))++s_link_epoch;s_conn_state=ConnState::kConnected;}
   if(line.find("DISCONNECT")!=std::string::npos||line.find("CONNECT FAIL")!=std::string::npos||line.find("CONNECT TIMEOUT")!=std::string::npos){++s_link_epoch;s_headset_linked=false;s_conn_state=ConnState::kIdle;s_sco_up=false;s_a2dp_selected=false;metalio_bt_leave_voice();}
   for(int mode=1;mode<=3;mode++){char token[24];snprintf(token,sizeof(token),"SET MODE %d",mode);
    if(line.find(token)!=std::string::npos){++s_link_epoch;s_headset_linked=false;s_conn_state=ConnState::kIdle;s_sco_up=false;s_a2dp_selected=false;metalio_bt_leave_voice();s_active_mode=(BtMode)mode;s_ack_mode=mode;}}
  }
  if(s_observer_rx.size()>2048)s_observer_rx.clear();
 });SimpleUart::getInstance().begin();
 const int saved=Settings("bluetooth").GetInt("mode",1);
 if(saved==2||saved==3){
  PhoneRouting().boot_restore=true;
  lv_timer_create([](lv_timer_t*t){
   if(!PhoneRouting().boot_restore){lv_timer_delete(t);return;}
   // Keep local clocks during speech-model initialization. The saved mode
   // is restored without opening AI or resetting the radio/modem.
   auto &audio=Application::GetInstance().GetAudioService();
   if(!audio.IsWakeWordEngineReady()||!audio.IsStarted()||audio.HasExclusiveCapture()||s_mode_busy||s_profile_busy||s_request_busy||s_phone_audio||metalio_nt26_get_call_state()!=METALIO_CALL_IDLE)return;
   PhoneRouting().boot_restore=false;
   BluetoothScreen::ApplyDefaultMode();lv_timer_delete(t);
  },250,nullptr);
 }
}
int WaitForAudioRouteIdle(){
 // Playback/record workers may arrive just after a mode/profile click.
 // Wait for that transaction instead of failing a healthy radio/file.
 for(int n=0;n<220;n++){
  if(CellularAudioActive()||(s_phone_audio&&metalio_nt26_get_call_state()!=METALIO_CALL_INCOMING))return -EBUSY;
  if(!s_mode_busy&&!s_profile_busy&&!s_request_busy)return 0;
  usleep(100000);
 }
 return -ETIMEDOUT;
}
int BluetoothScreen::PrepareVoiceRoute(){
 if(CellularAudioActive()||(s_phone_audio&&metalio_nt26_get_call_state()!=METALIO_CALL_INCOMING))return -EBUSY;
 int ready=WaitForAudioRouteIdle();if(ready<0)return ready;
 if(s_active_mode==BtMode::kMode2&&s_headset_linked){
  if(!s_sco_up&&!ProfileCommand(true))return -EIO;
  return AcceptHeadsetStream(true);
 }
 return metalio_bt_ensure_mode1();
}
int BluetoothScreen::PreparePlaybackRoute(){
 if(CellularAudioActive()||(s_phone_audio&&metalio_nt26_get_call_state()!=METALIO_CALL_INCOMING))return -EBUSY;
 int ready=WaitForAudioRouteIdle();if(ready<0)return ready;
 if(s_active_mode==BtMode::kMode2&&s_headset_linked){
  if(!s_a2dp_selected&&!ProfileCommand(false))return -EIO;
  return AcceptHeadsetStream(false);
 }
 return metalio_bt_ensure_mode1();
}

void BluetoothScreen::BeginPhoneAudio(){
 auto&r=PhoneRouting();
 {std::lock_guard<std::mutex> lock(s_phone_audio_mutex);
  if(!s_phone_audio.exchange(true)){r.before=s_active_mode.load();r.before_sco=s_sco_up.load();++r.generation;++r.request;r.preferred=-1;r.route=0;r.ending=false;}
  ++s_scan_epoch;metalio_bt_set_scan_mode(IsHeadsetConnected()||metalio_nt26_get_call_state()!=METALIO_CALL_INCOMING);
 }
 StartPhoneRouteWorker();
}
void BluetoothScreen::EndPhoneAudio(){
 auto&r=PhoneRouting();
 {std::lock_guard<std::mutex> lock(s_phone_audio_mutex);if(!s_phone_audio.exchange(false))return;++r.generation;++r.request;}
 StartPhoneRouteWorker();
}
int BluetoothScreen::SelectPhoneOutput(bool headset){
 if(!CellularAudioActive())return -ENOTCONN;
 BeginPhoneAudio();auto&r=PhoneRouting();unsigned generation,request;
 {std::lock_guard<std::mutex> lock(s_phone_audio_mutex);r.preferred=headset?1:0;generation=r.generation;request=++r.request;}
 std::lock_guard<std::mutex>lock(r.operation);return ApplyPhoneOutput(headset,generation,request);
}
void BluetoothScreen::CancelPhoneOutputRequest(){auto&r=PhoneRouting();r.ending=true;++r.request;}
int BluetoothScreen::PhoneAudioRoute(){
 auto&r=PhoneRouting();int route=r.route;
 if(!s_phone_audio||!CellularAudioActive())return 0;
 return route==2&&(!IsHeadsetConnected()||!s_sco_up)?0:route;
}
bool BluetoothScreen::IsVoiceModeForeground(){return s_screen_active&&s_active_mode==BtMode::kMode1&&!s_mode_busy;}
bool BluetoothScreen::IsHeadsetConnected(){
 // Clock readiness is also set by a valid mode-2 stream; it is not a
 // mode-1 indicator. Pairing is owned by the observed link/mode epochs.
 return metalio_bt_is_powered()&&s_active_mode==BtMode::kMode2&&s_headset_linked;
}
bool BluetoothScreen::IsHeadsetCaptureReady(){
 return IsHeadsetConnected()&&s_sco_up&&!s_profile_busy&&!s_mode_busy;
}

void BluetoothScreen::ApplyDefaultMode()
{
    int mode=Settings("bluetooth").GetInt("mode",1);
    if(mode<1||mode>3)mode=1;
    SendModeCommand((BtMode)mode);
}

lv_obj_t *BluetoothScreen::CreateStatic()
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    s_screen_active = true;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *header = lv_obj_create(scr);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20,
                            LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("蓝牙"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    // Body (transparent, scrollable) — the full BT control UI.
    lv_obj_t *body = lv_obj_create(scr);
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kPanelSize, kPanelSize - kHeaderH);
    lv_obj_set_pos(body, 0, kHeaderH);
    BuildIntoInternal(body);

    RestoreModeUi();

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}

lv_obj_t *BluetoothScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void BluetoothScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: bluetooth_screen");
        s_screen_active = true;
        {std::lock_guard<std::mutex>lock(s_rx_mutex);s_rx_buffer.clear();}
        SimpleUart::getInstance().begin();
        SimpleUart::getInstance().registerCallback(on_uart_data);
    } else {
        ESP_LOGI(TAG, "unload: bluetooth_screen");
        SimpleUart::getInstance().registerCallback(
            std::function<void(const std::vector<uint8_t> &)>());
        s_screen_active = false;
        ++s_scan_epoch;++s_ui_epoch;
    }
}
