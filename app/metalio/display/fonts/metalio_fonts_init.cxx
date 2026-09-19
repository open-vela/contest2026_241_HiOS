/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Load xiaozhi CBin PuHui fonts into the global symbols expected by
 * Metalio screens (font_puhui_20_4 / font_puhui_30_4).
 *
 * Previously these symbols aliased LVGL SimSun CJK, which is missing
 * common particles (吗/啊/呢/…), so TTS bubbles looked garbled.
 */

#include "cbin_font.h"
#include "esp_log_shim.h"

#include <lvgl.h>

#include <cstring>

extern "C" {
extern const uint8_t metalio_font_puhui_30_bin[];
extern const uint8_t metalio_font_puhui_30_bin_end[];
extern const uint8_t metalio_font_puhui_20_bin[];
extern const uint8_t metalio_font_puhui_20_bin_end[];

/*
 * Non-const definitions: LV_FONT_DECLARE() exposes them as const
 * (read-only view). We fill these once before UI starts.
 */
lv_font_t font_puhui_30_4;
lv_font_t font_puhui_20_4;
lv_font_t font_puhui_basic_20_4;
}

namespace
{

constexpr const char* TAG = "MetalioFonts";
bool g_ready = false;

bool LoadBin(const uint8_t* bin, lv_font_t* out, const char* name,
             const lv_font_t* fallback)
{
    if (bin == nullptr || out == nullptr)
        return false;

    lv_font_t* created = cbin_font_create(const_cast<uint8_t*>(bin));
    if (created == nullptr)
    {
        ESP_LOGE(TAG, "cbin_font_create failed for %s", name);
        return false;
    }

    *out = *created;
    lv_free(created);
    out->fallback = fallback;
    ESP_LOGE(TAG, "loaded %s line_height=%d", name, (int)out->line_height);
    return true;
}

} // namespace

extern "C" void metalio_fonts_init(void)
{
    if (g_ready)
        return;

    extern const lv_font_t lv_font_simsun_30_cjk;
    extern const lv_font_t lv_font_simsun_20_cjk;

    (void)metalio_font_puhui_30_bin_end;
    (void)metalio_font_puhui_20_bin_end;

    if (!LoadBin(metalio_font_puhui_30_bin, &font_puhui_30_4, "puhui_30",
                 &lv_font_simsun_30_cjk))
        font_puhui_30_4 = lv_font_simsun_30_cjk;

    if (!LoadBin(metalio_font_puhui_20_bin, &font_puhui_20_4, "puhui_20",
                 &lv_font_simsun_20_cjk))
        font_puhui_20_4 = lv_font_simsun_20_cjk;

    font_puhui_basic_20_4 = font_puhui_20_4;
    g_ready = true;
}
