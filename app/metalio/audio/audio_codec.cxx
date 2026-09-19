/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * AudioCodec base implementation for NuttX/openvela.
 *
 * Replaces the ESP-IDF I2S + esp_codec_dev layer with the NuttX audio
 * character-device subsystem.  PCM data flows through /dev/audio/pcm0
 * using read()/write(); format, sample-rate and volume are configured
 * via the AUDIOIOC_* ioctls.
 */

#include "audio_codec.h"
#include "esp_log_shim.h"
#include "esp_err_shim.h"
#include "settings.h"

#include <nuttx/audio/audio.h>
#include <sys/ioctl.h>

#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec()
{
}

AudioCodec::~AudioCodec()
{
    CloseDevice();
}

/* ------------------------------------------------------------------ */
/* NuttX audio device management                                       */
/* ------------------------------------------------------------------ */

int AudioCodec::OpenDevice()
{
    if (tx_fd_ < 0)
    {
        tx_fd_ = open(AUDIO_DEV_PATH, O_WRONLY);
        if (tx_fd_ < 0)
        {
            ESP_LOGE(TAG, "Failed to open %s for write: %d", AUDIO_DEV_PATH, errno);
            return -errno;
        }
        ESP_LOGI(TAG, "Opened %s (tx fd=%d)", AUDIO_DEV_PATH, tx_fd_);
    }

    if (duplex_ && rx_fd_ < 0)
    {
        rx_fd_ = open(AUDIO_DEV_PATH, O_RDONLY);
        if (rx_fd_ < 0)
        {
            ESP_LOGE(TAG, "Failed to open %s for read: %d", AUDIO_DEV_PATH, errno);
            /* Not fatal — some boards only support output */
        }
        else
        {
            ESP_LOGI(TAG, "Opened %s (rx fd=%d)", AUDIO_DEV_PATH, rx_fd_);
        }
    }

    return 0;
}

void AudioCodec::CloseDevice()
{
    if (tx_fd_ >= 0)
    {
        close(tx_fd_);
        tx_fd_ = -1;
    }
    if (rx_fd_ >= 0)
    {
        close(rx_fd_);
        rx_fd_ = -1;
    }
}

int AudioCodec::ConfigureDevice(int type, int sample_rate, int channels)
{
    int fd = (type == AUDIO_TYPE_INPUT) ? rx_fd_ : tx_fd_;
    if (fd < 0)
    {
        ESP_LOGE(TAG, "ConfigureDevice: device not open (type=%d)", type);
        return -ENODEV;
    }

    struct audio_caps_s caps;
    memset(&caps, 0, sizeof(caps));
    caps.ac_len     = sizeof(struct audio_caps_s);
    caps.ac_type    = type;
    caps.ac_subtype = AUDIO_FMT_PCM;
    caps.ac_channels = channels;
    caps.ac_format.b[0] = AUDIO_SUBFMT_PCM_S16_LE;
    caps.ac_controls.w  = (uint32_t)sample_rate;

    int ret = ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&caps);
    if (ret < 0)
    {
        ESP_LOGE(TAG, "AUDIOIOC_CONFIGURE failed: %d (errno=%d)", ret, errno);
        return ret;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Low-level I/O                                                      */
/* ------------------------------------------------------------------ */

int AudioCodec::Read(int16_t* dest, int samples)
{
    if (rx_fd_ < 0)
    {
        ESP_LOGE(TAG, "Read: rx device not open");
        return 0;
    }

    size_t total_bytes = samples * sizeof(int16_t);
    size_t offset = 0;
    while (offset < total_bytes)
    {
        ssize_t n = read(rx_fd_, (uint8_t*)dest + offset,
                         total_bytes - offset);
        if (n < 0)
        {
            ESP_LOGE(TAG, "Read: read() failed: %d (errno=%d)", (int)n, errno);
            return (int)(offset / sizeof(int16_t));
        }
        if (n == 0)
            break;
        offset += (size_t)n;
    }
    return (int)(offset / sizeof(int16_t));
}

int AudioCodec::Write(const int16_t* data, int samples)
{
    if (tx_fd_ < 0)
    {
        ESP_LOGE(TAG, "Write: tx device not open");
        return 0;
    }

    size_t total_bytes = samples * sizeof(int16_t);
    size_t offset = 0;
    while (offset < total_bytes)
    {
        ssize_t n = write(tx_fd_, (const uint8_t*)data + offset,
                          total_bytes - offset);
        if (n < 0)
        {
            ESP_LOGE(TAG, "Write: write() failed: %d (errno=%d)", (int)n, errno);
            return (int)(offset / sizeof(int16_t));
        }
        if (n == 0)
            break;
        offset += (size_t)n;
    }
    return (int)(offset / sizeof(int16_t));
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void AudioCodec::OutputData(std::vector<int16_t>& data)
{
    Write(data.data(), (int)data.size());
}

void AudioCodec::OutputData(const int16_t* data, int samples)
{
    Write(data, samples);
}

bool AudioCodec::InputData(std::vector<int16_t>& data)
{
    int samples = Read(data.data(), (int)data.size());
    return samples > 0;
}

void AudioCodec::Start()
{
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0)
    {
        ESP_LOGW(TAG, "Output volume value (%d) too small, setting to default (10)",
                 output_volume_);
        output_volume_ = 10;
    }

    OpenDevice();

    /* Configure output path */
    if (tx_fd_ >= 0 && output_sample_rate_ > 0)
    {
        ConfigureDevice(AUDIO_TYPE_OUTPUT, output_sample_rate_,
                        output_channels_);
    }

    /* Configure input path */
    if (rx_fd_ >= 0 && input_sample_rate_ > 0)
    {
        ConfigureDevice(AUDIO_TYPE_INPUT, input_sample_rate_,
                        input_channels_);
    }

    EnableInput(true);
    EnableOutput(true);
}

void AudioCodec::SetOutputVolume(int volume)
{
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);

    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);

    /* Apply via NuttX audio FEATURE ioctl (0..1000 scale) */
    if (tx_fd_ >= 0)
    {
        struct audio_caps_s caps;
        memset(&caps, 0, sizeof(caps));
        caps.ac_len     = sizeof(struct audio_caps_s);
        caps.ac_type    = AUDIO_TYPE_FEATURE;
        caps.ac_subtype = AUDIO_FU_VOLUME;
        /* Map 0..100 to 0..1000 */
        caps.ac_controls.w = (uint32_t)(volume * 10);
        int ret = ioctl(tx_fd_, AUDIOIOC_CONFIGURE,
                        (unsigned long)(uintptr_t)&caps);
        if (ret < 0)
            ESP_LOGW(TAG, "SetOutputVolume ioctl failed: %d", ret);
    }
}

void AudioCodec::SetInputGain(float gain)
{
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);

    /* Apply via NuttX audio FEATURE ioctl if supported */
    if (rx_fd_ >= 0)
    {
        struct audio_caps_s caps;
        memset(&caps, 0, sizeof(caps));
        caps.ac_len     = sizeof(struct audio_caps_s);
        caps.ac_type    = AUDIO_TYPE_FEATURE;
        caps.ac_subtype = AUDIO_FU_INP_GAIN;
        caps.ac_controls.w = (uint32_t)(gain * 10);
        ioctl(rx_fd_, AUDIOIOC_CONFIGURE,
              (unsigned long)(uintptr_t)&caps);
    }
}

void AudioCodec::SetMute(bool mute)
{
    muted_ = mute;
    ESP_LOGI(TAG, "Set mute to %s", mute ? "true" : "false");

    if (tx_fd_ >= 0)
    {
        struct audio_caps_s caps;
        memset(&caps, 0, sizeof(caps));
        caps.ac_len     = sizeof(struct audio_caps_s);
        caps.ac_type    = AUDIO_TYPE_FEATURE;
        caps.ac_subtype = AUDIO_FU_MUTE;
        caps.ac_controls.w = mute ? 1 : 0;
        ioctl(tx_fd_, AUDIOIOC_CONFIGURE,
              (unsigned long)(uintptr_t)&caps);
    }
}

void AudioCodec::EnableInput(bool enable)
{
    if (enable == input_enabled_)
        return;

    input_enabled_ = enable;

    if (enable && rx_fd_ >= 0)
    {
        int ret = ioctl(rx_fd_, AUDIOIOC_START, 0);
        if (ret < 0)
            ESP_LOGW(TAG, "AUDIOIOC_START (input) failed: %d", ret);
    }
    else if (!enable && rx_fd_ >= 0)
    {
        ioctl(rx_fd_, AUDIOIOC_STOP, 0);
    }
}

void AudioCodec::EnableOutput(bool enable)
{
    if (enable == output_enabled_)
        return;

    output_enabled_ = enable;

    if (enable && tx_fd_ >= 0)
    {
        int ret = ioctl(tx_fd_, AUDIOIOC_START, 0);
        if (ret < 0)
            ESP_LOGW(TAG, "AUDIOIOC_START (output) failed: %d", ret);
    }
    else if (!enable && tx_fd_ >= 0)
    {
        ioctl(tx_fd_, AUDIOIOC_STOP, 0);
    }
}
