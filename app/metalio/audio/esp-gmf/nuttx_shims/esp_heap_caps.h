/*
 * esp_heap_caps.h redirect — extends the project's existing esp_heap_caps.h
 * with the additional functions used by gmf_core's OAL (aligned_alloc,
 * calloc_prefer).
 */
#ifndef __ESP_HEAP_CAPS_GMF_REDIRECT_H
#define __ESP_HEAP_CAPS_GMF_REDIRECT_H

/* Include the project's existing shim (provides heap_caps_malloc/free/etc.) */
#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* heap_caps_aligned_alloc — aligned memory allocation.
 * ESP-IDF signature: void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
 * NuttX provides posix_memalign() which does the same thing. */
static inline void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    (void)caps;
    if (alignment == 0)
        return malloc(size);
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0)
        return NULL;
    return ptr;
}

/* heap_caps_aligned_free — free aligned memory. Same as free() on NuttX. */
static inline void heap_caps_aligned_free(void *ptr)
{
    free(ptr);
}

/* heap_caps_calloc_prefer — ESP-IDF function that tries to allocate from
 * the first caps preference, falling back to the second.
 * Signature: heap_caps_calloc_prefer(size_t n, size_t size, size_t num_caps, uint32_t caps1, uint32_t caps2)
 * On NuttX's unified heap, just use calloc(). */
static inline void *heap_caps_calloc_prefer(size_t n, size_t size, size_t num_caps, ...)
{
    (void)num_caps;
    return calloc(n, size);
}

/* heap_caps_malloc_prefer — same pattern. */
static inline void *heap_caps_malloc_prefer(size_t size, size_t num_caps, ...)
{
    (void)num_caps;
    return malloc(size);
}

#ifdef __cplusplus
}
#endif

#endif /* __ESP_HEAP_CAPS_GMF_REDIRECT_H */
