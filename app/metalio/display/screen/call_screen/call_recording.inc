// Local microphone recording. The modem voice path is analog: this is not
// advertised as a digital recording of both parties.
std::atomic<bool> s_call_record_running{false},s_call_record_stop{false};
std::mutex s_call_record_mutex;
std::string s_call_record_status;
void CallRecordStatus(const char *text){std::lock_guard<std::mutex> lock(s_call_record_mutex);s_call_record_status=text;}
std::string CallRecordStatus(){std::lock_guard<std::mutex> lock(s_call_record_mutex);return s_call_record_status;}
void CallWavHeader(uint8_t *h,uint32_t bytes){
 memset(h,0,44);memcpy(h,"RIFF",4);memcpy(h+8,"WAVEfmt ",8);memcpy(h+36,"data",4);
 auto u16=[&](int i,uint16_t v){h[i]=v;h[i+1]=v>>8;};
 auto u32=[&](int i,uint32_t v){for(int b=0;b<4;b++)h[i+b]=v>>(b*8);};
 u32(4,bytes+36);u32(16,16);u16(20,1);u16(22,1);u32(24,16000);u32(28,32000);u16(32,2);u16(34,16);u32(40,bytes);
}
// Separate storage from capture: FAT writes must not hold up microphone DMA.
// This buffer belongs only to the current call recording, never AI or music.
struct CallRecordingWriter {
 FILE *file;std::mutex lock;std::vector<int16_t> ring;
 size_t read=0,write=0,count=0;bool done=false,failed=false;uint32_t written=0;
 explicit CallRecordingWriter(FILE*f):file(f),ring(64000){}
 bool Push(const std::vector<int16_t>&pcm){std::lock_guard<std::mutex>guard(lock);
  if(failed||pcm.size()>ring.size()-count)return false;
  for(auto sample:pcm){ring[write]=sample;write=(write+1)%ring.size();}count+=pcm.size();return true;}
 static void*Run(void*arg){auto*w=static_cast<CallRecordingWriter*>(arg);std::vector<int16_t>batch(3840);
  for(;;){size_t n;bool done;{std::lock_guard<std::mutex>guard(w->lock);n=std::min(batch.size(),w->count);
    for(size_t i=0;i<n;++i){batch[i]=w->ring[w->read];w->read=(w->read+1)%w->ring.size();}w->count-=n;done=w->done;}
   if(n){size_t at=0;while(at<n){size_t got=fwrite(batch.data()+at,sizeof(int16_t),n-at,w->file);
     if(!got){std::lock_guard<std::mutex>guard(w->lock);w->failed=true;return nullptr;}at+=got;}w->written+=n;}
   else if(done)break;else usleep(5000);
  }return nullptr;
 }
};
void *CallRecordWorker(void*){
 auto &audio=Application::GetInstance().GetAudioService();bool wake=audio.IsWakeWordRunning();
 FILE *file=nullptr;uint32_t bytes=0;bool ok=false;char path[192]{};
 if(!audio.IsStarted()||audio.IsAudioProcessorRunning()){CallRecordStatus("录音未开始：麦克风正在使用");s_call_record_running=false;return nullptr;}
 if(!audio.BeginExclusiveCapture()){CallRecordStatus("麦克风尚未就绪，请重试");s_call_record_running=false;return nullptr;}
 int policy;sched_param priority{};if(pthread_getschedparam(pthread_self(),&policy,&priority)==0){priority.sched_priority=std::max(priority.sched_priority,CONFIG_METALIO_APP_PRIORITY+10);(void)pthread_setschedparam(pthread_self(),SCHED_RR,&priority);}
 audio.EnableWakeWordDetection(false);
 auto *codec=audio.GetCodec();
 bool headset=BluetoothScreen::IsHeadsetCaptureReady();
 // Match the known recording PCM packing instead of AI's speech calibration.
 // No modem mux, sample clock, speaker route or call volume is changed here.
 if(codec)codec->SetRecordingInput(true);
 std::vector<char> file_buffer(32768);
 do{
  if(metalio_sdcard_ensure_rw()<0||(mkdir("/sdcard/recordings",0775)<0&&errno!=EEXIST)){CallRecordStatus("无法访问 SD 卡录音目录");break;}
  time_t now=time(nullptr);struct tm date{};localtime_r(&now,&date);char stamp[64];
  strftime(stamp,sizeof(stamp),"%Y%m%d_%H%M%S",&date);
  int fd=-1;for(int i=0;i<100;i++){snprintf(path,sizeof(path),"/sdcard/recordings/CALL_MIC_%s_%02d.wav",stamp,i);fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);if(fd>=0||errno!=EEXIST)break;}
  if(fd<0){CallRecordStatus("无法创建录音文件");break;}
  file=fdopen(fd,"wb");if(!file){close(fd);CallRecordStatus("无法打开录音文件");break;}
  setvbuf(file,file_buffer.data(),_IOFBF,file_buffer.size());
  uint8_t header[44];CallWavHeader(header,0);if(fwrite(header,1,44,file)!=44){CallRecordStatus("SD 卡写入失败");break;}
  CallRecordingWriter writer(file);pthread_t writer_thread;pthread_attr_t attr;
  pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,16384);
  int rc=pthread_create(&writer_thread,&attr,CallRecordingWriter::Run,&writer);pthread_attr_destroy(&attr);
  if(rc){CallRecordStatus("录音存储任务未就绪");break;}
  CallRecordStatus(headset?"正在录音：蓝牙耳机麦克风":"正在录音：设备麦克风");ok=true;unsigned failures=0;
  std::vector<int16_t> raw,mono;raw.reserve(640);mono.resize(320);
  int32_t dc_x=0,dc_y=0;
  while(!s_call_record_stop&&(metalio_nt26_get_call_state()==METALIO_CALL_ACTIVE||metalio_nt26_get_call_state()==METALIO_CALL_HELD)&&bytes<32000u*1800u){
   bool current=BluetoothScreen::IsHeadsetCaptureReady();
   if(current!=headset){headset=current;if(codec)codec->SetRecordingInput(true);CallRecordStatus(headset?"正在录音：蓝牙耳机麦克风":"正在录音：设备麦克风");}
   if(!audio.ReadAudioData(raw,16000,320)){if(++failures>=250){ok=false;CallRecordStatus("麦克风无响应，录音已停止");break;}usleep(20000);continue;}
   failures=0;if(raw.size()!=320&&raw.size()!=640)continue;
   // Fixed microphone slot, continuous DC/rumble filter, unity gain.
   // Per-frame gain changes pumped background hiss into quiet syllables.
   for(size_t i=0;i<320;i++){
    int32_t x=raw[raw.size()==640?i*2:i];
    dc_y=x-dc_x+(int32_t)((int64_t)dc_y*31900/32768);dc_x=x;
    mono[i]=(int16_t)std::max(-30000,std::min(30000,(int)dc_y));
   }
   if(s_call_record_stop)break;
   if(!writer.Push(mono)){ok=false;CallRecordStatus("录音存储跟不上，请检查 SD 卡空间");break;}
   bytes+=320*2;
  }
  {std::lock_guard<std::mutex>guard(writer.lock);writer.done=true;}
  pthread_join(writer_thread,nullptr);bytes=writer.written*2;
  if(writer.failed)ok=false;
  CallWavHeader(header,bytes);
  if(fseek(file,0,SEEK_SET)||fwrite(header,1,44,file)!=44||fflush(file)||fsync(fileno(file)))ok=false;
 }while(false);
 if(file&&fclose(file))ok=false;
 if(codec)codec->SetRecordingInput(false);
 audio.EndExclusiveCapture();
 if(ok&&bytes)CallRecordStatus("录音已保存到 /recordings");
 else if(bytes)CallRecordStatus("录音保存不完整，请检查 SD 卡");
 else {if(path[0])unlink(path);if(ok)CallRecordStatus("未录入有效音频，未保存空文件");}
 if(wake)audio.EnableWakeWordDetection(true);
 s_call_record_running=false;return nullptr;
}
void ToggleCallRecording(){
 if(s_call_record_running){s_call_record_stop=true;CallRecordStatus("正在保存录音…");return;}
 if(metalio_nt26_get_call_state()!=METALIO_CALL_ACTIVE)return;
 if(s_call_record_running.exchange(true))return;s_call_record_stop=false;
 CallRecordStatus("正在准备麦克风录音…");pthread_t thread;pthread_attr_t attr;
 pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,32768);
 int rc=pthread_create(&thread,&attr,CallRecordWorker,nullptr);pthread_attr_destroy(&attr);
 if(!rc)pthread_detach(thread);else{s_call_record_running=false;CallRecordStatus("录音任务未能启动");}
}
