/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_heap_caps.h — shim for ESP-IDF heap_caps_* APIs.
 *
 * Implementations live in port/esp_sr_link_exports.c so prebuilt
 * esp-sr archives can resolve the symbols at link time.
 */

#ifndef ESP_HEAP_CAPS_SHIM_H
#define ESP_HEAP_CAPS_SHIM_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_INTERNAL    (1 << 0)
#define MALLOC_CAP_DEFAULT     (1 << 0)
#define MALLOC_CAP_DMA         (1 << 1)
#define MALLOC_CAP_SPIRAM      (1 << 2)
#define MALLOC_CAP_8BIT        (1 << 3)
#define MALLOC_CAP_32BIT       (1 << 4)
#define MALLOC_CAP_EXEC        (1 << 5)
#define MALLOC_CAP_RETENTION   (1 << 6)
#define MALLOC_CAP_RTCRAM      (1 << 7)

void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
void *heap_caps_malloc_default(size_t size);
void *heap_caps_realloc_default(void *ptr, size_t size);
size_t heap_caps_get_free_size(uint32_t caps);
size_t esp_get_free_heap_size(void);
size_t esp_get_minimum_free_heap_size(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_HEAP_CAPS_SHIM_H */
