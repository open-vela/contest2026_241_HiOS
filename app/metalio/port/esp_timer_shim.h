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
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/sched.h>
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
    bool deleting;
    bool deferred_free;
    bool running;
    pid_t callback_tid;
    uint32_t generation;
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

/* Deleted callbacks must never requeue a freed work_s on LPWORK. */
static inline void esp_timer_shim_trampoline(void *arg)
{
    esp_timer_t *t = (esp_timer_t *)arg;
    irqstate_t flags = enter_critical_section();
    if (!t->active || t->deleting) {
        leave_critical_section(flags);
        return;
    }
    uint32_t generation = t->generation;
    t->running = true;
    t->callback_tid = nxsched_gettid();
    if (!t->periodic) t->active = false;
    leave_critical_section(flags);
    t->cb(t->arg);
    flags = enter_critical_section();
    if (!t->deleting && t->active && t->periodic && t->generation == generation) {
        uint32_t delay_ms = (t->period_us + 999u) / 1000u;
        if (!delay_ms) delay_ms = 1;
        if (work_queue(LPWORK, &t->work, esp_timer_shim_trampoline, t,
                       MSEC2TICK(delay_ms)) < 0) t->active = false;
    }
    t->running = false;
    bool release = t->deferred_free;
    leave_critical_section(flags);
    if (release) free(t);
}

static inline esp_err_t esp_timer_create(
    const esp_timer_create_args_t *args,
    esp_timer_handle_t *handle)
{
    if (!args || !handle || !args->callback)
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

static inline esp_err_t esp_timer_shim_start(esp_timer_handle_t timer,
                                              uint64_t delay_us, bool periodic)
{
    if (!timer || delay_us == 0 || delay_us > UINT32_MAX - 999u) return -EINVAL;
    irqstate_t flags = enter_critical_section();
    if (timer->deleting) {
        leave_critical_section(flags);
        return -EINVAL;
    }
    ++timer->generation;
    timer->periodic = periodic;
    timer->active = true;
    timer->period_us = (uint32_t)delay_us;
    int ret = work_queue(LPWORK, &timer->work, esp_timer_shim_trampoline,
                         timer, MSEC2TICK((delay_us + 999u) / 1000u));
    if (ret < 0) timer->active = false;
    leave_critical_section(flags);
    return ret;
}
static inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us)
{
    return esp_timer_shim_start(timer, period_us, true);
}
static inline esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    return esp_timer_shim_start(timer, timeout_us, false);
}
static inline esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    if (!timer) return -EINVAL;
    irqstate_t flags = enter_critical_section();
    timer->active = false;
    ++timer->generation;
    work_cancel(LPWORK, &timer->work);
    leave_critical_section(flags);
    return 0;
}
static inline esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    if (!timer) return -EINVAL;
    irqstate_t flags = enter_critical_section();
    timer->active = false;
    timer->deleting = true;
    ++timer->generation;
    const bool self = timer->running && timer->callback_tid == nxsched_gettid();
    timer->deferred_free = self;
    work_cancel(LPWORK, &timer->work);
    leave_critical_section(flags);
    if (!self) {
        /* Wait for an already running callback before freeing its argument. */
        work_cancel_sync(LPWORK, &timer->work);
        free(timer);
    }
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
