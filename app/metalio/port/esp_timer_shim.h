/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-IDF timer shim for NuttX/openvela.
 * Maps esp_timer to NuttX work_queue.
 */

#ifndef __ESP_TIMER_SHIM_H
#define __ESP_TIMER_SHIM_H

#include <nuttx/config.h>
#include <nuttx/clock.h>
#include <nuttx/wqueue.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*esp_timer_cb_t)(void *arg);

typedef struct esp_timer_shim
{
    struct work_s work;
    bool periodic;
    bool active;
    uint32_t period_us;
    esp_timer_cb_t cb;
    void *arg;
} esp_timer_t;

typedef esp_timer_t *esp_timer_handle_t;

typedef struct
{
    esp_timer_cb_t callback;
    void *arg;
    const char *name;
    /* ESP-IDF compatibility fields — ignored by the NuttX work_queue shim. */
    int dispatch_method;      /* ESP_TIMER_TASK / ESP_TIMER_ISR */
    bool skip_unhandled_events;
} esp_timer_create_args_t;

/* ESP-IDF timer dispatch method enums (ignored by the shim) */
#define ESP_TIMER_TASK 0
#define ESP_TIMER_ISR  1

static inline void esp_timer_shim_trampoline(void *arg)
{
    esp_timer_t *t = (esp_timer_t *)arg;
    if (!t || !t->cb)
        return;

    t->cb(t->arg);

    if (t->periodic && t->active)
    {
        /* Re-arm for next period */
        uint32_t delay_ms = t->period_us / 1000;
        if (delay_ms == 0)
            delay_ms = 1;
        work_queue(LPWORK, &t->work, esp_timer_shim_trampoline, t,
                   MSEC2TICK(delay_ms));
    }
}

static inline esp_err_t esp_timer_create(
    const esp_timer_create_args_t *args,
    esp_timer_handle_t *handle)
{
    if (!args || !handle)
        return -EINVAL;

    esp_timer_t *t = (esp_timer_t *)calloc(1, sizeof(esp_timer_t));
    if (!t)
        return -ENOMEM;

    t->cb = args->callback;
    t->arg = args->arg;
    t->periodic = false;
    t->active = false;
    *handle = t;
    return 0;
}

static inline esp_err_t esp_timer_start_periodic(
    esp_timer_handle_t timer, uint64_t period_us)
{
    if (!timer)
        return -EINVAL;

    timer->periodic = true;
    timer->active = true;
    timer->period_us = (uint32_t)period_us;

    uint32_t delay_ms = period_us / 1000;
    if (delay_ms == 0)
        delay_ms = 1;

    return work_queue(LPWORK, &timer->work, esp_timer_shim_trampoline,
                      timer, MSEC2TICK(delay_ms));
}

static inline esp_err_t esp_timer_start_once(
    esp_timer_handle_t timer, uint64_t timeout_us)
{
    if (!timer)
        return -EINVAL;

    timer->periodic = false;
    timer->active = true;
    timer->period_us = (uint32_t)timeout_us;

    uint32_t delay_ms = timeout_us / 1000;
    if (delay_ms == 0)
        delay_ms = 1;

    return work_queue(LPWORK, &timer->work, esp_timer_shim_trampoline,
                      timer, MSEC2TICK(delay_ms));
}

static inline esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    if (!timer)
        return -EINVAL;
    timer->active = false;
    work_cancel(LPWORK, &timer->work);
    return 0;
}

static inline esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    if (!timer)
        return -EINVAL;
    timer->active = false;
    work_cancel(LPWORK, &timer->work);
    free(timer);
    return 0;
}

static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#ifdef __cplusplus
}
#endif

#endif /* __ESP_TIMER_SHIM_H */
