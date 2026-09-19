/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * UI home bootstrap placeholder.
 *
 * The real Application::Start() (in application.cxx) owns the full LVGL
 * screen stack via the Display framework in display/.  This file is kept
 * as a thin, self-contained hook so that boards or tools that want to
 * drive the home screen independently can call metalio::ui_home_start()
 * before/without the full application loop.
 *
 * Wire this to Display / ScreenManager once the LVGL screen port lands.
 */

#include <cstdio>

namespace metalio {

void ui_home_start()
{
    std::printf("UI: home screen (LVGL binding via /dev/fb0 + /dev/input0)\n");
    std::printf("UI: apps Chat / OpenClaw / Settings / Network / Camera ...\n");
}

} // namespace metalio
