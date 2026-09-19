#ifndef AFE_AUDIO_PROCESSOR_H
#define AFE_AUDIO_PROCESSOR_H

#include "audio_processor.h"
#include "audio_codec.h"

#include <esp_afe_sr_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <model_path.h>

#include <functional>
#include <string>
#include <vector>

class AfeAudioProcessor : public AudioProcessor
{
public:
    AfeAudioProcessor();
    ~AfeAudioProcessor() override;

    void Initialize(AudioCodec *codec, int frame_duration_ms,
                    srmodel_list_t *models_list) override;
    void Feed(std::vector<int16_t> &&data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t> &&data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;

private:
    EventGroupHandle_t event_group_ = nullptr;
    const esp_afe_sr_iface_t *afe_iface_ = nullptr;
    esp_afe_sr_data_t *afe_data_ = nullptr;
    std::function<void(std::vector<int16_t> &&data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    AudioCodec *codec_ = nullptr;
    int frame_samples_ = 0;
    bool is_speaking_ = false;
    std::vector<int16_t> output_buffer_;
    TaskHandle_t task_ = 0;

    void AudioProcessorTask();
};

#endif
