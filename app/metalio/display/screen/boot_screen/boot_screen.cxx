/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * BootScreen — ported from MetalioClaw4
 * main/display/screen/boot_screen/boot_screen.cc.
 *
 * The original loads an EAF vector animation (ic_boot_animation.eaf) via
 * lv_eaf_create(). That widget belongs to the ESP-IDF EAF component and
 * is not present on NuttX/openvela, so we render a static boot layout:
 *   - "HiOS" Tetris-style pixel (Press Start 2P) logo
 *   - "Powered By openvela" small system text
 *   - "Thanks MetalioClaw4" small system text
 *   - a white 5-second progress bar
 */

#include "boot_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"

#include <stdlib.h>
#include <unistd.h>

LV_FONT_DECLARE(lv_font_press_start_2p_120);
LV_FONT_DECLARE(font_puhui_20_4);

namespace {
constexpr const char *TAG = "BootScreen";
}  // namespace

lv_obj_t *BootScreen::CreateStatic()
{
    lv_obj_t *screen = lv_obj_create(NULL);
    write(1, "BS0\n", 4);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    write(1, "BS0a\n", 5);
    {
        void *p = malloc(64);
        if (p != NULL)
          {
            write(1, "BSP_OK\n", 7);
            free(p);
          }
        else
          {
            write(1, "BSP_NULL\n", 9);
          }
    }
    write(1, "BS0a2\n", 6);

    /* "HiOS" Tetris-style pixel (Press Start 2P) logo — large, centred,
     * each letter in a Tetris tetromino colour with a blocky drop shadow. */
    lv_obj_t *logo = lv_label_create(screen);
    write(1, "BS0b\n", 5);
    lv_label_set_recolor(logo, true);
    lv_label_set_text(logo, "#00E5FF H##2979FF i##FFEA00 O##00E676 S#");
    write(1, "BS0c\n", 5);
    lv_obj_set_style_text_color(logo, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    write(1, "BS0d\n", 5);
    lv_obj_set_style_text_font(logo, &lv_font_press_start_2p_120, LV_PART_MAIN);
    lv_obj_set_style_drop_shadow_color(logo, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_drop_shadow_opa(logo, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_drop_shadow_offset_x(logo, 7, LV_PART_MAIN);
    lv_obj_set_style_drop_shadow_offset_y(logo, 7, LV_PART_MAIN);
    lv_obj_set_style_drop_shadow_radius(logo, 0, LV_PART_MAIN);
    write(1, "BS0e\n", 5);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -80);
    write(1, "BS1\n", 4);

    /* "Powered By openvela" small system text. */
    lv_obj_t *powered = lv_label_create(screen);
    lv_label_set_text(powered, "Powered By openvela");
    lv_obj_set_style_text_color(powered, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(powered, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(powered, LV_ALIGN_CENTER, 0, 70);
    write(1, "BS2\n", 4);

    /* "Thanks MetalioClaw4" small system text near the bottom. */
    lv_obj_t *thanks = lv_label_create(screen);
    lv_label_set_text(thanks, "Thanks MetalioClaw4");
    lv_obj_set_style_text_color(thanks, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(thanks, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(thanks, LV_ALIGN_BOTTOM_MID, 0, -70);
    write(1, "BS3\n", 4);

    /* White 5-second progress bar at the very bottom. */
    lv_obj_t *bar = lv_bar_create(screen);
    lv_obj_set_size(bar, 400, 10);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(bar, 5000, LV_PART_MAIN);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -30);
    write(1, "BS4\n", 4);
    lv_bar_set_value(bar, 100, LV_ANIM_ON);
    write(1, "BS5\n", 4);

    (void)TAG;

    return screen;
}

lv_obj_t *BootScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}
