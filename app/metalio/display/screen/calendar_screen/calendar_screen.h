/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * CalendarScreen — ported from MetalioClaw4
 * main/display/screen/calendar_screen/calendar_screen.h.
 *
 * 720x720 fullscreen monthly calendar. Self-contained: only POSIX
 * localtime_r + LVGL. Reads the current system time on Create() and
 * renders the appropriate month with weekend coloring, "today"
 * highlight, prev/next-month greyed overflow days, and a "今天 M/D 周X"
 * badge in the header. Right-swipe gesture returns to HomeScreen.
 *
 * Ported 1:1 from the ESP-IDF source — every layout constant, color,
 * and date-math helper is preserved verbatim.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class CalendarScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "calendar"; }

    // Legacy static entry point — kept for callers that still use the
    // original MetalioClaw4 signature (e.g. home_screen).
    static lv_obj_t *CreateStatic();
};
