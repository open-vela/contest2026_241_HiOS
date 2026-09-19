/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test UI common helpers — ported from MetalioClaw4
 * main/display/screen/test_screen/test_ui_common.h.
 *
 * Shared panel geometry, palette and row/header/body builders reused by
 * the hardware test sub-screens (auto / stress / screen color / touch).
 */

#pragma once

#include "lvgl.h"

constexpr int kTestPanelW     = 720;
constexpr int kTestPanelH     = 720;
constexpr int kTestHeaderH    = 96;
constexpr int kTestRowH       = 84;
constexpr int kTestRowGap     = 10;
constexpr int kTestSideMargin = 20;
constexpr int kTestBodyY      = kTestHeaderH;
constexpr int kTestBodyH      = kTestPanelH - kTestHeaderH;

constexpr uint32_t kTestColorBg      = 0x0E1116;
constexpr uint32_t kTestColorCardBg  = 0x1B2030;
constexpr uint32_t kTestColorTextDim = 0x9AA3B2;
constexpr uint32_t kTestColorHigh    = 0x10B981;
constexpr uint32_t kTestColorMuted   = 0x2A2F3A;
constexpr uint32_t kTestColorError   = 0xEF4444;

constexpr const char *kTestIconPass = "A:ic_app_test_pass.spng";
constexpr const char *kTestIconFail = "A:ic_app_test_fail.spng";

// Create a row card: title + optional status icon + right control container.
lv_obj_t *TestUiCreateRowShell(lv_obj_t *parent, const char *title,
                               lv_obj_t **out_status_icon,
                               lv_obj_t **out_right_ctrl);

void TestUiUpdateStatus(lv_obj_t *status_icon, bool pass);

// Root test screen, set on Create, used to host the confirm dialog.
void TestUiSetScreen(lv_obj_t *scr);
lv_obj_t *TestUiGetScreen();

typedef void (*TestUiConfirmResultCb)(bool pass, void *user_data);

void TestUiShowConfirmDialog(const char *message, TestUiConfirmResultCb cb,
                             void *user_data);
void TestUiDismissConfirmDialog();

lv_obj_t *TestUiCreateValueLabel(lv_obj_t *right_ctrl);
lv_obj_t *TestUiCreateSwitch(lv_obj_t *right_ctrl, lv_event_cb_t cb,
                             void *user_data);

lv_obj_t *TestUiCreateHeader(lv_obj_t *scr, const char *title,
                             lv_event_cb_t on_back_cb);
lv_obj_t *TestUiCreateScrollBody(lv_obj_t *scr);
lv_obj_t *TestUiCreateMenuRow(lv_obj_t *parent, const char *title,
                              lv_event_cb_t on_click_cb);

void TestUiNavigateTo(lv_obj_t *(*create_screen)());
