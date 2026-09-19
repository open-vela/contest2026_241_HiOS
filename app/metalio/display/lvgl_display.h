/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL display — ported from MetalioClaw4 main/display/lvgl_display/lvgl_display.h.
 * Main display implementation using LVGL. Replaces esp_timer/esp_pm with shims.
 */

#ifndef METALIO_LVGL_DISPLAY_H
#define METALIO_LVGL_DISPLAY_H

#include "display.h"
#include "lvgl_image.h"
#include "lvgl_theme.h"

#include <lvgl.h>
#include "esp_timer_shim.h"
#include "esp_pm_shim.h"

#include <string>
#include <chrono>
#include <memory>

class LvglDisplay : public Display
{
public:
    LvglDisplay();
    virtual ~LvglDisplay();

    virtual void SetStatus(const char *status);
    virtual void ShowNotification(const char *notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual bool SnapshotToJpeg(std::string &jpeg_data, int quality = 80);

    /* LVGL-specific: set the emotion (emoji display) */
    virtual void SetEmotion(const char *emotion);
    virtual void SetChatMessage(const char *role, const char *content);
    virtual void SetTheme(Theme *theme);

    /* LVGL display access */
    inline lv_display_t *display() const { return display_; }

protected:
    esp_pm_lock_handle_t pm_lock_ = nullptr;
    lv_display_t *display_ = nullptr;

    lv_obj_t *network_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *notification_label_ = nullptr;
    lv_obj_t *mute_label_ = nullptr;
    lv_obj_t *battery_label_ = nullptr;
    lv_obj_t *low_battery_popup_ = nullptr;
    lv_obj_t *low_battery_label_ = nullptr;

    lv_obj_t *emotion_label_ = nullptr;
    lv_obj_t *chat_container_ = nullptr;
    lv_obj_t *chat_label_ = nullptr;

    const char *battery_icon_ = nullptr;
    const char *network_icon_ = nullptr;
    bool muted_ = false;

    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;

    /* Called by subclasses to initialize the status bar LVGL objects */
    void SetupStatusBar(lv_obj_t *parent);

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

#endif /* METALIO_LVGL_DISPLAY_H */
