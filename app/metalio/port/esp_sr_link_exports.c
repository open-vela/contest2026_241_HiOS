/*
 * Linker-visible FreeRTOS / DSP symbols for esp-sr prebuilt archives.
 *
 * ESP32-P4 ROM memset/memcpy/strcmp hard-lock on SPIRAM operands.
 * Do NOT replace memset with pure software for all callers — that
 * previously wedged boot/Home (solid blue). Instead:
 *   - memcpy/memmove/strcmp → always IRAM software
 *   - memset → SPIRAM software, DRAM still calls ROM
 *   - dl_nn_bzero → wrap (prebuilt jumps to ROM memset by absolute addr)
 */

#define ESP_SR_LINK_EXPORTS 1

#include "freertos_shim.h"
#include "esp_err_shim.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IRAM1 __attribute__((section(".iram1")))

/* ESP32-P4 ROM memset — DRAM-only path (SPIRAM hard-locks ROM). */
typedef void *(*rom_memset_fn)(void *s, int c, size_t n);
#define ROM_MEMSET ((rom_memset_fn)(uintptr_t)0x4fc00268)

bool esp_psram_check_ptr_addr(const void *p);

/* Local clear — safe on SPIRAM and DRAM. */
IRAM1 static void *wn_memset(void *s, int c, size_t n)
{
    uint8_t *p = (uint8_t *)s;
    uint8_t v = (uint8_t)c;
    while (n && ((uintptr_t)p & 3u))
    {
        *p++ = v;
        n--;
    }
    uint32_t vv = (uint32_t)v * 0x01010101u;
    while (n >= 16)
    {
        ((uint32_t *)p)[0] = vv;
        ((uint32_t *)p)[1] = vv;
        ((uint32_t *)p)[2] = vv;
        ((uint32_t *)p)[3] = vv;
        p += 16;
        n -= 16;
    }
    while (n >= 4)
    {
        *(uint32_t *)p = vv;
        p += 4;
        n -= 4;
    }
    while (n--)
        *p++ = v;
    return s;
}

/* Linked as memset: SPIRAM → software; DRAM → ROM (keeps Home/LVGL). */
IRAM1 void *metalio_wn_memset(void *s, int c, size_t n)
{
    if (s == NULL || n == 0)
        return s;
    if (esp_psram_check_ptr_addr(s))
        return wn_memset(s, c, n);
    return ROM_MEMSET(s, c, n);
}

/* IRAM memcpy — replaces ROM via metalio_psram_memcpy.ld. */
IRAM1 void *metalio_wn_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n && (((uintptr_t)d | (uintptr_t)s) & 3u))
    {
        *d++ = *s++;
        n--;
    }
    while (n >= 16)
    {
        ((uint32_t *)d)[0] = ((const uint32_t *)s)[0];
        ((uint32_t *)d)[1] = ((const uint32_t *)s)[1];
        ((uint32_t *)d)[2] = ((const uint32_t *)s)[2];
        ((uint32_t *)d)[3] = ((const uint32_t *)s)[3];
        d += 16;
        s += 16;
        n -= 16;
    }
    while (n >= 4)
    {
        *(uint32_t *)d = *(const uint32_t *)s;
        d += 4;
        s += 4;
        n -= 4;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

IRAM1 void *metalio_wn_memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0)
        return dst;
    if (d < s)
        return metalio_wn_memcpy(dst, src, n);
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dst;
}

static void *wn_calloc(size_t cnt, size_t size)
{
    size_t total = cnt * size;
    if (cnt && total / cnt != size)
        return NULL;
    void *p = malloc(total);
    if (p)
        wn_memset(p, 0, total);
    return p;
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec ts;
    ts.tv_sec = ticks / CLOCKS_PER_SEC;
    ts.tv_nsec = (ticks % CLOCKS_PER_SEC) * (1000000000 / CLOCKS_PER_SEC);
    nanosleep(&ts, NULL);
}

typedef struct shim_queue
{
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t *storage;
    UBaseType_t length;
    UBaseType_t item_size;
    UBaseType_t count;
    UBaseType_t head;
    UBaseType_t tail;
    bool is_mutex;
} shim_queue_t;

static QueueHandle_t shim_queue_create(UBaseType_t length, UBaseType_t item_size,
                                       bool is_mutex)
{
    if (length == 0)
        return NULL;

    shim_queue_t *q = (shim_queue_t *)wn_calloc(1, sizeof(*q));
    if (!q)
        return NULL;

    q->length = length;
    q->item_size = item_size;
    q->is_mutex = is_mutex;
    if (item_size > 0)
    {
        q->storage = (uint8_t *)wn_calloc(length, item_size);
        if (!q->storage)
        {
            free(q);
            return NULL;
        }
    }

    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);

    if (is_mutex)
        q->count = 1;

    return (QueueHandle_t)q;
}

QueueHandle_t xQueueCreateMutex(const uint8_t ucQueueType)
{
    (void)ucQueueType;
    return shim_queue_create(1, 0, true);
}

QueueHandle_t xQueueGenericCreate(const UBaseType_t uxQueueLength,
                                  const UBaseType_t uxItemSize,
                                  const uint8_t ucQueueType)
{
    (void)ucQueueType;
    return shim_queue_create(uxQueueLength, uxItemSize, false);
}

static int shim_queue_wait(shim_queue_t *q, TickType_t ticks, bool wait_empty)
{
    if (ticks == portMAX_DELAY)
    {
        while (wait_empty ? (q->count == 0) : (q->count >= q->length))
            pthread_cond_wait(wait_empty ? &q->not_empty : &q->not_full, &q->mu);
        return 0;
    }

    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += ticks / CLOCKS_PER_SEC;
    abs.tv_nsec += (ticks % CLOCKS_PER_SEC) * (1000000000 / CLOCKS_PER_SEC);
    if (abs.tv_nsec >= 1000000000)
    {
        abs.tv_sec++;
        abs.tv_nsec -= 1000000000;
    }

    while (wait_empty ? (q->count == 0) : (q->count >= q->length))
    {
        if (pthread_cond_timedwait(wait_empty ? &q->not_empty : &q->not_full,
                                   &q->mu, &abs) != 0)
            return -1;
    }
    return 0;
}

BaseType_t xQueueSemaphoreTake(QueueHandle_t xQueue, TickType_t xTicksToWait)
{
    shim_queue_t *q = (shim_queue_t *)xQueue;
    if (!q)
        return pdFAIL;

    pthread_mutex_lock(&q->mu);
    /* libhufzip get_flash_index() Takes and never Gives. */
    if (q->is_mutex && q->count == 0)
    {
        pthread_mutex_unlock(&q->mu);
        return pdPASS;
    }
    if (shim_queue_wait(q, xTicksToWait, true) != 0)
    {
        pthread_mutex_unlock(&q->mu);
        return pdFAIL;
    }
    q->count--;
    if (q->item_size > 0)
        q->head = (q->head + 1) % q->length;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return pdPASS;
}

BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void *pvItemToQueue,
                             TickType_t xTicksToWait, const BaseType_t xCopyPosition)
{
    (void)xCopyPosition;
    shim_queue_t *q = (shim_queue_t *)xQueue;
    if (!q)
        return pdFAIL;

    pthread_mutex_lock(&q->mu);
    if (shim_queue_wait(q, xTicksToWait, false) != 0)
    {
        pthread_mutex_unlock(&q->mu);
        return pdFAIL;
    }
    if (q->item_size > 0 && pvItemToQueue && q->storage)
    {
        memcpy(q->storage + q->tail * q->item_size, pvItemToQueue, q->item_size);
        q->tail = (q->tail + 1) % q->length;
    }
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return pdPASS;
}

void vQueueDelete(QueueHandle_t xQueue)
{
    shim_queue_t *q = (shim_queue_t *)xQueue;
    if (!q)
        return;
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->storage);
    free(q);
}

float log1pf(float x)
{
    return logf(1.0f + x);
}

bool dsp_is_power_of_two(int x)
{
    return (x != 0) && ((x & (x - 1)) == 0);
}

int dsp_power_of_two(int x)
{
    int i;
    for (i = 0; i < 32; i++)
    {
        x = x >> 1;
        if (x == 0)
            return i;
    }
    return 0;
}

esp_err_t dsps_fft2r_fc32_ansi_(float *data, int N, float *w);
esp_err_t dsps_fft2r_fc32_arp4_(float *data, int N, float *w)
{
    return dsps_fft2r_fc32_ansi_(data, N, w);
}

esp_err_t dsps_dotprod_f32_ansi(const float *src1, const float *src2, float *dest,
                                int len);
esp_err_t dsps_dotprod_f32_arp4(const float *src1, const float *src2, float *dest,
                                int len)
{
    return dsps_dotprod_f32_ansi(src1, src2, dest, len);
}

float dsps_sqrtf_f32_ansi(float f)
{
    return sqrtf(f);
}

esp_err_t dl_fft2r_fc32_ansi(float *data, int N, float *table);
esp_err_t dl_fft4r_fc32_ansi(float *data, int N, float *table, int table_size);

esp_err_t dl_fft2r_fc32_arp4_(float *data, int N, float *table)
{
    return dl_fft2r_fc32_ansi(data, N, table);
}

esp_err_t dl_fft4r_fc32_arp4_(float *data, int N, float *table, int table_size)
{
    return dl_fft4r_fc32_ansi(data, N, table, table_size);
}

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (path && (strstr(path, "spiffs") != NULL || strstr(path, "srmodel") != NULL))
        return NULL;
    return __real_fopen(path, mode);
}

/* Prebuilt dl_nn_bzero jumps to ROM memset by absolute address — wrap it. */
void __wrap_dl_nn_bzero(void *ptr, int nbytes)
{
    if (ptr != NULL && nbytes > 0)
        wn_memset(ptr, 0, (size_t)nbytes);
}

void __wrap_bzero(void *ptr, size_t n)
{
    if (ptr != NULL && n > 0)
        wn_memset(ptr, 0, n);
}

/* ROM strcmp @ 4fc00280 hard-locks on SPIRAM strings (srmodels in PSRAM).
 * Linked as strcmp via metalio_psram_memcpy.ld (same pattern as memcpy). */
IRAM1 int metalio_wn_strcmp(const char *a, const char *b)
{
    if (a == b)
        return 0;
    if (a == NULL)
        return -1;
    if (b == NULL)
        return 1;
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ESP32-P4 hand-tuned memcpy uses cache ops that hard-lock on SPIRAM.
 * WakeNet sigmoid_table fallback copies DRAM→PSRAM through this path. */
void *__wrap_dl_nn_memcpy(void *dst, const void *src, size_t n)
{
    return metalio_wn_memcpy(dst, src, n);
}
