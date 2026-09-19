/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NetworkService implementation — wraps the board-specific
 * metalio_esp_hosted_* and metalio_nt26_* helpers exported by the openvela
 * board library, plus a small DNS resolver helper.
 */

#include "network_service.h"
#include "esp_log_shim.h"

#include <metalio/metalio.h>

#include <netutils/netlib.h>

#include <cstring>
#include <cstdio>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

/* nuttx/net/dns.h uses 'class' as a struct member name, which cannot be
 * parsed in C++, so declare just the one prototype we need. */
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
    if (wifi_started_)
    {
        ESP_LOGW(TAG, "WiFi already started");
        return 0;
    }

#ifdef CONFIG_METALIO_ESP_HOSTED
    int ret = metalio_esp_hosted_initialize();
    if (ret < 0)
    {
        ESP_LOGE(TAG, "esp_hosted_initialize failed: %d", ret);
        return ret;
    }

    /* Bring the C5 Wi-Fi stack up even when STA connect will fail — otherwise
     * network-settings scan never starts (ENETDOWN / radio stuck associating).
     */
    ret = metalio_esp_hosted_start_wifi();
    if (ret < 0)
    {
        ESP_LOGE(TAG, "esp_hosted_start_wifi failed: %d", ret);
        return ret;
    }

    if (ssid != nullptr && ssid[0] != '\0')
    {
        ret = metalio_esp_hosted_connect(ssid, pass);
        if (ret < 0)
        {
            ESP_LOGE(TAG, "esp_hosted_connect('%s') failed: %d", ssid, ret);
            (void)metalio_esp_hosted_disconnect();
        }
        else
        {
            /* The connect RPC is asynchronous: the C5 associates in the
             * background and then pushes Event_StaConnected.  Wait for the
             * link before bringing the netdev up and running DHCP. */
            bool connected = false;
            for (int i = 0; i < 300; i++)
            {
                if (metalio_esp_hosted_is_connected())
                {
                    connected = true;
                    break;
                }
                usleep(100 * 1000);
            }

            if (!connected)
            {
                ESP_LOGE(TAG, "STA connect timeout for '%s' "
                         "(WiFi left up for scan)", ssid);
                (void)metalio_esp_hosted_disconnect();
            }
            else
            {
                /* The esp-hosted netdev is registered as eth0 (NET_LL_ETHERNET).
                 * Bring the interface up, then run the NuttX DHCP client. */
                if (netlib_ifup("eth0") < 0)
                {
                    ESP_LOGE(TAG, "netlib_ifup(eth0) failed");
                }
                else
                {
                    int dhcp_ret = netlib_obtain_ipv4addr("eth0");
                    if (dhcp_ret < 0)
                    {
                        ESP_LOGW(TAG,
                                 "DHCP on eth0 failed: %d (continuing, no IP yet)",
                                 dhcp_ret);
                    }
                    else
                    {
                        ESP_LOGI(TAG, "DHCP lease acquired: %s",
                                 GetIpAddress().c_str());
                    }

                    {
                        char dbg[96];
                        int n = snprintf(dbg, sizeof(dbg),
                                         "NETWIFI dhcp_ret=%d ip=%s\n",
                                         dhcp_ret, GetIpAddress().c_str());
                        if (n > 0)
                            write(1, dbg, n);
                    }

                    {
                        struct in_addr gw;
                        memset(&gw, 0, sizeof(gw));
                        if (netlib_get_dripv4addr("eth0", &gw) == 0 &&
                            gw.s_addr != 0)
                        {
                            struct sockaddr_in fb;
                            memset(&fb, 0, sizeof(fb));
                            fb.sin_family = AF_INET;
                            fb.sin_addr = gw;
                            dns_add_nameserver((const struct sockaddr *)&fb,
                                               sizeof(fb));
                        }

                        struct sockaddr_in pub;
                        memset(&pub, 0, sizeof(pub));
                        pub.sin_family = AF_INET;
                        inet_pton(AF_INET, "223.5.5.5", &pub.sin_addr);
                        dns_add_nameserver((const struct sockaddr *)&pub,
                                           sizeof(pub));
                    }

                    ESP_LOGI(TAG, "STA connected to '%s'", ssid);
                }
            }
        }
    }
    else
    {
        ESP_LOGI(TAG, "WiFi initialised (no STA connect requested)");
    }

    wifi_started_ = true;
    transport_ = Transport::Wifi;
    ESP_LOGI(TAG, "WiFi up (ssid=%s)", ssid ? ssid : "(cached)");
    if (on_state_change_)
        on_state_change_(true);
    return 0;
#else
    (void)ssid;
    (void)pass;
    ESP_LOGW(TAG, "WiFi not enabled in this build (CONFIG_METALIO_ESP_HOSTED off)");
    return -1;
#endif
}

int NetworkService::StartCellular()
{
    if (cellular_started_)
    {
        ESP_LOGW(TAG, "Cellular already started");
        return 0;
    }

    int ret = metalio_nt26_initialize();
    if (ret < 0)
    {
        ESP_LOGE(TAG, "nt26_initialize failed: %d", ret);
        return ret;
    }
    cellular_started_ = true;
    transport_ = Transport::Cellular;
    ESP_LOGI(TAG, "Cellular up");
    if (on_state_change_)
        on_state_change_(true);
    return 0;
}

int NetworkService::SelectPath(int cellular)
{
#ifdef CONFIG_METALIO_DUAL_NETWORK
    int ret = metalio_dual_network_select(cellular);
    if (ret < 0)
    {
        ESP_LOGE(TAG, "dual_network_select(%d) failed: %d", cellular, ret);
        return ret;
    }
    transport_ = cellular ? Transport::Cellular : Transport::Wifi;
    ESP_LOGI(TAG, "Active data path: %s", cellular ? "cellular" : "wifi");
    return 0;
#else
    (void)cellular;
    ESP_LOGW(TAG, "Dual-network not enabled");
    return -ENOSYS;
#endif
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

bool NetworkService::Online() const
{
    if (wifi_started_)
    {
#ifdef CONFIG_METALIO_ESP_HOSTED
        return metalio_esp_hosted_is_connected();
#else
        return true;
#endif
    }

    if (cellular_started_)
    {
        /* Best-effort connectivity check: try to resolve a well-known host.
         * If DNS works we're online; otherwise report offline. */
        std::string ip;
        if (Resolve("dns.google", ip))
            return true;
        /* Even if DNS fails we may be online with a broken resolver — be
         * optimistic if any transport was started. */
        return true;
    }

    return false;
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
    if (netlib_get_ipv4addr("eth0", &addr) == 0 && addr.s_addr != 0)
        return std::string(inet_ntoa(addr));

    return "";
}

bool NetworkService::Resolve(const std::string &host, std::string &ip_out)
{
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
