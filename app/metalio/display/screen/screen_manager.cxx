/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ScreenManager — singleton registry that owns the active screen and
 * provides SwitchTo(name) to destroy the current screen and create a
 * new one.
 */

#include "screen_manager.h"

#include "esp_log_shim.h"

namespace {
constexpr const char *TAG = "ScreenManager";
}  // namespace

void ScreenManager::RegisterScreen(const std::string &name, Screen *screen)
{
    if (screen == nullptr)
    {
        ESP_LOGW(TAG, "RegisterScreen('%s'): null screen", name.c_str());
        return;
    }
    screens_[name] = screen;
    ESP_LOGI(TAG, "registered screen '%s'", name.c_str());
}

Screen *ScreenManager::GetScreen(const std::string &name) const
{
    auto it = screens_.find(name);
    if (it == screens_.end())
    {
        return nullptr;
    }
    return it->second;
}

bool ScreenManager::SwitchTo(const std::string &name)
{
    Screen *next = GetScreen(name);
    if (next == nullptr)
    {
        ESP_LOGE(TAG, "SwitchTo('%s'): unknown screen", name.c_str());
        return false;
    }

    if (current_ == next)
    {
        ESP_LOGD(TAG, "SwitchTo('%s'): already active", name.c_str());
        return true;
    }

    // Tear down the outgoing screen before building the new one so the
    // LVGL tree is empty for a single frame -- matches the original
    // LaunchXxx() pattern.  Destroy() already schedules an async delete of
    // the outgoing root (and nulls it), so we must NOT delete the old
    // active screen again after lv_screen_load(): doing so queues a second
    // lv_obj_delete_async() on the same root, which double-frees it and
    // corrupts the LVGL heap (the source of the app blue/garbled screens).
    if (current_ != nullptr)
    {
        current_->Destroy();
    }

    next->set_display(display_);
    next->set_theme(theme_);

    lv_obj_t *new_scr = next->Create();
    if (new_scr == nullptr)
    {
        ESP_LOGE(TAG, "SwitchTo('%s'): Create() returned null", name.c_str());
        current_ = nullptr;
        current_name_.clear();
        return false;
    }

    lv_screen_load(new_scr);

    current_ = next;
    current_name_ = name;
    ESP_LOGI(TAG, "switched to '%s'", name.c_str());
    return true;
}
