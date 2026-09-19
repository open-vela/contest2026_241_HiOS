/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ThemeScreen — ported from MetalioClaw4
 * main/display/screen/theme_screen/theme_screen.h.
 *
 * Theme picker: 2x2 grid of preview cards. Tapping a non-active theme
 * pops a confirm dialog; confirming writes the choice via
 * ThemeManager and rebuilds the home screen so icons reload.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class ThemeScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "theme"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
