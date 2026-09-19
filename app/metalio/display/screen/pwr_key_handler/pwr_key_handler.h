/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * PwrKey handler — ported from MetalioClaw4
 * main/display/screen/pwr_key_handler.h.
 *
 * Centralised PWR_KEY management: short-press and long-press handlers
 * are registered once at boot, and each screen pushes / pops its name
 * on a foreground stack via PwrKey_OnScreenLifecycle(). The key
 * callback dispatches based on the top-of-stack screen name.
 */

#pragma once

#include "screen_util.h"

// Register short-press / long-press callbacks. Call once after the IO
// expander is ready; guarded internally by a once flag.
void PwrKey_Init();

// Screen lifecycle hook: LOAD pushes `name`, UNLOAD removes the
// topmost matching entry.
void PwrKey_OnScreenLifecycle(const char *name,
                              screen_lifecycle_event_t event);

// Name of the current foreground screen (never null; "none" when the
// stack is empty).
const char *PwrKey_ActiveScreen();
