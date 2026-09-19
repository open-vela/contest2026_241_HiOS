/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * DigitalPeopleScreen — ported from MetalioClaw4
 * main/display/screen/digital_people_screen/digital_people_screen.h.
 *
 * 720x720 digital-human page: pure-black background with a centred
 * emotion resource loaded from SD card (system/emotion/{category}{ext}).
 * Resource format (.eaf / .sjpg) and EAF frame interval are persisted by
 * DigitalPeoplePrefs (file-based Settings on NuttX). Long-press anywhere
 * for 5 seconds opens DigitalPeopleSettingsScreen.
 *
 * On entry the page checks that all 6 required emotion categories are
 * present on the SD card; if any are missing, a centred hint guides the
 * user to copy the resource bundle into system/emotion/.
 *
 * Device-activation gate: if the device is not activated, an unclosable
 * modal dialog (with a back button) is shown over the page so the user
 * cannot interact with the digital human until activation completes.
 *
 * Chat bubbles:
 *   - ShowUserMessage(): user utterance -> bottom-centre bubble (below gif).
 *   - ShowSystemMessage(): system/assistant reply -> top-left bubble.
 *   Both are white-bordered, semi-transparent white chat bubbles, hidden
 *   by default. Calling either one refreshes the content and shows the
 *   bubble, overwriting the previous same-type message. All calls are
 *   no-ops while the screen is not in the foreground.
 *
 * Porting notes (openvela/NuttX):
 *   - lv_eaf.h / lv_eaf_create / lv_eaf_set_src / lv_eaf_set_frame_delay /
 *     lv_eaf_pause are NOT available on NuttX. When the active format is
 *     EAF, a centred label showing the emotion category name is used as
 *     a placeholder so the rest of the control flow (resource checks,
 *     bubbles, activation gate, long-press settings) remains exercisable.
 *     A real EAF decoder can be dropped in by replacing CreateEmotionWidget()
 *     and SetEmotionSrc() in the .cxx file.
 *   - esp_log.h -> esp_log_shim.h.
 *   - Application / SdCardManager / home_screen / screen_util are already
 *     ported; HomeScreen::Create() calls are redirected to
 *     HomeScreen::CreateStatic() to match the openvela Screen base pattern.
 *   - Class now inherits from Screen and overrides Create() / Name() while
 *     preserving the full original static API (IsActive, ShowUserMessage,
 *     ShowSystemMessage, ClearMessages, SetEmotion, LifecycleCallback).
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class DigitalPeopleScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "digital_people"; }

    // Legacy static entry point — used by home_screen / settings screen
    // to build the digital-people page (matches the MetalioClaw4 API).
    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    static bool IsActive();
    static void ShowUserMessage(const char *text);
    static void ShowSystemMessage(const char *text);
    static void ClearMessages();

    // Switch the emotion animation / static image being shown.
    // category must be one of: crying / happy / loving / neutral /
    // surprised / thinking. The path is assembled as
    //   S:/sdcard/system/emotion/{category}{ext}
    // where ext is read from DigitalPeoplePrefs (.eaf / .sjpg).
    // Safe to call while the screen is not on stage: the requested
    // category is cached and applied on the next Create().
    static void SetEmotion(const char *category);
};
