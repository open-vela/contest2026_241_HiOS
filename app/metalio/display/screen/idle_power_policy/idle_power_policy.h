/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Idle power policy — ported from MetalioClaw4
 * main/display/screen/idle_power_policy.h.
 *
 * One no-input interval across all apps and overlays. App transitions do
 * not stop the timer; automatic standby preserves the shutdown interval.
 */

#pragma once

#include <cstdint>

enum class IdlePowerSession : uint8_t {
    None = 0,
    Home,
    Standby,
    App,
};

void IdlePower_NotifyActivity();
void IdlePower_Attach(IdlePowerSession session, bool reset_activity);
void IdlePower_Detach(IdlePowerSession session);
void IdlePower_Stop();

// Called right before an auto-standby switch: preserves last_activity
// so deleting the home screen does not reset the accumulated idle time.
void IdlePower_PrepareAutoStandby();

int IdlePower_GetStandbyMinutes();
void IdlePower_SetStandbyMinutes(int minutes);
int IdlePower_GetShutdownMinutes();
void IdlePower_SetShutdownMinutes(int minutes);

IdlePowerSession IdlePower_CurrentSession();
