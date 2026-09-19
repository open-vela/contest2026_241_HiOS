/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * VibrateScreen — ported from MetalioClaw4
 * main/display/screen/vibrate_screen/vibrate_screen.h.
 *
 * Vibration motor control: amplitude slider (0..100%) and mode buttons
 * (off / weak / medium / strong / pulse / heartbeat). The motor is
 * driven by board LEDC PWM on GPIO 22 via /dev/pwm1 (LEDC timer1).
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class VibrateScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "vibrate"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
