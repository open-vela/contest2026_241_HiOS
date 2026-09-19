/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * TouchPanelTest — ported from MetalioClaw4
 * main/display/screen/test_screen/touch_panel_test.h.
 */

#pragma once

#include "lvgl.h"

class TouchPanelTest {
public:
    static lv_obj_t *Create();
};
