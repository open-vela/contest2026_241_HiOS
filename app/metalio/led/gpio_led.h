/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * GpioLed — ported from MetalioClaw4 main/led/gpio_led.h.
 *
 * Drives a single LED through the LEDC (PWM) peripheral with optional
 * hardware fade.  The ESP-IDF driver/ledc.h is replaced by the
 * nuttx_ledc.h stub; the rest of the logic (state → brightness mapping,
 * blink / fade timers) is preserved.
 */

#ifndef _GPIO_LED_H_
#define _GPIO_LED_H_

#include "led.h"
#include "nuttx_ledc.h"
#include "nuttx_led_strip.h"  /* for gpio_num_t / GPIO_NUM_NC */
#include "esp_timer_shim.h"
#include "freertos_shim.h"

#include <atomic>
#include <mutex>

class GpioLed : public Led
{
public:
    explicit GpioLed(gpio_num_t gpio);
    GpioLed(gpio_num_t gpio, int output_invert);
    GpioLed(gpio_num_t gpio, int output_invert,
            ledc_timer_t timer_num, ledc_channel_t channel);
    virtual ~GpioLed();

    void OnStateChanged() override;
    void TurnOn();
    void TurnOff();
    void SetBrightness(uint8_t brightness);

private:
    std::mutex mutex_;
    TaskHandle_t blink_task_ = 0;
    ledc_channel_config_t ledc_channel_ = {};
    bool ledc_initialized_ = false;
    uint32_t duty_ = 0;
    int blink_counter_ = 0;
    int blink_interval_ms_ = 0;
    esp_timer_handle_t blink_timer_ = nullptr;
    bool fade_up_ = true;

    void StartBlinkTask(int times, int interval_ms);
    void OnBlinkTimer();

    void BlinkOnce();
    void Blink(int times, int interval_ms);
    void StartContinuousBlink(int interval_ms);
    void StartFadeTask();
    void OnFadeEnd();
    static bool FadeCallback(const ledc_cb_param_t *param, void *user_arg);
};

#endif /* _GPIO_LED_H_ */
