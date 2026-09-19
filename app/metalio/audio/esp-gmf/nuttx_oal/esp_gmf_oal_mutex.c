/*
 * SPDX-FileCopyrightText: 2026 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX OAL mutex implementation — replaces esp_gmf_oal_mutex.c.
 *
 * Maps the ESP-GMF OAL mutex API to NuttX pthread_mutex:
 *   xSemaphoreCreateMutex  →  malloc(pthread_mutex_t) + pthread_mutex_init
 *   xSemaphoreTake         →  pthread_mutex_lock
 *   xSemaphoreGive         →  pthread_mutex_unlock
 *   vSemaphoreDelete       →  pthread_mutex_destroy + free
 *
 * The OAL API uses void* for the mutex handle, so we allocate a
 * pthread_mutex_t on the heap and return its address.
 */

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "esp_gmf_oal_mutex.h"

void *esp_gmf_oal_mutex_create(void)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (m == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(m, NULL) != 0) {
        free(m);
        return NULL;
    }
    return (void *)m;
}

int esp_gmf_oal_mutex_destroy(void *mutex)
{
    if (mutex == NULL) {
        return -1;
    }
    pthread_mutex_t *m = (pthread_mutex_t *)mutex;
    pthread_mutex_destroy(m);
    free(m);
    return 0;
}

int esp_gmf_oal_mutex_lock(void *mutex)
{
    if (mutex == NULL) {
        return -1;
    }
    return pthread_mutex_lock((pthread_mutex_t *)mutex);
}

int esp_gmf_oal_mutex_unlock(void *mutex)
{
    if (mutex == NULL) {
        return -1;
    }
    return pthread_mutex_unlock((pthread_mutex_t *)mutex);
}
