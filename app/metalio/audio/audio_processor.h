/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Audio processor abstract interface.
 *
 * Ported from the ESP-IDF MetalioClaw4 reference.  The original
 * implementation uses ESP-IDF AFE (Audio Front End) for AEC, NS and VAD.
 * On NuttX/openvela these DSP stages will be provided by a different
 * pipeline; the abstract interface below is intentionally identical so
 * that concrete processors can be dropped in later.
 */

#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <cmath>
#include <climits>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "audio_codec.h"

class AudioProcessor
{
public:
    virtual ~AudioProcessor() = default;

    virtual void Initialize(AudioCodec* codec, int frame_duration_ms,
                            srmodel_list_t* models_list) = 0;
    virtual void Feed(std::vector<int16_t>&& data) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() = 0;

    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;

    virtual size_t GetFeedSize() = 0;
    virtual void EnableDeviceAec(bool enable) = 0;
};

/*
 * No-op audio processor — used when voice processing (AEC / NS / VAD) is
 * not available.  Matches MetalioClaw4 no_audio_processor.cc: forward
 * frames to the encode queue, de-interleaving mic+ref to mono first.
 */
class NoAudioProcessor : public AudioProcessor
{
public:
    void Initialize(AudioCodec* codec, int frame_duration_ms,
                    srmodel_list_t* models_list) override
    {
        (void)models_list;
        codec_ = codec;
        if (frame_duration_ms > 0)
            frame_duration_ms_ = frame_duration_ms;
        initialized_ = true;
    }

    void Feed(std::vector<int16_t>&& data) override
    {
        if (!running_)
            return;

        /* Mic+ref interleaved → mono. Prefer an exact stereo frame; if the
         * caller already de-interleaved (size == GetFeedSize()), pass through. */
        const size_t mono_frame = GetFeedSize();
        std::vector<int16_t> mono;
        const int16_t *samples = data.data();
        size_t n = data.size();
        if (codec_ && codec_->input_channels() == 2 &&
            data.size() >= 2 && (data.size() % 2) == 0)
        {
            const size_t frames = data.size() / 2;
            /* Pick the louder I2S slot as mic every frame. Voting hysteresis
             * kept the wrong (noise/ref) channel for ~180ms and hurt STT. */
            int64_t e0 = 0;
            int64_t e1 = 0;
            for (size_t i = 0; i < frames; ++i)
            {
                const int32_t a0 = data[i * 2];
                const int32_t a1 = data[i * 2 + 1];
                e0 += (a0 < 0 ? -a0 : a0);
                e1 += (a1 < 0 ? -a1 : a1);
            }
            mic_slot_ = (e1 > e0) ? 1 : 0;
            mic_slot_votes_ = 0;

            mono.resize(frames);
            for (size_t i = 0; i < frames; ++i)
                mono[i] = data[i * 2 + (size_t)mic_slot_];

            /* Light DC/rumble HPF + mild gain. 3/2 clipped peaks and made
             * cloud STT less accurate; 5/4 is enough for quiet speech. */
            constexpr int kGainNum = 5;
            constexpr int kGainDen = 4;
            int32_t prev_x = hp_x_;
            int32_t prev_y = hp_y_;
            for (size_t i = 0; i < frames; ++i)
            {
                const int32_t x = mono[i];
                /* y = 0.95*(y + x - x_prev) ≈ 80–100 Hz @ 16 kHz */
                int32_t y = (prev_y * 15 + (x - prev_x) * 16) / 16;
                prev_x = x;
                prev_y = y;
                int32_t g = (y * kGainNum) / kGainDen;
                if (g > INT16_MAX)
                    g = INT16_MAX;
                else if (g < -INT16_MAX)
                    g = -INT16_MAX;
                mono[i] = (int16_t)g;
            }
            hp_x_ = prev_x;
            hp_y_ = prev_y;

            /* Opus expects exactly one frame; trim/pad to mono_frame. */
            if (mono.size() > mono_frame)
                mono.resize(mono_frame);
            else if (mono.size() < mono_frame)
                mono.resize(mono_frame, 0);

            samples = mono.data();
            n = mono.size();
        }

        /* Lightweight energy VAD (no AFE) so AutoStop / UI see speech edges
         * like Xiaozhi while cloud still owns endpointing. */
        if (on_vad_ && n > 0)
        {
            int64_t sum_sq = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const int32_t s = samples[i];
                sum_sq += (int64_t)s * (int64_t)s;
            }
            const float rms = std::sqrt((float)sum_sq / (float)n);
            /* Match post-gain levels; was 120 before soft gain. */
            const bool loud = rms >= 180.0f;
            if (loud)
            {
                silence_hang_ = 0;
                if (!vad_speaking_)
                {
                    vad_speaking_ = true;
                    on_vad_(true);
                }
            }
            else if (vad_speaking_)
            {
                if (++silence_hang_ >= 10) /* ~600 ms — avoid cutting mid-phrase */
                {
                    vad_speaking_ = false;
                    silence_hang_ = 0;
                    on_vad_(false);
                }
            }
        }

        if (!on_output_)
            return;
        if (!mono.empty())
            on_output_(std::move(mono));
        else
            on_output_(std::move(data));
    }

    void Start() override
    {
        vad_speaking_ = false;
        silence_hang_ = 0;
        hp_x_ = 0;
        hp_y_ = 0;
        running_ = true;
    }
    void Stop() override
    {
        running_ = false;
        if (vad_speaking_ && on_vad_)
            on_vad_(false);
        vad_speaking_ = false;
        silence_hang_ = 0;
    }
    bool IsRunning() override { return running_; }

    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override
    {
        on_output_ = std::move(callback);
    }

    void OnVadStateChange(std::function<void(bool speaking)> callback) override
    {
        on_vad_ = std::move(callback);
    }

    size_t GetFeedSize() override { return 16000 * frame_duration_ms_ / 1000; }
    void EnableDeviceAec(bool enable) override { (void)enable; }

private:
    AudioCodec* codec_ = nullptr;
    bool initialized_ = false;
    bool running_ = false;
    bool vad_speaking_ = false;
    int silence_hang_ = 0;
    int mic_slot_ = 0;       /* 0=L / 1=R — auto-picked by energy */
    int mic_slot_votes_ = 0;
    int32_t hp_x_ = 0;
    int32_t hp_y_ = 0;
    int frame_duration_ms_ = 60;
    std::function<void(std::vector<int16_t>&&)> on_output_;
    std::function<void(bool)> on_vad_;
};

#endif /* AUDIO_PROCESSOR_H */
