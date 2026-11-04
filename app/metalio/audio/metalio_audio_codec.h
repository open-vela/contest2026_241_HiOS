/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * MetalioClaw4 direct-I2S audio codec for NuttX/openvela.
 *
 * The MetalioClaw4 board has NO I2C codec chip (no ES8311 at 0x10).
 * Audio flows directly through the ESP32-P4 I2S peripheral:
 *   TX:  I2S0 DOUT (GPIO 9)  → speaker amplifier (PA enabled by TCA9555 P1.0)
 *   RX:  I2S0 DIN  (GPIO 11) ← PDM/I2S microphone
 *   BCLK (GPIO 12), WS (GPIO 10)
 *
 * This class replaces the original BTAudioCodecDuplex from the ESP-IDF
 * firmware.  The ESP-IDF version used i2s_channel_read/write directly
 * with 32-bit slots and software volume scaling.  In NuttX we use the
 * standard /dev/audio/pcm0 (playback) and /dev/audio/pcm_in0 (capture)
 * character devices, which are registered by board_i2s_init().
 *
 * The base AudioCodec class already handles read()/write(), AUDIOIOC_*
 * ioctls for format/rate/volume/mute, and start/stop.  This subclass
 * only:
 *   1. Sets duplex + 2-channel input (mic + reference for AEC)
 *   2. Opens the correct NuttX device paths (pcm0 for TX, pcm_in0 for RX)
 */

#ifndef _METALIO_AUDIO_CODEC_H
#define _METALIO_AUDIO_CODEC_H

#include "audio_codec.h"
#include <mutex>
#include <atomic>

/*
 * NuttX I2S audio device paths.
 * board_i2sdev_initialize() registers:
 *   /dev/audio/pcm0    — TX (playback, with PCM decode wrapper)
 *   /dev/audio/pcm_in0 — RX (capture, raw I2S)
 */
#define METALIO_AUDIO_TX_DEV  "/dev/audio/pcm0"
#define METALIO_AUDIO_RX_DEV  "/dev/audio/pcm_in0"

class MetalioAudioCodec : public AudioCodec
{
public:
    /*
     * @param input_sample_rate   Mic capture sample rate (Hz)
     * @param output_sample_rate  Speaker playback sample rate (Hz)
     *
     * The original firmware uses 16 kHz for both (voice assistant mode).
     * input_reference_ is true → 2 input channels (mic + AEC reference).
     */
    MetalioAudioCodec(int input_sample_rate = 16000,
                      int output_sample_rate = 16000);

    virtual ~MetalioAudioCodec() = default;

    /* Software volume only — I2S has no FU_VOLUME and the ioctl can glitch. */
    virtual void SetOutputVolume(int volume) override;
    virtual void SetOutputVolume(int volume, bool persist) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
    virtual void Start() override;
    void SetRecordingInput(bool enabled) override;

protected:
    /*
     * Override OpenDevice to use separate NuttX device paths for TX/RX.
     * The base class opens AUDIO_DEV_PATH ("/dev/audio/pcm0") for both
     * directions, but NuttX registers pcm0 as TX-only and pcm_in0 as
     * RX-only.
     */
    virtual int OpenDevice() override;

    /* 32-bit I2S slots: configure bits=32, convert like Claw4 BTAudioCodec. */
    virtual int ConfigureDevice(int type, int sample_rate,
                                int channels) override;
    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

private:
    /* Reuse conversion scratch to avoid per-frame heap churn on the
     * audio threads (fragmentation here has caused crackle + LVGL blue). */
    std::mutex device_mutex_;
    std::mutex read_mutex_;
    std::mutex write_mutex_;
    std::vector<int32_t> write_scratch_;
    std::vector<int32_t> read_scratch_;
    /* Cached from SetOutputVolume — avoids pow() every Write() frame. */
    std::atomic<int32_t> volume_factor_{0};
    // Write mutex owns ramp state; UI only publishes the requested gain.
    int32_t applied_volume_factor_ = 0;
    int32_t ramp_start_factor_ = 0;
    int32_t ramp_target_factor_ = 0;
    unsigned gain_ramp_position_ = 160;
    /* 0=>>12 (Claw4), 1=>>16, 2=low 16-bit. Locked after a few frames. */
    bool recording_input_=false;
    int rx_pack_ = 0;
    int rx_calib_ = 0;
    bool rx_started_ = false;
    bool tx_started_ = false;
    void RecalcVolumeFactor();
    void EnsureDirStarted(bool input);
};

#endif /* _METALIO_AUDIO_CODEC_H */
