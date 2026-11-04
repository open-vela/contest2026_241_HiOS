#include "hi_task_policy.h"
#include <nuttx/config.h>
#include "sms_service.h"
#include "sms_pdu.h"
#include "notification_sound.h"
#include <metalio/metalio.h>
#include "cJSON.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <initializer_list>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" int metalio_app_sms_ensure_standby(void);

namespace SmsService { namespace {
constexpr size_t kStoreLimit=4*1024*1024;
// CLOCK_REALTIME is Beijing wall time when LOCALTIME is disabled. Stored
// records retain the existing format; only returned copies use UTC uniformly.
int64_t SystemToUtc(int64_t system){
#ifndef CONFIG_LIBC_LOCALTIME
 return system>0?system-8*3600:system;
#else
 return system;
#endif
}
int64_t UtcNow(){return SystemToUtc(time(nullptr));}

struct Storage {
 std::mutex lock;
 std::vector<Message> messages,pending_notifications,ready_notifications;
 std::string draft,status="正在读取短信…",sending_id,inbox_sim;
 std::vector<ThreadOptions> threads;
 unsigned revision=1,generation=0;
 bool started=false,refresh=true,dirty=false,loaded=false;
 bool inbox_seen=false,notify=false;
};
Storage &Data(){static auto *s=new Storage;return *s;}
bool Blocked(const Storage&s,const std::string&number){auto key=NormalizeNumber(number);for(const auto&t:s.threads)if(t.number==key)return t.blocked;return false;}
uint32_t Hash(const std::string &text){uint32_t h=2166136261u;for(unsigned char c:text){h^=c;h*=16777619u;}return h;}
std::string Id(const std::string &text){char out[24];std::snprintf(out,sizeof(out),"%08lx-%08lx",static_cast<unsigned long>(Hash(text)),static_cast<unsigned long>(Hash("sms:"+text)));return out;}
void StatusText(const std::string &text){auto&s=Data();std::lock_guard<std::mutex>l(s.lock);if(s.status!=text){s.status=text;++s.revision;}}
const char *String(cJSON *o,const char *key){auto *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
double Number(cJSON *o,const char *key){auto *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsNumber(v)?v->valuedouble:0;}
bool ReadBank(const char *path,unsigned &generation,std::string &body){
 FILE*f=fopen(path,"rb");if(!f)return false;struct stat st{};bool ok=fstat(fileno(f),&st)==0&&st.st_size>0&&static_cast<uint64_t>(st.st_size)<kStoreLimit;
 char header[96]{};unsigned gen=0,crc=0;
 if(ok)ok=fgets(header,sizeof(header),f)&&std::sscanf(header,"HIOS_SMS_1 %u %x",&gen,&crc)==2;
 if(ok){long offset=ftell(f);if(offset<0||offset>st.st_size)ok=false;else{body.resize(st.st_size-offset);ok=fread(&body[0],1,body.size(),f)==body.size()&&Hash(body)==crc;}}
 fclose(f);if(ok)generation=gen;return ok;
}
void Load(){auto&s=Data();if(s.loaded)return;
 if(!metalio_sdcard_is_mounted()&&metalio_sdcard_mount_ro()<0)return;
 unsigned ga=0,gb=0;std::string a,b;bool va=ReadBank("/sdcard/Messages/sms-a.dat",ga,a),vb=ReadBank("/sdcard/Messages/sms-b.dat",gb,b);
 if(!va&&!vb && (access("/sdcard/Messages/sms-a.dat",F_OK)==0||access("/sdcard/Messages/sms-b.dat",F_OK)==0)){
  StatusText("短信存储校验失败，原文件已保留");return;
 }
 auto *root=cJSON_Parse(va&&(!vb||static_cast<int32_t>(ga-gb)>0)?a.c_str():b.c_str());
 if((va||vb)&&!root){StatusText("短信存储解析失败，原文件已保留");return;}
 std::lock_guard<std::mutex>l(s.lock);s.loaded=true;s.generation=va&&(!vb||static_cast<int32_t>(ga-gb)>0)?ga:gb;
 if(root){s.draft=String(root,"draft");s.inbox_sim=String(root,"inbox_sim");
  auto*seen=cJSON_GetObjectItemCaseSensitive(root,"inbox_seen");
  // Older saved banks already contain a durable inbox baseline. Migrating
  // them must not suppress notification for a genuinely new message.
  s.inbox_seen=cJSON_IsNumber(seen)?seen->valuedouble!=0:(va||vb);
  auto*array=cJSON_GetObjectItemCaseSensitive(root,"messages");cJSON *entry=nullptr;
  cJSON_ArrayForEach(entry,array){Message m;m.id=String(entry,"id");m.number=String(entry,"number");m.text=String(entry,"text");m.error=String(entry,"error");m.when=Number(entry,"when");
   int state=Number(entry,"state");if(state<0||state>5)continue;m.state=static_cast<State>(state);m.unread=Number(entry,"unread")!=0;m.deleted=Number(entry,"deleted")!=0;
   if(m.id.empty()||m.number.empty())continue;
   if(m.state==State::Queued){m.state=State::Uncertain;m.error="发送曾被中断，请核实后再决定是否重发";s.dirty=true;}
   s.messages.push_back(std::move(m));
  }
  cJSON_ArrayForEach(entry,cJSON_GetObjectItemCaseSensitive(root,"threads")){
   ThreadOptions t;t.number=NormalizeNumber(String(entry,"n"));t.pinned=Number(entry,"p")!=0;t.blocked=Number(entry,"b")!=0;
   if(!t.number.empty()&&t.number.size()<=32&&s.threads.size()<1024)s.threads.push_back(t);
  }cJSON_Delete(root);
 }++s.revision;
}
bool Save(){auto&s=Data();std::vector<Message>messages;std::vector<ThreadOptions>threads;std::string draft,inbox_sim;bool inbox_seen;unsigned generation,revision;
 {std::lock_guard<std::mutex>l(s.lock);if(!s.loaded)return false;messages=s.messages;threads=s.threads;draft=s.draft;inbox_sim=s.inbox_sim;inbox_seen=s.inbox_seen;generation=s.generation+1;revision=s.revision;}
 if(metalio_sdcard_ensure_rw()<0|| (mkdir("/sdcard/Messages",0775)<0&&errno!=EEXIST))return false;
 auto *root=cJSON_CreateObject();auto *array=cJSON_AddArrayToObject(root,"messages");cJSON_AddStringToObject(root,"draft",draft.c_str());
 cJSON_AddStringToObject(root,"inbox_sim",inbox_sim.c_str());cJSON_AddNumberToObject(root,"inbox_seen",inbox_seen);
 for(const auto&m:messages){auto *entry=cJSON_CreateObject();cJSON_AddStringToObject(entry,"id",m.id.c_str());cJSON_AddStringToObject(entry,"number",m.number.c_str());cJSON_AddStringToObject(entry,"text",m.text.c_str());cJSON_AddStringToObject(entry,"error",m.error.c_str());cJSON_AddNumberToObject(entry,"when",m.when);cJSON_AddNumberToObject(entry,"state",static_cast<int>(m.state));cJSON_AddNumberToObject(entry,"unread",m.unread);cJSON_AddNumberToObject(entry,"deleted",m.deleted);cJSON_AddItemToArray(array,entry);}
 auto*thread_array=cJSON_AddArrayToObject(root,"threads");for(const auto&t:threads){auto*o=cJSON_CreateObject();cJSON_AddStringToObject(o,"n",t.number.c_str());cJSON_AddNumberToObject(o,"p",t.pinned);cJSON_AddNumberToObject(o,"b",t.blocked);cJSON_AddItemToArray(thread_array,o);}
 char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!json)return false;std::string body(json);cJSON_free(json);if(body.size()>kStoreLimit-128)return false;
 const char *path=generation&1?"/sdcard/Messages/sms-a.dat":"/sdcard/Messages/sms-b.dat";
 FILE *f=fopen(path,"wb");if(!f)return false;bool ok=std::fprintf(f,"HIOS_SMS_1 %u %08lx\n",generation,static_cast<unsigned long>(Hash(body)))>0&&fwrite(body.data(),1,body.size(),f)==body.size();
 if(fflush(f)!=0||fsync(fileno(f))!=0)ok=false;if(fclose(f)!=0)ok=false;
 if(ok){std::lock_guard<std::mutex>l(s.lock);s.generation=generation;if(s.revision==revision)s.dirty=false;}return ok;
}
struct Incoming {std::string key;SmsPdu::Part part;};
void Import(const std::string &response,const std::string &sim,std::vector<Incoming>&incoming){
 size_t pos=0;while((pos=response.find("+CMGL:",pos))!=std::string::npos){size_t begin=response.find('\n',pos);if(begin==std::string::npos)break;
  ++begin;while(begin<response.size()&&(response[begin]=='\r'||response[begin]=='\n'))++begin;size_t end=response.find_first_of("\r\n",begin);if(end==std::string::npos)end=response.size();
  const auto hex=response.substr(begin,end-begin);SmsPdu::Part part;if(SmsPdu::Decode(hex,part))incoming.push_back({Id(sim+":"+hex),std::move(part)});pos=end;
 }
}
bool SyncInbox(){
 const int standby=metalio_app_sms_ensure_standby();
 if(standby<0){std::printf("SMS_INBOX_DEFER standby=%d\n",standby);const char *text="正在恢复短信接收服务";if(standby==-EPERM)text="蜂窝待机已关闭，开启网络后接收短信";else if(standby==-EACCES)text="SIM 卡需要解锁，请在网络配置中输入 PIN";else if(standby==-ENODEV)text="暂未读到短信，请检查 4G 与 SIM 状态";StatusText(text);return false;}
 const int call_state=metalio_nt26_get_call_state();
 if(call_state!=METALIO_CALL_IDLE){std::printf("SMS_INBOX_DEFER call=%d\n",call_state);return false;}
 metalio_sim_profile_s profile{};int identity=metalio_nt26_get_sim_profile(&profile);if(identity<0)identity=metalio_nt26_read_sim_profile(&profile);std::string sim=profile.iccid;
 if(sim.empty()){std::printf("SMS_INBOX_DEFER identity=%d\n",identity);StatusText(identity==-EACCES?"SIM 卡需要解锁，请在网络配置中输入 PIN":"正在读取 SIM 身份，稍后自动刷新");return false;}
 auto *buffer=static_cast<char*>(std::malloc(128*1024));if(!buffer){StatusText("内存不足，稍后刷新");return false;}
 std::vector<Incoming>incoming;int successes=0,last_error=0;
 for(const char *storage:{"SM","ME"}){buffer[0]=0;int rc=metalio_nt26_sms_list(storage,buffer,128*1024);if(rc==0){++successes;Import(buffer,sim,incoming);}else {last_error=rc;if(std::strstr(buffer,"+CMGL:"))Import(buffer,sim,incoming);}}
 std::free(buffer);if(!successes){
  std::printf("SMS_INBOX_RETRY rc=%d\n",last_error);
  StatusText(last_error==-EAGAIN||last_error==-EBUSY?"正在后台同步短信":"暂未读到短信，请检查 4G 与 SIM 状态");return false;
 }
 auto&s=Data();std::lock_guard<std::mutex>l(s.lock);
 if(s.inbox_sim.empty()){s.inbox_sim=sim;s.dirty=true;++s.revision;}
 else if(s.inbox_sim!=sim){s.inbox_sim=sim;s.inbox_seen=false;s.dirty=true;++s.revision;}
 unsigned imported=0;
 for(size_t i=0;i<incoming.size();++i){const auto &item=incoming[i];const auto &part=item.part;Message message;
  if(part.total==1){message.id=item.key;message.text=part.text;}
  else {std::vector<const Incoming*>parts(part.total,nullptr);
   for(const auto&other:incoming)if(other.part.number==part.number&&other.part.reference==part.reference&&other.part.total==part.total&&other.part.when-part.when<86400&&part.when-other.part.when<86400)parts[other.part.sequence-1]=&other;
   bool missing=false;for(auto *entry:parts)if(!entry)missing=true;if(missing)continue;
   std::string key;for(auto *entry:parts){message.text+=entry->part.text;key+=entry->key;}message.id=Id(key);
  }
  bool known=false;for(const auto&m:s.messages)if(m.id==message.id){known=true;break;}if(known)continue;
  message.number=part.number;message.when=part.when?part.when:UtcNow();message.unread=!Blocked(s,message.number);
  if(s.inbox_seen&&message.unread){s.notify=true;if(s.pending_notifications.size()>=8)s.pending_notifications.erase(s.pending_notifications.begin());s.pending_notifications.push_back(message);}
  s.messages.push_back(std::move(message));s.dirty=true;++s.revision;++imported;
 }
 if(!s.inbox_seen){s.inbox_seen=true;s.dirty=true;++s.revision;}
 if(imported)std::printf("SMS_SYNC segments=%lu imported=%u\n",static_cast<unsigned long>(incoming.size()),imported);
 std::printf(successes==2?"SMS_INBOX_READY banks=%d imported=%u\n":"SMS_INBOX_PARTIAL banks=%d imported=%u\n",successes,imported);
 const char *status=successes==2?"短信已更新":"正在后台同步短信";
 if(s.status!=status){s.status=status;++s.revision;}
 return successes==2; // A busy bank remains pending even if the other bank succeeded.
}
std::string SendError(int rc,int cms){
 if(cms>=0){
  const char*why=cms==300?"4G 模块的短信服务处理失败":cms==301?"模块短信服务正被占用":cms==8||cms==21||cms==302?"SIM 或运营商未允许发送短信":
   cms==303?"模块固件不支持当前短信操作":
   cms==304||cms==305?"模块拒绝短信编码格式":
   cms==310||cms==313||cms==315?"请检查 SIM 卡状态":
   cms==311||cms==312||cms==316||cms==317||cms==318?"SIM 卡需要解锁":
   cms==314?"SIM 卡忙，请稍后手动重试":
   cms==320||cms==322?"模块短信存储空间不足":
   cms==330?"SIM 短信中心未配置，请核对运营商设置":
   cms==331?"短信网络尚未注册，请检查信号和 VoLTE 服务":
   cms==332?"运营商短信响应超时，请核实后再重发":"运营商返回短信错误";
  return std::string(why)+"（CMS "+std::to_string(cms)+"）";
 }
 if(rc==-EPERM)return "飞行模式或网络已关闭，未发送短信";
 if(rc==-EACCES)return "SIM 卡需要解锁，未发送短信";
 if(rc==-ENETDOWN)return "蜂窝短信服务尚未就绪，请确认 SIM 和信号可用";
 if(rc==-ENETUNREACH)return "蜂窝短信尚未注册，请确认 SIM、信号和短信服务可用";
 if(rc==-ENODATA)return "当前 SIM 和模块均未提供有效短信中心，未发送，请向运营商核对";
 if(rc==-EAGAIN||rc==-EBUSY)return "模块忙，未发送，请稍后手动重试";
 if(rc==-ETIMEDOUT)return "模块未返回短信输入提示，未发送，请稍后重试";
 if(rc==-EPROTO)return "模块短信输入提示不完整，未发送";
 if(rc==-EINVAL||rc==-EBADMSG)return "短信号码或编码格式无效，未发送";
 return "短信模块未接受请求（错误 "+std::to_string(-rc)+"），请检查 SIM 的短信服务";
}
void SendNext(){auto&s=Data();Message message;
 {std::lock_guard<std::mutex>l(s.lock);int64_t now=MonotonicMillis();auto i=std::find_if(s.messages.begin(),s.messages.end(),[&](const Message&m){return !m.deleted&&m.state==State::Queued&&now>=m.cancel_until_ms;});if(i==s.messages.end())return;message=*i;s.sending_id=message.id;}
 // Persist BEFORE Ctrl-Z. A restart can never silently resubmit an SMS.
 bool stored=Save();std::vector<std::string>pdus;int rc=-ENOSPC,cms=-1;size_t submitted=0;
 if(stored){
  rc=metalio_app_sms_ensure_standby();
  if(rc==0){
   if(!SmsPdu::Encode(message.number,message.text,Hash(message.id),pdus))rc=-EINVAL;
   else for(const auto&pdu:pdus){rc=metalio_nt26_sms_send_pdu_ex(pdu.c_str(),pdu.size()/2-1,&cms);if(rc<0)break;++submitted;}
  }
 }
 std::lock_guard<std::mutex>l(s.lock);s.sending_id.clear();auto i=std::find_if(s.messages.begin(),s.messages.end(),[&](const Message&m){return m.id==message.id;});if(i==s.messages.end())return;
 i->state=rc==0?State::Sent:rc==-EINPROGRESS||submitted?State::Uncertain:State::Failed;
 i->error=rc==0?"":!stored?"SD 卡保存失败，未发送":rc==-EINPROGRESS?"发送结果未确认，请勿重复发送":submitted?"部分短信已提交，请核实后再发送；"+SendError(rc,cms):SendError(rc,cms);
 s.dirty=true;++s.revision;
}
void *Worker(void*){
 unsigned modem_revision=metalio_nt26_sms_revision(),retry=0;
 int64_t due=0,earliest=0;bool pending=true;
 for(;;){
  Load();auto&s=Data();bool loaded,refresh,dirty;
  {std::lock_guard<std::mutex>l(s.lock);loaded=s.loaded;refresh=s.refresh;s.refresh=false;dirty=s.dirty;}
  const unsigned changed=metalio_nt26_sms_revision();
  const int64_t now=MonotonicMillis();
  if(refresh || changed!=modem_revision){pending=true;modem_revision=changed;}
  if(loaded){
   SendNext();
   if(now>=earliest && (pending || now>=due)){
    std::printf("SMS_INBOX_POLL\n");
    const bool complete=SyncInbox();const int64_t finished=MonotonicMillis();
    if(complete){pending=false;retry=0;due=finished+10000;earliest=finished+1000;}
    else {
     // Coalesce requests and retain BOTH SIM/ME banks. Non-periodic retry
     // intervals avoid repeatedly colliding with the AT/network poll cadence.
     static constexpr unsigned delays[]={2,5,11,17};
     const unsigned delay=delays[std::min(retry,3u)];if(retry<3)++retry;
     pending=true;earliest=finished+delay*1000;due=earliest;
    }
   }
   {std::lock_guard<std::mutex>l(s.lock);dirty=s.dirty;}
   bool saved=!dirty||Save();if(!saved)StatusText("短信尚未保存，请检查 SD 卡");
   bool notify=false;if(saved){std::lock_guard<std::mutex>l(s.lock);notify=s.notify;s.notify=false;
    for(auto&m:s.pending_notifications){if(s.ready_notifications.size()>=8)s.ready_notifications.erase(s.ready_notifications.begin());s.ready_notifications.push_back(std::move(m));}s.pending_notifications.clear();
   }if(notify)NotificationSound::Sms();
  }else if(!metalio_sdcard_is_mounted())StatusText("请插入 SD 卡保存短信");
  sleep(1);
 }
 return nullptr;
}
}
int64_t DisplayTime(int64_t utc){
#ifndef CONFIG_LIBC_LOCALTIME
 return utc>0?utc+8*3600:utc;
#else
 return utc;
#endif
}
std::string NormalizeNumber(const std::string &number){std::string n;for(char c:number)if(c>='0'&&c<='9')n+=c;if(n.size()==15&&n.compare(0,4,"0086")==0)n.erase(0,4);else if(n.size()==13&&n.compare(0,2,"86")==0)n.erase(0,2);return n.empty()?number:n;}
void Init(){auto&s=Data();std::lock_guard<std::mutex>l(s.lock);if(s.started)return;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,32768);pthread_attr_setdetachstate(&a,PTHREAD_CREATE_DETACHED);sched_param priority{};priority.sched_priority=CONFIG_METALIO_APP_PRIORITY+2;pthread_attr_setschedparam(&a,&priority);pthread_attr_setschedpolicy(&a,SCHED_RR);pthread_attr_setinheritsched(&a,PTHREAD_EXPLICIT_SCHED);pthread_t t;int rc=hi_task_create_legacy(&t,&a,Worker,nullptr);pthread_attr_destroy(&a);s.started=rc==0;if(rc){s.status="短信服务启动失败";++s.revision;}}
void Refresh(){Init();auto&s=Data();std::lock_guard<std::mutex>l(s.lock);s.refresh=true;}
unsigned Revision(){auto&s=Data();std::lock_guard<std::mutex>l(s.lock);return s.revision;}
std::string Status(){auto&s=Data();std::lock_guard<std::mutex>l(s.lock);return s.status;}
std::vector<Message> Messages(){auto&s=Data();std::lock_guard<std::mutex>l(s.lock);std::vector<Message>result;for(auto i=s.messages.rbegin();i!=s.messages.rend();++i)if(!i->deleted){result.push_back(*i);auto&view=result.back();if(view.state!=State::Received)view.when=SystemToUtc(view.when);}std::stable_sort(result.begin(),result.end(),[](const Message&a,const Message&b){return a.when>b.when;});return result;}
bool Send(const std::string &number,const std::string &text){std::vector<std::string>pdus;if(!SmsPdu::Encode(number,text,0,pdus))return false;auto&s=Data();std::lock_guard<std::mutex>l(s.lock);if(!s.loaded)return false;Message m;m.number=number;m.text=text;m.when=time(nullptr);m.id=Id(number+text+std::to_string(m.when)+std::to_string(s.revision));m.state=State::Queued;m.cancel_until_ms=MonotonicMillis()+4000;s.messages.push_back(std::move(m));s.draft.clear();s.dirty=true;++s.revision;return true;}
bool TakeNotification(Message &message,unsigned &count){auto&s=Data();std::lock_guard<std::mutex>l(s.lock);count=0;
 for(const auto&candidate:s.ready_notifications){for(const auto&m:s.messages)if(m.id==candidate.id&&!m.deleted&&m.unread&&!Blocked(s,m.number)){message=m;++count;break;}}
 s.ready_notifications.clear();return count!=0;
}
int64_t MonotonicMillis(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return(int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
bool CancelSend(const std::string&id){auto&s=Data();int64_t now=MonotonicMillis();std::lock_guard<std::mutex>l(s.lock);
 for(auto&m:s.messages)if(m.id==id&&!m.deleted&&m.state==State::Queued&&now<m.cancel_until_ms&&s.sending_id!=id){m.state=State::Cancelled;m.error="已取消发送";m.cancel_until_ms=0;s.dirty=true;++s.revision;return true;}return false;
}
std::vector<ThreadOptions> ConversationOptions(){auto&s=Data();std::lock_guard<std::mutex>l(s.lock);return s.threads;}
void SetThreadOption(const std::string&number,bool pinned,bool value){auto&s=Data();auto key=NormalizeNumber(number);if(key.empty()||key.size()>32)return;std::lock_guard<std::mutex>l(s.lock);
 auto i=std::find_if(s.threads.begin(),s.threads.end(),[&](const ThreadOptions&t){return t.number==key;});
 if(i==s.threads.end()){if(s.threads.size()>=1024)return;ThreadOptions t;t.number=key;s.threads.push_back(t);i=s.threads.end()-1;}
 if(pinned)i->pinned=value;else{i->blocked=value;if(value)for(auto&m:s.messages)if(NormalizeNumber(m.number)==key)m.unread=false;}
 s.dirty=true;++s.revision;
}
void SetPinned(const std::string&number,bool value){SetThreadOption(number,true,value);}
void SetBlocked(const std::string&number,bool value){SetThreadOption(number,false,value);}
void DeleteConversation(const std::string&number){auto&s=Data();auto key=NormalizeNumber(number);std::lock_guard<std::mutex>l(s.lock);
 for(auto&m:s.messages)if(!m.deleted&&NormalizeNumber(m.number)==key){m.deleted=true;m.unread=false;if(m.state==State::Queued&&s.sending_id!=m.id)m.state=State::Cancelled;s.dirty=true;++s.revision;}
}
void MarkRead(const std::string &number){auto&s=Data();auto key=NormalizeNumber(number);std::lock_guard<std::mutex>l(s.lock);for(auto&m:s.messages)if(m.unread&&NormalizeNumber(m.number)==key){m.unread=false;s.dirty=true;++s.revision;}}
void Delete(const std::string &id){auto&s=Data();std::lock_guard<std::mutex>l(s.lock);for(auto&m:s.messages)if(m.id==id){m.deleted=true;m.unread=false;s.dirty=true;++s.revision;}}
std::string Draft(){auto&s=Data();std::lock_guard<std::mutex>l(s.lock);return s.draft;}
void SaveDraft(const std::string &number,const std::string &text){auto&s=Data();std::lock_guard<std::mutex>l(s.lock);const auto draft=number+"\n"+text;if(s.draft!=draft){s.draft=draft;s.dirty=true;++s.revision;}}
}
