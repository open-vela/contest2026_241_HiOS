/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * System chrome — HyperOS-inspired desktop gestures:
 *   - Bottom gesture pill (swipe up / hold / long-press)
 *   - Pull-down control center (WiFi / BT / 4G / Airplane / brightness / volume)
 *   - Recents overlay (swipe-up-and-hold)
 *
 * Attach to every fullscreen screen (home + apps).
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Attach gesture pill + top pull zone to `scr`.
 * is_home: home uses its own vertical-swipe path for control center too. */
void SystemChrome_Attach(lv_obj_t *scr, bool is_home);
void SystemChrome_AttachStandbyPill(lv_obj_t *clock);
/* Non-intercepting edge observer; also sees top-layer/fullscreen dialogs. */
bool SystemChrome_ObservePointer(lv_indev_t *input, lv_event_code_t code,
                                lv_obj_t *target);
bool SystemChrome_ChatOwnsAssistantUi(void);

/* Track an opened app for the recents overlay. */
void SystemChrome_NotifyAppOpened(const char *app_id, const char *app_name,
                                  const char *icon_path);

void SystemChrome_ShowControlCenter(void);
void SystemChrome_HideControlCenter(void);
bool SystemChrome_IsControlCenterOpen(void);

void SystemChrome_ShowRecents(void);
void SystemChrome_HideRecents(void);
bool SystemChrome_IsRecentsOpen(void);

void SystemChrome_DismissOverlays(void);

/* Floating AI dialog opened by a long-press on the gesture pill. */
bool SystemChrome_FloatAiOpen(void);
void SystemChrome_FloatAiAppend(const char *role, const char *text);
void SystemChrome_FloatAiSyncState(void);
/* Show the desktop AI card. start_session: also RequestWakeSession (pill).
 * Wake-word path uses start_session=false and opens the channel itself. */
void SystemChrome_EnsureFloatAi(bool start_session);
void SystemChrome_CloseFloatAi(void);

#ifdef __cplusplus
}
#endif
