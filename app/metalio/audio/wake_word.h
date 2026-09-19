/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wake word detection abstract interface.
 *
 * Ported from the ESP-IDF MetalioClaw4 reference.  The original
 * implementation uses ESP-SR WakeNet / MultiNet.  On NuttX/openvela a
 * portable speech-recognition engine will be plugged in later; the
 * interface below is kept identical so concrete implementations can be
 * added without changing AudioService.
 */

#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <string>
#include <vector>
#include <functional>

#include "audio_codec.h"

class WakeWord
{
public:
    virtual ~WakeWord() = default;

    virtual bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) = 0;
    /* Release the underlying engine (e.g. AFE instance).  Next
     * Initialize() recreates it. */
    virtual void Deinitialize() {}
    virtual void Feed(const std::vector<int16_t>& data) = 0;
    virtual void OnWakeWordDetected(
        std::function<void(const std::string& wake_word)> callback) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EncodeWakeWordData() = 0;
    virtual bool GetWakeWordOpus(std::vector<uint8_t>& opus) = 0;
    virtual const std::string& GetLastDetectedWakeWord() const = 0;
};

/*
 * Stub wake-word engine — always returns "no wake word detected".
 * Prefer EnergyWakeWord for chat wake UX until ESP-SR is ported.
 */
class StubWakeWord : public WakeWord
{
public:
    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override
    {
        (void)codec; (void)models_list;
        initialized_ = true;
        return true;
    }

    void Deinitialize() override { initialized_ = false; }

    void Feed(const std::vector<int16_t>& data) override
    {
        (void)data;  /* no-op */
    }

    void OnWakeWordDetected(
        std::function<void(const std::string& wake_word)> callback) override
    {
        on_detected_ = std::move(callback);
    }

    void Start() override { running_ = true; }
    void Stop() override  { running_ = false; }

    size_t GetFeedSize() override
    {
        /* 30 ms frames at 16 kHz mono */
        return 16000 * 30 / 1000;
    }

    void EncodeWakeWordData() override { /* no-op */ }

    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override
    {
        (void)opus;
        return false;
    }

    const std::string& GetLastDetectedWakeWord() const override
    {
        static const std::string empty;
        return empty;
    }

private:
    bool initialized_ = false;
    bool running_ = false;
    std::function<void(const std::string&)> on_detected_;
};

#endif /* WAKE_WORD_H */
