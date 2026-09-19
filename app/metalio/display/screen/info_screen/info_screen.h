/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * InfoScreen — ported from MetalioClaw4
 * main/display/screen/info_screen/info_screen.h.
 *
 * Scrollable system-info page: device model, chip, cores, firmware
 * version, MAC, UUID, flash / SRAM / PSRAM sizes, OTA partition, etc.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class InfoScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "info"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
