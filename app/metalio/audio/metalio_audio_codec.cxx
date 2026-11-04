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
#include "bluetooth_screen/bluetooth_screen.h"

#include <nuttx/audio/audio.h>
#include <sys/ioctl.h>

#include <cstdio>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <climits>
#include <vector>

extern "C" {
#include <metalio/metalio.h>
}

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
    Settings network("network", false);
    const int sound_mode = network.GetInt("sound_mode", 0);
    if (sound_mode == 1 || sound_mode == 2) {
        output_volume_ = 0;
    } else {
        // The shade keeps the last audible volume separately from mute.
        output_volume_ = network.GetInt("ring_volume", output_volume_);
        if (output_volume_ < 0) output_volume_ = 70;
        if (output_volume_ > 100) output_volume_ = 100;
    }
    RecalcVolumeFactor();

    /* Defer OpenDevice / I2S until first EnableInput/EnableOutput so SDIO
     * Wi-Fi bring-up and HomeScreen LVGL do not race the speaker path
     * (solid-blue frame after enter home). */
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
    volume_factor_.store((int32_t)(std::pow(vol, 2.0) * 65536.0 * 0.72),
                         std::memory_order_release);
}

void MetalioAudioCodec::SetOutputVolume(int volume)
{
    SetOutputVolume(volume, true);
}

void MetalioAudioCodec::SetOutputVolume(int volume, bool persist)
{
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    output_volume_ = volume;
    RecalcVolumeFactor();
    if (!persist)
        return;
    /* Persist only on release / explicit save. */
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
    /* Keep settings-screen key in lockstep with voice MCP / codec. */
    settings.SetInt("volume", output_volume_);
    /* Software gain in Write() only — skip AUDIOIOC FU_VOLUME. */
}

void MetalioAudioCodec::EnsureDirStarted(bool input)
{
    std::lock_guard<std::mutex> lock(device_mutex_);
    // Microphone and speaker share the BT codec's supply. A saved Bluetooth
    // power-off state must not leave local recording/playback without clocks.
    if (!metalio_bt_is_powered()) {
        if (metalio_bt_power(true) < 0) return;
        usleep(1200000);
    }
    /* Music mode 3 drops I2S voice clocks. Re-enter mode 1 before mic or
     * speaker use. A one-shot flag here used to skip this forever after
     * the first open, so chat capture and TTS stayed dead. */
    if (!metalio_bt_voice_ready() && metalio_bt_ensure_mode1() < 0) return;

    if (input)
    {
        if (rx_fd_ < 0)
        {
            OpenDevice();

        }
        if (!rx_started_ && rx_fd_ >= 0)
        {
            if (ConfigureDevice(AUDIO_TYPE_INPUT, input_sample_rate_, input_channels_) == 0)
                rx_started_ = ioctl(rx_fd_, AUDIOIOC_START, 0) == 0;
        }
        return;
    }
    if (tx_fd_ < 0)
    {
        OpenDevice();

    }
    if (!tx_started_ && tx_fd_ >= 0)
    {
        if (ConfigureDevice(AUDIO_TYPE_OUTPUT, output_sample_rate_, output_channels_) == 0)
            tx_started_ = ioctl(tx_fd_, AUDIOIOC_START, 0) == 0;
    }
}

void MetalioAudioCodec::EnableInput(bool enable)
{
    /* Shared I2S0: never AUDIOIOC_STOP. Stopping RX also kills TX (no
     * speaker) and stopping TX kills the mic. Keep DMA running. */
    if (enable)
        EnsureDirStarted(true);
    input_enabled_ = enable;
}

void MetalioAudioCodec::EnableOutput(bool enable)
{
    if (enable)
        EnsureDirStarted(false);
    output_enabled_ = enable;
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

void MetalioAudioCodec::SetRecordingInput(bool enabled){
    std::lock_guard<std::mutex>lock(read_mutex_);
    // Local ADC slots retain the established >>12 gain. SCO input must use
    // the same packing detection as working headset voice, rather than force
    // that gain onto an already full-scale digital microphone stream.
    const bool headset=BluetoothScreen::IsHeadsetCaptureReady();
    recording_input_=enabled&&!headset;
    rx_calib_=0;rx_pack_=0;
}

int MetalioAudioCodec::Read(int16_t* dest, int samples)
{
    std::lock_guard<std::mutex> lock(read_mutex_);
    if (rx_fd_ < 0 || dest == nullptr || samples <= 0)
        return 0;

    if ((int)read_scratch_.size() < samples)
        read_scratch_.resize((size_t)samples);

    size_t total_bytes = (size_t)samples * sizeof(int32_t);
    size_t offset = 0;
    while (offset < total_bytes)
    {
        ssize_t n = read(rx_fd_, (uint8_t*)read_scratch_.data() + offset,
                         std::min<size_t>(7680, total_bytes - offset));
        if (n < 0)
        {
            ESP_LOGE(TAG, "Read: read() failed errno=%d", errno);
            if(errno==EINTR)continue;
            break;
        }
        if (n == 0)
            break;
        offset += (size_t)n;
    }

    int got = (int)(offset / sizeof(int32_t));
    if (got <= 0)
        return 0;

    if (!recording_input_ && rx_calib_ < 8)
    {
        int32_t p12 = 0;
        int32_t p16 = 0;
        int32_t plo = 0;
        for (int i = 0; i < got; ++i)
        {
            const int32_t raw = read_scratch_[(size_t)i];
            int32_t a12 = raw >> 12;
            if (a12 < 0) a12 = -a12;
            int32_t a16 = raw >> 16;
            if (a16 < 0) a16 = -a16;
            int32_t alo = (int16_t)raw;
            if (alo < 0) alo = -alo;
            if (a12 > p12) p12 = a12;
            if (a16 > p16) p16 = a16;
            if (alo > plo) plo = alo;
        }
        ++rx_calib_;
        if (plo > p12 * 4 && plo > 80)
            rx_pack_ = 2;
        else if (p16 > 40 && p12 > 8000)
            rx_pack_ = 1;
        else
            rx_pack_ = 0;
        if (rx_calib_ == 8)
        {
            char b[48];
            int n = snprintf(b, sizeof(b), "MIC_PACK=%d p12=%ld lo=%ld\n",
                             rx_pack_, (long)p12, (long)plo);
            if (n > 0)
                write(1, b, (size_t)n);
        }
    }

    for (int i = 0; i < got; ++i)
    {
        const int32_t raw = read_scratch_[(size_t)i];
        int32_t value;
        if(recording_input_) value=raw>>12;
        else if (rx_pack_ == 2)
            value = (int16_t)raw;
        else if (rx_pack_ == 1)
            value = raw >> 16;
        else
            value = raw >> 12;
        /* Preserve the reference codec's gain. Multiplying by four here
         * clipped normal speech before Opus or wake-word processing. */
        if (value > INT16_MAX)
            dest[i] = INT16_MAX;
        else if (value < -INT16_MAX)
            dest[i] = (int16_t)-INT16_MAX;
        else
            dest[i] = (int16_t)value;
    }

    /* Preserve mic/reference slots as in BTAudioCodec. Per-sample selection
     * of the louder slot spliced unrelated waveforms and introduced echo. */
    return got;
}

int MetalioAudioCodec::Write(const int16_t* data, int samples)
{
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (tx_fd_ < 0 || data == nullptr || samples <= 0)
        return 0;

    /* Mono PCM → stereo 32-bit left-aligned slots (Claw4 BTAudioCodec::Write).
     * volume_factor 0..65536 places the 16-bit sample in the upper half of each
     * 32-bit I2S word — requires I2S left_align=true (see esp_i2s.c). */
    const size_t need = (size_t)samples * 2;
    if (write_scratch_.size() < need)
        write_scratch_.resize(need);

    const int32_t requested_gain = volume_factor_.load(std::memory_order_acquire);
    // A slider/notification can change gain between PCM blocks. An instant
    // step creates a broadband click even on an otherwise clean sine wave.
    // Ramp for 10 ms at the fixed 16 kHz link rate, carrying state across
    // short writes. Steady audio retains exactly the existing PCM scaling.
    if (requested_gain != ramp_target_factor_) {
        ramp_start_factor_ = applied_volume_factor_;
        ramp_target_factor_ = requested_gain;
        gain_ramp_position_ = 0;
    }

    for (int i = 0; i < samples; ++i)
    {
        if (gain_ramp_position_ < 160) {
            ++gain_ramp_position_;
            applied_volume_factor_ = ramp_start_factor_ +
                (int32_t)((int64_t)(ramp_target_factor_ - ramp_start_factor_) *
                          gain_ramp_position_ / 160);
        } else applied_volume_factor_ = ramp_target_factor_;
        int64_t temp = (int64_t)data[i] * (int64_t)applied_volume_factor_;
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
    unsigned timeouts = 0;
    while (offset < total_bytes)
    {
        ssize_t n = write(tx_fd_, (const uint8_t*)write_scratch_.data() + offset,
                          std::min<size_t>(7680, total_bytes - offset));
        if (n < 0)
        {
            // The lower half retains the pending DMA waiter on timeout.
            // Retry this same PCM block instead of dropping it.
            if (errno == EINTR || (errno == ETIMEDOUT && timeouts++ < 3))
                continue;
            ESP_LOGE(TAG, "Write: write() failed errno=%d", errno);
            return (int)(offset / (2 * sizeof(int32_t)));
        }
        if (n == 0)
            break;
        offset += (size_t)n;
        timeouts = 0;
    }
    static bool reported = false;
    if (!reported && offset == total_bytes) {
        reported = true;
        write(1, "AUDIO_TX_OK\n", 12);
    }
    return (int)(offset / (2 * sizeof(int32_t)));
}
