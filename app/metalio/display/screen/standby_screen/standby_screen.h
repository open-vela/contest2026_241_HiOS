/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * StandbyScreen — ported from MetalioClaw4
 * main/display/screen/standby_screen/standby_screen.h.
 *
 * Fullscreen standby page: flip-clock (HH MM SS), date / weekday, and
 * a charging particle effect. Tap or short-press the side key to return
 * to the home screen.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class StandbyScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "standby"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // Switch to the standby page (must be called on the LVGL thread).
    static void Show();
    // Return from standby to the home page (must be called on the LVGL thread).
    static void ReturnHome();
};
