#ifndef AFE_WAKE_WORD_H
#define AFE_WAKE_WORD_H

#include "wake_word.h"
#include "audio_codec.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_afe_sr_models.h>
#include <model_path.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AfeWakeWord : public WakeWord
{
public:
    AfeWakeWord();
    ~AfeWakeWord() override;

    bool Initialize(AudioCodec *codec, srmodel_list_t *models_list) override;
    void Deinitialize() override;
    void Feed(const std::vector<int16_t> &data) override;
    void OnWakeWordDetected(
        std::function<void(const std::string &wake_word)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override;
    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t> &opus) override;
    const std::string &GetLastDetectedWakeWord() const override
    {
        return last_detected_wake_word_;
    }

private:
    srmodel_list_t *models_ = nullptr;
    bool models_owned_ = false;
    const esp_afe_sr_iface_t *afe_iface_ = nullptr;
    esp_afe_sr_data_t *afe_data_ = nullptr;
    char *wakenet_model_ = nullptr;
    std::vector<std::string> wake_words_;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t detect_task_ = 0;
    std::function<void(const std::string &wake_word)> wake_word_detected_callback_;
    AudioCodec *codec_ = nullptr;
    std::string last_detected_wake_word_;

    std::deque<std::vector<int16_t>> wake_word_pcm_;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;
    std::mutex afe_mutex_;
    std::atomic<bool> fetching_{false};
    std::atomic<bool> encode_running_{false};

    void StoreWakeWordData(const int16_t *data, size_t size);
    void AudioDetectionTask();
};

#endif
