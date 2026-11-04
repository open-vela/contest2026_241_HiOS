// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "hiapp.h"
#include "lvgl.h"
#include <atomic>
#include <mutex>
#include <deque>
namespace HiApp {
struct VideoFrame:RefCounted {
 uint16_t*pixels=nullptr;lv_image_dsc_t image{};uint32_t pts=0;
 VideoFrame(unsigned,unsigned);~VideoFrame();
 bool valid()const{return pixels!=nullptr;}
};
struct VideoStatus {std::string state="idle",error,source,warning;uint32_t position=0,duration=0,audio_samples=0;uint64_t read_us=0,extract_us=0,decode_us=0,convert_us=0,audio_us=0;uint32_t wall_ms=0,first_frame_ms=0;unsigned decoded=0,presented=0,width=0,height=0,frames=0,dropped=0,memory=0;int volume=80;bool live=false,seekable=false;};
class VideoPlayer:public RefCounted {
public:
 VideoPlayer(bool storage,bool network):storage_(storage),network_(network){}
 ~VideoPlayer();
 bool Open(const std::string&,std::string&);
 void Stop();void Pause(bool);void Seek(uint32_t);void Volume(int);
 VideoStatus Status();Ref<VideoFrame>PollFrame();
 static bool List(const std::string&,unsigned,std::vector<std::pair<std::string,bool>>&,bool&,std::string&);
 static std::string Directory();static bool KeepsAwake();bool Running();
private:
 struct Playback;friend struct Playback;
 std::mutex mutex_;std::atomic<unsigned>generation_{0};std::atomic<bool>paused_{false},volume_dirty_{false};
 bool storage_,network_,running_=false;std::string requested_;uint32_t seek_=0;
 VideoStatus status_;std::deque<Ref<VideoFrame>>frames_;std::atomic<uint32_t>audio_session_{0};
 uint32_t clock_base_=0;int64_t opened_ms_=0,wall_start_=0,pause_start_=0;bool audio_clock_=false;
 static void*Entry(void*);void Run();bool Current(unsigned g)const{return generation_.load()==g;}
 void Error(unsigned,const std::string&);uint32_t Clock();void Publish(unsigned,const Ref<VideoFrame>&);
};
}
