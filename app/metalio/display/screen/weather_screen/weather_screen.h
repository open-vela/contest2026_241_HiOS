/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class WeatherScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "weather"; }

    static lv_obj_t *CreateStatic();
};
