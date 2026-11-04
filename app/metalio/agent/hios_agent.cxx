// SPDX-License-Identifier: Apache-2.0
#include "hios_agent.h"
#include "mimo_voice.h"
#include "application.h"
#include "audio_service.h"
#include "home_screen.h"
#include "control_center.h"
#include "board_shim.h"
#include "SdCardManager.hpp"
#include "lvgl.h"
#include <mutex>
#include <memory>
#include <map>
#include <deque>
#include <atomic>
#include <dirent.h>
#include <sys/stat.h>
extern "C" {
#include "core/message_bus.h"
#include "core/agent_loop.h"
#include "core/session_mgr.h"
#include "core/memory_store.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "llm/llm_cache.h"
#include "tools/tool_registry.h"
#include "tools/tool_guard.h"
#include "tools/skill_loader.h"
#include "node/node_client.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "voice/volc_asr.h"
#include "voice/volc_tts.h"
}
extern "C" bool agent_shutdown_requested(void) { return false; }
namespace {
std::mutex init_lock, replies_lock;
std::atomic<bool> ready{false};
bool bus_initialized=false,core_initialized=false,dispatch_started=false,loop_started=false;
std::mutex config_lock;
std::mutex request_lock;
std::mutex capture_lock;
std::map<std::string,std::string> config_cache;
std::atomic<bool> busy{false};
std::deque<HiOSAgent::Reply> replies;
std::deque<HiOSAgent::Reply> transcripts;
std::atomic<bool> voice_busy{false},record_stop{false},voice_cancel{false};
std::atomic<bool> speaking{false};
struct VoiceJob { std::string session,text; };
void *RecordWorker(void *arg) {
 std::unique_ptr<VoiceJob> job(static_cast<VoiceJob*>(arg));
 auto& audio=Application::GetInstance().GetAudioService();
 std::vector<int16_t> pcm; pcm.reserve(16000*30);
 std::string text;bool recognized_ok=false;
 {
  std::unique_lock<std::mutex> capture(capture_lock, std::try_to_lock);
  if(!capture.owns_lock())text="语音输入忙，请先停止其它录音或蓝牙播放";
  else {
   while(!record_stop && !voice_cancel && pcm.size()<16000*30){std::vector<int16_t> frame;if(!audio.ReadAudioData(frame,16000,320)){text="麦克风读取失败";break;}pcm.insert(pcm.end(),frame.begin(),frame.end());}
  }
 }
 if(!voice_cancel && text.empty()) {
  if(pcm.size()<1600)text="录音太短，请按住说话";
  else {char recognized[4096]{};std::string backend=HiOSAgent::Config("voice_backend");voice_asr_set_backend(backend.empty()?"mimo":backend.c_str());if(voice_asr_recognize(reinterpret_cast<const unsigned char*>(pcm.data()),pcm.size()*2,recognized,sizeof(recognized))!=0)text="语音识别失败，请检查语音凭据与网络";else {text=recognized;recognized_ok=!text.empty();}}
 }
 if(!voice_cancel){std::lock_guard<std::mutex> guard(replies_lock);transcripts.push_back({job->session,text,recognized_ok});}
 voice_busy=false; return nullptr;
}
void *SpeakWorker(void *arg) {
 std::unique_ptr<VoiceJob> job(static_cast<VoiceJob*>(arg));
 std::string backend=HiOSAgent::Config("voice_backend");voice_tts_set_backend(backend.empty()?"mimo":backend.c_str());
 // Bound sentences separately so the complete response is audible without
 // allocating unbounded PCM or truncating the first sentence.
 size_t pos=0;bool failed=false;
 while(pos<job->text.size() && !voice_cancel) {
  size_t end=std::min(pos+240,job->text.size());
  while(end<job->text.size() && (static_cast<unsigned char>(job->text[end])&0xc0)==0x80)--end;
  std::string part=job->text.substr(pos,end-pos);
  std::vector<unsigned char> pcm(16000*2*30);size_t length=0;
  if(voice_tts_speak(part.c_str(),pcm.data(),pcm.size(),&length)!=0 || length==0){failed=true;break;}
  if(!voice_cancel){auto& audio=Application::GetInstance().GetAudioService();if(!audio.IsMuted()){speaking=true;audio.PlayLocalPcm(reinterpret_cast<const int16_t*>(pcm.data()),length/2);speaking=false;}}
  pos=end;
 }
 if(failed && !voice_cancel){std::lock_guard<std::mutex> guard(replies_lock);transcripts.push_back({job->session,"朗读失败，请检查语音 API Key、额度与网络",false});}
 speaking=false;voice_busy=false;return nullptr;
}
char *Tools() {
 return strdup(R"([{"name":"hios_open_app","description":"Open a HiOS application by its visible name or ID","input_schema":{"type":"object","properties":{"app":{"type":"string"}},"required":["app"]}},{"name":"hios_volume","description":"Set shared system volume from 0 to 100","input_schema":{"type":"object","properties":{"volume":{"type":"integer","minimum":0,"maximum":100}},"required":["volume"]}},{"name":"get_battery","description":"Read this device battery percent and whether it is charging. Use for 电量/电池.","input_schema":{"type":"object","properties":{}}}])");
}
struct OpenRequest { std::string app; std::atomic<int> result{0}; std::atomic<int> refs{2}; };
void Release(OpenRequest *r) { if (--r->refs == 0) delete r; }
int Execute(const char *name,const char *input,char *out,size_t cap) {
 cJSON *json=cJSON_Parse(input); if(!json)return -1;
 int ret=-1;
 if(!strcmp(name,"hios_open_app")) {
  auto *v=cJSON_GetObjectItem(json,"app");
  if(cJSON_IsString(v)) {
   auto *r=new OpenRequest; r->app=v->valuestring;
   lv_lock(); auto queued=lv_async_call([](void *p){auto *r=static_cast<OpenRequest*>(p);r->result=HomeScreen::LaunchApp(r->app.c_str())?1:-1;Release(r);},r);lv_unlock();
   if(queued==LV_RESULT_OK){for(int i=0;i<100 && r->result==0;++i)usleep(20000);ret=r->result==1?0:-1;}else Release(r);
   Release(r);
  }
 } else if(!strcmp(name,"hios_volume")) {
  auto *v=cJSON_GetObjectItem(json,"volume");
  if(cJSON_IsNumber(v) && v->valuedouble>=0 && v->valuedouble<=100){Application::GetInstance().GetAudioService().SetVolume(v->valueint);lv_lock();lv_async_call([](void*){ControlCenter_Refresh();},nullptr);lv_unlock();ret=0;}
 } else if(!strcmp(name,"get_battery") || !strcmp(name,"hios_battery")) {
  int level=0,charging=0,discharging=0;
  cJSON_Delete(json);
  if(metalio_board_peek_battery(&level,&charging,&discharging) ||
     metalio_board_get_battery(&level,&charging,&discharging))
   snprintf(out,cap,charging?"当前电量 %d%%，正在充电。":"当前电量 %d%%。",level);
  else snprintf(out,cap,"暂时读不到电量。");
  return 0;
 }
 cJSON_Delete(json);snprintf(out,cap,"{\"ok\":%s}",ret==0?"true":"false");return ret;
}
void *Dispatch(void*) {
 for(;;){agent_msg_t msg{};if(message_bus_pop_outbound(&msg,1000)!=0)continue;if(!strcmp(msg.channel,"hios")){std::lock_guard<std::mutex> guard(replies_lock);if(replies.size()>=16)replies.pop_front();replies.push_back({msg.chat_id,msg.content?msg.content:""});busy=false;}message_bus_msg_free(&msg);}
 return nullptr;
}
}
namespace HiOSAgent {
bool Initialize(std::string& error) {
 std::lock_guard<std::mutex> guard(init_lock);if(ready)return true;
 if(!SdCardManager::GetInstance().EnsureWritable()){error="无法打开可写的 microSD 卡，请检查卡片是否已插好";return false;}
 if(!core_initialized){
 if(config_store_init()!=0){error="Agent 存储初始化失败";return false;}
 char host[256]{};if(claw_config_get("llm_host",host,sizeof(host))!=0){const char *keys[]={"llm_host","llm_path","llm_port","model"};const char *values[]={"api.xiaomimimo.com","/v1/chat/completions","443","mimo-v2.6-flash"};if(claw_config_set_many(keys,values,4)!=0){error="初始配置写入失败，请检查 SD 卡";return false;}}
 if(!bus_initialized){if(message_bus_init()!=0){error="消息总线初始化失败";return false;}bus_initialized=true;}
 if(memory_store_init()!=0 || session_mgr_init()!=0 || http_proxy_init()!=0){error="Agent 存储或网络适配初始化失败";return false;}
 if(llm_proxy_init()!=0 || llm_router_init()!=0 || tool_registry_init()!=0 || tool_guard_init()!=0 || skill_loader_init()!=0 || agent_loop_init()!=0){error="Agent 核心初始化失败";return false;}
 llm_cache_init();tool_registry_register_provider("hios",Tools,Execute);node_client_init();hios_mimo_voice_register();volc_asr_register();volc_tts_register();
 {std::lock_guard<std::mutex> guard(config_lock);const char *keys[]={"api_key","model","llm_host","llm_path","llm_port","voice_backend","voice_api_key","voice_host","mimo_voice","volc_appkey","volc_token","volc_asr_cluster","volc_api_key","volc_speaker","gateway_host","gateway_port","gateway_token"};for(const char *key:keys){char value[512]{};claw_config_get(key,value,sizeof(value));config_cache[key]=value;}}
 core_initialized=true;
 }
 if(!dispatch_started){if(agent_task_create(Dispatch,"hios_agent_out",16384,nullptr,100)!=0){error="回复任务创建失败";return false;}dispatch_started=true;}
 if(!loop_started){if(agent_loop_start()!=0){error="Agent 任务创建失败";return false;}loop_started=true;}
 ready=true;return true;
}
std::string Config(const char *key){std::lock_guard<std::mutex> guard(config_lock);auto found=config_cache.find(key);return found==config_cache.end()?"":found->second;}
bool SaveConfiguration(const std::vector<std::pair<std::string,std::string>>& values,std::string& error){
 std::lock_guard<std::mutex> guard(request_lock);
 if(!ready || busy || voice_busy){error="请等待当前对话或语音处理结束";return false;}
 std::vector<const char*> keys,contents;
 for(const auto& item:values){
  const auto& key=item.first;const auto& value=item.second;
  size_t limit=(key=="model" || key=="mimo_voice" || key=="volc_speaker")?63:(key=="volc_token" || key=="volc_api_key" || key=="voice_api_key" || key=="gateway_token")?255:127;
  if(value.size()>limit || value.find('\r')!=std::string::npos || value.find('\n')!=std::string::npos){error="配置过长或含换行，请检查输入";return false;}
  if((key=="llm_host" || key=="voice_host" || key=="gateway_host") && value.find_first_of("/ :")!=std::string::npos){error="主机只填写域名或 IP，不含协议、路径或端口";return false;}
  if((key=="llm_host" || key=="model" || key=="llm_path" || key=="llm_port") && value.empty()){error="模型、接口和端口不能为空";return false;}
  if(key=="voice_backend" && !value.empty() && value!="mimo" && value!="volcengine"){error="语音后端请填写 mimo 或 volcengine";return false;}
  if(key=="llm_path" && !value.empty() && value[0]!='/'){error="接口路径需以 / 开头";return false;}
  if((key=="llm_port" || key=="gateway_port") && !value.empty()){char *end=nullptr;long port=strtol(value.c_str(),&end,10);if(*end || port<1 || port>65535){error="端口需为 1–65535";return false;}}
  keys.push_back(key.c_str());contents.push_back(value.c_str());
 }
 if(claw_config_set_many(keys.data(),contents.data(),keys.size())!=0){error="配置写入失败，请检查 SD 卡";return false;}
 {std::lock_guard<std::mutex> guard(config_lock);for(const auto& item:values)config_cache[item.first]=item.second;}
 if(llm_proxy_init()!=0 || llm_router_init()!=0){error="模型配置应用失败";return false;}
 return true;
}
bool SaveConfig(const char *key,const std::string& value){std::string error;std::vector<std::pair<std::string,std::string>> values;values.emplace_back(std::string(key),value);return SaveConfiguration(values,error);}
bool Send(const std::string& session,const std::string& text,std::string& error){
 if(!Initialize(error))return false;
 std::lock_guard<std::mutex> guard(request_lock);
 if(Config("api_key").empty()){error="请在设置中填写 MiMo API Key";return false;}
 if(session.empty() || session.size()>=64 || text.empty() || text.size()>4096){error="消息为空或过长";return false;}
 bool expected=false;if(!busy.compare_exchange_strong(expected,true)){error="正在处理上一条消息";return false;}
 agent_msg_t msg{};strlcpy(msg.channel,"hios",sizeof(msg.channel));strlcpy(msg.chat_id,session.c_str(),sizeof(msg.chat_id));msg.content=strdup(text.c_str());
 if(!msg.content || message_bus_push_inbound(&msg)!=0){message_bus_msg_free(&msg);busy=false;error="消息队列繁忙";return false;}return true;
}
bool Poll(Reply& reply){std::lock_guard<std::mutex> guard(replies_lock);if(replies.empty())return false;reply=std::move(replies.front());replies.pop_front();return true;}
bool Busy(){return busy;}
std::vector<std::string> Sessions(){std::vector<std::string> result;if(!ready)return result;DIR *dir=opendir(AGENT_SESSION_DIR);if(!dir)return result;while(auto *entry=readdir(dir)){std::string n=entry->d_name;if(n.compare(0,3,"tg_")==0 && n.size()>9 && n.compare(n.size()-6,6,".jsonl")==0)result.push_back(n.substr(3,n.size()-9));}closedir(dir);return result;}
std::string History(const std::string& session){std::vector<char> data(32768);if(!ready || session_get_history_json(session.c_str(),data.data(),data.size(),40)!=0)return "[]";return data.data();}
bool Clear(const std::string& session){return ready && !busy && session_clear(session.c_str())==0;}
bool StartGateway(std::string& error){if(!Initialize(error))return false;if(Config("gateway_host").empty() || Config("gateway_token").empty()){error="请先填写并保存 Gateway 主机与 Token";return false;}if(node_client_start()!=0){error="Gateway 连接任务启动失败，请检查配置";return false;}return true;}
void StopGateway(){if(ready)node_client_stop();}
bool StartRecording(const std::string& session,std::string& error) {
 std::lock_guard<std::mutex> guard(request_lock);
 if(!ready || busy){error="请等待当前处理完成";return false;}
 if(Config("voice_backend")=="volcengine") {if(Config("volc_appkey").empty() || Config("volc_token").empty()){error="请先配置语音识别 App Key 和 Token";return false;}}
 else if(Config("voice_api_key").empty() && Config("api_key").empty()){error="请先配置 MiMo API Key";return false;}
 bool expected=false;if(!voice_busy.compare_exchange_strong(expected,true)){error="语音正在处理中";return false;}
 record_stop=false;voice_cancel=false;
 auto *job=new VoiceJob{session,""};if(agent_task_create(RecordWorker,"hios_agent_mic",32768,job,100)!=0){delete job;voice_busy=false;error="录音任务创建失败";return false;}return true;
}
void StopRecording(bool cancel){if(cancel)voice_cancel=true;record_stop=true;}
bool PollTranscript(Reply& reply){std::lock_guard<std::mutex> guard(replies_lock);if(transcripts.empty())return false;reply=std::move(transcripts.front());transcripts.pop_front();return true;}
bool Speak(const std::string& text,std::string& error,const std::string& session){
 std::lock_guard<std::mutex> guard(request_lock);
 if(!ready){error="Agent 尚未初始化";return false;}
 auto& audio=Application::GetInstance().GetAudioService();
 if(audio.IsMuted()){error="系统处于静音，回复已显示";return false;}
 if(Config("voice_backend")=="volcengine"){if(Config("volc_api_key").empty()){error="请先配置语音合成 API Key";return false;}}
 else if(Config("voice_api_key").empty() && Config("api_key").empty()){error="请先配置 MiMo API Key";return false;}
 bool expected=false;if(!voice_busy.compare_exchange_strong(expected,true)){error="语音正在处理中";return false;}
 voice_cancel=false;auto *job=new VoiceJob{session,text};if(agent_task_create(SpeakWorker,"hios_agent_tts",32768,job,100)!=0){delete job;voice_busy=false;error="播放任务创建失败";return false;}return true;
}
void CancelVoice(){voice_cancel=true;record_stop=true;}
bool VoiceBusy(){return voice_busy;}
}
