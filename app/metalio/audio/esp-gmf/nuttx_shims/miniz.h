/*
 * miniz.h forwarding shim — redirects to the ESP-ROM miniz.h bundled in
 * esp-hal-3rdparty.  The ESP-ROM header provides:
 *   tinfl_init / tinfl_decompress / TINFL_LZ_DICT_SIZE
 *   TDEFL_* / tdefl_*
 *   mz_stream, MZ_* enum values, etc.
 *
 * Note: the ESP-ROM header defines MZ_NO_ZLIB_APIS automatically when the
 * ROM-only subset is in use, which triggers the inline mz_inflateInit2 /
 * mz_inflate / mz_inflateEnd / mz_deflate* implementations inside
 * miniz_inflate.h.
 */
#pragma once

#if defined(__has_include)
#  if __has_include("esp32p4/esp-hal-3rdparty/components/esp_rom/include/miniz.h")
#    include "esp32p4/esp-hal-3rdparty/components/esp_rom/include/miniz.h"
#  elif __has_include("chip/esp-hal-3rdparty/components/esp_rom/include/miniz.h")
#    include "chip/esp-hal-3rdparty/components/esp_rom/include/miniz.h"
#  endif
#endif

/*
 * If neither include path worked above (e.g. build-time include dirs haven't
 * exposed the ROM header yet), fall back to the absolute file-system path.
 * This is expected to work because the ESP32-P4 port symlinks src/chip ->
 * src/esp32p4 and esp-hal-3rdparty lives under that tree.
 */
#if !defined(MZ_VERSION) && !defined(_MINIZ_H)
#  include "/home/kaykay/HiOS/nuttx/arch/risc-v/src/esp32p4/esp-hal-3rdparty/components/esp_rom/include/miniz.h"
#endif
