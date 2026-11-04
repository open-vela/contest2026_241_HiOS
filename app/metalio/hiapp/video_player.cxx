#include "hi_task_policy.h"
// SPDX-License-Identifier: Apache-2.0
// One cancellable worker owns every file, HTTP connection, demuxer and decoder.
#include "video_player.h"
#include "video_source.h"
#include "video_convert.h"
#include "video_decoder.h"
#include "ota.h"
#include "application.h"
#include "audio_service.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "esp_heap_caps.h"
#include "esp_extractor.h"
#include "esp_extractor_ctrl.h"
#include "impl/esp_mp4_extractor.h"
#include "impl/esp_ts_extractor.h"
#include "impl/esp_flv_extractor.h"
#include "camera_screen/camera_screen.h"
#include "esp_aac_dec.h"
#include "esp_audio_dec.h"
#include "media_lib_os_nuttx.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <ctime>
#include <algorithm>
#include <cstring>
#include <strings.h>
#include <cstdlib>
#include <memory>
#include <cstdio>

namespace HiApp {namespace {
int64_t Mono(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
int64_t Micro(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
struct Measure{uint64_t&total;int64_t start=Micro();~Measure(){total+=(uint64_t)(Micro()-start);}};
VideoPlayer*active=nullptr;std::mutex active_mutex;
pthread_once_t extract_once=PTHREAD_ONCE_INIT;
void RegisterExtractors(){media_lib_os_nuttx_install();esp_ts_extractor_register();esp_mp4_extractor_register();esp_flv_extractor_register();}
bool PathStat(const char*path,struct stat&st){
 for(unsigned attempt=0;attempt<3;++attempt){if(lstat(path,&st)==0)return true;int code=errno;if(code!=EINTR&&code!=EAGAIN&&code!=EBUSY&&code!=EIO&&code!=ETIMEDOUT){printf("HIVIDEO_PATH errno=%d\n",code);return false;}if(attempt<2)usleep(20000);}
 printf("HIVIDEO_PATH errno=%d retries=3\n",errno);return false;
}
bool PathSafe(const std::string&path){if(!VideoSource::VideoPath(path))return false;size_t at=7;for(;;){auto end=path.find('/',at+1);auto prefix=path.substr(0,end);struct stat st{};if(!PathStat(prefix.c_str(),st)||S_ISLNK(st.st_mode))return false;if(end==path.npos)break;at=end;}return true;}
uint16_t RGB(int y,int u,int v){int c=std::max(0,y-16),d=u-128,e=v-128;int r=std::max(0,std::min(255,(298*c+409*e+128)>>8));int g=std::max(0,std::min(255,(298*c-100*d-208*e+128)>>8));int b=std::max(0,std::min(255,(298*c+516*d+128)>>8));return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
}
VideoFrame::VideoFrame(unsigned w,unsigned h){
 if(!w||!h||w>480||h>360)return;pixels=(uint16_t*)heap_caps_malloc(w*h*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!pixels)return;
 image.header.magic=LV_IMAGE_HEADER_MAGIC;image.header.cf=LV_COLOR_FORMAT_RGB565;image.header.w=w;image.header.h=h;image.header.stride=w*2;image.data_size=w*h*2;image.data=(const uint8_t*)pixels;
}
VideoFrame::~VideoFrame(){if(pixels)heap_caps_free(pixels);}
VideoPlayer::~VideoPlayer(){}
std::string VideoPlayer::Directory(){DIR*d=opendir("/sdcard");std::string root="/sdcard/video";if(d){while(auto*e=readdir(d))if(!strcasecmp(e->d_name,"video")){root=std::string("/sdcard/")+e->d_name;break;}closedir(d);}return root;}
bool VideoPlayer::KeepsAwake(){std::lock_guard<std::mutex>owner(active_mutex);if(!active)return false;std::lock_guard<std::mutex>g(active->mutex_);return active->status_.state=="opening"||active->status_.state=="playing";}
bool VideoPlayer::Running(){std::lock_guard<std::mutex>g(mutex_);return running_;}
bool VideoPlayer::List(const std::string&path,unsigned offset,std::vector<std::pair<std::string,bool>>&out,bool&more,std::string&error){
 more=false;if(offset>4096||!PathSafe(path)){error="请在SD卡根目录创建video文件夹并放入视频";return false;}DIR*d=opendir(path.c_str());if(!d){error="video目录无法读取，请检查SD卡";return false;}
 unsigned visited=0,matched=0;while(auto*e=readdir(d)){if(++visited>8192){more=true;break;}std::string name=e->d_name;if(name.empty()||name[0]=='.'||name.find('/')!=name.npos||name.find('\\')!=name.npos)continue;
  struct stat st{};auto full=path+"/"+name;if(lstat(full.c_str(),&st)<0||S_ISLNK(st.st_mode))continue;bool directory=S_ISDIR(st.st_mode);auto dot=name.rfind('.');std::string ext=dot==name.npos?"":name.substr(dot);for(char&c:ext)if(c>='A'&&c<='Z')c+=32;
  if(!directory&&(!S_ISREG(st.st_mode)||(ext!=".mp4"&&ext!=".flv"&&ext!=".mov"&&ext!=".m4v"&&ext!=".ts")))continue;
  if(matched++<offset)continue;if(out.size()==60){more=true;break;}out.emplace_back(name,directory);
 }closedir(d);return true;
}
bool VideoPlayer::Open(const std::string&source,std::string&error){
 const bool remote=VideoSource::HttpUrl(source);if((remote&&!network_)||(!remote&&(!storage_||!VideoSource::VideoPath(source)))){error="媒体地址无效；本地视频须位于SD卡/video内";return false;}
 std::lock_guard<std::mutex>guard(mutex_);requested_=source;seek_=0;paused_=false;generation_.fetch_add(1);frames_.clear();status_=VideoStatus{};status_.state="opening";opened_ms_=Mono();status_.source=source;status_.volume=Application::GetInstance().GetAudioService().GetVolume();
 if(running_)return true;running_=true;auto*ref=new Ref<VideoPlayer>(this);pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,48*1024);sched_param priority{};priority.sched_priority=108;pthread_attr_setschedparam(&attr,&priority);pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);pthread_t thread;int rc=hi_task_create_legacy(&thread,&attr,Entry,ref);pthread_attr_destroy(&attr);if(rc){delete ref;running_=false;status_.state="error";status_.error="无法创建媒体工作线程";error=status_.error;return false;}pthread_detach(thread);return true;
}
void VideoPlayer::Stop(){std::lock_guard<std::mutex>g(mutex_);requested_.clear();generation_.fetch_add(1);paused_=false;frames_.clear();status_.state=running_?"stopping":"idle";}
void VideoPlayer::Pause(bool pause){std::lock_guard<std::mutex>g(mutex_);if(status_.state!="playing"&&status_.state!="paused")return;paused_=pause;auto session=audio_session_.load();if(session)Application::GetInstance().GetAudioService().PauseLocalPlayback(session,pause);if(pause)pause_start_=Mono();else if(pause_start_){wall_start_+=Mono()-pause_start_;pause_start_=0;}status_.state=pause?"paused":"playing";}
void VideoPlayer::Seek(uint32_t at){std::lock_guard<std::mutex>g(mutex_);if(requested_.empty()||!status_.seekable||status_.live||!status_.duration)return;seek_=std::min(at,status_.duration-1);generation_.fetch_add(1);paused_=false;frames_.clear();status_.state="opening";}
void VideoPlayer::Volume(int volume){
 // NVS erase/write briefly masks interrupts. Do not persist during SD DMA.
 auto&audio=Application::GetInstance().GetAudioService();
 std::lock_guard<std::mutex>g(mutex_);audio.SetVolume(std::max(0,std::min(100,volume)),false);
 if(running_)volume_dirty_=true;else audio.SetVolume(audio.GetVolume());
}
uint32_t VideoPlayer::Clock(){auto session=audio_session_.load();if(audio_clock_&&session)return clock_base_+Application::GetInstance().GetAudioService().LocalPlaybackTimeMs(session);return clock_base_+(uint32_t)std::max<int64_t>(0,(pause_start_?pause_start_:Mono())-wall_start_);}
VideoStatus VideoPlayer::Status(){std::lock_guard<std::mutex>g(mutex_);auto result=status_;if(running_)result.wall_ms=(uint32_t)(Mono()-opened_ms_);result.volume=Application::GetInstance().GetAudioService().GetVolume();if(result.state=="playing"||result.state=="paused")result.position=Clock();return result;}
Ref<VideoFrame>VideoPlayer::PollFrame(){std::lock_guard<std::mutex>g(mutex_);Ref<VideoFrame>frame;if(paused_)return frame;auto position=Clock();while(!frames_.empty()&&frames_.front()->pts<=position+30){frame=frames_.front();frames_.pop_front();}if(frame)++status_.presented;return frame;}
void VideoPlayer::Publish(unsigned generation,const Ref<VideoFrame>&frame){std::lock_guard<std::mutex>g(mutex_);if(!Current(generation))return;while(frames_.size()>=3){frames_.pop_front();++status_.dropped;}frames_.push_back(frame);if(!status_.frames)status_.first_frame_ms=(uint32_t)(Mono()-opened_ms_);++status_.frames;}
void VideoPlayer::Error(unsigned gen,const std::string&error){std::lock_guard<std::mutex>g(mutex_);if(Current(gen)){status_.state="error";status_.error=error;frames_.clear();}}
void*VideoPlayer::Entry(void*data){Ref<VideoPlayer>self=*(Ref<VideoPlayer>*)data;delete(Ref<VideoPlayer>*)data;self->Run();return nullptr;}

struct VideoPlayer::Playback {
 VideoPlayer&p;unsigned generation;uint32_t target;std::string error;
 esp_extractor_handle_t extractor=nullptr;hv_decoder*decoder=nullptr;void*aac=nullptr;uint32_t audio=0;
 unsigned length_size=0;std::vector<uint8_t>annex,config;Ref<VideoFrame>pool[5];
 esp_extractor_stream_info_t video{},sound{};bool probed_aac=false;bool has_audio=false,have_audio_base=false,have_pts=false,container_audio=false,raw_aac=false;VideoSource::AacConfig aac_config{};uint32_t pts_base=0,segment_begin=0;
 uint64_t output_samples=0;int input_rate=0;int64_t resample_phase=0;int16_t previous=0;bool have_previous=false;
 uint64_t read_us=0,extract_us=0,decode_us=0,convert_us=0,audio_us=0;unsigned decoded=0;
 void Metrics(){std::lock_guard<std::mutex>g(p.mutex_);if(Current()){p.status_.read_us=read_us;p.status_.extract_us=extract_us;p.status_.decode_us=decode_us;p.status_.convert_us=convert_us;p.status_.audio_us=audio_us;p.status_.decoded=decoded;}}
 std::vector<int16_t>pcm,mono;uint32_t last_audio_pts=0;
 Playback(VideoPlayer&owner,unsigned gen,uint32_t seek):p(owner),generation(gen),target(seek){}
 ~Playback(){CloseExtractor();if(aac)esp_aac_dec_close(aac);if(decoder)hv_decoder_destroy(decoder);if(audio){auto&as=Application::GetInstance().GetAudioService();as.PauseLocalPlayback(audio,false);as.EndLocalPlayback(audio);}p.audio_session_=0;}
 bool Current(){return p.Current(generation);}
 bool Wait(){while(Current()&&p.paused_)usleep(10000);return Current();}
 void CloseExtractor(){if(extractor){esp_extractor_close(extractor);extractor=nullptr;}}
 void Silent(const char*reason){
  {std::lock_guard<std::mutex>g(p.mutex_);if(!Current())return;const auto now=p.Clock();p.clock_base_=now;p.wall_start_=Mono();p.pause_start_=0;p.audio_clock_=false;p.status_.warning=std::string("静音播放：")+reason;}
  if(audio){Application::GetInstance().GetAudioService().EndLocalPlayback(audio);audio=0;p.audio_session_=0;}
  if(aac){esp_aac_dec_close(aac);aac=nullptr;}has_audio=false;have_audio_base=false;
 }
 struct Input {
  Playback&owner;std::unique_ptr<Http>http;int fd=-1;uint32_t size=0,position=0;bool remote=false;std::string url;
  Input(Playback&o):owner(o){}~Input(){if(fd>=0)close(fd);}
  bool Open(const std::string&source){url=source;remote=VideoSource::HttpUrl(url);if(remote){for(unsigned redirect=0;redirect<6&&owner.Current();redirect++){
    http.reset(new Http);http->SetDeadlineMs(10000);http->SetHeader("User-Agent","HiOS-HiVideo/0.1");http->SetHeader("Accept-Encoding","identity");if(!http->Open("GET",url)){owner.error="媒体网络连接失败";return false;}int code=http->GetStatusCode();
    if(code==301||code==302||code==303||code==307||code==308){url=VideoSource::Resolve(url,http->GetResponseLocation());if(url.empty())break;continue;}
    if(code!=200){owner.error="媒体HTTP状态："+std::to_string(code);return false;}if(!http->GetResponseEncoding().empty()&&http->GetResponseEncoding()!="identity"){owner.error="媒体服务器返回了压缩响应";return false;}auto length=http->GetBodyLength();if(length>UINT32_MAX){owner.error="媒体文件超过4GiB";return false;}size=(uint32_t)length;return true;
   }owner.error="媒体重定向无效或次数过多";return false;
  }if(!PathSafe(url)){owner.error="本地媒体路径无效";return false;}fd=open(url.c_str(),O_RDONLY);struct stat st{};if(fd<0||fstat(fd,&st)<0||!S_ISREG(st.st_mode)||st.st_size<=0||(uint64_t)st.st_size>UINT32_MAX){owner.error="本地媒体文件无法读取或超过4GiB";return false;}size=(uint32_t)st.st_size;return true;}
  static int Read(void*buffer,uint32_t size,void*ctx){
   auto&s=*(Input*)ctx;Measure measured{s.owner.read_us};if(size>256*1024){s.owner.error="媒体读取请求超过256KiB上限";return -1;}
   uint32_t total=0;while(total<size){if(!s.owner.Wait())return -1;uint32_t count=std::min<uint32_t>(size-total,8192);int n;
    if(s.remote){s.http->SetDeadlineMs(5000);n=s.http->Read((char*)buffer+total,count);}else{
     for(unsigned attempt=0;;attempt++){
      do{n=(int)read(s.fd,(uint8_t*)buffer+total,count);}while(n<0&&errno==EINTR&&s.owner.Current());
      if(n>=0||attempt>=3||(errno!=ETIMEDOUT&&errno!=EIO)||!s.owner.Current())break;
      printf("HIVIDEO_SD_RETRY pos=%u attempt=%u errno=%d\n",s.position,attempt+1,errno);
      usleep(50000);if(lseek(s.fd,s.position,SEEK_SET)<0)break;
     }
    }
    if(n<0){printf("HIVIDEO_READ_FAIL pos=%u request=%u errno=%d\n",s.position,count,errno);return -1;}if(!n)break;total+=(uint32_t)n;s.position+=(uint32_t)n;
    if(s.remote&&s.position>16u*1024*1024){s.owner.error="单个媒体分段超过16MiB上限";return -1;}
   }return (int)total;
  }
  static int Seek(uint32_t at,void*ctx){auto&s=*(Input*)ctx;if(!s.owner.Current()||s.remote||at>s.size){printf("HIVIDEO_SEEK_REJECT at=%u size=%u\n",at,s.size);return -1;}auto n=lseek(s.fd,at,SEEK_SET);if(n<0){printf("HIVIDEO_SEEK_FAIL at=%u errno=%d\n",at,errno);return -1;}s.position=(uint32_t)n;return 0;}
  static bool ReadAt(void*ctx,uint32_t at,void*data,size_t bytes){auto&s=*(Input*)ctx;if(at>s.size||bytes>s.size-at)return false;return Seek(at,ctx)==0&&Read(data,bytes,ctx)==(int)bytes;}
  static uint32_t Size(void*ctx){return ((Input*)ctx)->size;}
 };
 bool ReadText(const std::string&url,std::string&text,std::string&effective){Input in(*this);if(!in.Open(url))return false;effective=in.url;char chunk[2048];int n=-1;while(Current()&&(n=Input::Read(chunk,sizeof(chunk),&in))>0){if(text.size()+n>192*1024){error="播放列表超过192KiB上限";return false;}text.append(chunk,n);}return Current()&&n==0;}
 bool Configure(){
  if(esp_extractor_get_stream_info(extractor,ESP_EXTRACTOR_STREAM_TYPE_VIDEO,0,&video)!=ESP_EXTRACTOR_ERR_OK||video.video_info.format!=ESP_EXTRACTOR_VIDEO_FORMAT_H264){error="仅支持含H.264视频的MP4或TS";return false;}
  unsigned w=video.video_info.width,h=video.video_info.height;
  // FLV/TS can leave dimensions unknown until SPS arrives. The decoder checks
  // the SPS and its allocation budget before allocating any reference picture.
  if(w>640||h>368){error="视频分辨率 "+std::to_string(w)+"×"+std::to_string(h)+" 超出上限。\n支持：H.264 Baseline，最大640×368。\n建议：480×272、15帧/秒。";return false;}
  if(!decoder){decoder=hv_decoder_create();if(!decoder){error="无法分配视频解码内存";return false;}}
  if(video.spec_info&&video.spec_info_len){if(video.spec_info_len>16384){error="视频参数过大";return false;}if(video.spec_info[0]==1){if(!VideoSource::AvccConfig(video.spec_info,video.spec_info_len,config,length_size)){error="无效MP4视频参数";return false;}}else if(!VideoSource::AnnexB(video.spec_info,video.spec_info_len,0,config)){error="无效视频参数";return false;}hv_picture pic{};if(hv_decoder_decode(decoder,config.data(),config.size(),&pic)<0){error=hv_decoder_error(decoder);return false;}}
  has_audio=esp_extractor_get_stream_info(extractor,ESP_EXTRACTOR_STREAM_TYPE_AUDIO,0,&sound)==ESP_EXTRACTOR_ERR_OK;
  raw_aac=container_audio&&(probed_aac||VideoSource::AacAsc(sound.spec_info,sound.spec_info_len,aac_config));
  printf("HIVIDEO_STREAM w=%u h=%u video_spec=%u audio=%d audio_spec=%u asc_lc=%d\n",w,h,(unsigned)video.spec_info_len,has_audio,(unsigned)sound.spec_info_len,raw_aac);
  if(has_audio&&sound.audio_info.format!=ESP_EXTRACTOR_AUDIO_FORMAT_AAC)Silent("音轨编码暂不支持，视频继续");
  {std::lock_guard<std::mutex>g(p.mutex_);if(!Current())return false;p.status_.width=w;p.status_.height=h;p.status_.duration=std::max(p.status_.duration,video.duration);}
  return true;
 }
 bool Audio(esp_extractor_frame_info_t&frame){Measure measured{audio_us};
  if(!has_audio)return true;
  if(frame.frame_size>16384){error="音频帧超过上限";return false;}
  const uint8_t*payload=frame.frame_buffer;size_t bytes=frame.frame_size;
  if(raw_aac&&!VideoSource::AacRawPayload(payload,bytes,payload,bytes)){Silent("AAC数据不完整，视频继续");return true;}
  if(!aac){bool adts=!raw_aac&&bytes>=7&&payload[0]==0xff&&(payload[1]&0xf6)==0xf0;esp_aac_dec_cfg_t cfg{};cfg.sample_rate=raw_aac?aac_config.rate:sound.audio_info.sample_rate;cfg.channel=raw_aac?aac_config.channels:sound.audio_info.channel;cfg.bits_per_sample=16;cfg.no_adts_header=!adts;
   if((adts&&((payload[2]>>6)&3)!=1)||(!adts&&!raw_aac)){Silent("音轨暂不支持，建议AAC-LC");return true;}
   if(!adts&&(cfg.channel<1||cfg.channel>2||cfg.sample_rate<8000||cfg.sample_rate>48000)){Silent("音轨参数超限，视频继续");return true;}
   if(esp_aac_dec_open(&cfg,sizeof(cfg),&aac)!=ESP_AUDIO_ERR_OK){Silent("声音解码器无法启动");return true;}pcm.resize(4096);mono.reserve(4096);
   printf("HIVIDEO_AAC raw=%d rate=%d ch=%u\n",cfg.no_adts_header,(int)cfg.sample_rate,(unsigned)cfg.channel);
  }
  esp_audio_dec_in_raw_t raw{};raw.buffer=const_cast<uint8_t*>(payload);raw.len=bytes;
  unsigned loops=0;while(raw.len&&Current()&&loops++<32){esp_audio_dec_out_frame_t out{};out.buffer=(uint8_t*)pcm.data();out.len=pcm.size()*2;esp_audio_dec_info_t info{};int rc=esp_aac_dec_decode(aac,&raw,&out,&info);
   if(rc!=ESP_AUDIO_ERR_OK||raw.consumed>raw.len||out.decoded_size>out.len||(!raw.consumed&&!out.decoded_size)){Silent("AAC音轨无法解码，视频继续");return true;}
   if(!out.decoded_size){raw.buffer+=raw.consumed;raw.len-=raw.consumed;continue;}
   if(info.bits_per_sample!=16||info.channel<1||info.channel>2||info.sample_rate<8000||info.sample_rate>48000){Silent("音轨输出参数超限，视频继续");return true;}
   if(!audio){auto&as=Application::GetInstance().GetAudioService();for(int i=0;i<100&&Current();i++){audio=as.BeginLocalPlayback(3200,true,false,true);if(audio||as.HasLocalPlaybackSession()||as.HasExclusiveCapture())break;usleep(10000);}if(!audio){error="其他音频任务正在占用声音，请关闭后重试";return false;}p.audio_session_=audio;if(BluetoothScreen::PreparePlaybackRoute()<0){error="音频路由尚未就绪";return false;}}
   if(input_rate!=(int)info.sample_rate){input_rate=info.sample_rate;resample_phase=0;have_previous=false;}
   mono.clear();size_t samples=out.decoded_size/(2*info.channel);for(size_t i=0;i<samples;i++){int32_t current=pcm[i*info.channel];if(info.channel==2)current=(current+pcm[i*2+1])/2;
    if(!have_previous){previous=(int16_t)current;have_previous=true;resample_phase=input_rate;mono.push_back(previous);continue;}
    while(resample_phase<=16000){int64_t value=(int64_t)previous*16000+(current-previous)*resample_phase;mono.push_back((int16_t)(value/16000));resample_phase+=input_rate;}resample_phase-=16000;previous=(int16_t)current;
   }
   if(!have_audio_base){std::lock_guard<std::mutex>g(p.mutex_);p.clock_base_=frame.pts;p.audio_clock_=true;have_audio_base=true;}
   auto&as=Application::GetInstance().GetAudioService();size_t at=0;while(at<mono.size()&&Wait()){if(as.HasExclusiveCapture()){error="录音或通话已接管声音，视频播放已停止";return false;}size_t count=std::min((size_t)960,mono.size()-at);auto deadline=Mono()+5000;while(Current()&&!as.TryEnqueueLocalPcm(audio,mono.data()+at,count)){if(!Wait())return false;if(as.HasExclusiveCapture()||as.LocalPlaybackWriteFailed(audio)){error="声音输出被其他任务中断";return false;}if(Mono()>deadline){error="声音输出等待超时";return false;}usleep(5000);}at+=count;output_samples+=count;{std::lock_guard<std::mutex>g(p.mutex_);p.status_.audio_samples+=(uint32_t)count;}}
   raw.buffer+=raw.consumed;raw.len-=raw.consumed;
  }if(raw.len&&Current()){error="AAC数据循环超过上限";return false;}return Current();
 }
 bool Video(esp_extractor_frame_info_t&frame){
  if(!VideoSource::AnnexB(frame.frame_buffer,frame.frame_size,length_size,annex)){error="视频帧格式无效或大于192KiB";return false;}
  hv_picture picture{};int result;{Measure measured{decode_us};result=hv_decoder_decode(decoder,annex.data(),annex.size(),&picture);}if(result<0){error=hv_decoder_error(decoder);return false;}if(!result)return true;++decoded;
  if(!picture.width||!picture.height||picture.width>640||picture.height>368||picture.width*picture.height>640*368){error="视频在播放中改变为超限分辨率";return false;}
  {std::lock_guard<std::mutex>g(p.mutex_);if(have_pts&&p.status_.frames>0&&(int32_t)(p.Clock()-frame.pts)>160){++p.status_.dropped;return true;}}
  unsigned w=picture.width,h=picture.height;if(w>480){h=h*480/w;w=480;}if(h>360){w=w*360/h;h=360;}w=std::max(2u,w&~1u);h=std::max(2u,h&~1u);Ref<VideoFrame>available;
  for(auto&slot:pool){if(!slot||(__atomic_load_n(&slot->references,__ATOMIC_ACQUIRE)==1&&(slot->image.header.w!=w||slot->image.header.h!=h)))slot=Ref<VideoFrame>(new VideoFrame(w,h));if(!slot->valid()){error="PSRAM不足，无法分配视频显示缓冲";return false;}if(slot->image.header.w==w&&slot->image.header.h==h&&__atomic_load_n(&slot->references,__ATOMIC_ACQUIRE)==1){available=slot;break;}}
  if(!available){std::lock_guard<std::mutex>g(p.mutex_);++p.status_.dropped;return true;}
  if(!Current())return false;{Measure measured{convert_us};hv_yuv420_rgb565(picture.y,picture.u,picture.v,picture.stride,picture.crop_x,picture.crop_y,picture.width,picture.height,available->pixels,w,h);}
  available->pts=frame.pts;
  // Audio can be queued ahead of its presentation clock. Give LVGL time to
  // consume pending frames instead of evicting every not-yet-due picture.
  // Bound the wait so demuxing can always continue feeding audio.
  auto show_deadline=Mono()+100;while(Wait()){
   bool full;{std::lock_guard<std::mutex>g(p.mutex_);full=p.frames_.size()>=3;}
   if(!full||Mono()>=show_deadline)break;usleep(5000);
  }if(!Current())return false;p.Publish(generation,available);{std::lock_guard<std::mutex>g(p.mutex_);p.status_.memory=hv_decoder_bytes(decoder);p.status_.width=picture.width;p.status_.height=picture.height;if(p.status_.frames==1)printf("HIVIDEO_FIRST_FRAME w=%u h=%u\n",picture.width,picture.height);}
  return true;
 }
 bool Extract(Input&input,esp_extractor_type_t type,uint32_t begin){
  // The extractor keeps callbacks to Input. Close it on every error path
  // before its stack-owned Input (and the HTTP/file handle) can disappear.
  struct Closer{Playback*self;~Closer(){self->CloseExtractor();}}closer{this};
  const bool indexed=type!=ESP_EXTRACTOR_TYPE_TS;
  CloseExtractor();esp_extractor_config_t cfg{};cfg.type=type;cfg.extract_mask=ESP_EXTRACT_MASK_AV;cfg.in_read_cb=Input::Read;cfg.in_seek_cb=input.remote?nullptr:Input::Seek;cfg.in_size_cb=Input::Size;cfg.in_ctx=&input;cfg.out_pool_size=256*1024;cfg.out_align=4;
  if(esp_extractor_open(&cfg,&extractor)!=ESP_EXTRACTOR_ERR_OK){error="无法创建媒体提取器";return false;}bool dynamic=true,wait=false;esp_extractor_ctrl(extractor,ESP_EXTRACTOR_CTRL_TYPE_SET_DYNAMIC_INDEXING,&dynamic,sizeof(dynamic));esp_extractor_ctrl(extractor,ESP_EXTRACTOR_CTRL_TYPE_SET_WAIT_OUTPUT,&wait,sizeof(wait));uint32_t max_frame=192*1024;esp_extractor_ctrl(extractor,ESP_EXTRACTOR_CTRL_TYPE_SET_MAX_FRAME_SIZE,&max_frame,sizeof(max_frame));
  if(type==ESP_EXTRACTOR_TYPE_TS){bool frames=true;esp_extractor_ctrl(extractor,ESP_EXTRACTOR_CTRL_TYPE_SET_FRAME_ACROSS_PES,&frames,sizeof(frames));}
  container_audio=indexed;probed_aac=false;if(indexed&&!input.remote){probed_aac=VideoSource::ProbeAacConfig(Input::ReadAt,&input,input.size,type==ESP_EXTRACTOR_TYPE_FLV,aac_config);if(Input::Seek(0,&input)!=0){error="无法重置媒体文件";return false;}}
  if(esp_extractor_parse_stream(extractor)!=ESP_EXTRACTOR_ERR_OK||!Configure()){if(error.empty())error="无法识别MP4/FLV/TS媒体信息";return false;}
  if(indexed&&target){if(esp_extractor_seek(extractor,target)!=ESP_EXTRACTOR_ERR_OK){error="此文件无法定位，请从头播放";return false;}}
  bool segment_pts=false;uint32_t first=0;int64_t progress=Mono();
  while(Wait()){esp_extractor_frame_info_t frame{};int rc;{auto before=read_us;auto started=Micro();rc=esp_extractor_read_frame(extractor,&frame);extract_us+=(uint64_t)(Micro()-started)-(read_us-before);}
   if(rc==ESP_EXTRACTOR_ERR_EOS)break;
   if(rc==ESP_EXTRACTOR_ERR_STREAM_CHANGED){if(aac){esp_aac_dec_close(aac);aac=nullptr;}if(!Configure())return false;progress=Mono();continue;}
   if(rc==ESP_EXTRACTOR_ERR_NEED_MORE_BUF||rc==ESP_EXTRACTOR_ERR_ALREADY_EXIST||rc==ESP_EXTRACTOR_ERR_WAITING_OUTPUT){
    if(Mono()-progress>10000){error="媒体提取器等待数据或输出超过10秒，请重试";return false;}usleep(10000);continue;
   }
   if(rc!=ESP_EXTRACTOR_ERR_OK){printf("HIVIDEO_EXTRACT rc=%d input=%u/%u\n",rc,input.position,input.size);fflush(stdout);if(error.empty())error=rc==ESP_EXTRACTOR_ERR_SKIPPED?"单帧超过缓冲上限，请降低码率后重试":"媒体读取或解封装失败（代码"+std::to_string(rc)+"），请检查文件是否完整";return false;}
   if(!frame.frame_size){esp_extractor_release_frame(extractor,&frame);if(Mono()-progress>5000){error="媒体未产生有效数据";return false;}continue;}progress=Mono();
   if(!segment_pts){first=frame.pts;segment_pts=true;}
   if(!indexed)frame.pts=begin+(uint32_t)std::max<int32_t>(0,(int32_t)(frame.pts-first));
   if(!have_pts){std::lock_guard<std::mutex>g(p.mutex_);p.clock_base_=frame.pts;p.wall_start_=Mono();p.pause_start_=0;p.audio_clock_=false;p.status_.state="playing";have_pts=true;}
   bool ok=true;if(!EXTRACTOR_IS_EOS(frame.frame_flag)){if(frame.stream_type==ESP_EXTRACTOR_STREAM_TYPE_AUDIO)ok=Audio(frame);else if(frame.stream_type==ESP_EXTRACTOR_STREAM_TYPE_VIDEO)ok=Video(frame);}
   const uint32_t frame_pts=frame.pts;esp_extractor_release_frame(extractor,&frame);Metrics();if(!ok)return false;
   if(!has_audio){while(Current()){uint32_t clock;{std::lock_guard<std::mutex>g(p.mutex_);clock=p.Clock();}if(frame_pts<=clock+160)break;if(!Wait())return false;usleep(5000);}}
  }CloseExtractor();return Current();
 }
 bool Play(const std::string&source){
  auto path=source.substr(0,source.find_first_of("?#"));for(char&c:path)if(c>='A'&&c<='Z')c+=32;bool hls=VideoSource::HttpUrl(source)||(path.size()>=5&&path.substr(path.size()-5)==".m3u8");
  if(!hls){Input input(*this);if(!input.Open(source))return false;auto dot=path.rfind('.');auto ext=dot==path.npos?"":path.substr(dot);auto type=ext==".mp4"||ext==".mov"||ext==".m4v"?ESP_EXTRACTOR_TYPE_MP4:ext==".flv"?ESP_EXTRACTOR_TYPE_FLV:ESP_EXTRACTOR_TYPE_TS;if(type==ESP_EXTRACTOR_TYPE_TS&&ext!=".ts"){error="请选择MP4、FLV、MOV、M4V或TS视频";return false;}{std::lock_guard<std::mutex>g(p.mutex_);p.status_.seekable=type!=ESP_EXTRACTOR_TYPE_TS;}return Extract(input,type,0);}
  if(!VideoSource::HttpUrl(source)){error="首版m3u8请使用HTTP或HTTPS地址";return false;}
  std::string url=source;uint64_t next=0;uint32_t live_begin=0;bool live_started=false;int64_t fresh=Mono();unsigned depth=0;
  while(Wait()){std::string text,effective;if(!ReadText(url,text,effective))return false;VideoSource::Playlist playlist;if(!VideoSource::ParsePlaylist(text,effective,next,target,playlist,error))return false;
   if(!playlist.variant.empty()){if(++depth>3){error="嵌套播放列表超过上限";return false;}url=playlist.variant;continue;}url=effective;
   {std::lock_guard<std::mutex>g(p.mutex_);p.status_.live=playlist.live;p.status_.seekable=!playlist.live;p.status_.duration=playlist.live?0:playlist.duration;}
   for(auto&segment:playlist.segments){if(!Wait())return false;if(segment.discontinuity){if(decoder){hv_decoder_destroy(decoder);decoder=nullptr;}if(aac){esp_aac_dec_close(aac);aac=nullptr;}length_size=0;}
    uint32_t begin=playlist.live&&live_started?live_begin:segment.begin;
    Input input(*this);if(!input.Open(segment.url)||!Extract(input,ESP_EXTRACTOR_TYPE_TS,begin))return false;next=segment.sequence+1;live_begin=begin+segment.duration;live_started=true;target=0;fresh=Mono();}
   if(!playlist.live&&(playlist.segments.empty()||playlist.segments.back().begin+playlist.segments.back().duration>=playlist.duration))return true;
   if(Mono()-fresh>20000){error="直播流超过20秒没有新分段";return false;}for(int i=0;i<100&&Wait();i++)usleep(10000);
  }return false;
 }
};
void VideoPlayer::Run(){
 {std::lock_guard<std::mutex>g(active_mutex);if(active&&active!=this){Error(generation_,"另一视频播放任务正在释放，请稍后重试");std::lock_guard<std::mutex>l(mutex_);running_=false;return;}active=this;}
 pthread_once(&extract_once,RegisterExtractors);
 for(;;){unsigned gen;uint32_t seek;std::string source;{std::lock_guard<std::mutex>g(mutex_);gen=generation_;source=requested_;seek=seek_;}
  if(!source.empty()){bool success=false;{
   Playback playback(*this,gen,seek);auto camera_deadline=Mono()+12000;while(Current(gen)&&!CameraScreen::PreviewStopped()&&Mono()<camera_deadline)usleep(10000);if(!CameraScreen::PreviewStopped())playback.error="摄像头尚未关闭，请稍后重试";else if(Current(gen))success=playback.Play(source);playback.Metrics();
   if(success&&playback.audio){auto&as=Application::GetInstance().GetAudioService();as.FinishLocalPlayback(playback.audio);auto end=Mono()+3000;while(Current(gen)&&!as.LocalPlaybackDrained(playback.audio)){auto before=Mono();if(!playback.Wait())break;end+=Mono()-before;if(Mono()>end){success=false;playback.error="音频尾部输出超时";break;}usleep(10000);}}
   if(Current(gen)){if(!success)Error(gen,playback.error.empty()?"媒体播放失败":playback.error);else{std::lock_guard<std::mutex>g(mutex_);status_.position=Clock();status_.wall_ms=(uint32_t)(Mono()-opened_ms_);status_.state="ended";}}
  }}
  {std::lock_guard<std::mutex>owner(active_mutex);std::lock_guard<std::mutex>g(mutex_);if(Current(gen)){if(volume_dirty_.exchange(false)){auto&audio=Application::GetInstance().GetAudioService();audio.SetVolume(audio.GetVolume());}if(requested_.empty()){status_.state="idle";status_.position=0;}if(active==this)active=nullptr;running_=false;break;}}
 }
}
}
