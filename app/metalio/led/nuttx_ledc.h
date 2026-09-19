/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX LEDC (PWM) shim — mirrors the ESP-IDF driver/ledc.h API surface
 * used by GpioLed.
 *
 * ESP-IDF's LEDC driver provides PWM output with hardware-assisted fade.
 * NuttX on esp32p4 does not yet expose a userland LEDC driver, so this
 * header provides a compile-compatible stub that:
 *   - Accepts the same configuration structs
 *   - Tracks duty / fade state in RAM
 *   - Logs (ESP_LOGD) duty updates instead of driving a PWM hardware block
 *
 * When a real NuttX LEDC / PWM driver lands, replace the bodies of
 * ledc_set_duty / ledc_update_duty / ledc_set_fade_with_time /
 * ledc_fade_start with the real hardware calls — no porting changes are
 * needed in gpio_led.cxx.
 */

#ifndef _NUTTX_LEDC_H_
#define _NUTTX_LEDC_H_

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_err_shim.h"
#include "esp_log_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* LEDC enums / types (subset of driver/ledc.h)                       */
/* ------------------------------------------------------------------ */

typedef enum
{
    LEDC_LOW_SPEED_MODE = 0,
    LEDC_HIGH_SPEED_MODE,
} ledc_mode_t;

typedef enum
{
    LEDC_TIMER_0 = 0,
    LEDC_TIMER_1,
    LEDC_TIMER_2,
    LEDC_TIMER_3,
} ledc_timer_t;

typedef enum
{
    LEDC_CHANNEL_0 = 0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2,
    LEDC_CHANNEL_3,
    LEDC_CHANNEL_4,
    LEDC_CHANNEL_5,
    LEDC_CHANNEL_6,
    LEDC_CHANNEL_7,
} ledc_channel_t;

typedef enum
{
    LEDC_TIMER_13_BIT = 13,
    LEDC_TIMER_14_BIT = 14,
} ledc_timer_bit_t;

typedef enum
{
    LEDC_AUTO_CLK = 0,
} ledc_clk_cfg_t;

typedef enum
{
    LEDC_FADE_NO_WAIT = 0,
    LEDC_FADE_WAIT_DONE,
    LEDC_FADE_MAX,
} ledc_fade_mode_t;

typedef enum
{
    LEDC_FADE_END_EVT = 0,
} ledc_event_t;

typedef struct
{
    ledc_event_t event;
} ledc_cb_param_t;

typedef bool (*ledc_cb_t)(const ledc_cb_param_t *param, void *user_arg);

typedef struct
{
    ledc_cb_t fade_cb;
} ledc_cbs_t;

typedef struct
{
    ledc_timer_bit_t duty_resolution;
    uint32_t freq_hz;
    ledc_mode_t speed_mode;
    ledc_timer_t timer_num;
    ledc_clk_cfg_t clk_cfg;
} ledc_timer_config_t;

typedef struct
{
    ledc_channel_t channel;
    uint32_t duty;
    int gpio_num;
    ledc_mode_t speed_mode;
    uint32_t hpoint;
    ledc_timer_t timer_sel;
    struct
    {
        unsigned int output_invert : 1;
    } flags;
} ledc_channel_config_t;

/* ------------------------------------------------------------------ */
/* LEDC API stubs                                                     */
/* ------------------------------------------------------------------ */

/* Single global channel-state slot is enough — the metalio-claw-4 board
 * only instantiates one GpioLed.  If multiple channels are needed,
 * promote this to an array indexed by (speed_mode, channel). */
typedef struct
{
    bool initialized;
    uint32_t duty;
    int gpio;
    bool output_invert;
    ledc_cb_t fade_cb;
    void *fade_user_arg;
    bool fade_active;
    uint32_t fade_target_duty;
    int fade_total_ms;
} nuttx_ledc_state_t;

static inline nuttx_ledc_state_t *nuttx_ledc_state(void)
{
    static nuttx_ledc_state_t s = {0};
    return &s;
}

static inline esp_err_t ledc_timer_config(const ledc_timer_config_t *cfg)
{
    if (!cfg)
        return ESP_ERR_INVALID_ARG;
    ESP_LOGD("ledc", "timer_config mode=%d timer=%d freq=%u res=%d",
             cfg->speed_mode, cfg->timer_num, cfg->freq_hz, cfg->duty_resolution);
    return ESP_OK;
}

static inline esp_err_t ledc_channel_config(const ledc_channel_config_t *cfg)
{
    if (!cfg)
        return ESP_ERR_INVALID_ARG;
    nuttx_ledc_state_t *s = nuttx_ledc_state();
    s->initialized = true;
    s->duty = cfg->duty;
    s->gpio = cfg->gpio_num;
    s->output_invert = cfg->flags.output_invert ? true : false;
    ESP_LOGD("ledc", "channel_config gpio=%d duty=%u invert=%d",
             cfg->gpio_num, cfg->duty, s->output_invert);
    return ESP_OK;
}

static inline esp_err_t ledc_fade_func_install(int intr_alloc_flags)
{
    (void)intr_alloc_flags;
    return ESP_OK;
}

static inline void ledc_fade_func_uninstall(void)
{
    nuttx_ledc_state_t *s = nuttx_ledc_state();
    s->fade_active = false;
}

static inline esp_err_t ledc_set_duty(ledc_mode_t mode, ledc_channel_t channel,
                                      uint32_t duty)
{
    (void)mode;
    (void)channel;
    nuttx_ledc_state_t *s = nuttx_ledc_state();
    s->duty = duty;
    return ESP_OK;
}

static inline esp_err_t ledc_update_duty(ledc_mode_t mode, ledc_channel_t channel)
{
    (void)mode;
    (void)channel;
    nuttx_ledc_state_t *s = nuttx_ledc_state();
    ESP_LOGV("ledc", "update_duty gpio=%d duty=%u", s->gpio, s->duty);
    return ESP_OK;
}

static inline esp_err_t ledc_fade_stop(ledc_mode_t mode, ledc_channel_t channel)
{
    (void)mode;
    (void)channel;
    nuttx_ledc_state_t *s = nuttx_ledc_state();
    s->fade_active = false;
    return ESP_OK;
}

static inline esp_err_t ledc_set_fade_with_time(ledc_mode_t mode,
                                                ledc_channel_t channel,
                                                uint32_t target_duty,
                                                int max_fade_time_ms)
{
    (void)mode;
    (void)channel;
    nuttx_ledc_state_t *s = nuttx_ledc_state();
    s->fade_target_duty = target_duty;
    s->fade_total_ms = max_fade_time_ms;
    return ESP_OK;
}

static inline esp_err_t ledc_fade_start(ledc_mode_t mode,
                                        ledc_channel_t channel,
                                        ledc_fade_mode_t fade_mode)
{
    (void)mode;
    (void)channel;
    (void)fade_mode;
    nuttx_ledc_state_t *s = nuttx_ledc_state();
    s->fade_active = true;
    s->duty = s->fade_target_duty;
    ESP_LOGD("ledc", "fade_start target_duty=%u", s->fade_target_duty);
    /* Stub: in ESP-IDF the hardware raises LEDC_FADE_END_EVT on completion.
     * We synchronously snap to the target duty and synthesize the callback
     * so the GpioLed fade loop keeps cycling. */
    if (s->fade_cb)
    {
        ledc_cb_param_t param;
        param.event = LEDC_FADE_END_EVT;
        s->fade_cb(&param, s->fade_user_arg);
    }
    s->fade_active = false;
    return ESP_OK;
}

static inline esp_err_t ledc_cb_register(ledc_mode_t mode,
                                         ledc_channel_t channel,
                                         const ledc_cbs_t *cbs,
                                         void *user_arg)
{
    (void)mode;
    (void)channel;
    if (!cbs)
        return ESP_ERR_INVALID_ARG;
    nuttx_ledc_state_t *s = nuttx_ledc_state();
    s->fade_cb = cbs->fade_cb;
    s->fade_user_arg = user_arg;
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* _NUTTX_LEDC_H_ */
