/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL "A:" filesystem driver — serves MetalioClaw4 UI icons from the
 * embedded PNG bundle (icons_bundle.c) as read-only in-memory files.
 *
 * The ESP-IDF reference mounts a "resources" SPIFFS partition with the
 * letter 'A' (esp_lv_fs + mmap_assets) and stores the icons pre-split as
 * .spng/.sjpg.  On NuttX we instead embed the original PNG bytes directly
 * and serve them through a minimal lv_fs driver.  LVGL's built-in LODEPNG
 * decoder reads the file through lv_fs_* (the vendored lodepng.c has been
 * adapted to use lv_fs_open/lv_fs_read), so the "A:ic_*.spng" sources used
 * across every screen resolve transparently.
 */

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

extern int metalio_icons_lookup(const char *name, const uint8_t **data,
                                uint32_t *size);

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
} icon_file_t;

static bool icon_fs_ready(lv_fs_drv_t *drv)
{
    (void)drv;
    return true;
}

static void *icon_fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    (void)drv;

    /* Read-only bundle. */
    if (mode & LV_FS_MODE_WR)
        return NULL;

    const uint8_t *data = NULL;
    uint32_t size = 0;
    if (!metalio_icons_lookup(path, &data, &size)) {
        static int misses;
        if (misses < 8) {
            printf("A: MISS %s\n", path);
            fflush(stdout);
            misses++;
        }
        return NULL;
    }

    icon_file_t *f = lv_malloc(sizeof(icon_file_t));
    if (f == NULL)
        return NULL;

    f->data = data;
    f->size = size;
    f->pos  = 0;
    return f;
}

static lv_fs_res_t icon_fs_close(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    lv_free(file_p);
    return LV_FS_RES_OK;
}

static lv_fs_res_t icon_fs_read(lv_fs_drv_t *drv, void *file_p, void *buf,
                                uint32_t btr, uint32_t *br)
{
    (void)drv;
    icon_file_t *f = (icon_file_t *)file_p;

    uint32_t remaining = f->size - f->pos;
    uint32_t n = (btr < remaining) ? btr : remaining;
    if (n > 0)
        memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    if (br != NULL)
        *br = n;

    return LV_FS_RES_OK;
}

static lv_fs_res_t icon_fs_seek(lv_fs_drv_t *drv, void *file_p, uint32_t pos,
                                lv_fs_whence_t whence)
{
    (void)drv;
    icon_file_t *f = (icon_file_t *)file_p;

    switch (whence) {
    case LV_FS_SEEK_SET:
        f->pos = pos;
        break;
    case LV_FS_SEEK_CUR:
        f->pos += pos;
        break;
    case LV_FS_SEEK_END:
        f->pos = f->size + pos;
        break;
    default:
        return LV_FS_RES_INV_PARAM;
    }

    if (f->pos > f->size)
        f->pos = f->size;

    return LV_FS_RES_OK;
}

static lv_fs_res_t icon_fs_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    (void)drv;
    icon_file_t *f = (icon_file_t *)file_p;
    if (pos_p != NULL)
        *pos_p = f->pos;
    return LV_FS_RES_OK;
}

void lvgl_assets_fs_init(void)
{
    static lv_fs_drv_t drv;

    lv_fs_drv_init(&drv);
    drv.letter     = 'A';
    drv.cache_size = 0;
    drv.ready_cb   = icon_fs_ready;
    drv.open_cb    = icon_fs_open;
    drv.close_cb   = icon_fs_close;
    drv.read_cb    = icon_fs_read;
    drv.seek_cb    = icon_fs_seek;
    drv.tell_cb    = icon_fs_tell;

    lv_fs_drv_register(&drv);
}
