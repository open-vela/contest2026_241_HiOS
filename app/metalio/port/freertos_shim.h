/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * FreeRTOS API shim for NuttX/openvela.
 * Maps xTaskCreate/xEventGroup/vTaskDelete to NuttX task/nxevent APIs.
 */

#ifndef __FREERTOS_SHIM_H
#define __FREERTOS_SHIM_H

#include <nuttx/config.h>
#include <nuttx/sched.h>
#include <nuttx/event.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Task handles */
typedef pid_t TaskHandle_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;

#define pdTRUE    1
#define pdFALSE   0
#define pdPASS    1
#define pdFAIL    0
#define portMAX_DELAY  UINT32_MAX
#define portTICK_PERIOD_MS  (CONFIG_USEC_PER_TICK / 1000)
/* FreeRTOS: ticks = ms / period. (Was wrongly multiplying USEC_PER_TICK.) */
#define pdMS_TO_TICKS(x)    ((TickType_t)(((uint64_t)(x) * 1000u) / CONFIG_USEC_PER_TICK))

/* Task creation.
 *
 * The FreeRTOS `arg` is an opaque void*.  We cannot pass it through NuttX
 * `task_create()`'s argv[] because nxtask_setup_stackargs() treats argv
 * entries as NUL-terminated strings and strlcpy()'s them onto the new
 * task stack, destroying the pointer value.  Use pthread_create() instead:
 * in NuttX pthread_t is pid_t, so TaskHandle_t stays compatible with
 * vTaskDelete()/task_setaffinity().
 */
struct shim_task_wrapper
{
    void (*fn)(void *);
    void *arg;
};

static inline void *shim_task_trampoline(void *p)
{
    write(1, "TR0\n", 4);
    struct shim_task_wrapper *w = (struct shim_task_wrapper *)p;
    void (*fn)(void *) = w->fn;
    void *arg = w->arg;
    free(w);
    write(1, "TR1\n", 4);
    fn(arg);
    write(1, "TR2\n", 4);
    return NULL;
}

static inline BaseType_t xTaskCreate(
    void (*task_func)(void *),
    const char *name,
    uint16_t stack_depth,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *handle)
{
    struct shim_task_wrapper *w =
        (struct shim_task_wrapper *)malloc(sizeof(*w));
    if (w == NULL)
        return pdFAIL;

    (void)name;

    w->fn = task_func;
    w->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)stack_depth * sizeof(void *));

    struct sched_param sp;
    /* FreeRTOS priorities are small (0..N).  Map them above NuttX's default
     * so workers are not starved by SCHED_FIFO LVGL (~DEFAULT+10).
     */
    sp.sched_priority = SCHED_PRIORITY_DEFAULT + (int)priority;
    if (sp.sched_priority < 1)
        sp.sched_priority = 1;
    if (sp.sched_priority > 255)
        sp.sched_priority = 255;
    pthread_attr_setschedpolicy(&attr, SCHED_RR);
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    pthread_t th;
    write(1, "XC0\n", 4);
    int ret = pthread_create(&th, &attr, shim_task_trampoline, w);
    write(1, "XC1\n", 4);
    pthread_attr_destroy(&attr);

    if (ret != 0)
        {
            free(w);
            return pdFAIL;
        }

    if (handle)
        *handle = th;

    return pdTRUE;
}

static inline BaseType_t xTaskCreatePinnedToCore(
    void (*task_func)(void *),
    const char *name,
    uint16_t stack_depth,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *handle,
    int core_id)
{
    BaseType_t ret = xTaskCreate(task_func, name, stack_depth, arg, priority, handle);
    if (ret == pdTRUE && core_id >= 0)
    {
        /* Pin to core if SMP and valid core id */
#ifdef CONFIG_SMP
        if (handle && *handle > 0)
            task_setaffinity(*handle, (cpu_set_t *)(uintptr_t)(1 << core_id));
#endif
    }
    return ret;
}

#ifdef __cplusplus
} /* close extern "C" — keep helper C++-callable from the macro */
#endif
/* ESP-IDF allows vTaskDelete(NULL/nullptr) to delete the calling task.
 * NuttX's task_delete takes a pid_t. Use one impl + macro so nullptr
 * (no conversion to pid_t) and TaskHandle_t both work without overloads
 * (overloads conflict when std::nullptr_t collapses to an integral type). */
static inline void vTaskDeleteImpl(TaskHandle_t handle)
{
    if (handle)
        task_delete(handle);
    else
        task_delete(getpid());
}
#define vTaskDelete(h) vTaskDeleteImpl((TaskHandle_t)(uintptr_t)(h))
#ifdef __cplusplus
extern "C" {
#endif

static inline void vTaskDelayMs(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

#ifdef ESP_SR_LINK_EXPORTS
/* Global definition lives in esp_sr_link_exports.c for prebuilt .a */
void vTaskDelay(TickType_t ticks);
#else
static inline void vTaskDelay(TickType_t ticks)
{
    struct timespec ts;
    ts.tv_sec = ticks / CLOCKS_PER_SEC;
    ts.tv_nsec = (ticks % CLOCKS_PER_SEC) * (1000000000 / CLOCKS_PER_SEC);
    nanosleep(&ts, NULL);
}
#endif

static inline TickType_t xTaskGetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (TickType_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Event Groups */
typedef nxevent_t *EventGroupHandle_t;

#define BIT0 (1 << 0)
#define BIT1 (1 << 1)
#define BIT2 (1 << 2)
#define bitsEVENT_ALL 0xFFFFFFFF

static inline EventGroupHandle_t xEventGroupCreate(void)
{
    nxevent_t *ev = (nxevent_t *)malloc(sizeof(nxevent_t));
    if (ev)
        nxevent_init(ev, 0);
    return ev;
}

static inline void vEventGroupDelete(EventGroupHandle_t eg)
{
    if (eg)
        free(eg);
}

static inline EventBits_t xEventGroupSetBits(EventGroupHandle_t eg, EventBits_t bits)
{
    if (eg)
        nxevent_post(eg, bits, 0);
    return 0;
}

static inline EventBits_t xEventGroupClearBits(EventGroupHandle_t eg, EventBits_t bits)
{
    /* FreeRTOS xEventGroupClearBits() clears the given bits (eventBits &=
     * ~bits).  nxevent_reset() REPLACES the whole mask (event->events = bits),
     * which would corrupt the event group and cause spurious wakeups /
     * lost-wakeup hangs.  nxevent_clear() is the correct ANDNOT primitive. */
    if (eg)
        return (EventBits_t)nxevent_clear(eg, (nxevent_mask_t)bits);
    return 0;
}

static inline EventBits_t xEventGroupGetBits(EventGroupHandle_t eg)
{
    if (eg)
        return nxevent_tickwait(eg, 0xFFFFFFFF, NXEVENT_WAIT_NOCLEAR, 0);
    return 0;
}

static inline EventBits_t xEventGroupWaitBits(
    EventGroupHandle_t eg, EventBits_t bits, BaseType_t clear, BaseType_t wait_all,
    TickType_t ticks)
{
    if (!eg)
        return 0;
    nxevent_flags_t flags = 0;
    if (wait_all)
        flags |= NXEVENT_WAIT_ALL;
    if (!clear)
        flags |= NXEVENT_WAIT_NOCLEAR;
    return nxevent_tickwait(eg, bits, flags, ticks);
}

/* Semaphore — use POSIX sem_t for both mutexes and binary semaphores.
 * A mutex is a binary semaphore initialized to 1 (available/owned).
 * A binary semaphore is initialized to 0 (empty).  sem_t supports
 * cross-thread give/take without ownership constraints, which the GMF
 * data_bus (esp_gmf_fifo / esp_gmf_block / esp_gmf_ringbuffer) and
 * esp_gmf_task require for signaling between the pipeline task and the
 * caller. */
typedef sem_t *SemaphoreHandle_t;

#ifndef QueueHandle_t
typedef void *QueueHandle_t;
#endif

/* Queue-as-mutex / generic queue APIs used by esp-sr AFE (esp_sr_link_exports.c). */
QueueHandle_t xQueueCreateMutex(const uint8_t ucQueueType);
QueueHandle_t xQueueGenericCreate(const UBaseType_t uxQueueLength,
                                  const UBaseType_t uxItemSize,
                                  const uint8_t ucQueueType);
BaseType_t xQueueSemaphoreTake(QueueHandle_t xQueue, TickType_t xTicksToWait);
BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void *pvItemToQueue,
                             TickType_t xTicksToWait, const BaseType_t xCopyPosition);
void vQueueDelete(QueueHandle_t xQueue);

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    sem_t *s = (sem_t *)malloc(sizeof(sem_t));
    if (s)
        sem_init(s, 0, 1);  /* mutex: initially available */
    return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    sem_t *s = (sem_t *)malloc(sizeof(sem_t));
    if (s)
        sem_init(s, 0, 0);  /* binary: initially empty */
    return s;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    if (sem)
    {
        sem_destroy(sem);
        free(sem);
    }
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    if (!sem)
        return pdFALSE;
    if (ticks == portMAX_DELAY)
        return sem_wait(sem) == 0 ? pdTRUE : pdFALSE;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ticks / CLOCKS_PER_SEC;
    ts.tv_nsec += (ticks % CLOCKS_PER_SEC) * (1000000000 / CLOCKS_PER_SEC);
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    return sem_timedwait(sem, &ts) == 0 ? pdTRUE : pdFALSE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    if (!sem)
        return pdFALSE;
    return sem_post(sem) == 0 ? pdTRUE : pdFALSE;
}

#ifdef __cplusplus
}
#endif

#endif /* __FREERTOS_SHIM_H */
