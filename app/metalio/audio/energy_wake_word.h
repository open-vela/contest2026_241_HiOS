/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Interim wake-word detector for NuttX until ESP-SR (AfeWakeWord /
 * wn9_nihaoxiaozhi) is ported.  MetalioClaw4 detects WakeNet then always
 * reports "Hi openvela" to the server — this class approximates that UX with a
 * near-field phrase-energy detector on the board mic.
 *
 * Also buffers recent mic PCM and encodes it to Opus on detect so
 * xiaozhi.me can play the wake clip (Claw4 CONFIG_SEND_WAKE_WORD_DATA).
 */

#ifndef ENERGY_WAKE_WORD_H
#define ENERGY_WAKE_WORD_H

#include "wake_word.h"
#include "opus_codec.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

class EnergyWakeWord : public WakeWord
{
public:
    static constexpr const char *kWakeWordText = "Hi openvela";

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override
    {
        (void)models_list;
        codec_ = codec;
        channels_ = (codec_ && codec_->input_channels() > 0)
                        ? codec_->input_channels()
                        : 1;
        ResetDetector();
        initialized_ = true;
        return true;
    }

    void Deinitialize() override
    {
        Stop();
        initialized_ = false;
        codec_ = nullptr;
    }

    void Feed(const std::vector<int16_t>& data) override
    {
        if (!running_ || data.empty())
            return;

        const size_t frames = data.size() / (size_t)channels_;
        if (frames == 0)
            return;

        /* Keep ~2 s of mono PCM for xiaozhi.me wake-clip playback. */
        StoreWakeWordPcm(data, frames);

        const auto now = std::chrono::steady_clock::now();
        if (now < armed_at_)
            return;

        int64_t sum_sq = 0;
        int32_t peak_abs = 0;
        for (size_t i = 0; i < frames; ++i)
        {
            int32_t s = data[i * (size_t)channels_];
            sum_sq += (int64_t)s * (int64_t)s;
            int32_t a = s < 0 ? -s : s;
            if (a > peak_abs)
                peak_abs = a;
        }
        const float rms = std::sqrt((float)sum_sq / (float)frames);

        if (noise_floor_ <= 0.0f)
            noise_floor_ = std::max(rms, 40.0f);
        else if (speech_frames_ == 0 && rms < noise_floor_ * 1.4f)
            noise_floor_ = noise_floor_ * 0.95f + rms * 0.05f;
        if (noise_floor_ > kNoiseFloorCap)
            noise_floor_ = kNoiseFloorCap;

        float thresh = std::max(noise_floor_ * 1.8f, (float)kAbsRmsMin);
        if (thresh > kThreshCap)
            thresh = (float)kThreshCap;

        const bool loud = (rms >= thresh) && (peak_abs >= kAbsPeakMin);

        if (loud)
        {
            if (speech_frames_ == 0)
            {
                if (pre_silence_frames_ < kPreSilenceFrames)
                    return;
            }
            ++speech_frames_;
            silence_frames_ = 0;
            if (rms > peak_rms_)
                peak_rms_ = rms;
            if (peak_abs > peak_abs_)
                peak_abs_ = peak_abs;

            if (speech_frames_ > kMaxSpeechFrames)
            {
                speech_frames_ = 0;
                silence_frames_ = 0;
                peak_rms_ = 0.0f;
                peak_abs_ = 0;
                pre_silence_frames_ = kPreSilenceFrames;
            }
        }
        else if (speech_frames_ > 0)
        {
            ++silence_frames_;
            if (silence_frames_ >= kSilenceFramesEnd)
            {
                MaybeFireWake(speech_frames_, peak_rms_, peak_abs_);
                speech_frames_ = 0;
                silence_frames_ = 0;
                peak_rms_ = 0.0f;
                peak_abs_ = 0;
                pre_silence_frames_ = kPreSilenceFrames;
            }
        }
        else
        {
            if (pre_silence_frames_ < 1000)
                ++pre_silence_frames_;
        }

        if (++log_counter_ >= 50)
        {
            log_counter_ = 0;
            char buf[96];
            int n = snprintf(buf, sizeof(buf),
                             "WAKE rms=%.0f nf=%.0f thr=%.0f sp=%d\n",
                             (double)rms, (double)noise_floor_,
                             (double)thresh, speech_frames_);
            if (n > 0)
                write(1, buf, (size_t)n);
        }
    }

    void OnWakeWordDetected(
        std::function<void(const std::string& wake_word)> callback) override
    {
        on_detected_ = std::move(callback);
    }

    void Start() override
    {
        ResetDetector();
        armed_at_ = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kArmDelayMs);
        running_ = true;
        write(1, "WW_ARM\n", 7);
    }

    void Stop() override
    {
        running_ = false;
        ResetDetector();
    }

    size_t GetFeedSize() override
    {
        /* 30 ms @ 16 kHz, per channel (AudioService multiplies by ch). */
        return 16000 * 30 / 1000;
    }

    void EncodeWakeWordData() override
    {
        if (encode_running_.exchange(true))
        {
            /* Prior encode still running — unblock any waiter. */
            std::lock_guard<std::mutex> lock(wake_word_mutex_);
            if (wake_word_opus_.empty())
            {
                wake_word_opus_.push_back(std::vector<uint8_t>());
                wake_word_cv_.notify_all();
            }
            return;
        }

        {
            std::lock_guard<std::mutex> lock(wake_word_mutex_);
            wake_word_opus_.clear();
        }

        std::thread([this]() {
            std::deque<std::vector<int16_t>> pcm;
            {
                std::lock_guard<std::mutex> lock(wake_word_mutex_);
                pcm.swap(wake_word_pcm_);
            }

            /* Flatten 30 ms chunks into 60 ms Opus frames (server expects
             * the same frame duration as the hello audio_params). */
            constexpr int kFrame = 16000 * 60 / 1000;
            auto encoder = std::make_unique<OpusEncoderWrapper>(16000, 1, 60);
            encoder->SetComplexity(0);

            std::vector<int16_t> acc;
            int packets = 0;
            auto encode_frame = [&](std::vector<int16_t>&& frame) {
                std::vector<uint8_t> opus;
                if (!encoder->Encode(std::move(frame), opus) || opus.empty())
                    return;
                std::lock_guard<std::mutex> lock(wake_word_mutex_);
                wake_word_opus_.push_back(std::move(opus));
                wake_word_cv_.notify_all();
                packets++;
            };

            for (auto& chunk : pcm)
            {
                acc.insert(acc.end(), chunk.begin(), chunk.end());
                while ((int)acc.size() >= kFrame)
                {
                    std::vector<int16_t> frame(acc.begin(),
                                               acc.begin() + kFrame);
                    acc.erase(acc.begin(), acc.begin() + kFrame);
                    encode_frame(std::move(frame));
                }
            }
            if (!acc.empty())
            {
                std::vector<int16_t> frame((size_t)kFrame, 0);
                std::copy(acc.begin(), acc.end(), frame.begin());
                encode_frame(std::move(frame));
            }

            {
                std::lock_guard<std::mutex> lock(wake_word_mutex_);
                wake_word_opus_.push_back(std::vector<uint8_t>()); /* end */
                wake_word_cv_.notify_all();
            }

            char buf[48];
            int n = snprintf(buf, sizeof(buf), "WW_ENC %d\n", packets);
            if (n > 0)
                write(1, buf, (size_t)n);
            encode_running_.store(false);
        }).detach();
    }

    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override
    {
        std::unique_lock<std::mutex> lock(wake_word_mutex_);
        /* NuttX cxx11_support wait_for has no predicate overload.
         * Cap wait so empty Energy buffer cannot hang first wake. */
        if (wake_word_opus_.empty())
            wake_word_cv_.wait_for(lock, std::chrono::milliseconds(300));
        if (wake_word_opus_.empty())
        {
            opus.clear();
            return false;
        }
        opus.swap(wake_word_opus_.front());
        wake_word_opus_.pop_front();
        return !opus.empty();
    }

    const std::string& GetLastDetectedWakeWord() const override
    {
        return last_wake_word_;
    }

private:
    /* ~30 ms frames. Energy is interim until EspWakeWord swaps in. */
    static constexpr int kAbsRmsMin = 80;
    static constexpr int kAbsPeakMin = 1200;
    static constexpr int kThreshCap = 600;
    static constexpr float kNoiseFloorCap = 120.0f;
    static constexpr int kMinSpeechFrames = 5;    /* ~150 ms */
    static constexpr int kMaxSpeechFrames = 45;   /* ~1.35 s */
    static constexpr int kSilenceFramesEnd = 3;   /* ~90 ms end silence */
    static constexpr int kPreSilenceFrames = 2;   /* ~60 ms leading quiet */
    static constexpr int kArmDelayMs = 500;
    static constexpr int kCooldownMs = 2000;
    static constexpr float kMinPeakRms = 200.0f;
    static constexpr size_t kMaxPcmChunks = 2000 / 30; /* ~2 s */

    void ResetDetector()
    {
        speech_frames_ = 0;
        silence_frames_ = 0;
        pre_silence_frames_ = 0;
        log_counter_ = 0;
        peak_rms_ = 0.0f;
        peak_abs_ = 0;
        noise_floor_ = 0.0f;
    }

    void StoreWakeWordPcm(const std::vector<int16_t>& data, size_t frames)
    {
        std::vector<int16_t> mono(frames);
        for (size_t i = 0; i < frames; ++i)
            mono[i] = data[i * (size_t)channels_];

        std::lock_guard<std::mutex> lock(wake_word_mutex_);
        wake_word_pcm_.push_back(std::move(mono));
        while (wake_word_pcm_.size() > kMaxPcmChunks)
            wake_word_pcm_.pop_front();
    }

    void MaybeFireWake(int speech_frames, float peak_rms, int32_t peak_abs)
    {
        /* Interim near-field energy wake until EspWakeWord swaps in.
         * Without this, the first「Hi openvela」before WW_ESP_OK is silently
         * dropped (Energy was muted + Esp deferred → no voice wake). */
        if (speech_frames < kMinSpeechFrames)
            return;
        if (speech_frames > kMaxSpeechFrames)
            return;
        if (peak_rms < kMinPeakRms)
            return;
        if (peak_abs < kAbsPeakMin)
            return;

        const auto now = std::chrono::steady_clock::now();
        if (now - last_fire_ < std::chrono::milliseconds(kCooldownMs))
            return;

        last_fire_ = now;
        last_wake_word_ = kWakeWordText;

        char buf[96];
        int n = snprintf(buf, sizeof(buf),
                         "WAKE hit \"%s\" frames=%d peak=%.0f abs=%ld\n",
                         kWakeWordText, speech_frames, (double)peak_rms,
                         (long)peak_abs);
        if (n > 0)
            write(1, buf, (size_t)n);

        if (on_detected_)
            on_detected_(last_wake_word_);
    }

    AudioCodec* codec_ = nullptr;
    int channels_ = 1;
    bool initialized_ = false;
    bool running_ = false;
    float noise_floor_ = 0.0f;
    float peak_rms_ = 0.0f;
    int32_t peak_abs_ = 0;
    int speech_frames_ = 0;
    int silence_frames_ = 0;
    int pre_silence_frames_ = 0;
    int log_counter_ = 0;
    std::string last_wake_word_;
    std::chrono::steady_clock::time_point last_fire_{};
    std::chrono::steady_clock::time_point armed_at_{};
    std::function<void(const std::string& wake_word)> on_detected_;

    std::deque<std::vector<int16_t>> wake_word_pcm_;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;
    std::atomic<bool> encode_running_{false};
};

#endif /* ENERGY_WAKE_WORD_H */
