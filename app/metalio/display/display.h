/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display base class — ported from MetalioClaw4 main/display/display.h.
 * Replaces esp_timer/esp_log/esp_pm with NuttX shims.
 */

#ifndef METALIO_DISPLAY_H
#define METALIO_DISPLAY_H

#include "esp_log_shim.h"
#include "esp_timer_shim.h"
#include "esp_pm_shim.h"

#include <lvgl.h>
#include <string>
#include <chrono>

class Theme
{
public:
    Theme(const std::string &name) : name_(name) {}
    virtual ~Theme() = default;
    inline std::string name() const { return name_; }

private:
    std::string name_;
};

class Display
{
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char *status);
    virtual void ShowNotification(const char *notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetEmotion(const char *emotion);
    virtual void SetChatMessage(const char *role, const char *content);
    virtual void SetTheme(Theme *theme);
    virtual Theme *GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);

    /* Start display-specific background thread (e.g. LVGL event loop).
     * Default no-op; NuttxLvglDisplay overrides to start LVGL thread. */
    virtual void StartDisplayThread() {}

    /* Allow boot→home after network bring-up (NuttxLvglDisplay). */
    virtual void ArmHomeTransition() {}
    /* True after boot→home CreateStatic finished (or no display). */
    virtual bool IsHomeReady() const { return true; }

    /* Check whether the display hardware is active (fb registered, etc.) */
    virtual bool IsActive() const { return false; }

    inline int width() const { return width_; }
    inline int height() const { return height_; }

protected:
    int width_ = 0;
    int height_ = 0;
    Theme *current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

class DisplayLockGuard
{
public:
    DisplayLockGuard(Display *display) : display_(display)
    {
        if (display_ && !display_->Lock(30000))
        {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard()
    {
        if (display_)
            display_->Unlock();
    }

private:
    Display *display_;
};

class NoDisplay : public Display
{
private:
    virtual bool Lock(int timeout_ms = 0) override { return true; }
    virtual void Unlock() override {}
};

#endif // METALIO_DISPLAY_H
