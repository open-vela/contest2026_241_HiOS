/* SPDX-License-Identifier: Apache-2.0
 * Reader for MetalioClaw4 sd_images/jpg_to_sjpg.py's strip-JPEG container. */
#include "lvgl.h"
#include "src/draw/lv_image_decoder_private.h"
#include "esp_err_shim.h"
#include <stdlib.h>
#include <string.h>

extern esp_err_t eaf_nuttx_jpeg_decode(const uint8_t *, size_t, uint8_t *, size_t *, bool);

static unsigned read16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static bool read_exact(lv_fs_file_t *file, void *data, uint32_t size)
{
    uint32_t got = 0;
    return lv_fs_read(file, data, size, &got) == LV_FS_RES_OK && got == size;
}
static bool read_header(lv_fs_file_t *file, uint8_t *h)
{
    if (!read_exact(file, h, 22) || memcmp(h, "_SJPG__", 7)) return false;
    unsigned w = read16(h + 14), height = read16(h + 16),
             blocks = read16(h + 18), rows = read16(h + 20);
    return w && height && w <= 1024 && height <= 1024 && rows &&
           blocks == (height + rows - 1) / rows;
}
static lv_result_t sjpg_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header)
{
    (void)decoder;
    if (dsc->src_type != LV_IMAGE_SRC_FILE || strcmp(lv_fs_get_ext(dsc->src), "sjpg")) return LV_RESULT_INVALID;
    lv_fs_file_t file;
    if (lv_fs_open(&file, dsc->src, LV_FS_MODE_RD) != LV_FS_RES_OK) return LV_RESULT_INVALID;
    uint8_t h[22];
    bool valid = read_header(&file, h);
    lv_fs_close(&file);
    if (!valid) return LV_RESULT_INVALID;
    memset(header, 0, sizeof(*header));
    header->magic = LV_IMAGE_HEADER_MAGIC;
    header->cf = LV_COLOR_FORMAT_RGB565;
    header->w = read16(h + 14);
    header->h = read16(h + 16);
    header->stride = header->w * 2;
    return LV_RESULT_OK;
}
static lv_result_t sjpg_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    (void)decoder;
    lv_fs_file_t file;
    if (lv_fs_open(&file, dsc->src, LV_FS_MODE_RD) != LV_FS_RES_OK) return LV_RESULT_INVALID;
    uint8_t h[22];
    lv_draw_buf_t *pixels = NULL;
    uint8_t *table = NULL, *jpeg = NULL;
    bool valid = false;
    if (!read_header(&file, h)) goto done;
    unsigned w = read16(h + 14), height = read16(h + 16),
             count = read16(h + 18), rows = read16(h + 20);
    table = malloc(count * 2);
    if (!table || !read_exact(&file, table, count * 2)) goto done;
    pixels = lv_draw_buf_create(w, height, LV_COLOR_FORMAT_RGB565, w * 2);
    jpeg = malloc(65535);
    if (!pixels || !jpeg) goto done;
    for (unsigned block = 0; block < count; ++block) {
        unsigned size = read16(table + block * 2);
        unsigned y = block * rows;
        unsigned block_rows = height - y < rows ? height - y : rows;
        size_t out_size = (size_t)block_rows * w * 2;
        size_t expected = out_size;
        if (!size || !read_exact(&file, jpeg, size) ||
            eaf_nuttx_jpeg_decode(jpeg, size, pixels->data + (size_t)y * w * 2,
                                 &out_size, false) != ESP_OK || out_size != expected) goto done;
    }
    valid = true;
done:
    lv_fs_close(&file);
    free(table);
    free(jpeg);
    if (!valid) { if (pixels) lv_draw_buf_destroy(pixels); return LV_RESULT_INVALID; }
    dsc->decoded = pixels;
    if (!dsc->args.no_cache && lv_image_cache_is_enabled()) {
        lv_image_cache_data_t key;
        memset(&key, 0, sizeof(key));
        key.src_type = dsc->src_type;
        key.src = dsc->src;
        key.slot.size = pixels->data_size;
        dsc->cache_entry = lv_image_decoder_add_to_cache(decoder, &key, pixels, NULL);
        if (!dsc->cache_entry) {
            lv_draw_buf_destroy(pixels);
            dsc->decoded = NULL;
            return LV_RESULT_INVALID;
        }
    }
    return LV_RESULT_OK;
}
static void sjpg_close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    (void)decoder;
    if (dsc->decoded && !dsc->cache_entry)
        lv_draw_buf_destroy((lv_draw_buf_t *)dsc->decoded);
}
void lv_sjpg_init(void)
{
    lv_image_decoder_t *decoder = lv_image_decoder_create();
    if (!decoder) return;
    lv_image_decoder_set_info_cb(decoder, sjpg_info);
    lv_image_decoder_set_open_cb(decoder, sjpg_open);
    lv_image_decoder_set_close_cb(decoder, sjpg_close);
}
