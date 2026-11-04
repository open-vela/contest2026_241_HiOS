#include "hi_task_policy.h"
#include "local_music_player.h"
#include "display_text.h"

#include "application.h"
#include "audio_service.h"
#include "audio_codec.h"
#include "music_screen/music_screen.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "esp_mp3_dec.h"
#include "radio_screen/radio_player.h"
#include <pthread.h>
#include <sched.h>
#include <nuttx/config.h>
#include "esp_log_shim.h"
#include "freertos_shim.h"
#include <metalio/metalio.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr const char *TAG = "HiMusicPlayer";
constexpr const char *kMusicRoot = "/sdcard/Music";
constexpr size_t kMaxTracks = 512;
constexpr size_t kPcmQueueLimit = 48 * 1024;
constexpr int kOutputRate = 16000;
constexpr size_t kOutputChunk = 960;

struct Track { std::string path; std::string title; };
struct PcmChunk { std::vector<int16_t> samples; };

/* FAT directory names are passed directly to LVGL labels. Keep only complete,
 * well-formed UTF-8 sequences so a truncated/invalid filename cannot render
 * as tofu boxes or split a multibyte Chinese character. */
std::string SanitizeUtf8(const std::string &text) { return DisplayUtf8(text); }

bool IsSupported(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8]{};
    size_t n = std::min(sizeof(ext) - 1, std::strlen(dot));
    for (size_t i = 0; i < n; ++i) ext[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(dot[i])));
    return std::strcmp(ext, ".mp3") == 0 || std::strcmp(ext, ".wav") == 0;
}

void ScanDir(const std::string &path, std::vector<Track> &out, int depth)
{
    if (depth > 3 || out.size() >= kMaxTracks) return;
    DIR *dir = opendir(path.c_str());
    if (!dir) return;
    while (out.size() < kMaxTracks) {
        struct dirent *entry = readdir(dir);
        if (!entry) break;
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        std::string full = path + "/" + entry->d_name;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { ScanDir(full, out, depth + 1); continue; }
        if (!S_ISREG(st.st_mode) || !IsSupported(entry->d_name)) continue;
        std::string title(entry->d_name);
        const size_t dot = title.find_last_of('.');
        if (dot != std::string::npos) title.resize(dot);
        out.push_back({full, SanitizeUtf8(title)});
    }
    closedir(dir);
}
}

// Local files have their own bounded decoder worker. Network radio is unchanged.
struct LocalMusicPlayer::Impl {
 mutable std::mutex mutex;
 std::vector<Track> tracks;
 std::string path,status="正在读取 SD 卡歌曲";
 std::atomic<State> state{State::Idle};
 std::atomic<int> current{-1},position{0},duration{0},seek{-1},speed{1000},repeat{0};
 std::atomic<unsigned> epoch{0},active{0};
 std::atomic<bool> paused{false},scrubbing{false},muted{false},noise{false},shuffle{false},preview{false},ringtone{false},alive{true};
 // Bookmark I/O runs on the decoder/scan worker, never on the LVGL thread.
 // Keep it outside the global settings flash and outside telephone files.
 std::mutex bookmark_io;
 std::string remembered_path;
 int remembered_position=0,remembered_duration=0;
 unsigned long bookmark_generation=0;
 unsigned bookmark_revision=0;
 bool bookmark_loaded=false,bookmark_dirty=false;
 uint64_t bookmark_retry_at=0;
 static uint32_t BookmarkChecksum(const std::string &text){uint32_t h=2166136261u;for(unsigned char c:text){h^=c;h*=16777619u;}return h;}
 static bool BookmarkPath(const std::string &p){return p.size()>8&&p.size()<1024&&p.compare(0,8,"/sdcard/")==0&&p.find("/../")==std::string::npos&&p.find_first_of("\r\n")==std::string::npos&&p.find('\0')==std::string::npos&&IsSupported(p.c_str());}
 void RememberLocked(){
  if(preview||ringtone||!BookmarkPath(path))return;
  int at=std::max(0,position.load()),total=std::max(0,duration.load());
  if(total>0&&at>=total-250)at=0;
  if(remembered_path==path&&remembered_position==at&&remembered_duration==total)return;
  remembered_path=path;remembered_position=at;remembered_duration=total;bookmark_dirty=true;++bookmark_revision;
 }
 bool ReadBookmark(char bank,std::string &data,unsigned long &generation){
  char name[80],header[120];snprintf(name,sizeof(name),"/sdcard/.HiOSMusic/playback.%c",bank);FILE *f=fopen(name,"rb");if(!f)return false;
  unsigned long length=0,seq=0,sum=0;bool ok=fgets(header,sizeof(header),f)&&sscanf(header,"HiOSM1 %lu %lu %lx",&seq,&length,&sum)==3&&length>0&&length<1280;
  if(ok){data.resize(length);ok=fread(&data[0],1,length,f)==length&&BookmarkChecksum(data)==sum;}fclose(f);
  if(ok){size_t nl=data.find('\n');int at=-1,total=-1;ok=nl!=std::string::npos&&sscanf(data.c_str(),"%d %d",&at,&total)==2&&at>=0&&total>=0&&BookmarkPath(data.substr(nl+1));}
  if(ok)generation=seq;return ok;
 }
 void LoadBookmark(){
  std::lock_guard<std::mutex> io(bookmark_io);
  {std::lock_guard<std::mutex> l(mutex);if(bookmark_loaded)return;}
  if(!metalio_sdcard_is_mounted())return;
  std::string a,b;unsigned long ga=0,gb=0;bool va=ReadBookmark('a',a,ga),vb=ReadBookmark('b',b,gb);
  std::string data=va&&(!vb||ga>=gb)?a:vb?b:"";unsigned long generation=va&&(!vb||ga>=gb)?ga:vb?gb:0;
  std::lock_guard<std::mutex> l(mutex);bookmark_loaded=true;bookmark_generation=generation;
  if(!bookmark_dirty&&!data.empty()){
   int at=0,total=0;sscanf(data.c_str(),"%d %d",&at,&total);remembered_path=data.substr(data.find('\n')+1);remembered_position=at;remembered_duration=total;
  }
 }
 void FlushBookmark(bool force=false){
  {std::lock_guard<std::mutex> l(mutex);if(!bookmark_dirty)return;}
  timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);uint64_t now=(uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
  if(!force&&now<bookmark_retry_at)return;bookmark_retry_at=now+3000;
  LoadBookmark();
  std::lock_guard<std::mutex> io(bookmark_io);
  std::string data;unsigned revision;unsigned long next;
  {std::lock_guard<std::mutex> l(mutex);if(!bookmark_loaded||!bookmark_dirty||!BookmarkPath(remembered_path))return;
   data=std::to_string(remembered_position)+" "+std::to_string(remembered_duration)+"\n"+remembered_path;revision=bookmark_revision;next=bookmark_generation+1;}
  if(metalio_sdcard_ensure_rw()<0)return;
  if(mkdir("/sdcard/.HiOSMusic",0775)<0&&errno!=EEXIST)return;
  char name[80],header[120];snprintf(name,sizeof(name),"/sdcard/.HiOSMusic/playback.%c",next%2?'a':'b');FILE *f=fopen(name,"wb");if(!f)return;
  int n=snprintf(header,sizeof(header),"HiOSM1 %lu %lu %08lx\n",next,(unsigned long)data.size(),(unsigned long)BookmarkChecksum(data));
  bool ok=fwrite(header,1,n,f)==(size_t)n&&fwrite(data.data(),1,data.size(),f)==data.size();
  if(fflush(f)!=0)ok=false;if(fsync(fileno(f))!=0)ok=false;if(fclose(f)!=0)ok=false;
  if(ok){std::lock_guard<std::mutex> l(mutex);bookmark_generation=next;if(bookmark_revision==revision)bookmark_dirty=false;}
 }
 uint32_t random=0x5a1937u;
 uint32_t session=0;uint64_t sample_phase=0;int phase_rate=0,phase_scale=0;
 int16_t staged[960]{};size_t staged_count=0;
 void ResetOutput(){sample_phase=0;phase_rate=phase_scale=0;staged_count=0;}
 bool Submit(unsigned id,bool tail=false){
  if(!staged_count||(!tail&&staged_count<960))return true;auto &audio=Application::GetInstance().GetAudioService();
  int waited=0;while(Same(id)&&!(paused&&!scrubbing)&&seek.load()<0&&!audio.TryEnqueueLocalPcm(session,staged,staged_count)){
   if(++waited>=2000){Status("音频输出未就绪，请稍后重试");state=State::Error;return false;}usleep(2000);}
  if(!Same(id)||(paused&&!scrubbing)||seek.load()>=0)return false;staged_count=0;return true;
 }
 static uint16_t U16(const uint8_t*p){return p[0]|p[1]<<8;}
 static uint32_t U32(const uint8_t*p){return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
 static uint32_t BE32(const uint8_t*p){return(uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3];}
 struct Meta{bool wav=false,toc_valid=false;long begin=0,end=0;int rate=0,channels=0,bits=16,format=1,bitrate=0,total=0;uint8_t toc[100]{};};
 void Status(const char *text){{std::lock_guard<std::mutex>l(mutex);status=text;}char line[192];int n=snprintf(line,sizeof(line),"HiMusic: %s\n",text);if(n>0)write(1,line,std::min((size_t)n,sizeof(line)-1));}
 bool Same(unsigned id){return alive&&epoch.load()==id;}
 // Refill PCM ahead of the software-rendered rotating record. The worker
 // sleeps on bounded queue backpressure; restore its idle priority on exit.
 struct DecodePriority {
  int policy=0;bool changed=false;sched_param original{};
  DecodePriority(){if(pthread_getschedparam(pthread_self(),&policy,&original)==0){auto target=original;target.sched_priority=std::max(target.sched_priority,CONFIG_METALIO_APP_PRIORITY+12);changed=pthread_setschedparam(pthread_self(),SCHED_RR,&target)==0;}}
  ~DecodePriority(){if(changed)(void)pthread_setschedparam(pthread_self(),policy,&original);}
 };
 bool Metadata(FILE*f,Meta &m){
  fseek(f,0,SEEK_END);m.end=ftell(f);fseek(f,0,SEEK_SET);uint8_t h[4096]{};size_t n=fread(h,1,12,f);
  if(n==12&&!memcmp(h,"RIFF",4)&&!memcmp(h+8,"WAVE",4)){
   m.wav=true;bool format=false;long at=12;
   for(int chunks=0;chunks<128&&at+8<m.end;chunks++){
    fseek(f,at,SEEK_SET);if(fread(h,1,8,f)!=8)return false;uint32_t size=U32(h+4);long next=at+8+(long)size+(size&1);
    if(size>(uint32_t)(m.end-at-8)||next<=at)return false;
    if(!memcmp(h,"fmt ",4)){
     if(size<16||fread(h,1,std::min((uint32_t)40,size),f)<16)return false;
     m.format=U16(h);m.channels=U16(h+2);m.rate=U32(h+4);m.bits=U16(h+14);
     if(m.format==0xfffe&&size>=40)m.format=U16(h+24);
     format=true;
    }else if(!memcmp(h,"data",4)){
     m.begin=at+8;m.end=m.begin+size;
     if(!format||m.channels<1||m.channels>2||m.rate<8000||m.rate>192000||
       (m.format!=1&&m.format!=3)||(m.bits!=8&&m.bits!=16&&m.bits!=24&&m.bits!=32)||(m.format==3&&m.bits!=32))return false;
     m.total=(int)((int64_t)size*1000/(m.rate*m.channels*(m.bits/8)));fseek(f,m.begin,SEEK_SET);return true;
    }at=next;
   }return false;
  }
  long tag=0;fseek(f,0,SEEK_SET);if(fread(h,1,10,f)==10&&!memcmp(h,"ID3",3)){
   for(int i=6;i<10;i++)if(h[i]&128)return false;
   tag=10+((h[6]&127)<<21)+((h[7]&127)<<14)+((h[8]&127)<<7)+(h[9]&127)+((h[5]&16)?10:0);
  }
  if(tag<0||tag>=m.end)return false;fseek(f,tag,SEEK_SET);n=fread(h,1,sizeof(h),f);
  for(size_t i=0;i+4<n;i++){
   uint32_t b=BE32(h+i);int ver=(b>>19)&3,layer=(b>>17)&3,br=(b>>12)&15,sr=(b>>10)&3;
   if((b&0xffe00000)!=0xffe00000||ver==1||layer!=1||br==0||br==15||sr==3)continue;
   const int r[]={44100,48000,32000},b1[]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320},b2[]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
   m.rate=r[sr]/(ver==3?1:ver==2?2:4);m.channels=((b>>6)&3)==3?1:2;m.bitrate=(ver==3?b1[br]:b2[br])*1000;m.begin=tag+i;
   if(m.end>=128){uint8_t tail[3];fseek(f,m.end-128,SEEK_SET);if(fread(tail,1,3,f)==3&&!memcmp(tail,"TAG",3))m.end-=128;}
   m.total=(int)((int64_t)(m.end-m.begin)*8000/m.bitrate);
   size_t x=i+4+(((b>>16)&1)?0:2)+(ver==3?(m.channels==1?17:32):(m.channels==1?9:17));
   if(x+12<n&&(!memcmp(h+x,"Xing",4)||!memcmp(h+x,"Info",4))){
    uint32_t flags=BE32(h+x+4);size_t p=x+8;
    if((flags&1)&&p+4<=n){uint32_t frames=BE32(h+p);m.total=(int)((int64_t)frames*(ver==3?1152:576)*1000/m.rate);p+=4;}
    if(flags&2)p+=4;if((flags&4)&&p+100<=n){memcpy(m.toc,h+p,100);m.toc_valid=true;}
   }
   fseek(f,m.begin,SEEK_SET);return true;
  }return false;
 }
 // The raw ESP MP3 decoder accepts a complete Layer III frame, not an
 // arbitrary file-read chunk. Keep partial frames and tags outside the codec.
 static size_t Mp3FrameBytes(const uint8_t *p){
  uint32_t b=BE32(p);int ver=(b>>19)&3,layer=(b>>17)&3,br=(b>>12)&15,sr=(b>>10)&3;
  if((b&0xffe00000u)!=0xffe00000u||ver==1||layer!=1||!br||br==15||sr==3)return 0;
  const int rates[]={44100,48000,32000},v1[]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320},v2[]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
  int rate=rates[sr]/(ver==3?1:ver==2?2:4),bitrate=(ver==3?v1[br]:v2[br])*1000;
  return (ver==3?144:72)*bitrate/rate+((b>>9)&1);
 }
 bool Output(const int16_t *pcm,int frames,int channels,int rate,unsigned id){
  if(frames<1||rate<8000||rate>192000||channels<1||channels>2)return true;
  int velocity=scrubbing?speed.load():1000;int scale=std::max(200,std::min(3000,std::abs(velocity)));
  if(scrubbing&&std::abs(velocity)<50)return true;
  if(rate!=phase_rate||scale!=phase_scale){sample_phase=0;phase_rate=rate;phase_scale=scale;}
  auto &audio=Application::GetInstance().GetAudioService();audio.MuteWakeWordForMs(600);
  const uint64_t step=((uint64_t)rate*scale<<16)/16000000u,limit=(uint64_t)frames<<16;
  while(sample_phase<limit&&Same(id)){
   if(staged_count==960&&!Submit(id))return false;
   int a=std::min(frames-1,(int)(sample_phase>>16)),b=std::min(frames-1,a+1);int frac=sample_phase&65535;
   if(scrubbing&&velocity<0){a=frames-1-a;b=std::max(0,a-1);}
   int va=pcm[a*channels],vb=pcm[b*channels];if(channels==2){va=(va+pcm[a*2+1])/2;vb=(vb+pcm[b*2+1])/2;}
   int v=va+(int)(((int64_t)(vb-va)*frac)>>16);random=random*1664525u+1013904223u;
   if(noise&&!preview&&(random&2047u)==0)v+=((random>>16)&1)?700:-700;
   if(muted&&!ringtone)v=0;staged[staged_count++]=(int16_t)std::max(-32768,std::min(32767,v));sample_phase+=step;
   if(staged_count==960){if(!Submit(id))return false;state=State::Playing;}
  }sample_phase-=limit;return Same(id);
 }
 void Decode(const std::string &file,unsigned id){
  if(!Same(id))return;
  DecodePriority priority;
  auto &audio=Application::GetInstance().GetAudioService();
  int call=metalio_nt26_get_call_state();
  // A lost hangup indication must not leave local files blocked forever.
  // CLCC is read-only; if the modem is busy, retain the call protection.
  if(call!=METALIO_CALL_IDLE&&!ringtone){(void)metalio_nt26_call_poll();call=metalio_nt26_get_call_state();}
  if(call!=METALIO_CALL_IDLE&&!(call==METALIO_CALL_INCOMING&&ringtone)){Status("通话中，暂不能播放音乐");state=State::Error;return;}
  RadioPlayer::Instance().Stop();if(!Same(id))return;
  if(!metalio_bt_is_powered()){if(metalio_bt_power(true)<0){Status("音频设备供电失败");state=State::Error;return;}usleep(1200000);if(!Same(id))return;}
  MusicScreen::PauseForVoice();
  int bt=BluetoothScreen::PreparePlaybackRoute();if(!Same(id))return;if(bt<0){Status("音频设备未就绪，请检查蓝牙连接");state=State::Error;return;}
  if(metalio_nt26_get_call_state()!=METALIO_CALL_IDLE&&!(ringtone&&metalio_nt26_get_call_state()==METALIO_CALL_INCOMING)){Status("通话中，暂不能播放音乐");state=State::Error;return;}
  if(metalio_audio_select_local()<0||metalio_nt26_call_control(3,1)<0){Status("扬声器音频通路未就绪");state=State::Error;return;}
  if(!metalio_sdcard_is_mounted()&&metalio_sdcard_mount_ro()<0){Status("SD 卡不可用");state=State::Error;return;}
  // Upgrade before this long-lived reader opens the volume. Otherwise a
  // later call/contact save cannot unmount the RO filesystem while music
  // or its paused stream holds the file. Read-only media may still play.
  int writable=metalio_sdcard_ensure_rw();
  if(writable<0)ESP_LOGW("HiMusic","SD remains read-only (%d); pending phone saves will retry",writable);
  FlushBookmark(true);
  FILE *f=fopen(file.c_str(),"rb");if(!f){Status("歌曲无法打开，请检查 SD 卡");state=State::Error;return;}
  std::vector<char> file_buffer(32768);setvbuf(f,file_buffer.data(),_IOFBF,file_buffer.size());
  Meta meta;if(!Metadata(f,meta)){fclose(f);Status("音频文件损坏，或 WAV 编码不受支持");state=State::Error;return;}
  if(!Same(id)){fclose(f);return;}
  duration=meta.total;position=0;void *decoder=nullptr;
  if(!meta.wav&&esp_mp3_dec_open(nullptr,0,&decoder)!=ESP_AUDIO_ERR_OK){fclose(f);Status("MP3 解码器初始化失败");state=State::Error;return;}
  std::vector<uint8_t> input(8192),raw_pcm(16384);std::vector<int16_t> wave;size_t used=0;bool eof=false,delivered=false,was_paused=false;int failures=0;int64_t clock_us=0;
  session=audio.BeginLocalPlayback(ringtone?3200:6400);ResetOutput();
  audio.MuteWakeWordForMs(600);
  Status("正在播放");state=paused?State::Paused:State::Loading;
  while(Same(id)){
   int target=seek.exchange(-1);
   if(target>=0){
    target=std::max(0,std::min(std::max(0,meta.total-20),target));long offset;
    if(meta.wav){long align=meta.channels*(meta.bits/8);offset=meta.begin+(long)((int64_t)target*meta.rate/1000)*align;}
    else if(meta.toc_valid&&meta.total>0){double percent=100.*target/meta.total;int a=std::min(99,(int)percent);double x=meta.toc[a],y=a==99?256:meta.toc[a+1];offset=meta.begin+(long)((x+(y-x)*(percent-a))/256*(meta.end-meta.begin));}
    else offset=meta.begin+(long)((int64_t)(meta.end-meta.begin)*target/std::max(1,meta.total));
    fseek(f,std::min(meta.end,offset),SEEK_SET);if(decoder)esp_mp3_dec_reset(decoder);
    used=0;eof=false;failures=0;clock_us=(int64_t)target*1000;position=target;audio.FlushLocalPlayback(session);ResetOutput();
   }
   if(paused&&!scrubbing){if(!was_paused){audio.EndLocalPlayback(session);session=0;ResetOutput();was_paused=true;{std::lock_guard<std::mutex> l(mutex);if(Same(id))RememberLocked();}}FlushBookmark();state=State::Paused;usleep(10000);continue;}if(was_paused){session=audio.BeginLocalPlayback(ringtone?3200:6400);ResetOutput();}was_paused=false;
   if(scrubbing&&std::abs(speed.load())<50){usleep(10000);continue;}
   int frames=0,rate=meta.rate,ch=meta.channels;const int16_t *pcm=nullptr;
   if(meta.wav){
    long left=meta.end-ftell(f);if(left<=0)break;int width=meta.bits/8,align=width*ch;size_t bytes=fread(input.data(),1,std::min((long)(input.size()/align*align),left),f);
    frames=bytes/align;if(!frames)break;wave.resize(frames*ch);
    for(int i=0;i<frames*ch;i++){auto*p=input.data()+i*width;int v=0;
     if(meta.format==3){float sample;memcpy(&sample,p,4);if(!__builtin_isfinite(sample))sample=0;v=(int)(std::max(-1.f,std::min(1.f,sample))*32767);}
     else if(width==1)v=((int)p[0]-128)*256;else if(width==2)v=(int16_t)U16(p);else if(width==3)v=(int32_t)((uint32_t)p[0]<<8|p[1]<<16|(uint32_t)p[2]<<24)>>16;else v=(int32_t)U32(p)>>16;
     wave[i]=(int16_t)v;
    }pcm=wave.data();
   }else{
    auto Fill=[&](bool force=false){if(!eof&&(force||used<2048)&&used<input.size()){long left=meta.end-ftell(f);size_t n=left>0?fread(input.data()+used,1,std::min((long)(input.size()-used),left),f):0;used+=n;if(!n||ftell(f)>=meta.end)eof=true;}};
    Fill();if(used<4)break;
    size_t at=0,frame_bytes=0;
    while(at+4<=used&&!(frame_bytes=Mp3FrameBytes(input.data()+at)))++at;
    if(at){used-=at;memmove(input.data(),input.data()+at,used);Fill();}
    if(!frame_bytes){if(eof)break;continue;}
    if(used<frame_bytes){Fill(true);if(used<frame_bytes){if(eof)break;continue;}}
    esp_audio_dec_in_raw_t raw{};raw.buffer=input.data();raw.len=frame_bytes;
    esp_audio_dec_out_frame_t out{};out.buffer=raw_pcm.data();out.len=raw_pcm.size();esp_audio_dec_info_t info{};
    int result=esp_mp3_dec_decode(decoder,&raw,&out,&info);
    if(result==ESP_AUDIO_ERR_BUFF_NOT_ENOUGH&&raw.consumed==0&&out.needed_size>raw_pcm.size()&&out.needed_size<=65536){raw_pcm.resize(out.needed_size);continue;}
    if(result==ESP_AUDIO_ERR_OK&&out.decoded_size&&info.bits_per_sample==16&&info.channel>=1&&info.channel<=2&&info.sample_rate>=8000&&info.sample_rate<=192000){
     ch=info.channel;rate=info.sample_rate;frames=out.decoded_size/(ch*2);pcm=(const int16_t*)raw_pcm.data();failures=0;
    }else if(result!=ESP_AUDIO_ERR_DATA_LACK&&++failures>=64){char message[96];snprintf(message,sizeof(message),"MP3 解码失败 (%d)，请检查音频文件",result);Status(message);state=State::Error;break;}
    // Every call owns exactly this frame, including bit-reservoir warm-up.
    // DATA_LACK must advance to the next frame, never refill the same bytes.
    used-=frame_bytes;memmove(input.data(),input.data()+frame_bytes,used);
    if(!frames)continue;
   }

   if(Output(pcm,frames,ch,rate,id)){delivered=true;if(!scrubbing){clock_us+=(int64_t)frames*1000000/rate;position=(int)(clock_us/1000);}}
   if(state==State::Error)break;
   if(preview&&position>=12000)break;
  }
  if(decoder)esp_mp3_dec_close(decoder);fclose(f);
  if(!Same(id)||state==State::Error){audio.EndLocalPlayback(session);session=0;ResetOutput();return;}
  if(staged_count)Submit(id,true);audio.FinishLocalPlayback(session);
  while(Same(id)&&!audio.LocalPlaybackDrained(session))usleep(5000);
  audio.EndLocalPlayback(session);session=0;ResetOutput();
  if(Same(id)){if(!delivered){Status("未能解出有效音频，请检查文件");state=State::Error;}
   else{Status(preview?"试听结束":"播放结束");state=State::Idle;}}
 }
 void Run(){
  int policy;sched_param priority{};if(pthread_getschedparam(pthread_self(),&policy,&priority)==0){priority.sched_priority=std::max(priority.sched_priority,CONFIG_METALIO_APP_PRIORITY+5);(void)pthread_setschedparam(pthread_self(),SCHED_RR,&priority);}
  while(alive){FlushBookmark();unsigned id=epoch.load();if(id==active.load()){usleep(10000);continue;}
   std::string file;{std::lock_guard<std::mutex>l(mutex);file=path;}active=id;
   if(file.empty()){state=State::Idle;continue;}Decode(file,id);FlushBookmark(true);
   if(Same(id)&&state==State::Idle&&!preview&&!ringtone&&(repeat.load()!=0||shuffle)){
    std::lock_guard<std::mutex>l(mutex);int n=tracks.size();if(n){int next=repeat==2?current.load():shuffle?(int)((random>>8)%n):(current+1)%n;path=tracks[next].path;current=next;position=0;duration=0;paused=false;seek=-1;RememberLocked();++epoch;}
   }
  }
 }
};
LocalMusicPlayer::LocalMusicPlayer():impl_(new Impl){
 pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,65536);
 int rc=hi_task_create_legacy(&thread,&attr,[](void*p)->void*{static_cast<Impl*>(p)->Run();return nullptr;},impl_);pthread_attr_destroy(&attr);
 if(!rc)pthread_detach(thread);else{impl_->alive=false;impl_->state=State::Error;impl_->status="无法创建音频线程";}
}
LocalMusicPlayer::~LocalMusicPlayer(){Stop();impl_->alive=false;/* singleton lives until process exit */}
LocalMusicPlayer &LocalMusicPlayer::Instance(){static LocalMusicPlayer player;return player;}
bool LocalMusicPlayer::ScanLibrary(){
 if(!metalio_sdcard_is_mounted()&&metalio_sdcard_mount_ro()<0){impl_->Status("未检测到 SD 卡");return false;}
 impl_->LoadBookmark();
 struct stat st{};if(stat(kMusicRoot,&st)<0&&metalio_sdcard_ensure_rw()==0)(void)mkdir(kMusicRoot,0775);
 std::vector<Track> found;ScanDir(kMusicRoot,found,0);std::sort(found.begin(),found.end(),[](const Track&a,const Track&b){return strcasecmp(a.title.c_str(),b.title.c_str())<0;});
 std::lock_guard<std::mutex>l(impl_->mutex);std::string path=impl_->path;impl_->tracks.swap(found);int index=-1;
 for(size_t i=0;i<impl_->tracks.size();i++)if(impl_->tracks[i].path==path)index=i;impl_->current=index;
 if(impl_->state!=State::Playing&&impl_->state!=State::Paused)impl_->status=impl_->tracks.empty()?"未找到歌曲，请放入 /Music":"点击歌曲开始播放";return !impl_->tracks.empty();
}
size_t LocalMusicPlayer::Count()const{std::lock_guard<std::mutex>l(impl_->mutex);return impl_->tracks.size();}
std::string LocalMusicPlayer::Title(size_t i)const{std::lock_guard<std::mutex>l(impl_->mutex);return i<impl_->tracks.size()?impl_->tracks[i].title:"";}
bool LocalMusicPlayer::FindLocalTrack(const std::string &query,std::string &path,std::string &title){
 auto key=[](const std::string &s){std::string out;for(unsigned char c:s){if(c==' '||c=='\t'||c=='-'||c=='_'||c=='+'||c=='.')continue;out+=c>='A'&&c<='Z'?char(c+32):char(c);}return out;};
 const std::string needle=key(query);if(needle.empty())return false;
 if(!ScanLibrary())return false;
 std::lock_guard<std::mutex>l(impl_->mutex);int best=-1;size_t score=0;
 for(size_t i=0;i<impl_->tracks.size();++i){const auto &track=impl_->tracks[i];const auto candidate=key(track.title);
  const size_t pos=candidate.find(needle);if(pos==std::string::npos)continue;
  const size_t value=candidate==needle?100000:10000-std::min<size_t>(candidate.size(),9000);
  if(best<0||value>score){best=static_cast<int>(i);score=value;}
 }
 if(best<0)return false;path=impl_->tracks[best].path;title=impl_->tracks[best].title;return true;
}
std::string LocalMusicPlayer::CurrentTitle()const{std::lock_guard<std::mutex>l(impl_->mutex);int i=impl_->current;return i>=0&&(size_t)i<impl_->tracks.size()?impl_->tracks[i].title:"Hi 音乐";}
std::string LocalMusicPlayer::StatusText()const{std::lock_guard<std::mutex>l(impl_->mutex);return impl_->status;}
LocalMusicPlayer::State LocalMusicPlayer::GetState()const{return impl_->state;}
int LocalMusicPlayer::CurrentIndex()const{return impl_->current;}
void LocalMusicPlayer::PlayIndex(int i){std::string path;{std::lock_guard<std::mutex>l(impl_->mutex);if(i<0||(size_t)i>=impl_->tracks.size())return;path=impl_->tracks[i].path;}PlayFile(path);}
void LocalMusicPlayer::PlayFile(const std::string &path,std::function<bool()> allowed){RequestFile(path,allowed,false,false);}
void LocalMusicPlayer::RestorePlayback(std::function<bool()> allowed){
 std::string path;int at=0;
 {std::lock_guard<std::mutex> l(impl_->mutex);auto state=impl_->state.load();
  if(state==State::Playing||state==State::Loading)return;
  if(state==State::Paused&&!impl_->path.empty()&&(impl_->preview||impl_->ringtone))return;
  if(state==State::Paused&&!impl_->path.empty())return;
  path=impl_->remembered_path;at=impl_->remembered_position;}
 struct stat st{};if(!Impl::BookmarkPath(path)||stat(path.c_str(),&st)!=0||!S_ISREG(st.st_mode))return;
 // Restore the selection only. Opening the app must never acquire audio
 // focus or start the decoder; the user's play button starts from here.
 std::lock_guard<std::mutex> l(impl_->mutex);
 if((allowed&&!allowed())||impl_->state!=State::Idle||!impl_->path.empty())return;
 for(size_t i=0;i<impl_->tracks.size();++i)if(impl_->tracks[i].path==path){
  impl_->current=i;impl_->position=at;impl_->duration=impl_->remembered_duration;
  impl_->status="点击播放继续上次歌曲";break;
 }
}
void LocalMusicPlayer::RememberPlayback(){std::lock_guard<std::mutex> l(impl_->mutex);impl_->RememberLocked();}
void LocalMusicPlayer::RequestFile(const std::string &path,std::function<bool()> allowed,bool preview,bool ringtone,int start_ms){
 if(!impl_->alive)return;
 if((allowed&&!allowed())||(path.size()<8||memcmp(path.data(),"/sdcard/",8)!=0)||path.find("/../")!=std::string::npos||!IsSupported(path.c_str()))return;
 std::lock_guard<std::mutex>l(impl_->mutex);impl_->RememberLocked();int index=-1;for(size_t i=0;i<impl_->tracks.size();i++)if(impl_->tracks[i].path==path)index=i;
 if(index<0){std::string title=path.substr(path.rfind('/')+1);title.resize(title.rfind('.'));index=impl_->tracks.size();impl_->tracks.push_back({path,SanitizeUtf8(title)});}
 impl_->path=path;impl_->current=index;impl_->preview=preview;impl_->ringtone=ringtone;impl_->position=std::max(0,start_ms);impl_->duration=0;impl_->seek=start_ms>0?start_ms:-1;impl_->paused=false;impl_->scrubbing=false;impl_->state=State::Loading;impl_->status="正在准备播放";impl_->RememberLocked();++impl_->epoch;
}
void LocalMusicPlayer::PreviewFile(const std::string &path){impl_->noise=false;RequestFile(path,{},true,false);}
void LocalMusicPlayer::PlayRingtoneFile(const std::string &path){impl_->noise=false;RequestFile(path,{},false,true);}
void LocalMusicPlayer::Toggle(){auto s=GetState();if(s==State::Playing||s==State::Paused){impl_->paused=!impl_->paused;return;}
 std::string path;int at=0;{std::lock_guard<std::mutex> l(impl_->mutex);int i=impl_->current;if(i<0)i=0;
  if((size_t)i>=impl_->tracks.size())return;path=impl_->tracks[i].path;
  if(path==impl_->remembered_path)at=impl_->remembered_position;}
 RequestFile(path,{},false,false,at);
}
void LocalMusicPlayer::SetPaused(bool paused){impl_->paused=paused;if(paused&&GetState()==State::Playing)impl_->state=State::Paused;}
void LocalMusicPlayer::SeekTo(int ms){impl_->seek=std::max(0,std::min(DurationMillis(),ms));}
void LocalMusicPlayer::BeginScrub(){impl_->scrubbing=true;impl_->speed=0;}
void LocalMusicPlayer::ScrubTo(int ms,int velocity){impl_->speed=std::max(-3000,std::min(3000,velocity));SeekTo(ms);}
void LocalMusicPlayer::EndScrub(bool resume){impl_->scrubbing=false;impl_->speed=1000;impl_->paused=!resume;}
int LocalMusicPlayer::PositionMillis()const{return impl_->position;}
int LocalMusicPlayer::DurationMillis()const{return impl_->duration;}
void LocalMusicPlayer::SetVinylNoise(bool on){impl_->noise=on;}
bool LocalMusicPlayer::VinylNoise()const{return impl_->noise;}
void LocalMusicPlayer::SetRepeat(int mode){impl_->repeat=mode%3;}
int LocalMusicPlayer::RepeatMode()const{return impl_->repeat;}
void LocalMusicPlayer::SetShuffle(bool on){impl_->shuffle=on;}
bool LocalMusicPlayer::Shuffle()const{return impl_->shuffle;}
void LocalMusicPlayer::Next(){int n=Count();if(n)PlayIndex((CurrentIndex()+1+n)%n);}
void LocalMusicPlayer::Previous(){int n=Count();if(n)PlayIndex((CurrentIndex()-1+n)%n);}
bool LocalMusicPlayer::IsMuted()const{return impl_->muted;}
void LocalMusicPlayer::ToggleMute(){impl_->muted=!impl_->muted;}
void LocalMusicPlayer::Stop(){if(!impl_)return;{std::lock_guard<std::mutex>l(impl_->mutex);impl_->RememberLocked();impl_->path.clear();impl_->paused=false;impl_->scrubbing=false;impl_->seek=-1;++impl_->epoch;}impl_->state=State::Idle;}
void LocalMusicPlayer::StopRingtonePlayback(){
 if(!impl_)return;std::lock_guard<std::mutex>lock(impl_->mutex);if(!impl_->ringtone)return;
 impl_->path.clear();impl_->paused=false;impl_->scrubbing=false;impl_->seek=-1;++impl_->epoch;impl_->state=State::Idle;
}
void LocalMusicPlayer::StopPreviewPlayback(){
 if(!impl_)return;std::lock_guard<std::mutex>lock(impl_->mutex);if(!impl_->preview)return;
 impl_->path.clear();impl_->paused=false;impl_->scrubbing=false;impl_->seek=-1;++impl_->epoch;impl_->state=State::Idle;
}
