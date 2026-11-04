#include "hi_task_policy.h"
// SPDX-License-Identifier: Apache-2.0
// HiOS OpenClaw presentation; AI and audio remain in the existing Agent adapter.
#include "openclaw_screen.h"
#include "hios_agent.h"
#include "home_screen.h"
#include "hi_input_method.h"
#include "lvgl.h"
#include "cJSON_compat.h"
#include <atomic>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include <pthread.h>
#include <time.h>
#include <algorithm>
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
namespace {
lv_obj_t *s_root=nullptr,*content=nullptr,*body=nullptr,*status=nullptr,*input=nullptr,*keyboard=nullptr;
lv_timer_t *timer=nullptr;
lv_obj_t *header=nullptr,*composer=nullptr,*sessions_body=nullptr,*empty_chat=nullptr;
enum class Page { List, Chat, Settings };
Page page=Page::List;
std::string session;
bool auto_speak=true,recording=false;
std::atomic<bool> job_busy{false};
std::mutex result_lock;
struct Result { unsigned generation=0;std::string status,history;std::vector<std::string> sessions,config;bool ready=false; };
Result result;
unsigned generation=0;
struct Field {const char *label,*key;bool secret;};
const Field fields[]={
 {"MiMo API Key","api_key",true},{"模型","model",false},{"接口域名（不含 https://）","llm_host",false},{"接口路径","llm_path",false},{"端口","llm_port",false},
 {"语音后端（mimo 或 volcengine，空值默认 mimo）","voice_backend",false},{"MiMo 语音 Key（空值复用对话 Key）","voice_api_key",true},{"MiMo 语音域名（空值使用官方接口）","voice_host",false},{"MiMo 音色（空值默认 mimo_default）","mimo_voice",false},
 {"语音识别 App Key","volc_appkey",true},{"语音识别 Token","volc_token",true},{"识别资源 ID","volc_asr_cluster",false},{"语音合成 API Key","volc_api_key",true},{"音色","volc_speaker",false},
 {"Gateway 主机（可选）","gateway_host",false},{"Gateway 端口","gateway_port",false},{"Gateway Token","gateway_token",true}
};
std::vector<lv_obj_t*> editors;
void ShowList();void ShowChat();void ShowSettings();void Back();
// Keep every page inside the app-owned content container. The root also owns
// system chrome; cleaning it would invalidate the system's cached handles.
constexpr uint32_t kBackground=0x101117, kSurface=0x1b1e28, kBorder=0x303442;
constexpr uint32_t kAccent=0xbba8ff, kText=0xf1f2f7, kMuted=0xa5aab9;
lv_obj_t *Label(lv_obj_t *parent,const char *text,int width=630) {
 auto *o=lv_label_create(parent);
 lv_label_set_text(o,text);
 lv_obj_set_width(o,width);
 lv_label_set_long_mode(o,LV_LABEL_LONG_WRAP);
 lv_obj_set_style_text_font(o,&font_puhui_20_4,0);
 lv_obj_set_style_text_color(o,lv_color_hex(kText),0);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);
 return o;
}
lv_obj_t *Muted(lv_obj_t *parent,const char *text,int width=630) {
 auto *o=Label(parent,text,width);
 lv_obj_set_style_text_color(o,lv_color_hex(kMuted),0);
 return o;
}
lv_obj_t *Panel(lv_obj_t *parent,int width=672) {
 auto *o=lv_obj_create(parent);
 screen_strip_obj_chrome(o);
 lv_obj_set_size(o,width,LV_SIZE_CONTENT);
 lv_obj_set_style_bg_color(o,lv_color_hex(kSurface),0);
 lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
 lv_obj_set_style_radius(o,24,0);
 lv_obj_set_style_pad_all(o,20,0);
 lv_obj_set_style_pad_row(o,12,0);
 lv_obj_set_flex_flow(o,LV_FLEX_FLOW_COLUMN);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);
 return o;
}
lv_obj_t *Button(lv_obj_t *parent,const char *text,lv_event_cb_t cb,void *data=nullptr,int w=160) {
 auto *o=lv_button_create(parent);
 screen_strip_obj_chrome(o);
 lv_obj_set_size(o,w,60);
 lv_obj_set_style_radius(o,18,0);
 lv_obj_set_style_bg_color(o,lv_color_hex(kSurface),0);
 lv_obj_set_style_bg_color(o,lv_color_hex(0x373341),LV_STATE_PRESSED);
 lv_obj_set_style_border_width(o,1,0);
 lv_obj_set_style_border_opa(o,LV_OPA_COVER,0);
 lv_obj_set_style_border_color(o,lv_color_hex(kBorder),0);
 lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);
 auto *l=Label(o,text,w-20);
 lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);
 lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
 lv_obj_center(l);
 lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,data);
 return o;
}
void Primary(lv_obj_t *button) {
 lv_obj_set_style_bg_color(button,lv_color_hex(kAccent),0);
 lv_obj_set_style_bg_color(button,lv_color_hex(0xa18be9),LV_STATE_PRESSED);
 lv_obj_set_style_border_width(button,0,0);
 lv_obj_set_style_text_color(lv_obj_get_child(button,0),lv_color_hex(0x211936),0);
}
void StyleInput(lv_obj_t *o,int width=632,int height=58) {
 lv_obj_set_size(o,width,height);
 lv_obj_set_style_text_font(o,&font_puhui_20_4,0);
 lv_obj_set_style_text_color(o,lv_color_hex(kText),0);
 lv_obj_set_style_text_color(o,lv_color_hex(kMuted),LV_PART_TEXTAREA_PLACEHOLDER);
 lv_obj_set_style_bg_color(o,lv_color_hex(0x12151e),0);
 lv_obj_set_style_radius(o,14,0);
 lv_obj_set_style_border_width(o,1,0);
 lv_obj_set_style_border_color(o,lv_color_hex(kBorder),0);
 lv_obj_set_style_border_color(o,lv_color_hex(kAccent),LV_STATE_FOCUSED);
 lv_obj_set_style_outline_width(o,0,LV_STATE_FOCUSED);
 lv_obj_set_style_shadow_width(o,0,0);
 lv_obj_set_style_pad_all(o,14,0);
 lv_obj_set_scrollbar_mode(o,LV_SCROLLBAR_MODE_OFF);
}
void SetStatus(const char *text){if(status)lv_label_set_text(status,text);}
void SetKeyboardVisible(bool visible) {
 if(!keyboard)return;
 if(visible)lv_obj_remove_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
 else lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
 if(page==Page::Chat){lv_obj_set_height(body,visible?108:304);if(composer)lv_obj_set_y(composer,visible?252:450);}
 else if(page==Page::Settings)lv_obj_set_height(body,visible?196:430);
}
void Focus(lv_event_t *e) {
 if(!keyboard)return;
 auto *target=static_cast<lv_obj_t*>(lv_event_get_target(e));
 HiInputMethod_SetTextArea(keyboard,target);
 SetKeyboardVisible(true);
 if(page==Page::Settings)lv_obj_scroll_to_view_recursive(target,LV_ANIM_OFF);
}
void CreateKeyboard() {
 keyboard=HiInputMethod_Create(content);
 lv_obj_set_size(keyboard,720,350);
 lv_obj_set_align(keyboard,LV_ALIGN_TOP_LEFT);
 lv_obj_set_pos(keyboard,0,340);
 lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
 screen_swipe_back_ignore(keyboard,true);
}
void Frame(const char *title,const char *subtitle) {
 input=nullptr;keyboard=nullptr;composer=nullptr;sessions_body=nullptr;empty_chat=nullptr;
 editors.clear();lv_obj_clean(content);
 header=lv_obj_create(content);screen_strip_obj_chrome(header);
 lv_obj_set_size(header,720,120);
 lv_obj_set_style_bg_opa(header,LV_OPA_TRANSP,0);
 lv_obj_remove_flag(header,LV_OBJ_FLAG_SCROLLABLE);
 auto *back=Button(header,"返回",[](lv_event_t*){Back();},nullptr,92);
 lv_obj_set_pos(back,24,40);lv_obj_set_height(back,64);
 auto *name=Label(header,title,442);lv_obj_set_pos(name,138,38);
 lv_obj_set_style_text_font(name,&font_puhui_30_4,0);
 auto *detail=Muted(header,subtitle,442);lv_obj_set_pos(detail,138,80);
 body=lv_obj_create(content);screen_strip_obj_chrome(body);
 lv_obj_set_pos(body,24,132);lv_obj_set_size(body,672,490);
 lv_obj_set_style_bg_opa(body,LV_OPA_TRANSP,0);
 lv_obj_set_style_pad_row(body,16,0);lv_obj_set_flex_flow(body,LV_FLEX_FLOW_COLUMN);
 lv_obj_set_scroll_dir(body,LV_DIR_VER);
 status=Muted(content,"",672);lv_obj_set_pos(status,24,650);lv_obj_set_height(status,42);
}

enum class JobType { Initialize,List,Config,History,Save,Send,Clear,Gateway,StopGateway };
struct Job {JobType type;unsigned generation;std::string session,text;std::vector<std::string> values;};
void *Worker(void *arg) {
 std::unique_ptr<Job> j(static_cast<Job*>(arg));Result r;r.generation=j->generation;
 std::string error;
 if(!HiOSAgent::Initialize(error)){r.status=error;}
 else {
  switch(j->type){
   case JobType::Initialize:case JobType::List:r.sessions=HiOSAgent::Sessions();r.status="MiMo · 新建或选择会话";break;
   case JobType::Config:break;
   case JobType::History:r.history=HiOSAgent::History(j->session);break;
   case JobType::Save:{std::vector<std::pair<std::string,std::string>> values;for(size_t i=0;i<j->values.size();++i)values.emplace_back(fields[i].key,j->values[i]);r.status=HiOSAgent::SaveConfiguration(values,error)?"配置已保存":error;break;}
   case JobType::Send:r.status=HiOSAgent::Send(j->session,j->text,error)?"正在思考…":error;break;
   case JobType::Clear:r.status=HiOSAgent::Clear(j->session)?"会话已清空":"处理期间不能清空会话";r.history=HiOSAgent::History(j->session);break;
   case JobType::Gateway:r.status=HiOSAgent::StartGateway(error)?"Gateway 连接任务已启动，请在服务端确认设备上线":error;break;
   case JobType::StopGateway:HiOSAgent::StopGateway();r.status="Gateway 已断开";break;
  }
  if(j->type==JobType::Config)for(const auto& f:fields)r.config.push_back(HiOSAgent::Config(f.key));
 }
 r.ready=true;{std::lock_guard<std::mutex> guard(result_lock);result=std::move(r);}job_busy=false;return nullptr;
}
bool Start(JobType type,const std::string& text="",std::vector<std::string> values={}) {
 bool expected=false;if(!job_busy.compare_exchange_strong(expected,true)){SetStatus("正在处理中，请稍候");return false;}
 auto *j=new Job{type,generation,session,text,std::move(values)};pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,32768);pthread_t tid;int rc=hi_task_create_legacy(&tid,&attr,Worker,j);pthread_attr_destroy(&attr);if(rc){delete j;job_busy=false;SetStatus("无法创建任务");return false;}pthread_detach(tid);return true;
}
void NewConversation() {
 lv_async_call([](void*){
  if(!s_root || lv_screen_active()!=s_root)return;
  char id[64];snprintf(id,sizeof(id),"hios-%lld-%lu",static_cast<long long>(time(nullptr)),static_cast<unsigned long>(lv_tick_get()));
  session=id;ShowChat();
  // Start a new conversation with the shared pinyin composer ready to type.
  if(input && keyboard) lv_obj_send_event(input,LV_EVENT_FOCUSED,nullptr);
 },nullptr);
}
void EmptyChat() {
 empty_chat=Panel(body);
 auto *title=Label(empty_chat,"想聊些什么？",632);
 lv_obj_set_style_text_font(title,&font_puhui_30_4,0);
 lv_obj_set_style_text_color(title,lv_color_hex(kAccent),0);
 Muted(empty_chat,"和 MiMo 聊聊，也能控制你的设备。",632);
}
void Bubble(const char *role,const char *text) {
 if(empty_chat){lv_obj_delete(empty_chat);empty_chat=nullptr;}
 std::string shown=text?text:"";
 if(shown.size()>8192){size_t end=8192;while(end && (static_cast<unsigned char>(shown[end])&0xc0)==0x80)--end;shown.resize(end);shown+="\n（更多内容保存在 SD 卡会话中）";}
 while(lv_obj_get_child_count(body)>=50)lv_obj_delete(lv_obj_get_child(body,0));
 bool user=!strcmp(role,"user");
 auto *row=lv_obj_create(body);screen_strip_obj_chrome(row);
 lv_obj_set_size(row,672,LV_SIZE_CONTENT);lv_obj_set_style_bg_opa(row,LV_OPA_TRANSP,0);
 lv_obj_set_style_pad_left(row,user?72:0,0);
 lv_obj_remove_flag(row,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_set_flex_flow(row,LV_FLEX_FLOW_COLUMN);
 lv_obj_set_flex_align(row,LV_FLEX_ALIGN_START,user?LV_FLEX_ALIGN_END:LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
 auto *box=Panel(row,600);
 lv_obj_set_style_bg_color(box,lv_color_hex(user?0x342c49:kSurface),0);
 lv_obj_set_style_pad_all(box,18,0);lv_obj_set_style_pad_row(box,8,0);
 auto *who=Muted(box,user?"你":"Hi 万物",564);
 lv_obj_set_style_text_color(who,lv_color_hex(user?0xd2c3ff:kAccent),0);
 Label(box,shown.c_str(),564);
 lv_obj_scroll_to_view(row,LV_ANIM_OFF);
}
void RenderHistory(const std::string& text) {
 empty_chat=nullptr;lv_obj_clean(body);
 auto *json=cJSON_Parse(text.c_str());
 if(json){cJSON *item=nullptr;cJSON_ArrayForEach(item,json){auto *role=cJSON_GetObjectItem(item,"role");auto *value=cJSON_GetObjectItem(item,"content");if(cJSON_IsString(role)&&cJSON_IsString(value))Bubble(role->valuestring,value->valuestring);}cJSON_Delete(json);}
 if(lv_obj_get_child_count(body)==0)EmptyChat();
}
std::string SessionDate(const std::string& id) {
 long long timestamp=0;char result[80];
 if(sscanf(id.c_str(),"hios-%lld-",&timestamp)==1 && timestamp>1600000000){
  time_t value=static_cast<time_t>(timestamp);struct tm date{};localtime_r(&value,&date);
  snprintf(result,sizeof(result),"%04d.%02d.%02d  %02d:%02d",date.tm_year+1900,date.tm_mon+1,date.tm_mday,date.tm_hour,date.tm_min);
  return result;
 }
 return "已保存的对话";
}
void RenderSessions(const std::vector<std::string>& ids) {
 if(!sessions_body)return;
 lv_obj_clean(sessions_body);
 if(ids.empty()){
  auto *card=Panel(sessions_body);Label(card,"还没有历史对话",632);
  Muted(card,"点击「新建对话」，开始和 MiMo 聊聊。",632);return;
 }
 std::vector<std::string> sorted=ids;std::sort(sorted.begin(),sorted.end(),[](const std::string& a,const std::string& b){return a>b;});
 for(size_t i=0;i<sorted.size();++i){
  auto *data=new std::string(sorted[i]);
  auto *button=Button(sessions_body,"",[](lv_event_t *e){session=*static_cast<std::string*>(lv_event_get_user_data(e));lv_async_call([](void*){if(s_root && lv_screen_active()==s_root)ShowChat();},nullptr);},data,672);
  lv_obj_set_height(button,96);lv_obj_set_style_radius(button,22,0);
  auto *title=lv_obj_get_child(button,0);char name[40];snprintf(name,sizeof(name),"对话 %02lu",static_cast<unsigned long>(sorted.size()-i));
  lv_label_set_text(title,name);lv_obj_set_align(title,LV_ALIGN_TOP_LEFT);lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_LEFT,0);lv_obj_set_pos(title,22,16);lv_obj_set_width(title,540);
  auto *when=Muted(button,SessionDate(sorted[i]).c_str(),540);lv_obj_set_pos(when,22,52);
  auto *arrow=Label(button,">",24);lv_obj_set_style_text_color(arrow,lv_color_hex(kAccent),0);lv_obj_align(arrow,LV_ALIGN_RIGHT_MID,-22,0);
  lv_obj_add_event_cb(button,[](lv_event_t *e){delete static_cast<std::string*>(lv_event_get_user_data(e));},LV_EVENT_DELETE,data);
 }
}
void ShowList() {
 HiOSAgent::CancelVoice();recording=false;++generation;page=Page::List;
 Frame("Hi 万物","你的随身 AI 助手");
 auto *hero=Panel(body);lv_obj_set_height(hero,132);
 lv_obj_set_style_bg_color(hero,lv_color_hex(0x272338),0);
 auto *name=Label(hero,"和 MiMo 一起，轻松完成。",632);
 lv_obj_set_style_text_font(name,&font_puhui_30_4,0);
 Muted(hero,"聊想法、问问题，也能帮你控制设备。",632);
 auto *actions=lv_obj_create(body);screen_strip_obj_chrome(actions);lv_obj_set_size(actions,672,64);
 lv_obj_set_style_bg_opa(actions,LV_OPA_TRANSP,0);lv_obj_remove_flag(actions,LV_OBJ_FLAG_SCROLLABLE);
 auto *create=Button(actions,"+  新建对话",[](lv_event_t*){NewConversation();},nullptr,430);lv_obj_set_height(create,64);Primary(create);
 auto *settings=Button(actions,"配置助手",[](lv_event_t*){lv_async_call([](void*){if(s_root && lv_screen_active()==s_root)ShowSettings();},nullptr);},nullptr,226);lv_obj_set_pos(settings,446,0);lv_obj_set_height(settings,64);
 Muted(body,"最近的对话",672);
 sessions_body=lv_obj_create(body);screen_strip_obj_chrome(sessions_body);
 lv_obj_set_size(sessions_body,672,LV_SIZE_CONTENT);lv_obj_set_style_bg_opa(sessions_body,LV_OPA_TRANSP,0);
 lv_obj_set_flex_flow(sessions_body,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(sessions_body,12,0);
 lv_obj_remove_flag(sessions_body,LV_OBJ_FLAG_SCROLLABLE);
 if(lv_screen_active()==s_root)Start(JobType::List);
}
void ShowChat() {
 ++generation;page=Page::Chat;Frame("Hi 万物","MiMo  ·  对话与语音");lv_obj_set_height(body,304);
 auto *clear=Button(header,"清空",[](lv_event_t*){Start(JobType::Clear);},nullptr,102);lv_obj_set_pos(clear,594,42);
 EmptyChat();
 composer=lv_obj_create(content);screen_strip_obj_chrome(composer);lv_obj_set_pos(composer,24,450);lv_obj_set_size(composer,672,76);
 lv_obj_set_style_bg_opa(composer,LV_OPA_TRANSP,0);lv_obj_remove_flag(composer,LV_OBJ_FLAG_SCROLLABLE);
 input=lv_textarea_create(composer);HiInputMethod_Bind(input,true);StyleInput(input,546,76);
 screen_swipe_back_ignore(composer,true);
 lv_textarea_set_max_length(input,1024);lv_textarea_set_placeholder_text(input,"想问些什么…");lv_obj_add_event_cb(input,Focus,LV_EVENT_FOCUSED,nullptr);lv_obj_add_event_cb(input,Focus,LV_EVENT_CLICKED,nullptr);
 auto *send=Button(composer,"发送",[](lv_event_t*){
  if(!input||HiOSAgent::Busy())return;
  if(keyboard&&!HiInputMethod_Flush(keyboard))return;
  std::string text=lv_textarea_get_text(input);if(text.empty())return;
  if(Start(JobType::Send,text)){Bubble("user",text.c_str());lv_textarea_set_text(input,"");SetKeyboardVisible(false);}
 },nullptr,110);lv_obj_set_pos(send,562,0);lv_obj_set_height(send,76);Primary(send);
 auto *voice=Button(content,"按住说话 · 松开发送",[](lv_event_t*){},nullptr,442);lv_obj_set_pos(voice,24,540);lv_obj_set_height(voice,64);
 lv_obj_set_style_bg_color(voice,lv_color_hex(0x272338),0);lv_obj_set_style_border_color(voice,lv_color_hex(0x665583),0);
 lv_obj_set_style_bg_color(voice,lv_color_hex(0x584674),LV_STATE_PRESSED);
 lv_obj_add_event_cb(voice,[](lv_event_t*){std::string error;recording=HiOSAgent::StartRecording(session,error);SetStatus(recording?"正在听你说，松开发送（最长 30 秒）":error.c_str());},LV_EVENT_PRESSED,nullptr);
 lv_obj_add_event_cb(voice,[](lv_event_t*){if(recording){HiOSAgent::StopRecording();recording=false;SetStatus("正在识别语音…");}},LV_EVENT_RELEASED,nullptr);
 lv_obj_add_event_cb(voice,[](lv_event_t*){if(recording){HiOSAgent::StopRecording(true);recording=false;SetStatus("录音已取消");}},LV_EVENT_PRESS_LOST,nullptr);
 auto *stop=Button(content,"停止朗读",[](lv_event_t*){HiOSAgent::CancelVoice();SetStatus("语音已停止");},nullptr,214);lv_obj_set_pos(stop,482,540);lv_obj_set_height(stop,64);
 auto *speech=lv_checkbox_create(content);screen_strip_obj_chrome(speech);
 lv_checkbox_set_text(speech,"自动朗读回复");lv_obj_set_style_text_font(speech,&font_puhui_20_4,0);lv_obj_set_style_text_color(speech,lv_color_hex(kMuted),0);lv_obj_set_pos(speech,28,618);
 lv_obj_set_style_bg_color(speech,lv_color_hex(kAccent),LV_PART_INDICATOR|LV_STATE_CHECKED);lv_obj_set_style_border_color(speech,lv_color_hex(kAccent),LV_PART_INDICATOR);
 if(auto_speak)lv_obj_add_state(speech,LV_STATE_CHECKED);
 lv_obj_add_event_cb(speech,[](lv_event_t *e){auto_speak=lv_obj_has_state(static_cast<lv_obj_t*>(lv_event_get_target(e)),LV_STATE_CHECKED);if(!auto_speak)HiOSAgent::CancelVoice();},LV_EVENT_VALUE_CHANGED,nullptr);
 CreateKeyboard();Start(JobType::History);
}
void ShowSettings() {
 ++generation;page=Page::Settings;Frame("配置助手","设置一次，随时开始对话");lv_obj_set_height(body,430);
 const char *titles[]={"MiMo 模型","语音与朗读","备用语音服务","Gateway（可选）"};
 const char *notes[]={"填写 API Key，其余项目可保持默认。","默认使用 MiMo；语音 Key 留空时复用对话 Key。","仅在选择 volcengine 后端时需要填写。","上游协议 3，不保证兼容新版服务端。"};
 lv_obj_t *group=nullptr;size_t section=0;
 for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);++i){
  if(i==0 || i==5 || i==9 || i==14){
   group=Panel(body);auto *title=Label(group,titles[section],632);
   lv_obj_set_style_text_font(title,&font_puhui_30_4,0);lv_obj_set_style_text_color(title,lv_color_hex(kAccent),0);
   Muted(group,notes[section++],632);
  }
  Label(group,fields[i].label,632);
  auto *editor=lv_textarea_create(group);HiInputMethod_Bind(editor,true);StyleInput(editor);screen_swipe_back_ignore(editor,true);
  lv_textarea_set_one_line(editor,true);lv_textarea_set_max_length(editor,255);lv_textarea_set_password_mode(editor,fields[i].secret);
  if(fields[i].secret)lv_textarea_set_placeholder_text(editor,"点击填写");
  editors.push_back(editor);lv_obj_add_event_cb(editor,Focus,LV_EVENT_FOCUSED,nullptr);lv_obj_add_event_cb(editor,Focus,LV_EVENT_CLICKED,nullptr);
 }
 auto *connections=Panel(body);Label(connections,"Gateway 连接",632);
 Button(connections,"连接 Gateway",[](lv_event_t*){Start(JobType::Gateway);},nullptr,632);
 Button(connections,"断开 Gateway",[](lv_event_t*){Start(JobType::StopGateway);},nullptr,632);
 auto *save=Button(content,"保存配置",[](lv_event_t*){
  if(keyboard&&!HiInputMethod_Flush(keyboard))return;
  std::vector<std::string> values;for(auto *editor:editors)values.emplace_back(lv_textarea_get_text(editor));
  if(Start(JobType::Save,"",std::move(values)))SetKeyboardVisible(false);
 },nullptr,672);lv_obj_set_pos(save,24,580);lv_obj_set_height(save,64);Primary(save);
 CreateKeyboard();Start(JobType::Config);
}
void Back() {
 if(keyboard&&!lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN)){SetKeyboardVisible(false);return;}
 lv_async_call([](void*){if(!s_root || lv_screen_active()!=s_root)return;if(page==Page::List)HomeScreen::SwitchToHome();else ShowList();},nullptr);
}

void Tick(lv_timer_t*) {
 if(!s_root || lv_screen_active()!=s_root)return;
 Result r;{std::lock_guard<std::mutex> guard(result_lock);if(result.ready){r=std::move(result);result=Result{};}}
 if(r.ready && r.generation!=generation){Start(page==Page::Chat?JobType::History:page==Page::Settings?JobType::Config:JobType::List);}
 if(r.ready && r.generation==generation){if(!r.status.empty())SetStatus(r.status.c_str());if(page==Page::Chat&&!r.history.empty())RenderHistory(r.history);if(page==Page::List)RenderSessions(r.sessions);if(page==Page::Settings&&r.config.size()==editors.size())for(size_t i=0;i<editors.size();++i)lv_textarea_set_text(editors[i],r.config[i].c_str());}
 HiOSAgent::Reply reply;while(HiOSAgent::Poll(reply)){if(page==Page::Chat&&reply.session==session){Bubble("assistant",reply.text.c_str());SetStatus("回复完成");if(auto_speak){std::string error;if(!HiOSAgent::Speak(reply.text,error,session))SetStatus(error.c_str());}}}
 while(HiOSAgent::PollTranscript(reply)){if(page==Page::Chat&&reply.session==session){if(reply.ok){if(Start(JobType::Send,reply.text))Bubble("user",reply.text.c_str());}else SetStatus(reply.text.empty()?"没有识别到语音":reply.text.c_str());}}
}
}
lv_obj_t *OpenClawScreen::CreateStatic(){if(s_root)return s_root;s_root=lv_obj_create(nullptr);screen_strip_obj_chrome(s_root);lv_obj_set_size(s_root,720,720);lv_obj_set_style_bg_color(s_root,lv_color_hex(kBackground),0);content=lv_obj_create(s_root);screen_strip_obj_chrome(content);lv_obj_set_size(content,720,720);lv_obj_set_style_bg_opa(content,LV_OPA_TRANSP,0);lv_obj_remove_flag(content,LV_OBJ_FLAG_SCROLLABLE);ShowList();screen_attach_swipe_back(s_root,Back);screen_attach_left_edge_back(s_root,0,Back);timer=lv_timer_create(Tick,100,nullptr);lv_obj_add_event_cb(s_root,[](lv_event_t *e){if(lv_event_get_target(e)!=s_root)return;++generation;HiOSAgent::CancelVoice();recording=false;if(timer){lv_timer_delete(timer);timer=nullptr;}s_root=nullptr;content=nullptr;body=nullptr;status=nullptr;input=nullptr;keyboard=nullptr;header=nullptr;composer=nullptr;sessions_body=nullptr;empty_chat=nullptr;editors.clear();},LV_EVENT_DELETE,nullptr);return s_root;}
lv_obj_t *OpenClawScreen::Create(){return CreateStatic();}
void OpenClawScreen::LifecycleCallback(screen_lifecycle_event_t event){if(event==SCREEN_LIFECYCLE_LOAD){if(!job_busy)Start(page==Page::Chat?JobType::History:page==Page::Settings?JobType::Config:JobType::List);}else{++generation;HiOSAgent::CancelVoice();recording=false;SetKeyboardVisible(false);}}
