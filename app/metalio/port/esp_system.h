/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_system.h — shim for ESP-IDF system APIs.
 *
 * Maps esp_system.h functions to NuttX equivalents (boardctl, reboot).
 */

#ifndef ESP_SYSTEM_SHIM_H
#define ESP_SYSTEM_SHIM_H

#include <stdint.h>
#include <stdlib.h>
#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <sys/boardctl.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reset reasons — stubbed; NuttX doesn't expose a standard enum. */
typedef enum
{
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
} esp_reset_reason_t;

static inline esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_UNKNOWN;
}

/* Software reset — NuttX boardctl → board_reset → up_systemreset.
 * Flat builds: boardctl is available (CONFIG_BOARDCTL=y). Always fall
 * through to up_systemreset() if boardctl returns (must not hang). */
static inline void esp_restart(void)
{
#ifdef CONFIG_BOARDCTL_RESET
    (void)boardctl(BOARDIOC_RESET, 0);
#endif
    /* Hardware reset — never return to a black panel. */
    up_systemreset();
    for (;;)
    {
    }
}

static inline void esp_system_abort(const char *details)
{
    (void)details;
    abort();
}

/* Chip model — stubbed. */
typedef enum
{
    ESP_CHIP_UNKNOWN = 0,
    ESP_CHIP_ESP32P4,
} esp_chip_model_t;

static inline esp_chip_model_t esp_chip_model(void)
{
    return ESP_CHIP_ESP32P4;
}

/* Misc info getters — stubbed. */
static inline uint32_t esp_get_chip_revision(void) { return 0; }
static inline uint32_t esp_get_idf_version(void)   { return 0; }

/* Sleep helpers — NuttX uses nanosleep; keep the ESP-IDF names as wrappers. */
static inline void esp_deep_sleep(uint64_t time_in_us)
{
    (void)time_in_us;
    /* Not supported on NuttX without PM; no-op. */
}

static inline void esp_sleep(uint64_t time_in_us)
{
    (void)time_in_us;
}

#ifdef __cplusplus
}
#endif

#endif /* ESP_SYSTEM_SHIM_H */
