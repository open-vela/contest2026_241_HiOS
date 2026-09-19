/*
 * SPDX-FileCopyrightText: 2026 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * FreeRTOS redirect header for NuttX/openvela.
 * The gmf_core source files include "freertos/FreeRTOS.h" for type
 * definitions (BaseType_t, TickType_t, etc.). All actual OS operations
 * go through the OAL (esp_gmf_oal_*), so these types are the only thing
 * needed from this header.
 *
 * We redirect to the project's existing freertos_shim.h which provides
 * all the types plus inline API implementations using NuttX primitives.
 */
#ifndef __FREERTOS_SHIM_REDIRECT_H
#define __FREERTOS_SHIM_REDIRECT_H

#include "freertos_shim.h"

/* Additional types used by gmf_core that are not in freertos_shim.h.
 * QueueHandle_t is referenced in the OAL mutex casts but never actually
 * used as a queue on NuttX — the OAL uses void* for mutex handles. */
#ifndef QueueHandle_t
typedef void *QueueHandle_t;
#endif

/* portNUM_PROCESSORS — ESP-IDF SMP macro. NuttX on ESP32-P4 is single-core. */
#ifndef portNUM_PROCESSORS
#define portNUM_PROCESSORS 1
#endif

/* BIT(x) — ESP-IDF bit-manipulation macro used by event groups, GMF task
 * state flags, and esp_audio_simple_player. freertos_shim.h only defines
 * BIT0/BIT1/BIT2; the GMF sources (and esp_audio_simple_player.c) use the
 * parametric BIT(n) form. Provide it here so it's available to every file
 * that includes freertos/FreeRTOS.h. */
#ifndef BIT
#define BIT(x) (1U << (x))
#endif

/* tskNO_AFFINITY — no core pinning (single core). */
#ifndef tskNO_AFFINITY
#define tskNO_AFFINITY (-1)
#endif

/* configRUN_TIME_COUNTER_TYPE — used in esp_gmf_oal_sys.c stats code. */
#ifndef configRUN_TIME_COUNTER_TYPE
#define configRUN_TIME_COUNTER_TYPE uint32_t
#endif

#endif /* __FREERTOS_SHIM_REDIRECT_H */
