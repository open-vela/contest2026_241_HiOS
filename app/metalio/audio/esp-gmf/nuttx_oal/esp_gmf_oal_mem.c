/*
 * SPDX-FileCopyrightText: 2026 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX OAL memory implementation — replaces esp_gmf_oal_mem.c.
 *
 * Maps the ESP-GMF OAL memory API to NuttX standard libc:
 *   heap_caps_malloc(size, MALLOC_CAP_SPIRAM)  →  malloc(size)
 *   heap_caps_aligned_alloc(align, size, caps) →  posix_memalign(&ptr, align, size)
 *   heap_caps_calloc_prefer(n, size, ...)      →  calloc(n, size)
 *   heap_caps_realloc(ptr, size, caps)         →  realloc(ptr, size)
 *
 * NuttX uses a unified heap (kumm_malloc). When CONFIG_ESPRESSIF_SPIRAM is
 * enabled, PSRAM is mapped into the unified heap, so all allocations may
 * use PSRAM transparently. The MALLOC_CAP_* flags are accepted but ignored.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_gmf_oal_mem.h"

// #define ENABLE_AUDIO_MEM_TRACE

#ifdef ENABLE_AUDIO_MEM_TRACE
int __attribute__((weak)) media_lib_add_trace_mem(const char *module, void *addr, int size, uint8_t flag)
{
    return 0;
}

void __attribute__((weak)) media_lib_remove_trace_mem(void *addr)
{
}
#endif  /* ENABLE_AUDIO_MEM_TRACE */

void *esp_gmf_oal_malloc(size_t size)
{
    void *data = malloc(size);
#ifdef ENABLE_AUDIO_MEM_TRACE
    if (data) media_lib_add_trace_mem(NULL, data, size, 0);
#endif
    return data;
}

void *esp_gmf_oal_malloc_align(uint8_t align, size_t size)
{
    void *data = NULL;
    if (align == 0) {
        data = malloc(size);
    } else {
        if (posix_memalign(&data, align, size) != 0) {
            data = NULL;
        }
    }
#ifdef ENABLE_AUDIO_MEM_TRACE
    if (data) media_lib_add_trace_mem(NULL, data, size, 0);
#endif
    return data;
}

void esp_gmf_oal_free(void *ptr)
{
#ifdef ENABLE_AUDIO_MEM_TRACE
    if (ptr) media_lib_remove_trace_mem(ptr);
#endif
    free(ptr);
}

void *esp_gmf_oal_calloc(size_t nmemb, size_t size)
{
    void *data = calloc(nmemb, size);
#ifdef ENABLE_AUDIO_MEM_TRACE
    if (data) media_lib_add_trace_mem(NULL, data, nmemb * size, 0);
#endif
    return data;
}

void *esp_gmf_oal_realloc(void *ptr, size_t size)
{
#ifdef ENABLE_AUDIO_MEM_TRACE
    if (ptr) media_lib_remove_trace_mem(ptr);
#endif
    void *p = realloc(ptr, size);
#ifdef ENABLE_AUDIO_MEM_TRACE
    if (p) media_lib_add_trace_mem(NULL, p, size, 0);
#endif
    return p;
}

char *esp_gmf_oal_strdup(const char *str)
{
    if (str == NULL) {
        return NULL;
    }
    size_t len = strlen(str) + 1;
    char *copy = (char *)malloc(len);
    if (copy) {
        memcpy(copy, str, len);
#ifdef ENABLE_AUDIO_MEM_TRACE
        media_lib_add_trace_mem(NULL, copy, len, 0);
#endif
    }
    return copy;
}

void *esp_gmf_oal_calloc_inner(size_t n, size_t size)
{
    /* On NuttX, "inner" (internal RAM) is the same as the default heap. */
    return calloc(n, size);
}

void esp_gmf_oal_mem_print(const char *tag, int line, const char *func)
{
    ESP_LOGI(tag, "Func:%s, Line:%d (NuttX unified heap)", func, line);
}

bool esp_gmf_oal_mem_spiram_is_enabled(void)
{
#ifdef CONFIG_ESPRESSIF_SPIRAM
    return true;
#else
    return false;
#endif
}

bool esp_gmf_oal_mem_spiram_stack_is_enabled(void)
{
    /* Stack on PSRAM is not supported on NuttX. */
    return false;
}

uint8_t esp_gmf_oal_get_spiram_cache_align(void)
{
    /* ESP32-P4 L1 cache line is 64 bytes. */
    return 64;
}
