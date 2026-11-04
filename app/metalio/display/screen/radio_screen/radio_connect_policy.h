#pragma once
#include <cstdint>

// A slow, progressing cellular download is different from a stalled stream.
// Keep Wi-Fi's existing deadlines and bound every cellular recovery window.
inline bool RadioProgressExpired(bool cellular, bool playing, uint32_t now,
                                uint32_t started, uint32_t decoded,
                                uint32_t submitted)
{
    const uint32_t age = now - (playing ? submitted : started);
    if (!cellular) return age >= (playing ? 12000u : 20000u);
    return age >= 60000u || (age >= 20000u && now - decoded >= 20000u);
}
