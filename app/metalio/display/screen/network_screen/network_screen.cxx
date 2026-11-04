/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NetworkScreen — ported from MetalioClaw4
 * main/display/screen/network_screen/network_screen.cc (~2400 lines).
 *
 * Adaptations for openvela/NuttX:
 *   - ESP-IDF headers (esp_log / esp_event / esp_netif / esp_wifi /
 *     freertos) → NuttX shims (esp_log_shim.h / freertos_shim.h).
 *   - Wi-Fi, data-path and SIM operations use the real ESP-Hosted/NT26
 *     board drivers through NetworkService on background workers.
 *   - wifi_auth_mode_t / wifi_ap_record_t / WIFI_REASON_* constants
 *     are mirrored as local enums / structs so the rest of the code
 *     reads identically to the reference.
 *   - esp_restart() → Application::GetInstance().Reboot() (the App
 *     wrapper turns off the backlight first to avoid transition
 *     tearing, matching the original behaviour).
 *   - HomeScreen::Create() → HomeScreen::CreateStatic() (openvela
 *     port convention).
 *   - TaskHandle_t is pid_t (int) on NuttX; assignments use 0
 *     instead of nullptr. vTaskDelete(nullptr) is provided by the
 *     freertos shim.
 *
 * Preserved verbatim:
 *   - 720x720 layout: header (back + title), tabview with up to four
 *     tabs (附近WiFi / 已保存WiFi / 网络切换 / SIM卡切换).
 *   - Full password popup (modal mask + card + textarea + checkbox +
 *     cancel/connect buttons + embedded keyboard).
 *   - Connecting / success / failure / restart-countdown / switch-
 *     reboot / sim-switching popups (all modal masks with spinners
 *     and status text).
 *   - All event handlers (scan, nearby-item click, saved set-default
 *     / remove, clear-all, password connect / cancel / show-pwd,
 *     network wifi / cell switch, sim external / internal switch,
 *     back, swipe-back, screen-unloaded).
 *   - Auto-refresh of saved list / network switch UI / SIM slot UI.
 *   - 4G mode hides the two WiFi tabs and adds the SIM tab, matching
 *     the reference's tab-ordering logic.
 */

#include "network_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "esp_err_shim.h"
#include "freertos_shim.h"
#include "home_screen/home_screen.h"
#include "hi_input_method.h"

#include "application.h"
#include "settings.h"
#include "network_service.h"
extern "C" int metalio_app_select_sim(int slot);

#include <algorithm>
#include <atomic>
#include <new>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <metalio/metalio.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netutils/netlib.h>
#include <sys/socket.h>

/* nuttx/net/dns.h uses 'class' as a struct member — declare what we need. */
extern "C" int dns_add_nameserver(const struct sockaddr *addr,
                                  socklen_t addrlen);

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

// FreeRTOS priority baseline — NuttX sched.h has SCHED_PRIORITY_MIN; the
// ESP-IDF tskIDLE_PRIORITY symbol is not always defined through the shim
// layer, so provide a sane fallback.
#ifndef tskIDLE_PRIORITY
#define tskIDLE_PRIORITY 0
#endif

namespace {

constexpr const char *TAG = "NetworkScreen";

// ---------------------------------------------------------------------------
// 视觉常量
// ---------------------------------------------------------------------------
constexpr int kPanelW = 720;
constexpr int kPanelH = 720;
constexpr int kHeaderH = 90;

constexpr uint32_t kColorBg         = 0x0E1116;
constexpr uint32_t kColorCard       = 0x1B2030;
constexpr uint32_t kColorBtn        = 0x2A2F3A;
constexpr uint32_t kColorBtnActive  = 0x3B82F6;
constexpr uint32_t kColorBtnDanger  = 0xDC2626;
constexpr uint32_t kColorBtnAccent  = 0x10B981;
constexpr uint32_t kColorText       = 0xFFFFFF;
constexpr uint32_t kColorSubtle     = 0x9AA3B2;
constexpr uint32_t kColorSuccess    = 0x34C759;
constexpr uint32_t kColorError      = 0xFF3B30;
constexpr uint32_t kColorScanning   = 0xF59E0B;
constexpr uint32_t kColorListBg     = 0x12151C;
constexpr uint32_t kColorItem       = 0x202736;
constexpr uint32_t kColorItemSel    = 0x2F3A52;

constexpr size_t kMaxSsidLen = 32;
constexpr size_t kMaxPasswordLen = 64;

// ---------------------------------------------------------------------------
// 本地镜像：wifi_auth_mode_t / wifi_ap_record_t
//
// 原代码直接用 ESP-IDF 的 wifi_auth_mode_t 枚举与 wifi_ap_record_t 结构。
// openvela 没有 esp_wifi.h，所以本地复制一份等价的枚举 / 结构，让 UI
// 层的所有比较 / 显示逻辑保持原样。
// ---------------------------------------------------------------------------
enum WifiAuthMode {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK,
};

struct WifiApRecord {
    char ssid[33] = {};
    int8_t rssi = -127;
    WifiAuthMode authmode = WIFI_AUTH_OPEN;
};

// 上网方式：0 = WiFi，1 = 4G（蜂窝模组）。
constexpr int kNetTypeWifi     = 0;
constexpr int kNetTypeCellular = 1;

// SIM 卡槽位：0 = 外置卡（默认），1 = 内置卡。
constexpr int kSimSlotExternal = 0;
constexpr int kSimSlotInternal = 1;

// ---------------------------------------------------------------------------
// Backend interface
//
// Hardware operations are isolated from LVGL and dispatched to workers.
// The fallback below is used only when ESP-Hosted is disabled in Kconfig.
// ---------------------------------------------------------------------------
struct ApItem {
    std::string ssid;
    int8_t rssi = -127;
    WifiAuthMode authmode = WIFI_AUTH_OPEN;
};

struct SavedSsid {
    std::string ssid;
    std::string password;
};

struct ConnectResult {
    bool ok = false;
    std::string err;  // 失败原因（"密码错误" / "未找到该 WiFi" / ...）
};

const char *CellularFailureText(int error) {
    if (error == -EACCES) return I18n::T("SIM 卡已锁定，需要 PIN / PUK 解锁");
    if (error == -EPERM) return I18n::T("请先关闭飞行模式");
    if (error == -ENODEV) return I18n::T("未检测到可用的 SIM 卡");
    if (error == -EBUSY) return I18n::T("正在通话，请结束后重试");
    if (error == -ESTALE) return I18n::T("SIM 卡已切换，请重新打开");
    if (error == -ETIMEDOUT) return I18n::T("4G 连接超时，请检查信号和 APN");
    if (error == -EADDRNOTAVAIL) return I18n::T("4G 获取 IP 失败，请检查 APN");
    return I18n::T("4G 连接失败，请检查 SIM 卡、信号和套餐");
}

class NetworkScreenBackend {
public:
    virtual ~NetworkScreenBackend() = default;

    // 扫描附近 AP。返回 false 时 err 填写失败原因。
    virtual bool Scan(std::vector<ApItem> &out, std::string &err) = 0;

    // 连接到指定 SSID（开放网络 password 为空）。同步阻塞调用，由
    // 后台 task 触发。返回 false 时 err 填写失败原因。
    virtual ConnectResult Connect(const std::string &ssid,
                                  const std::string &password) = 0;

    // 已保存网络列表（首个为默认）。
    virtual std::vector<SavedSsid> GetSavedList() = 0;
    virtual void SetDefaultSsid(size_t index) = 0;
    virtual void RemoveSsid(size_t index) = 0;
    virtual void ClearAll() = 0;

    // 当前上网方式（WiFi / 4G）。默认 WiFi。
    virtual int GetNetworkType() = 0;
    // 切换上网方式（设备会重启）。返回 false 表示当前板子不支持切换。
    virtual bool SwitchNetworkType(int target_type) = 0;

    // 当前 SIM 槽位（外置 / 内置）。默认外置。
    virtual int GetSimSlot() = 0;
    // 切换 SIM 槽位（AT+CFUN=0 / AT+ECSIMCFG=SimSlot,X / AT+CFUN=1）。
    // 返回 false 表示当前不在 4G 模式 / 没检测到 4G 模组。
    virtual bool SwitchSimSlot(int target_slot) = 0;
    virtual int LastSwitchError() const { return 0; }
};

class StubNetworkScreenBackend : public NetworkScreenBackend {
public:
    bool Scan(std::vector<ApItem> &out, std::string &err) override {
        (void)err;
        // TODO(openvela): wire through the NuttX wifi stack once it is
        // exposed via Board / NetworkService.
        out.clear();
        return true;
    }

    ConnectResult Connect(const std::string & /*ssid*/,
                          const std::string & /*password*/) override {
        ConnectResult r;
        r.ok = false;
        r.err = "network stack not yet wired";
        return r;
    }

    std::vector<SavedSsid> GetSavedList() override { return std::vector<SavedSsid>(); }
    void SetDefaultSsid(size_t /*index*/) override {}
    void RemoveSsid(size_t /*index*/) override {}
    void ClearAll() override {}

    int GetNetworkType() override { return kNetTypeWifi; }
    bool SwitchNetworkType(int /*target_type*/) override { return false; }
    int GetSimSlot() override { return kSimSlotExternal; }
    bool SwitchSimSlot(int /*target_slot*/) override { return false; }
};

// 真实 backend：接 P4 SDIO Host + esp-hosted RPC，调用板级 metalio_esp_hosted_*
// 完成扫描、连接、已保存网络、双网络及 NT26 SIM 切换。
class EspHostedNetworkScreenBackend : public NetworkScreenBackend {
    std::atomic<int> last_error_{0};
public:
    int LastSwitchError() const override { return last_error_.load(); }
    bool Scan(std::vector<ApItem> &out, std::string &err) override {
        out.clear();

        int ret = metalio_esp_hosted_initialize();
        if (ret < 0) {
            err = "WiFi 驱动初始化失败";
            return false;
        }

        ret = metalio_esp_hosted_ensure_wifi_ready();
        if (ret < 0) {
            /* C5 lives on SDIO slot 1. A missing microSD is not the
             * failure. Bounce the link once and try again. */
            usleep(200 * 1000);
            (void)metalio_esp_hosted_initialize();
            ret = metalio_esp_hosted_ensure_wifi_ready();
        }
        if (ret < 0) {
            err = "WiFi 正在启动，请再点一次扫描";
            return false;
        }

        struct metalio_wifi_ap_s aps[METALIO_WIFI_MAX_AP];
        int n = metalio_esp_hosted_scan(aps, METALIO_WIFI_MAX_AP);
        if (n == -ENETDOWN) {
            usleep(200 * 1000);
            (void)metalio_esp_hosted_initialize();
            n = metalio_esp_hosted_scan(aps, METALIO_WIFI_MAX_AP);
        }
        if (n < 0) {
            if (n == -ENETDOWN) {
                err = "WiFi 链路未就绪，请再点一次扫描";
            } else if (n == -ETIMEDOUT) {
                err = "扫描超时：请稍后重试";
            } else {
                err = "扫描失败，请稍后重试";
            }
            return false;
        }

        for (int i = 0; i < n; i++) {
            ApItem item;
            item.ssid = aps[i].ssid;
            item.rssi = aps[i].rssi;
            item.authmode = static_cast<WifiAuthMode>(aps[i].authmode);
            out.push_back(std::move(item));
        }
        return true;
    }

    ConnectResult Connect(const std::string &ssid,
                          const std::string &password) override {
        ConnectResult r;
        NetworkService network;
        const int ret = network.StartWifi(ssid.c_str(), password.empty() ? nullptr : password.c_str());
        r.ok = ret == 0;
        if (ret == -ETIMEDOUT || ret == -ENOTCONN)
            r.err = "连接超时，请检查网络名称、密码和信号";
        else if (ret == -EADDRNOTAVAIL)
            r.err = "获取 IP 失败，请检查路由器";
        else if (ret < 0)
            r.err = "WiFi 连接失败，请稍后重试";
        return r;
    }

    std::vector<SavedSsid> GetSavedList() override {
        Settings wifi("wifi", false);
        std::string ssid = wifi.GetString("ssid");
        std::vector<SavedSsid> list;
        if (ssid.empty()) {
            return list;
        }
        SavedSsid item;
        item.ssid = std::move(ssid);
        item.password = wifi.GetString("password");
        list.push_back(std::move(item));
        return list;
    }

    void SetDefaultSsid(size_t /*index*/) override {}

    void RemoveSsid(size_t index) override {
        if (index != 0) {
            return;
        }
        Settings wifi("wifi", true);
        wifi.EraseKey("ssid");
        wifi.EraseKey("password");
    }

    void ClearAll() override {
        Settings wifi("wifi", true);
        wifi.EraseKey("ssid");
        wifi.EraseKey("password");
    }

    int GetNetworkType() override {
        int active = metalio_dual_network_path();
        if (active == 0 || active == 1) return active;
        Settings network("network", false);
        return network.GetInt("type", kNetTypeWifi);
    }

    bool SwitchNetworkType(int target_type) override {
        if (target_type != kNetTypeWifi && target_type != kNetTypeCellular)
            return false;
#ifndef CONFIG_METALIO_NT26_4G
        if (target_type == kNetTypeCellular) return false;
#endif
        const int result = metalio_dual_network_select(target_type == kNetTypeCellular);
        last_error_.store(result);
        if (result < 0)
            return false;
        Settings network("network", true);
        const char *keys[] = {"type", "cellular", "wifi_enabled", "airplane"};
        const int32_t values[] = {target_type, target_type == kNetTypeCellular,
                                 target_type == kNetTypeWifi, 0};
        if(!network.SetInts(keys, values, 4))
            ESP_LOGW(TAG,"Data path is connected; network preference save remains pending");
        return true; // A storage retry is not a failed cellular connection.
    }

    int GetSimSlot() override {
        const int actual = metalio_nt26_get_sim_slot();
        if (actual == kSimSlotExternal || actual == kSimSlotInternal)
            return actual;
        return -1; // A saved preference is not a confirmed modem slot.
    }

    bool SwitchSimSlot(int target_slot) override {
#ifdef CONFIG_METALIO_NT26_4G
        if (target_slot != kSimSlotExternal && target_slot != kSimSlotInternal)
            return false;
        // A locked/absent current card must not prevent selecting the other
        // slot while WiFi remains the active or restored data path.
        const int result = metalio_app_select_sim(target_slot);
        last_error_.store(result);
        if (result < 0) return false;
        Settings network("network", true);
        const char *keys[] = {"sim_slot", "type", "cellular", "wifi_enabled", "airplane"};
        const int32_t values[] = {target_slot, kNetTypeCellular, 1, 0, 0};
        if(!network.SetInts(keys, values, 5))
            ESP_LOGW(TAG,"SIM data path is connected; preference save remains pending");
        return true;
#else
        (void)target_slot;
        return false;
#endif
    }
};

NetworkScreenBackend *g_network_backend = nullptr;

NetworkScreenBackend *GetBackend() {
    if (g_network_backend == nullptr) {
#ifdef CONFIG_METALIO_ESP_HOSTED
        static EspHostedNetworkScreenBackend s_real;
        g_network_backend = &s_real;
#else
        static StubNetworkScreenBackend s_stub;
        g_network_backend = &s_stub;
#endif
    }
    return g_network_backend;
}

// ---------------------------------------------------------------------------
// 数据
// ---------------------------------------------------------------------------
struct UiState {
    lv_obj_t *screen        = nullptr;
    lv_obj_t *status_label  = nullptr;
    lv_obj_t *back_btn      = nullptr;
    lv_obj_t *scan_btn      = nullptr;
    lv_obj_t *scan_btn_lbl  = nullptr;
    lv_obj_t *tabview       = nullptr;
    lv_obj_t *nearby_tab    = nullptr;
    lv_obj_t *saved_tab     = nullptr;
    lv_obj_t *network_tab   = nullptr;
    lv_obj_t *sim_tab       = nullptr;
    lv_obj_t *nearby_list   = nullptr;
    lv_obj_t *nearby_spinner = nullptr;
    lv_obj_t *nearby_search = nullptr;
    lv_obj_t *nearby_keyboard = nullptr;
    lv_obj_t *saved_list    = nullptr;
    lv_obj_t *clear_btn     = nullptr;
    lv_obj_t *network_wifi_btn    = nullptr;
    lv_obj_t *network_wifi_lbl    = nullptr;
    lv_obj_t *network_cell_btn    = nullptr;
    lv_obj_t *network_cell_lbl    = nullptr;
    lv_obj_t *network_current_lbl = nullptr;
    lv_obj_t *sim_external_btn    = nullptr;
    lv_obj_t *sim_external_lbl    = nullptr;
    lv_obj_t *sim_internal_btn    = nullptr;
    lv_obj_t *sim_internal_lbl    = nullptr;
    lv_obj_t *sim_current_lbl     = nullptr;
    lv_obj_t *apn_overlay = nullptr;
    lv_obj_t *apn_fields[3] = {};
    lv_obj_t *apn_auth = nullptr;
    lv_obj_t *apn_keyboard = nullptr;
    lv_obj_t *pin_overlay = nullptr;
    lv_obj_t *pin_field = nullptr;
    lv_obj_t *pin_keyboard = nullptr;
    lv_obj_t *pin_hint = nullptr;
    int pin_slot = -1;
    lv_obj_t *pwd_overlay   = nullptr;
    lv_obj_t *pwd_textarea  = nullptr;
    lv_obj_t *pwd_keyboard  = nullptr;
    lv_obj_t *pwd_title     = nullptr;
    lv_obj_t *pwd_show_chk  = nullptr;
    lv_obj_t *status_overlay     = nullptr;
    lv_obj_t *status_message_lbl = nullptr;
};

UiState s_ui;
metalio_sim_profile_s s_edit_profile{};
int s_locked_slot = -1;

std::vector<ApItem>  s_scan_results;
std::string          s_nearby_filter;
bool                 s_screen_active = false;
unsigned             s_screen_session = 0;
std::atomic<bool>    s_scan_worker_busy{false};
std::atomic<bool>    s_connect_worker_busy{false};
std::atomic<bool>    s_switch_worker_busy{false};
bool                 s_scan_in_progress = false;
bool                 s_connect_in_progress = false;
bool                 s_auto_scan_on_next_load = false;
std::string          s_pending_ssid;
WifiAuthMode         s_pending_authmode = WIFI_AUTH_OPEN;

// 重启倒计时（连接成功 / 网络切换 / SIM 切换后用）。
constexpr int        kRestartCountdownSec = 3;
lv_timer_t          *s_restart_timer = nullptr;
int                  s_restart_remaining = 0;
std::string          s_restart_headline;
bool                 s_network_switch_pending = false;
bool                 s_sim_switch_pending = false;

// 前向声明
void post_status(const char *text, uint32_t color = kColorText);
void refresh_saved_list();
void refresh_nearby_list();
void rebuild_nearby_list_now();
void rebuild_saved_list_now();
void open_password_popup(const std::string &ssid, WifiAuthMode authmode);
void close_password_popup();
void schedule_scan();
void schedule_connect(const std::string &ssid, const std::string &password);
void open_connecting_popup(const std::string &ssid);
void open_activation_wait_popup(const std::string &ssid);
void post_open_activation_wait(const std::string &ssid);
void open_restart_countdown_popup(const std::string &headline);
void close_status_popup();
void show_failure_in_status_popup(const std::string &title,
                                  const std::string &detail,
                                  uint32_t auto_close_ms = 2500);
void refresh_network_switch_ui();
void open_switch_reboot_popup(const char *target_name);
void schedule_network_switch(int target_type);
void refresh_sim_slot_ui();
void open_sim_switching_popup(int target_slot);
void schedule_sim_switch(int target_slot);

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
const char *auth_label(WifiAuthMode mode) {
    return (mode == WIFI_AUTH_OPEN) ? I18n::T("[开放]") : I18n::T("[加密]");
}

const char *rssi_quality_text(int8_t rssi) {
    if (rssi >= -55) return I18n::T("信号强");
    if (rssi >= -65) return I18n::T("信号较强");
    if (rssi >= -75) return I18n::T("信号中");
    if (rssi >= -85) return I18n::T("信号弱");
    return I18n::T("信号很弱");
}

bool screen_alive() { return s_screen_active && s_ui.screen != nullptr; }

bool IsCellularMode() {
    return GetBackend()->GetNetworkType() == kNetTypeCellular;
}

const char *SimSlotName(int slot) {
    if (slot != kSimSlotInternal && slot != kSimSlotExternal) return I18n::T("未知");
    return (slot == kSimSlotInternal) ? I18n::T("内置卡") : I18n::T("外置卡");
}

// 把 (part | state) 显式转成 lv_style_selector_t，规避
// -Wdeprecated-enum-enum-conversion 告警。
inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

// ---------------------------------------------------------------------------
// 异步 UI 更新（worker task -> LVGL 线程）
// ---------------------------------------------------------------------------
struct AsyncStatusMsg {
    char text[160];
    uint32_t color;
    unsigned session;
};

void async_update_status(void *user_data) {
    auto *msg = static_cast<AsyncStatusMsg *>(user_data);
    if (msg->session == s_screen_session && screen_alive() && s_ui.status_label != nullptr) {
        lv_label_set_text(s_ui.status_label, msg->text);
        lv_obj_set_style_text_color(s_ui.status_label, lv_color_hex(msg->color), LV_PART_MAIN);
    }
    delete msg;
}

void post_status(const char *text, uint32_t color) {
    if (!s_screen_active) return;
    auto *msg = new AsyncStatusMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text);
    msg->color = color;
    msg->session = s_screen_session;
    if (lv_async_call(async_update_status, msg) != LV_RESULT_OK) delete msg;
}

void async_rebuild_nearby(void * /*user_data*/) {
    if (screen_alive()) {
        rebuild_nearby_list_now();
    }
}

void async_rebuild_saved(void * /*user_data*/) {
    if (screen_alive()) {
        rebuild_saved_list_now();
    }
}

void refresh_nearby_list() {
    if (!s_screen_active) return;
    lv_async_call(async_rebuild_nearby, nullptr);
}

void refresh_saved_list() {
    if (!s_screen_active) return;
    lv_async_call(async_rebuild_saved, nullptr);
}

bool ContainsSearchText(const std::string &value, const std::string &query) {
    if (query.empty()) return true;
    if (value.find(query) != std::string::npos) return true;
    std::string folded_value(value), folded_query(query);
    std::transform(folded_value.begin(), folded_value.end(), folded_value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(folded_query.begin(), folded_query.end(), folded_query.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return folded_value.find(folded_query) != std::string::npos;
}

void on_nearby_search_changed(lv_event_t *e) {
    auto *ta = lv_event_get_target_obj(e);
    s_nearby_filter = lv_textarea_get_text(ta) ? lv_textarea_get_text(ta) : "";
    rebuild_nearby_list_now();
}

void on_nearby_search_focused(lv_event_t *e) {
    if (s_ui.nearby_keyboard != nullptr)
        HiInputMethod_SetTextArea(s_ui.nearby_keyboard,
                                  lv_event_get_target_obj(e), true);
}

void async_set_scan_btn_enabled(void *user_data) {
    if (!screen_alive() || s_ui.scan_btn == nullptr) return;
    const bool enabled = (user_data != nullptr);
    if (enabled) {
        lv_obj_remove_state(s_ui.scan_btn, LV_STATE_DISABLED);
        if (s_ui.scan_btn_lbl != nullptr) lv_label_set_text(s_ui.scan_btn_lbl, I18n::T("扫描"));
    } else {
        lv_obj_add_state(s_ui.scan_btn, LV_STATE_DISABLED);
        if (s_ui.scan_btn_lbl != nullptr) lv_label_set_text(s_ui.scan_btn_lbl, I18n::T("扫描中…"));
    }
}

void post_scan_btn_enabled(bool enabled) {
    if (!s_screen_active) return;
    lv_async_call(async_set_scan_btn_enabled,
                  reinterpret_cast<void *>(static_cast<intptr_t>(enabled ? 1 : 0)));
}

// ---- 「附近 WiFi」列表中央的圆环 spinner --------------------------------
void async_set_nearby_spinner(void *user_data) {
    if (!screen_alive() || s_ui.nearby_spinner == nullptr) return;
    const bool show = (user_data != nullptr);
    if (show) {
        lv_obj_remove_flag(s_ui.nearby_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ui.nearby_spinner);
    } else {
        lv_obj_add_flag(s_ui.nearby_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

void post_nearby_spinner(bool show) {
    if (!s_screen_active) return;
    lv_async_call(async_set_nearby_spinner,
                  reinterpret_cast<void *>(static_cast<intptr_t>(show ? 1 : 0)));
}

struct AsyncStringMsg {
    std::string text;
};

void async_open_connecting(void *user_data) {
    auto *msg = static_cast<AsyncStringMsg *>(user_data);
    if (screen_alive()) open_connecting_popup(msg->text);
    delete msg;
}

void post_open_connecting(const std::string &ssid) {
    if (!s_screen_active) return;
    lv_async_call(async_open_connecting, new AsyncStringMsg{ssid});
}

void async_close_status(void * /*user_data*/) {
    if (screen_alive()) close_status_popup();
}

void post_close_status_popup() {
    if (!s_screen_active) return;
    lv_async_call(async_close_status, nullptr);
}

void async_close_password(void * /*user_data*/) {
    if (screen_alive()) close_password_popup();
}

void post_close_password_popup() {
    if (!s_screen_active) return;
    lv_async_call(async_close_password, nullptr);
}

void async_return_home(void * /*user_data*/) {
    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    s_restart_remaining = 0;
    if (screen_alive()) {
        close_status_popup();
        close_password_popup();
    }
    HomeScreen::SwitchToHome();
    HomeScreen::RefreshStatusBar();
}

void post_return_home() {
    lv_async_call(async_return_home, nullptr);
}

struct AsyncFailureMsg {
    std::string title;
    std::string detail;
    uint32_t    auto_close_ms;
};

void async_show_failure(void *user_data) {
    auto *msg = static_cast<AsyncFailureMsg *>(user_data);
    if (screen_alive()) {
        show_failure_in_status_popup(msg->title, msg->detail, msg->auto_close_ms);
    }
    delete msg;
}

void post_show_failure(const std::string &title, const std::string &detail,
                       uint32_t auto_close_ms = 2500) {
    if (!s_screen_active) return;
    lv_async_call(async_show_failure,
                  new AsyncFailureMsg{title, detail, auto_close_ms});
}

// ---------------------------------------------------------------------------
// 扫描任务
//
// 原实现通过 esp_wifi_scan_start + xEventGroupWaitBits 等待 SCAN_DONE。
// stub backend 的 Scan() 是同步的，但 task 结构保留——方便真实 backend
// 落地后直接换实现而无需改 UI 层。
// ---------------------------------------------------------------------------
struct ScanResultMsg {
    unsigned session;
    bool ok;
    std::vector<ApItem> results;
    std::string error;
};
void async_scan_done(void *data) {
    auto *msg = static_cast<ScanResultMsg *>(data);
    if (msg->session == s_screen_session && screen_alive()) {
        s_scan_in_progress = false;
        if (msg->ok) {
            std::vector<int> order;
            for (size_t i=0; i<msg->results.size(); ++i) order.push_back(i);
            std::sort(order.begin(), order.end(), [msg](int a, int b) {
                return msg->results[a].rssi > msg->results[b].rssi;
            });
            // Only the LVGL thread owns the vector used by list callbacks.
            s_scan_results.clear();
            for (int i : order) {
                const auto &ap = msg->results[i];
                if (ap.ssid.empty()) continue;
                bool duplicate = false;
                for (const auto &existing : s_scan_results)
                    if (existing.ssid == ap.ssid) { duplicate = true; break; }
                if (!duplicate) s_scan_results.push_back(ap);
            }
            char status[64];
            snprintf(status, sizeof(status), I18n::T("扫描完成，共 %d 个网络"),
                     static_cast<int>(s_scan_results.size()));
            post_status(status, kColorSuccess);
        } else post_status(msg->error.c_str(), kColorError);
        rebuild_nearby_list_now();
        async_set_nearby_spinner(nullptr);
        async_set_scan_btn_enabled(reinterpret_cast<void *>(1));
    }
    msg->~ScanResultMsg(); free(msg);
}
void scan_task(void *data) {
    auto *msg = static_cast<ScanResultMsg *>(data);
    msg->ok = GetBackend()->Scan(msg->results, msg->error);
    s_scan_worker_busy.store(false);
    if (lv_async_call(async_scan_done, msg) != LV_RESULT_OK) {
        msg->~ScanResultMsg(); free(msg);
    }
    vTaskDelete(nullptr);
}
void schedule_scan() {
    if (s_connect_worker_busy.load() || s_scan_worker_busy.exchange(true)) return;
    void *mem = malloc(sizeof(ScanResultMsg));
    if (!mem) { s_scan_worker_busy.store(false); return; }
    auto *msg = new (mem) ScanResultMsg{};
    msg->session = s_screen_session;
    s_scan_in_progress = true;
    async_set_scan_btn_enabled(nullptr);
    async_set_nearby_spinner(reinterpret_cast<void *>(1));
    post_status(I18n::T("正在扫描附近 WiFi…"), kColorScanning);
    if (xTaskCreateLegacy(scan_task, "wifi_scan", 8192, msg, 5, nullptr) != pdPASS) {
        msg->~ScanResultMsg(); free(msg);
        s_scan_worker_busy.store(false); s_scan_in_progress = false;
        async_set_scan_btn_enabled(reinterpret_cast<void *>(1));
        async_set_nearby_spinner(nullptr);
        post_status(I18n::T("无法启动扫描任务"), kColorError);
    }
}

// ---------------------------------------------------------------------------
// 连接任务
// ---------------------------------------------------------------------------
struct ConnectCtx {
    std::string ssid;
    std::string password;
    unsigned session;
    ConnectResult result;
};
void async_connect_done(void *data) {
    auto *ctx = static_cast<ConnectCtx *>(data);
    if (ctx->session == s_screen_session && screen_alive()) {
        s_connect_in_progress = false;
        if (ctx->result.ok) {
            close_status_popup(); close_password_popup();
            HomeScreen::SwitchToHome(); HomeScreen::RefreshStatusBar();
        } else {
            show_failure_in_status_popup(I18n::T("连接失败"), ctx->result.err, 2500);
        }
    }
    delete ctx;
}
void connect_task(void *arg) {
    auto *ctx = static_cast<ConnectCtx *>(arg);
    ctx->result = GetBackend()->Connect(ctx->ssid, ctx->password);
    if (ctx->result.ok) {
        Settings wifi("wifi", true);
        wifi.SetString("ssid", ctx->ssid);
        wifi.SetString("password", ctx->password);
        Application::GetInstance().OnWifiLinkReady();
    }
    s_connect_worker_busy.store(false);
    if (lv_async_call(async_connect_done, ctx) != LV_RESULT_OK) delete ctx;
    vTaskDelete(nullptr);
}

void schedule_connect(const std::string &ssid, const std::string &password) {
    if (s_connect_worker_busy.load() || s_scan_worker_busy.load()) {
        post_status(I18n::T("已有正在进行的连接任务"), kColorScanning);
        return;
    }
    if (ssid.empty() || ssid.size() > kMaxSsidLen) {
        post_status(I18n::T("SSID 不合法"), kColorError);
        return;
    }
    if (password.size() > kMaxPasswordLen) {
        post_status(I18n::T("密码超长"), kColorError);
        return;
    }
    auto *ctx = new ConnectCtx{ssid, password, s_screen_session, {}};
    s_connect_worker_busy.store(true);
    write(1, "SC0\n", 4);
    s_connect_in_progress = true;
    // 立刻弹出转圈遮罩，覆盖密码键盘，让用户得到「我点了连接」的视觉反馈。
    open_connecting_popup(ssid);
    write(1, "SC1\n", 4);
    if (xTaskCreateLegacy(connect_task, "wifi_connect", 8192, ctx, 5, nullptr) != pdPASS) {
        write(1, "SC2\n", 4);
        delete ctx;
        s_connect_worker_busy.store(false);
        s_connect_in_progress = false;
        post_status(I18n::T("无法启动连接任务"), kColorError);
        close_status_popup();
    }
}

// ---------------------------------------------------------------------------
// 列表渲染
// ---------------------------------------------------------------------------
struct NearbyClickCtx {
    char ssid[kMaxSsidLen + 1];
    int authmode;  // WifiAuthMode
};

void on_nearby_item_clicked(lv_event_t *e) {
    auto *ctx = static_cast<NearbyClickCtx *>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    open_password_popup(ctx->ssid, static_cast<WifiAuthMode>(ctx->authmode));
}

void on_nearby_item_delete(lv_event_t *e) {
    auto *ctx = static_cast<NearbyClickCtx *>(lv_event_get_user_data(e));
    delete ctx;
}

void rebuild_nearby_list_now() {
    if (s_ui.nearby_list == nullptr) return;
    // 不能用 lv_obj_clean()：会把 nearby_spinner 这个浮层子物件也一起删掉。
    for (int32_t i = static_cast<int32_t>(
                         lv_obj_get_child_count(s_ui.nearby_list)) - 1;
         i >= 0; --i) {
        lv_obj_t *child = lv_obj_get_child(s_ui.nearby_list, i);
        if (child != nullptr && child != s_ui.nearby_spinner) {
            lv_obj_delete(child);
        }
    }

    size_t visible_count = 0;
    for (const auto &ap : s_scan_results)
        if (ContainsSearchText(ap.ssid, s_nearby_filter)) ++visible_count;

    if (visible_count == 0) {
        if (s_scan_in_progress) {
            return;
        }
        lv_obj_t *hint = lv_label_create(s_ui.nearby_list);
        lv_label_set_text(hint, s_nearby_filter.empty()
            ? I18n::T("未发现网络，点右上「扫描」试试")
            : I18n::T("没有匹配的 WiFi 网络"));
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_pad_all(hint, 16, LV_PART_MAIN);
        return;
    }

    for (auto &ap : s_scan_results) {
        if (!ContainsSearchText(ap.ssid, s_nearby_filter)) continue;
        lv_obj_t *item = lv_button_create(s_ui.nearby_list);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, 60);
        lv_obj_set_style_radius(item, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(item, lv_color_hex(kColorItem), LV_PART_MAIN);
        lv_obj_set_style_bg_color(item, lv_color_hex(kColorItemSel),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(item, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(item, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_opa(item, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(item, 0, LV_PART_MAIN);

        auto *ctx = new NearbyClickCtx{};
        snprintf(ctx->ssid, sizeof(ctx->ssid), "%s", ap.ssid.c_str());
        ctx->authmode = static_cast<int>(ap.authmode);
        lv_obj_add_event_cb(item, on_nearby_item_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(item, on_nearby_item_delete, LV_EVENT_DELETE, ctx);

        // 左侧：加密标记 + SSID
        lv_obj_t *left = lv_label_create(item);
        char ltext[96];
        snprintf(ltext, sizeof(ltext), "%s %s", auth_label(ap.authmode),
                 ap.ssid.c_str());
        lv_label_set_text(left, ltext);
        lv_label_set_long_mode(left, LV_LABEL_LONG_DOT);
        lv_obj_set_width(left, kPanelW - 40 - 200);
        lv_obj_set_style_text_color(left, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(left, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(left, LV_ALIGN_LEFT_MID, 0, 0);

        // 右侧：信号描述 + dBm
        lv_obj_t *right = lv_label_create(item);
        char rtext[48];
        snprintf(rtext, sizeof(rtext), "%s  %d dBm",
                 rssi_quality_text(ap.rssi), ap.rssi);
        lv_label_set_text(right, rtext);
        lv_obj_set_style_text_color(right, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(right, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

// 已保存 WiFi 列表项的按钮上下文（每行两个：置顶 / 删除）
struct SavedActionCtx {
    int index;
};

void on_saved_set_default(lv_event_t *e) {
    auto *ctx = static_cast<SavedActionCtx *>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    GetBackend()->SetDefaultSsid(static_cast<size_t>(ctx->index));
    post_status(I18n::T("已设置为默认网络"), kColorSuccess);
    refresh_saved_list();
}

void on_saved_remove(lv_event_t *e) {
    auto *ctx = static_cast<SavedActionCtx *>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    GetBackend()->RemoveSsid(static_cast<size_t>(ctx->index));
    post_status(I18n::T("已删除该网络"), kColorSuccess);
    refresh_saved_list();
}

void on_saved_btn_delete(lv_event_t *e) {
    delete static_cast<SavedActionCtx *>(lv_event_get_user_data(e));
}

void on_clear_all_saved(lv_event_t * /*e*/) {
    GetBackend()->ClearAll();
    post_status(I18n::T("已清空所有已保存网络"), kColorSuccess);
    refresh_saved_list();
}

void rebuild_saved_list_now() {
    if (s_ui.saved_list == nullptr) return;
    lv_obj_clean(s_ui.saved_list);

    auto list = GetBackend()->GetSavedList();

    if (list.empty()) {
        lv_obj_t *hint = lv_label_create(s_ui.saved_list);
        lv_label_set_text(hint, I18n::T("暂无已连接过的 WiFi"));
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_pad_all(hint, 16, LV_PART_MAIN);
        if (s_ui.clear_btn != nullptr) {
            lv_obj_add_state(s_ui.clear_btn, LV_STATE_DISABLED);
        }
        return;
    }
    if (s_ui.clear_btn != nullptr) {
        lv_obj_remove_state(s_ui.clear_btn, LV_STATE_DISABLED);
    }

    for (size_t i = 0; i < list.size(); ++i) {
        const auto &item = list[i];

        lv_obj_t *row = lv_obj_create(s_ui.saved_list);
        screen_strip_obj_chrome(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 60);
        lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorItem), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 14, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // SSID 文本（带「默认」标记，列表中第 0 项是默认）
        lv_obj_t *lbl = lv_label_create(row);
        char ttext[96];
        if (i == 0) {
            snprintf(ttext, sizeof(ttext), I18n::T("%s  (默认)"), item.ssid.c_str());
        } else {
            snprintf(ttext, sizeof(ttext), "%s", item.ssid.c_str());
        }
        lv_label_set_text(lbl, ttext);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, kPanelW - 40 - 260);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        // 「置顶」按钮（i == 0 时禁用）
        lv_obj_t *def_btn = lv_button_create(row);
        lv_obj_set_size(def_btn, 110, 44);
        lv_obj_align(def_btn, LV_ALIGN_RIGHT_MID, -120, 0);
        lv_obj_set_style_radius(def_btn, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(def_btn, lv_color_hex(kColorBtnAccent), LV_PART_MAIN);
        lv_obj_set_style_border_width(def_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_width(def_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(def_btn, 0, LV_PART_MAIN);
        auto *def_ctx = new SavedActionCtx{static_cast<int>(i)};
        lv_obj_add_event_cb(def_btn, on_saved_set_default, LV_EVENT_CLICKED, def_ctx);
        lv_obj_add_event_cb(def_btn, on_saved_btn_delete, LV_EVENT_DELETE, def_ctx);
        if (i == 0) lv_obj_add_state(def_btn, LV_STATE_DISABLED);
        lv_obj_t *def_lbl = lv_label_create(def_btn);
        lv_label_set_text(def_lbl, I18n::T("设为默认"));
        lv_obj_set_style_text_color(def_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(def_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(def_lbl);

        // 「删除」按钮
        lv_obj_t *del_btn = lv_button_create(row);
        lv_obj_set_size(del_btn, 100, 44);
        lv_obj_align(del_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(del_btn, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(del_btn, lv_color_hex(kColorBtnDanger), LV_PART_MAIN);
        lv_obj_set_style_border_width(del_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_width(del_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(del_btn, 0, LV_PART_MAIN);
        auto *del_ctx = new SavedActionCtx{static_cast<int>(i)};
        lv_obj_add_event_cb(del_btn, on_saved_remove, LV_EVENT_CLICKED, del_ctx);
        lv_obj_add_event_cb(del_btn, on_saved_btn_delete, LV_EVENT_DELETE, del_ctx);
        lv_obj_t *del_lbl = lv_label_create(del_btn);
        lv_label_set_text(del_lbl, I18n::T("删除"));
        lv_obj_set_style_text_color(del_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(del_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(del_lbl);
    }
}

// ---------------------------------------------------------------------------
// 密码输入弹窗（模态遮罩 + 文本框 + 键盘）
// ---------------------------------------------------------------------------
void on_show_pwd_changed(lv_event_t *e) {
    lv_obj_t *chk = lv_event_get_target_obj(e);
    if (s_ui.pwd_textarea == nullptr) return;
    const bool checked = lv_obj_has_state(chk, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(s_ui.pwd_textarea, !checked);
}

void on_pwd_connect_btn(lv_event_t * /*e*/) {
    if (s_ui.pwd_textarea == nullptr) return;
    if (s_ui.pwd_keyboard != nullptr &&
        !HiInputMethod_Flush(s_ui.pwd_keyboard)) return;
    const char *pwd = lv_textarea_get_text(s_ui.pwd_textarea);
    schedule_connect(s_pending_ssid, pwd ? pwd : "");
}

void on_pwd_cancel_btn(lv_event_t * /*e*/) { close_password_popup(); }

void open_password_popup(const std::string &ssid, WifiAuthMode authmode) {
    if (s_ui.screen == nullptr) return;
    close_password_popup();

    s_pending_ssid = ssid;
    s_pending_authmode = authmode;

    // 全屏半透明遮罩
    lv_obj_t *mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.pwd_overlay = mask;

    // 顶部信息卡
    lv_obj_t *card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kPanelW - 60, 260);
    lv_obj_set_pos(card, 30, 30);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    s_ui.pwd_title = title;
    char ttext[128];
    snprintf(ttext, sizeof(ttext), I18n::T("连接到: %s"), ssid.c_str());
    lv_label_set_text(title, ttext);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, kPanelW - 60 - 40);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint,
                      authmode == WIFI_AUTH_OPEN
                          ? I18n::T("该网络无需密码，可直接连接")
                          : I18n::T("请输入 WiFi 密码（8~63 字符）"));
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align_to(hint, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);

    // 密码输入框
    lv_obj_t *ta = lv_textarea_create(card);HiInputMethod_Bind(ta,false);
    s_ui.pwd_textarea = ta;
    lv_obj_set_size(ta, kPanelW - 60 - 40, 60);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, 92);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_max_length(ta, kMaxPasswordLen);
    lv_textarea_set_placeholder_text(ta, I18n::T("WiFi 密码"));
    lv_obj_set_style_text_font(ta, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x121726), LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_radius(ta, 10, LV_PART_MAIN);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    screen_swipe_back_ignore(ta, true);

    // 显示密码 checkbox
    lv_obj_t *chk = lv_checkbox_create(card);
    s_ui.pwd_show_chk = chk;
    lv_checkbox_set_text(chk, I18n::T("显示密码"));
    lv_obj_align(chk, LV_ALIGN_TOP_LEFT, 0, 168);
    lv_obj_set_style_text_color(chk, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(chk, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_add_event_cb(chk, on_show_pwd_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    screen_swipe_back_ignore(chk, true);

    // 取消 / 连接按钮
    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 160, 56);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -180, 160);
    lv_obj_set_style_radius(cancel, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel, on_pwd_cancel_btn, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, I18n::T("取消"));
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(cancel_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);
    screen_swipe_back_ignore(cancel, true);

    lv_obj_t *connect = lv_button_create(card);
    lv_obj_set_size(connect, 160, 56);
    lv_obj_align(connect, LV_ALIGN_TOP_RIGHT, 0, 160);
    lv_obj_set_style_radius(connect, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(connect, lv_color_hex(kColorBtnActive), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(connect, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(connect, on_pwd_connect_btn, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *connect_lbl = lv_label_create(connect);
    lv_label_set_text(connect_lbl, I18n::T("连接"));
    lv_obj_set_style_text_color(connect_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(connect_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(connect_lbl);
    screen_swipe_back_ignore(connect, true);

    // Reuse the same phone-style pinyin panel used by OpenClaw and weather.
    lv_obj_t *kb = HiInputMethod_Create(mask);
    if (kb == nullptr) return;
    s_ui.pwd_keyboard = kb;
    lv_obj_set_size(kb, kPanelW, 350);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    HiInputMethod_SetTextArea(kb, ta, false);
    lv_obj_add_event_cb(ta, [](lv_event_t *e) {
        if (s_ui.pwd_keyboard != nullptr)
            HiInputMethod_SetTextArea(s_ui.pwd_keyboard, lv_event_get_target_obj(e), false);
    }, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(kb, true);
}

void close_password_popup() {
    if (s_ui.pwd_overlay != nullptr) {
        lv_obj_delete(s_ui.pwd_overlay);
    }
    s_ui.pwd_overlay  = nullptr;
    s_ui.pwd_textarea = nullptr;
    s_ui.pwd_keyboard = nullptr;
    s_ui.pwd_title    = nullptr;
    s_ui.pwd_show_chk = nullptr;
    s_pending_ssid.clear();
}

// ---------------------------------------------------------------------------
// 连接进度 / 成功弹窗
// ---------------------------------------------------------------------------
void close_status_popup() {
    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    if (s_ui.status_overlay != nullptr) {
        lv_obj_delete(s_ui.status_overlay);
    }
    s_ui.status_overlay     = nullptr;
    s_ui.status_message_lbl = nullptr;
    s_restart_remaining = 0;
    s_restart_headline.clear();
}

void open_connecting_popup(const std::string &ssid) {
    if (s_ui.screen == nullptr) return;
    close_status_popup();

    lv_obj_t *mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t *card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 520, 360);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t *spin = lv_spinner_create(card);
    lv_obj_set_size(spin, 140, 140);
    lv_obj_align(spin, LV_ALIGN_TOP_MID, 0, 20);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtnActive), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);

    lv_obj_t *lbl = lv_label_create(card);
    s_ui.status_message_lbl = lbl;
    char buf[160];
    snprintf(buf, sizeof(buf), I18n::T("正在连接\n%s …"), ssid.c_str());
    lv_label_set_text(lbl, buf);
    lv_obj_set_width(lbl, 520 - 48);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -16);
}

static int s_activation_wait_ticks = 0;

void activation_wait_timer_cb(lv_timer_t * /*timer*/) {
    if (!screen_alive() || s_ui.status_message_lbl == nullptr) {
        if (s_restart_timer != nullptr) {
            lv_timer_delete(s_restart_timer);
            s_restart_timer = nullptr;
        }
        return;
    }

    auto &app = Application::GetInstance();
    if (app.HasPendingActivation()) {
        if (s_restart_timer != nullptr) {
            lv_timer_delete(s_restart_timer);
            s_restart_timer = nullptr;
        }
        close_status_popup();
        /* Claw4: show「验证码: NNNNNN」on the home status bar (top center,
         * beside the clock) — not on the network settings overlay. */
        HomeScreen::SwitchToHome();
        HomeScreen::RefreshStatusBar();
        return;
    }

    s_activation_wait_ticks++;
    if (s_activation_wait_ticks >= 90) { /* ~90s @ 1Hz */
        lv_label_set_text(s_ui.status_message_lbl,
                          I18n::T("WiFi 已连接\n获取验证码超时，请返回首页重试"));
        if (s_restart_timer != nullptr) {
            lv_timer_delete(s_restart_timer);
            s_restart_timer = nullptr;
        }
        return;
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             I18n::T("WiFi 已连接\n正在获取小智验证码… (%d)"),
             s_activation_wait_ticks);
    lv_label_set_text(s_ui.status_message_lbl, buf);
}

void open_activation_wait_popup(const std::string &ssid) {
    if (s_ui.screen == nullptr) return;
    close_status_popup();

    lv_obj_t *mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t *card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 520, 360);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t *spin = lv_spinner_create(card);
    lv_obj_set_size(spin, 100, 100);
    lv_obj_align(spin, LV_ALIGN_TOP_MID, 0, 16);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtnAccent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);

    lv_obj_t *lbl = lv_label_create(card);
    s_ui.status_message_lbl = lbl;
    char buf[160];
    snprintf(buf, sizeof(buf),
             I18n::T("已连接 %s\n正在获取小智验证码…"), ssid.c_str());
    lv_label_set_text(lbl, buf);
    lv_obj_set_width(lbl, 520 - 48);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -16);

    s_activation_wait_ticks = 0;
    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    s_restart_timer = lv_timer_create(activation_wait_timer_cb, 1000, nullptr);
}

void async_open_activation_wait(void *user_data) {
    auto *msg = static_cast<AsyncStringMsg *>(user_data);
    if (screen_alive()) open_activation_wait_popup(msg->text);
    delete msg;
}

void post_open_activation_wait(const std::string &ssid) {
    if (!s_screen_active) return;
    lv_async_call(async_open_activation_wait, new AsyncStringMsg{ssid});
}

void reboot_task(void * /*arg*/) {
    ESP_LOGI(TAG, "network config done -> Application::Reboot()");
    // 给 LVGL 一点点时间把 「正在重启…」 渲染出来
    vTaskDelay(pdMS_TO_TICKS(200));
    // 走 App 重启：先关背光再 esp_restart，避免过渡花屏
    Application::GetInstance().Reboot();
    vTaskDelete(nullptr);
}

void restart_timer_cb(lv_timer_t * /*timer*/) {
    s_restart_remaining--;
    if (s_restart_remaining > 0) {
        if (s_ui.status_message_lbl != nullptr) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     I18n::T("%s\n设备将在 %d 秒后自动重启…"),
                     s_restart_headline.c_str(), s_restart_remaining);
            lv_label_set_text(s_ui.status_message_lbl, buf);
        }
        return;
    }

    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    if (s_ui.status_message_lbl != nullptr) {
        lv_label_set_text(s_ui.status_message_lbl, I18n::T("正在重启…"));
    }
    xTaskCreateLegacy(reboot_task, "wifi_reboot", 2048, nullptr, 5, nullptr);
}

// 失败提示自动关闭：复用 s_restart_timer 槽位，到期回调里只做收掉遮罩。
void failure_close_timer_cb(lv_timer_t * /*timer*/) {
    s_restart_timer = nullptr;
    close_status_popup();
}

// 把当前的 status_overlay 替换成「失败」卡片，N 毫秒后自动关闭遮罩。
void show_failure_in_status_popup(const std::string &title,
                                  const std::string &detail,
                                  uint32_t auto_close_ms) {
    if (s_ui.screen == nullptr) return;

    if (s_ui.status_overlay == nullptr) {
        lv_obj_t *mask = lv_obj_create(s_ui.screen);
        screen_strip_obj_chrome(mask);
        lv_obj_set_size(mask, kPanelW, kPanelH);
        lv_obj_set_pos(mask, 0, 0);
        lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
        lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
        screen_swipe_back_ignore(mask, true);
        s_ui.status_overlay = mask;
    } else {
        lv_obj_clean(s_ui.status_overlay);
        s_ui.status_message_lbl = nullptr;
    }

    lv_obj_t *card = lv_obj_create(s_ui.status_overlay);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 520, 320);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t *head = lv_label_create(card);
    lv_label_set_text(head, title.c_str());
    lv_obj_set_style_text_color(head, lv_color_hex(kColorBtnDanger), LV_PART_MAIN);
    lv_obj_set_style_text_font(head, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *body = lv_label_create(card);
    s_ui.status_message_lbl = body;
    lv_label_set_text(body, detail.c_str());
    lv_obj_set_width(body, 520 - 48);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 20);

    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    s_restart_timer = lv_timer_create(failure_close_timer_cb, auto_close_ms, nullptr);
    lv_timer_set_repeat_count(s_restart_timer, 1);
}

void open_restart_countdown_popup(const std::string &headline) {
    if (s_ui.screen == nullptr) return;
    close_status_popup();

    lv_obj_t *mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t *card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 520, 320);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t *check = lv_label_create(card);
    lv_label_set_text(check, I18n::T("成功"));
    lv_obj_set_style_text_color(check, lv_color_hex(kColorBtnAccent), LV_PART_MAIN);
    lv_obj_set_style_text_font(check, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(check, LV_ALIGN_TOP_MID, 0, 20);

    s_restart_headline = headline;
    s_restart_remaining = kRestartCountdownSec;

    lv_obj_t *lbl = lv_label_create(card);
    s_ui.status_message_lbl = lbl;
    char buf[160];
    snprintf(buf, sizeof(buf), I18n::T("%s\n设备将在 %d 秒后自动重启…"),
             s_restart_headline.c_str(), s_restart_remaining);
    lv_label_set_text(lbl, buf);
    lv_obj_set_width(lbl, 520 - 48);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -16);

    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
    }
    s_restart_timer = lv_timer_create(restart_timer_cb, 1000, nullptr);
}

// ---------------------------------------------------------------------------
// 头部按钮 / 屏幕生命周期
// ---------------------------------------------------------------------------
void on_scan_clicked(lv_event_t * /*e*/) { schedule_scan(); }

void on_back_clicked(lv_event_t * /*e*/);

void on_swipe_back() {
    // 弹窗存在时优先关闭弹窗，不触发返回。
    if (s_ui.status_overlay != nullptr) {
        return;
    }
    if (s_ui.pwd_overlay != nullptr) {
        close_password_popup();
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    HomeScreen::SwitchToHome();
}

void on_back_clicked(lv_event_t * /*e*/) { on_swipe_back(); }

void on_screen_unloaded(lv_event_t * /*e*/) {
    ++s_screen_session;
    s_screen_active = false;
    s_ui.screen        = nullptr;
    s_ui.status_label  = nullptr;
    s_ui.back_btn      = nullptr;
    s_ui.scan_btn      = nullptr;
    s_ui.scan_btn_lbl  = nullptr;
    s_ui.tabview       = nullptr;
    s_ui.nearby_tab    = nullptr;
    s_ui.saved_tab       = nullptr;
    s_ui.network_tab     = nullptr;
    s_ui.sim_tab         = nullptr;
    s_ui.nearby_list     = nullptr;
    s_ui.nearby_spinner  = nullptr;
    s_ui.nearby_search   = nullptr;
    s_ui.nearby_keyboard = nullptr;
    s_ui.saved_list      = nullptr;
    s_ui.clear_btn       = nullptr;
    s_ui.network_wifi_btn    = nullptr;
    s_ui.network_wifi_lbl    = nullptr;
    s_ui.network_cell_btn    = nullptr;
    s_ui.network_cell_lbl    = nullptr;
    s_ui.network_current_lbl = nullptr;
    s_ui.sim_external_btn = nullptr;
    s_ui.sim_external_lbl = nullptr;
    s_ui.sim_internal_btn = nullptr;
    s_ui.sim_internal_lbl = nullptr;
    s_ui.sim_current_lbl  = nullptr;
    s_ui.apn_overlay = nullptr;
    for (auto &field : s_ui.apn_fields) field = nullptr;
    s_ui.apn_auth = nullptr;
    s_ui.apn_keyboard = nullptr;
    s_ui.pin_overlay = nullptr;
    s_ui.pin_field = nullptr;
    s_ui.pin_keyboard = nullptr;
    s_ui.pin_hint = nullptr;
    s_ui.pin_slot = -1;
    s_locked_slot = -1;
    s_network_switch_pending = false;
    s_sim_switch_pending = false;
    s_ui.pwd_overlay   = nullptr;
    s_ui.pwd_textarea  = nullptr;
    s_ui.pwd_keyboard  = nullptr;
    s_ui.pwd_title     = nullptr;
    s_ui.pwd_show_chk  = nullptr;
    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    s_ui.status_overlay     = nullptr;
    s_ui.status_message_lbl = nullptr;
    s_restart_remaining = 0;
    s_restart_headline.clear();
    s_pending_ssid.clear();
    s_scan_results.clear();
    s_nearby_filter.clear();
}

// ---------------------------------------------------------------------------
// 网络切换（WiFi <-> 4G）
// ---------------------------------------------------------------------------
struct NetworkSwitchJob {
    int target;
    unsigned session;
    bool success;
    int error;
};
void async_network_switch_done(void *data) {
    auto *job = static_cast<NetworkSwitchJob *>(data);
    if (job->session == s_screen_session && screen_alive()) {
        if(!job->success && metalio_dual_network_path()==job->target && NetworkService::Available())
            job->success=true;
        s_network_switch_pending = false;
        close_status_popup();
        if (job->success) {
            if (job->target == kNetTypeCellular) s_locked_slot = -1;
            post_status(I18n::T("网络切换成功"), kColorBtnAccent);
        } else {
            if (job->target == kNetTypeCellular && job->error == -EACCES)
                s_locked_slot = GetBackend()->GetSimSlot();
            post_status(job->target == kNetTypeCellular ?
                CellularFailureText(job->error) :
                I18n::T("WiFi 连接失败，请检查已保存的网络"), kColorError);
        }
        refresh_network_switch_ui();
    }
    free(job);
}
void switch_network_task(void *arg) {
    auto *job = static_cast<NetworkSwitchJob *>(arg);
    job->success = GetBackend()->SwitchNetworkType(job->target);
    job->error = GetBackend()->LastSwitchError();
    lv_lock();
    if (lv_async_call(async_network_switch_done, job) != LV_RESULT_OK) free(job);
    lv_unlock();
    s_switch_worker_busy.store(false);
    vTaskDelete(nullptr);
}

void open_switch_reboot_popup(const char *target_name) {
    if (s_ui.screen == nullptr) {
        return;
    }
    close_status_popup();

    lv_obj_t *mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t *card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 520, 320);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t *head = lv_label_create(card);
    lv_label_set_text(head, I18n::T("切换网络"));
    lv_obj_set_style_text_color(head, lv_color_hex(kColorBtnActive), LV_PART_MAIN);
    lv_obj_set_style_text_font(head, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *body = lv_label_create(card);
    s_ui.status_message_lbl = body;
    char buf[160];
    snprintf(buf, sizeof(buf), I18n::T("正在切换到 %s\n设备即将重启…"), target_name);
    lv_label_set_text(body, buf);
    lv_obj_set_width(body, 520 - 48);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 20);
}

void schedule_network_switch(int target_type) {
    if (s_network_switch_pending || s_sim_switch_pending || s_switch_worker_busy.load()) {
        return;
    }
    if (target_type != kNetTypeWifi && target_type != kNetTypeCellular) {
        return;
    }
    if (metalio_dual_network_path() == target_type && NetworkService::Available()) {
        return;
    }

    if (s_switch_worker_busy.exchange(true)) return;
    s_network_switch_pending = true;
    post_status(target_type == kNetTypeCellular ? I18n::T("准备切换到 4G…")
                                                 : I18n::T("准备切换到 WiFi…"),
                kColorScanning);
    refresh_network_switch_ui();
    auto *job = static_cast<NetworkSwitchJob *>(calloc(1, sizeof(NetworkSwitchJob)));
    if (job) { job->target = target_type; job->session = s_screen_session; }
    if (!job || xTaskCreateLegacy(switch_network_task, "net_switch", 8192,
                            job, 5, nullptr) != pdPASS) {
        free(job);
        s_switch_worker_busy.store(false);
        s_network_switch_pending = false;
        post_status(I18n::T("无法启动切换任务"), kColorError);
        close_status_popup();
        refresh_network_switch_ui();
    }
}

void on_network_wifi_clicked(lv_event_t * /*e*/) {
    schedule_network_switch(kNetTypeWifi);
}

void on_network_cell_clicked(lv_event_t * /*e*/) {
    schedule_network_switch(kNetTypeCellular);
}

void refresh_network_switch_ui() {
    if (s_ui.network_wifi_btn == nullptr ||
        s_ui.network_cell_btn == nullptr) {
        return;
    }
    const int type = GetBackend()->GetNetworkType();
    const bool is_wifi = (type != kNetTypeCellular);

    lv_obj_set_style_bg_color(s_ui.network_wifi_btn,
                              lv_color_hex(is_wifi ? kColorBtnActive
                                                   : kColorBtn),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.network_cell_btn,
                              lv_color_hex(is_wifi ? kColorBtn
                                                   : kColorBtnActive),
                              LV_PART_MAIN);

    if (s_ui.network_current_lbl != nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), I18n::T("当前：%s"), is_wifi ? "WiFi" : "4G");
        lv_label_set_text(s_ui.network_current_lbl, buf);
    }
}

// ---------------------------------------------------------------------------
// SIM 卡切换（仅 4G 模式）
// ---------------------------------------------------------------------------
void refresh_sim_slot_ui() {
    if (s_ui.sim_external_btn == nullptr || s_ui.sim_internal_btn == nullptr) {
        return;
    }
    const int slot = GetBackend()->GetSimSlot();
    const bool is_external = (slot == kSimSlotExternal);

    lv_obj_set_style_bg_color(s_ui.sim_external_btn,
                              lv_color_hex(is_external ? kColorBtnActive
                                                       : kColorBtn),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.sim_internal_btn,
                              lv_color_hex(slot == kSimSlotInternal ? kColorBtnActive : kColorBtn),
                              LV_PART_MAIN);

    if (s_ui.sim_current_lbl != nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), I18n::T("当前：%s"), SimSlotName(slot));
        lv_label_set_text(s_ui.sim_current_lbl, buf);
    }
}

void close_apn_popup() {
    if (s_ui.apn_overlay) lv_obj_delete(s_ui.apn_overlay);
    s_ui.apn_overlay = nullptr;
    for (auto &field : s_ui.apn_fields) field = nullptr;
    s_ui.apn_keyboard = nullptr;
    s_ui.apn_auth = nullptr;
}

struct ApnApplyCtx {
    metalio_sim_profile_s profile;
    unsigned session;
    int result;
};

void apn_apply_task(void *arg) {
    auto *ctx = static_cast<ApnApplyCtx *>(arg);
    ctx->result = metalio_app_configure_sim_profile(&ctx->profile);
    if (lv_async_call([](void *data) {
        auto *result = static_cast<ApnApplyCtx *>(data);
        if (result->session == s_screen_session && screen_alive()) {
            s_sim_switch_pending = false;
            post_status(result->result == 0 ? I18n::T("已连接") :
                        CellularFailureText(result->result),
                        result->result == 0 ? kColorBtnAccent : kColorError);
            refresh_sim_slot_ui();
        }
        free(result);
    }, ctx) != LV_RESULT_OK) free(ctx);
    s_switch_worker_busy.store(false);
    vTaskDelete(nullptr);
}

void on_apn_save(lv_event_t *) {
    if (!s_ui.apn_overlay || !s_ui.apn_keyboard ||
        !HiInputMethod_Flush(s_ui.apn_keyboard) ||
        s_switch_worker_busy.exchange(true)) return;
    auto *ctx = static_cast<ApnApplyCtx *>(calloc(1, sizeof(ApnApplyCtx)));
    if (!ctx) { s_switch_worker_busy.store(false); return; }
    ctx->profile = s_edit_profile;
    snprintf(ctx->profile.apn, sizeof(ctx->profile.apn), "%s",
             lv_textarea_get_text(s_ui.apn_fields[0]));
    snprintf(ctx->profile.username, sizeof(ctx->profile.username), "%s",
             lv_textarea_get_text(s_ui.apn_fields[1]));
    snprintf(ctx->profile.password, sizeof(ctx->profile.password), "%s",
             lv_textarea_get_text(s_ui.apn_fields[2]));
    ctx->profile.auth = lv_dropdown_get_selected(s_ui.apn_auth);
    ctx->session = s_screen_session;
    close_apn_popup();
    s_sim_switch_pending = true;
    post_status(I18n::T("正在连接…"), kColorScanning);
    if (xTaskCreateLegacy(apn_apply_task, "sim_apn", 8192, ctx, 5, nullptr) != pdPASS) {
        free(ctx); s_sim_switch_pending = false; s_switch_worker_busy.store(false);
        post_status(I18n::T("系统忙"), kColorError);
    }
}

void show_apn_popup();

void on_apn_clicked(lv_event_t *) {
    if (!screen_alive() || s_switch_worker_busy.exchange(true)) return;
    auto *ctx = static_cast<ApnApplyCtx *>(calloc(1, sizeof(ApnApplyCtx)));
    if (!ctx) { s_switch_worker_busy.store(false); return; }
    ctx->session = s_screen_session;
    post_status(I18n::T("正在读取 SIM 卡…"), kColorScanning);
    if (xTaskCreateLegacy([](void *arg) {
        auto *ctx = static_cast<ApnApplyCtx *>(arg);
        ctx->result = metalio_app_read_sim_profile(&ctx->profile);
        if (lv_async_call([](void *data) {
            auto *result = static_cast<ApnApplyCtx *>(data);
            if (result->session == s_screen_session && screen_alive()) {
                if (result->result == 0) {
                    s_edit_profile = result->profile;
                    post_status("", kColorSubtle);
                    show_apn_popup();
                } else post_status(CellularFailureText(result->result), kColorError);
            }
            free(result);
        }, ctx) != LV_RESULT_OK) free(ctx);
        s_switch_worker_busy.store(false);
        vTaskDelete(nullptr);
    }, "sim_read", 8192, ctx, 5, nullptr) != pdPASS) {
        free(ctx); s_switch_worker_busy.store(false);
        post_status(I18n::T("系统忙"), kColorError);
    }
}

void show_apn_popup() {
    close_apn_popup();
    auto *mask = lv_obj_create(s_ui.screen);
    s_ui.apn_overlay = mask;
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);
    auto *card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_pos(card, 20, 10);
    lv_obj_set_size(card, kPanelW-40, 330);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    auto *title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("APN 设置（留空为自动）"));
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    const char *hints[] = {"APN", I18n::T("用户名"), I18n::T("密码")};
    const char *values[] = {s_edit_profile.apn, s_edit_profile.username, s_edit_profile.password};
    for (int i = 0; i < 3; ++i) {
        auto *field = lv_textarea_create(card);HiInputMethod_Bind(field,false);
        s_ui.apn_fields[i] = field;
        lv_obj_set_pos(field, 0, 34+i*48);
        lv_obj_set_size(field, kPanelW-64, 44);
        lv_textarea_set_one_line(field, true);
        lv_textarea_set_max_length(field, i == 0 ? METALIO_APN_MAX-1 : METALIO_APN_USER_MAX-1);
        lv_textarea_set_placeholder_text(field, hints[i]);
        lv_textarea_set_password_mode(field, i == 2);
        lv_textarea_set_text(field, values[i]);
        lv_obj_set_style_text_font(field, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_add_event_cb(field, [](lv_event_t *e) {
            if (s_ui.apn_keyboard)
                HiInputMethod_SetTextArea(s_ui.apn_keyboard, lv_event_get_target_obj(e), false);
        }, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(field, true);
    }
    auto *auth = lv_dropdown_create(card);
    s_ui.apn_auth = auth;
    lv_obj_set_pos(auth, 0, 180); lv_obj_set_size(auth, kPanelW-64, 44);
    lv_dropdown_set_options(auth, "No authentication\nPAP\nCHAP\nPAP / CHAP");
    lv_dropdown_set_selected(auth, s_edit_profile.auth);
    lv_obj_set_style_text_font(auth, &font_puhui_20_4, LV_PART_MAIN);
    screen_swipe_back_ignore(auth, true);
    const char *buttons[] = {I18n::T("取消"), I18n::T("自动"), I18n::T("保存并连接")};
    for (int i = 0; i < 3; ++i) {
        auto *btn = lv_button_create(card);
        lv_obj_set_pos(btn, i*216, 244); lv_obj_set_size(btn, 202, 50);
        auto *label = lv_label_create(btn);
        lv_label_set_text(label, buttons[i]);
        lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(label); screen_swipe_back_ignore(btn, true);
        if (i == 0) lv_obj_add_event_cb(btn, [](lv_event_t *) { close_apn_popup(); }, LV_EVENT_CLICKED, nullptr);
        else if (i == 1) lv_obj_add_event_cb(btn, [](lv_event_t *) {
            if (s_ui.apn_keyboard) (void)HiInputMethod_Flush(s_ui.apn_keyboard);
            for (auto *field : s_ui.apn_fields) if (field) lv_textarea_set_text(field, "");
            if (s_ui.apn_auth) lv_dropdown_set_selected(s_ui.apn_auth, 0);
        }, LV_EVENT_CLICKED, nullptr);
        else lv_obj_add_event_cb(btn, on_apn_save, LV_EVENT_CLICKED, nullptr);
    }
    s_ui.apn_keyboard = HiInputMethod_Create(mask);
    if (s_ui.apn_keyboard) {
        lv_obj_set_size(s_ui.apn_keyboard, kPanelW, 350);
        lv_obj_align(s_ui.apn_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        HiInputMethod_SetTextArea(s_ui.apn_keyboard, s_ui.apn_fields[0], false);
        screen_swipe_back_ignore(s_ui.apn_keyboard, true);
    }
}

void open_sim_switching_popup(int target_slot) {
    if (s_ui.screen == nullptr) return;
    close_status_popup();

    lv_obj_t *mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t *card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 520, 360);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(card, true);

    lv_obj_t *spin = lv_spinner_create(card);
    lv_obj_set_size(spin, 120, 120);
    lv_obj_align(spin, LV_ALIGN_TOP_MID, 0, 16);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtnActive),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);

    lv_obj_t *lbl = lv_label_create(card);
    s_ui.status_message_lbl = lbl;
    char buf[160];
    snprintf(buf, sizeof(buf), I18n::T("正在切换到%s…\nAT+CFUN=0"),
             SimSlotName(target_slot));
    lv_label_set_text(lbl, buf);
    lv_obj_set_width(lbl, 520 - 48);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -16);
}

struct SimSwitchCtx {
    int target_slot;
    unsigned session;
    bool success;
    const char *detail;
    int error;
};
using SimSwitchResultMsg = SimSwitchCtx;

void async_sim_switch_done(void *user_data) {
    auto *msg = static_cast<SimSwitchResultMsg *>(user_data);
    if (msg->session == s_screen_session && screen_alive()) {
        s_sim_switch_pending = false;
        if (msg->success) {
            s_locked_slot = -1;
            char buf[96];
            snprintf(buf, sizeof(buf), I18n::T("已切换到%s"),
                     SimSlotName(msg->target_slot));
            close_status_popup();
            post_status(buf, kColorBtnAccent);
            refresh_sim_slot_ui(); refresh_network_switch_ui();
        } else {
            close_status_popup();
            if (msg->error == -EACCES) s_locked_slot = msg->target_slot;
            post_status(msg->detail, kColorError);

            refresh_sim_slot_ui(); refresh_network_switch_ui();
        }
    }
    free(msg);
}

void sim_switch_task(void *arg) {
    auto *ctx = static_cast<SimSwitchCtx *>(arg);
    auto *result = ctx;
    result->success = GetBackend()->SwitchSimSlot(ctx->target_slot);
    result->error = GetBackend()->LastSwitchError();
    if (!result->success) {
        result->detail = CellularFailureText(result->error);
    }
    lv_lock();
    if (lv_async_call(async_sim_switch_done, result) != LV_RESULT_OK) free(result);
    lv_unlock();
    s_switch_worker_busy.store(false);
    vTaskDelete(nullptr);
}

void schedule_sim_switch(int target_slot) {
    if (s_sim_switch_pending || s_network_switch_pending || s_switch_worker_busy.load()) {
        return;
    }
    if (target_slot != kSimSlotExternal && target_slot != kSimSlotInternal) {
        return;
    }
    // Selecting the current slot explicitly retries service after a card change.

    if (s_switch_worker_busy.exchange(true)) return;
    s_sim_switch_pending = true;
    auto *ctx = static_cast<SimSwitchCtx *>(calloc(1, sizeof(SimSwitchCtx)));
    if (ctx) { ctx->target_slot = target_slot; ctx->session = s_screen_session; }
    post_status(I18n::T("正在切换 SIM 卡…"), kColorScanning);
    if (!ctx || xTaskCreateLegacy(sim_switch_task, "sim_switch", 8192, ctx, 5, nullptr) != pdPASS) {
        free(ctx);
        s_switch_worker_busy.store(false);
        s_sim_switch_pending = false;
        close_status_popup();
        post_status(I18n::T("无法启动 SIM 切换任务"), kColorError);
    }
}

void on_sim_external_clicked(lv_event_t * /*e*/) {
    schedule_sim_switch(kSimSlotExternal);
}

void on_sim_internal_clicked(lv_event_t * /*e*/) {
    schedule_sim_switch(kSimSlotInternal);
}

struct SimPinJob {
    char pin[9];
    int slot;
    int result;
    unsigned session;
};

void close_pin_popup() {
    if (s_ui.pin_field) lv_textarea_set_text(s_ui.pin_field, "");
    if (s_ui.pin_overlay) lv_obj_delete(s_ui.pin_overlay);
    s_ui.pin_overlay = s_ui.pin_field = s_ui.pin_keyboard = nullptr;
    s_ui.pin_hint = nullptr;
    s_ui.pin_slot = -1;
}

void on_pin_submit(lv_event_t *) {
    if (!s_ui.pin_field || s_switch_worker_busy.load()) return;
    const char *pin = lv_textarea_get_text(s_ui.pin_field);
    const size_t length = strlen(pin);
    if (length < 4 || length > 8) {
        if (s_ui.pin_hint) lv_label_set_text(s_ui.pin_hint, I18n::T("请输入 4 到 8 位正确的 PIN"));
        return;
    }
    auto *job = static_cast<SimPinJob *>(calloc(1, sizeof(SimPinJob)));
    if (!job) return;
    snprintf(job->pin, sizeof(job->pin), "%s", pin);
    job->slot = s_ui.pin_slot; job->session = s_screen_session;
    close_pin_popup();
    if (s_switch_worker_busy.exchange(true)) {
        for (volatile char *p = job->pin; p < job->pin + sizeof(job->pin); ++p) *p = 0;
        free(job); return;
    }
    s_sim_switch_pending = true;
    post_status(I18n::T("正在连接…"), kColorScanning);
    if (xTaskCreateLegacy([](void *arg) {
        auto *job = static_cast<SimPinJob *>(arg);
        job->result = metalio_app_unlock_sim(job->pin, job->slot);
        for (volatile char *p = job->pin; p < job->pin + sizeof(job->pin); ++p) *p = 0;
        if (lv_async_call([](void *data) {
            auto *job = static_cast<SimPinJob *>(data);
            if (job->session == s_screen_session && screen_alive()) {
                s_sim_switch_pending = false;
                if (job->result == 0) s_locked_slot = -1;
                post_status(job->result == 0 ? I18n::T("已连接") : CellularFailureText(job->result),
                            job->result == 0 ? kColorBtnAccent : kColorError);
                refresh_sim_slot_ui(); refresh_network_switch_ui();
            }
            free(job);
        }, job) != LV_RESULT_OK) free(job);
        s_switch_worker_busy.store(false);
        vTaskDelete(nullptr);
    }, "sim_pin", 8192, job, 5, nullptr) != pdPASS) {
        for (volatile char *p = job->pin; p < job->pin + sizeof(job->pin); ++p) *p = 0;
        free(job); s_sim_switch_pending = false; s_switch_worker_busy.store(false);
        post_status(I18n::T("系统忙"), kColorError);
    }
}

void on_pin_clicked(lv_event_t *) {
    if (!screen_alive() || s_switch_worker_busy.load()) return;
    close_pin_popup();
    s_ui.pin_slot = s_locked_slot >= 0 ? s_locked_slot : GetBackend()->GetSimSlot();
    if (s_ui.pin_slot != 0 && s_ui.pin_slot != 1) {
        post_status(I18n::T("未检测到可用的 SIM 卡"), kColorError); return;
    }
    auto *mask = lv_obj_create(s_ui.screen); s_ui.pin_overlay = mask;
    screen_strip_obj_chrome(mask); lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0); lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(mask, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, LV_PART_MAIN);
    screen_swipe_back_ignore(mask, true);
    auto *title = lv_label_create(mask);
    char heading[64]; snprintf(heading, sizeof(heading), "%s SIM PIN", SimSlotName(s_ui.pin_slot));
    lv_label_set_text(title, heading);
    lv_obj_set_pos(title, 30, 20); lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    auto *hint = lv_label_create(mask); s_ui.pin_hint = hint;
    lv_label_set_text(hint, I18n::T("解锁成功后自动记忆此卡 PIN。\n需要 PUK 时请联系运营商。"));
    lv_obj_set_pos(hint, 30, 70); lv_obj_set_width(hint, 660);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    auto *field = lv_textarea_create(mask);HiInputMethod_Bind(field,false); s_ui.pin_field = field;
    lv_obj_set_pos(field, 30, 146); lv_obj_set_size(field, 660, 64);
    lv_textarea_set_one_line(field, true); lv_textarea_set_password_mode(field, true);
    lv_textarea_set_accepted_chars(field, "0123456789"); lv_textarea_set_max_length(field, 8);
    lv_textarea_set_placeholder_text(field, "PIN");
    lv_obj_set_style_text_font(field, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(field, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_text_color(field, lv_color_hex(kColorText), LV_PART_MAIN);
    screen_swipe_back_ignore(field, true);
    const char *labels[] = {I18n::T("取消"), I18n::T("连接")};
    for (int i = 0; i < 2; ++i) {
        auto *button = lv_button_create(mask); lv_obj_set_pos(button, 30+i*340, 244);
        lv_obj_set_size(button, 320, 60); screen_swipe_back_ignore(button, true);
        auto *label = lv_label_create(button); lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN); lv_obj_center(label);
        if (i == 0) lv_obj_add_event_cb(button, [](lv_event_t *) { close_pin_popup(); }, LV_EVENT_CLICKED, nullptr);
        else lv_obj_add_event_cb(button, on_pin_submit, LV_EVENT_CLICKED, nullptr);
    }
    auto *keyboard = HiInputMethod_Create(mask); s_ui.pin_keyboard = keyboard;
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(kColorBtn), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(kColorText), LV_PART_ITEMS);
    static lv_font_t pin_keyboard_font;
    pin_keyboard_font = font_puhui_20_4;
    pin_keyboard_font.fallback = LV_FONT_DEFAULT;
    lv_obj_set_style_text_font(keyboard, &pin_keyboard_font, LV_PART_ITEMS);

    lv_obj_set_size(keyboard, kPanelW, 350); lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    HiInputMethod_SetTextArea(keyboard, field, false); screen_swipe_back_ignore(keyboard, true);
}

// ---------------------------------------------------------------------------
// UI 组装
// ---------------------------------------------------------------------------
void build_header(lv_obj_t *parent) {
    lv_obj_t *header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int kBackBtnSize = 72;
    lv_obj_t *back = lv_button_create(header);
    s_ui.back_btn = back;
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF), Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_20, Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, on_back_clicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("网络配置"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);
}

void build_tabview(lv_obj_t *parent) {
    constexpr int y = kHeaderH;
    constexpr int h = kPanelH - y;
    constexpr int kTabBarH = 56;

    lv_obj_t *tv = lv_tabview_create(parent);
    s_ui.tabview = tv;
    lv_obj_set_size(tv, kPanelW, h);
    lv_obj_set_pos(tv, 0, y);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, kTabBarH);

    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(tv, 0, LV_PART_MAIN);

    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_ITEMS);
    lv_obj_set_style_outline_width(bar, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorBtnActive),
                              Sel(LV_PART_ITEMS, LV_STATE_CHECKED));
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER,
                            Sel(LV_PART_ITEMS, LV_STATE_CHECKED));

    lv_obj_t *content = lv_tabview_get_content(tv);
    screen_swipe_back_ignore(content, true);

    // 4G 模式下没有 WiFi 列表 / 扫描概念，直接跳过这两个 Tab。
    const bool show_wifi_tabs = true;

    if (show_wifi_tabs) {
        // Tab 1：附近 WiFi
        lv_obj_t *tab1 = lv_tabview_add_tab(tv, I18n::T("附近 WiFi"));
        s_ui.nearby_tab = tab1;
        lv_obj_set_style_pad_all(tab1, 14, LV_PART_MAIN);
        lv_obj_set_style_pad_row(tab1, 10, LV_PART_MAIN);
        lv_obj_remove_flag(tab1, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(tab1, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tab1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);

        lv_obj_t *nearby_toolbar = lv_obj_create(tab1);
        screen_strip_obj_chrome(nearby_toolbar);
        lv_obj_set_size(nearby_toolbar, LV_PCT(100), 48);
        lv_obj_set_style_bg_opa(nearby_toolbar, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(nearby_toolbar, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nearby_hint = lv_label_create(nearby_toolbar);
        lv_label_set_text(nearby_hint, I18n::T("扫描附近可用网络"));
        lv_obj_set_width(nearby_hint, 250);
        lv_label_set_long_mode(nearby_hint, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(nearby_hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(nearby_hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(nearby_hint, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *search = lv_textarea_create(nearby_toolbar);HiInputMethod_Bind(search,true);
        s_ui.nearby_search = search;
        lv_obj_set_size(search, 270, 44);
        lv_obj_align(search, LV_ALIGN_RIGHT_MID, -150, 0);
        lv_textarea_set_one_line(search, true);
        lv_textarea_set_max_length(search, 32);
        lv_textarea_set_placeholder_text(search, I18n::T("搜索 WiFi 名称"));
        lv_obj_set_style_radius(search, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(search, lv_color_hex(0x121726), LV_PART_MAIN);
        lv_obj_set_style_text_color(search, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(search, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_add_event_cb(search, on_nearby_search_focused, LV_EVENT_FOCUSED,
                            nullptr);
        lv_obj_add_event_cb(search, on_nearby_search_focused, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(search, on_nearby_search_changed,
                            LV_EVENT_VALUE_CHANGED, nullptr);
        screen_swipe_back_ignore(search, true);

        lv_obj_t *scan = lv_button_create(nearby_toolbar);
        s_ui.scan_btn = scan;
        lv_obj_set_size(scan, 140, 44);
        lv_obj_align(scan, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(scan, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(scan, lv_color_hex(kColorBtnActive), LV_PART_MAIN);
        lv_obj_set_style_border_width(scan, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_width(scan, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(scan, lv_color_hex(kColorBtn),
                                  Sel(LV_PART_MAIN, LV_STATE_DISABLED));
        lv_obj_set_style_shadow_width(scan, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(scan, on_scan_clicked, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(scan, true);
        lv_obj_t *scan_lbl = lv_label_create(scan);
        s_ui.scan_btn_lbl = scan_lbl;
        lv_label_set_text(scan_lbl, I18n::T("扫描"));
        lv_obj_set_style_text_color(scan_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(scan_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(scan_lbl);

        lv_obj_t *status = lv_label_create(tab1);
        s_ui.status_label = status;
        lv_label_set_text(status, "");
        lv_obj_set_width(status, LV_PCT(100));
        lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(status, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(status, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(status, 4, LV_PART_MAIN);

        lv_obj_t *nearby = lv_obj_create(tab1);
        s_ui.nearby_list = nearby;
        screen_strip_obj_chrome(nearby);
        lv_obj_set_width(nearby, LV_PCT(100));
        lv_obj_set_flex_grow(nearby, 1);
        lv_obj_set_style_bg_color(nearby, lv_color_hex(kColorListBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(nearby, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(nearby, 14, LV_PART_MAIN);
        lv_obj_set_style_pad_all(nearby, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(nearby, 6, LV_PART_MAIN);
        lv_obj_set_flex_flow(nearby, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(nearby, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_scroll_dir(nearby, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(nearby, LV_SCROLLBAR_MODE_AUTO);

        // 扫描期间的圆环 spinner
        lv_obj_t *spin = lv_spinner_create(nearby);
        lv_obj_set_size(spin, 96, 96);
        lv_obj_add_flag(spin, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(spin, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtnActive),
                                   LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(spin, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtn), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(spin, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_arc_width(spin, 6, LV_PART_MAIN);
        lv_obj_add_flag(spin, LV_OBJ_FLAG_HIDDEN);
        s_ui.nearby_spinner = spin;

        // Tab 2：已保存 WiFi
        lv_obj_t *tab2 = lv_tabview_add_tab(tv, I18n::T("已保存 WiFi"));
        s_ui.saved_tab = tab2;
        lv_obj_set_style_pad_all(tab2, 14, LV_PART_MAIN);
        lv_obj_set_style_pad_row(tab2, 10, LV_PART_MAIN);
        lv_obj_remove_flag(tab2, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(tab2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tab2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);

        lv_obj_t *toolbar = lv_obj_create(tab2);
        screen_strip_obj_chrome(toolbar);
        lv_obj_set_size(toolbar, LV_PCT(100), 48);
        lv_obj_set_style_bg_opa(toolbar, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *hint = lv_label_create(toolbar);
        lv_label_set_text(hint, I18n::T("管理已连接过的网络"));
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *clear = lv_button_create(toolbar);
        s_ui.clear_btn = clear;
        lv_obj_set_size(clear, 140, 44);
        lv_obj_align(clear, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(clear, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(clear, lv_color_hex(kColorBtnDanger), LV_PART_MAIN);
        lv_obj_set_style_bg_color(clear, lv_color_hex(kColorBtn),
                                  Sel(LV_PART_MAIN, LV_STATE_DISABLED));
        lv_obj_set_style_shadow_width(clear, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(clear, on_clear_all_saved, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *clear_lbl = lv_label_create(clear);
        lv_label_set_text(clear_lbl, I18n::T("清空全部"));
        lv_obj_set_style_text_color(clear_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(clear_lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(clear_lbl);

        lv_obj_t *saved = lv_obj_create(tab2);
        s_ui.saved_list = saved;
        screen_strip_obj_chrome(saved);
        lv_obj_set_width(saved, LV_PCT(100));
        lv_obj_set_flex_grow(saved, 1);
        lv_obj_set_style_bg_color(saved, lv_color_hex(kColorListBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(saved, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(saved, 14, LV_PART_MAIN);
        lv_obj_set_style_pad_all(saved, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(saved, 6, LV_PART_MAIN);
        lv_obj_set_flex_flow(saved, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(saved, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_scroll_dir(saved, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(saved, LV_SCROLLBAR_MODE_AUTO);
    }

    // 「网络切换」和「SIM 卡切换」两个 Tab 分别封到 lambda 里。
    auto build_network_switch_tab = [&]() {
        lv_obj_t *tab3 = lv_tabview_add_tab(tv, I18n::T("网络切换"));
        s_ui.network_tab = tab3;
        lv_obj_set_style_pad_all(tab3, 24, LV_PART_MAIN);
        lv_obj_remove_flag(tab3, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *card = lv_obj_create(tab3);
        screen_strip_obj_chrome(card);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(card, 16, LV_PART_MAIN);

        lv_obj_t *title3 = lv_label_create(card);
        lv_label_set_text(title3, I18n::T("上网方式"));
        lv_obj_set_style_text_color(title3, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(title3, &font_puhui_30_4, LV_PART_MAIN);

        lv_obj_t *hint3 = lv_label_create(card);
        lv_label_set_text(hint3,
                          I18n::T("选择上网方式。切换后自动连接，无需重启设备。"));
        lv_obj_set_width(hint3, LV_PCT(100));
        lv_label_set_long_mode(hint3, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(hint3, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(hint3, &font_puhui_20_4, LV_PART_MAIN);

        lv_obj_t *net_row = lv_obj_create(card);
        screen_strip_obj_chrome(net_row);
        lv_obj_set_size(net_row, LV_PCT(100), 96);
        lv_obj_set_style_bg_opa(net_row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(net_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(net_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(net_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        auto make_net_btn = [&](const char *text, lv_event_cb_t cb,
                                lv_obj_t **out_btn, lv_obj_t **out_lbl) {
            lv_obj_t *btn = lv_button_create(net_row);
            lv_obj_set_size(btn, 280, 80);
            lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
            lv_obj_set_style_bg_color(btn, lv_color_hex(kColorBtn),
                                      LV_PART_MAIN);
            lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
            screen_swipe_back_ignore(btn, true);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, text);
            lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
            lv_obj_center(lbl);
            *out_btn = btn;
            *out_lbl = lbl;
        };

        make_net_btn("WiFi", on_network_wifi_clicked,
                     &s_ui.network_wifi_btn, &s_ui.network_wifi_lbl);
        make_net_btn("4G", on_network_cell_clicked,
                     &s_ui.network_cell_btn, &s_ui.network_cell_lbl);

        lv_obj_t *cur = lv_label_create(card);
        s_ui.network_current_lbl = cur;
        lv_label_set_text(cur, I18n::T("当前：--"));
        lv_obj_set_style_text_color(cur, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(cur, &font_puhui_20_4, LV_PART_MAIN);

        refresh_network_switch_ui();
#ifdef CONFIG_METALIO_NT26_4G
        auto *apn_btn = lv_button_create(card);
        lv_obj_set_size(apn_btn, LV_PCT(100), 52);
        lv_obj_add_event_cb(apn_btn, on_apn_clicked, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(apn_btn, true);
        auto *apn_label = lv_label_create(apn_btn);
        lv_label_set_text(apn_label, I18n::T("APN 设置"));
        lv_obj_set_style_text_font(apn_label, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(apn_label);
#endif
    };

    auto build_sim_switch_tab = [&]() {
        lv_obj_t *tab4 = lv_tabview_add_tab(tv, I18n::T("SIM 卡切换"));
        s_ui.sim_tab = tab4;
        lv_obj_set_style_pad_all(tab4, 24, LV_PART_MAIN);
        lv_obj_add_flag(tab4, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sim_card = lv_obj_create(tab4);
        screen_strip_obj_chrome(sim_card);
        lv_obj_set_size(sim_card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(sim_card, lv_color_hex(kColorCard),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sim_card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(sim_card, 18, LV_PART_MAIN);
        lv_obj_set_style_pad_all(sim_card, 24, LV_PART_MAIN);
        lv_obj_remove_flag(sim_card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(sim_card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(sim_card, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(sim_card, 16, LV_PART_MAIN);

        lv_obj_t *sim_title = lv_label_create(sim_card);
        lv_label_set_text(sim_title, I18n::T("SIM 卡选择"));
        lv_obj_set_style_text_color(sim_title, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(sim_title, &font_puhui_30_4, LV_PART_MAIN);

        lv_obj_t *sim_hint = lv_label_create(sim_card);
        lv_label_set_text(sim_hint,
                          I18n::T("选择 4G 模组使用的 SIM 卡。\n更换卡后自动连接，无需重启设备。"));
        lv_obj_set_width(sim_hint, LV_PCT(100));
        lv_label_set_long_mode(sim_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(sim_hint, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(sim_hint, &font_puhui_20_4, LV_PART_MAIN);

        lv_obj_t *sim_row = lv_obj_create(sim_card);
        screen_strip_obj_chrome(sim_row);
        lv_obj_set_size(sim_row, LV_PCT(100), 96);
        lv_obj_set_style_bg_opa(sim_row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(sim_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(sim_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sim_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        auto make_sim_btn = [&](const char *text, lv_event_cb_t cb,
                                lv_obj_t **out_btn, lv_obj_t **out_lbl) {
            lv_obj_t *btn = lv_button_create(sim_row);
            lv_obj_set_size(btn, 280, 80);
            lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
            lv_obj_set_style_bg_color(btn, lv_color_hex(kColorBtn),
                                      LV_PART_MAIN);
            lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
            screen_swipe_back_ignore(btn, true);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, text);
            lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
            lv_obj_center(lbl);
            *out_btn = btn;
            *out_lbl = lbl;
        };

        make_sim_btn(I18n::T("外置卡"), on_sim_external_clicked,
                     &s_ui.sim_external_btn, &s_ui.sim_external_lbl);
        make_sim_btn(I18n::T("内置卡"), on_sim_internal_clicked,
                     &s_ui.sim_internal_btn, &s_ui.sim_internal_lbl);

        lv_obj_t *sim_cur = lv_label_create(sim_card);
        s_ui.sim_current_lbl = sim_cur;
        char sim_cur_buf[64];
        snprintf(sim_cur_buf, sizeof(sim_cur_buf), I18n::T("当前：%s"),
                 SimSlotName(GetBackend()->GetSimSlot()));
        lv_label_set_text(sim_cur, sim_cur_buf);
        lv_obj_set_style_text_color(sim_cur, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(sim_cur, &font_puhui_20_4, LV_PART_MAIN);

        lv_obj_t *apn_btn = lv_button_create(sim_card);
        lv_obj_set_size(apn_btn, LV_PCT(100), 52);
        lv_obj_set_style_bg_color(apn_btn, lv_color_hex(kColorBtn), LV_PART_MAIN);
        lv_obj_set_style_radius(apn_btn, 12, LV_PART_MAIN);
        lv_obj_add_event_cb(apn_btn, on_apn_clicked, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(apn_btn, true);
        lv_obj_t *apn_label = lv_label_create(apn_btn);
        lv_label_set_text(apn_label, I18n::T("APN 设置"));
        lv_obj_set_style_text_font(apn_label, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(apn_label);

        auto *pin_button = lv_button_create(sim_card);
        lv_obj_set_size(pin_button, LV_PCT(100), 52);
        lv_obj_add_event_cb(pin_button, on_pin_clicked, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(pin_button, true);
        auto *pin_label = lv_label_create(pin_button);
        lv_label_set_text(pin_label, I18n::T("PIN 解锁"));
        lv_obj_set_style_text_font(pin_label, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(pin_label);

        lv_obj_t *sim_tip = lv_label_create(sim_card);
        lv_label_set_text(sim_tip, I18n::T("切换 SIM 卡后将重新连接移动网络，无需重启设备。"));
        lv_obj_set_width(sim_tip, LV_PCT(100));
        lv_label_set_long_mode(sim_tip, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(sim_tip, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(sim_tip, &font_puhui_20_4, LV_PART_MAIN);

        refresh_sim_slot_ui();
    };

    // 4G 模式：「SIM 卡切换」放在「网络切换」前面；WiFi 模式：只挂「网络切换」。
    build_network_switch_tab();
    build_sim_switch_tab();
}

}  // namespace

// ===========================================================================
// 公共接口
// ===========================================================================
void NetworkScreen::RequestAutoScanOnNextLoad() {
    s_auto_scan_on_next_load = true;
}

lv_obj_t *NetworkScreen::CreateStatic() {
    s_screen_active = true;
    s_scan_in_progress = false;
    s_connect_in_progress = false;
    s_scan_results.clear();

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);
    build_tabview(scr);
    // Status belongs to the whole network page. Keeping this label inside
    // the nearby-WiFi tab hid SIM/PIN failures while the SIM tab was selected.
    if (s_ui.status_label) {
        lv_obj_set_parent(s_ui.status_label, scr);
        lv_obj_set_pos(s_ui.status_label, 104, 64);
        lv_obj_set_width(s_ui.status_label, kPanelW - 120);
        lv_label_set_long_mode(s_ui.status_label, LV_LABEL_LONG_DOT);
    }

    s_nearby_filter.clear();
    if (s_ui.nearby_search != nullptr) lv_textarea_set_text(s_ui.nearby_search, "");
    if (s_ui.nearby_tab != nullptr) {
        s_ui.nearby_keyboard = HiInputMethod_Create(scr);
        if (s_ui.nearby_keyboard != nullptr) {
            lv_obj_add_flag(s_ui.nearby_keyboard, LV_OBJ_FLAG_HIDDEN);
            screen_swipe_back_ignore(s_ui.nearby_keyboard, true);
        }
    }

    rebuild_nearby_list_now();
    rebuild_saved_list_now();

    screen_kill_theme_chrome(scr);
    screen_attach_swipe_back(scr, on_swipe_back);
    screen_attach_left_edge_back(scr, kHeaderH, on_swipe_back);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    return scr;
}

lv_obj_t *NetworkScreen::Create() {
    root_ = CreateStatic();
    return root_;
}

void NetworkScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: network_screen");
        if (!IsCellularMode()) {
            post_status("", kColorSubtle);
            refresh_saved_list();
            if (s_auto_scan_on_next_load) {
                s_auto_scan_on_next_load = false;
                schedule_scan();
            }
        }
        refresh_network_switch_ui();
        refresh_sim_slot_ui();
    } else {
        ESP_LOGI(TAG, "unload: network_screen");
        s_screen_active = false;
        // openvela: wifi 栈的 start/stop 由 backend 管理；这里无需 teardown。
    }
}
