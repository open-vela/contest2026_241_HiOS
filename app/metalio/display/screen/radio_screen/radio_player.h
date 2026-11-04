/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * RadioPlayer — HTTP/HTTPS HLS (m3u8) and MP3 live radio for HiOS.
 *
 * Owns stream playback, paced I2S output via AudioService (not pcm0),
 * lightweight spectrum levels, and soft system-audio occupancy.
 */

#pragma once

#include "radio_stations.h"

#include <cstdint>

enum class RadioStatus : uint8_t {
    Idle = 0,
    Connecting,
    Playing,
    Paused,
    Failed,
};

class RadioPlayer
{
public:
    static constexpr int kBandCount = 12;
    static constexpr int kSampleRate = 16000;
    static constexpr int kVolumeStep = 5;

    static RadioPlayer &Instance();

    RadioPlayer(const RadioPlayer &) = delete;
    RadioPlayer &operator=(const RadioPlayer &) = delete;

    /* Occupy system audio and start (or resume) the current station. */
    void Start();
    /* Stop playback and restore wake-word / voice processing. */
    void Stop();
    /* Connecting too long with no PCM — fail once, no retry storm. */
    void AbortConnectTimeout();
    void Pause();
    void Resume();
    void TogglePlayPause();
    void SelectStation(int index);

    RadioStatus status() const;
    bool want_play() const;
    bool is_session_running() const;
    int station_index() const;
    const RadioStation &station() const;
    const char *stream_url() const;

    /* 12-band levels 0..255 (lightweight energy bands from PCM). */
    void CopyBands(uint8_t out[kBandCount]) const;

    int volume() const;
    void AdjustVolume(int delta);
#ifdef HI_STABILITY_PROBE
    struct ProbeStats { uint32_t produced,submitted,rebuffer;size_t buffered; };
    ProbeStats GetProbeStats() const;
#endif

private:
    RadioPlayer();
};
