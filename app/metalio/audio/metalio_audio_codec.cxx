/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * MetalioAudioCodec implementation — direct I2S audio for NuttX/openvela.
 *
 * Hardware is 32-bit I2S (CONFIG_ESPRESSIF_I2S0_DATA_BIT_WIDTH_32BIT),
 * matching MetalioClaw4 BTAudioCodec: capture int32 >> 12 → int16,
 * playback mono int16 → stereo int32 with software volume.
 *
 * Dialogue path matches Claw4: full-duplex EnableInput/EnableOutput (no
 * half-duplex mute races that left Listening with a dead mic after TTS).
 */

#include "metalio_audio_codec.h"
#include "esp_log_shim.h"
#include "settings.h"

#include <nuttx/audio/audio.h>
#include <sys/ioctl.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <climits>
#include <vector>

#define TAG "MetalioAudioCodec"

MetalioAudioCodec::MetalioAudioCodec(int input_sample_rate,
                                       int output_sample_rate)
{
    /* Full-duplex I2S: simultaneous mic capture + speaker playback */
    duplex_ = true;

    /* 2 input channels: mic + reference (for echo cancellation).
     * The AudioService de-interleaves these and feeds the reference
     * channel to the AEC/resampler pipeline. */
    input_reference_ = true;
    input_channels_  = 2;
    output_channels_ = 1;

    input_sample_rate_  = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    /* Default volume/gain — will be overridden by Settings on Start() */
    output_volume_ = 70;
    input_gain_    = 30.0f;
    RecalcVolumeFactor();
}

void MetalioAudioCodec::Start()
{
    Settings settings("audio", false);
    /* Prefer output_volume; fall back to settings-UI key "volume" so voice
     * MCP and the system volume slider stay on one value. */
    output_volume_ = settings.GetInt("output_volume", -1);
    if (output_volume_ < 0)
        output_volume_ = settings.GetInt("volume", 70);
    if (output_volume_ <= 0)
        output_volume_ = 10;
    if (output_volume_ > 100)
        output_volume_ = 100;
    RecalcVolumeFactor();

    OpenDevice();

    if (tx_fd_ >= 0 && output_sample_rate_ > 0)
        ConfigureDevice(AUDIO_TYPE_OUTPUT, output_sample_rate_,
                        output_channels_);
    if (rx_fd_ >= 0 && input_sample_rate_ > 0)
        ConfigureDevice(AUDIO_TYPE_INPUT, input_sample_rate_,
                        input_channels_);

    /* Match Claw4 AudioCodec::Start: both directions available; AudioService
     * arms input lazily via ReadAudioData when WW/VP need samples. */
    EnableInput(false);
    EnableOutput(false);
}

void MetalioAudioCodec::RecalcVolumeFactor()
{
    double vol = (double)output_volume_ / 100.0;
    if (vol < 0.0)
        vol = 0.0;
    if (vol > 1.0)
        vol = 1.0;
    /* L=R stereo doubles acoustic energy vs mono — scale ~0.72 so
     * feedback is less harsh without changing the UI volume number. */
    volume_factor_ = (int32_t)(std::pow(vol, 2.0) * 65536.0 * 0.72);
}

void MetalioAudioCodec::SetOutputVolume(int volume)
{
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    output_volume_ = volume;
    RecalcVolumeFactor();
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
    /* Keep settings-screen key in lockstep with voice MCP / codec. */
    settings.SetInt("volume", output_volume_);
    /* Software gain in Write() only — skip AUDIOIOC FU_VOLUME. */
}

void MetalioAudioCodec::EnableInput(bool enable)
{
    AudioCodec::EnableInput(enable);
}

void MetalioAudioCodec::EnableOutput(bool enable)
{
    AudioCodec::EnableOutput(enable);
}

int MetalioAudioCodec::OpenDevice()
{
    /* Open TX (playback) device: /dev/audio/pcm0 */
    if (tx_fd_ < 0)
    {
        tx_fd_ = open(METALIO_AUDIO_TX_DEV, O_WRONLY);
        if (tx_fd_ < 0)
        {
            ESP_LOGE(TAG, "Failed to open %s (errno=%d)",
                     METALIO_AUDIO_TX_DEV, errno);
            return -errno;
        }
    }

    /* Open RX (capture) device: /dev/audio/pcm_in0
     * NuttX registers the I2S input as a separate device. */
    if (duplex_ && rx_fd_ < 0)
    {
        rx_fd_ = open(METALIO_AUDIO_RX_DEV, O_RDONLY);
        if (rx_fd_ < 0)
        {
            ESP_LOGW(TAG, "Failed to open %s (errno=%d)",
                     METALIO_AUDIO_RX_DEV, errno);
            /* Not fatal: some configurations only need output */
        }
    }

    return 0;
}

int MetalioAudioCodec::ConfigureDevice(int type, int sample_rate, int channels)
{
    int fd = (type == AUDIO_TYPE_INPUT) ? rx_fd_ : tx_fd_;
    if (fd < 0)
    {
        ESP_LOGE(TAG, "ConfigureDevice: device not open (type=%d)", type);
        return -ENODEV;
    }

    struct audio_caps_s caps;
    memset(&caps, 0, sizeof(caps));
    caps.ac_len      = sizeof(struct audio_caps_s);
    caps.ac_type     = type;
    caps.ac_subtype  = AUDIO_FMT_PCM;
    /* App PCM is mono out; BT I2S link is stereo 32-bit (Claw4 Write). */
    int hw_channels = channels;
    if (type == AUDIO_TYPE_OUTPUT)
        hw_channels = 2;
    caps.ac_channels = hw_channels;
    /* NuttX packing: hw[0]+b[3] = rate, b[2] = bits per sample.
     * Keep 32-bit slots to match the BT audio codec / Claw4 path. */
    caps.ac_controls.hw[0] = (uint16_t)(sample_rate & 0xffff);
    caps.ac_controls.b[3]  = (uint8_t)((sample_rate >> 16) & 0xff);
    caps.ac_controls.b[2]  = 32;

    int ret = ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&caps);
    if (ret < 0)
    {
        ESP_LOGE(TAG, "AUDIOIOC_CONFIGURE failed: %d (errno=%d)", ret, errno);
        return ret;
    }

    ESP_LOGI(TAG, "Configured type=%d rate=%d ch=%d (hw_ch=%d) bits=32",
             type, sample_rate, channels, hw_channels);
    return 0;
}

int MetalioAudioCodec::Read(int16_t* dest, int samples)
{
    if (rx_fd_ < 0 || dest == nullptr || samples <= 0)
        return 0;

    if ((int)read_scratch_.size() < samples)
        read_scratch_.resize((size_t)samples);

    size_t total_bytes = (size_t)samples * sizeof(int32_t);
    size_t offset = 0;
    while (offset < total_bytes)
    {
        ssize_t n = read(rx_fd_, (uint8_t*)read_scratch_.data() + offset,
                         total_bytes - offset);
        if (n < 0)
        {
            ESP_LOGE(TAG, "Read: read() failed errno=%d", errno);
            return (int)(offset / sizeof(int32_t));
        }
        if (n == 0)
            break;
        offset += (size_t)n;
    }

    int got = (int)(offset / sizeof(int32_t));
    for (int i = 0; i < got; ++i)
    {
        int32_t value = read_scratch_[(size_t)i] >> 12;
        if (value > INT16_MAX)
            dest[i] = INT16_MAX;
        else if (value < -INT16_MAX)
            dest[i] = (int16_t)-INT16_MAX;
        else
            dest[i] = (int16_t)value;
    }
    return got;
}

int MetalioAudioCodec::Write(const int16_t* data, int samples)
{
    if (tx_fd_ < 0 || data == nullptr || samples <= 0)
        return 0;

    /* Mono PCM → stereo 32-bit left-aligned slots (Claw4 BTAudioCodec::Write).
     * volume_factor 0..65536 places the 16-bit sample in the upper half of each
     * 32-bit I2S word — requires I2S left_align=true (see esp_i2s.c). */
    const size_t need = (size_t)samples * 2;
    if (write_scratch_.size() < need)
        write_scratch_.resize(need);

    const int32_t volume_factor = volume_factor_;

    for (int i = 0; i < samples; ++i)
    {
        int64_t temp = (int64_t)data[i] * (int64_t)volume_factor;
        int32_t processed;
        if (temp > INT32_MAX)
            processed = INT32_MAX;
        else if (temp < INT32_MIN)
            processed = INT32_MIN;
        else
            processed = (int32_t)temp;

        write_scratch_[(size_t)i * 2]     = processed;
        write_scratch_[(size_t)i * 2 + 1] = processed;
    }

    size_t total_bytes = need * sizeof(int32_t);
    size_t offset = 0;
    while (offset < total_bytes)
    {
        ssize_t n = write(tx_fd_, (const uint8_t*)write_scratch_.data() + offset,
                          total_bytes - offset);
        if (n < 0)
        {
            ESP_LOGE(TAG, "Write: write() failed errno=%d", errno);
            return (int)(offset / (2 * sizeof(int32_t)));
        }
        if (n == 0)
            break;
        offset += (size_t)n;
    }
    return (int)(offset / (2 * sizeof(int32_t)));
}
