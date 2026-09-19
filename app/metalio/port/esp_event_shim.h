/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-IDF event loop shim for NuttX/openvela.
 * Provides a simple in-app event registry.
 */

#ifndef __ESP_EVENT_SHIM_H
#define __ESP_EVENT_SHIM_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event base constants */
#define ESP_EVENT_ANY_BASE  NULL
#define ESP_EVENT_ANY_ID    (-1)

/* WiFi/IP event bases (string constants) */
#define WIFI_EVENT       "wifi_event"
#define IP_EVENT         "ip_event"
#define BLUETOOTH_EVENT  "bt_event"

/* Common WiFi event IDs */
#define WIFI_EVENT_STA_START       2
#define WIFI_EVENT_STA_CONNECTED   4
#define WIFI_EVENT_STA_DISCONNECTED 5
#define WIFI_EVENT_AP_START        7
#define WIFI_EVENT_AP_STACONNECTED 8

/* Common IP event IDs */
#define IP_EVENT_STA_GOT_IP        0
#define IP_EVENT_STA_LOST_IP       1

typedef void (*esp_event_handler_t)(void *arg, const char *event_base,
                                     int32_t event_id, void *event_data);

typedef struct esp_event_handler_entry
{
    const char *base;
    int32_t id;
    esp_event_handler_t cb;
    void *arg;
    struct esp_event_handler_entry *next;
} esp_event_handler_entry_t;

typedef struct esp_event_loop
{
    esp_event_handler_entry_t *handlers;
    pthread_mutex_t lock;
} esp_event_loop_t;

static esp_event_loop_t s_default_loop;
static bool s_loop_initialized = false;

static inline esp_err_t esp_event_loop_create_default(void)
{
    if (s_loop_initialized)
        return ESP_OK;
    pthread_mutex_init(&s_default_loop.lock, NULL);
    s_default_loop.handlers = NULL;
    s_loop_initialized = true;
    return ESP_OK;
}

static inline esp_err_t esp_event_handler_register(
    const char *event_base, int32_t event_id,
    esp_event_handler_t event_handler, void *arg)
{
    if (!s_loop_initialized || !event_handler)
        return -EINVAL;

    esp_event_handler_entry_t *e =
        (esp_event_handler_entry_t *)calloc(1, sizeof(*e));
    if (!e)
        return -ENOMEM;
    e->base = event_base;
    e->id = event_id;
    e->cb = event_handler;
    e->arg = arg;

    pthread_mutex_lock(&s_default_loop.lock);
    e->next = s_default_loop.handlers;
    s_default_loop.handlers = e;
    pthread_mutex_unlock(&s_default_loop.lock);
    return ESP_OK;
}

static inline esp_err_t esp_event_handler_unregister(
    const char *event_base, int32_t event_id,
    esp_event_handler_t event_handler)
{
    if (!s_loop_initialized)
        return -EINVAL;

    pthread_mutex_lock(&s_default_loop.lock);
    esp_event_handler_entry_t **pp = &s_default_loop.handlers;
    while (*pp)
    {
        if ((*pp)->cb == event_handler &&
            (*pp)->base == event_base &&
            (*pp)->id == event_id)
        {
            esp_event_handler_entry_t *del = *pp;
            *pp = del->next;
            free(del);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&s_default_loop.lock);
    return ESP_OK;
}

static inline esp_err_t esp_event_post(
    const char *event_base, int32_t event_id,
    void *event_data, size_t event_data_size,
    void *unused)
{
    if (!s_loop_initialized)
        return -EINVAL;

    pthread_mutex_lock(&s_default_loop.lock);
    esp_event_handler_entry_t *e = s_default_loop.handlers;
    while (e)
    {
        if ((e->base == NULL || e->base == event_base) &&
            (e->id == ESP_EVENT_ANY_ID || e->id == event_id))
        {
            e->cb(e->arg, event_base, event_id, event_data);
        }
        e = e->next;
    }
    pthread_mutex_unlock(&s_default_loop.lock);
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* __ESP_EVENT_SHIM_H */
