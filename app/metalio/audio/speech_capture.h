#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>
namespace HiSpeech {
inline std::string ImageAsrUrl(const std::string& base){return base+(base.find('?')==base.npos?"?":"&")+"profile=imagegen";}
// Input is mic + echo reference, not stereo programme audio. Never average
// the reference into speech or label the interleaved samples as mono WAV.
inline bool MicOnly(std::vector<int16_t>& samples,int channels) {
    if(channels==1)return !samples.empty();
    if(channels!=2 || samples.empty() || samples.size()%2)return false;
    const std::size_t count=samples.size()/2;
    for(std::size_t i=0;i<count;++i)samples[i]=samples[i*2];
    samples.resize(count);return true;
}
template<class Audio> class CaptureOwner {
    Audio& audio_;bool owned_=false,configured_=false;
public:
    explicit CaptureOwner(Audio& audio):audio_(audio) {
        owned_=audio_.BeginExclusiveCapture();
    }
    ~CaptureOwner(){
        if(owned_){if(configured_&&audio_.GetCodec())audio_.GetCodec()->SetRecordingInput(false);audio_.EndExclusiveCapture();}
    }
    bool Ready()const{return owned_&&audio_.GetCodec();}
    void Configure(){if(Ready()){audio_.GetCodec()->SetRecordingInput(true);configured_=true;}}
    int Channels()const{return audio_.GetCodec()?audio_.GetCodec()->input_channels():0;}
    CaptureOwner(const CaptureOwner&)=delete;
    CaptureOwner& operator=(const CaptureOwner&)=delete;
};
struct Level{int peak=0,rms=0;float gain=1;};
inline Level Normalize(int16_t* samples,std::size_t count){
    Level level;if(!count)return level;int64_t sum=0;for(std::size_t i=0;i<count;++i)sum+=samples[i];
    const int mean=int(sum/int64_t(count));double energy=0;
    for(std::size_t i=0;i<count;++i){int value=int(samples[i])-mean;level.peak=std::max(level.peak,std::abs(value));energy+=double(value)*value;}
    level.rms=int(std::sqrt(energy/count));if(level.peak<80||level.rms<12)return level;
    level.gain=std::max(1.f,std::min(16.f,14000.f/level.peak));
    for(std::size_t i=0;i<count;++i){int value=int((int(samples[i])-mean)*level.gain);samples[i]=int16_t(std::max(-32767,std::min(32767,value)));}
    return level;
}
}
