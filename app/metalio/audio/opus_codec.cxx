/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 */

#include "opus_codec.h"

#include "esp_log_shim.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

#include <cstring>

namespace
{

constexpr const char* TAG = "OpusCodec";

esp_opus_dec_frame_duration_t DecFrameDuration(int ms)
{
    switch (ms)
    {
    case 5:
        return ESP_OPUS_DEC_FRAME_DURATION_5_MS;
    case 10:
        return ESP_OPUS_DEC_FRAME_DURATION_10_MS;
    case 20:
        return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    case 40:
        return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
    case 60:
        return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    case 80:
        return ESP_OPUS_DEC_FRAME_DURATION_80_MS;
    case 100:
        return ESP_OPUS_DEC_FRAME_DURATION_100_MS;
    case 120:
        return ESP_OPUS_DEC_FRAME_DURATION_120_MS;
    default:
        return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    }
}

esp_opus_enc_frame_duration_t EncFrameDuration(int ms)
{
    switch (ms)
    {
    case 5:
        return ESP_OPUS_ENC_FRAME_DURATION_5_MS;
    case 10:
        return ESP_OPUS_ENC_FRAME_DURATION_10_MS;
    case 20:
        return ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    case 40:
        return ESP_OPUS_ENC_FRAME_DURATION_40_MS;
    case 60:
        return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    case 80:
        return ESP_OPUS_ENC_FRAME_DURATION_80_MS;
    case 100:
        return ESP_OPUS_ENC_FRAME_DURATION_100_MS;
    case 120:
        return ESP_OPUS_ENC_FRAME_DURATION_120_MS;
    default:
        return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    }
}

} // namespace

OpusEncoderWrapper::OpusEncoderWrapper(int sample_rate, int channels,
                                       int frame_duration_ms)
    : sample_rate_(sample_rate), channels_(channels),
      duration_ms_(frame_duration_ms)
{
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = sample_rate;
    cfg.channel = (uint8_t)channels;
    cfg.bits_per_sample = ESP_AUDIO_BIT16;
    cfg.bitrate = 24000;
    cfg.frame_duration = EncFrameDuration(frame_duration_ms);
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    /* Higher complexity improves cloud STT clarity vs silence/noise. */
    cfg.complexity = 5;
    /* DTX sends comfort-noise frames the cloud often ignores for ASR — keep
     * continuous Opus so listen/start speech is recognized. */
    cfg.enable_dtx = false;
    cfg.enable_vbr = true;

    esp_audio_err_t err =
        esp_opus_enc_open(&cfg, sizeof(cfg), &enc_);
    if (err != ESP_AUDIO_ERR_OK || enc_ == nullptr)
    {
        ESP_LOGE(TAG, "esp_opus_enc_open failed err=%d", (int)err);
        enc_ = nullptr;
        return;
    }

    int in_size = 0;
    int out_size = 0;
    if (esp_opus_enc_get_frame_size(enc_, &in_size, &out_size) ==
        ESP_AUDIO_ERR_OK)
    {
        frame_samples_ = in_size / (int)sizeof(int16_t);
        out_buf_size_ = out_size > 0 ? out_size : 4000;
    }
    else
    {
        frame_samples_ = sample_rate / 1000 * channels * frame_duration_ms;
        out_buf_size_ = 4000;
    }

    ESP_LOGE(TAG, "encoder ok rate=%d ch=%d dur=%d frame=%d", sample_rate,
             channels, frame_duration_ms, frame_samples_);
}

OpusEncoderWrapper::~OpusEncoderWrapper()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (enc_ != nullptr)
    {
        esp_opus_enc_close(enc_);
        enc_ = nullptr;
    }
}

void OpusEncoderWrapper::SetComplexity(int complexity)
{
    std::lock_guard<std::mutex> lock(mutex_);
    (void)complexity;
    /* esp_opus_enc has no live complexity ctl; set at open. */
}

bool OpusEncoderWrapper::Encode(std::vector<int16_t>&& pcm,
                                std::vector<uint8_t>& payload)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (enc_ == nullptr)
        return false;

    if ((int)pcm.size() != frame_samples_)
    {
        ESP_LOGE(TAG, "encode size %u != frame %d", (unsigned)pcm.size(),
                 frame_samples_);
        return false;
    }

    std::vector<uint8_t> out((size_t)out_buf_size_);
    esp_audio_enc_in_frame_t in_frame = {};
    in_frame.buffer = (uint8_t*)pcm.data();
    in_frame.len = (uint32_t)(pcm.size() * sizeof(int16_t));

    esp_audio_enc_out_frame_t out_frame = {};
    out_frame.buffer = out.data();
    out_frame.len = (uint32_t)out.size();

    esp_audio_err_t err = esp_opus_enc_process(enc_, &in_frame, &out_frame);
    if (err != ESP_AUDIO_ERR_OK)
    {
        ESP_LOGE(TAG, "esp_opus_enc_process failed err=%d", (int)err);
        return false;
    }

    payload.assign(out.data(), out.data() + out_frame.encoded_bytes);
    return true;
}

OpusDecoderWrapper::OpusDecoderWrapper(int sample_rate, int channels,
                                       int frame_duration_ms)
    : sample_rate_(sample_rate), channels_(channels),
      duration_ms_(frame_duration_ms)
{
    frame_samples_ = sample_rate / 1000 * channels * frame_duration_ms;

    esp_opus_dec_cfg_t cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    cfg.sample_rate = (uint32_t)sample_rate;
    cfg.channel = (uint8_t)channels;
    cfg.frame_duration = DecFrameDuration(frame_duration_ms);
    cfg.self_delimited = false;

    esp_audio_err_t err =
        esp_opus_dec_open(&cfg, sizeof(cfg), &dec_);
    if (err != ESP_AUDIO_ERR_OK || dec_ == nullptr)
    {
        ESP_LOGE(TAG, "esp_opus_dec_open failed err=%d", (int)err);
        dec_ = nullptr;
        return;
    }

    ESP_LOGE(TAG, "decoder ok rate=%d ch=%d dur=%d", sample_rate, channels,
             frame_duration_ms);
}

OpusDecoderWrapper::~OpusDecoderWrapper()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (dec_ == nullptr)
        return;
    esp_opus_dec_close(dec_);
    dec_ = nullptr;
}

bool OpusDecoderWrapper::Decode(std::vector<uint8_t>&& payload,
                                std::vector<int16_t>& pcm)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (dec_ == nullptr || payload.empty())
        return false;

    /* Output buffer: one frame of PCM (16-bit) with headroom. */
    size_t bytes_needed =
        (size_t)frame_samples_ * sizeof(int16_t);
    if (bytes_needed < 4096)
        bytes_needed = 4096;

    std::vector<uint8_t> out_buf(bytes_needed);

    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = payload.data();
    raw.len = (uint32_t)payload.size();
    raw.consumed = 0;
    raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

    esp_audio_dec_out_frame_t frame = {};
    frame.buffer = out_buf.data();
    frame.len = (uint32_t)out_buf.size();
    frame.needed_size = 0;
    frame.decoded_size = 0;

    esp_audio_dec_info_t info = {};
    esp_audio_err_t err =
        esp_opus_dec_decode(dec_, &raw, &frame, &info);

    if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > 0)
    {
        out_buf.resize(frame.needed_size);
        frame.buffer = out_buf.data();
        frame.len = (uint32_t)out_buf.size();
        frame.decoded_size = 0;
        raw.buffer = payload.data();
        raw.len = (uint32_t)payload.size();
        raw.consumed = 0;
        err = esp_opus_dec_decode(dec_, &raw, &frame, &info);
    }

    if (err != ESP_AUDIO_ERR_OK)
    {
        ESP_LOGE(TAG, "esp_opus_dec_decode failed err=%d", (int)err);
        return false;
    }

    size_t samples = frame.decoded_size / sizeof(int16_t);
    pcm.resize(samples);
    if (samples > 0)
        std::memcpy(pcm.data(), out_buf.data(), samples * sizeof(int16_t));
    return samples > 0;
}

void OpusDecoderWrapper::ResetState()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (dec_ != nullptr)
        esp_opus_dec_reset(dec_);
}
