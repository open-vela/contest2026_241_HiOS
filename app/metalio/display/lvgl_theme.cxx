/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL theme — ported from MetalioClaw4 main/display/lvgl_display/lvgl_theme.cc.
 */

#include "lvgl_theme.h"
#include "esp_log_shim.h"

#include <cstdlib>
#include <cstring>

#include <lvgl.h>

#define TAG "LvglTheme"

/* Matches MetalioClaw4 main/display/lcd_display.cc — BUILTIN_TEXT_FONT /
 * BUILTIN_ICON_FONT resolve to font_puhui_basic_20_4 / font_awesome_20_4
 * for the Metalio Claw 4 board. These are provided by stub_fonts.cxx. */
LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

LvglTheme::LvglTheme(const std::string &name) : Theme(name)
{
    /* Initialize colors to black by default */
    background_color_ = lv_color_black();
    text_color_ = lv_color_white();
    chat_background_color_ = lv_color_black();
    user_bubble_color_ = lv_color_make(64, 128, 255);
    assistant_bubble_color_ = lv_color_make(64, 64, 64);
    system_bubble_color_ = lv_color_make(40, 40, 40);
    system_text_color_ = lv_color_make(180, 180, 180);
    border_color_ = lv_color_make(60, 60, 60);
    low_battery_color_ = lv_color_make(255, 80, 80);
}

lv_color_t LvglTheme::ParseColor(const std::string &color)
{
    if (color.size() >= 7 && color[0] == '#')
    {
        /* Convert #112233 to lv_color_t */
        uint8_t r = (uint8_t)strtol(color.substr(1, 2).c_str(), nullptr, 16);
        uint8_t g = (uint8_t)strtol(color.substr(3, 2).c_str(), nullptr, 16);
        uint8_t b = (uint8_t)strtol(color.substr(5, 2).c_str(), nullptr, 16);
        return lv_color_make(r, g, b);
    }
    return lv_color_black();
}

LvglThemeManager::LvglThemeManager()
{
    InitializeDefaultThemes();
}

/* The exact theme colors from MetalioClaw4 LcdDisplay::InitializeLcdThemes().
 * The original registers these as the built-in fallbacks before the assets
 * partition (index.json / skin) overrides them at runtime. */
void LvglThemeManager::InitializeDefaultThemes()
{
    /* Create default dark theme */
    LvglTheme *dark = new LvglTheme("dark");
    dark->set_background_color(lv_color_hex(0x000000));
    dark->set_text_color(lv_color_hex(0xFFFFFF));
    dark->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark->set_system_bubble_color(lv_color_hex(0x000000));
    dark->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark->set_border_color(lv_color_hex(0xFFFFFF));
    dark->set_low_battery_color(lv_color_hex(0xFF0000));

    /* Set default fonts (20 px CJK text, 20/30 px icon fonts) */
    dark->set_text_font(std::make_shared<LvglBuiltInFont>(&font_puhui_basic_20_4));
    dark->set_icon_font(std::make_shared<LvglBuiltInFont>(&font_awesome_20_4));
    dark->set_large_icon_font(std::make_shared<LvglBuiltInFont>(&font_awesome_30_4));

    themes_["dark"] = dark;

    /* Create default light theme */
    LvglTheme *light = new LvglTheme("light");
    light->set_background_color(lv_color_hex(0xFFFFFF));
    light->set_text_color(lv_color_hex(0x000000));
    light->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light->set_user_bubble_color(lv_color_hex(0x00FF00));
    light->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light->set_system_text_color(lv_color_hex(0x000000));
    light->set_border_color(lv_color_hex(0x000000));
    light->set_low_battery_color(lv_color_hex(0x000000));

    light->set_text_font(std::make_shared<LvglBuiltInFont>(&font_puhui_basic_20_4));
    light->set_icon_font(std::make_shared<LvglBuiltInFont>(&font_awesome_20_4));
    light->set_large_icon_font(std::make_shared<LvglBuiltInFont>(&font_awesome_30_4));

    themes_["light"] = light;

    ESP_LOGI(TAG, "Initialized %zu themes", themes_.size());
}

LvglTheme *LvglThemeManager::GetTheme(const std::string &theme_name)
{
    auto it = themes_.find(theme_name);
    if (it != themes_.end())
    {
        return it->second;
    }
    return nullptr;
}

void LvglThemeManager::RegisterTheme(const std::string &theme_name, LvglTheme *theme)
{
    themes_[theme_name] = theme;
}
