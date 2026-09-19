/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiFi-required dialog — ported from MetalioClaw4
 * main/display/screen/wifi_required_dialog.h.
 *
 * Blocks networked apps in WiFi mode when the station is not connected.
 * 4G (cellular) mode is never blocked.
 */

#pragma once

// Returns true when the device is in WiFi mode and the station is not
// connected.
bool WifiRequired_ShouldBlock();

// Pop a "WiFi not connected" dialog on the active screen.
// `hint_msgid` is an I18n source string (zh-CN msgid); nullptr falls
// back to the default hint.
void WifiRequired_ShowDialog(const char *hint_msgid = nullptr);
