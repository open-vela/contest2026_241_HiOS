/*
 * hal/efuse_hal.h shim — ESP-IDF eFuse HAL.
 * gmf_core OAL mem.c uses efuse_hal_chip_revision() only to check if
 * ESP32 (rev < 3) supports PSRAM stack. On ESP32-P4 this is irrelevant.
 */
#ifndef __HAL_EFUSE_HAL_SHIM_H
#define __HAL_EFUSE_HAL_SHIM_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t efuse_hal_chip_revision(void)
{
    return 3; /* ESP32-P4 — always "new enough" for any PSRAM features */
}

#ifdef __cplusplus
}
#endif

#endif /* __HAL_EFUSE_HAL_SHIM_H */
