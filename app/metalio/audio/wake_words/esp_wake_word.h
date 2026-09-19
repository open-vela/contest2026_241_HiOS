#ifndef ESP_WAKE_WORD_H
#define ESP_WAKE_WORD_H

/*
 * WakeNet-only wake word (no AFE). Claw4 uses this on non-AFE targets.
 * On HiOS, full AfeWakeWord create_from_config hard-locks; use this for P1.
 */

#include "wake_word.h"
#include "audio_codec.h"

#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <model_path.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

class EspWakeWord : public WakeWord
{
public:
    EspWakeWord();
    ~EspWakeWord() override;

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
    esp_wn_iface_t *wakenet_iface_ = nullptr;
    model_iface_data_t *wakenet_data_ = nullptr;
    srmodel_list_t *models_ = nullptr;
    bool models_owned_ = false;
    char *model_name_ = nullptr;
    AudioCodec *codec_ = nullptr;
    std::atomic<bool> running_{false};
    std::function<void(const std::string &wake_word)> wake_word_detected_callback_;
    std::string last_detected_wake_word_;
};

#endif
