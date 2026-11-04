/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * HyperOS-style recent-apps overlay.
 */

#pragma once

#include "lvgl.h"

void Recents_NotifyOpened(const char *app_id, const char *app_name,
                          const char *icon_path);
void Recents_Show(lv_obj_t *parent);
void Recents_Hide(void);
bool Recents_IsOpen(void);
void Recents_Clear(void);

// Positive direction selects an older app; negative selects a newer app.
bool Recents_SwitchAdjacent(int direction, bool from_home);
