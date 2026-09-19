/*
 * SPDX-FileCopyrightText: 2026 Contributors to gmf_fft
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#else
#include <stdlib.h>
#endif  /* defined(ESP_PLATFORM) */

#include "esp_gmf_fft_heap.h"

void *esp_gmf_fft_calloc_aligned(size_t n, size_t elem, size_t align)
{
    size_t nbytes = n * elem;
    if (n != 0u && nbytes / n != elem) {
        return NULL;
    }
#if defined(ESP_PLATFORM)
    void *p = heap_caps_aligned_alloc(align, nbytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p != NULL) {
        memset(p, 0, nbytes);
    }
    return p;
#else
    /* NuttX (MM_ALIGN=8) mm_memalign corrupts the heap on the 16-byte
     * aligned split path that posix_memalign() exercises, which blue-screened
     * the radio spectrum FFT.  Allocate with plain malloc() and manually
     * align the returned pointer up to `align`, storing the original base
     * pointer immediately before the aligned block so free can recover it.
     * The radix-2 C kernel only touches int16_t data (2-byte alignment), so
     * this is safe even if the SIMD kernels are not selected. */
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    size_t total = nbytes + align + sizeof(void *);
    void *raw = malloc(total);
    if (raw == NULL) {
        return NULL;
    }
    uintptr_t addr = (uintptr_t)raw + sizeof(void *);
    addr = (addr + (align - 1u)) & ~((uintptr_t)(align - 1u));
    void *aligned = (void *)addr;
    ((void **)aligned)[-1] = raw;
    memset(aligned, 0, nbytes);
    return aligned;
#endif  /* defined(ESP_PLATFORM) */
}

void esp_gmf_fft_free_aligned(void *p)
{
    if (p == NULL) {
        return;
    }
#if defined(ESP_PLATFORM)
    heap_caps_free(p);
#else
    free(((void **)p)[-1]);
#endif  /* defined(ESP_PLATFORM) */
}
