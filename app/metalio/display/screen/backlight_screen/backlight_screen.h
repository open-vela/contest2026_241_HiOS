/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * BacklightScreen — ported from MetalioClaw4
 * main/display/screen/backlight_screen/backlight_screen.h.
 *
 * Brightness slider page. Slider writes NVS and drives PwmBacklight
 * via Board::GetBacklight()->SetBrightness(). Voice control uses the
 * same path through MCP `self.screen.set_brightness`.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class BacklightScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "backlight"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
