/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * DigitalPeoplePrefs — ported from MetalioClaw4
 * main/display/screen/digital_people_screen/digital_people_prefs.cc.
 *
 * Digital-people emotion resource format and EAF frame-interval
 * persistence. Uses the file-based Settings class (NVS replacement).
 */

#pragma once

#include <cstdint>

namespace DigitalPeoplePrefs
{

enum class EmotionFormat : int
{
    Sjpg = 0,
    Eaf = 1,
};

constexpr uint32_t kDefaultFrameDelayMs = 30;
constexpr uint32_t kMinFrameDelayMs = 10;
constexpr uint32_t kMaxFrameDelayMs = 500;

EmotionFormat GetEmotionFormat();
void SetEmotionFormat(EmotionFormat format);

const char *GetEmotionExt();
bool UsesEaf();

uint32_t GetFrameDelayMs();
void SetFrameDelayMs(uint32_t delay_ms);

} // namespace DigitalPeoplePrefs
