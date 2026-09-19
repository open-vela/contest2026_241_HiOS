/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Opus encode/decode wrappers for AudioService — same surface as
 * MetalioClaw4's esp-opus-encoder OpusEncoderWrapper / OpusDecoderWrapper,
 * implemented with esp_audio_codec's esp_opus_enc / esp_opus_dec
 * (CONFIG_METALIO_GMF_CODEC).
 */

#ifndef METALIO_OPUS_CODEC_H
#define METALIO_OPUS_CODEC_H

#include <cstdint>
#include <mutex>
#include <vector>

class OpusEncoderWrapper
{
public:
    OpusEncoderWrapper(int sample_rate, int channels, int frame_duration_ms);
    ~OpusEncoderWrapper();

    OpusEncoderWrapper(const OpusEncoderWrapper&) = delete;
    OpusEncoderWrapper& operator=(const OpusEncoderWrapper&) = delete;

    void SetComplexity(int complexity);
    bool Encode(std::vector<int16_t>&& pcm, std::vector<uint8_t>& payload);

    int sample_rate() const { return sample_rate_; }

private:
    std::mutex mutex_;
    void* enc_ = nullptr;
    int sample_rate_ = 0;
    int channels_ = 0;
    int duration_ms_ = 0;
    int frame_samples_ = 0;
    int out_buf_size_ = 0;
};

class OpusDecoderWrapper
{
public:
    OpusDecoderWrapper(int sample_rate, int channels, int frame_duration_ms);
    ~OpusDecoderWrapper();

    OpusDecoderWrapper(const OpusDecoderWrapper&) = delete;
    OpusDecoderWrapper& operator=(const OpusDecoderWrapper&) = delete;

    bool Decode(std::vector<uint8_t>&& payload, std::vector<int16_t>& pcm);
    void ResetState();

    int sample_rate() const { return sample_rate_; }
    int duration_ms() const { return duration_ms_; }

private:
    std::mutex mutex_;
    void* dec_ = nullptr;
    int sample_rate_ = 0;
    int channels_ = 0;
    int duration_ms_ = 0;
    int frame_samples_ = 0;
};

#endif /* METALIO_OPUS_CODEC_H */
