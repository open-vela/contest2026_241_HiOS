/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * TestScreen — ported from MetalioClaw4
 * main/display/screen/test_screen/test_screen.h.
 *
 * Hardware test menu: auto test / stress test / screen test / touch test.
 * The original test_screen directory contains a dozen sub-screens
 * (audio_test, camera_test, gps_test, ...); on openvela those sub-screens
 * are stubbed with TODO markers and the menu launches placeholders.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class TestScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "test"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // Unified entry point used by home_screen.
    static void LaunchFromHome(screen_lifecycle_cb_t lifecycle_cb);
};
