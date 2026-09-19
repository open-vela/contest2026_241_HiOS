/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Game2048Screen — ported from MetalioClaw4
 * main/display/screen/game_2048_screen/game_2048_screen.cc.
 *
 * A 720x720 fullscreen 2048 game. Swipe on the board to move tiles.
 * Self-contained: only depends on LVGL and i18n.
 */

#pragma once

#include "screen_base.h"
#include "lvgl.h"

class Game2048Screen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "game_2048"; }

    /* Legacy static entry point — builds a fullscreen 2048 page. */
    static lv_obj_t *CreateStatic();
};
