/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX-compatible SystemInfo implementation.
 * Uses /proc/mem, mallinfo, and boardctl instead of ESP-IDF APIs.
 */

#include "system_info.h"
#include "esp_log_shim.h"
#include "board_shim.h"
#include "settings.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <ctime>

#include <sys/stat.h>
#include <malloc.h>

#ifdef CONFIG_NUTTX_KERNEL
#include <nuttx/board.h>
#endif

#ifdef CONFIG_NET
#include <netutils/netlib.h>
#endif

static const char *TAG = "SystemInfo";

size_t SystemInfo::GetFlashSize()
{
    /* NuttX: return configured flash size (32 MB for Metalio Claw4) */
    return 32 * 1024 * 1024;
}

size_t SystemInfo::GetMinimumFreeHeapSize()
{
    /* NuttX mallinfo doesn't track historical minimum; use mxordblk
     * (largest contiguous free block) as an approximation. */
    struct mallinfo mi = mallinfo();
    return static_cast<size_t>(mi.mxordblk);
}

size_t SystemInfo::GetFreeHeapSize()
{
    struct mallinfo mi = mallinfo();
    /* fordblks = total size of free (not in use) chunks */
    return static_cast<size_t>(mi.fordblks);
}

std::string SystemInfo::GetMacAddress()
{
    /* xiaozhi.me identifies the device by this MAC (Device-Id header).
     * Prefer the live eth0 address from esp-hosted; fall back to a
     * persisted value so the identity stays stable across boots. */
    char mac_str[18] = {};
    bool have_live = false;

#ifdef CONFIG_NET
    uint8_t mac[6] = {};
    if (netlib_getmacaddr("eth0", mac) == 0)
    {
        bool nonzero = false;
        for (int i = 0; i < 6; ++i)
        {
            if (mac[i] != 0)
            {
                nonzero = true;
                break;
            }
        }
        if (nonzero)
        {
            std::snprintf(mac_str, sizeof(mac_str),
                          "%02X:%02X:%02X:%02X:%02X:%02X",
                          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            have_live = true;
        }
    }
#endif

    Settings settings("board", true);
    if (have_live)
    {
        if (settings.GetString("mac") != mac_str)
            settings.SetString("mac", mac_str);
        return std::string(mac_str);
    }

    std::string stored = settings.GetString("mac");
    if (!stored.empty())
        return stored;

    /* Last resort: locally-administered random MAC, persisted. */
    unsigned seed = static_cast<unsigned>(time(nullptr));
    srand(seed);
    std::snprintf(mac_str, sizeof(mac_str), "02:%02X:%02X:%02X:%02X:%02X",
                  rand() & 0xff, rand() & 0xff, rand() & 0xff,
                  rand() & 0xff, rand() & 0xff);
    settings.SetString("mac", mac_str);
    ESP_LOGW(TAG, "No eth0 MAC, generated local address %s", mac_str);
    return std::string(mac_str);
}

std::string SystemInfo::GetChipModelName()
{
    return std::string("esp32p4");
}

std::string SystemInfo::GetUserAgent()
{
    return std::string("metalio-claw-4/1.0.0-openvela");
}

void SystemInfo::PrintTaskList()
{
    /* NuttX: /proc/[pid]/status or ps builtin */
    ESP_LOGI(TAG, "Task list: (use 'ps' from nsh>)");
}

void SystemInfo::PrintHeapStats()
{
    struct mallinfo mi = mallinfo();
    ESP_LOGI(TAG, "Heap: arena=%d used=%d free=%d",
             mi.arena, mi.uordblks, mi.fordblks);
}
