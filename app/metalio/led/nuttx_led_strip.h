/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX LED-strip + GPIO shim — mirrors the ESP-IDF led_strip / driver/gpio
 * API surface used by SingleLed and CircularStrip.
 *
 * The ESP-IDF implementation drives WS2812 addressable LEDs through the
 * RMT peripheral.  NuttX on esp32p4 does not yet expose a userland
 * addressable-LED driver, so this header provides a compile-compatible
 * stub that:
 *   - Accepts the same configuration structs
 *   - Buffers pixel data in RAM
 *   - Logs (ESP_LOGD) the refresh calls instead of toggling GPIOs
 *
 * When a real NuttX WS2812 driver lands (e.g. via the esp32p4 LP_IO +
 * DMA, or an external I2C LED driver), replace the bodies of
 * led_strip_set_pixel / led_strip_refresh / led_strip_clear with the
 * real hardware calls — no porting changes are needed in single_led.cxx
 * or circular_strip.cxx.
 */

#ifndef _NUTTX_LED_STRIP_H_
#define _NUTTX_LED_STRIP_H_

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err_shim.h"
#include "esp_log_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* GPIO types (subset of driver/gpio.h)                               */
/* ------------------------------------------------------------------ */

typedef int gpio_num_t;

#define GPIO_NUM_NC  (-1)

typedef enum
{
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT,
    GPIO_MODE_OUTPUT,
    GPIO_MODE_OUTPUT_OD,
    GPIO_MODE_INPUT_OUTPUT,
    GPIO_MODE_INPUT_OUTPUT_OD,
} gpio_mode_t;

typedef enum
{
    GPIO_PULLUP_DISABLE = 0,
    GPIO_PULLUP_ENABLE,
} gpio_pullup_t;

typedef enum
{
    GPIO_PULLDOWN_DISABLE = 0,
    GPIO_PULLDOWN_ENABLE,
} gpio_pulldown_t;

typedef enum
{
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_POSEDGE,
    GPIO_INTR_NEGEDGE,
    GPIO_INTR_ANYEDGE,
    GPIO_INTR_LOW_LEVEL,
    GPIO_INTR_HIGH_LEVEL,
} gpio_int_type_t;

typedef struct
{
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

/* GPIO config / set_level are stubbed — the real NuttX GPIO driver is
 * accessed through /dev/gpio via ioctl, not these ESP-IDF wrappers. */
static inline esp_err_t gpio_config(const gpio_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

static inline esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level)
{
    (void)gpio;
    (void)level;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* LED strip types (subset of led_strip.h)                            */
/* ------------------------------------------------------------------ */

typedef enum
{
    LED_STRIP_COLOR_COMPONENT_FMT_RGB = 0,
    LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    LED_STRIP_COLOR_COMPONENT_FMT_BGR,
} led_strip_color_component_format_t;

typedef enum
{
    LED_MODEL_WS2812 = 0,
    LED_MODEL_SK6812,
    LED_MODEL_WS2811,
} led_strip_model_t;

typedef struct
{
    int strip_gpio_num;
    int max_leds;
    led_strip_color_component_format_t color_component_format;
    led_strip_model_t led_model;
    /* ESP-IDF also has invert_out / flags; unused by ported code. */
} led_strip_config_t;

typedef struct
{
    uint32_t resolution_hz;
    /* ESP-IDF has more fields; only the one used by ported code is kept. */
} led_strip_rmt_config_t;

typedef struct led_strip_s
{
    int gpio;
    int max_leds;
    led_strip_color_component_format_t fmt;
    /* Per-pixel RGB buffer (max_leds * 3 bytes). */
    uint8_t *pixels;
} led_strip_t;

typedef led_strip_t *led_strip_handle_t;

static inline esp_err_t led_strip_new_rmt_device(
    const led_strip_config_t *config,
    const led_strip_rmt_config_t *rmt_config,
    led_strip_handle_t *ret_strip)
{
    if (!config || !ret_strip || config->max_leds <= 0)
        return ESP_ERR_INVALID_ARG;

    led_strip_t *s = (led_strip_t *)calloc(1, sizeof(*s));
    if (!s)
        return ESP_ERR_NO_MEM;
    s->pixels = (uint8_t *)calloc((size_t)config->max_leds, 3);
    if (!s->pixels)
    {
        free(s);
        return ESP_ERR_NO_MEM;
    }
    s->gpio = config->strip_gpio_num;
    s->max_leds = config->max_leds;
    s->fmt = config->color_component_format;
    (void)rmt_config;
    *ret_strip = s;
    ESP_LOGD("led_strip", "new rmt device gpio=%d max_leds=%d",
             config->strip_gpio_num, config->max_leds);
    return ESP_OK;
}

static inline esp_err_t led_strip_set_pixel(
    led_strip_handle_t strip, uint32_t index,
    uint8_t red, uint8_t green, uint8_t blue)
{
    if (!strip || (int)index >= strip->max_leds)
        return ESP_ERR_INVALID_ARG;
    /* Color component format reorders the bytes — keep RGB/GRB/BGR. */
    switch (strip->fmt)
    {
        default:
        case LED_STRIP_COLOR_COMPONENT_FMT_RGB:
            strip->pixels[index * 3 + 0] = red;
            strip->pixels[index * 3 + 1] = green;
            strip->pixels[index * 3 + 2] = blue;
            break;
        case LED_STRIP_COLOR_COMPONENT_FMT_GRB:
            strip->pixels[index * 3 + 0] = green;
            strip->pixels[index * 3 + 1] = red;
            strip->pixels[index * 3 + 2] = blue;
            break;
        case LED_STRIP_COLOR_COMPONENT_FMT_BGR:
            strip->pixels[index * 3 + 0] = blue;
            strip->pixels[index * 3 + 1] = green;
            strip->pixels[index * 3 + 2] = red;
            break;
    }
    return ESP_OK;
}

static inline esp_err_t led_strip_refresh(led_strip_handle_t strip)
{
    if (!strip)
        return ESP_ERR_INVALID_ARG;
    /* Stub: real hardware would push `strip->pixels` out the RMT/DMA
     * engine here.  We just log at verbose level so blink state machines
     * can be observed in syslog without a real LED. */
    ESP_LOGV("led_strip", "refresh gpio=%d max_leds=%d",
             strip->gpio, strip->max_leds);
    return ESP_OK;
}

static inline esp_err_t led_strip_clear(led_strip_handle_t strip)
{
    if (!strip)
        return ESP_ERR_INVALID_ARG;
    memset(strip->pixels, 0, (size_t)strip->max_leds * 3);
    return led_strip_refresh(strip);
}

static inline esp_err_t led_strip_del(led_strip_handle_t strip)
{
    if (!strip)
        return ESP_ERR_INVALID_ARG;
    free(strip->pixels);
    free(strip);
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* _NUTTX_LED_STRIP_H_ */
