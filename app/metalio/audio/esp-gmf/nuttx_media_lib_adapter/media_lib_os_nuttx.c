/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * NuttX pthread/semaphore/nxevent-based adapter for media_lib_sal.
 * Implements the media_lib_os_reg_t vtable so that media_lib_os.c
 * (the registration-based dispatcher) can forward all malloc/thread/
 * mutex/semaphore/event-group calls to NuttX POSIX APIs.
 *
 * Initialisation: call media_lib_os_nuttx_install() once at boot
 * (before any esp_hls_stream or esp_extractor code runs) to register
 * the vtable.  gmf_bootstrap.c (in radio_screen) handles this.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "media_lib_err.h"

static const char *TAG = "ml_os_nuttx";

/* -------------------------------------------------------------------- *
 * Memory helpers — straight malloc/free wrappers; "caps" (IRAM/DMA/
 * PSRAM) are meaningful on ESP-IDF only, so for NuttX we ignore caps
 * and fall back to standard posix_memalign for aligned allocations.
 * -------------------------------------------------------------------- */

static void *__malloc(size_t size)
{
    return malloc(size);
}

static void __free(void *buf)
{
    free(buf);
}

static void *__calloc(size_t num, size_t size)
{
    return calloc(num, size);
}

static void *__realloc(void *buf, size_t size)
{
    return realloc(buf, size);
}

static char *__strdup(const char *str)
{
    if (str == NULL) {
        return NULL;
    }
    size_t len = strlen(str) + 1;
    char *out = (char *)malloc(len);
    if (out) {
        memcpy(out, str, len);
    }
    return out;
}

static void *__caps_malloc_align(size_t align, size_t size, int caps)
{
    (void)caps;
    void *p = NULL;
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    if (posix_memalign(&p, align, size) != 0) {
        return NULL;
    }
    return p;
}

static int __get_stack_frame(void **addr, int n)
{
    /* Stack unwinding requires platform specific support.
     * Return 0 (no frames captured) — the trace/debug code in
     * media_lib_sal tolerates empty stack frames. */
    (void)addr;
    (void)n;
    return 0;
}

/* -------------------------------------------------------------------- *
 * Threads — pthread_create with detached-by-default, stored as a
 * malloc'd pthread_t handle (to survive after create() returns).
 * FreeRTOS task priorities are in range 0..(configMAX_PRIORITIES-1);
 * on NuttX we map them linearly onto the SCHED_FIFO realtime band.
 * -------------------------------------------------------------------- */

struct ml_thread {
    pthread_t       id;
    char           *name;
    void          (*body)(void *arg);
    void           *arg;
    int             prio;
    bool            running;
};

static void *__thread_trampoline(void *arg)
{
    struct ml_thread *t = (struct ml_thread *)arg;
    t->body(t->arg);
    t->running = false;
    return NULL;
}

static int __thread_create(media_lib_thread_handle_t *handle, const char *name,
                           void (*body)(void *arg), void *arg,
                           uint32_t stack_size, int prio, int core)
{
    (void)core;  /* single-core RISC-V in the Metalio Claw4 configuration */
    if (handle == NULL || body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct ml_thread *t = (struct ml_thread *)calloc(1, sizeof(*t));
    if (t == NULL) {
        return ESP_ERR_NO_MEM;
    }
    t->body = body;
    t->arg  = arg;
    t->prio = prio;
    t->running = true;
    if (name) {
        t->name = __strdup(name);
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size > 0 && stack_size >= PTHREAD_STACK_MIN) {
        pthread_attr_setstacksize(&attr, stack_size);
    }
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    struct sched_param sp;
    /* FreeRTOS: higher number = higher priority, range 0..N.
     * On NuttX, lower number = higher priority and the app/UI/network
     * default is SCHED_PRIORITY_DEFAULT (100).  Map media-lib threads to a
     * priority LOWER than that default (100 + prio) and use SCHED_RR so they
     * time-slice.  The previous SCHED_FIFO mapping landed prio-5 threads at
     * NuttX priority ~51, which preempts the default-100 DHCP recv thread and
     * the LPWORK SDIO poll worker; their busy-loops starve DHCP (recv timeout
     * never fires) and the C5 RX poll (DHCP ACK never read), wedging DNS and
     * hence the whole HLS fetch. */
    sp.sched_priority = SCHED_PRIORITY_DEFAULT + (int)prio;
    pthread_attr_setschedpolicy(&attr, SCHED_RR);
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    int ret = pthread_create(&t->id, &attr, __thread_trampoline, t);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        ESP_LOGE(TAG, "pthread_create '%s' failed: %d", name ? name : "?", ret);
        free(t->name);
        free(t);
        return ESP_FAIL;
    }
    if (t->name) {
        pthread_setname_np(t->id, t->name);
    }
    *handle = (media_lib_thread_handle_t)t;
    return ESP_OK;
}

static void __thread_destroy(media_lib_thread_handle_t handle)
{
    struct ml_thread *t = (struct ml_thread *)handle;
    if (t == NULL) {
        return;
    }
    void *unused;
    pthread_join(t->id, &unused);
    free(t->name);
    free(t);
}

static bool __thread_set_priority(media_lib_thread_handle_t handle, int prio)
{
    struct ml_thread *t = (struct ml_thread *)handle;
    if (t == NULL) {
        return false;
    }
    struct sched_param sp;
    sp.sched_priority = SCHED_PRIORITY_DEFAULT + (int)prio;
    if (pthread_setschedparam(t->id, SCHED_RR, &sp) == 0) {
        t->prio = prio;
        return true;
    }
    return false;
}

static void __thread_sleep(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* -------------------------------------------------------------------- *
 * Semaphores (counting).  POSIX sem_t stored in heap memory.
 * Timeout is in milliseconds (per media_lib_sal convention).
 * -------------------------------------------------------------------- */

static int __sema_create(media_lib_sema_handle_t *sema)
{
    if (sema == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sem_t *s = (sem_t *)malloc(sizeof(sem_t));
    if (s == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (sem_init(s, 0, 0) != 0) {
        free(s);
        return ESP_FAIL;
    }
    *sema = (media_lib_sema_handle_t)s;
    return ESP_OK;
}

static int __sema_lock(media_lib_sema_handle_t sema, uint32_t timeout_ms)
{
    sem_t *s = (sem_t *)sema;
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0xFFFFFFFFu || timeout_ms == 0) {
        /* 0xFFFFFFFF = "infinite"; 0 also treated as blocking by default path */
        while (sem_wait(s) != 0 && errno == EINTR) { }
        return 0;
    }
    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_sec  += timeout_ms / 1000;
    abstime.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    while (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_nsec -= 1000000000L;
        abstime.tv_sec  += 1;
    }
    int ret;
    do {
        ret = sem_timedwait(s, &abstime);
    } while (ret != 0 && errno == EINTR);
    if (ret != 0) {
        return -ETIMEDOUT;
    }
    return 0;
}

static int __sema_unlock(media_lib_sema_handle_t sema)
{
    sem_t *s = (sem_t *)sema;
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sem_post(s) != 0) {
        return -errno;
    }
    return 0;
}

static int __sema_destroy(media_lib_sema_handle_t sema)
{
    sem_t *s = (sem_t *)sema;
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sem_destroy(s);
    free(s);
    return 0;
}

/* -------------------------------------------------------------------- *
 * Mutexes — pthread_mutex_t stored on heap.
 * -------------------------------------------------------------------- */

static int __mutex_create(media_lib_mutex_handle_t *mutex)
{
    if (mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (m == NULL) {
        return ESP_ERR_NO_MEM;
    }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (pthread_mutex_init(m, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        free(m);
        return ESP_FAIL;
    }
    pthread_mutexattr_destroy(&attr);
    *mutex = (media_lib_mutex_handle_t)m;
    return ESP_OK;
}

static int __mutex_lock(media_lib_mutex_handle_t mutex, uint32_t timeout_ms)
{
    pthread_mutex_t *m = (pthread_mutex_t *)mutex;
    if (m == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0xFFFFFFFFu || timeout_ms == 0) {
        while (pthread_mutex_lock(m) != 0 && errno == EINTR) { }
        return 0;
    }
    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_sec  += timeout_ms / 1000;
    abstime.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    while (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_nsec -= 1000000000L;
        abstime.tv_sec  += 1;
    }
    int ret = pthread_mutex_timedlock(m, &abstime);
    return ret == 0 ? 0 : -ETIMEDOUT;
}

static int __mutex_unlock(media_lib_mutex_handle_t mutex)
{
    pthread_mutex_t *m = (pthread_mutex_t *)mutex;
    if (m == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pthread_mutex_unlock(m) != 0) {
        return -errno;
    }
    return 0;
}

static int __mutex_destroy(media_lib_mutex_handle_t mutex)
{
    pthread_mutex_t *m = (pthread_mutex_t *)mutex;
    if (m == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_destroy(m);
    free(m);
    return 0;
}

/* -------------------------------------------------------------------- *
 * Critical sections — approximate with a global recursive mutex, since
 * NuttX has no single-reader-writer "up_irq_save()/up_irq_restore()"
 * in flat userspace builds we can safely use here.  This is coarse
 * but sufficient for media_lib_sal's infrequent enter/leave usage
 * (mostly reference counter tweaks).
 * -------------------------------------------------------------------- */

static pthread_mutex_t g_critical_lock = PTHREAD_MUTEX_INITIALIZER;

static int __enter_critical_section(void)
{
    pthread_mutex_lock(&g_critical_lock);
    return 0;
}

static int __leave_critical_section(void)
{
    pthread_mutex_unlock(&g_critical_lock);
    return 0;
}

/* -------------------------------------------------------------------- *
 * Event groups — backed by the Metalio freertos_shim nxevent_t helpers.
 * -------------------------------------------------------------------- */

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static int __event_group_create(media_lib_event_grp_handle_t *event_group)
{
    if (event_group == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    EventGroupHandle_t eg = xEventGroupCreate();
    if (eg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *event_group = (media_lib_event_grp_handle_t)eg;
    return ESP_OK;
}

static uint32_t __event_group_set_bits(media_lib_event_grp_handle_t event_group,
                                       uint32_t bits)
{
    EventGroupHandle_t eg = (EventGroupHandle_t)event_group;
    return (uint32_t)xEventGroupSetBits(eg, (EventBits_t)bits);
}

static uint32_t __event_group_clr_bits(media_lib_event_grp_handle_t event_group,
                                       uint32_t bits)
{
    EventGroupHandle_t eg = (EventGroupHandle_t)event_group;
    return (uint32_t)xEventGroupClearBits(eg, (EventBits_t)bits);
}

static uint32_t __event_group_wait_bits(media_lib_event_grp_handle_t event_group,
                                        uint32_t bits, uint32_t timeout_ms)
{
    EventGroupHandle_t eg = (EventGroupHandle_t)event_group;
    TickType_t tks;
    if (timeout_ms == 0xFFFFFFFFu) {
        tks = portMAX_DELAY;
    } else {
        tks = pdMS_TO_TICKS(timeout_ms);
    }
    return (uint32_t)xEventGroupWaitBits(eg, (EventBits_t)bits,
                                         pdFALSE,   /* don't clear on exit  */
                                         pdFALSE,   /* wait for ANY bit     */
                                         tks);
}

static int __event_group_destroy(media_lib_event_grp_handle_t event_group)
{
    EventGroupHandle_t eg = (EventGroupHandle_t)event_group;
    if (eg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    vEventGroupDelete(eg);
    return 0;
}

/* -------------------------------------------------------------------- *
 * Vtable assembly + public install helper.
 * -------------------------------------------------------------------- */

void media_lib_os_nuttx_install(void)
{
    static bool installed = false;
    if (installed) {
        return;
    }
    media_lib_os_t os = { 0 };

    os.malloc            = __malloc;
    os.free              = __free;
    os.calloc            = __calloc;
    os.realloc           = __realloc;
    os.strdup            = __strdup;
    os.caps_malloc_align = __caps_malloc_align;
    os.get_stack_frame   = __get_stack_frame;

    os.thread_create     = __thread_create;
    os.thread_destroy    = __thread_destroy;
    os.thread_set_prio   = __thread_set_priority;
    os.thread_sleep      = __thread_sleep;

    os.sema_create       = __sema_create;
    os.sema_lock         = __sema_lock;
    os.sema_unlock       = __sema_unlock;
    os.sema_destroy      = __sema_destroy;

    os.mutex_create      = __mutex_create;
    os.mutex_lock        = __mutex_lock;
    os.mutex_unlock      = __mutex_unlock;
    os.mutex_destroy     = __mutex_destroy;

    os.enter_critical    = __enter_critical_section;
    os.leave_critical    = __leave_critical_section;

    os.group_create      = __event_group_create;
    os.group_set_bits    = __event_group_set_bits;
    os.group_clr_bits    = __event_group_clr_bits;
    os.group_wait_bits   = __event_group_wait_bits;
    os.group_destroy     = __event_group_destroy;

    media_lib_os_register(&os);
    installed = true;
    ESP_LOGI(TAG, "media_lib_sal NuttX OS adapter installed");
}

/*
 * media_lib_adapter.c installs a constructor
 * (media_lib_auto_init_os_adapter) that calls
 * media_lib_add_default_os_adapter() before app_main.  The upstream
 * FreeRTOS adapter (port/media_lib_os_freertos.c) provides this symbol;
 * on NuttX we are the OS adapter, so expose the same entry point and
 * forward it to our pthread-based installer.
 */
esp_err_t media_lib_add_default_os_adapter(void)
{
    media_lib_os_nuttx_install();
    return ESP_OK;
}
