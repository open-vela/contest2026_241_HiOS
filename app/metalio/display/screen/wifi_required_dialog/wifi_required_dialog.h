/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiFi-required dialog — ported from MetalioClaw4
 * main/display/screen/wifi_required_dialog.h.
 *
 * Blocks networked actions only when both WiFi and cellular are unavailable.
 */

#pragma once

// Legacy name: returns true only when neither WiFi nor cellular is usable.
bool WifiRequired_ShouldBlock();

// Pop a "Network not connected" dialog on the active screen.
// `hint_msgid` is an I18n source string (zh-CN msgid); nullptr falls
// back to the default hint.
void WifiRequired_ShowDialog(const char *hint_msgid = nullptr);
