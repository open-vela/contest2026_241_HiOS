/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * BluetoothScreen — ported from MetalioClaw4
 * main/display/screen/bluetooth_screen/bluetooth_screen.h.
 *
 * BT audio module AT-command control panel, normally embedded inside
 * the Settings "Bluetooth" tab via BuildInto(). Three operating modes:
 *   mode 1: AT+RX=2 / AT+MODE=1  (receive)
 *   mode 2: AT+TX=1 / AT+MODE=2  (pair / scan)
 *   mode 3: AT+RX=1 / AT+MODE=3  (music receive)
 *
 * The original talks to the BT chip over SimpleUart; openvela does not
 * yet expose that UART helper, so the AT commands are stubbed (TODO).
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class BluetoothScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "bluetooth"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // Embed the BT control UI into a parent container (no header / back).
    static void BuildInto(lv_obj_t *parent);
    static void ResetUi();

    // Send AT+RX=2 / AT+MODE=1 at boot to default the BT module to
    // receive mode. Safe to call before LVGL is up.
    static void ApplyDefaultMode();
};
