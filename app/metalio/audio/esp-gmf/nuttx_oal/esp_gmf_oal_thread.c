/*
 * SPDX-FileCopyrightText: 2026 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX OAL thread implementation — replaces esp_gmf_oal_thread.c.
 *
 * Maps the ESP-GMF OAL thread API to NuttX pthread:
 *   xTaskCreatePinnedToCore  →  pthread_create (stack size via attr)
 *   vTaskDelete              →  pthread_cancel + pthread_detach
 *   xTaskCreatePinnedToCoreWithCaps → pthread_create (PSRAM stack not supported)
 *
 * The ESP-IDF thread function signature is void(*)(void*), which matches
 * pthread_create's void*(*)(void*) with a small wrapper.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/types.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_gmf_err.h"
#include "esp_gmf_oal_mem.h"
#include "esp_gmf_oal_thread.h"

static const char *TAG = "ESP_GMF_THREAD";

/* Wrapper to adapt FreeRTOS-style void(*)(void*) to pthread's void*(*)(void*). */
typedef struct {
    void (*main_func)(void *arg);
    void *arg;
} gmf_thread_ctx_t;

static void *gmf_thread_trampoline(void *p)
{
    gmf_thread_ctx_t *ctx = (gmf_thread_ctx_t *)p;
    void (*fn)(void *) = ctx->main_func;
    void *arg = ctx->arg;
    free(ctx);
    fn(arg);
    return NULL;
}

esp_gmf_err_t esp_gmf_oal_thread_create(esp_gmf_oal_thread_t *p_handle, const char *name,
                                         void (*main_func)(void *arg), void *arg,
                                         uint32_t stack, int prio, bool stack_in_ext, int core_id)
{
    (void)stack_in_ext; /* PSRAM stack not supported on NuttX — always internal */
    (void)core_id;      /* Single-core ESP32-P4 */
    (void)name;         /* pthread doesn't support named threads on NuttX */

    gmf_thread_ctx_t *ctx = (gmf_thread_ctx_t *)malloc(sizeof(gmf_thread_ctx_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Failed to allocate thread context for %s", name ? name : "(null)");
        return ESP_GMF_ERR_FAIL;
    }
    ctx->main_func = main_func;
    ctx->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);

    /* Stack size — NuttX pthread uses bytes (FreeRTOS uses words) */
    if (stack > 0) {
        pthread_attr_setstacksize(&attr, stack);
    }

    /* Priority — NuttX uses sched_priority (1=highest, 255=lowest).
     * ESP-IDF FreeRTOS priorities are 0-31 with higher=more important.
     * ESP-IDF "5" is low-mid priority; mapping it directly would yield NuttX
     * priority 5 (very high) and starve the UI/event loop (default 100).
     * Map to a priority LOWER than the app default. */
    if (prio > 0) {
        struct sched_param sp;
        sp.sched_priority = SCHED_PRIORITY_DEFAULT + (int)prio;
        pthread_attr_setschedparam(&attr, &sp);
    }

    pthread_t tid;
    write(1, "OAL_T0\n", 7);
    int ret = pthread_create(&tid, &attr, gmf_thread_trampoline, ctx);
    write(1, "OAL_T1\n", 7);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to create thread %s (ret=%d)", name ? name : "(null)", ret);
        free(ctx);
        return ESP_GMF_ERR_FAIL;
    }

    /* Detach so the thread resources are auto-reclaimed when it exits. */
    pthread_detach(tid);

    if (p_handle) {
        /* Store the pthread_t by value (it's typically a unsigned long). */
        *p_handle = (esp_gmf_oal_thread_t)(uintptr_t)tid;
    }
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t esp_gmf_oal_thread_delete(esp_gmf_oal_thread_t p_handle)
{
    if (p_handle == NULL) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    pthread_t tid = (pthread_t)(uintptr_t)p_handle;
    /* Cancel the thread. On NuttX, pthread_cancel is supported if
     * CONFIG_PTHREAD_CANCEL is enabled. The thread's cleanup handlers
     * (if any) will run. */
    pthread_cancel(tid);
    /* No join — the thread was created detached. */
    return ESP_GMF_ERR_OK;
}
