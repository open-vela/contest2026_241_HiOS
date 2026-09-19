/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * SecondaryScreen — ported from MetalioClaw4
 * main/display/screen/secondary_screen/secondary_screen.cc.
 *
 * USB extend screen page. The original uses ESP-IDF USB device + touch
 * feed + LCD touch APIs; on NuttX these are stubbed with a clean
 * interface. Settings (JPEG quality, FPS, frame limit) use the
 * file-based Settings class.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class SecondaryScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "secondary"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
