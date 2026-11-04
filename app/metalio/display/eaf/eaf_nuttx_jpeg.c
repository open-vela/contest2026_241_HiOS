/* SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err_shim.h"
#include "src/libs/tjpgd/tjpgd.h"

typedef struct {
    const uint8_t *source;
    size_t size, offset;
    uint8_t *dest;
    bool swap;
} eaf_jpeg_io_t;

static size_t eaf_jpeg_read(JDEC *jd, uint8_t *buffer, size_t size)
{
    eaf_jpeg_io_t *io = jd->device;
    if (size > io->size - io->offset) size = io->size - io->offset;
    if (buffer) memcpy(buffer, io->source + io->offset, size);
    io->offset += size;
    return size;
}

static int eaf_jpeg_write(JDEC *jd, void *pixels, JRECT *rect)
{
    eaf_jpeg_io_t *io = jd->device;
    const uint8_t *p = pixels;
    if (rect->right >= jd->width || rect->bottom >= jd->height) return 0;
    for (unsigned y = rect->top; y <= rect->bottom; ++y) {
        uint8_t *out = io->dest + ((size_t)y * jd->width + rect->left) * 2;
        for (unsigned x = rect->left; x <= rect->right; ++x) {
            /* LVGL's TJpgDec emits BGR888. EAF stores RGB565. */
            uint16_t v = ((uint16_t)(p[2] >> 3) << 11) | ((uint16_t)(p[1] >> 2) << 5) | (p[0] >> 3);
            out[io->swap ? 1 : 0] = v & 255;
            out[io->swap ? 0 : 1] = v >> 8;
            p += 3; out += 2;
        }
    }
    return 1;
}

esp_err_t eaf_nuttx_jpeg_decode(const uint8_t *data, size_t size, uint8_t *out, size_t *out_size, bool swap)
{
    if (!data || !out || !out_size) return ESP_ERR_INVALID_ARG;
    void *work = malloc(4096);
    if (!work) return ESP_ERR_NO_MEM;
    JDEC jd = {0};
    eaf_jpeg_io_t io = {data, size, 0, out, swap};
    JRESULT result = jd_prepare(&jd, eaf_jpeg_read, work, 4096, &io);
    size_t required = (size_t)jd.width * jd.height * 2;
    if (result == JDR_OK && required && required <= *out_size) {
        result = jd_decomp(&jd, eaf_jpeg_write, 0);
        if (result == JDR_OK) *out_size = required;
    } else result = JDR_PAR;
    free(work);
    return result == JDR_OK ? ESP_OK : ESP_FAIL;
}
