/*
 * esp_memory_utils.h shim — ESP-IDF memory utility functions.
 * gmf_core OAL uses esp_ptr_internal() to check if a pointer is in
 * internal RAM (vs PSRAM). On NuttX with a unified heap, all pointers
 * are "internal" from the application's perspective.
 */
#ifndef __ESP_MEMORY_UTILS_SHIM_H
#define __ESP_MEMORY_UTILS_SHIM_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On NuttX, all heap memory is treated as internal. PSRAM is mapped
 * into the unified heap by CONFIG_ESPRESSIF_SPIRAM_USER_HEAP. */
static inline bool esp_ptr_internal(const void *p)
{
    (void)p;
    return true;
}

static inline bool esp_ptr_in_drom(const void *p)
{
    (void)p;
    return false;
}

static inline bool esp_ptr_in_iram(const void *p)
{
    (void)p;
    return false;
}

static inline bool esp_ptr_executable(const void *p)
{
    (void)p;
    return false;
}

#ifdef __cplusplus
}
#endif

#endif /* __ESP_MEMORY_UTILS_SHIM_H */
