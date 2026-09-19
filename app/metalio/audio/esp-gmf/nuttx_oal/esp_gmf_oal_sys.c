/*
 * SPDX-FileCopyrightText: 2026 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX OAL system implementation — replaces esp_gmf_oal_sys.c.
 *
 * Maps the ESP-GMF OAL system API to NuttX POSIX:
 *   esp_gmf_oal_sys_get_tick_by_time_ms  →  ms / portTICK_PERIOD_MS
 *   esp_gmf_oal_sys_get_time_ms          →  gettimeofday (POSIX)
 *   esp_gmf_oal_sys_get_real_time_stats  →  STUB (FreeRTOS task stats not available)
 *
 * The real-time stats function is a debug/diagnostic feature that lists
 * all FreeRTOS tasks with their CPU usage. NuttX does not have an
 * equivalent API, so this is stubbed to return ESP_GMF_ERR_FAIL.
 */

#include <sys/time.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"   /* portTICK_PERIOD_MS */
#include "esp_log.h"
#include "esp_gmf_err.h"
#include "esp_gmf_oal_mem.h"
#include "esp_gmf_oal_sys.h"

static const char *TAG = "ESP_GMF_OAL_SYS";

int esp_gmf_oal_sys_get_tick_by_time_ms(int ms)
{
    return (int)(ms / portTICK_PERIOD_MS);
}

int64_t esp_gmf_oal_sys_get_time_ms(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return (int64_t)t.tv_sec * 1000LL + (int64_t)t.tv_usec / 1000;
}

/* FreeRTOS task stats — not available on NuttX. The original code has
 * an #else branch that returns ESP_GMF_ERR_FAIL with a warning; we do
 * the same here unconditionally. */
esp_gmf_err_t esp_gmf_oal_sys_get_real_time_stats(int elapsed_time_ms, bool markdown)
{
    (void)elapsed_time_ms;
    (void)markdown;
    ESP_LOGW(TAG, "esp_gmf_oal_sys_get_real_time_stats not supported on NuttX");
    return ESP_GMF_ERR_FAIL;
}
