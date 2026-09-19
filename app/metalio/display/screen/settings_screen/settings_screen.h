/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * SettingsScreen — ported from MetalioClaw4
 * main/display/screen/settings_screen/settings_screen.h.
 *
 * Tabbed settings: brightness / standby / volume / language / charge /
 * bluetooth. Hardware accessors (Backlight, AudioCodec, CX25601N charge
 * IC, BluetoothScreen) are stubbed on openvela pending driver ports.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class SettingsScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "settings"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
