/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * DigitalPeopleSettingsScreen — ported from MetalioClaw4
 * main/display/screen/digital_people_screen/digital_people_settings_screen.h.
 *
 * Digital-people settings sub-screen: pick the emotion resource format
 * (EAF animation / SJPG static image) and, when EAF is selected, adjust
 * the EAF frame interval (10–500 ms). Both values are persisted via
 * DigitalPeoplePrefs. Only the top-left back button returns to the
 * digital-people main page (right-swipe-back is intentionally disabled
 * to avoid clashing with the slider drag).
 *
 * Porting notes (openvela/NuttX):
 *   - esp_log.h -> esp_log_shim.h.
 *   - pwr_key_handler.h is already ported.
 *   - digital_people_screen.h / digital_people_prefs.h are local siblings.
 *   - Class now inherits from Screen and overrides Create() / Name() while
 *     preserving the original static Create() entry point as CreateStatic()
 *     so the digital-people main page can navigate here with the same call
 *     shape as the other openvela screens.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class DigitalPeopleSettingsScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "digital_people_settings"; }

    // Legacy static entry point — used by DigitalPeopleScreen to navigate
    // into the settings page (matches the MetalioClaw4 API).
    static lv_obj_t *CreateStatic();
};
