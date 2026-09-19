/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL font wrapper — ported from MetalioClaw4 main/display/lvgl_display/lvgl_font.h.
 */

#pragma once

#include <lvgl.h>

class LvglFont
{
public:
    virtual const lv_font_t *font() const = 0;
    virtual ~LvglFont() = default;
};

/* Built-in font (e.g. lv_font_montserrat_14) */
class LvglBuiltInFont : public LvglFont
{
public:
    LvglBuiltInFont(const lv_font_t *font) : font_(font) {}
    virtual const lv_font_t *font() const override { return font_; }

private:
    const lv_font_t *font_;
};

/* CBin font loaded from binary data (stub: returns nullptr if no cbin support) */
class LvglCBinFont : public LvglFont
{
public:
    LvglCBinFont(void *data);
    virtual ~LvglCBinFont();
    virtual const lv_font_t *font() const override { return font_; }

private:
    lv_font_t *font_ = nullptr;
};
