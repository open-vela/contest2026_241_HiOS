/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * SingleLed — ported from MetalioClaw4 main/led/single_led.h.
 *
 * Drives a single WS2812 addressable LED.  The ESP-IDF led_strip RMT
 * driver is replaced by the nuttx_led_strip.h stub; the rest of the
 * logic (state → color/blink mapping, blink timer) is preserved.
 */

#ifndef _SINGLE_LED_H_
#define _SINGLE_LED_H_

#include "led.h"
#include "nuttx_led_strip.h"
#include "esp_timer_shim.h"
#include "freertos_shim.h"

#include <atomic>
#include <mutex>

class SingleLed : public Led
{
public:
    explicit SingleLed(gpio_num_t gpio);
    virtual ~SingleLed();

    void OnStateChanged() override;

private:
    std::mutex mutex_;
    TaskHandle_t blink_task_ = 0;
    led_strip_handle_t led_strip_ = nullptr;
    uint8_t r_ = 0, g_ = 0, b_ = 0;
    int blink_counter_ = 0;
    int blink_interval_ms_ = 0;
    esp_timer_handle_t blink_timer_ = nullptr;

    void StartBlinkTask(int times, int interval_ms);
    void OnBlinkTimer();

    void BlinkOnce();
    void Blink(int times, int interval_ms);
    void StartContinuousBlink(int interval_ms);
    void TurnOn();
    void TurnOff();
    void SetColor(uint8_t r, uint8_t g, uint8_t b);
};

#endif /* _SINGLE_LED_H_ */
