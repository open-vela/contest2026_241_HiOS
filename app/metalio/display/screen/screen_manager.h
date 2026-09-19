/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ScreenManager — singleton registry that owns the active screen and
 * provides SwitchTo(name) to destroy the current screen and create a
 * new one. Mirrors the role played by HomeScreen's LaunchXxx() helpers
 * in the original MetalioClaw4 code, but in a uniform, data-driven
 * shape so the rest of the system (idle power policy, power-key
 * handler, etc.) can ask "what screen is showing?" without reaching
 * into home_screen internals.
 */

#pragma once

#include "screen_base.h"

#include <map>
#include <string>

class LvglDisplay;
class LvglTheme;

class ScreenManager
{
public:
    static ScreenManager &GetInstance()
    {
        static ScreenManager instance;
        return instance;
    }

    // Register a screen instance under `name`. The manager does not take
    // ownership of the pointer; callers are expected to keep the screen
    // alive for the program lifetime (screens are typically file-scope
    // singletons).
    void RegisterScreen(const std::string &name, Screen *screen);

    // Destroy the current screen (if any) and create + load the named
    // one. Returns true on success, false if `name` is unknown.
    bool SwitchTo(const std::string &name);

    // Return the active screen (nullptr if none).
    Screen *GetCurrent() const { return current_; }

    // Return the name of the active screen (empty if none).
    const std::string &GetCurrentName() const { return current_name_; }

    // Look up a registered screen by name (nullptr if not found).
    Screen *GetScreen(const std::string &name) const;

    // Inject the display / theme that will be handed to each screen
    // before its Create() is called.
    void set_display(LvglDisplay *display) { display_ = display; }
    void set_theme(LvglTheme *theme) { theme_ = theme; }

private:
    ScreenManager() = default;
    ~ScreenManager() = default;
    ScreenManager(const ScreenManager &) = delete;
    ScreenManager &operator=(const ScreenManager &) = delete;

    std::map<std::string, Screen *> screens_;
    Screen *current_ = nullptr;
    std::string current_name_;

    LvglDisplay *display_ = nullptr;
    LvglTheme *theme_ = nullptr;
};
