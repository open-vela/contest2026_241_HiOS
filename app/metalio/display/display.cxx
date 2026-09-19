/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display base class implementation — ported from MetalioClaw4.
 */

#include "display.h"

Display::Display() {}
Display::~Display() {}

void Display::SetStatus(const char *status) { (void)status; }
void Display::ShowNotification(const char *notification, int duration_ms) { (void)notification; (void)duration_ms; }
void Display::ShowNotification(const std::string &notification, int duration_ms)
{
    ShowNotification(notification.c_str(), duration_ms);
}
void Display::SetEmotion(const char *emotion) { (void)emotion; }
void Display::SetChatMessage(const char *role, const char *content) { (void)role; (void)content; }
void Display::SetTheme(Theme *theme) { current_theme_ = theme; }
void Display::UpdateStatusBar(bool update_all) { (void)update_all; }
void Display::SetPowerSaveMode(bool on) { (void)on; }
