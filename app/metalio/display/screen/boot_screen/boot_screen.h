/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * BootScreen — ported from MetalioClaw4
 * main/display/screen/boot_screen/boot_screen.h.
 *
 * Fullscreen boot page: black background with the boot animation.
 * The original used the EAF (embedded animation framework) widget
 * (lv_eaf_create); on NuttX/openvela that widget is not available, so
 * the animation is stubbed and a static logo placeholder is shown.
 */

#pragma once

#include "screen_base.h"
#include "lvgl.h"

class BootScreen : public Screen
{
public:
    // Build a fullscreen boot page (parent == NULL).
    lv_obj_t *Create() override;
    const char *Name() const override { return "boot"; }

    // Legacy static entry point — kept for callers that still use the
    // original MetalioClaw4 signature (e.g. metalio_main).
    static lv_obj_t *CreateStatic();
};
