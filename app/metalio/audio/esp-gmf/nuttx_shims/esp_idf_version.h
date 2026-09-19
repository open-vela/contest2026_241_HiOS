/*
 * esp_idf_version.h shim — ESP-IDF version macros.
 * gmf_core uses ESP_IDF_VERSION and ESP_IDF_VERSION_VAL for version-gated
 * code paths (e.g., pxTaskGetStackStart vs xTaskGetStackStart). We set a
 * version that selects the newer code paths where possible.
 */
#ifndef __ESP_IDF_VERSION_SHIM_H
#define __ESP_IDF_VERSION_SHIM_H

#define ESP_IDF_VERSION_VAL(major, minor, patch) \
    ((major) << 16 | (minor) << 8 | (patch))

/* Claim ESP-IDF 5.4.0.
 *
 * Why >= 5.3.0?
 *   miniz_inflate.h branches on ESP_IDF_VERSION:
 *     IDF v4.x       -> #include "esp32/rom/miniz.h"
 *     4.0.0..<5.3.0  -> #include "rom/miniz.h"
 *     >= 5.3.0       -> #include "miniz.h"
 *   For openvela / esp-hal-3rdparty (ESP32-P4), tinfl_* is provided by
 *   esp_rom/include/miniz.h which we can reach via
 *   arch/risc-v/src/esp32p4/esp-hal-3rdparty/components/esp_rom/include/
 *   (symlinked at arch/risc-v/src/chip/...).
 *
 *   Since the third branch (>= 5.3.0) does a plain `#include "miniz.h"` we
 *   additionally provide a forwarding shim under nuttx_shims/miniz.h that
 *   points at the ESP-ROM header.
 */
#define ESP_IDF_VERSION_MAJOR 5
#define ESP_IDF_VERSION_MINOR 4
#define ESP_IDF_VERSION_PATCH 0
#define ESP_IDF_VERSION       ESP_IDF_VERSION_VAL(5, 4, 0)

#endif /* __ESP_IDF_VERSION_SHIM_H */
