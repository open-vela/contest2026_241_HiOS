/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * OtaScreen — ported from MetalioClaw4
 * main/display/screen/ota_screen/ota_screen.cc.
 *
 * Fullscreen OTA upgrade progress page: version label, progress bar,
 * downloaded/total bytes, speed, elapsed time, and status message.
 * The original used esp_lv_adapter_lock for thread-safe LVGL access;
 * on NuttX/openvela we use lv_lock()/lv_unlock() instead.
 */

#pragma once

#include "screen_base.h"
#include "lvgl.h"

#include <cstddef>

class OtaScreen : public Screen
{
public:
    // Build a fullscreen OTA progress page (parent == NULL).
    lv_obj_t *Create() override;
    const char *Name() const override { return "ota"; }

    // --- Static API (matches the original MetalioClaw4 signatures) ---
    // Show the OTA screen with an optional version string.
    static void Show(const char *version_text);
    // Update progress (0-100), downloaded bytes, total bytes, speed (B/s).
    static void Update(int progress, size_t downloaded, size_t total, size_t speed_bps);
    // Set a status message (e.g. "升级完成" / "升级失败").
    static void SetStatusMessage(const char *message);
    // Dismiss the OTA screen and return to home.
    static void Dismiss();
    // Check if the OTA screen is currently active.
    static bool IsActive();

private:
    // Instance-based Create builds the same UI but tracks root_ for the
    // Screen base class. The static Show() path is the primary entry point
    // used by the OTA module.
};
