/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * PinTestScreen — ported from MetalioClaw4
 * main/display/screen/pin_test_screen/pin_test_screen.cc.
 *
 * Hardware pin test page for ESP32-P4 direct GPIOs.
 * ESP-IDF driver/gpio.h replaced with a thin GPIO shim.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class PinTestScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "pin_test"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
