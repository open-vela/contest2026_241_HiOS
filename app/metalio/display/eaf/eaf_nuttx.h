#pragma once
#include <stdlib.h>
#include <malloc.h>
#include "esp_err_shim.h"
#define MALLOC_CAP_DEFAULT 0
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#ifndef ESP_ERR_INVALID_CRC
#define ESP_ERR_INVALID_CRC 0x109
#endif
static inline void *heap_caps_aligned_alloc(size_t align, size_t size, int caps)
{
    (void)caps;
    return memalign(align < sizeof(void *) ? sizeof(void *) : align, size);
}
#define ESP_GOTO_ON_FALSE(condition, code, label, tag, ...) do { \
    if (!(condition)) { ret = (code); goto label; } \
} while (0)
