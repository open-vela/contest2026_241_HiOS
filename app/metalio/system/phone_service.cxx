#include "hi_task_policy.h"
#include "phone_service.h"
#include "settings.h"
#include "metalio_claw_4_board.h"
#include "application.h"
#include "audio_service.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "esp_audio_simple_player.h"
#include "radio_screen/radio_player.h"
#include "hi_music_screen/local_music_player.h"
#include <metalio/metalio.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <nuttx/timers/pwm.h>
#include <arch/board/board.h>

namespace PhoneService { namespace {
#include "phone_initials.inc"
std::mutex data_lock;
std::mutex identity_lock;
std::string identity_number,identity_name;
struct PhoneEvent { int state; char number[25]; };
pthread_mutex_t event_lock=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t event_ready=PTHREAD_COND_INITIALIZER;
PhoneEvent event_queue[32]{};
unsigned event_head=0,event_count=0;
bool event_worker_started=false;
bool event_worker_busy=false;
std::mutex ring_control_lock;
std::string current_number;
bool session=false,answered=false,outgoing=false;
int last_state=METALIO_CALL_IDLE;
time_t began=0,connected=0;
std::atomic<bool> ring_wanted{false},ring_worker{false},ring_decoding{false};
std::atomic<int> ring_rate{16000},ring_channels{1};
std::vector<Contact> contacts;
std::vector<Call> calls;
bool store_loaded=false,history_dirty=false;
unsigned long contact_generation=0,history_generation=0;
uint64_t next_call_id=0,session_call_id=0;
std::string Clean(std::string s){for(char &c:s)if(c=='\n'||c=='\r'||c=='\t'||c=='|')c=' ';return s;}
std::string MatchNumber(const std::string &number){
 std::string digits;for(char c:number)if(c>='0'&&c<='9')digits+=c;
 if(digits.size()==15&&digits.compare(0,4,"0086")==0)digits.erase(0,4);
 else if(digits.size()==13&&digits.compare(0,2,"86")==0)digits.erase(0,2);return digits;
}
// Caller text has its own short lock, so painting never waits for FAT/fsync.
void PublishIdentityLocked(){
 std::string name;auto key=MatchNumber(current_number);
 if(!key.empty())for(const auto &c:contacts)if(!c.name.empty()&&MatchNumber(c.number)==key){name=c.name;break;}
 std::lock_guard<std::mutex> lock(identity_lock);identity_number=current_number;identity_name=std::move(name);
}
unsigned long Checksum(const std::string &s){uint32_t h=2166136261u;for(unsigned char c:s){h^=c;h*=16777619u;}return h;}
bool ReadBank(const char *kind,char bank,std::string &payload,unsigned long &generation){
 char path[100];snprintf(path,sizeof(path),"/sdcard/Card/%s.%c",kind,bank);
 FILE *file=fopen(path,"rb");if(!file)return false;
 char header[120];unsigned long count=0,checksum=0,seq=0;
 bool ok=fgets(header,sizeof(header),file)&&sscanf(header,"HiOSPH1 %lu %lu %lx",&seq,&count,&checksum)==3&&count<=(strcmp(kind,"history")==0?2u*1024u*1024u:32768u);
 std::string data;if(ok){data.resize(count);ok=fread(&data[0],1,count,file)==count&&Checksum(data)==checksum;}
 fclose(file);if(ok){payload=std::move(data);generation=seq;}return ok;
}
bool WriteBank(const char *kind,const std::string &data,unsigned long &generation){
 if(data.size()>(strcmp(kind,"history")==0?2u*1024u*1024u:32768u)||metalio_sdcard_ensure_rw()<0)return false;
 if(mkdir("/sdcard/Card",0775)<0&&errno!=EEXIST)return false;
 char path[100],header[120];unsigned long next=generation+1;
 snprintf(path,sizeof(path),"/sdcard/Card/%s.%c",kind,next%2?'a':'b');
 FILE *file=fopen(path,"wb");if(!file)return false;
 int n=snprintf(header,sizeof(header),"HiOSPH1 %lu %lu %08lx\n",next,(unsigned long)data.size(),Checksum(data));
 bool ok=fwrite(header,1,n,file)==(size_t)n&&fwrite(data.data(),1,data.size(),file)==data.size();
 if(fflush(file)!=0)ok=false;if(fsync(fileno(file))!=0)ok=false;if(fclose(file)!=0)ok=false;
 if(ok)generation=next;return ok;
}
std::string ReadStore(const char *kind,unsigned long &generation){
 std::string a,b;unsigned long ga=0,gb=0;bool va=ReadBank(kind,'a',a,ga),vb=ReadBank(kind,'b',b,gb);
 if(va&&(!vb||ga>=gb)){generation=ga;return a;}if(vb){generation=gb;return b;}generation=0;return {};
}
bool StoreCalls(){
 std::string data;for(const auto &c:calls){char prefix[80];snprintf(prefix,sizeof(prefix),"%lld\t%d\t%s\t",(long long)c.when,c.seconds,c.kind.c_str());data+=prefix;data+=c.number;data+='\t';data+=std::to_string(c.id);data+='|';}
 return WriteBank("history",data,history_generation);
}
bool LoadStore(){
 if(store_loaded){if(history_dirty&&metalio_sdcard_is_mounted())history_dirty=!StoreCalls();return true;}
 if(!metalio_sdcard_is_mounted()&&metalio_sdcard_mount_ro()<0)return false;
 std::string data=ReadStore("contacts",contact_generation);size_t at=0;
 while(at<data.size()&&contacts.size()<100){size_t end=data.find('|',at),p=data.find('\t',at),q=p==std::string::npos?p:data.find('\t',p+1);
 if(end==std::string::npos)break;if(p<end&&q<end){size_t extra=data.find('\t',q+1);
   bool favorite=extra<end&&data.substr(extra+1,end-extra-1)=="1";
   contacts.push_back({data.substr(at,p-at),data.substr(p+1,q-p-1),data.substr(q+1,(extra<end?extra:end)-q-1),favorite});}at=end+1;}
 data=ReadStore("history",history_generation);at=0;
 while(at<data.size()){
  size_t end=data.find('|',at);if(end==std::string::npos)break;
  auto row=data.substr(at,end-at);at=end+1;
  // Earlier firmware lacked CONFIG_LIBC_SCANSET, so %[...] scanf silently
  // prevented persisted calls from loading. It is enabled in both configs;
  // Split the versioned tab-separated fields explicitly, including an
  // empty withheld number, and accept the older four-field format too.
  std::string fields[5];size_t pos=0;int count=0;
  while(count<5){size_t tab=row.find('\t',pos);fields[count++]=row.substr(pos,tab==std::string::npos?tab:tab-pos);if(tab==std::string::npos)break;pos=tab+1;}
  if(count<4||fields[0].empty()||fields[1].empty()||fields[3].size()>24)continue;
  if(fields[2]!="outgoing"&&fields[2]!="incoming"&&fields[2]!="missed")continue;
  char *tail=nullptr;errno=0;long long when=strtoll(fields[0].c_str(),&tail,10);
  if(errno||!tail||*tail)continue;
  errno=0;long seconds=strtol(fields[1].c_str(),&tail,10);if(errno||!tail||*tail||seconds<0||seconds>2147483647L)continue;
  uint64_t id=0;if(count==5&&!fields[4].empty()){errno=0;id=strtoull(fields[4].c_str(),&tail,10);if(errno||!tail||*tail)continue;}
  if(!id)id=++next_call_id;else next_call_id=std::max(next_call_id,id);
  calls.push_back({fields[3],fields[2],when,(int)seconds,id});
 }
 store_loaded=true;if(history_dirty)history_dirty=!StoreCalls();return true;
}
bool StoreContacts(){std::string data;for(const auto &c:contacts)data+=c.id+'\t'+Clean(c.name)+'\t'+c.number+'\t'+(c.favorite?"1":"0")+'|';return WriteBank("contacts",data,contact_generation);}
bool Number(const std::string &s){if(s.empty()||s.size()>24)return false;
 for(size_t i=0;i<s.size();i++)if(!(s[i]>='0'&&s[i]<='9')&&!(i==0&&s[i]=='+'))return false;return s!="+";}
int RingtoneOutput(uint8_t *data,int bytes,void*){
 if(!ring_wanted||!data||bytes<=0)return 0;
 auto &audio=Application::GetInstance().GetAudioService();
 int ch=std::max(1,std::min(2,ring_channels.load())),rate=std::max(8000,ring_rate.load());
 auto *pcm=(int16_t*)data;int frames=bytes/(2*ch),out=frames*16000/rate;
 int16_t block[480];int at=0;
 for(int i=0;i<out&&ring_wanted;i++){
   int src=std::min(frames-1,i*rate/16000);int sample=pcm[src*ch];
   if(ch==2)sample=(sample+pcm[src*ch+1])/2;block[at++]=(int16_t)sample;
   if(at==480||i==out-1){while(ring_wanted&&!audio.TryEnqueuePlaybackPcm(block,at))usleep(5000);at=0;}}
 return 0;
}
int RingtoneEvent(esp_asp_event_pkt_t *pkt,void*){
 if(pkt&&pkt->type==ESP_ASP_EVENT_TYPE_MUSIC_INFO&&pkt->payload){
   auto *m=(esp_asp_music_info_t*)pkt->payload;ring_rate=m->sample_rate;ring_channels=m->channels;
 } else if(pkt&&pkt->type==ESP_ASP_EVENT_TYPE_STATE&&pkt->payload){
   auto s=*(esp_asp_state_t*)pkt->payload;if(s==ESP_ASP_STATE_FINISHED||s==ESP_ASP_STATE_ERROR)ring_decoding=false;}
 return 0;
}

// Only RingWorker owns this handle and the incoming-call pulse pattern.
// Keep it open: closing PWM1 shuts down the shared LEDC/backlight block.
int call_motor_fd=-1;
bool call_motor_started=false,call_motor_on=false;
void SetIncomingVibration(bool on){
 if(on==call_motor_on)return;
 if(call_motor_fd<0&&on)call_motor_fd=open("/dev/pwm1",O_WRONLY);
 if(call_motor_fd<0)return;
 pwm_info_s info{};
 // Same timer frequency as the existing backlight and vibration app;
 // duty-only changes never retune the panel's shared PWM clock.
 info.frequency=BACKLIGHT_FREQ_HZ;
 info.duty=on?(60u*65535u/100u):0;
 info.cpol=PWM_CPOL_NDEF;info.dcpol=PWM_DCPOL_NDEF;
 if(ioctl(call_motor_fd,PWMIOC_SETCHARACTERISTICS,(unsigned long)&info)<0)return;
 call_motor_on=on;
 if(on&&!call_motor_started){
  if(ioctl(call_motor_fd,PWMIOC_START,0)>=0)call_motor_started=true;
  else{info.duty=0;(void)ioctl(call_motor_fd,PWMIOC_SETCHARACTERISTICS,(unsigned long)&info);call_motor_on=false;}
 }
}
uint64_t IncomingAlertMillis(){
 timespec now{};clock_gettime(CLOCK_MONOTONIC,&now);
 return (uint64_t)now.tv_sec*1000u+now.tv_nsec/1000000u;
}
void *RingWorker(void*){
 BluetoothScreen::BeginPhoneAudio();
 auto &player=LocalMusicPlayer::Instance();player.Stop();player.SetVinylNoise(false);
 auto &audio=Application::GetInstance().GetAudioService();bool started=false;int retry=0;
 // The ringtone player selects the guarded local path. A late ring worker
 // must never release NT26's route after the answer worker has acquired it.
 for(;;){
  int previous_mode=-1;uint64_t mode_began=0;
  while(ring_wanted){
   Settings settings("network",false);const int mode=settings.GetInt("sound_mode",0);
   const uint64_t now=IncomingAlertMillis();
   if(mode!=previous_mode){previous_mode=mode;mode_began=now;}
   // Ring=0 keeps the selected ringtone; Vibrate=1 pulses without audio;
   // Mute=2 (or an invalid saved mode) produces neither call alert.
   // Read the shared setting every tick so changing modes during a call
   // takes effect without restarting the phone app or the device.
   SetIncomingVibration(mode==1&&ring_wanted&&
      metalio_nt26_get_call_state()==METALIO_CALL_INCOMING&&(now-mode_began)%1400u<500u);
   bool audible=mode==0&&audio.GetVolume()>0;
   if(!audible){if(started){player.StopRingtonePlayback();started=false;}usleep(50000);continue;}
   auto state=player.GetState();
   if(!started||((state==LocalMusicPlayer::State::Idle||state==LocalMusicPlayer::State::Error||state==LocalMusicPlayer::State::Paused)&&++retry>=20)){
    player.PlayRingtoneFile(SelectedRingtone());started=true;retry=0;
   }
   audio.MuteWakeWordForMs(1500);usleep(50000);
  }
  SetIncomingVibration(false);
  if(started)player.StopRingtonePlayback();started=false;audio.ClearWakeWordMute();
  std::lock_guard<std::mutex>lock(ring_control_lock);if(ring_wanted)continue;ring_worker=false;if(metalio_nt26_get_call_state()==METALIO_CALL_IDLE)BluetoothScreen::EndPhoneAudio();return nullptr;
 }
}
void StartRing(){
 std::lock_guard<std::mutex> lock(ring_control_lock);
 ring_wanted=true;if(ring_worker.exchange(true))return;
 pthread_t t;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,16384);
 int rc=hi_task_create_legacy(&t,&a,RingWorker,nullptr);pthread_attr_destroy(&a);
 if(rc==0)pthread_detach(t);else{ring_wanted=false;ring_worker=false;}
}
void SaveCall(){
 LoadStore();time_t now=time(nullptr);
 if(!session_call_id)session_call_id=++next_call_id;
 Call record{current_number,outgoing?"outgoing":answered?"incoming":"missed",(int64_t)began,
      answered&&connected&&now>=connected?(int)(now-connected):0,session_call_id};
 bool updated=false;for(auto &saved:calls)if(saved.id==record.id){saved=record;updated=true;break;}
 if(!updated)calls.insert(calls.begin(),record);history_dirty=!StoreCalls();
}
}
std::string Initials(const std::string &name){
 std::string out;
 for(size_t i=0;i<name.size();){unsigned char c=name[i];uint32_t cp=0;size_t width=1;
   if(c<0x80)cp=c;else if((c&0xe0)==0xc0&&i+1<name.size()){width=2;cp=((c&31)<<6)|(name[i+1]&63);}
   else if((c&0xf0)==0xe0&&i+2<name.size()){width=3;cp=((c&15)<<12)|((name[i+1]&63)<<6)|(name[i+2]&63);}
   else{++i;continue;}i+=width;
   if(cp<128){if(std::isalnum((unsigned char)cp))out+=(char)std::toupper((unsigned char)cp);continue;}
   char initial='#';size_t lo=0,hi=sizeof(kPhoneInitials)/sizeof(kPhoneInitials[0]);
   while(lo<hi){size_t mid=(lo+hi)/2;if(kPhoneInitials[mid].codepoint<cp)lo=mid+1;else hi=mid;}
   if(lo<sizeof(kPhoneInitials)/sizeof(kPhoneInitials[0])&&kPhoneInitials[lo].codepoint==cp)initial=kPhoneInitials[lo].initial;
   if(out.empty()){switch(cp){case 0x66fe:initial='Z';break;case 0x5355:initial='S';break;case 0x4ec7:initial='Q';break;case 0x533a:initial='O';break;case 0x89e3:initial='X';break;case 0x67e5:initial='Z';break;}}
   out+=initial;
 }return out.empty()?"#":out;
}
bool Matches(const Contact &c,const std::string &q){if(q.empty())return true;if(c.name.find(q)!=std::string::npos||c.number.find(q)!=std::string::npos)return true;
 auto key=Initials(c.name);std::string query=q;for(char &x:query)if((unsigned char)x<128)x=std::toupper((unsigned char)x);return key.find(query)!=std::string::npos;}
std::vector<Contact> Contacts(){std::lock_guard<std::mutex> lock(data_lock);LoadStore();auto out=contacts;
 std::sort(out.begin(),out.end(),[](const Contact&a,const Contact&b){auto x=Initials(a.name),y=Initials(b.name);if(x[0]=='#'&&y[0]!='#')return false;if(x[0]!='#'&&y[0]=='#')return true;return x==y?a.name<b.name:x<y;});return out;}
bool SaveContact(const Contact &c){if(!Number(c.number)||c.name.empty()||c.name.size()>96)return false;
 std::lock_guard<std::mutex> lock(data_lock);if(!LoadStore()||!metalio_sdcard_is_mounted())return false;std::string id=c.id;
 if(id.empty()){for(int i=0;i<100;i++){bool exists=false;for(const auto &p:contacts)if(p.id==std::to_string(i))exists=true;if(!exists){id=std::to_string(i);break;}}}
 if(id.empty()||id.size()>2||id.find_first_not_of("0123456789")!=std::string::npos)return false;
 auto old=contacts;bool replaced=false;for(auto &p:contacts)if(p.id==id){p={id,Clean(c.name),c.number,c.favorite};replaced=true;break;}
 if(!replaced)contacts.push_back({id,Clean(c.name),c.number,c.favorite});if(StoreContacts()){PublishIdentityLocked();return true;}contacts=std::move(old);return false;}
void DeleteContact(const std::string &id){std::lock_guard<std::mutex> lock(data_lock);if(!LoadStore())return;auto old=contacts;
 for(auto i=contacts.begin();i!=contacts.end();++i)if(i->id==id){contacts.erase(i);break;}if(!StoreContacts())contacts=std::move(old);}
std::vector<Call> History(){std::lock_guard<std::mutex> lock(data_lock);LoadStore();return calls;}
bool DeleteHistory(uint64_t id){
 if(!id)return false;std::lock_guard<std::mutex> lock(data_lock);if(!LoadStore())return false;
 auto old=calls;bool found=false;for(auto i=calls.begin();i!=calls.end();++i)if(i->id==id){calls.erase(i);found=true;break;}
 if(!found)return false;if(StoreCalls()){history_dirty=false;return true;}calls=std::move(old);return false;
}
void FlushHistory(){
 // Include the just-ended call even if power-off follows the hangup event.
 for(int waited=0;waited<200;++waited){pthread_mutex_lock(&event_lock);bool pending=event_count||event_worker_busy;pthread_mutex_unlock(&event_lock);if(!pending)break;usleep(10000);}
 std::lock_guard<std::mutex> lock(data_lock);if(session)SaveCall();else if(history_dirty&&LoadStore())history_dirty=!StoreCalls();
}
void ClearHistory(){std::lock_guard<std::mutex> lock(data_lock);if(!LoadStore())return;auto old=calls;calls.clear();if(!StoreCalls())calls=std::move(old);else history_dirty=false;}
void DialStarted(const char *number){
 BluetoothScreen::BeginPhoneAudio();LocalMusicPlayer::Instance().Stop();RadioPlayer::Instance().Stop();
 std::lock_guard<std::mutex> lock(data_lock);current_number=number?number:"";session_call_id=0;outgoing=true;answered=false;session=true;began=time(nullptr);connected=0;PublishIdentityLocked();
}
void CallerNumber(const char *number){if(!number||!Number(number))return;std::lock_guard<std::mutex> lock(data_lock);current_number=number;LoadStore();PublishIdentityLocked();}
std::string IncomingNumber(){std::lock_guard<std::mutex> lock(identity_lock);return identity_number;}
std::string IncomingName(){std::lock_guard<std::mutex> lock(identity_lock);return identity_name;}
void StateChanged(int state){
 // Route acquisition precedes SD history/contact work and never blocks UI.
 if(state==METALIO_CALL_INCOMING||state==METALIO_CALL_DIALING||state==METALIO_CALL_ACTIVE||state==METALIO_CALL_HELD)
  BluetoothScreen::BeginPhoneAudio();
 {std::lock_guard<std::mutex> lock(data_lock);
  if(state==METALIO_CALL_INCOMING&&last_state!=state&&!session){session_call_id=0;session=true;outgoing=false;answered=false;began=time(nullptr);connected=0;}
  if((state==METALIO_CALL_ACTIVE||state==METALIO_CALL_HELD)&&!answered){answered=true;connected=time(nullptr);}
  if(state==METALIO_CALL_IDLE&&session){SaveCall();session=false;current_number.clear();}
  last_state=state;PublishIdentityLocked();
 }
 if(state==METALIO_CALL_INCOMING&&metalio_nt26_get_call_state()==METALIO_CALL_INCOMING)StartRing();else{ring_wanted=false;if(state==METALIO_CALL_IDLE)BluetoothScreen::EndPhoneAudio();}
}
void Init(){
 pthread_mutex_lock(&event_lock);
 if(!event_worker_started){
  pthread_t worker;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,24576);
  int rc=hi_task_create_legacy(&worker,&attr,[](void*)->void*{
   {std::lock_guard<std::mutex> lock(data_lock);LoadStore();PublishIdentityLocked();}
   for(;;){
    PhoneEvent event{};pthread_mutex_lock(&event_lock);
    while(!event_count){
     struct timespec deadline{};clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=2;
     int rc=pthread_cond_timedwait(&event_ready,&event_lock,&deadline);
     if(rc==ETIMEDOUT&&!event_count){
      pthread_mutex_unlock(&event_lock);
      // A ringtone/music file may briefly prevent upgrading the RO mount.
      // Retry after it closes, even when the phone screen is never opened.
      {std::lock_guard<std::mutex> lock(data_lock);if(history_dirty&&!session){history_dirty=!StoreCalls();if(!history_dirty)printf("PHONE_HISTORY_SAVED records=%u\n",(unsigned)calls.size());}}
      pthread_mutex_lock(&event_lock);
     }
    }
    event=event_queue[event_head];event_head=(event_head+1)%32;--event_count;event_worker_busy=true;pthread_mutex_unlock(&event_lock);
    if(event.state<0)CallerNumber(event.number);else StateChanged(event.state);
    pthread_mutex_lock(&event_lock);event_worker_busy=false;pthread_mutex_unlock(&event_lock);
   }return nullptr;
  },nullptr);pthread_attr_destroy(&attr);
  if(rc==0){pthread_detach(worker);event_worker_started=true;}
 }
 pthread_mutex_unlock(&event_lock);
}
void QueuePhoneEvent(int state,const char *number){
 Init();pthread_mutex_lock(&event_lock);
 // Only state transitions and caller changes are queued, not repeated RINGs.
 // Keep the newest information if a failing SD card stalls the worker.
 if(event_count==32){event_head=(event_head+1)%32;--event_count;}
 auto &event=event_queue[(event_head+event_count)%32];event.state=state;
 snprintf(event.number,sizeof(event.number),"%s",number?number:"");++event_count;
 pthread_cond_signal(&event_ready);pthread_mutex_unlock(&event_lock);
}
void PostStateChanged(int state){if(state!=METALIO_CALL_INCOMING)CancelRingtone();QueuePhoneEvent(state,nullptr);}
void PostCallerNumber(const char *number){
 if(!number||!Number(number))return;
 {std::lock_guard<std::mutex> lock(identity_lock);if(identity_number==number)return;identity_number=number;identity_name.clear();}
 QueuePhoneEvent(-1,number);
}
void CancelRingtone(){ring_wanted=false;}
void StopRingtone(){bool active=ring_worker.load();ring_wanted=false;
 for(int i=0;i<200&&ring_worker;i++)usleep(10000);
 if(active)Application::GetInstance().GetAudioService().ResetDecoder();}
std::vector<Tone> Ringtones(){
 std::vector<Tone> out; if(!metalio_sdcard_is_mounted()&&metalio_sdcard_mount_ro()<0)return out;
 struct stat music_dir{};if(stat("/sdcard/Music",&music_dir)<0){
  if(metalio_sdcard_ensure_rw()<0)return out;
  if(mkdir("/sdcard/Music",0775)<0&&errno!=EEXIST)return out;
 }
 DIR *d=opendir("/sdcard/Music");if(!d)return out;struct dirent *e;
 while((e=readdir(d))&&out.size()<256){std::string name=e->d_name;auto dot=name.rfind('.');if(dot==std::string::npos)continue;
   auto ext=name.substr(dot);if(strcasecmp(ext.c_str(),".mp3")&&strcasecmp(ext.c_str(),".wav"))continue;
   std::string path="/sdcard/Music/"+name;struct stat st{};if(stat(path.c_str(),&st)==0&&S_ISREG(st.st_mode))out.push_back({name.substr(0,dot),path});}
 closedir(d);std::sort(out.begin(),out.end(),[](const Tone&a,const Tone&b){return a.title<b.title;});return out;
}
std::string SelectedRingtone(){Settings s("phone",false);return s.GetString("ringtone","/sdcard/Music/olivia_parker-chime-alert-demo-309545.mp3");}
bool SelectRingtone(const std::string &path){if((path.size()<14||memcmp(path.data(),"/sdcard/Music/",14)!=0)||path.find("/../")!=std::string::npos)return false;
 struct stat st{};if(stat(path.c_str(),&st)<0||!S_ISREG(st.st_mode))return false;Settings s("phone",true);return s.SetStringSaved("ringtone",path);}
}
