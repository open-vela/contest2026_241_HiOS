/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-IDF error type shim for NuttX/openvela.
 */

#ifndef __ESP_ERR_SHIM_H
#define __ESP_ERR_SHIM_H

#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-IDF error codes map to NuttX errno */
typedef int esp_err_t;

#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM  -ENOMEM
#define ESP_ERR_INVALID_ARG  -EINVAL
#define ESP_ERR_INVALID_STATE -ENOSYS
#define ESP_ERR_NOT_FOUND    -ENOENT
#define ESP_ERR_NOT_SUPPORTED -ENOSYS
#define ESP_ERR_TIMEOUT      -ETIMEDOUT
#define ESP_ERR_INVALID_SIZE -EINVAL
#define ESP_ERR_INVALID_RESPONSE -EBADMSG
#define ESP_ERR_INVALID_VERSION  -EBADMSG
#define ESP_ERR_INVALID_CRC      -EILSEQ   /* Illegal byte sequence ≡ CRC mismatch */

/* Internal helper: write-formatted error log without syslog */
#define __ESP_ERR_LOG(fmt, ...) do { \
    char __eb[192]; \
    int __el = snprintf(__eb, sizeof(__eb), "E " fmt "\n", ##__VA_ARGS__); \
    if (__el > 0) { \
        if (__el > (int)sizeof(__eb)) __el = (int)sizeof(__eb); \
        write(1, __eb, (size_t)__el); \
    } \
} while(0)

/* ESP_ERROR_CHECK — logs and asserts on failure */
#define ESP_ERROR_CHECK(x) do { \
    esp_err_t __err_rc = (x); \
    if (__err_rc != ESP_OK) { \
        __ESP_ERR_LOG("ESP_ERROR_CHECK: %s:%d err=%d", __FILE__, __LINE__, __err_rc); \
        assert(0); \
    } \
} while(0)

/* ESP_RETURN_ON_ERROR / ESP_GOTO_ON_ERROR macros */
#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...) do { \
    esp_err_t __err_rc = (x); \
    if (__err_rc != ESP_OK) { \
        __ESP_ERR_LOG("%s: " fmt " err=%d", tag, ##__VA_ARGS__, __err_rc); \
        return __err_rc; \
    } \
} while(0)

#define ESP_GOTO_ON_ERROR(x, gototag, tag, fmt, ...) do { \
    esp_err_t __err_rc = (x); \
    if (__err_rc != ESP_OK) { \
        __ESP_ERR_LOG("%s: " fmt " err=%d", tag, ##__VA_ARGS__, __err_rc); \
        goto gototag; \
    } \
} while(0)

#ifdef __cplusplus
}
#endif

#endif /* __ESP_ERR_SHIM_H */
