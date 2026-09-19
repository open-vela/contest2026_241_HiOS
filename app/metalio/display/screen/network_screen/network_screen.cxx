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
 *   - esp_wifi_* / SsidManager / WifiStation / DualNetworkBoard /
 *     Nt26Board are not exposed on openvela. They are hidden behind a
 *     NetworkScreenBackend interface with a default
 *     StubNetworkScreenBackend that returns empty scan results and
 *     fails connect / clear / switch operations. The full UI flow
 *     (scan button, list rebuild, password popup, connecting spinner,
 *     success / failure cards, network-switch + SIM-switch reboot
 *     countdown) is preserved verbatim and exercises end-to-end
 *     against the stub.
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

#include "application.h"
#include "settings.h"

#include <algorithm>
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
// 把所有硬件 / 系统依赖集中到这一个接口里，方便后续 openvela 把真实
// 的 wifi / ssid_manager / dual_network_board / nt26_board 接进来时
// 只换一份实现。默认 StubNetworkScreenBackend 行为：
//   - 扫描立即返回空列表（"未发现网络"）
//   - 连接永远失败（"network stack not yet wired"）
//   - 已保存网络列表为空
//   - 网络类型默认 WiFi、SIM 槽位默认外置
//   - SwitchNetworkType / SwitchSimSlot 立即返回 false
// UI 流程因此能完整跑通：扫描 -> 空列表 -> 输密码 -> 连接失败弹窗 ->
// 关闭回到密码键盘。当真实 backend 落地后，所有路径自动生效。
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
// 完成扫描 / 连接 / 状态反馈。已保存列表 / 网络切换 / SIM 切换在当前单 WiFi
// 板子上仍为 no-op，保持与 stub 一致。
class EspHostedNetworkScreenBackend : public NetworkScreenBackend {
public:
    bool Scan(std::vector<ApItem> &out, std::string &err) override {
        out.clear();

        int ret = metalio_esp_hosted_initialize();
        if (ret < 0) {
            err = "WiFi 驱动初始化失败";
            return false;
        }

        ret = metalio_esp_hosted_start_wifi();
        if (ret < 0) {
            err = "WiFi 未就绪：请插入 microSD 后重试扫描";
            return false;
        }

        struct metalio_wifi_ap_s aps[METALIO_WIFI_MAX_AP];
        int n = metalio_esp_hosted_scan(aps, METALIO_WIFI_MAX_AP);
        if (n < 0) {
            if (n == -ENETDOWN) {
                err = "WiFi 链路未就绪：请确认已插入 microSD";
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
        write(1, "BK0\n", 4);

        int ret = metalio_esp_hosted_initialize();
        if (ret < 0) {
            r.ok = false;
            r.err = "WiFi 驱动初始化失败";
            return r;
        }

        const char *pass = password.empty() ? nullptr : password.c_str();
        write(1, "BK1\n", 4);
        ret = metalio_esp_hosted_connect(ssid.c_str(), pass);
        write(1, "BK2\n", 4);
        if (ret < 0) {
            r.ok = false;
            r.err = "连接请求失败";
            return r;
        }

        // 轮询 STA 关联，最长 15 秒
        bool associated = false;
        for (int i = 0; i < 300; i++) {
            if (metalio_esp_hosted_is_connected()) {
                associated = true;
                break;
            }
            usleep(50 * 1000);
        }
        if (!associated) {
            r.ok = false;
            r.err = "连接超时或密码错误";
            return r;
        }

        /* Claw4 waits for IP; on NuttX the host runs DHCP on eth0. Without
         * this step the radio is up but xiaozhi.me MQTT/DNS cannot work. */
        if (netlib_ifup("eth0") < 0) {
            r.ok = false;
            r.err = "网卡启动失败";
            return r;
        }

        int dhcp_ret = netlib_obtain_ipv4addr("eth0");
        if (dhcp_ret < 0) {
            r.ok = false;
            r.err = "获取 IP 失败，请检查路由器";
            return r;
        }

        {
            struct in_addr gw;
            memset(&gw, 0, sizeof(gw));
            if (netlib_get_dripv4addr("eth0", &gw) == 0 && gw.s_addr != 0) {
                struct sockaddr_in fb;
                memset(&fb, 0, sizeof(fb));
                fb.sin_family = AF_INET;
                fb.sin_addr = gw;
                dns_add_nameserver((const struct sockaddr *)&fb, sizeof(fb));
            }

            struct sockaddr_in pub;
            memset(&pub, 0, sizeof(pub));
            pub.sin_family = AF_INET;
            inet_pton(AF_INET, "223.5.5.5", &pub.sin_addr);
            dns_add_nameserver((const struct sockaddr *)&pub, sizeof(pub));
        }

        r.ok = true;
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

    int GetNetworkType() override { return kNetTypeWifi; }
    bool SwitchNetworkType(int /*target_type*/) override { return false; }

    int GetSimSlot() override { return kSimSlotExternal; }
    bool SwitchSimSlot(int /*target_slot*/) override { return false; }
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
    lv_obj_t *pwd_overlay   = nullptr;
    lv_obj_t *pwd_textarea  = nullptr;
    lv_obj_t *pwd_keyboard  = nullptr;
    lv_obj_t *pwd_title     = nullptr;
    lv_obj_t *pwd_show_chk  = nullptr;
    lv_obj_t *status_overlay     = nullptr;
    lv_obj_t *status_message_lbl = nullptr;
};

UiState s_ui;

std::vector<ApItem>  s_scan_results;
bool                 s_screen_active = false;
bool                 s_scan_in_progress = false;
bool                 s_connect_in_progress = false;
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
};

void async_update_status(void *user_data) {
    auto *msg = static_cast<AsyncStatusMsg *>(user_data);
    if (screen_alive() && s_ui.status_label != nullptr) {
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
    lv_async_call(async_update_status, msg);
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
void scan_task(void * /*arg*/) {
    s_scan_in_progress = true;
    post_scan_btn_enabled(false);
    post_nearby_spinner(true);
    refresh_nearby_list();

    post_status(I18n::T("正在扫描附近 WiFi…"), kColorScanning);

    std::vector<ApItem> results;
    std::string err;
    bool ok = GetBackend()->Scan(results, err);

    if (!ok) {
        post_status(err.c_str(), kColorError);
        s_scan_in_progress = false;
        refresh_nearby_list();
        post_nearby_spinner(false);
        post_scan_btn_enabled(true);
        vTaskDelete(nullptr);
        return;
    }

    // 排序：RSSI 降序。只排序下标，避免对含 std::string 的元素做
    // move/swap（uClibc++ 下 std::string move 已知会导致堆破坏）。
    std::vector<int> order;
    order.reserve(results.size());
    for (size_t i = 0; i < results.size(); i++) order.push_back((int)i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return results[a].rssi > results[b].rssi;
    });

    // 去重（同名只保留信号最强的）。用 copy 而非 move，进一步规避
    // std::string 移动语义问题。
    s_scan_results.clear();
    s_scan_results.reserve(results.size());
    for (int idx : order) {
        const ApItem &r = results[idx];
        if (r.ssid.empty()) continue;
        bool dup = false;
        for (auto &existing : s_scan_results) {
            if (existing.ssid == r.ssid) { dup = true; break; }
        }
        if (dup) continue;
        s_scan_results.push_back(r);
    }

    char status[64];
    snprintf(status, sizeof(status), I18n::T("扫描完成，共 %d 个网络"),
             static_cast<int>(s_scan_results.size()));
    post_status(status, kColorSuccess);

    s_scan_in_progress = false;
    refresh_nearby_list();
    post_nearby_spinner(false);
    post_scan_btn_enabled(true);
    vTaskDelete(nullptr);
}

void schedule_scan() {
    if (s_scan_in_progress) {
        ESP_LOGW(TAG, "scan already in progress");
        return;
    }
    if (s_connect_in_progress) {
        post_status(I18n::T("正在连接，请稍后再扫描"), kColorScanning);
        return;
    }
    if (xTaskCreate(scan_task, "wifi_scan", 8192, nullptr, 5, nullptr) != pdPASS) {
        post_status(I18n::T("无法启动扫描任务"), kColorError);
    }
}

// ---------------------------------------------------------------------------
// 连接任务
// ---------------------------------------------------------------------------
struct ConnectCtx {
    std::string ssid;
    std::string password;
};

void connect_task(void *arg) {
    auto *ctx = static_cast<ConnectCtx *>(arg);
    write(1, "CT0\n", 4);

    char buf[128];
    snprintf(buf, sizeof(buf), I18n::T("正在连接 %s …"), ctx->ssid.c_str());
    write(1, "CT1\n", 4);
    post_status(buf, kColorScanning);

    write(1, "CT2\n", 4);
    ConnectResult res = GetBackend()->Connect(ctx->ssid, ctx->password);
    write(1, "CT3\n", 4);

    if (res.ok) {
        /* Persist for next boot (best-effort; Settings may be /tmp-backed). */
        {
            Settings wifi("wifi", true);
            wifi.SetString("ssid", ctx->ssid);
            wifi.SetString("password", ctx->password);
        }
        snprintf(buf, sizeof(buf), I18n::T("连接 %s 成功"), ctx->ssid.c_str());
        post_status(buf, kColorSuccess);
        refresh_saved_list();
        post_close_password_popup();
        /* Keep a modal up: OTA CheckVersion needs the new IP to mint the
         * 6-digit xiaozhi.me code — closing immediately hid the result. */
        post_open_activation_wait(ctx->ssid);
        Application::GetInstance().OnWifiLinkReady();
    } else {
        std::string detail = res.err.empty() ? I18n::T("连接失败") : res.err;
        snprintf(buf, sizeof(buf), I18n::T("连接 %s 失败：%s"),
                 ctx->ssid.c_str(), detail.c_str());
        post_status(buf, kColorError);
        // 失败弹窗保留 2.5 秒让用户看清原因，然后自动收起回到密码键盘
        post_show_failure(I18n::T("连接失败"), detail);
    }

    s_connect_in_progress = false;
    delete ctx;
    vTaskDelete(nullptr);
}

void schedule_connect(const std::string &ssid, const std::string &password) {
    if (s_connect_in_progress) {
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
    auto *ctx = new ConnectCtx{ssid, password};
    write(1, "SC0\n", 4);
    s_connect_in_progress = true;
    // 立刻弹出转圈遮罩，覆盖密码键盘，让用户得到「我点了连接」的视觉反馈。
    open_connecting_popup(ssid);
    write(1, "SC1\n", 4);
    if (xTaskCreate(connect_task, "wifi_connect", 8192, ctx, 5, nullptr) != pdPASS) {
        write(1, "SC2\n", 4);
        delete ctx;
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

    if (s_scan_results.empty()) {
        if (s_scan_in_progress) {
            return;
        }
        lv_obj_t *hint = lv_label_create(s_ui.nearby_list);
        lv_label_set_text(hint, I18n::T("未发现网络，点右上「扫描」试试"));
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_pad_all(hint, 16, LV_PART_MAIN);
        return;
    }

    for (auto &ap : s_scan_results) {
        lv_obj_t *item = lv_button_create(s_ui.nearby_list);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, 60);
        lv_obj_set_style_radius(item, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(item, lv_color_hex(kColorItem), LV_PART_MAIN);
        lv_obj_set_style_bg_color(item, lv_color_hex(kColorItemSel),
                                  Sel(LV_PART_MAIN, LV_STATE_PRESSED));
        lv_obj_set_style_pad_hor(item, 14, LV_PART_MAIN);
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
void on_kb_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *kb = lv_event_get_target_obj(e);

    if (code == LV_EVENT_CANCEL) {
        close_password_popup();
    } else if (code == LV_EVENT_READY) {
        lv_obj_t *ta = lv_keyboard_get_textarea(kb);
        const char *pwd = (ta != nullptr) ? lv_textarea_get_text(ta) : "";
        // 开放网络若用户没输入密码也允许（password 留空）
        schedule_connect(s_pending_ssid, pwd ? pwd : "");
    }
}

void on_show_pwd_changed(lv_event_t *e) {
    lv_obj_t *chk = lv_event_get_target_obj(e);
    if (s_ui.pwd_textarea == nullptr) return;
    const bool checked = lv_obj_has_state(chk, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(s_ui.pwd_textarea, !checked);
}

void on_pwd_connect_btn(lv_event_t * /*e*/) {
    if (s_ui.pwd_textarea == nullptr) return;
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
    lv_obj_t *ta = lv_textarea_create(card);
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

    // 屏幕底部键盘（吃满底部 ~390px）
    lv_obj_t *kb = lv_keyboard_create(mask);
    s_ui.pwd_keyboard = kb;
    lv_obj_set_size(kb, kPanelW, 390);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_CANCEL, nullptr);
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
    xTaskCreate(reboot_task, "wifi_reboot", 2048, nullptr, 5, nullptr);
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
    s_network_switch_pending = false;
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
}

// ---------------------------------------------------------------------------
// 网络切换（WiFi <-> 4G）
// ---------------------------------------------------------------------------
void switch_network_task(void *arg) {
    int target = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    vTaskDelay(pdMS_TO_TICKS(1500));
    bool ok = GetBackend()->SwitchNetworkType(target);
    if (!ok) {
        ESP_LOGE(TAG, "SwitchNetworkType failed, reboot anyway");
    }
    Application::GetInstance().Reboot();
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
    if (s_network_switch_pending) {
        return;
    }
    if (target_type != kNetTypeWifi && target_type != kNetTypeCellular) {
        return;
    }
    if (GetBackend()->GetNetworkType() == target_type) {
        return;
    }

    s_network_switch_pending = true;
    const char *target = target_type == kNetTypeCellular ? "4G" : "WiFi";
    post_status(target_type == kNetTypeCellular ? I18n::T("准备切换到 4G…")
                                                 : I18n::T("准备切换到 WiFi…"),
                kColorScanning);
    open_switch_reboot_popup(target);
    if (xTaskCreate(switch_network_task, "net_switch", 4096,
                    reinterpret_cast<void *>(static_cast<intptr_t>(target_type)), 5,
                    nullptr) != pdPASS) {
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
                              lv_color_hex(is_external ? kColorBtn
                                                       : kColorBtnActive),
                              LV_PART_MAIN);

    if (s_ui.sim_current_lbl != nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), I18n::T("当前：%s"), SimSlotName(slot));
        lv_label_set_text(s_ui.sim_current_lbl, buf);
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
};

struct SimSwitchResultMsg {
    bool        success;
    int         target_slot;
    std::string detail;
};

void async_sim_switch_done(void *user_data) {
    auto *msg = static_cast<SimSwitchResultMsg *>(user_data);
    if (screen_alive()) {
        s_sim_switch_pending = false;
        if (msg->success) {
            char buf[96];
            snprintf(buf, sizeof(buf), I18n::T("已切换到%s"),
                     SimSlotName(msg->target_slot));
            open_restart_countdown_popup(buf);
            refresh_sim_slot_ui();
        } else {
            close_status_popup();
            post_status(msg->detail.c_str(), kColorError);
            show_failure_in_status_popup(I18n::T("SIM 卡切换失败"), msg->detail, 3000);
            refresh_sim_slot_ui();
        }
    } else {
        s_sim_switch_pending = false;
    }
    delete msg;
}

void sim_switch_task(void *arg) {
    auto *ctx = static_cast<SimSwitchCtx *>(arg);
    auto *result = new SimSwitchResultMsg{};
    result->target_slot = ctx->target_slot;
    result->success = GetBackend()->SwitchSimSlot(ctx->target_slot);
    if (!result->success) {
        result->detail = I18n::T("当前不在 4G 模式 / 未检测到 4G 模块");
    }
    lv_async_call(async_sim_switch_done, result);
    delete ctx;
    vTaskDelete(nullptr);
}

void schedule_sim_switch(int target_slot) {
    if (s_sim_switch_pending) {
        return;
    }
    if (target_slot != kSimSlotExternal && target_slot != kSimSlotInternal) {
        return;
    }
    if (GetBackend()->GetSimSlot() == target_slot) {
        return;
    }

    s_sim_switch_pending = true;
    auto *ctx = new SimSwitchCtx{target_slot};
    open_sim_switching_popup(target_slot);
    if (xTaskCreate(sim_switch_task, "sim_switch", 4096, ctx, 5, nullptr) !=
        pdPASS) {
        delete ctx;
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

    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
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
    const bool show_wifi_tabs = !IsCellularMode();

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
        lv_obj_set_style_text_color(nearby_hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(nearby_hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(nearby_hint, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *scan = lv_button_create(nearby_toolbar);
        s_ui.scan_btn = scan;
        lv_obj_set_size(scan, 140, 44);
        lv_obj_align(scan, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(scan, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(scan, lv_color_hex(kColorBtnActive), LV_PART_MAIN);
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
                          I18n::T("选择上网方式。切换后设备将自动重启生效。"));
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
    };

    auto build_sim_switch_tab = [&]() {
        lv_obj_t *tab4 = lv_tabview_add_tab(tv, I18n::T("SIM 卡切换"));
        s_ui.sim_tab = tab4;
        lv_obj_set_style_pad_all(tab4, 24, LV_PART_MAIN);
        lv_obj_remove_flag(tab4, LV_OBJ_FLAG_SCROLLABLE);

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
                          I18n::T("选择 4G 模组使用的 SIM 卡。\n切换后设备将自动重启生效。"));
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

        lv_obj_t *sim_tip = lv_label_create(sim_card);
        lv_label_set_text(sim_tip,
                          I18n::T("AT 命令序列：\n  AT+CFUN=0\n  AT+ECSIMCFG=SimSlot,X\n  AT+CFUN=1"));
        lv_obj_set_width(sim_tip, LV_PCT(100));
        lv_label_set_long_mode(sim_tip, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(sim_tip, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(sim_tip, &font_puhui_20_4, LV_PART_MAIN);

        refresh_sim_slot_ui();
    };

    // 4G 模式：「SIM 卡切换」放在「网络切换」前面；WiFi 模式：只挂「网络切换」。
    if (IsCellularMode()) {
        build_sim_switch_tab();
        build_network_switch_tab();
    } else {
        build_network_switch_tab();
    }
}

}  // namespace

// ===========================================================================
// 公共接口
// ===========================================================================
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

    rebuild_nearby_list_now();
    rebuild_saved_list_now();

    screen_attach_swipe_back(scr, on_swipe_back);
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
            // 不再自动扫描——等用户主动点「扫描」。
            post_status("", kColorSubtle);
            refresh_saved_list();
        }
        refresh_network_switch_ui();
        refresh_sim_slot_ui();
    } else {
        ESP_LOGI(TAG, "unload: network_screen");
        s_screen_active = false;
        // openvela: wifi 栈的 start/stop 由 backend 管理；这里无需 teardown。
    }
}
