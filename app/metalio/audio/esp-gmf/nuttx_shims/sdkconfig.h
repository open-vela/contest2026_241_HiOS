/*
 * sdkconfig.h shim — ESP-IDF build configuration.
 * gmf_core uses CONFIG_SPIRAM_BOOT_INIT, CONFIG_FREERTOS_UNICORE, etc.
 * On NuttX, these are provided by the NuttX Kconfig system (defconfig).
 * Define safe defaults here for symbols that ESP-IDF expects.
 */
#ifndef __SDKCONFIG_SHIM_H
#define __SDKCONFIG_SHIM_H

/* SPIRAM — NuttX manages PSRAM via CONFIG_ESPRESSIF_SPIRAM, not the
 * ESP-IDF CONFIG_SPIRAM_BOOT_INIT symbol. Map it so #ifdef checks work. */
#ifdef CONFIG_ESPRESSIF_SPIRAM
#define CONFIG_SPIRAM_BOOT_INIT 1
#else
/* If NuttX SPIRAM is not enabled, don't define it */
#endif

/* FreeRTOS — NuttX is single-core on ESP32-P4 */
#define CONFIG_FREERTOS_UNICORE 1

/* Task list / run-time stats — not available on NuttX. The OAL sys.c
 * has an #else branch that logs a warning when these are undefined. */
/* #undef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID */
/* #undef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS */

/* SPIRAM stack — not supported on NuttX (tasks always use internal stack) */
/* #undef CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY */

/* ---------------------------------------------------------------------------
 * Audio decoder registration (esp_audio_codec).
 *
 * The prebuilt libesp_audio_simple_dec.a / libesp_audio_codec.a ship the
 * actual decoder implementations, but the small *_decoder_reg.c glue under
 * components/esp_audio_codec/src only calls their `*_register()` entrypoints
 * when these Kconfig symbols are defined.  The shim otherwise leaves them
 * undefined, so esp_audio_dec_register_default() /
 * esp_audio_simple_dec_register_default() (called by esp_audio_simple_player)
 * register nothing and esp_audio_simple_dec_open() fails for every type.
 *
 * Enable the container parser + codec the current HLS radio path needs:
 *   TS  (container)  -> esp_ts_dec_register()   (MPEG-TS demux)
 *   AAC (codec)      -> esp_aac_dec_register()  (TS audio payload)
 *
 * NOTE: MP3 is intentionally left off — libesp_audio_codec.a's MP3 decoder
 * references esp_chip_info(), which openvela does not provide.  Add it back
 * together with an esp_chip_info stub if an MP3 radio stream is needed.
 * ---------------------------------------------------------------------------
 */
#define CONFIG_AUDIO_DECODER_AAC_SUPPORT 1
#define CONFIG_AUDIO_SIMPLE_DEC_TS_SUPPORT 1

#endif /* __SDKCONFIG_SHIM_H */
