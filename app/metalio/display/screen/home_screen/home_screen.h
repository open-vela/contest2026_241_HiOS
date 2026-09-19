/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * HomeScreen — ported from MetalioClaw4
 * main/display/screen/home_screen/home_screen.h.
 *
 * Fullscreen launcher: 720x720 panel with a status bar (time, battery,
 * network icon), a 3x3 grid of app icons per page, horizontal pager with
 * snap-scrolling and infinite loop, and a long-press power dialog.
 *
 * Every other ported screen returns to the home page via
 * HomeScreen::CreateStatic(), so this class MUST expose that static
 * entry point in addition to the Screen virtual-override Create().
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class HomeScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "home"; }

    // Legacy static entry point — used by every other ported screen to
    // return to the home page (matches the MetalioClaw4 API).
    static lv_obj_t *CreateStatic();

    /* Replace the active screen with a fresh HomeScreen. Frees the old
     * screen synchronously first so CreateStatic() does not OOM into the
     * LVGL default-theme blue fallback (boot→home and app→home). */
    static lv_obj_t *SwitchToHome();

    // Force the next CreateStatic() call to start from page 0 (used
    // after locale / theme changes so the user sees the first page).
    static void ResetToFirstPage();

    // Refresh the status bar from a non-LVGL thread (schedules an
    // async call onto the LVGL thread).
    static void RefreshStatusBar();

    // PWR_KEY long-press popup: [重启 / 关机] modal dialog. Must be
    // called on the LVGL thread.
    static void ShowPowerOptionsDialog();

    // Software shutdown — shows the shutdown screen and (on real
    // hardware) starts the PWR_KEY_PULSE sequence. On openvela the
    // pulse driver is not yet wired up, so the screen is shown and
    // the request is logged.
    static void RequestSystemShutdown(const char *reason);

    // Idle power thresholds (delegated to idle_power_policy).
    static int  GetIdleShutdownMinutes();
    static void SetIdleShutdownMinutes(int minutes);
    static int  GetIdleStandbyMinutes();
    static void SetIdleStandbyMinutes(int minutes);

    /* Voice/MCP: list and open desktop apps (id = icon_suffix, name = 中文名). */
    struct AppInfo {
        const char *id;
        const char *name;
        bool available;
    };
    static int GetAppCount();
    static bool GetAppInfo(int index, AppInfo *out);
    /* Match by id ("camera") or Chinese name ("相机"). Schedules LVGL launch. */
    static bool LaunchApp(const char *id_or_name);
};
