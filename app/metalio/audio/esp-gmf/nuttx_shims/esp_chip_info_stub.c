/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Prebuilt MP3 decoder in libesp_audio_codec.a references esp_chip_info().
 */
#include "esp_chip_info.h"

#include <string.h>

void esp_chip_info(esp_chip_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->model = 18; /* ESP32-P4 class — enough for decoder feature checks */
    out->cores = 1;
    out->revision = 0;
}
