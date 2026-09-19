/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL theme — ported from MetalioClaw4 main/display/lvgl_display/lvgl_theme.h.
 * Manages colors, fonts, and images for the UI theme.
 */

#pragma once

#include "display.h"
#include "lvgl_image.h"
#include "lvgl_font.h"
#include "emoji_collection.h"

#include <lvgl.h>
#include <memory>
#include <map>
#include <string>

class LvglTheme : public Theme
{
public:
    static lv_color_t ParseColor(const std::string &color);

    LvglTheme(const std::string &name);

    /* Properties */
    inline lv_color_t background_color() const { return background_color_; }
    inline lv_color_t text_color() const { return text_color_; }
    inline lv_color_t chat_background_color() const { return chat_background_color_; }
    inline lv_color_t user_bubble_color() const { return user_bubble_color_; }
    inline lv_color_t assistant_bubble_color() const { return assistant_bubble_color_; }
    inline lv_color_t system_bubble_color() const { return system_bubble_color_; }
    inline lv_color_t system_text_color() const { return system_text_color_; }
    inline lv_color_t border_color() const { return border_color_; }
    inline lv_color_t low_battery_color() const { return low_battery_color_; }
    inline std::shared_ptr<LvglImage> background_image() const { return background_image_; }
    inline std::shared_ptr<EmojiCollection> emoji_collection() const { return emoji_collection_; }
    inline std::shared_ptr<LvglFont> text_font() const { return text_font_; }
    inline std::shared_ptr<LvglFont> icon_font() const { return icon_font_; }
    inline std::shared_ptr<LvglFont> large_icon_font() const { return large_icon_font_; }
    inline int spacing(int scale) const { return spacing_ * scale; }

    inline void set_background_color(lv_color_t c) { background_color_ = c; }
    inline void set_text_color(lv_color_t c) { text_color_ = c; }
    inline void set_chat_background_color(lv_color_t c) { chat_background_color_ = c; }
    inline void set_user_bubble_color(lv_color_t c) { user_bubble_color_ = c; }
    inline void set_assistant_bubble_color(lv_color_t c) { assistant_bubble_color_ = c; }
    inline void set_system_bubble_color(lv_color_t c) { system_bubble_color_ = c; }
    inline void set_system_text_color(lv_color_t c) { system_text_color_ = c; }
    inline void set_border_color(lv_color_t c) { border_color_ = c; }
    inline void set_low_battery_color(lv_color_t c) { low_battery_color_ = c; }
    inline void set_background_image(std::shared_ptr<LvglImage> img) { background_image_ = img; }
    inline void set_emoji_collection(std::shared_ptr<EmojiCollection> ec) { emoji_collection_ = ec; }
    inline void set_text_font(std::shared_ptr<LvglFont> f) { text_font_ = f; }
    inline void set_icon_font(std::shared_ptr<LvglFont> f) { icon_font_ = f; }
    inline void set_large_icon_font(std::shared_ptr<LvglFont> f) { large_icon_font_ = f; }

private:
    int spacing_ = 2;

    lv_color_t background_color_;
    lv_color_t text_color_;
    lv_color_t chat_background_color_;
    lv_color_t user_bubble_color_;
    lv_color_t assistant_bubble_color_;
    lv_color_t system_bubble_color_;
    lv_color_t system_text_color_;
    lv_color_t border_color_;
    lv_color_t low_battery_color_;

    std::shared_ptr<LvglImage> background_image_ = nullptr;
    std::shared_ptr<LvglFont> text_font_ = nullptr;
    std::shared_ptr<LvglFont> icon_font_ = nullptr;
    std::shared_ptr<LvglFont> large_icon_font_ = nullptr;
    std::shared_ptr<EmojiCollection> emoji_collection_ = nullptr;
};

class LvglThemeManager
{
public:
    static LvglThemeManager &GetInstance()
    {
        static LvglThemeManager instance;
        return instance;
    }

    void RegisterTheme(const std::string &theme_name, LvglTheme *theme);
    LvglTheme *GetTheme(const std::string &theme_name);

private:
    LvglThemeManager();
    void InitializeDefaultThemes();

    std::map<std::string, LvglTheme *> themes_;
};
