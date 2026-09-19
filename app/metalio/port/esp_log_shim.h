/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-IDF logging shim for NuttX/openvela.
 * Uses write() + stack-buffer snprintf to avoid syslog heap corruption.
 */

#ifndef __ESP_LOG_SHIM_H
#define __ESP_LOG_SHIM_H

#include <unistd.h>
#include <stdio.h>

/* NuttX defines getpagesize(f) and getdtablesize(f) as macros in unistd.h;
 * these conflict with the toolchain's sys/unistd.h which declares the
 * functions as `int getpagesize(void)` / `int getdtablesize(void)`.
 * Undefine the macros so the toolchain header can declare the functions. */
#ifdef getpagesize
#undef getpagesize
#endif
#ifdef getdtablesize
#undef getdtablesize
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* esp_log_timestamp() — returns milliseconds since boot (ESP-IDF API).
 * Mapped to clock_gettime(CLOCK_MONOTONIC) on NuttX. */
#include <time.h>
static inline uint32_t esp_log_timestamp(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ESP-IDF log levels */
#ifndef ESP_LOG_NONE
#define ESP_LOG_NONE    0
#define ESP_LOG_ERROR   1
#define ESP_LOG_WARN    2
#define ESP_LOG_INFO    3
#define ESP_LOG_DEBUG   4
#define ESP_LOG_VERBOSE 5
#endif

#ifndef ESP_LOG_LEVEL_T_DEFINED
#define ESP_LOG_LEVEL_T_DEFINED
typedef int esp_log_level_t;
#endif

void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...);

/* Default log level — can be overridden by Kconfig */
#ifndef CONFIG_METALIO_LOG_LEVEL
#define CONFIG_METALIO_LOG_LEVEL 3
#endif

/* Internal helper: format log message into stack buffer and write to fd 1.
 * Avoids syslog() which causes heap corruption on repeated calls. */
#define __ESP_LOG_WRITE(level_char, tag, fmt, ...) do { \
    if (1) { \
        char __log_buf[128]; \
        int __log_len = snprintf(__log_buf, sizeof(__log_buf), \
            "%c %s: " fmt "\n", level_char, tag, ##__VA_ARGS__); \
        if (__log_len > 0) { \
            if (__log_len > (int)sizeof(__log_buf)) \
                __log_len = (int)sizeof(__log_buf); \
            write(1, __log_buf, (size_t)__log_len); \
        } \
    } \
} while(0)

/* Tag-prefixed logging macros — use write() instead of syslog() */
#define ESP_LOGE(tag, fmt, ...)  do { if (CONFIG_METALIO_LOG_LEVEL >= 1) \
    __ESP_LOG_WRITE('E', tag, fmt, ##__VA_ARGS__); } while(0)

#define ESP_LOGW(tag, fmt, ...)  do { if (CONFIG_METALIO_LOG_LEVEL >= 2) \
    __ESP_LOG_WRITE('W', tag, fmt, ##__VA_ARGS__); } while(0)

#define ESP_LOGI(tag, fmt, ...)  do { if (CONFIG_METALIO_LOG_LEVEL >= 3) \
    __ESP_LOG_WRITE('I', tag, fmt, ##__VA_ARGS__); } while(0)

#define ESP_LOGD(tag, fmt, ...)  do { if (CONFIG_METALIO_LOG_LEVEL >= 4) \
    __ESP_LOG_WRITE('D', tag, fmt, ##__VA_ARGS__); } while(0)

#define ESP_LOGV(tag, fmt, ...)  do { if (CONFIG_METALIO_LOG_LEVEL >= 5) \
    __ESP_LOG_WRITE('V', tag, fmt, ##__VA_ARGS__); } while(0)

/* Early-boot variants — used by constructor code (e.g. media_lib_adapter.c)
 * before the full log subsystem is up. Map them onto the regular macros. */
#define ESP_EARLY_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGV(tag, fmt, ...) ESP_LOGV(tag, fmt, ##__VA_ARGS__)

/* ESP-IDF hex dump — simplified to line-by-line */
#define ESP_LOG_BUFFER_HEX(tag, buf, len)  do { } while(0)
#define ESP_LOG_BUFFER_HEXDUMP(tag, buf, len, level)  do { } while(0)

/* Log level setter (no-op in NuttX; level is compile-time) */
#define esp_log_level_set(tag, level)  do { } while(0)

#ifdef __cplusplus
}
#endif

#endif /* __ESP_LOG_SHIM_H */
