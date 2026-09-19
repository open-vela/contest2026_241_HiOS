/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ThemeManager — ported from MetalioClaw4 main/display/theme_manager.cc.
 */

#include "theme_manager.h"
#include "settings.h"

namespace ThemeManager {
namespace {

constexpr const char *kSettingsNs = "ui";
constexpr const char *kKeyThemeId = "theme_id";

}  // namespace

int GetCurrentThemeId()
{
    Settings s(kSettingsNs, false);
    int id = s.GetInt(kKeyThemeId, kDefaultThemeId);
    if (id < kMinThemeId || id > kMaxThemeId) {
        id = kDefaultThemeId;
    }
    return id;
}

void SetCurrentThemeId(int id)
{
    if (id < kMinThemeId || id > kMaxThemeId) {
        return;
    }
    Settings s(kSettingsNs, true);
    s.SetInt(kKeyThemeId, id);
}

}  // namespace ThemeManager
