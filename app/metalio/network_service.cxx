#include "hi_task_policy.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NetworkService implementation — wraps the board-specific
 * metalio_esp_hosted_* and metalio_nt26_* helpers exported by the openvela
 * board library, plus a small DNS resolver helper.
 */

#include "network_service.h"
#include "application.h"
#include "app_dns.h"
#include "esp_log_shim.h"
#include "settings.h"
#include "phone_service.h"

#include <metalio/metalio.h>
extern "C" int metalio_nt26_get_sim_slot(void);
extern "C" int metalio_nt26_is_registered(void);
extern "C" int metalio_nt26_get_dns_servers(uint32_t servers[2]);

#include <netutils/netlib.h>
#include <nuttx/mm/iob.h>

#include <cstring>
#include <cstdio>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <atomic>
#include <net/if.h>

namespace {
pthread_mutex_t s_cellular_start_lock;
pthread_once_t s_network_lock_once = PTHREAD_ONCE_INIT;
std::atomic<int> s_active_path{-1};
std::atomic<bool> s_cellular_requested{false};
std::atomic<bool> s_sim_recovery_pending{false};
std::atomic<bool> s_sim_present{false};
std::atomic<bool> s_call_session{false};
bool s_call_radio_owned = false;
void InitNetworkLock() {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_cellular_start_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
struct CellularStartGuard {
    bool locked;
    explicit CellularStartGuard(bool wait = true) {
        pthread_once(&s_network_lock_once, InitNetworkLock);
        locked = (wait ? pthread_mutex_lock(&s_cellular_start_lock) :
                         pthread_mutex_trylock(&s_cellular_start_lock)) == 0;
    }
    ~CellularStartGuard() { if (locked) pthread_mutex_unlock(&s_cellular_start_lock); }
};

// WiFi is a data path, not a request to stop receiving calls and SMS.
// Check both saved intent and live radio state so airplane/all-radios-off
// transitions cannot be undone by the background SMS worker.
bool TelephonyStandbyAllowed() {
    Settings preferences("network", false);
    if (preferences.GetBool("airplane", false)) return false;
    const bool cellular = preferences.GetBool("cellular", preferences.GetInt("type", 0) == 1);
    const bool wifi = preferences.GetBool("wifi_enabled", !cellular);
    return (wifi && metalio_esp_hosted_radio_enabled()) ||
        (cellular && (s_cellular_requested.load() || s_active_path.load() == 1));
}

// CPIN READY is stored, then get_sim_status() reports registration instead:
// searching or registered. Neither value means the card is absent.
bool SimCardUsable() {
    switch (metalio_nt26_get_sim_status()) {
    case METALIO_SIM_READY:
    case METALIO_SIM_SEARCHING:
    case METALIO_SIM_REGISTERED:
        return true;
    default:
        return false;
    }
}

bool InterfaceReady(const char *name) {
    uint8_t flags = 0;
    struct in_addr address{};
    return netlib_getifstatus(name, &flags) == 0 && (flags & IFF_UP) &&
           netlib_get_ipv4addr(name, &address) == 0 && address.s_addr != 0;
}

bool CellularLeaseReady() {
    struct in_addr mask{};
    return InterfaceReady("eth1") && metalio_nt26_is_registered() &&
        netlib_get_ipv4netmask("eth1",&mask)==0 && mask.s_addr!=0;
}

int start_saved_wifi(NetworkService &network)
{
    Settings wifi("wifi", false);
    std::string saved_ssid = wifi.GetString("ssid");
    std::string saved_pass = wifi.GetString("password");
    const char *ssid = saved_ssid.empty() ? CONFIG_METALIO_WIFI_SSID : saved_ssid.c_str();
    const char *pass = saved_ssid.empty() ? CONFIG_METALIO_WIFI_PASSWORD :
                       (saved_pass.empty() ? nullptr : saved_pass.c_str());
    if (ssid == nullptr || ssid[0] == '\0') ssid = nullptr;
    if (pass != nullptr && pass[0] == '\0') pass = nullptr;
    return network.StartWifi(ssid, pass);
}
}

/* nuttx/net/dns.h uses 'class' as a struct member name, which cannot be
 * parsed in C++, so declare just the one prototype we need. */
extern "C" int dns_default_nameserver(void);
extern "C" int dns_add_nameserver(const struct sockaddr *addr,
                                  socklen_t addrlen);

#define TAG "NetworkService"

NetworkService::NetworkService() {}

NetworkService::~NetworkService()
{
    Stop();
}

int NetworkService::StartWifi(const char *ssid, const char *pass)
{
    CellularStartGuard lock;
    s_cellular_requested.store(false);
#ifdef CONFIG_METALIO_ESP_HOSTED
    int ret = metalio_esp_hosted_ensure_wifi_ready();
    if (ret < 0) return ret;
    const char *connected_ssid = metalio_esp_hosted_get_ssid();
    const bool same_link = metalio_esp_hosted_is_connected() &&
        (!ssid || !*ssid || (connected_ssid && strcmp(ssid, connected_ssid) == 0));
    if (!same_link) {
        if (!ssid || !*ssid) return -ENOTCONN;
        ret = metalio_esp_hosted_connect(ssid, pass);
        if (ret < 0) return ret;
        for (int i = 0; i < 300 && !metalio_esp_hosted_is_connected(); ++i)
            usleep(100000);
        if (!metalio_esp_hosted_is_connected()) {
            (void)metalio_esp_hosted_disconnect();
            return -ETIMEDOUT;
        }
    }
    // A retained radio association does not mean eth0 is up after 4G use.
    if (netlib_ifup("eth0") < 0) return -errno;
    // Keep a usable lease on the same retained association. A second DHCP
    // exchange during a 4G->Wi-Fi switch can time out despite a valid lease.
    ret = same_link && InterfaceReady("eth0") ? 0 : netlib_obtain_ipv4addr("eth0");
    if (ret < 0 || !InterfaceReady("eth0")) {
        (void)netlib_ifdown("eth0");
        return ret < 0 ? ret : -EADDRNOTAVAIL;
    }
    struct in_addr gateway{};
    if (netlib_get_dripv4addr("eth0", &gateway) == 0 && gateway.s_addr) {
        struct sockaddr_in dns{};
        dns.sin_family = AF_INET;
        dns.sin_addr = gateway;
        dns_add_nameserver(reinterpret_cast<const struct sockaddr *>(&dns), sizeof(dns));
    }
    // All Wi-Fi entry points (including the saved-network screen) must
    // retire the old cellular route after the new lease is established.
    struct in_addr empty{};
    (void)netlib_ifdown("eth1");
    (void)netlib_set_ipv4addr("eth1", &empty);
    wifi_started_ = true;
    transport_ = Transport::Wifi;
    s_active_path.store(0);
    metalio_app_dns_set_wifi();
    ESP_LOGI(TAG, "WiFi lease ready: %s", GetIpAddress().c_str());
    if (on_state_change_) on_state_change_(true);
    return 0;
#else
    (void)ssid;
    (void)pass;
    return -ENOSYS;
#endif
}

int NetworkService::StartCellular()
{
    CellularStartGuard lock;
    s_cellular_requested.store(true);
    int ret = metalio_nt26_initialize();
    if (ret < 0)
    {
        ESP_LOGE(TAG, "nt26_initialize failed: %d", ret);
        return ret;
    }

    /* Boot must select the same data interface as the control-center path.
     * A retained C5 link with an unset IPv4 mask otherwise catches every
     * destination in NuttX's connected-subnet lookup, before the eth1 route. */
    struct in_addr wifi_address{};
    uint8_t wifi_flags = 0;
    (void)netlib_get_ipv4addr("eth0", &wifi_address);
    (void)netlib_getifstatus("eth0", &wifi_flags);
    int wifi_down = netlib_ifdown("eth0");
    {
        char status[112];
        int n = snprintf(status, sizeof(status),
                         "NET4G select eth0_flags=%02x ip=%s down=%d\n",
                         wifi_flags, inet_ntoa(wifi_address), wifi_down);
        if (n > 0) write(1, status, n < (int)sizeof(status) ? n : sizeof(status)-1);
    }

    /* NT26 is registered as the second Ethernet interface (eth1 after the
     * ESP-Hosted Wi-Fi interface eth0).  The modem supplies the IPv4 lease
     * through its UART-Ethernet/NAT path. */
    ret = netlib_ifup("eth1");
    if (ret < 0)
    {
        ESP_LOGE(TAG, "netlib_ifup(eth1) failed: %d", ret);
        struct in_addr empty{};
        (void)netlib_ifdown("eth1");
        (void)netlib_set_ipv4addr("eth1", &empty);
        return ret;
    }

    /* A freshly reset cellular modem can bring up the UART Ethernet link a
     * little before its DHCP server is ready.  Retry DHCP on eth1 while
     * keeping the modem data path up; taking the interface down here can
     * strand the modem and prevent a later lease attempt. */
    struct in_addr current{};
    const bool have_lease = netlib_get_ipv4addr("eth1", &current) == 0 &&
                            current.s_addr != 0;
    for (int attempt = 0; !have_lease && attempt < 3; ++attempt)
    {
        ret = netlib_obtain_ipv4addr("eth1");
        // DHCP can finish installing its lease just as the waiter expires.
        // Reconcile the current interface before discarding a working link.
        if (ret == 0 || CellularLeaseReady()) {
            ret=0;
            break;
        }

        struct in_addr observed_ip;
        memset(&observed_ip, 0, sizeof(observed_ip));
        (void)netlib_get_ipv4addr("eth1", &observed_ip);
        struct iob_stats_s iob_stats;
        (void)netlib_get_iobinfo(&iob_stats);
        ESP_LOGE(TAG, "DHCP eth1 attempt %d/3 failed: ret=%d errno=%d ip=%s iob=%d/%d free",
                 attempt + 1, ret, errno, inet_ntoa(observed_ip),
                 iob_stats.nfree, iob_stats.ntotal);
        if (attempt < 2)
            usleep(1000000);
    }
    if(ret<0 && CellularLeaseReady()) ret=0;
    if (ret < 0 || !InterfaceReady("eth1"))
    {
        if (ret == 0) ret = -EADDRNOTAVAIL;
        ESP_LOGE(TAG, "DHCP on NT26 eth1 failed after retries: %d", ret);
        /* Drop the unleased interface before restoring Wi-Fi.  An up eth1
         * with no address can interfere with NuttX route selection. */
        struct in_addr empty{};
        (void)netlib_ifdown("eth1");
        (void)netlib_set_ipv4addr("eth1", &empty);
        return ret;
    }

    /* Prefer the operator-assigned PDP DNS over the modem's LAN proxy.
     * Only reset DNS when real replacement addresses were read; otherwise
     * preserve DHCP and add fallbacks. This runs on the cellular worker. */
    uint32_t carrier_dns[2]{};
    const bool have_carrier_dns = metalio_nt26_get_dns_servers(carrier_dns) > 0;
    if (have_carrier_dns)
    {
        (void)dns_default_nameserver();
        for (uint32_t address : carrier_dns)
        {
            if (address == 0) continue;
            struct sockaddr_in dns{};
            dns.sin_family = AF_INET;
            dns.sin_addr.s_addr = address;
            (void)dns_add_nameserver((const struct sockaddr *)&dns, sizeof(dns));
            char status[64];
            int n = snprintf(status, sizeof(status), "NET4G dns=%s\n",
                             inet_ntoa(dns.sin_addr));
            if (n > 0) write(1, status, n < (int)sizeof(status) ? n : sizeof(status)-1);
        }
    }
    const char *servers[] = {"223.5.5.5", "119.29.29.29"};
    const int fallback_count = have_carrier_dns ? 1 : 2;
    for (int i = 0; i < fallback_count; i++)
    {
        struct sockaddr_in dns{};
        dns.sin_family = AF_INET;
        inet_pton(AF_INET, servers[i], &dns.sin_addr);
        (void)dns_add_nameserver((const struct sockaddr *)&dns, sizeof(dns));
    }

    /* Keep this resolver private to app-level cloud clients. The global
     * NuttX resolver and radio path remain unchanged. */
    metalio_app_dns_set_cellular(have_carrier_dns ? carrier_dns : nullptr,
                                 have_carrier_dns ? 2 : 0);

    cellular_started_ = true;
    transport_ = Transport::Cellular;
    s_active_path.store(1);
    {
        char status[96];
        int n = snprintf(status, sizeof(status), "NET4G up ip=%s sim=%d\n",
                         GetIpAddress().c_str(), metalio_nt26_get_sim_slot());
        if (n > 0)
            write(1, status, n < (int)sizeof(status) ? n : sizeof(status) - 1);
    }
    ESP_LOGI(TAG, "Cellular up");
    if (on_state_change_)
        on_state_change_(true);
    return 0;
}

int NetworkService::SelectPath(int cellular)
{
    /* The board's dual_network.c is part of this target's board sources;
     * CONFIG_METALIO_DUAL_NETWORK is not a Kconfig symbol in this tree.
     * Guarding this call with that nonexistent option made the service API
     * always return -ENOSYS even though NT26 and ESP-Hosted were compiled. */
    int ret = metalio_dual_network_select(cellular);
    if (ret < 0)
    {
        ESP_LOGE(TAG, "dual_network_select(%d) failed: %d", cellular, ret);
        return ret;
    }
    if (!cellular)
        metalio_app_dns_set_wifi();
    transport_ = cellular ? Transport::Cellular : Transport::Wifi;
    ESP_LOGI(TAG, "Active data path: %s", cellular ? "cellular" : "wifi");
    return 0;
}

void NetworkService::Stop()
{
    /* The board-specific drivers don't expose a stop API; we just mark the
     * transport as down so Online() returns false. */
    bool was_online = wifi_started_ || cellular_started_;
    wifi_started_ = false;
    cellular_started_ = false;
    transport_ = Transport::None;
    if (was_online && on_state_change_)
        on_state_change_(false);
}

NetworkService::Transport NetworkService::AvailableTransport()
{
    // IFF_UP rejects retained DHCP addresses after switching radios off.
    // A connected AP or a selected SIM alone does not provide an IP route.
    const bool cellular = InterfaceReady("eth1") && metalio_nt26_is_registered();
    if (s_active_path.load() == 1 && cellular) return Transport::Cellular;
#ifdef CONFIG_METALIO_ESP_HOSTED
    if (InterfaceReady("eth0") && metalio_esp_hosted_is_connected())
        return Transport::Wifi;
#endif
    return cellular ? Transport::Cellular : Transport::None;
}

bool NetworkService::Online() const
{
    return Available();
}

std::string NetworkService::GetSsid() const
{
#ifdef CONFIG_METALIO_ESP_HOSTED
    const char *ssid = metalio_esp_hosted_get_ssid();
    if (ssid != nullptr && ssid[0] != '\0')
        return std::string(ssid);
#endif
    return "(unknown)";
}

std::string NetworkService::GetIpAddress() const
{
    /* NuttX: read the interface IPv4 address directly via SIOCGIFADDR.
     * The previous connect-and-getsockname trick returned the socket's
     * local bind address (INADDR_ANY == 0.0.0.0) rather than the address
     * DHCP assigned to eth0, so the UI always showed 0.0.0.0 even after a
     * successful lease. */
    struct in_addr addr;
    memset(&addr, 0, sizeof(addr));
    const char *ifname = s_active_path.load() == 1 ? "eth1" : "eth0";
    if (netlib_get_ipv4addr(ifname, &addr) == 0 && addr.s_addr != 0)
        return std::string(inet_ntoa(addr));

    return "";
}

bool NetworkService::Resolve(const std::string &host, std::string &ip_out)
{
    uint32_t direct_address = 0;
    if (metalio_app_resolve_ipv4(host.c_str(), &direct_address) == 0)
    {
        struct in_addr addr{direct_address};
        ip_out = inet_ntoa(addr);
        return !ip_out.empty();
    }

    struct addrinfo hints;
    struct addrinfo *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr)
        return false;

    char addr_buf[INET_ADDRSTRLEN] = {0};
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &sa->sin_addr, addr_buf, sizeof(addr_buf));
    ip_out = std::string(addr_buf);
    freeaddrinfo(res);
    return !ip_out.empty();
}

/* All callers, including quick settings and boot, use this transaction.
 * Commit confirmed data intent before releasing the network lock, so a
 * background standby poll or shutdown cannot observe the previous mode. */
namespace {
void RememberConfirmedPath(int path, int slot = -1) {
    Settings settings("network", true);
    const char *keys[] = {"type", "cellular", "wifi_enabled", "airplane", "sim_slot"};
    const int32_t values[] = {path, path == 1, path == 0, 0, slot};
    if (!settings.SetInts(keys, values, slot == 0 || slot == 1 ? 5 : 4))
        ESP_LOGE(TAG, "Confirmed network preference save pending");
}

int RestoreSavedSimSlot() {
    Settings preferences("network", false);
    const int slot = preferences.GetInt("sim_slot", -1);
    if ((slot != 0 && slot != 1) || metalio_nt26_get_sim_slot() == slot) return 0;
    // The driver first reads the hardware selection. A matching slot does
    // not cycle RF; a genuine change is rejected while a call is active.
    return metalio_nt26_set_sim_slot(slot);
}

void RetireInterface(const char *name) {
    struct in_addr empty{};
    (void)netlib_ifdown(name);
    (void)netlib_set_ipv4addr(name, &empty);
}

int BringUpPath(NetworkService &network, int path) {
    // The modem helper reads back RF state before writing it, so re-selecting
    // a live modem leaves registration intact without trusting a stale cache.
    int ret = path == 1 ? metalio_nt26_set_radio(true) :
                         metalio_esp_hosted_set_radio(true);
    if (ret < 0) return ret;
    return path == 1 ? network.StartCellular() : start_saved_wifi(network);
}

void RestorePath(NetworkService &network, int previous) {
    RetireInterface(previous == 1 ? "eth0" : "eth1");
    s_active_path.store(-1);
    if (previous == 0 || previous == 1) {
        const int ret = BringUpPath(network, previous);
        if (ret == 0) {
            if (previous == 0) {
                if (!s_call_session.load() && !TelephonyStandbyAllowed())
                    (void)metalio_nt26_set_radio(false);
            } else (void)metalio_esp_hosted_set_radio(false);
            Application::GetInstance().OnWifiLinkReady();
        } else ESP_LOGE(TAG, "Previous path %d recovery failed: %d", previous, ret);
    }
}
}

extern "C" int metalio_app_select_data_path(int cellular)
{
    CellularStartGuard path_lock;
    static NetworkService network;
    const int previous = s_active_path.load();
    const int target = cellular ? 1 : 0;
    const int ret = BringUpPath(network, target);
    if (ret < 0) {
        ESP_LOGW(TAG, "Path %d failed: %d; restoring %d", target, ret, previous);
        RestorePath(network, previous);
        return ret;
    }
    RememberConfirmedPath(target);
    if (target == 0) {
        const int off = s_call_session.load() || TelephonyStandbyAllowed() ? 0 : metalio_nt26_set_radio(false);
        if (off < 0) ESP_LOGW(TAG, "Wi-Fi ready; modem radio-off pending: %d", off);
    } else (void)metalio_esp_hosted_set_radio(false);
    Application::GetInstance().OnWifiLinkReady();
    return 0;
}

extern "C" int metalio_app_apply_radios(int wifi, int cellular)
{
    CellularStartGuard path_lock;
    // The UI enforces one data radio. Old dual-enabled preferences prefer 4G.
    if (cellular || wifi) return metalio_app_select_data_path(cellular != 0);
    s_cellular_requested.store(false);
    RetireInterface("eth0");
    RetireInterface("eth1");
    s_active_path.store(-1);
    const int wifi_ret = metalio_esp_hosted_set_radio(false);
    const int cell_ret = metalio_nt26_set_radio(false);
    return wifi_ret < 0 ? wifi_ret : cell_ret;
}

extern "C" int metalio_app_restore_saved_network(void)
{
    CellularStartGuard guard;
    Settings preferences("network", false);
    const bool airplane = preferences.GetBool("airplane", false);
    const bool cellular = preferences.GetBool("cellular", preferences.GetInt("type", 0) == 1);
    const bool wifi = preferences.GetBool("wifi_enabled", !cellular);
    if (airplane || (!wifi && !cellular)) return metalio_app_apply_radios(0, 0);
    s_cellular_requested.store(cellular);
    const int slot_result = RestoreSavedSimSlot();
    if (slot_result < 0) {
        ESP_LOGW(TAG, "Saved SIM selection pending: %d", slot_result);
        // WiFi Internet remains independent of SIM presence/readiness.
        // Do not start paid cellular data on an unintended SIM instead.
        if (cellular) return slot_result;
    }
    return metalio_app_apply_radios(wifi, cellular);
}

// Shared phone/SMS standby: no data-route selection, APN rewrite, modem reset
// or outgoing request. Called by the existing call and SMS workers only.
extern "C" int metalio_app_sms_ensure_standby(void)
{
    // A background call poll must not queue behind a long network switch,
    // preventing the same call worker from answering/hanging up promptly.
    CellularStartGuard guard(false);
    if (!guard.locked) {
        // A DHCP/DNS/data-path transaction is not ownership of an already
        // registered SIM's SMS service. Use only cached, read-only readiness
        // here: never change RF or slot behind the network transaction.
        if (!TelephonyStandbyAllowed()) return -EPERM;
        Settings preferences("network", false);
        const int expected_slot = preferences.GetInt("sim_slot", -1);
        if (metalio_nt26_radio_enabled() && SimCardUsable() &&
            ((expected_slot != 0 && expected_slot != 1) ||
             metalio_nt26_get_sim_slot() == expected_slot)) return 0;
        return -EBUSY;
    }
    if (!TelephonyStandbyAllowed()) return -EPERM;
    const int slot_result = RestoreSavedSimSlot();
    if (slot_result < 0) return slot_result;
    const bool was_on = metalio_nt26_radio_enabled();
    int ret = was_on ? 0 : metalio_nt26_set_radio(true);
    if (ret == 0 && !SimCardUsable()) {
        metalio_sim_profile_s profile{};
        ret = metalio_nt26_read_sim_profile(&profile);
    }
    if (ret == 0 && !was_on)
        ESP_LOGI(TAG, "PHONE_SMS_STANDBY_READY data_path=%d", s_active_path.load());
    return ret;
}

extern "C" int metalio_app_select_sim(int slot)
{
    if (slot != 0 && slot != 1) return -EINVAL;
    CellularStartGuard path_lock;
    static NetworkService network;
    const int previous_path = s_active_path.load();
    const int previous_slot = metalio_nt26_get_sim_slot();
    int ret = metalio_nt26_set_sim_slot(slot);
    if (ret < 0) {
        // The modem may already have cycled RF before slot verification failed.
        // Restore service as well as the driver's previous slot selection.
        RestorePath(network, previous_path);
        return ret;
    }
    RetireInterface("eth1");
    s_active_path.store(-1);
    ret = BringUpPath(network, 1);
    if (ret == 0) {
        RememberConfirmedPath(1, slot);
        (void)metalio_esp_hosted_set_radio(false);
        Application::GetInstance().OnWifiLinkReady();
        return 0;
    }
    // A confirmed slot alone is not a successful data-network switch.
    if (previous_slot == 0 || previous_slot == 1) {
        const int restore = metalio_nt26_set_sim_slot(previous_slot);
        if (restore < 0) ESP_LOGE(TAG, "SIM rollback failed: %d", restore);
    }
    RestorePath(network, previous_path);
    return ret;
}

extern "C" int metalio_app_active_data_path(void)
{
    return s_active_path.load();
}

namespace {
bool ValidSimId(const char *id) {
    if (!id) return false;
    const size_t n = strnlen(id, METALIO_SIM_ID_MAX);
    if (n < 18 || n >= METALIO_SIM_ID_MAX) return false;
    for (size_t i = 0; i < n; ++i) if (id[i] < '0' || id[i] > '9') return false;
    return true;
}
bool ValidApnField(const char *text, size_t cap) {
    size_t n = strnlen(text, cap);
    if (n == cap) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)text[i] < 32 || (unsigned char)text[i] > 126 ||
            text[i] == '"' || text[i] == '\\') return false;
    return true;
}
std::string EncodeProfileField(const char *text) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        out += hex[*p >> 4]; out += hex[*p & 15];
    }
    return out;
}
bool DecodeProfileField(const std::string &text, char *out, size_t cap) {
    if (text.size() % 2 || text.size() / 2 >= cap) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c-'0' :
                                   c >= 'a' && c <= 'f' ? c-'a'+10 : -1; };
    size_t n = 0;
    for (size_t i = 0; i < text.size(); i += 2) {
        int a = digit(text[i]), b = digit(text[i+1]);
        if (a < 0 || b < 0 || (a == 0 && b == 0)) return false;
        out[n++] = (char)((a << 4) | b);
    }
    out[n] = 0;
    return ValidApnField(out, cap);
}
}

extern "C" int metalio_app_load_sim_profile(const char *id,
                                             metalio_sim_profile_s *profile)
{
    if (!profile || !ValidSimId(id)) return -EINVAL;
    Settings cards("sim_apn", false);
    const std::string value = cards.GetString(id);
    if (value.empty()) return -ENOENT;
    if (value.size() < 4 || value[0] < '0' || value[0] > '3' || value[1] != '|')
        return -EINVAL;
    size_t a = value.find('|', 2), b = a == std::string::npos ? a : value.find('|', a+1);
    if (a == std::string::npos || b == std::string::npos ||
        value.find('|', b+1) != std::string::npos) return -EINVAL;
    auto loaded = *profile;
    loaded.auth = value[0]-'0';
    if (!DecodeProfileField(value.substr(2,a-2), loaded.apn, sizeof(loaded.apn)) ||
        !DecodeProfileField(value.substr(a+1,b-a-1), loaded.username, sizeof(loaded.username)) ||
        !DecodeProfileField(value.substr(b+1), loaded.password, sizeof(loaded.password)))
        return -EINVAL;
    *profile = loaded;
    return 0;
}

extern "C" int metalio_app_save_sim_profile(const metalio_sim_profile_s *profile)
{
    if (!profile || !ValidSimId(profile->iccid) || profile->auth < 0 || profile->auth > 3 ||
        !ValidApnField(profile->apn, sizeof(profile->apn)) ||
        !ValidApnField(profile->username, sizeof(profile->username)) ||
        !ValidApnField(profile->password, sizeof(profile->password))) return -EINVAL;
    std::string value(1, (char)('0'+profile->auth));
    value += '|' + EncodeProfileField(profile->apn) + '|' +
             EncodeProfileField(profile->username) + '|' + EncodeProfileField(profile->password);
    Settings cards("sim_apn", true);
    cards.SetString(profile->iccid, value);
    return 0;
}

extern "C" int metalio_app_configure_sim_profile(const metalio_sim_profile_s *profile)
{
    CellularStartGuard guard;
    const int previous = s_active_path.load();
    Settings preference("network", false);
    if (preference.GetBool("airplane", false)) return -EPERM;
    int radio = metalio_nt26_set_radio(true);
    if (radio < 0) return radio;
    metalio_sim_profile_s old{};
    int probe = metalio_nt26_read_sim_profile(&old);
    if (probe < 0) { if (previous == 0 && !s_call_session.load() && !TelephonyStandbyAllowed()) (void)metalio_nt26_set_radio(false); return probe; }
    int ret = metalio_nt26_set_sim_profile(profile);
    if (ret < 0) { if (previous == 0 && !s_call_session.load() && !TelephonyStandbyAllowed()) (void)metalio_nt26_set_radio(false); return ret; }
    static NetworkService network;
    RetireInterface("eth1");
    s_active_path.store(-1);
    ret = BringUpPath(network, 1);
    if (ret == 0) {
        (void)metalio_esp_hosted_set_radio(false);
        Application::GetInstance().OnWifiLinkReady();
        Settings settings("network", true);
        const char *keys[] = {"type", "cellular", "wifi_enabled", "airplane"};
        const int32_t values[] = {1, 1, 0, 0};
        (void)settings.SetInts(keys, values, 4);
    } else {
        // This identity check prevents writing the old profile onto a newly inserted card.
        (void)metalio_nt26_set_sim_profile(&old);
        RestorePath(network, previous);
    }
    return ret;
}

extern "C" void metalio_app_cellular_sim_changed(bool present)
{
    s_sim_present.store(present);
    if (!s_cellular_requested.load() || s_sim_recovery_pending.exchange(true)) return;
    // Create the worker from the application task, keeping its descriptor ownership.
    Application::GetInstance().Schedule([] {
        pthread_t worker;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 65536);
        int ret = hi_task_create_legacy(&worker, &attr, [](void *) -> void * {
            {
                CellularStartGuard guard;
                if (s_cellular_requested.load()) {
                    RetireInterface("eth1");
                    s_active_path.store(-1);
                    if (s_sim_present.load()) {
                        static NetworkService network;
                        if (BringUpPath(network, 1) == 0)
                            Application::GetInstance().OnWifiLinkReady();
                    }
                }
            }
            s_sim_recovery_pending.store(false);
            return nullptr;
        }, nullptr);
        pthread_attr_destroy(&attr);
        if (ret == 0) pthread_detach(worker);
        else s_sim_recovery_pending.store(false);
    });
}

// A phone session may keep the modem RF on while Internet traffic stays on WiFi.
// This does not select eth1 or rewrite the saved data-network preference.
extern "C" int metalio_app_call_session(bool enabled)
{
    CellularStartGuard guard;
    if (enabled) {
        Settings network("network", false);
        if (network.GetBool("airplane", false)) return -EPERM;
        if (!s_call_session.load()) {
            const bool was_on = metalio_nt26_radio_enabled();
            int ret = was_on ? 0 : metalio_nt26_set_radio(true);
            if (ret < 0) return ret;
            s_call_radio_owned = !was_on;
            s_call_session.store(true);
        }
        return 0;
    }
    (void)metalio_audio_select_local();
    s_call_session.store(false);
    int ret = 0;
    if (s_call_radio_owned && !s_cellular_requested.load() && !TelephonyStandbyAllowed())
        ret = metalio_nt26_set_radio(false);
    s_call_radio_owned = false;
    return ret;
}
extern "C" int metalio_app_call_dial(const char *number)
{
    CellularStartGuard guard;
    PhoneService::StopRingtone();
    int ret = metalio_app_call_session(true);
    if(ret<0)return ret;
    PhoneService::DialStarted(number);
    ret=metalio_nt26_call_dial(number);
    if(ret<0)PhoneService::StateChanged(METALIO_CALL_IDLE);
    return ret;
}
extern "C" int metalio_app_call_answer(void)
{
    CellularStartGuard guard;
    PhoneService::StopRingtone();
    int ret = metalio_app_call_session(true);
    return ret < 0 ? ret : metalio_nt26_call_answer();
}
extern "C" int metalio_app_call_hangup(void)
{
    PhoneService::CancelRingtone();
    return metalio_nt26_call_hangup();
}
extern "C" int metalio_app_call_poll(void)
{
    // Global incoming calls work before opening Phone and independently of
    // the SMS SD-card store. Restore control/SIM standby, never eth1 data.
    if (!metalio_nt26_radio_enabled() || !SimCardUsable()) {
        const int standby = metalio_app_sms_ensure_standby();
        if (standby < 0) return standby;
    }
    return metalio_nt26_call_poll();
}

extern "C" int metalio_app_read_sim_profile(metalio_sim_profile_s *profile)
{
    CellularStartGuard guard;
    Settings preference("network", false);
    if (preference.GetBool("airplane", false)) return -EPERM;
    const bool was_on = metalio_nt26_radio_enabled();
    int ret = was_on ? 0 : metalio_nt26_set_radio(true);
    if (ret == 0) ret = metalio_nt26_read_sim_profile(profile);
    if (!was_on && !s_call_session.load() && !TelephonyStandbyAllowed())
        (void)metalio_nt26_set_radio(false);
    return ret;
}

extern "C" int metalio_app_unlock_sim(const char *pin, int expected_slot)
{
    // Reject malformed input before powering RF or consuming any PIN attempt.
    const size_t length = pin ? strnlen(pin, 9) : 0;
    if (length < 4 || length > 8 || (expected_slot != 0 && expected_slot != 1)) return -EINVAL;
    for (size_t i = 0; i < length; ++i) if (pin[i] < '0' || pin[i] > '9') return -EINVAL;
    CellularStartGuard guard;
    Settings preference("network", false);
    if (preference.GetBool("airplane", false)) return -EPERM;
    const int previous = s_active_path.load();
    const bool was_on = metalio_nt26_radio_enabled();
    const int previous_slot = metalio_nt26_get_sim_slot();
    int ret = was_on ? 0 : metalio_nt26_set_radio(true);
    if (ret == 0 && metalio_nt26_get_sim_slot() != expected_slot)
        ret = metalio_nt26_set_sim_slot(expected_slot);
    if (ret == 0) ret = metalio_nt26_unlock_sim(pin, expected_slot);
    static NetworkService network;
    if (ret < 0) {
        if (previous_slot == 0 || previous_slot == 1)
            (void)metalio_nt26_set_sim_slot(previous_slot);
        RestorePath(network, previous);
        if (!was_on && !s_call_session.load() && !TelephonyStandbyAllowed())
            (void)metalio_nt26_set_radio(false);
        return ret;
    }
    // Unlocking for WiFi phone/SMS standby must retain the current WiFi
    // route and saved Internet preference, rather than select 4G data.
    if (previous == 0) {
        Settings preferences("network", true);
        const char *keys[] = {"sim_slot"};
        const int32_t values[] = {expected_slot};
        return preferences.SetInts(keys, values, 1) ? 0 : -EIO;
    }
    RetireInterface("eth1");
    s_active_path.store(-1);
    ret = BringUpPath(network, 1);
    if (ret == 0) {
        (void)metalio_esp_hosted_set_radio(false);
        Application::GetInstance().OnWifiLinkReady();
        Settings preferences("network", true);
        const char *keys[] = {"type", "cellular", "wifi_enabled", "airplane", "sim_slot"};
        const int32_t values[] = {1, 1, 0, 0, expected_slot};
        if (!preferences.SetInts(keys, values, 5)) return -EIO;
    } else {
        if (previous_slot == 0 || previous_slot == 1)
            (void)metalio_nt26_set_sim_slot(previous_slot);
        RestorePath(network, previous);
        if (!was_on && previous != 1 && !s_call_session.load() && !TelephonyStandbyAllowed())
            (void)metalio_nt26_set_radio(false);
    }
    return ret;
}
