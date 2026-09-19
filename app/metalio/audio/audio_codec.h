/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Audio codec abstraction for NuttX/openvela.
 * Replaces the ESP-IDF I2S + esp_codec_dev layer with the NuttX
 * audio character-device subsystem (/dev/audio/pcm0).
 */

#ifndef _AUDIO_CODEC_H
#define _AUDIO_CODEC_H

#include <nuttx/config.h>

#include <nuttx/audio/audio.h>

#include <vector>
#include <string>
#include <functional>

#include "esp_err_shim.h"

/*
 * Stub for the ESP-SR model_path.h type so the audio_processor / wake_word
 * abstract interfaces can reference it without pulling in the full ESP-SR
 * SDK when CONFIG_METALIO_ESP_SR is off.
 */
#if defined(CONFIG_METALIO_ESP_SR) || defined(METALIO_HAS_ESP_SR)
#include "model_path.h"
#else
struct srmodel_list_t;
#endif

/* DMA / buffer tuning — kept for API compatibility with the reference */
#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240

/* Default NuttX audio device paths */
#define AUDIO_DEV_PATH "/dev/audio/pcm0"

class AudioCodec
{
public:
    AudioCodec();
    virtual ~AudioCodec();

    virtual void SetOutputVolume(int volume);
    virtual void SetInputGain(float gain);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);

    virtual void OutputData(std::vector<int16_t>& data);
    virtual void OutputData(const int16_t* data, int samples);
    virtual bool InputData(std::vector<int16_t>& data);
    virtual void Start();

    /* Mute / volume convenience (NuttX audio FEATURE ioctls) */
    virtual void SetMute(bool mute);
    bool muted() const { return muted_; }

    inline bool duplex() const { return duplex_; }
    inline bool input_reference() const { return input_reference_; }
    inline int input_sample_rate() const { return input_sample_rate_; }
    inline int output_sample_rate() const { return output_sample_rate_; }
    inline int input_channels() const { return input_channels_; }
    inline int output_channels() const { return output_channels_; }
    inline int output_volume() const { return output_volume_; }
    inline float input_gain() const { return input_gain_; }
    inline bool input_enabled() const { return input_enabled_; }
    inline bool output_enabled() const { return output_enabled_; }

protected:
    /* NuttX audio device file descriptors.
     * On full-dupex hardware a single /dev/audio/pcm0 fd handles both
     * directions; on split devices two fds may be used. */
    int tx_fd_ = -1;   /* playback (write) */
    int rx_fd_ = -1;   /* capture (read)  */

    bool duplex_ = false;
    bool input_reference_ = false;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
    bool muted_ = false;
    int  input_sample_rate_ = 0;
    int  output_sample_rate_ = 0;
    int  input_channels_ = 1;
    int  output_channels_ = 1;
    int  output_volume_ = 70;
    float input_gain_ = 0.0f;

    /* Low-level I/O — overridden by concrete codecs (e.g. MetalioAudioCodec) */
    virtual int Read(int16_t* dest, int samples);
    virtual int Write(const int16_t* data, int samples);

    /* Open / configure the NuttX audio device */
    virtual int OpenDevice();
    virtual void CloseDevice();
    virtual int ConfigureDevice(int type, int sample_rate, int channels);
};

#endif /* _AUDIO_CODEC_H */
