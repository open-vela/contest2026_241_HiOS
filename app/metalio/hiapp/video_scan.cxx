#include "hi_task_policy.h"
// SPDX-License-Identifier: Apache-2.0
#include "video_scan.h"
#include "video_source.h"
#include "quirc/quirc.h"
#include "esp_heap_caps.h"
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <cstring>
namespace HiApp {namespace {
constexpr int Width=360,Height=300;
std::mutex observer_mutex;Ref<VideoScanner>observer;
}
bool VideoScanner::Start(){
 {std::lock_guard<std::mutex>g(observer_mutex);if(observer)return false;observer=Ref<VideoScanner>(this);}
 auto*ref=new Ref<VideoScanner>(this);pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,32*1024);sched_param priority{};priority.sched_priority=81;pthread_attr_setschedparam(&attr,&priority);pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);pthread_t thread;int rc=hi_task_create_compute(&thread,&attr,Entry,ref);pthread_attr_destroy(&attr);
 if(rc){delete ref;Stop();finished_=true;return false;}pthread_detach(thread);return true;
}
void VideoScanner::Stop(){stop_=true;std::lock_guard<std::mutex>g(observer_mutex);if(observer.get()==this)observer.reset();}
VideoScanResult VideoScanner::Result(){std::lock_guard<std::mutex>g(result_mutex_);VideoScanResult result;result.url=url_;result.hint=hint_;result.finished=finished_;result.has_frame=seen_;return result;}
void VideoScanner::Observe(const uint8_t*bgr,int width,int height){Ref<VideoScanner>current;{std::lock_guard<std::mutex>g(observer_mutex);current=observer;}if(current)current->Frame(bgr,width,height);}
void VideoScanner::Frame(const uint8_t*bgr,int width,int height){
 if(stop_||!bgr||width!=720||height!=600)return;
 std::unique_lock<std::mutex>g(frame_mutex_,std::try_to_lock);if(!g.owns_lock()||stop_||!gray_||ready_)return;
 for(int y=0;y<Height;y++)for(int x=0;x<Width;x++){auto*p=bgr+((y*2)*width+x*2)*3;gray_[y*Width+x]=(uint8_t)((29*p[0]+150*p[1]+77*p[2])>>8);}ready_=true;seen_=true;
}
void*VideoScanner::Entry(void*data){Ref<VideoScanner>self=*(Ref<VideoScanner>*)data;delete(Ref<VideoScanner>*)data;self->Run();return nullptr;}
void VideoScanner::Run(){
 quirc*decoder=quirc_new();bool initialized=decoder&&quirc_resize(decoder,Width,Height)==0;
 {std::lock_guard<std::mutex>g(frame_mutex_);gray_=(uint8_t*)heap_caps_malloc(Width*Height,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);initialized=initialized&&gray_;}
 if(!initialized){std::lock_guard<std::mutex>g(result_mutex_);hint_="二维码识别内存不足，请重试";}
 while(initialized&&!stop_){bool frame=false;{std::lock_guard<std::mutex>g(frame_mutex_);if(ready_){std::memcpy(quirc_begin(decoder,nullptr,nullptr),gray_,Width*Height);ready_=false;frame=true;}}
  if(!frame){usleep(50000);continue;}quirc_end(decoder);
  for(int i=0;i<quirc_count(decoder)&&i<4&&!stop_;i++){quirc_code code;quirc_data data;quirc_extract(decoder,i,&code);auto rc=quirc_decode(&code,&data);if(rc==QUIRC_ERROR_DATA_ECC){quirc_flip(&code);rc=quirc_decode(&code,&data);}if(rc!=QUIRC_SUCCESS)continue;std::string url;
   if(data.payload_len>0&&VideoSource::QrUrl(data.payload,(size_t)data.payload_len,url)){std::lock_guard<std::mutex>g(result_mutex_);url_=std::move(url);stop_=true;break;}
   std::lock_guard<std::mutex>g(result_mutex_);hint_="此二维码不是HTTP/HTTPS视频链接，请换一个";
  }for(int i=0;i<25&&!stop_;i++)usleep(10000);
 }
 if(decoder)quirc_destroy(decoder);
 {std::lock_guard<std::mutex>g(frame_mutex_);if(gray_)heap_caps_free(gray_);gray_=nullptr;ready_=false;}
 Stop();finished_=true;
}
}
