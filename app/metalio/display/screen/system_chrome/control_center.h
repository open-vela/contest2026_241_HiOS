/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * HyperOS-style control center (quick settings shade).
 */

#pragma once

#include "lvgl.h"

void ControlCenter_Show(lv_obj_t *parent);
void ControlCenter_Hide(void);
bool ControlCenter_IsOpen(void);
void ControlCenter_Refresh(void);

/* UI-thread-only: half-size copy of the control center's airplane artwork. */
const lv_image_dsc_t *ControlCenter_GetStatusPlaneIcon(void);

/* Shared motor pulse (control-center vibration tile + PWR_KEY haptic).
 * pulse_us defaults to 160 ms; use a shorter value for the power key.
 * PulseVibrateAsync spawns a worker (start + stop) — has spawn latency.
 * PulseVibrateInstant starts the motor synchronously (haptic fires on the
 * press edge) and only schedules the trailing stop. */
void PulseVibrateAsync(uint32_t pulse_us = 160 * 1000);
void PulseVibrateInstant(uint32_t pulse_us = 160 * 1000);

/* Flush pending preferences before dimming/resetting or cutting power. */
void ControlCenter_SaveBeforePowerOff();
