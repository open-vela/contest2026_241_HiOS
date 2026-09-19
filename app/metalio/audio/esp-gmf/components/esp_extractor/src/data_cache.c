/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Proprietary
 *
 * See LICENSE file for details.
 */

#include "reg/data_cache.h"

/*
 * The prebuilt libhls_lib.a links against the esp_extractor data_cache
 * abstraction but does not bundle the data_cache.c translation unit itself
 * (it is built as part of the esp_extractor component in the upstream
 * ESP-IDF build).  This is a self-contained re-implementation of that TU.
 *
 * The HLS fetcher only needs sequential read / seek / get-file-size over
 * the stream-IO callbacks already supplied by hls_io.c; buffering is handled
 * by the GMF data-bus layer above it.  We therefore keep the cache simple:
 * when a cache buffer has not been configured we forward straight through to
 * the registered callbacks, which is functionally identical and correct for
 * this streaming use case.
 */

data_cache_t *data_cache_create(data_cache_cfg_t *cfg)
{
    if (cfg == NULL) {
        return NULL;
    }
    data_cache_t *cache = (data_cache_t *)calloc(1, sizeof(data_cache_t));
    if (cache == NULL) {
        return NULL;
    }
    cache->open = cfg->open;
    cache->read = cfg->read;
    cache->read_abort = cfg->read_abort;
    cache->seek = cfg->seek;
    cache->get_file_size = cfg->file_size;
    cache->close = cfg->close;
    cache->input_ctx = cfg->input_ctx;
    if (cfg->url != NULL) {
        cache->url = strdup(cfg->url);
    }
    cache->cache_buffer_size = cfg->cache_size;
    cache->block_size = cfg->block_size;
    if (cache->cache_buffer_size != 0) {
        cache->cache_buffer = (uint8_t *)malloc(cache->cache_buffer_size);
    }
    return cache;
}

int data_cache_open(data_cache_t *cache)
{
    if (cache == NULL || cache->open == NULL || cache->url == NULL) {
        return -1;
    }
    if (cache->ctx != NULL && cache->close != NULL) {
        cache->close(cache->ctx);
    }
    cache->ctx = cache->open(cache->url, cache->input_ctx);
    if (cache->ctx == NULL) {
        return -1;
    }
    cache->file_size = (cache->get_file_size != NULL)
                           ? cache->get_file_size(cache->ctx)
                           : 0;
    cache->cache_start_pos = 0;
    cache->cached_size = 0;
    cache->read_pos = 0;
    cache->is_eof = false;
    cache->aborted = false;
    return 0;
}

int data_cache_read(data_cache_t *cache, void *buffer, uint32_t size)
{
    if (cache == NULL || cache->read == NULL) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }
    if (buffer == NULL) {
        /* Cache-fill request: only possible when a cache buffer exists. */
        if (cache->cache_buffer != NULL && cache->cache_buffer_size != 0) {
            uint32_t n = size;
            if (n > cache->cache_buffer_size) {
                n = cache->cache_buffer_size;
            }
            return cache->read(cache->cache_buffer, n, cache->ctx);
        }
        return -1;
    }
    return cache->read(buffer, size, cache->ctx);
}

int data_cache_read_directly(data_cache_t *cache, void *buffer, uint32_t size)
{
    if (cache == NULL || cache->read == NULL || buffer == NULL) {
        return -1;
    }
    return cache->read(buffer, size, cache->ctx);
}

int data_cache_seek(data_cache_t *cache, uint32_t pos)
{
    if (cache == NULL || cache->seek == NULL) {
        return -1;
    }
    cache->read_pos = pos;
    cache->cache_start_pos = pos;
    cache->cached_size = 0;
    cache->is_eof = false;
    return cache->seek(pos, cache->ctx);
}

uint32_t data_cache_get_file_size(data_cache_t *cache)
{
    if (cache == NULL) {
        return 0;
    }
    if (cache->file_size == 0 && cache->get_file_size != NULL &&
        cache->ctx != NULL) {
        cache->file_size = cache->get_file_size(cache->ctx);
    }
    return cache->file_size;
}

uint32_t data_cache_get_position(data_cache_t *cache)
{
    return (cache == NULL) ? 0 : cache->read_pos;
}

int data_cache_close(data_cache_t *cache)
{
    if (cache == NULL) {
        return -1;
    }
    if (cache->ctx != NULL && cache->close != NULL) {
        cache->close(cache->ctx);
    }
    cache->ctx = NULL;
    cache->cached_size = 0;
    cache->read_pos = 0;
    cache->is_eof = false;
    return 0;
}

void data_cache_destroy(data_cache_t *cache)
{
    if (cache == NULL) {
        return;
    }
    data_cache_close(cache);
    if (cache->url != NULL) {
        free(cache->url);
    }
    if (cache->cache_buffer != NULL) {
        free(cache->cache_buffer);
    }
    free(cache);
}
