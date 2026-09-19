#pragma once

// ---------------------------------------------------------------------------
// ThemeManager — home icon pack id (theme1..theme4), persisted under "ui".
// Ported from MetalioClaw4 main/display/theme_manager.h.
// ---------------------------------------------------------------------------
namespace ThemeManager {

constexpr int kMinThemeId     = 1;
constexpr int kMaxThemeId     = 4;
constexpr int kThemeCount     = kMaxThemeId - kMinThemeId + 1;
constexpr int kDefaultThemeId = 1;

int GetCurrentThemeId();
void SetCurrentThemeId(int id);

}  // namespace ThemeManager
