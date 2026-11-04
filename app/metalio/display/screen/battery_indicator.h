#ifndef METALIO_BATTERY_INDICATOR_H
#define METALIO_BATTERY_INDICATOR_H
#include <lvgl.h>
#include "board_shim.h"
#include "battery_font.h"
#include <cstdio>
#include <cstring>

/* Shared status battery: native outline/bolt, ASCII digits, no icon font. */
inline lv_obj_t *BatteryIndicatorCreate(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 64, 26);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *body = lv_obj_create(root);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 58, 24);
    lv_obj_set_pos(body, 0, 1);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_white(), 0);
    lv_obj_set_style_radius(body, 6, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *label = lv_label_create(body);
    lv_obj_remove_style_all(label);
    lv_obj_set_size(label, 56, 20);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_label_set_text(label, "--");
    lv_obj_set_style_text_font(label, &battery_font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_center(label);
    static const lv_point_precise_t bolt_points[] = {{7, 0}, {1, 7}, {6, 7}, {0, 14}};
    lv_obj_t *bolt = lv_line_create(body);
    lv_line_set_points(bolt, bolt_points, 4);
    lv_obj_set_style_line_color(bolt, lv_color_black(), 0);
    lv_obj_set_style_line_width(bolt, 2, 0);
    lv_obj_set_pos(bolt, 4, 2);
    lv_obj_add_flag(bolt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *fill = lv_obj_create(body);
    lv_obj_remove_style_all(fill);
    lv_obj_set_pos(fill, 1, 1);
    lv_obj_set_size(fill, 54, 20);
    lv_obj_set_style_radius(fill, 4, 0);
    lv_obj_set_style_bg_color(fill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(fill, 0);
    lv_obj_t *terminal = lv_obj_create(root);
    lv_obj_remove_style_all(terminal);
    lv_obj_set_size(terminal, 3, 8);
    lv_obj_set_pos(terminal, 59, 9);
    lv_obj_set_style_bg_color(terminal, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(terminal, LV_OPA_COVER, 0);
    lv_obj_remove_flag(terminal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(terminal, LV_OBJ_FLAG_CLICKABLE);
    return root;
}
inline void BatteryIndicatorUpdate(lv_obj_t *root)
{
    if (!root) return;
    int level = 0, charging = 0, discharging = 0;
    bool valid = metalio_board_get_battery(&level, &charging, &discharging);
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    auto *body = lv_obj_get_child(root, 0);
    auto *fill = lv_obj_get_child(body, 0);
    auto *label = lv_obj_get_child(body, 1);
    auto *bolt = lv_obj_get_child(body, 2);
    char value[8];
    if (valid) std::snprintf(value, sizeof(value), "%d", level);
    else std::snprintf(value, sizeof(value), "--");
    if (std::strcmp(lv_label_get_text(label), value)) {
        lv_label_set_text(label, value);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    }
    const bool charge = valid && charging;
    int width = valid ? (54 * level + 50) / 100 : 0;
    lv_obj_set_flag(fill, LV_OBJ_FLAG_HIDDEN, width == 0);
    if (width > 0 && lv_obj_get_width(fill) != width) lv_obj_set_width(fill, width);
    const lv_color_t color = lv_color_hex(charge ? 0x34C759 :
                                         (valid && level <= 20 ? 0xFF453A : 0xFFFFFF));
    if (!lv_color_eq(lv_obj_get_style_bg_color(fill, LV_PART_MAIN), color))
        lv_obj_set_style_bg_color(fill, color, 0);
    const bool was_charge = !lv_obj_has_flag(bolt, LV_OBJ_FLAG_HIDDEN);
    if (charge != was_charge) {
        lv_obj_set_flag(bolt, LV_OBJ_FLAG_HIDDEN, !charge);
        lv_obj_center(label);
    }
}
#endif
