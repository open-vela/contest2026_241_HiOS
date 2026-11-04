/*
 * Minimal esp_chip_info stub header for libesp_audio_codec MP3 on NuttX.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t model;
    uint32_t features;
    uint8_t cores;
    uint8_t revision;
} esp_chip_info_t;

void esp_chip_info(esp_chip_info_t *out);

#ifdef __cplusplus
}
#endif
