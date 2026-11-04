/* Minimal esp_attr.h for esp-sr / dl_fft on NuttX. */
#ifndef ESP_ATTR_H
#define ESP_ATTR_H

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif
#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR
#endif
#ifndef RTC_IRAM_ATTR
#define RTC_IRAM_ATTR
#endif
#ifndef EXT_RAM_ATTR
#define EXT_RAM_ATTR
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
#ifndef DMA_ATTR
#define DMA_ATTR
#endif
#ifndef WORD_ALIGNED_ATTR
#define WORD_ALIGNED_ATTR __attribute__((aligned(4)))
#endif
#ifndef DRAM_FORCE_ATTR
#define DRAM_FORCE_ATTR
#endif

#endif
