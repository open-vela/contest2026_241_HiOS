/*
 * SPDX-FileCopyrightText: 2026 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * CicadaScreen — lightweight rewrite of 竹知了 (touch swing, deferred UI).
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class CicadaScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "cicada"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
