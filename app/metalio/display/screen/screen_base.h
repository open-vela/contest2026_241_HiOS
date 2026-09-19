/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen base class — provides a uniform lifecycle interface for all
 * MetalioClaw4 app screens. Each concrete screen overrides Create() to
 * build its LVGL widget tree and returns the root lv_obj_t*.
 *
 * Ported from the MetalioClaw4 screen pattern (originally each screen
 * exposed a static Create() that returned lv_obj_t*). The Screen base
 * class wraps that pattern so a ScreenManager can switch between
 * instances uniformly while the original per-screen widget code is
 * preserved verbatim.
 */

#pragma once

#include "lvgl.h"
#include "esp_log_shim.h"

#include <string>

class LvglDisplay;
class LvglTheme;

class Screen
{
public:
    Screen() = default;
    virtual ~Screen() = default;

    // Build the LVGL widget tree and return the root screen object
    // (parent == NULL, ready for lv_screen_load()). Idempotent: calling
    // Create() twice on the same instance will reuse the existing root.
    virtual lv_obj_t *Create() = 0;

    // Tear down LVGL objects owned by this screen. The default
    // implementation async-deletes the root object, which transitively
    // frees every child.
    virtual void Destroy();

    // Generic click handler. Concrete screens usually attach their own
    // per-widget event callbacks, but this hook is available for
    // screens that want centralised dispatch.
    virtual void OnButtonClick(lv_event_t *e);

    // Stable identifier used by ScreenManager (e.g. "home", "calculator").
    virtual const char *Name() const = 0;

    // Access the root LVGL object created by Create().
    inline lv_obj_t *root() const { return root_; }

    // Display / theme accessors. Set by ScreenManager before Create() is
    // called so screens can read the active theme colours / fonts.
    inline void set_display(LvglDisplay *display) { display_ = display; }
    inline void set_theme(LvglTheme *theme) { theme_ = theme; }
    inline LvglDisplay *display() const { return display_; }
    inline LvglTheme *theme() const { return theme_; }

protected:
    LvglDisplay *display_ = nullptr;
    LvglTheme *theme_ = nullptr;
    lv_obj_t *root_ = nullptr;
};
