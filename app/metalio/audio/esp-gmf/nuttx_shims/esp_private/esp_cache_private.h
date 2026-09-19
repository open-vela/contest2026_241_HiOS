/*
 * esp_private/esp_cache_private.h shim — ESP-IDF cache alignment API.
 * gmf_core OAL mem.c uses esp_cache_get_alignment() to determine the
 * PSRAM cache line alignment. On ESP32-P4 with NuttX, the cache line is
 * 64 bytes (L1 cache).
 */
#ifndef __ESP_CACHE_PRIVATE_SHIM_H
#define __ESP_CACHE_PRIVATE_SHIM_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MALLOC_CAP_SPIRAM is defined by esp_heap_caps.h */
#include "esp_heap_caps.h"

static inline int esp_cache_get_alignment(uint32_t flags, size_t *alignment)
{
    (void)flags;
    if (alignment)
        *alignment = 64; /* ESP32-P4 L1 cache line size */
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* __ESP_CACHE_PRIVATE_SHIM_H */
