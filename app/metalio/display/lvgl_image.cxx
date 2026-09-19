/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL image wrapper — ported from MetalioClaw4 main/display/lvgl_display/lvgl_image.cc.
 * Replaces heap_caps_free with free. cbin_font support stubbed out.
 */

#include "lvgl_image.h"
#include "esp_log_shim.h"

#include <cstring>
#include <cstdlib>
#include <stdexcept>

#define TAG "LvglImage"

LvglRawImage::LvglRawImage(void *data, size_t size)
{
    memset(&image_dsc_, 0, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t *>(data);
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    image_dsc_.header.w = 0;
    image_dsc_.header.h = 0;
}

bool LvglRawImage::IsGif() const
{
    auto ptr = (const uint8_t *)image_dsc_.data;
    return ptr && ptr[0] == 'G' && ptr[1] == 'I' && ptr[2] == 'F';
}

LvglCBinImage::LvglCBinImage(void *data)
{
    (void)data;
    /* cbin_img_dsc_create is an ESP-IDF component not available in NuttX. */
    image_dsc_ = nullptr;
    ESP_LOGW(TAG, "CBinImage not supported in NuttX port");
}

LvglCBinImage::~LvglCBinImage()
{
    image_dsc_ = nullptr;
}

LvglAllocatedImage::LvglAllocatedImage(void *data, size_t size)
{
    memset(&image_dsc_, 0, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t *>(data);

    if (lv_image_decoder_get_info(&image_dsc_, &image_dsc_.header) != LV_RESULT_OK)
    {
        ESP_LOGE(TAG, "Failed to get image info, data: %p size: %u", data, (unsigned)size);
        /*
         * NOTE: Original ESP-IDF code throws std::runtime_error here, and
         * the caller catches it to free the buffer.  Metalio on NuttX is
         * built with -fno-exceptions to keep firmware size down (and to
         * avoid .gcc_except_table relocations the toolchain cannot
         * resolve for this SoC).  We therefore handle errors "by hand":
         *
         *   • free the now-useless pixel buffer ourselves,
         *   • set image_dsc_.data = NULL so that any later use via
         *     image_dsc() returns a dsc with a NULL data pointer that
         *     LVGL and the caller can detect as invalid by checking
         *     image_dsc_.data.
         *
         * The ai_image_gen_screen catch-block has been rewritten to
         * test `holder->image_dsc()->data == nullptr` instead.
         */
        free(data);
        image_dsc_.data      = nullptr;
        image_dsc_.data_size = 0;
    }
}

LvglAllocatedImage::LvglAllocatedImage(void *data, size_t size,
                                       int width, int height,
                                       int stride, int color_format)
{
    memset(&image_dsc_, 0, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t *>(data);
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = color_format;
    image_dsc_.header.w = width;
    image_dsc_.header.h = height;
    image_dsc_.header.stride = stride;
}

LvglAllocatedImage::~LvglAllocatedImage()
{
    if (image_dsc_.data)
    {
        free((void *)image_dsc_.data);
        image_dsc_.data = nullptr;
    }
}
