/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL display — ported from MetalioClaw4 main/display/lvgl_display/lvgl_display.cc.
 * Replaces esp_timer/esp_pm/Board with NuttX shims and board_shim.
 */

#include "lvgl_display.h"
#include "lvgl_theme.h"
#include "font_awesome.h"
#include "esp_log_shim.h"
#include "esp_timer_shim.h"
#include "esp_pm_shim.h"
#include "board_shim.h"

#include <lvgl.h>
#include <ctime>
#include <cstring>
#include <cstdio>     /* std::snprintf (and fallback global snprintf) */
#include <algorithm>

#define TAG "Display"

LvglDisplay::LvglDisplay()
{
    /* Notification timer — hides notification and shows status after duration */
    esp_timer_create_args_t notification_timer_args = {};
    notification_timer_args.callback = [](void *arg) {
        LvglDisplay *display = static_cast<LvglDisplay *>(arg);
        DisplayLockGuard lock(display);
        if (display->notification_label_)
            lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
        if (display->status_label_)
            lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
    };
    notification_timer_args.arg = this;
    notification_timer_args.dispatch_method = ESP_TIMER_TASK;
    notification_timer_args.name = "notification_timer";
    notification_timer_args.skip_unhandled_events = false;
    esp_timer_create(&notification_timer_args, &notification_timer_);

    /* Power management lock (stub in NuttX — no-op) */
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGI(TAG, "Power management not supported");
    }
}

LvglDisplay::~LvglDisplay()
{
    if (notification_timer_ != nullptr)
    {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }

    if (network_label_) lv_obj_del(network_label_);
    if (notification_label_) lv_obj_del(notification_label_);
    if (status_label_) lv_obj_del(status_label_);
    if (mute_label_) lv_obj_del(mute_label_);
    if (battery_label_) lv_obj_del(battery_label_);
    if (low_battery_popup_) lv_obj_del(low_battery_popup_);
    if (emotion_label_) lv_obj_del(emotion_label_);
    if (chat_container_) lv_obj_del(chat_container_);

    if (pm_lock_ != nullptr)
    {
        esp_pm_lock_delete(pm_lock_);
    }
}

void LvglDisplay::SetupStatusBar(lv_obj_t *parent)
{
    DisplayLockGuard lock(this);

    /* Status bar container at top of screen */
    lv_obj_t *status_bar = lv_obj_create(parent);
    lv_obj_set_size(status_bar, width_, 32);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 2, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Battery icon (right) */
    battery_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(battery_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_white(), 0);
    lv_label_set_text(battery_label_, FONT_AWESOME_BATTERY_FULL);
    lv_obj_align(battery_label_, LV_ALIGN_RIGHT_MID, -4, 0);

    /* Network icon */
    network_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(network_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(network_label_, lv_color_white(), 0);
    lv_label_set_text(network_label_, "");
    lv_obj_align_to(network_label_, battery_label_, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    /* Mute icon */
    mute_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(mute_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(mute_label_, lv_color_white(), 0);
    lv_label_set_text(mute_label_, "");
    lv_obj_align_to(mute_label_, network_label_, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    /* Status text (center) */
    status_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
    lv_label_set_text(status_label_, "");
    lv_obj_align(status_label_, LV_ALIGN_LEFT_MID, 4, 0);

    /* Notification text (hidden by default, replaces status when shown) */
    notification_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_color(notification_label_, lv_color_make(255, 200, 0), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    /* Low battery popup (hidden by default) */
    low_battery_popup_ = lv_obj_create(parent);
    lv_obj_set_size(low_battery_popup_, 200, 60);
    lv_obj_align(low_battery_popup_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_make(40, 0, 0), 0);
    lv_obj_set_style_border_color(low_battery_popup_, lv_color_make(255, 80, 80), 0);
    lv_obj_set_style_border_width(low_battery_popup_, 2, 0);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_make(255, 80, 80), 0);
    lv_label_set_text(low_battery_label_, "Low Battery!");
    lv_obj_center(low_battery_label_);

    /* Emotion label (center of screen) */
    emotion_label_ = lv_label_create(parent);
    lv_obj_set_style_text_font(emotion_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(emotion_label_, lv_color_white(), 0);
    lv_label_set_text(emotion_label_, "");
    lv_obj_align(emotion_label_, LV_ALIGN_CENTER, 0, -40);

    /* Chat message container */
    chat_container_ = lv_obj_create(parent);
    lv_obj_set_size(chat_container_, width_ - 20, height_ - 80);
    lv_obj_align(chat_container_, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(chat_container_, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_border_width(chat_container_, 0, 0);
    lv_obj_set_style_pad_all(chat_container_, 8, 0);
    lv_obj_clear_flag(chat_container_, LV_OBJ_FLAG_SCROLLABLE);

    chat_label_ = lv_label_create(chat_container_);
    lv_obj_set_style_text_color(chat_label_, lv_color_white(), 0);
    lv_obj_set_width(chat_label_, width_ - 40);
    lv_label_set_long_mode(chat_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(chat_label_, "");
    lv_obj_align(chat_label_, LV_ALIGN_TOP_LEFT, 0, 0);
}

void LvglDisplay::SetStatus(const char *status)
{
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr)
    {
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (notification_label_)
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvglDisplay::ShowNotification(const std::string &notification, int duration_ms)
{
    ShowNotification(notification.c_str(), duration_ms);
}

void LvglDisplay::ShowNotification(const char *notification, int duration_ms)
{
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr)
    {
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    if (status_label_)
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    if (notification_timer_)
    {
        esp_timer_stop(notification_timer_);
        esp_timer_start_once(notification_timer_, (int64_t)duration_ms * 1000);
    }
}

void LvglDisplay::SetEmotion(const char *emotion)
{
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr)
    {
        return;
    }

    /* Map emotion name to emoji character */
    if (strcmp(emotion, "neutral") == 0)
        lv_label_set_text(emotion_label_, LV_SYMBOL_OK);
    else if (strcmp(emotion, "happy") == 0)
        lv_label_set_text(emotion_label_, LV_SYMBOL_OK);
    else if (strcmp(emotion, "sad") == 0)
        lv_label_set_text(emotion_label_, LV_SYMBOL_OK);
    else if (strcmp(emotion, "sleepy") == 0)
        lv_label_set_text(emotion_label_, LV_SYMBOL_OK);
    else if (strcmp(emotion, "listening") == 0)
        lv_label_set_text(emotion_label_, LV_SYMBOL_AUDIO);
    else if (strcmp(emotion, "speaking") == 0)
        lv_label_set_text(emotion_label_, LV_SYMBOL_PLAY);
    else
        lv_label_set_text(emotion_label_, "");
}

void LvglDisplay::SetChatMessage(const char *role, const char *content)
{
    DisplayLockGuard lock(this);
    if (display_ == nullptr)
    {
        return;
    }

    /* Lazily create the chat overlay on the active screen.  The status bar
     * (which used to create chat_label_) is not wired into the openvela
     * boot flow, so the first inbound chat message materialises the label.
     * Guarded by DisplayLockGuard so the LVGL render thread cannot race. */
    if (chat_label_ == nullptr)
    {
        lv_obj_t *scr = lv_screen_active();
        chat_container_ = lv_obj_create(scr);
        lv_obj_set_size(chat_container_, width_ - 20, 120);
        lv_obj_align(chat_container_, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_set_style_bg_color(chat_container_, lv_color_make(20, 20, 20), 0);
        lv_obj_set_style_border_width(chat_container_, 0, 0);
        lv_obj_set_style_pad_all(chat_container_, 8, 0);
        lv_obj_clear_flag(chat_container_, LV_OBJ_FLAG_SCROLLABLE);

        chat_label_ = lv_label_create(chat_container_);
        lv_obj_set_style_text_color(chat_label_, lv_color_white(), 0);
        lv_obj_set_width(chat_label_, width_ - 40);
        lv_label_set_long_mode(chat_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(chat_label_, "");
        lv_obj_align(chat_label_, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    if (role == nullptr || strlen(role) == 0)
    {
        lv_label_set_text(chat_label_, content ? content : "");
        return;
    }

    /* Format: "role: content" */
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s: %s", role, content ? content : "");
    lv_label_set_text(chat_label_, buf);
}

void LvglDisplay::SetTheme(Theme *theme)
{
    current_theme_ = theme;
    /* Apply theme colors to LVGL objects.
     *
     * NOTE: Metalio is built with -fno-rtti to keep firmware size down,
     * so C++ dynamic_cast<> is unavailable.  Metalio code only ever
     * assigns LvglTheme instances to Theme* pointers (there is no
     * multiple inheritance or virtual bases in this tiny inheritance
     * graph), therefore a checked static_cast is equivalent. */
    LvglTheme *lvgl_theme = (theme != nullptr)
                                ? static_cast<LvglTheme *>(theme)
                                : nullptr;
    if (lvgl_theme && display_)
    {
        DisplayLockGuard lock(this);
        /* Update screen background */
        lv_obj_t *scr = lv_display_get_screen_active(display_);
        if (scr)
        {
            lv_obj_set_style_bg_color(scr, lvgl_theme->background_color(), 0);
        }
    }
}

void LvglDisplay::UpdateStatusBar(bool update_all)
{
    /* Update time display */
    auto now = std::chrono::system_clock::now();
    if (last_status_update_time_ + std::chrono::seconds(10) < now)
    {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        if (tm->tm_year >= 2025 - 1900)
        {
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M", tm);
            SetStatus(time_str);
        }
    }

    /* Update battery icon via board_shim */
    int battery_level = 100;
    int charging = 0;
    int discharging = 0;
    const char *icon = nullptr;

    if (metalio_board_get_battery(&battery_level, &charging, &discharging))
    {
        if (charging)
        {
            icon = FONT_AWESOME_BATTERY_BOLT;
        }
        else
        {
            const char *levels[] = {
                FONT_AWESOME_BATTERY_EMPTY,
                FONT_AWESOME_BATTERY_QUARTER,
                FONT_AWESOME_BATTERY_HALF,
                FONT_AWESOME_BATTERY_THREE_QUARTERS,
                FONT_AWESOME_BATTERY_FULL,
                FONT_AWESOME_BATTERY_FULL,
            };
            int idx = battery_level / 20;
            if (idx > 5) idx = 5;
            if (idx < 0) idx = 0;
            icon = levels[idx];
        }

        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon)
        {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }

        /* Low battery popup */
        if (low_battery_popup_ != nullptr)
        {
            if (battery_level < 20 && discharging)
            {
                if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN))
                {
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
            else
            {
                if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN))
                {
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    /* Update network icon */
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0)
    {
        const char *net_icon = metalio_board_get_network_icon();
        if (network_label_ != nullptr && net_icon != nullptr && network_icon_ != net_icon)
        {
            DisplayLockGuard lock(this);
            network_icon_ = net_icon;
            lv_label_set_text(network_label_, network_icon_);
        }
    }
}

void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image)
{
    (void)image;
    /* TODO: implement preview image display */
}

void LvglDisplay::SetPowerSaveMode(bool on)
{
    if (on)
    {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    }
    else
    {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

bool LvglDisplay::SnapshotToJpeg(std::string &jpeg_data, int quality)
{
    (void)jpeg_data;
    (void)quality;
    /* TODO: implement snapshot when LV_USE_SNAPSHOT is enabled */
    ESP_LOGW(TAG, "SnapshotToJpeg not yet implemented in NuttX port");
    return false;
}
