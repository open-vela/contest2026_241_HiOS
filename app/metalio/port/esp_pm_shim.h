/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-IDF power management shim for NuttX.
 * NuttX doesn't have esp_pm_lock; these are no-op stubs.
 */

#ifndef ESP_PM_SHIM_H
#define ESP_PM_SHIM_H

#include <stdint.h>
#include "esp_err_shim.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef void *esp_pm_lock_handle_t;

#define ESP_PM_APB_FREQ_MAX 0
#define ESP_PM_CPU_FREQ_MAX 1
#define ESP_PM_APB_FREQ_MIN 2
#define ESP_PM_CPU_FREQ_MIN 3

    static inline esp_err_t esp_pm_lock_create(int type, int arg,
                                               const char *name,
                                               esp_pm_lock_handle_t *handle)
    {
        (void)type;
        (void)arg;
        (void)name;
        if (handle)
            *handle = (void *)1; /* non-NULL sentinel */
        return ESP_OK;
    }

    static inline esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t handle)
    {
        (void)handle;
        return ESP_OK;
    }

    static inline esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t handle)
    {
        (void)handle;
        return ESP_OK;
    }

    static inline esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t handle)
    {
        (void)handle;
        return ESP_OK;
    }

#ifdef __cplusplus
}
#endif

#endif /* ESP_PM_SHIM_H */
