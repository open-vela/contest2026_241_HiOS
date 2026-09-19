/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * NuttX crypt-adapter stubs for media_lib_sal.
 *
 * The prebuilt libhls_lib.a references the media_lib_aes_* dispatcher
 * symbols unconditionally (they are pulled in by hls_fetcher.c's
 * AES-128 segment-decryption path).  The radio stations we target are
 * plain HTTP / unencrypted HLS, so AES is never exercised at runtime.
 *
 * We provide the four symbols as explicit "not supported" stubs rather
 * than compiling media_lib_crypt.c + the ESP-IDF mbedtls/esp_aes default
 * adapter, which would drag in a crypto stack we do not ship on NuttX yet.
 * If an encrypted playlist is opened, the fetcher reports a decrypt error
 * instead of silently producing garbage.
 */

#include "media_lib_crypt.h"
#include "esp_err.h"

void media_lib_aes_init(media_lib_aes_handle_t *ctx)
{
    if (ctx != NULL) {
        *ctx = NULL;
    }
}

void media_lib_aes_free(media_lib_aes_handle_t ctx)
{
    (void)ctx;
}

int media_lib_aes_set_key(media_lib_aes_handle_t ctx, uint8_t *key, uint8_t key_bits)
{
    (void)ctx;
    (void)key;
    (void)key_bits;
    return ESP_ERR_NOT_SUPPORTED;
}

int media_lib_aes_crypt_cbc(media_lib_aes_handle_t ctx, bool decrypt_mode,
                            uint8_t iv[16], uint8_t *input, size_t size,
                            uint8_t *output)
{
    (void)ctx;
    (void)decrypt_mode;
    (void)iv;
    (void)input;
    (void)size;
    (void)output;
    return ESP_ERR_NOT_SUPPORTED;
}
