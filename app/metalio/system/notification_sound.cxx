#include "hi_task_policy.h"
#include "notification_sound.h"
#include "clock_service.h"
#include "settings.h"
#include <arch/board/board.h>
#include "metalio_claw_4_board.h"
#include "application.h"
#include "audio_service.h"
#include "local_audio_decode.h"
#include "hi_music_screen/local_music_player.h"
#include "radio_screen/radio_player.h"
#include "music_screen/music_screen.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include <sched.h>
#include <metalio/metalio.h>
#include <nuttx/timers/pwm.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <mutex>
#include <atomic>
#include <cmath>
#include <vector>
namespace NotificationSound { namespace {
struct Data{std::mutex lock;bool started=false;unsigned serial=0;int pending=0,active_kind=0;std::string path;std::function<bool()> allowed;};
Data&D(){static auto*d=new Data;return *d;}
std::atomic<int> current_mode{-1};
std::atomic<uint32_t> alarm_session{0};
int Mode(){int mode=current_mode.load();if(mode>=0)return mode;Settings s("network");return s.GetInt("sound_mode",0);}
bool Valid(const std::string&p){return p.empty()||(p.compare(0,14,"/sdcard/Music/")==0&&p.find("/../")==std::string::npos&&p.size()<256);}
bool Current(unsigned id){std::function<bool()> allowed;{auto&d=D();std::lock_guard<std::mutex>l(d.lock);if(d.serial!=id)return false;allowed=d.allowed;}return !allowed||allowed();}
// Only SMS samples are attenuated: the mixer reserves half-scale for an
// existing music/radio stream, so a full-scale notification otherwise clips.
// Keep one decoded block for a fade at EOF/the eight-second preview limit.
class SmsEnvelope {
 std::vector<int16_t> tail;size_t samples=0;
public:
 bool Feed(const int16_t*p,size_t n,const std::function<bool(const int16_t*,size_t)>&sink){
  if(!tail.empty()&&!sink(tail.data(),tail.size()))return false;
  tail.resize(n);
  for(size_t i=0;i<n;++i){int32_t v=(int32_t)p[i]*45/100;
   if(samples<160)v=v*(int32_t)samples/160;
   tail[i]=(int16_t)v;++samples;
  }return true;
 }
 bool Finish(const std::function<bool(const int16_t*,size_t)>&sink){
  const size_t fade=std::min<size_t>(160,tail.size());
  for(size_t i=0;i<fade;++i)tail[tail.size()-fade+i]=(int16_t)((int32_t)tail[tail.size()-fade+i]*(int32_t)(fade-1-i)/(int32_t)fade);
  const bool ok=tail.empty()||sink(tail.data(),tail.size());tail.clear();return ok;
 }
};
// PWM1 shares its clock with the backlight. Keep the established frequency,
// leave its descriptor open and never stop/reconfigure the shared peripheral.
void Motor(bool on){static int fd=-1;static bool started=false;if(fd<0)fd=open("/dev/pwm1",O_WRONLY);if(fd<0)return;pwm_info_s info{};info.frequency=BACKLIGHT_FREQ_HZ;info.duty=on?60u*65535u/100u:0;info.cpol=PWM_CPOL_NDEF;info.dcpol=PWM_DCPOL_NDEF;if(ioctl(fd,PWMIOC_SETCHARACTERISTICS,(unsigned long)&info)==0&&on&&!started)started=ioctl(fd,PWMIOC_START,0)==0;}
// An alarm is an isolated, prebuffered local session. It never decodes on
// LVGL, never shares the short SMS mixing lane, and cancellation never joins.
void PlayAlarm(unsigned id,const std::string&path){
 auto active=[&](){return Current(id)&&metalio_nt26_get_call_state()==METALIO_CALL_IDLE;};
 auto &app=Application::GetInstance();auto &audio=app.GetAudioService();
 if(!audio.IsStarted()||!active())return;
 // Preserve a running recorder/call; wait cooperatively rather than stealing RX.
 while(active()&&audio.HasExclusiveCapture())usleep(20000);
 if(!active())return;
 app.RequestVoiceUiDesired(false);audio.EnableVoiceProcessing(false);
 LocalMusicPlayer::Instance().Stop();RadioPlayer::Instance().Pause();MusicScreen::PauseForVoice();
 for(int i=0;i<200&&active()&&audio.HasLocalPlaybackSession();++i)usleep(10000);
 if(!active()||audio.HasLocalPlaybackSession())return;
 if(BluetoothScreen::PreparePlaybackRoute()<0||!active())return;
 if(metalio_audio_select_local()<0||metalio_nt26_call_control(3,1)<0||!active())return;
 uint32_t session=audio.BeginLocalPlayback(5760,true);alarm_session=session;
 auto send=[&](const int16_t*p,size_t n){for(int i=0;i<800&&active();++i){
  if(audio.LocalPlaybackWriteFailed(session))return false;
  if(audio.TryEnqueueLocalPcm(session,p,n)){audio.MuteWakeWordForMs(700);return true;}usleep(5000);
 }return false;};
 bool failed=false;const int16_t lead[960]{};
 for(int i=0;i<6&&active();++i)if(!send(lead,960)){failed=true;break;}
 while(active()&&!failed){
  bool played=!path.empty()&&DecodeLocalAudio(path,send,120000);
  if(!played&&active()){
   std::vector<int16_t>pcm(960);
   for(int block=0;block<16&&active();++block){for(int i=0;i<960;++i){double t=(block*960+i)/16000.;double env=std::min(1.,t/.02)*std::max(0.,1.-t/.96);pcm[i]=(int16_t)(5000*env*(sin(6.283185307*880*t)+.3*sin(6.283185307*1320*t)));}if(!send(pcm.data(),pcm.size())){failed=true;break;}}
  }
  // Queue the gap as PCM. Do not starve the I2S writer between repeats.
  const int16_t silence[960]{};for(int i=0;i<10&&active()&&!failed;++i)if(!send(silence,960))failed=true;
 }
 audio.EndLocalPlayback(session);uint32_t expected=session;alarm_session.compare_exchange_strong(expected,0);
}
void *Worker(void*){sched_param priority{};priority.sched_priority=CONFIG_METALIO_APP_PRIORITY+8;pthread_setschedparam(pthread_self(),SCHED_RR,&priority);for(;;){unsigned id;int kind;std::string path;{auto&d=D();std::lock_guard<std::mutex>l(d.lock);kind=d.pending;d.pending=0;d.active_kind=kind;path=d.path;id=d.serial;}
 if(!kind){usleep(20000);continue;}
 if(kind==2){PlayAlarm(id,path);auto&d=D();std::lock_guard<std::mutex>l(d.lock);if(d.serial==id)d.active_kind=0;continue;}
 if(metalio_nt26_get_call_state()!=METALIO_CALL_IDLE)continue;
 if(kind==1&&Mode()==1){Motor(true);for(int i=0;i<15&&Current(id)&&Mode()==1;++i)usleep(20000);if(metalio_nt26_get_call_state()==METALIO_CALL_IDLE)Motor(false);continue;}
 if(kind==1&&Mode()!=0)continue;
 auto&audio=Application::GetInstance().GetAudioService();if(!audio.IsStarted()||audio.HasExclusiveCapture())continue;
 const uint32_t session=audio.BeginNotification();
 auto active=[&](){return Current(id)&&!audio.HasExclusiveCapture()&&metalio_nt26_get_call_state()==METALIO_CALL_IDLE&&(kind!=1||Mode()==0);};
 auto send_raw=[&](const int16_t*p,size_t n){for(int i=0;i<400&&active();++i){if(audio.EnqueueNotification(session,p,n)){audio.MuteWakeWordForMs(700);return true;}usleep(5000);}return false;};
 SmsEnvelope envelope;
 auto send=[&](const int16_t*p,size_t n){return kind==1?envelope.Feed(p,n,send_raw):send_raw(p,n);};
 do{bool played=!path.empty()&&DecodeLocalAudio(path,send,kind==1?8000:120000);
  if(!played&&active()){std::vector<int16_t>pcm(960);for(int block=0;block<16&&active();++block){for(int i=0;i<960;++i){int at=block*960+i;double phase=at/16000.;double env=std::min(1.,phase/.02)*std::max(0.,1.-phase/.96);pcm[i]=(int16_t)(5000*env*(sin(6.283185307*880*phase)+.3*sin(6.283185307*1320*phase)));}if(!send(pcm.data(),pcm.size()))break;}}
  if(kind==1&&active())envelope.Finish(send_raw);
  for(int i=0;i<400&&active()&&!audio.NotificationDrained(session);++i)usleep(5000);
  if(kind==2&&active())for(int i=0;i<30&&active();++i)usleep(20000);
 }while(kind==2&&active());audio.EndNotification(session);
 }return nullptr;}
void Post(int kind,const std::string&path,std::function<bool()> allowed={}){auto&d=D();std::lock_guard<std::mutex>l(d.lock);if(!d.started){pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,32768);pthread_attr_setdetachstate(&a,PTHREAD_CREATE_DETACHED);pthread_t t;int rc=hi_task_create_legacy(&t,&a,Worker,nullptr);pthread_attr_destroy(&a);if(rc)return;d.started=true;}if(kind==1&&(d.pending==2||d.active_kind==2))return;d.pending=kind;d.path=path;d.allowed=allowed;++d.serial;}
}
std::string Selected(Kind k){Settings s("tones");return s.GetString(k==Kind::Sms?"sms":"alarm","");}
bool Select(Kind k,const std::string&p){if(!Valid(p)||(!p.empty()&&access(p.c_str(),R_OK)!=0))return false;Settings s("tones",true);if(!s.SetStringSaved(k==Kind::Sms?"sms":"alarm",p))return false;if(k==Kind::Alarm)ClockService::UseSystemAlarmTone();return true;}
void SetSystemMode(int mode){
 const int next=mode>=0&&mode<=2?mode:2;
 if(current_mode.exchange(next)==next)return;
 // Cancel the previous SMS alert, including one queued while the mode
 // changed. Alarm sessions retain their independent stop/snooze behavior.
 auto&d=D();std::lock_guard<std::mutex>l(d.lock);
 if(d.active_kind==1||d.pending==1){++d.serial;if(d.pending==1)d.pending=0;}
}
void Sms(){if(Mode()!=2)Post(1,Selected(Kind::Sms));}
void Alarm(const std::string&p,std::function<bool()> allowed){Post(2,p.empty()?Selected(Kind::Alarm):p,allowed);}
void StopAlarm(){{auto&d=D();std::lock_guard<std::mutex>l(d.lock);++d.serial;d.pending=0;d.active_kind=0;d.allowed={};}
 uint32_t session=alarm_session.exchange(0);if(session)Application::GetInstance().GetAudioService().EndLocalPlayback(session);
}
}
