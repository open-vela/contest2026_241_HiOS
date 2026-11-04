/* Generated from the bundled PuHui CBin asset by scripts/generate_puhui_font.py.
 * Explicit metadata avoids copying serialized pointers into LVGL structures. */
#include <lvgl.h>
extern const uint8_t metalio_font_puhui_20_bin[];
_Static_assert(sizeof(lv_font_fmt_txt_glyph_dsc_t) == 16, "PuHui requires LV_FONT_FMT_TXT_LARGE");
static const lv_font_fmt_txt_cmap_t cmaps[] = {
    {0, 14, 1, (const uint16_t *)(metalio_font_puhui_20_bin + 1228076), NULL, 3, 3},
    {32, 95, 4, NULL, NULL, 0, 2},
    {160, 104, 99, NULL, NULL, 0, 2},
    {265, 640, 203, (const uint16_t *)(metalio_font_puhui_20_bin + 1228084), NULL, 102, 3},
    {912, 32, 305, NULL, (const void *)(metalio_font_puhui_20_bin + 1228288), 32, 0},
    {945, 26, 333, NULL, NULL, 0, 2},
    {972, 63, 359, (const uint16_t *)(metalio_font_puhui_20_bin + 1228320), NULL, 8, 3},
    {1040, 64, 367, NULL, NULL, 0, 2},
    {1105, 12, 431, NULL, NULL, 0, 2},
    {1118, 2478, 443, (const uint16_t *)(metalio_font_puhui_20_bin + 1228336), NULL, 22, 3},
    {3597, 25, 465, NULL, NULL, 0, 2},
    {3623, 47, 490, NULL, (const void *)(metalio_font_puhui_20_bin + 1228380), 47, 0},
    {7841, 4520, 528, (const uint16_t *)(metalio_font_puhui_20_bin + 1228428), NULL, 202, 3},
    {12362, 24, 730, NULL, NULL, 0, 2},
    {12387, 105, 754, NULL, (const void *)(metalio_font_puhui_20_bin + 1228832), 105, 0},
    {12493, 33, 834, NULL, NULL, 0, 2},
    {12527, 7445, 867, (const uint16_t *)(metalio_font_puhui_20_bin + 1228940), NULL, 11, 3},
    {19975, 91, 878, NULL, (const void *)(metalio_font_puhui_20_bin + 1228964), 91, 0},
    {20070, 1374, 941, (const uint16_t *)(metalio_font_puhui_20_bin + 1229056), NULL, 502, 3},
    {21448, 124, 1443, NULL, (const void *)(metalio_font_puhui_20_bin + 1230060), 124, 0},
    {21574, 14165, 1526, (const uint16_t *)(metalio_font_puhui_20_bin + 1230184), NULL, 3556, 3},
    {35745, 136, 5082, NULL, (const void *)(metalio_font_puhui_20_bin + 1237296), 136, 0},
    {35884, 244, 5168, (const uint16_t *)(metalio_font_puhui_20_bin + 1237432), NULL, 61, 3},
    {36129, 17, 5229, NULL, NULL, 0, 2},
    {36148, 29139, 5246, (const uint16_t *)(metalio_font_puhui_20_bin + 1237556), NULL, 1361, 3},
    {65288, 24, 6607, NULL, NULL, 0, 2},
    {65313, 221, 6631, (const uint16_t *)(metalio_font_puhui_20_bin + 1240280), NULL, 26, 3}
};
static const lv_font_fmt_txt_kern_classes_t kern = {
    (const int8_t *)(metalio_font_puhui_20_bin + 76),
    metalio_font_puhui_20_bin + 19760, metalio_font_puhui_20_bin + 26420, 148, 133
};
static const lv_font_fmt_txt_dsc_t dsc = {
    .glyph_bitmap = metalio_font_puhui_20_bin + 33080,
    .glyph_dsc = (const lv_font_fmt_txt_glyph_dsc_t *)(metalio_font_puhui_20_bin + 1121008),
    .cmaps = cmaps, .kern_dsc = &kern, .kern_scale = 16, .cmap_num = 27,
    .bpp = 4, .kern_classes = 1, .bitmap_format = 0, .stride = 0
};
const lv_font_t font_puhui_full_20 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 25, .base_line = 6,
    .underline_position = -1, .underline_thickness = 1, .dsc = &dsc
};
