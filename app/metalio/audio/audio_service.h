/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * AudioService — main audio orchestration for NuttX/openvela.
 *
 * Ported from the ESP-IDF MetalioClaw4 audio_service.  The ESP-IDF
 * FreeRTOS tasks / event groups / esp_timer have been replaced with
 * pthreads, std::mutex / std::condition_variable, and the NuttX
 * work_queue-based esp_timer shim.
 *
 * Audio data flow (unchanged from the reference):
 *
 *   1. (MIC) -> [Processors] -> {Encode Queue} -> [Opus Encoder]
 *            -> {Send Queue} -> (Server)
 *
 *   2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue}
 *               -> (Speaker)
 *
 * Opus encode/decode uses esp_audio_codec (see opus_codec.h).
 *
 * Press-to-talk (PTT) detection provides a wake-word fallback.
 */

#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <memory>
#include <cstring>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <string_view>
#include <functional>

#include "esp_err_shim.h"
#include "esp_timer_shim.h"

#include "audio_codec.h"
#include "audio_processor.h"
#include "opus_codec.h"
#include "wake_word.h"

/* ------------------------------------------------------------------ */
/* AudioStreamPacket — shared with the protocol layer                 */
/* ------------------------------------------------------------------ */

/* AudioStreamPacket is shared between the protocol layer and the audio
 * service. protocol.h may have already declared it, so guard against
 * redefinition. */
#ifndef METALIO_AUDIO_STREAM_PACKET_DEFINED
#define METALIO_AUDIO_STREAM_PACKET_DEFINED
struct AudioStreamPacket
{
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};
#endif /* METALIO_AUDIO_STREAM_PACKET_DEFINED */

/* ------------------------------------------------------------------ */
/* OpusResampler — linear rate convert (encode/decode in opus_codec). */
/* ------------------------------------------------------------------ */

class OpusResampler
{
public:
    OpusResampler() = default;

    void Configure(int input_rate, int output_rate)
    {
        input_rate_  = input_rate;
        output_rate_ = output_rate;
    }

    size_t GetOutputSamples(size_t input_samples) const
    {
        if (input_rate_ == 0 || output_rate_ == 0)
            return input_samples;
        return (input_samples * (size_t)output_rate_) / (size_t)input_rate_;
    }

    void Process(const int16_t* input, size_t input_samples, int16_t* output)
    {
        if (input_rate_ == output_rate_ || input_rate_ == 0 ||
            output_rate_ == 0)
        {
            std::memcpy(output, input, input_samples * sizeof(int16_t));
            return;
        }

        size_t out_samples = GetOutputSamples(input_samples);
        for (size_t i = 0; i < out_samples; ++i)
        {
            double src_idx =
                (double)i * (double)input_rate_ / (double)output_rate_;
            size_t idx0 = (size_t)src_idx;
            size_t idx1 = (idx0 + 1 < input_samples) ? idx0 + 1 : idx0;
            double frac = src_idx - (double)idx0;
            output[i] = (int16_t)((1.0 - frac) * input[idx0] +
                                  frac * input[idx1]);
        }
    }

private:
    int input_rate_  = 0;
    int output_rate_ = 0;
};

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 6
/* NuttX I2S has no ESP-IDF auto_clear_after_cb — keep more decoded PCM
 * buffered so brief decode/LVGL stalls do not DMA-underrun into 电流声. */
#define MAX_PLAYBACK_TASKS_IN_QUEUE 12
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

/* Event flags (replaces FreeRTOS event groups) */
#define AS_EVENT_AUDIO_TESTING_RUNNING      (1u << 0)
#define AS_EVENT_WAKE_WORD_RUNNING          (1u << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1u << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY         (1u << 3)
#define AS_EVENT_INPUT_TASK_RUNNING         (1u << 4)
#define AS_EVENT_OUTPUT_TASK_RUNNING        (1u << 5)
#define AS_EVENT_CODEC_TASK_RUNNING         (1u << 6)

#define AS_EVENT_ALL_TASKS_RUNNING (AS_EVENT_INPUT_TASK_RUNNING | \
    AS_EVENT_OUTPUT_TASK_RUNNING | AS_EVENT_CODEC_TASK_RUNNING)

/* ------------------------------------------------------------------ */
/* Callbacks and task structures                                      */
/* ------------------------------------------------------------------ */

struct AudioServiceCallbacks
{
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
    /* When set, EnableWakeWordDetection(true) is ignored unless this
     * returns true. Application uses Idle-only so dialogue is not yanked. */
    std::function<bool(void)> wake_word_allowed;
    /* Local energy barge-in while TTS uplink is gated (interrupt on). */
    std::function<void(void)> on_barge_in;
};

enum AudioTaskType
{
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask
{
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp;
};

struct DebugStatistics
{
    std::atomic<uint32_t> input_count{0};
    std::atomic<uint32_t> decode_count{0};
    std::atomic<uint32_t> encode_count{0};
    std::atomic<uint32_t> playback_count{0};
};

/* ------------------------------------------------------------------ */
/* AudioService                                                       */
/* ------------------------------------------------------------------ */

class AudioService
{
public:
    AudioService();
    ~AudioService();

    /*
     * Initialize with a specific audio codec.  If called with nullptr,
     * Start() will create a default MetalioAudioCodec internally.
     */
    void Initialize(AudioCodec* codec);

    /*
     * Convenience: start with default codec (creates a MetalioAudioCodec
     * internally).  This allows callers that don't need a custom codec
     * to simply construct and call Start().
     */
    void Start();
    void Stop();

    /* Wake word */
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    bool IsIdle();
    bool IsWakeWordRunning() const
    {
        return (events_.load() & AS_EVENT_WAKE_WORD_RUNNING) != 0;
    }
    bool IsStarted() const
    {
        return !service_stopped_ &&
               (events_.load() & AS_EVENT_ALL_TASKS_RUNNING) ==
                   AS_EVENT_ALL_TASKS_RUNNING;
    }
    bool IsAudioProcessorRunning() const
    {
        return (events_.load() & AS_EVENT_AUDIO_PROCESSOR_RUNNING) != 0;
    }
    bool IsAfeWakeWord();

    /* Gate EspWakeWord create/swap — false while wake session is open. */
    void SetWakeWordUpgradeAllowed(bool allowed);
    bool IsWakeWordUpgradeAllowed() const;

    void EnableWakeWordDetection(bool enable);
    void ReleaseWakeWordEngine();
    bool IsWakeWordEngineReady() const { return wake_word_initialized_; }
    /* Spawn WakeNet init off the boot path (after Home is ready). */
    void EnsureWakeWordReady();
    void EnableVoiceProcessing(bool enable);
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);

    /* Callbacks */
    void SetCallbacks(AudioServiceCallbacks& callbacks);

    /* Queue management */
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet,
                                 bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);

    /*
     * Write raw mono PCM directly to the codec output (speaker).  Used by
     * the radio screen's player callback to match the reference firmware's
     * codec->OutputData(s_pcm_buf) path, avoiding per-packet heap churn in
     * the decode/playback queue on this uClibc++ port.
     */
    void OutputPcm(const int16_t* data, size_t samples);

    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate,
                       int samples);
    void ResetDecoder();
    /* Reset Opus state without dropping already-queued wake greeting TTS. */
    void ResetDecoderStateOnly();
    bool HasPendingPlayback() const;
    /* Drop mic Opus already queued so TTS echo cannot STT as user speech. */
    void DiscardUplinkAudio();
    /* True while speaker is playing or briefly after — mute uplink echo. */
    bool ShouldSuppressUplink() const;
    /* Gate mic uplink during Speaking (AutoStop half-duplex). Cleared on
     * Listening so leftover TTS PCM cannot permanently mute STT. */
    void SetTtsUplinkGate(bool gated);
    bool IsTtsUplinkGated() const
    {
        return tts_uplink_gate_.load(std::memory_order_relaxed);
    }
    bool IsDownlinkPlaybackProtected() const
    {
        return protect_downlink_playback_.load(std::memory_order_relaxed);
    }
    /* While armed, EnableVoiceProcessing(true) must not wipe decode/playback
     * queues — first-wake cloud greeting often arrives around detect/start. */
    void ArmDownlinkPlaybackProtect();
    void ClearDownlinkPlaybackProtect();
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void SetModelsList(srmodel_list_t* models_list);

    /* Volume / mute control */
    void SetVolume(int volume);
    int GetVolume() const;
    AudioCodec *GetCodec() const { return codec_; }
    void SetMute(bool mute);
    bool IsMuted() const;

    /*
     * Press-to-talk (PTT) detection.
     * Returns true when the PTT button is pressed.  This is the fallback
     * wake-word trigger on boards without speech recognition.  The actual
     * button GPIO is read via Board::GetInstance() or a board-specific
     * helper.
     */
    bool IsPttPressed() const;
    /* Alias for backward compatibility with the original stub API */
    bool ptt_pressed() const { return IsPttPressed(); }

private:
    AudioCodec* codec_ = nullptr;
    std::unique_ptr<AudioCodec> owned_codec_;  /* when created internally */
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;
    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;
    DebugStatistics debug_statistics_;
    srmodel_list_t* models_list_ = nullptr;
    bool using_afe_wake_word_ = false;

    /* Event flags (replaces FreeRTOS EventGroupHandle_t) */
    std::atomic<uint32_t> events_{0};

    /* Worker threads (replaces FreeRTOS TaskHandle_t) */
    std::thread audio_input_thread_;
    std::thread audio_output_thread_;
    std::thread opus_codec_thread_;

    /* Audio encode / decode queues */
    mutable std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;

    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    bool device_aec_enabled_ = false;
    bool using_esp_wake_word_ = false;
    std::atomic<bool> wake_word_probe_started_{false};
    /* Cleared during wake/Connecting/Listening so Esp create cannot swap
     * the engine mid-session (solid-blue chat on this port). */
    std::atomic<bool> wake_word_upgrade_allowed_{true};
    std::atomic<bool> voice_detected_{false};
    std::atomic<bool> service_stopped_{true};
    bool audio_input_need_warmup_ = false;
    std::atomic<bool> protect_downlink_playback_{false};
    /* When true (Speaking), ShouldSuppressUplink may mute mic for TTS.
     * Must be false in Listening or leftover decode keeps STT dead. */
    std::atomic<bool> tts_uplink_gate_{false};
    /* Interrupt barge-in: adaptive echo floor while TTS is gated. */
    float barge_echo_floor_ = 400.0f;
    int barge_loud_frames_ = 0;
    std::chrono::steady_clock::time_point barge_cooldown_until_{};
    std::chrono::steady_clock::time_point barge_armed_after_{};
    std::chrono::steady_clock::time_point barge_last_poll_{};
    std::mutex wake_word_mutex_;

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    /* Thread entry points */
    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
    void MaybeDetectBargeIn();

    void PushTaskToEncodeQueue(AudioTaskType type,
                               std::vector<int16_t>&& pcm);
    void CheckAndUpdateAudioPowerState();

    /* Helper to create a default MetalioAudioCodec (direct I2S) */
    void CreateDefaultCodec();
};

#endif /* AUDIO_SERVICE_H */
