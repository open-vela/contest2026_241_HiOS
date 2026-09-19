/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NetworkService — WiFi / cellular bring-up and state monitoring for the
 * openvela port. Wraps the board-specific esp_hosted / NT26 driver helpers
 * declared in vendor/metalio/include/metalio/metalio.h.
 *
 * The xiaozhi protocol stack doesn't depend on this directly — it goes
 * through Board::GetNetwork() in the reference implementation. The openvela
 * port keeps NetworkService as a thin standalone class so the application
 * can bring up WiFi/cellular before the protocol layer starts.
 */

#ifndef METALIO_NETWORK_SERVICE_H
#define METALIO_NETWORK_SERVICE_H

#include <string>
#include <functional>

class NetworkService
{
public:
    enum class Transport
    {
        None,
        Wifi,
        Cellular,
    };

    NetworkService();
    ~NetworkService();

    /* Bring up WiFi via the esp-hosted SPI slave. If ssid/pass are null or
     * empty, the esp-hosted stack is initialised but no STA connect is issued
     * (useful when the cached credentials are still valid). */
    int StartWifi(const char *ssid = nullptr, const char *pass = nullptr);

    /* Bring up the NT26 4G modem. */
    int StartCellular();

    /* Select the active data path (0=WiFi, 1=Cellular) on dual-network
     * boards. No-op on single-network boards. */
    int SelectPath(int cellular);

    /* Stop the active transport. */
    void Stop();

    /* True when at least one transport reports an IP address. */
    bool Online() const;

    Transport transport() const { return transport_; }
    std::string GetSsid() const;
    std::string GetIpAddress() const;

    /* Resolve a hostname to an IPv4 dotted-quad string. Returns true on
     * success. Useful as a DNS sanity check before opening a socket. */
    static bool Resolve(const std::string &host, std::string &ip_out);

    /* State-change callback (called from the network driver context). */
    void OnStateChange(std::function<void(bool online)> cb)
    {
        on_state_change_ = std::move(cb);
    }

private:
    bool wifi_started_{false};
    bool cellular_started_{false};
    Transport transport_{Transport::None};
    std::function<void(bool online)> on_state_change_;
};

#endif /* METALIO_NETWORK_SERVICE_H */
