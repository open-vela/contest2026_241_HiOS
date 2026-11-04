#include "hi_task_cpp.h"
#pragma once
#include "wake_word.h"
#include "hi_openvela_command.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <pthread.h>
#include <cstring>
#include <cstdio>
#include <chrono>
extern "C" int esp_mn_commands_num(void);
#ifdef HI_APP_SMP_PROBE
extern "C" void hi_smp_probe_wake(unsigned,unsigned,uint32_t,uint32_t);
#endif

// The recognition worker owns all model state. Audio capture never runs inference.
class HiOpenvelaWakeWord final : public WakeWord {
public:
    ~HiOpenvelaWakeWord() override { Deinitialize(); }
    bool Initialize(AudioCodec *codec, srmodel_list_t *models) override {
        if (created_) return true;
        if (!codec || !models) return false;
        for (int i=0; i<models->num; ++i)
            if (!std::strcmp(models->model_name[i], HI_OPENVELA_MODEL)) name_=models->model_name[i];
        if (!name_) return false;
        channels_=codec->input_channels();
        if (channels_<1) return false;
        quit_=false; failed_=false; chunk_=0; processed_=0; dropped_=0;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 192*1024);
        pthread_attr_setschedpolicy(&attr, SCHED_RR);
        // Dedicated CPU1 inference stays above ordinary background work.
        // Capture and LVGL retain their core0 roles.
        sched_param priority{};
        priority.sched_priority = CONFIG_METALIO_APP_PRIORITY + 5;
        pthread_attr_setschedparam(&attr, &priority);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        int rc=hi_task_create_neural(&thread_, &attr, [](void *p)->void* {
            static_cast<HiOpenvelaWakeWord*>(p)->Worker(); return nullptr;
        }, this);
        pthread_attr_destroy(&attr);
        created_=rc==0;
        return created_;
    }
    void Deinitialize() override {
        { std::lock_guard<std::mutex> lock(mutex_); quit_=true; running_=false; }
        cv_.notify_all();
        if (created_) pthread_join(thread_, nullptr);
        created_=false; chunk_=0;
    }
    void Feed(const std::vector<int16_t>& data) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || !ready_ || quit_) return;
        // Capture can drain a short DMA backlog after network/UI scheduling.
        // Keep up to 2.56 seconds; Start/Stop/Reset still discard old audio.
        if (queue_.size()>=80) { dropped_ += queue_.size(); queue_.clear(); ++generation_; }
        if (data.size()!=chunk_.load()*channels_) { queue_.clear(); ++generation_; return; }
        std::vector<int16_t> mono(chunk_.load());
        if (channels_ == 2) {
            // Preserve every sample when the populated mono slot changes at
            // a DMA boundary. If mic and reference overlap, keep only mic.
            for (size_t begin=0; begin<mono.size(); begin+=16) {
                const size_t end=(begin+16<mono.size())?begin+16:mono.size();
                bool overlapping=false;
                for (size_t i=begin; i<end; ++i)
                    overlapping=overlapping || (data[i*2]!=0 && data[i*2+1]!=0);
                for (size_t i=begin; i<end; ++i) {
                    const int16_t primary=data[i*2];
                    mono[i]=(overlapping || primary!=0)?primary:data[i*2+1];
                }
            }
        } else {
            for (size_t i=0; i<mono.size(); ++i) mono[i]=data[i*channels_];
        }
        queue_.push_back(std::move(mono)); cv_.notify_one();
    }
    void OnWakeWordDetected(std::function<void(const std::string&)> cb) override {
        std::lock_guard<std::mutex> lock(mutex_); callback_=std::move(cb);
    }
    void Start() override { std::lock_guard<std::mutex> lock(mutex_); if(running_) return; running_=true; queue_.clear(); ++generation_; cv_.notify_one(); }
    void Stop() override { std::lock_guard<std::mutex> lock(mutex_); running_=false; queue_.clear(); ++generation_; }
    void Reset() override { std::lock_guard<std::mutex> lock(mutex_); queue_.clear(); ++generation_; }
    size_t GetFeedSize() override { return chunk_.load(); }
    uint32_t ProcessedFrames() const { return processed_.load(); }
    uint32_t DroppedFrames() const { return dropped_.load(); }
    bool Failed() const { return failed_.load(); }
    void EncodeWakeWordData() override {}
    bool GetWakeWordOpus(std::vector<uint8_t>& out) override { out.clear(); return false; }
    const std::string& GetLastDetectedWakeWord() const override { return phrase_; }
private:
    void Worker() {
        auto *iface=esp_mn_handle_from_name(name_);
        if (!iface) { failed_=true; std::printf("HI_WAKE_FAIL interface\n"); return; }
        std::printf("HI_WAKE_CREATE mn7_en cpu=%u mask=%u\n",hi_task_current_cpu(),hi_task_current_mask());
        auto *model=iface->create(name_, HI_OPENVELA_DURATION_MS);
        if (!model) { failed_=true; std::printf("HI_WAKE_FAIL create\n"); return; }
        bool registered=esp_mn_commands_alloc(iface, model)==0;
        struct Vocabulary { int id; const char *text; const char *phones; };
        const Vocabulary vocabulary[]={
            {HI_OPENVELA_COMMAND_ID,HI_OPENVELA_PHRASE,HI_OPENVELA_PHONEMES},
            {HI_OPENVELA_COMMAND_ID,HI_OPENVELA_PHRASE,HI_OPENVELA_PHONEMES_EY},
            {HI_OPENVELA_COMMAND_ID,HI_OPENVELA_PHRASE,HI_OPENVELA_PHONEMES_IY},
            {HI_OPENVELA_REJECT_COMMAND_ID,HI_OPENVELA_REJECT_PHRASE,HI_OPENVELA_REJECT_PHONEMES},
            {HI_OPENVELA_REJECT_COMMAND_ID,HI_OPENVELA_REJECT_PHRASE,HI_OPENVELA_REJECT_PHONEMES_EY},
            {HI_OPENVELA_REJECT_COMMAND_ID,HI_OPENVELA_REJECT_PHRASE,HI_OPENVELA_REJECT_PHONEMES_IY}
        };
        bool ok=registered;
        for (const auto &entry:vocabulary)
            if (ok) ok=esp_mn_commands_add(entry.id,entry.phones)==0;
        for (int i=0;ok && i<HI_OPENVELA_VOCABULARY_COUNT;++i) {
            auto *phrase=esp_mn_commands_get_from_index(i);
            ok=phrase && phrase->command_id==vocabulary[i].id &&
                !std::strcmp(phrase->string,vocabulary[i].text) &&
                !std::strcmp(phrase->phonemes,vocabulary[i].phones);
            if (ok) phrase->threshold=HI_OPENVELA_THRESHOLD;
        }
        if (ok) ok=esp_mn_commands_num()==HI_OPENVELA_VOCABULARY_COUNT && esp_mn_commands_update()==nullptr;
        const int samples=iface->get_samp_chunksize(model);
        ok=ok && iface->get_samp_rate(model)==HI_OPENVELA_SAMPLE_RATE && samples>0 && samples<=2048;
        if (!ok) {
            std::printf("HI_WAKE_FAIL commands\n");
            failed_=true; iface->destroy(model);
            if (registered) esp_mn_commands_free();
            return;
        }
        iface->set_det_threshold(model, HI_OPENVELA_THRESHOLD);
        { std::lock_guard<std::mutex> lock(mutex_); chunk_=samples; ready_=true; }
        std::printf("HI_WAKE_READY samples=%d id=%d phrase=%s phonemes=%s threshold=%.2f\n", samples, HI_OPENVELA_COMMAND_ID, HI_OPENVELA_PHRASE, HI_OPENVELA_PHONEMES, (double)HI_OPENVELA_THRESHOLD);
        unsigned seen=~0u;
        float peak_envelope=0.0f, noise_mean=80.0f;
        unsigned quiet_frames=16, onset_age=1000;
        // Continuous, normalized audio history; replay never reaches uplink.
        std::deque<std::vector<int16_t>> history, replay;
        unsigned live_since_replay=0, prefix_age=10000;
        for (;;) {
            std::vector<int16_t> frame;
            unsigned generation;
            bool replayed=false, backlog_restart=false, onset=false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (quit_) break;
                if (!running_ || (queue_.empty() && replay.empty())) { cv_.wait_for(lock,std::chrono::milliseconds(100)); continue; }
                // Do not let a speculative retry displace live microphone PCM.
                if (!replay.empty() && queue_.size()>=72) {
                    replay.clear(); backlog_restart=true;
                }
                generation=generation_;
                replayed=!replay.empty();
                if (replayed) { frame=std::move(replay.front()); replay.pop_front(); }
                else { frame=std::move(queue_.front()); queue_.pop_front(); }
            }
            if (generation!=seen || backlog_restart) {
                iface->clean(model); seen=generation; peak_envelope=0.0f;
                history.clear(); replay.clear(); live_since_replay=0; prefix_age=10000;
                quiet_frames=16; onset_age=1000;
                if (replayed) continue;
            }
            if (!replayed) {
            // Normalize only the recognizer input. Keep recording/uplink PCM
            // unchanged, and leave loud speech at its original level.
            int peak=0;
            int64_t absolute_sum=0;
            for (auto x:frame) {
                const int value=x<0?-(int)x:(int)x;
                if (value>peak) peak=value;
                absolute_sum+=value;
            }
            const float mean=frame.empty()?0.0f:(float)absolute_sum/frame.size();
            const bool speech=mean>(noise_mean*2.2f>100.0f?noise_mean*2.2f:100.0f) &&
                              peak>(noise_mean*6.0f>600.0f?noise_mean*6.0f:600.0f);
            // A natural speaker may pause between "Hi" and "openvela".
            // Keep the current command window intact instead of clearing its
            // first syllable on the next speech onset 1.5 seconds later.
            const unsigned command_frames=(unsigned)(HI_OPENVELA_DURATION_MS*HI_OPENVELA_SAMPLE_RATE/1000/samples);
            onset=speech && quiet_frames>=12 && onset_age>=command_frames &&
                  prefix_age>=command_frames;
            if (onset) onset_age=0;
            else if (onset_age<1000) ++onset_age;
            if (speech) quiet_frames=0;
            else if (quiet_frames<1000) ++quiet_frames;
            const float floor_sample=mean<noise_mean*1.4f?mean:noise_mean*1.4f;
            noise_mean=noise_mean*0.995f+floor_sample*0.005f;
            if (noise_mean<40.0f) noise_mean=40.0f;
            if (noise_mean>400.0f) noise_mean=400.0f;
            peak_envelope*=0.98f;
            if (peak>peak_envelope) peak_envelope=(float)peak;
            float gain=peak_envelope>0.0f?18000.0f/peak_envelope:1.0f;
            if (gain<1.0f) gain=1.0f;
            // Quiet natural speech needs more gain than close, loud TTS.
            // Peak limiting still prevents clipping; only recognition uses it.
            if (gain>8.0f) gain=8.0f;
            for (auto &x:frame) {
                const int value=(int)((float)x*gain);
                x=(int16_t)(value>32767?32767:(value< -32767?-32767:value));
            }
                history.push_back(frame);
                while (history.size()>(size_t)(2000*HI_OPENVELA_SAMPLE_RATE/1000/samples)) history.pop_front();
                ++live_since_replay;
            }
            // Context retries must yield to a backlog of live audio.
            if (onset) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.size()>=8) onset=false;
            }
            // The dedicated CPU1 neural worker is the sole PIE/HW-loop owner
            // on its core. It stays pinned throughout model initialization
            // and inference; ordinary app workers must not use these registers.
            // NuttX does not lazily enable these ESP32-P4 coprocessors. Keep
            // their state changes scoped to inference; leave interrupts enabled.
            uint32_t hwlp_before, pie_before;
            const uint32_t enabled=1;
            asm volatile("csrrw %0,0x7f1,%1" : "=r"(hwlp_before) : "r"(enabled) : "memory");
            asm volatile("csrrw %0,0x7f2,%1" : "=r"(pie_before) : "r"(enabled) : "memory");
            // Align the command window to speech, preserving the soft first
            // syllable. Energy can reset context but cannot trigger a wake.
            if (onset) {
                iface->clean(model); replay.clear(); prefix_age=10000;
                // MultiNet clean clears feature context as well as the decoder.
                // Seed a full second of context, never discard the first syllable.
                std::vector<int16_t> silence((size_t)samples,0);
                const size_t context=32;
                const size_t available=history.empty()?0:history.size()-1;
                for (size_t i=available;i<context;++i) iface->detect(model,silence.data());
                const size_t begin=history.size()>context+1?history.size()-context-1:0;
                for (size_t i=begin;i+1<history.size();++i)
                    iface->detect(model,history[i].data());
            }
            const auto state=iface->detect(model,frame.data());
            asm volatile("csrw 0x7f2,%0" :: "r"(pie_before) : "memory");
            asm volatile("csrw 0x7f1,%0" :: "r"(hwlp_before) : "memory");
            if (!replayed) {
                if (prefix_age<10000) ++prefix_age;
                auto *partial=iface->get_results(model);
                const char *text=partial?partial->string:nullptr;
                if (text) {
                    while (*text==' ') ++text;
                    if (text[0]=='h' && text[1]=='i' &&
                        (text[2]==' ' || text[2]=='\0')) prefix_age=0;
                }
            }
            if (!replayed && ++processed_==1) std::printf("HI_WAKE_AUDIO samples=%d\n", samples);
#ifdef HI_APP_SMP_PROBE
            hi_smp_probe_wake(hi_task_current_cpu(),hi_task_current_mask(),processed_.load(),dropped_.load());
#endif
            if (state==ESP_MN_STATE_TIMEOUT) {
                iface->clean(model);
                // A fixed command window can cut a complete wake phrase.
                // Retry only a recent model-decoded "hi" prefix, never silence.
                // Allow one retry per full window of fresh microphone input.
                if (!replayed && prefix_age<=(unsigned)(2000*HI_OPENVELA_SAMPLE_RATE/1000/samples) &&
                    live_since_replay>=(unsigned)(HI_OPENVELA_DURATION_MS*HI_OPENVELA_SAMPLE_RATE/1000/samples)) {
                    replay=history;
                    live_since_replay=0; prefix_age=10000;
                } else if (replayed) {
                    replay.clear();
                }
                continue;
            }
            if (state!=ESP_MN_STATE_DETECTED) continue;
            auto *result=iface->get_results(model);
            const bool hit=result && result->state==ESP_MN_STATE_DETECTED &&
                result->num>0 && result->num<=ESP_MN_RESULT_MAX_NUM &&
                result->command_id[0]==HI_OPENVELA_COMMAND_ID &&
                result->prob[0]>=HI_OPENVELA_THRESHOLD &&
                result->prob[0]<=1.0f;
            const float score=hit?result->prob[0]:0.0f;
            std::function<void(const std::string&)> callback;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (hit && running_ && !quit_ && generation==generation_) {
                    running_=false; queue_.clear(); ++generation_; callback=callback_;
                }
            }
            // Do not reconsider a completed rejected phrase in a later retry.
            if (!hit) { history.clear(); replay.clear(); prefix_age=10000; live_since_replay=0; }
            iface->clean(model);
            if (callback) { std::printf("HI_WAKE_MATCH id=%d score=%.4f phrase=%s\n", HI_OPENVELA_COMMAND_ID, (double)score, HI_OPENVELA_PHRASE); HiTask::IoScope callback_scope; callback(phrase_); }
        }
        { std::lock_guard<std::mutex> lock(mutex_); ready_=false; chunk_=0; }
        iface->destroy(model);
        esp_mn_commands_free();
    }
    pthread_t thread_{};
    bool created_=false, ready_=false, quit_=false, running_=false;
    int channels_=1;
    char *name_=nullptr;
    unsigned generation_=0;
    std::atomic<size_t> chunk_{0};
    std::atomic<uint32_t> processed_{0}, dropped_{0};
    std::atomic<bool> failed_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<int16_t>> queue_;
    std::function<void(const std::string&)> callback_;
    const std::string phrase_=HI_OPENVELA_PHRASE;
};
