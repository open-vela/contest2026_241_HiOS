/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Font definitions for the openvela port.
 *
 * UI text uses SimSun 20/30 for metrics, with font_chat_cjk_30 as
 * fallback for glyphs SimSun lacks (fullwidth punctuation: ，。：；…「」…).
 * Without that fallback, call / settings / network screens show tofu
 * (乱码) on those characters. Chat already uses font_chat_cjk_30 directly.
 */

#include <lvgl.h>

extern const lv_font_t lv_font_simsun_20_cjk;
extern const lv_font_t lv_font_simsun_30_cjk;
extern const lv_font_t font_chat_cjk_30;

/* Mutable so metalio_fonts_init can attach a fallback. LV_FONT_DECLARE
 * still sees them as extern const lv_font_t (compatible at link time). */
lv_font_t font_puhui_20_4;
lv_font_t font_puhui_30_4;
lv_font_t font_puhui_basic_20_4;

/* Defined in display/fonts/*.c */
extern const lv_font_t font_awesome_20_4;
extern const lv_font_t font_awesome_30_4;
extern const lv_font_t font_puhui_number_50_4;
extern const lv_font_t font_puhui_number_120_4;
extern const lv_font_t lv_font_press_start_2p_120;

extern "C" void metalio_fonts_init(void)
{
    font_puhui_20_4 = lv_font_simsun_20_cjk;
    font_puhui_20_4.fallback = &font_chat_cjk_30;

    font_puhui_30_4 = lv_font_simsun_30_cjk;
    font_puhui_30_4.fallback = &font_chat_cjk_30;

    font_puhui_basic_20_4 = font_puhui_20_4;
}
