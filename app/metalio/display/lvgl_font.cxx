/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL font wrapper — ported from MetalioClaw4 main/display/lvgl_display/lvgl_font.cc.
 * CBin font support is stubbed out (ESP-IDF cbin_font component not available in NuttX).
 */

#include "lvgl_font.h"
#include "esp_log_shim.h"

#define TAG "LvglFont"

LvglCBinFont::LvglCBinFont(void *data)
{
    (void)data;
    /* cbin_font is an ESP-IDF component not available in NuttX.
     * Fonts are loaded via LVGL built-in fonts or custom font files. */
    font_ = nullptr;
    ESP_LOGW(TAG, "CBinFont not supported in NuttX port, using nullptr");
}

LvglCBinFont::~LvglCBinFont()
{
    font_ = nullptr;
}
