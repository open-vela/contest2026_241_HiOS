#include "hi_task_policy.h"
#include "clock_service.h"
#include "notification_sound.h"
#include "digital_people_screen/xiaobai_resources.h"
#include "settings.h"
#include "cJSON.h"
#include <pthread.h>
#include <mutex>
#include <ctime>
#include <unistd.h>
#include <algorithm>
#include <map>
namespace ClockService {namespace {
int64_t Now(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return(int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
struct State{std::mutex lock;Snapshot v;bool started=false,dirty=false,tone_migrated=false;int next_id=1;int64_t sw_at=0,sw_base=0,timer_at=0,timer_left=60000,snooze_at=0,alert_at=0;std::map<int,int> fired;std::string active_tone,snooze_tone,snooze_label;int snooze_id=0;};
State&D(){static auto*s=new State;return *s;}
const char*S(cJSON*o,const char*k){auto*v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:"";}
int N(cJSON*o,const char*k){auto*v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsNumber(v)?(int)v->valuedouble:0;}
void Load(State&s){Settings cfg("clock");auto*j=cJSON_Parse(cfg.GetString("data","").c_str());
 if(j){cJSON*e=nullptr;cJSON_ArrayForEach(e,cJSON_GetObjectItemCaseSensitive(j,"alarms")){if(s.v.alarms.size()>=8)break;Alarm a;a.id=N(e,"id");a.hour=N(e,"h");a.minute=N(e,"m");a.days=N(e,"d")&127;a.enabled=N(e,"on");a.label=S(e,"n");a.tone=S(e,"t");if(a.id<1||a.hour<0||a.hour>23||a.minute<0||a.minute>59||a.label.size()>96||a.tone.size()>255)continue;s.next_id=std::max(s.next_id,a.id+1);s.v.alarms.push_back(a);}
 cJSON_ArrayForEach(e,cJSON_GetObjectItemCaseSensitive(j,"cities")){int i=(int)cJSON_GetNumberValue(e);if(i>=0&&i<14&&s.v.cities.size()<12&&std::find(s.v.cities.begin(),s.v.cities.end(),i)==s.v.cities.end())s.v.cities.push_back(i);}cJSON_Delete(j);}
 if(s.v.cities.empty())s.v.cities.push_back(0);if(!NotificationSound::Selected(NotificationSound::Kind::Alarm).empty())for(auto&a:s.v.alarms)if(!a.tone.empty()){a.tone.clear();s.dirty=true;}s.v.status="";
}
std::string Serialize(const Snapshot&v){auto*j=cJSON_CreateObject();auto*a=cJSON_AddArrayToObject(j,"alarms");for(const auto&x:v.alarms){auto*e=cJSON_CreateObject();cJSON_AddNumberToObject(e,"id",x.id);cJSON_AddNumberToObject(e,"h",x.hour);cJSON_AddNumberToObject(e,"m",x.minute);cJSON_AddNumberToObject(e,"d",x.days);cJSON_AddNumberToObject(e,"on",x.enabled);cJSON_AddStringToObject(e,"n",x.label.c_str());cJSON_AddStringToObject(e,"t",x.tone.c_str());cJSON_AddItemToArray(a,e);}a=cJSON_AddArrayToObject(j,"cities");for(int i:v.cities)cJSON_AddItemToArray(a,cJSON_CreateNumber(i));char*p=cJSON_PrintUnformatted(j);std::string out=p?p:"";cJSON_free(p);cJSON_Delete(j);return out;}
void*Worker(void*){auto&s=D();for(;;){
 // Publish an old Clock-only selection to Settings once SD installation has
 // finished. No SD reader may block the installer's RO-to-RW handoff.
 std::string legacy_tone;
 if(XiaobaiResources::Status()==2){std::lock_guard<std::mutex>l(s.lock);if(!s.tone_migrated){s.tone_migrated=true;for(const auto&a:s.v.alarms)if(!a.tone.empty()){legacy_tone=a.tone;break;}}}
 if(!legacy_tone.empty()&&NotificationSound::Selected(NotificationSound::Kind::Alarm).empty())NotificationSound::Select(NotificationSound::Kind::Alarm,legacy_tone);
 bool save=false,ring=false,stop=false;Snapshot v;std::string tone;const auto shared_tone=NotificationSound::Selected(NotificationSound::Kind::Alarm);unsigned rev=0;int64_t alert_token=0;
 {std::lock_guard<std::mutex>l(s.lock);int64_t now=Now();time_t wall=time(nullptr);tm local{};localtime_r(&wall,&local);int day=(local.tm_year+1900)*400+local.tm_yday;
  if(s.v.alert&&now-s.alert_at>=120000){s.v.alert=0;++s.v.revision;stop=true;}
  if(s.snooze_at&&now>=s.snooze_at&&!s.v.alert){s.snooze_at=0;s.v.alert=s.snooze_id?s.snooze_id:-2;s.v.alert_label=s.snooze_label.empty()?"稍后提醒":s.snooze_label;tone=s.snooze_tone;s.alert_at=now;ring=true;++s.v.revision;}
  if(s.v.timer_running&&now>=s.timer_at){s.v.timer_running=false;s.timer_left=0;s.v.timer=0;s.v.alert=-1;s.v.alert_label="计时结束";s.alert_at=now;ring=true;++s.v.revision;}
  if(wall>1700000000&&!s.v.alert){for(auto&a:s.v.alarms)if(a.enabled&&a.hour==local.tm_hour&&a.minute==local.tm_min&&(!a.days||(a.days&(1<<local.tm_wday)))&&s.fired[a.id]!=day){s.fired[a.id]=day;s.v.alert=a.id;s.v.alert_label=a.label;tone=shared_tone.empty()?a.tone:shared_tone;s.alert_at=now;ring=true;if(!a.days){a.enabled=false;s.dirty=true;}++s.v.revision;break;}}
  if(ring){if(tone.empty())tone=shared_tone;alert_token=s.alert_at;s.active_tone=tone;}
  if(s.dirty){s.dirty=false;v=s.v;rev=s.v.revision;save=true;}
 }
 if(stop)NotificationSound::StopAlarm();if(ring){NotificationSound::Alarm(tone,[alert_token](){auto&s=D();std::lock_guard<std::mutex>l(s.lock);return s.v.alert&&s.alert_at==alert_token;});}
 if(save){Settings cfg("clock",true);bool ok=cfg.SetStringSaved("data",Serialize(v));std::lock_guard<std::mutex>l(s.lock);s.v.status=ok?"":"设置尚未保存，请减少条目或铃声路径长度";if(s.v.revision==rev)++s.v.revision;}
 usleep(100000);
 }return nullptr;}
}
void Init(){auto&s=D();std::lock_guard<std::mutex>l(s.lock);if(s.started)return;Load(s);pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,24576);pthread_attr_setdetachstate(&a,PTHREAD_CREATE_DETACHED);pthread_t t;int rc=hi_task_create_legacy(&t,&a,Worker,nullptr);pthread_attr_destroy(&a);s.started=!rc;if(rc)s.v.status="时钟服务启动失败";}
AlertSnapshot GetAlert(){auto&s=D();std::lock_guard<std::mutex>l(s.lock);AlertSnapshot v;v.alert=s.v.alert;v.alert_label=s.v.alert_label;return v;}
Snapshot Get(){Init();auto&s=D();std::lock_guard<std::mutex>l(s.lock);auto v=s.v;const auto shared=NotificationSound::Selected(NotificationSound::Kind::Alarm);for(auto&a:v.alarms)if(!shared.empty()||a.tone.empty())a.tone=shared;int64_t now=Now();v.stopwatch=s.sw_base+(v.stopwatch_running?now-s.sw_at:0);v.timer=v.timer_running?std::max((int64_t)0,s.timer_at-now):s.timer_left;return v;}
bool SaveAlarm(Alarm a){Init();{auto&s=D();std::lock_guard<std::mutex>l(s.lock);if(s.v.alarms.size()>=8&&std::find_if(s.v.alarms.begin(),s.v.alarms.end(),[&](const Alarm&x){return x.id==a.id;})==s.v.alarms.end())return false;}if(a.hour<0||a.hour>23||a.minute<0||a.minute>59||a.label.size()>96||a.tone.size()>255)return false;if(a.tone!=NotificationSound::Selected(NotificationSound::Kind::Alarm)&&!NotificationSound::Select(NotificationSound::Kind::Alarm,a.tone))return false;a.tone.clear();auto&s=D();std::lock_guard<std::mutex>l(s.lock);auto it=std::find_if(s.v.alarms.begin(),s.v.alarms.end(),[&](const Alarm&x){return x.id==a.id;});if(it==s.v.alarms.end()){if(s.v.alarms.size()>=8)return false;a.id=s.next_id++;s.v.alarms.push_back(a);}else *it=a;s.fired.erase(a.id);s.dirty=true;++s.v.revision;return true;}
void RemoveAlarm(int id){auto&s=D();std::lock_guard<std::mutex>l(s.lock);s.v.alarms.erase(std::remove_if(s.v.alarms.begin(),s.v.alarms.end(),[&](const Alarm&a){return a.id==id;}),s.v.alarms.end());s.dirty=true;++s.v.revision;}
void ToggleAlarm(int id){auto&s=D();std::lock_guard<std::mutex>l(s.lock);for(auto&a:s.v.alarms)if(a.id==id)a.enabled=!a.enabled;s.dirty=true;++s.v.revision;}
void Cities(const std::vector<int>&ids){auto&s=D();std::lock_guard<std::mutex>l(s.lock);s.v.cities=ids;s.dirty=true;++s.v.revision;}
void Stopwatch(int command){auto&s=D();std::lock_guard<std::mutex>l(s.lock);int64_t now=Now();if(command==1){s.v.stopwatch_running=false;s.sw_base=0;s.v.laps.clear();}else if(command==2){if(s.v.stopwatch_running){if(s.v.laps.size()==100)s.v.laps.erase(s.v.laps.begin());s.v.laps.push_back(s.sw_base+now-s.sw_at);}}else if(s.v.stopwatch_running){s.sw_base+=now-s.sw_at;s.v.stopwatch_running=false;}else{s.sw_at=now;s.v.stopwatch_running=true;}++s.v.revision;}
void Timer(int command,int64_t duration){auto&s=D();std::lock_guard<std::mutex>l(s.lock);int64_t now=Now();if(command==2){s.v.timer_running=false;s.timer_left=std::max((int64_t)1000,std::min((int64_t)86400000,duration));s.v.timer_total=s.timer_left;}else if(command==1){s.v.timer_running=false;s.timer_left=s.v.timer_total;}else if(s.v.timer_running){s.timer_left=std::max((int64_t)0,s.timer_at-now);s.v.timer_running=false;}else{if(s.timer_left<=0)s.timer_left=s.v.timer_total;s.timer_at=now+s.timer_left;s.v.timer_running=true;}++s.v.revision;}
void UseSystemAlarmTone(){Init();auto&s=D();std::lock_guard<std::mutex>l(s.lock);for(auto&a:s.v.alarms)a.tone.clear();s.dirty=true;++s.v.revision;}
void Dismiss(bool snooze){auto&s=D();{std::lock_guard<std::mutex>l(s.lock);if(!s.v.alert)return;if(snooze){s.snooze_tone=s.active_tone;s.snooze_label=s.v.alert_label;s.snooze_id=s.v.alert;}else{s.snooze_tone.clear();s.snooze_label.clear();s.snooze_id=0;}s.v.alert=0;s.snooze_at=snooze?Now()+300000:0;++s.v.revision;}NotificationSound::StopAlarm();}
}
