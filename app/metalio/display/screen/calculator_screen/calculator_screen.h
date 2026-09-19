/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * CalculatorScreen — ported from MetalioClaw4
 * main/display/screen/calculator_screen/calculator_screen.h.
 *
 * 720x720 fullscreen calculator app. Fully self-contained: no hardware
 * dependencies. The original is a static `Calculator::Create()` factory;
 * this port wraps it in the `Screen` base class so ScreenManager can
 * drive it uniformly while preserving every button, the history line,
 * the big display, and the full iOS-style keypad layout verbatim.
 *
 * Calculation logic (digit / dot / sign / percent / + - x / = / AC /
 * backspace, NaN/inf error state) is ported faithfully from the
 * ESP-IDF source — no behaviour changes.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class CalculatorScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "calculator"; }

    // Legacy static entry point kept for home_screen / metalio_main callers
    // that still use the original MetalioClaw4 signature.
    static lv_obj_t *CreateStatic();
};
