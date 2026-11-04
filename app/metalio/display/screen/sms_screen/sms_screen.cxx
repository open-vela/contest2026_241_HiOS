#include "sms_screen.h"
#include "digital_people_screen/xiaobai_resources.h"
#include "sms_service.h"
#include "phone_service.h"
#include "home_screen/home_screen.h"
#include "hi_input_method.h"
#include "standby_screen/standby_screen.h"
#include "system_chrome/system_chrome.h"
#include "system_chrome/control_center.h"
#include "system_chrome/recents_overlay.h"
#include <metalio/metalio.h>
#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <map>
#include <initializer_list>
#include <string>
#include <vector>
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_full_20);
namespace {
constexpr unsigned kInk=0x3D403E,kMuted=0x858881,kPaper=0xF5F5F0;
struct View {
 lv_obj_t *root=nullptr,*title=nullptr,*status=nullptr,*list=nullptr,*editor=nullptr,*number=nullptr,*text=nullptr,*keyboard=nullptr,*search=nullptr;
 lv_timer_t *timer=nullptr;lv_obj_t*filters[4]{};lv_obj_t*quick=nullptr,*region=nullptr;int filter=0;
 struct Pending{std::string id;int64_t until;lv_obj_t*button;};std::vector<Pending>pending;std::map<std::string,SmsService::ThreadOptions>options;
 std::string peer;std::map<std::string,std::string> contacts;unsigned revision=0;bool composing=false;
 lv_obj_t *recipient_picker=nullptr,*recipient_search=nullptr,*recipient_list=nullptr;
 std::vector<PhoneService::Contact> recipient_contacts;
};
View *active_view=nullptr;
std::string pending_peer;
lv_obj_t *Label(lv_obj_t*p,const std::string&t,int x,int y,int width,bool big=false,unsigned color=kInk){auto*l=lv_label_create(p);lv_label_set_text(l,t.c_str());lv_obj_set_pos(l,x,y);lv_obj_set_width(l,width);lv_obj_set_style_text_font(l,big?&font_puhui_30_4:&font_puhui_20_4,0);lv_obj_set_style_text_color(l,lv_color_hex(color),0);lv_obj_remove_flag(l,LV_OBJ_FLAG_CLICKABLE);return l;}
lv_obj_t *Button(lv_obj_t*p,const char*t,int x,int y,int w,int h,lv_event_cb_t cb,void*data){auto*b=lv_button_create(p);screen_strip_obj_chrome(b);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);lv_obj_set_style_bg_color(b,lv_color_hex(0xE2E4DC),0);lv_obj_set_style_bg_grad_color(b,lv_color_hex(0xCDD0C6),0);lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_VER,0);lv_obj_set_style_bg_opa(b,255,0);lv_obj_set_style_border_width(b,1,0);lv_obj_set_style_border_color(b,lv_color_hex(0xBABFB2),0);lv_obj_set_style_radius(b,8,0);lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE);auto*l=Label(b,t,0,0,w);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(l);lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,data);return b;}
std::string ContactName(View*v,const std::string &number){auto i=v->contacts.find(SmsService::NormalizeNumber(number));return i==v->contacts.end()?number:i->second;}
std::string Stamp(int64_t when){time_t value=SmsService::DisplayTime(when);tm t{};char b[40];localtime_r(&value,&t);strftime(b,sizeof(b),"%Y-%m-%d %H:%M",&t);return b;}
void Draw(View*v);
void CloseRecipientPicker(View*v){
 if(!v->recipient_picker)return;HiInputMethod_Suspend();
 auto*picker=v->recipient_picker;v->recipient_picker=v->recipient_search=v->recipient_list=nullptr;
 lv_obj_delete(picker);v->recipient_contacts.clear();
}
void RenderRecipients(View*v){
 if(!v->recipient_list||!v->recipient_search)return;lv_obj_clean(v->recipient_list);
 const std::string query=lv_textarea_get_text(v->recipient_search);int y=0;
 struct Choice{View*view;std::string number;};
 for(const auto&contact:v->recipient_contacts){
  if(contact.number.empty()||!PhoneService::Matches(contact,query))continue;
  auto*choice=new Choice{v,contact.number};
  auto*row=Button(v->recipient_list,(contact.name+"\n"+contact.number).c_str(),0,y,656,76,[](lv_event_t*e){
   auto*choice=static_cast<Choice*>(lv_event_get_user_data(e));auto*v=choice->view;
   const std::string number=choice->number;CloseRecipientPicker(v);
   if(v->number&&v->text){lv_textarea_set_text(v->number,number.c_str());HiInputMethod_SetTextArea(v->keyboard,v->text,true);}
  },choice);
  lv_obj_set_style_text_font(lv_obj_get_child(row,0),&font_puhui_full_20,0);
  lv_obj_add_event_cb(row,[](lv_event_t*e){delete static_cast<Choice*>(lv_event_get_user_data(e));},LV_EVENT_DELETE,choice);
  screen_swipe_back_ignore(row,true);y+=84;
 }
 if(!y)Label(v->recipient_list,v->recipient_contacts.empty()?"通讯录暂无联系人，请先在电话中保存":"没有匹配的联系人",12,20,632);
}
void OpenRecipientPicker(View*v){
 if(v->recipient_picker||!v->composing)return;
 if(!HiInputMethod_Flush(v->keyboard))return;HiInputMethod_Suspend();
 v->recipient_contacts=PhoneService::Contacts();
 auto*picker=lv_obj_create(v->root);v->recipient_picker=picker;screen_strip_obj_chrome(picker);
 lv_obj_set_pos(picker,0,96);lv_obj_set_size(picker,720,590);lv_obj_set_style_bg_color(picker,lv_color_hex(kPaper),0);lv_obj_set_style_bg_opa(picker,255,0);lv_obj_remove_flag(picker,LV_OBJ_FLAG_SCROLLABLE);
 Label(picker,"选择收件人",20,16,490,true);
 Button(picker,"取消",584,10,112,48,[](lv_event_t*e){auto*v=static_cast<View*>(lv_event_get_user_data(e));CloseRecipientPicker(v);if(v->number)HiInputMethod_SetTextArea(v->keyboard,v->number,false);},v);
 v->recipient_search=lv_textarea_create(picker);HiInputMethod_Bind(v->recipient_search,true);
 lv_obj_set_pos(v->recipient_search,20,70);lv_obj_set_size(v->recipient_search,680,52);lv_textarea_set_one_line(v->recipient_search,true);lv_textarea_set_placeholder_text(v->recipient_search,"搜索姓名、首字母或电话号码");lv_obj_set_style_text_font(v->recipient_search,&font_puhui_full_20,0);
 lv_obj_add_event_cb(v->recipient_search,[](lv_event_t*e){RenderRecipients(static_cast<View*>(lv_event_get_user_data(e)));},LV_EVENT_VALUE_CHANGED,v);
 v->recipient_list=lv_obj_create(picker);screen_strip_obj_chrome(v->recipient_list);lv_obj_set_pos(v->recipient_list,20,136);lv_obj_set_size(v->recipient_list,680,428);lv_obj_set_scroll_dir(v->recipient_list,LV_DIR_VER);
 screen_swipe_back_ignore(picker,true);RenderRecipients(v);lv_obj_move_foreground(picker);
}
void KeepDraft(View*v){if(v->composing&&v->number&&v->text){HiInputMethod_Flush(v->keyboard);SmsService::SaveDraft(lv_textarea_get_text(v->number),lv_textarea_get_text(v->text));}}
void CloseEditor(View*v){if(!v->editor)return;CloseRecipientPicker(v);KeepDraft(v);lv_obj_delete(v->editor);v->editor=v->number=v->text=v->keyboard=nullptr;v->composing=false;Draw(v);}
void Compose(View*v,const std::string &number){
 if(v->editor)return;v->composing=true;v->editor=lv_obj_create(v->root);screen_strip_obj_chrome(v->editor);lv_obj_set_pos(v->editor,0,96);lv_obj_set_size(v->editor,720,590);lv_obj_set_style_bg_color(v->editor,lv_color_hex(kPaper),0);lv_obj_set_style_bg_opa(v->editor,255,0);lv_obj_remove_flag(v->editor,LV_OBJ_FLAG_SCROLLABLE);screen_swipe_back_ignore(v->editor,true);
 Button(v->editor,"联系人",16,0,96,48,[](lv_event_t*e){OpenRecipientPicker(static_cast<View*>(lv_event_get_user_data(e)));},v);v->number=lv_textarea_create(v->editor);lv_obj_set_pos(v->number,122,0);lv_obj_set_size(v->number,400,48);lv_textarea_set_one_line(v->number,true);lv_textarea_set_accepted_chars(v->number,"+0123456789");lv_textarea_set_max_length(v->number,21);lv_obj_set_style_text_font(v->number,&font_puhui_20_4,0);lv_textarea_set_placeholder_text(v->number,"号码，或点击联系人选择");lv_textarea_set_text(v->number,number.c_str());
 Button(v->editor,"取消",540,0,76,48,[](lv_event_t*e){CloseEditor(static_cast<View*>(lv_event_get_user_data(e)));},v);
 Button(v->editor,"发送",628,0,76,48,[](lv_event_t*e){auto*v=static_cast<View*>(lv_event_get_user_data(e));if(!HiInputMethod_Flush(v->keyboard))return;const std::string number=lv_textarea_get_text(v->number),text=lv_textarea_get_text(v->text);
  if(SmsService::Send(number,text)){v->peer=number;v->composing=false;HiInputMethod_Suspend();CloseEditor(v);Draw(v);}else lv_label_set_text(v->status,"请确认号码、短信内容及 SD 卡；最多 400 个字符");
 },v);
 v->text=lv_textarea_create(v->editor);lv_obj_set_pos(v->text,16,60);lv_obj_set_size(v->text,688,172);lv_obj_set_style_text_font(v->text,&font_puhui_full_20,0);lv_obj_set_style_radius(v->text,14,0);lv_obj_set_style_border_color(v->text,lv_color_hex(0xC7D1BD),0);lv_obj_set_style_pad_all(v->text,12,0);lv_textarea_set_placeholder_text(v->text,"输入短信内容");lv_textarea_set_max_length(v->text,400);
 const auto draft=SmsService::Draft();const auto split=draft.find('\n');if(split!=std::string::npos&&(number.empty()||draft.substr(0,split)==number)){if(number.empty())lv_textarea_set_text(v->number,draft.substr(0,split).c_str());lv_textarea_set_text(v->text,draft.substr(split+1).c_str());}
 v->keyboard=HiInputMethod_Create(v->editor);lv_obj_set_pos(v->keyboard,0,190);lv_obj_set_size(v->keyboard,720,400);HiInputMethod_SetTextArea(v->keyboard,number.empty()?v->number:v->text,!number.empty());
 HiInputMethod_Bind(v->number,false);HiInputMethod_Bind(v->text,true);
 if(v->region){lv_label_set_text(v->region,PhoneService::NumberRegion(lv_textarea_get_text(v->number)).c_str());lv_label_set_long_mode(v->region,LV_LABEL_LONG_DOT);}
 lv_obj_add_event_cb(v->number,[](lv_event_t*e){auto*v=static_cast<View*>(lv_event_get_user_data(e));if(v->region)lv_label_set_text(v->region,PhoneService::NumberRegion(lv_textarea_get_text(v->number)).c_str());},LV_EVENT_VALUE_CHANGED,v);

 for(auto *field:{v->number,v->text})lv_obj_add_event_cb(field,[](lv_event_t*e){auto*v=static_cast<View*>(lv_event_get_user_data(e));HiInputMethod_SetTextArea(v->keyboard,lv_event_get_target_obj(e),lv_event_get_target_obj(e)==v->text);},LV_EVENT_CLICKED,v);
}
struct Action{View *view;std::string value;int x=0,y=0;bool swiped=false;};
void OwnAction(lv_obj_t*o,Action*a){lv_obj_add_event_cb(o,[](lv_event_t*e){delete static_cast<Action*>(lv_event_get_user_data(e));},LV_EVENT_DELETE,a);}
void CloseQuick(View*v){if(v->quick){auto*m=v->quick;v->quick=nullptr;lv_obj_delete(m);}}
void QuickMenu(View*v,const std::string&number){
 HiInputMethod_Suspend();CloseQuick(v);auto*overlay=lv_obj_create(v->root);v->quick=overlay;screen_strip_obj_chrome(overlay);
 lv_obj_set_pos(overlay,0,96);lv_obj_set_size(overlay,720,590);lv_obj_set_style_bg_color(overlay,lv_color_black(),0);lv_obj_set_style_bg_opa(overlay,LV_OPA_50,0);lv_obj_remove_flag(overlay,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_add_event_cb(overlay,[](lv_event_t*e){CloseQuick((View*)lv_event_get_user_data(e));},LV_EVENT_CLICKED,v);
 auto*menu=lv_obj_create(overlay);screen_strip_obj_chrome(menu);lv_obj_set_pos(menu,24,270);lv_obj_set_size(menu,672,278);lv_obj_set_style_bg_color(menu,lv_color_hex(kPaper),0);lv_obj_set_style_bg_opa(menu,255,0);lv_obj_set_style_radius(menu,22,0);lv_obj_remove_flag(menu,LV_OBJ_FLAG_SCROLLABLE);
 auto*name=Label(menu,ContactName(v,number),24,18,624,true);lv_obj_set_style_text_font(name,&font_puhui_full_20,0);lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);
 auto options=v->options[SmsService::NormalizeNumber(number)];const char*labels[]={"删除会话","标记已读",options.pinned?"取消置顶":"置顶",options.blocked?"移出黑名单":"加入黑名单"};
 for(int i=0;i<4;++i){auto*a=new Action{v,std::to_string(i)+number};auto*button=Button(menu,labels[i],20+(i%2)*326,74+(i/2)*82,306,68,[](lv_event_t*e){
  auto*a=(Action*)lv_event_get_user_data(e);auto*v=a->view;int action=a->value[0]-'0';std::string number=a->value.substr(1);auto option=v->options[SmsService::NormalizeNumber(number)];
  CloseQuick(v);if(action==0)SmsService::DeleteConversation(number);else if(action==1)SmsService::MarkRead(number);else if(action==2)SmsService::SetPinned(number,!option.pinned);else SmsService::SetBlocked(number,!option.blocked);Draw(v);
 },a);OwnAction(button,a);if(i==0)lv_obj_set_style_bg_color(button,lv_color_hex(0xEDDCD6),0);}
 screen_swipe_back_ignore(overlay,true);lv_obj_move_foreground(overlay);
}
void UpdatePending(View*v){int64_t now=SmsService::MonotonicMillis();for(const auto&p:v->pending){
 int64_t left=p.until-now;if(left<=0){lv_obj_add_flag(p.button,LV_OBJ_FLAG_HIDDEN);continue;}
 auto*label=lv_obj_get_child(p.button,0);std::string text=std::to_string((left+999)/1000)+"秒内取消";if(text!=lv_label_get_text(label))lv_label_set_text(label,text.c_str());
}}
void Draw(View*v){
 if(v->composing)return;
 v->pending.clear();lv_obj_clean(v->list);v->revision=SmsService::Revision();lv_label_set_text(v->status,SmsService::Status().c_str());
 bool conversation=!v->peer.empty();for(int i=0;i<4;++i)if(v->filters[i]){
  if(conversation)lv_obj_add_flag(v->filters[i],LV_OBJ_FLAG_HIDDEN);else lv_obj_remove_flag(v->filters[i],LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(v->filters[i],lv_color_hex(i==v->filter?0xDDEBCF:0xE6E8E0),0);lv_obj_set_style_bg_grad_dir(v->filters[i],LV_GRAD_DIR_NONE,0);
 }
 if(conversation)lv_obj_add_flag(v->search,LV_OBJ_FLAG_HIDDEN);else lv_obj_remove_flag(v->search,LV_OBJ_FLAG_HIDDEN);
 lv_obj_set_pos(v->list,12,conversation?106:218);lv_obj_set_size(v->list,696,conversation?538:426);
 v->contacts.clear();for(const auto&c:PhoneService::Contacts())if(!c.name.empty())v->contacts[SmsService::NormalizeNumber(c.number)]=c.name;
 v->options.clear();for(const auto&o:SmsService::ConversationOptions())v->options[o.number]=o;
 const auto messages=SmsService::Messages();
 if(v->peer.empty()){
  lv_label_set_text(v->title,"短信");lv_obj_set_style_text_font(v->title,&font_puhui_30_4,0);if(v->region)lv_label_set_text(v->region,"");
  std::map<std::string,SmsService::Message>last;
  for(const auto&m:messages){auto key=SmsService::NormalizeNumber(m.number);auto option=v->options[key];
   if(v->filter==3?!option.blocked:option.blocked)continue;
   if(v->filter==1&&(!m.unread||m.state!=SmsService::State::Received))continue;
   if(v->filter==2&&m.state!=SmsService::State::Sent)continue;
   if(!last.count(key))last[key]=m;
  }
  std::vector<SmsService::Message>threads;for(const auto&pair:last)threads.push_back(pair.second);
  std::sort(threads.begin(),threads.end(),[&](const auto&a,const auto&b){bool pa=v->options[SmsService::NormalizeNumber(a.number)].pinned,pb=v->options[SmsService::NormalizeNumber(b.number)].pinned;return pa!=pb?pa:a.when>b.when;});
  std::string query=lv_textarea_get_text(v->search);unsigned shown=0;
  for(const auto&m:threads){auto key=SmsService::NormalizeNumber(m.number);std::string name=ContactName(v,m.number);bool known=v->contacts.count(key);auto option=v->options[key];
   if(!query.empty()&&name.find(query)==std::string::npos&&m.number.find(query)==std::string::npos&&m.text.find(query)==std::string::npos)continue;++shown;
   auto*row=lv_obj_create(v->list);screen_strip_obj_chrome(row);lv_obj_set_size(row,664,132);lv_obj_remove_flag(row,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(row,lv_color_hex(option.pinned?0xF0F5E9:0xFCFCF8),0);lv_obj_set_style_bg_opa(row,255,0);lv_obj_set_style_border_width(row,1,0);lv_obj_set_style_border_color(row,lv_color_hex(0xD6D8D0),0);lv_obj_set_style_radius(row,14,0);lv_obj_add_flag(row,LV_OBJ_FLAG_CLICKABLE);
   unsigned unread=0;for(const auto&x:messages)if(x.unread&&SmsService::NormalizeNumber(x.number)==key)++unread;
   auto*l=Label(row,name,18,10,378,true);if(known)lv_obj_set_style_text_font(l,&font_puhui_full_20,0);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);
   if(unread){auto*dot=lv_obj_create(row);lv_obj_remove_style_all(dot);lv_obj_set_pos(dot,399,23);lv_obj_set_size(dot,12,12);lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(dot,lv_color_hex(0x5BAE45),0);lv_obj_set_style_bg_opa(dot,255,0);lv_obj_remove_flag(dot,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(dot,LV_OBJ_FLAG_SCROLLABLE);}
   Label(row,Stamp(m.when).substr(5),430,16,220,false,kMuted);
   l=Label(row,PhoneService::NumberRegion(m.number),18,45,620,false,0x8A937E);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);
   l=Label(row,m.text,18,76,550,false,kMuted);lv_obj_set_style_text_font(l,&font_puhui_full_20,0);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);
   if(option.pinned)Label(row,"置顶",584,80,64,false,0x648547);
   auto*a=new Action{v,m.number};OwnAction(row,a);
   lv_obj_add_event_cb(row,[](lv_event_t*e){auto*a=(Action*)lv_event_get_user_data(e);auto*input=lv_event_get_indev(e);if(!input)return;lv_point_t p;lv_indev_get_point(input,&p);a->x=p.x;a->y=p.y;a->swiped=false;},LV_EVENT_PRESSED,a);
   lv_obj_add_event_cb(row,[](lv_event_t*e){auto*a=(Action*)lv_event_get_user_data(e);auto*input=lv_event_get_indev(e);if(!input)return;lv_point_t p;lv_indev_get_point(input,&p);if(a->x>32&&p.x-a->x>=80&&std::abs(p.y-a->y)<36){a->swiped=true;QuickMenu(a->view,a->value);}},LV_EVENT_RELEASED,a);
   lv_obj_add_event_cb(row,[](lv_event_t*e){auto*a=(Action*)lv_event_get_user_data(e);if(a->swiped){a->swiped=false;return;}auto*v=a->view;v->peer=a->value;SmsService::MarkRead(v->peer);Draw(v);},LV_EVENT_CLICKED,a);
   screen_swipe_back_ignore(row,true);
  }
  if(!shown)Label(v->list,v->filter==1?"没有未读短信":v->filter==2?"暂无已发送短信":v->filter==3?"没有拦截的短信":"暂无短信，开启 4G 后可接收。",8,8,620);
 }else{
  auto key=SmsService::NormalizeNumber(v->peer);bool known=v->contacts.count(key);lv_label_set_text(v->title,ContactName(v,v->peer).c_str());lv_obj_set_style_text_font(v->title,known?&font_puhui_full_20:&font_puhui_30_4,0);if(v->region)lv_label_set_text(v->region,PhoneService::NumberRegion(v->peer).c_str());
  Button(v->list,"回复 / 新短信",0,0,300,50,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);Compose(v,v->peer);},v);SmsService::MarkRead(v->peer);
  // Messages() already provides newest-first order, including received and sent.
  for(const auto&m:messages){if(SmsService::NormalizeNumber(m.number)!=key)continue;
   auto*card=lv_obj_create(v->list);screen_strip_obj_chrome(card);lv_obj_set_width(card,664);lv_obj_set_height(card,LV_SIZE_CONTENT);lv_obj_set_style_min_height(card,112,0);lv_obj_set_style_pad_all(card,16,0);lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(card,lv_color_hex(m.state==SmsService::State::Received?0xFFFFFF:0xE2EDD6),0);lv_obj_set_style_bg_opa(card,255,0);lv_obj_set_style_radius(card,18,0);
   auto*body=Label(card,m.text,0,0,620);lv_obj_set_style_text_font(body,&font_puhui_full_20,0);lv_obj_set_height(body,LV_SIZE_CONTENT);lv_obj_update_layout(body);int h=lv_obj_get_height(body);
   const char*state=m.state==SmsService::State::Received?"收到":m.state==SmsService::State::Sent?"已发送":m.state==SmsService::State::Queued?"等待发送":m.state==SmsService::State::Uncertain?"结果未确认":m.state==SmsService::State::Cancelled?"已取消":"未发送";
   Label(card,Stamp(m.when)+" · "+state,0,h+12,490,false,kMuted);int footer=h+46;
   if(m.state==SmsService::State::Queued&&m.cancel_until_ms>SmsService::MonotonicMillis()){
    auto*a=new Action{v,m.id};auto*cancel=Button(card,"4秒内取消",362,h+42,250,48,[](lv_event_t*e){auto*a=(Action*)lv_event_get_user_data(e);auto*v=a->view;std::string id=a->value;SmsService::CancelSend(id);Draw(v);},a);OwnAction(cancel,a);v->pending.push_back({m.id,m.cancel_until_ms,cancel});footer+=56;
   }
   if(!m.error.empty()&&m.state!=SmsService::State::Cancelled){auto*error=Label(card,m.error,0,footer,620,false,0xA44D3C);lv_obj_update_layout(error);footer+=lv_obj_get_height(error)+10;}
   auto*a=new Action{v,m.id};auto*remove=Button(card,"删除",540,h+6,80,36,[](lv_event_t*e){auto*a=(Action*)lv_event_get_user_data(e);auto*v=a->view;std::string id=a->value;SmsService::Delete(id);Draw(v);},a);OwnAction(remove,a);lv_obj_set_height(card,footer+24);
  }
  lv_obj_update_layout(v->list);lv_obj_scroll_to_y(v->list,0,LV_ANIM_OFF);UpdatePending(v);
 }
 v->revision=SmsService::Revision();
}

void Back(View*v){if(v->recipient_picker)CloseRecipientPicker(v);else if(v->quick)CloseQuick(v);else if(v->composing)CloseEditor(v);else if(!v->peer.empty()){v->peer.clear();Draw(v);}else HomeScreen::SwitchToHome();}
}
namespace {
lv_obj_t *sms_toast=nullptr;lv_timer_t *sms_notice_timer=nullptr;uint32_t sms_toast_at=0;
std::string sms_toast_peer;
void HideSmsToast(){if(sms_toast){auto*card=sms_toast;sms_toast=nullptr;lv_obj_delete(card);}sms_toast_peer.clear();}
std::string PreviewText(const std::string&text){std::string out;for(size_t i=0;i<text.size()&&out.size()<220;){
 unsigned char c=text[i];size_t n=c<128?1:(c&224)==192?2:(c&240)==224?3:4;if(i+n>text.size()||out.size()+n>220)break;
 if(c<32){if(out.empty()||out.back()!=' ')out+=' ';}else out.append(text,i,n);i+=n;
 }return out;}
void ShowSmsToast(const SmsService::Message&m,unsigned count){
 HideSmsToast();sms_toast_peer=m.number;sms_toast_at=lv_tick_get();
 auto*card=lv_obj_create(lv_layer_sys());sms_toast=card;screen_strip_obj_chrome(card);
 lv_obj_set_pos(card,24,44);lv_obj_set_size(card,672,190);lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_add_flag(card,LV_OBJ_FLAG_FLOATING);lv_obj_add_flag(card,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_set_style_radius(card,24,0);lv_obj_set_style_bg_color(card,lv_color_hex(0xFBFCF7),0);lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);
 lv_obj_set_style_border_width(card,1,0);lv_obj_set_style_border_color(card,lv_color_hex(0xD4DCCB),0);lv_obj_set_style_shadow_width(card,12,0);lv_obj_set_style_shadow_opa(card,60,0);lv_obj_set_style_shadow_ofs_y(card,4,0);
 auto*icon=SmsScreen::CreateIcon(card,48);lv_obj_set_pos(icon,20,18);screen_make_input_passive(icon);
 auto*hint=Label(card,"短信 · 刚刚",84,15,440,false,0x6A7A59);(void)hint;
 bool known=false;std::string name=m.number;for(const auto&c:PhoneService::Contacts())if(SmsService::NormalizeNumber(c.number)==SmsService::NormalizeNumber(m.number)&&!c.name.empty()){name=c.name;known=true;break;}
 auto*sender=Label(card,name,84,43,490,true);if(known)lv_obj_set_style_text_font(sender,&font_puhui_full_20,0);lv_label_set_long_mode(sender,LV_LABEL_LONG_DOT);
 lv_label_set_text(hint,("短信 · "+PhoneService::NumberRegion(m.number)).c_str());
 auto*body=Label(card,PreviewText(m.text),22,88,628,false,0x4E554A);lv_obj_set_style_text_font(body,&font_puhui_full_20,0);lv_obj_set_height(body,54);lv_label_set_long_mode(body,LV_LABEL_LONG_WRAP);
 Label(card,count>1?std::to_string(count)+" 条新短信 · 点击查看":"点击查看短信",24,157,500,false,0x829071);
 auto*close=Button(card,"关闭",568,14,84,44,[](lv_event_t*){HideSmsToast();},nullptr);screen_swipe_back_ignore(close,true);
 lv_obj_add_event_cb(card,[](lv_event_t*){std::string peer=sms_toast_peer;HideSmsToast();if(!peer.empty())SmsScreen::OpenConversation(peer);},LV_EVENT_CLICKED,nullptr);
 screen_swipe_back_ignore(card,true);lv_obj_move_foreground(card);
 lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,card);lv_anim_set_exec_cb(&a,[](void*obj,int32_t y){lv_obj_set_y((lv_obj_t*)obj,y);});lv_anim_set_values(&a,-190,44);lv_anim_set_duration(&a,180);lv_anim_set_path_cb(&a,lv_anim_path_ease_out);lv_anim_start(&a);
}
}
bool SmsScreen::IsForeground(){return active_view&&active_view->root==lv_screen_active()&&!StandbyScreen::IsVisible()&&!ControlCenter_IsOpen()&&!Recents_IsOpen()&&!SystemChrome_FloatAiOpen()&&metalio_nt26_get_call_state()==METALIO_CALL_IDLE;}
void SmsScreen::InitGlobal(){if(sms_notice_timer)return;sms_notice_timer=lv_timer_create([](lv_timer_t*){
 if(XiaobaiResources::Status()==0||XiaobaiResources::Status()==1)return;
 SmsService::Init();SmsService::Message message;unsigned count=0;bool fresh=SmsService::TakeNotification(message,count);
 if(IsForeground()){HideSmsToast();return;}
 if(fresh)ShowSmsToast(message,count);
 if(sms_toast&&lv_tick_elaps(sms_toast_at)>=10000)HideSmsToast();
 },200,nullptr);}
void SmsScreen::OpenConversation(const std::string&number){
 auto*copy=new std::string(number);HiInputMethod_Suspend();
 if(StandbyScreen::IsVisible())StandbyScreen::ReturnHome();SystemChrome_DismissOverlays();
 if(lv_async_call([](void*arg){auto*number=static_cast<std::string*>(arg);
  pending_peer=*number;
  if(IsForeground()&&active_view){auto*v=active_view;if(v->editor)CloseEditor(v);v->peer=pending_peer;pending_peer.clear();Draw(v);}
  else if(!HomeScreen::LaunchApp("sms"))pending_peer.clear();
  delete number;
 },copy)!=LV_RESULT_OK)delete copy;
}

lv_obj_t *SmsScreen::CreateStatic(){
 SmsService::Refresh();auto*v=new View;active_view=v;v->peer=pending_peer;pending_peer.clear();v->root=lv_obj_create(nullptr);screen_strip_obj_chrome(v->root);lv_obj_set_size(v->root,720,720);lv_obj_remove_flag(v->root,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(v->root,lv_color_hex(kPaper),0);lv_obj_set_style_bg_opa(v->root,255,0);
 auto*header=lv_obj_create(v->root);screen_strip_obj_chrome(header);lv_obj_set_size(header,720,96);lv_obj_set_style_bg_color(header,lv_color_hex(0xF4F4F1),0);lv_obj_set_style_bg_grad_color(header,lv_color_hex(0xDADDD4),0);lv_obj_set_style_bg_grad_dir(header,LV_GRAD_DIR_VER,0);lv_obj_set_style_bg_opa(header,255,0);lv_obj_remove_flag(header,LV_OBJ_FLAG_SCROLLABLE);
 Button(header,"返回",16,34,96,50,[](lv_event_t*e){Back(static_cast<View*>(lv_event_get_user_data(e)));},v);v->title=Label(header,"短信",130,42,360,true);lv_obj_set_style_text_align(v->title,LV_TEXT_ALIGN_CENTER,0);v->region=Label(header,"",130,74,360,false,0x87917C);lv_label_set_long_mode(v->region,LV_LABEL_LONG_DOT);lv_obj_set_style_text_align(v->region,LV_TEXT_ALIGN_CENTER,0);
 Button(header,"刷新",504,34,90,50,[](lv_event_t*){SmsService::Refresh();},v);Button(header,"新建",608,34,96,50,[](lv_event_t*e){Compose(static_cast<View*>(lv_event_get_user_data(e)),"");},v);
 v->search=lv_textarea_create(v->root);HiInputMethod_Bind(v->search,true);lv_obj_set_pos(v->search,18,105);lv_obj_set_size(v->search,684,48);lv_textarea_set_one_line(v->search,true);lv_textarea_set_placeholder_text(v->search,"搜索姓名、号码或短信");lv_obj_set_style_text_font(v->search,&font_puhui_20_4,0);
 lv_obj_add_event_cb(v->search,[](lv_event_t*e){auto*v=static_cast<View*>(lv_event_get_user_data(e));if(!v->composing)Draw(v);},LV_EVENT_VALUE_CHANGED,v);
 const char*categories[]={"全部","未读","已发送","拦截"};for(int i=0;i<4;++i){
  auto*a=new Action{v,std::to_string(i)};auto*b=Button(v->root,categories[i],18+i*172,162,166,46,[](lv_event_t*e){auto*a=static_cast<Action*>(lv_event_get_user_data(e));auto*v=a->view;v->filter=a->value.empty()?0:a->value[0]-'0';v->peer.clear();if(v->editor)CloseEditor(v);Draw(v);},a);OwnAction(b,a);v->filters[i]=b;
 }
 v->list=lv_obj_create(v->root);screen_strip_obj_chrome(v->list);lv_obj_set_pos(v->list,12,218);lv_obj_set_size(v->list,696,426);lv_obj_set_style_pad_all(v->list,10,0);lv_obj_set_style_pad_row(v->list,10,0);lv_obj_set_flex_flow(v->list,LV_FLEX_FLOW_COLUMN);lv_obj_set_scroll_dir(v->list,LV_DIR_VER);screen_swipe_back_ignore(v->list,true);
 v->status=Label(v->root,"",20,655,680,false,kMuted);
 v->timer=lv_timer_create([](lv_timer_t*t){auto*v=static_cast<View*>(lv_timer_get_user_data(t));if(v->revision!=SmsService::Revision())Draw(v);else UpdatePending(v);},100,v);
 lv_obj_add_event_cb(v->root,[](lv_event_t*e){auto*v=static_cast<View*>(lv_event_get_user_data(e));CloseRecipientPicker(v);KeepDraft(v);if(v->timer)lv_timer_pause(v->timer);},LV_EVENT_SCREEN_UNLOADED,v);
 lv_obj_add_event_cb(v->root,[](lv_event_t*e){auto*v=static_cast<View*>(lv_event_get_user_data(e));active_view=v;if(!pending_peer.empty()){v->peer=pending_peer;pending_peer.clear();}if(v->timer)lv_timer_resume(v->timer);Draw(v);},LV_EVENT_SCREEN_LOADED,v);
 lv_obj_add_event_cb(v->root,[](lv_event_t*e){auto*v=static_cast<View*>(lv_event_get_user_data(e));if(v->timer)lv_timer_delete(v->timer);if(active_view==v)active_view=nullptr;delete v;},LV_EVENT_DELETE,v);
 screen_attach_swipe_back(v->root,[]{if(active_view)Back(active_view);});Draw(v);return v->root;
}
lv_obj_t *SmsScreen::CreateIcon(lv_obj_t*parent,int size){auto*icon=lv_obj_create(parent);screen_strip_obj_chrome(icon);lv_obj_set_size(icon,size,size);lv_obj_remove_flag(icon,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_radius(icon,size/5,0);lv_obj_set_style_bg_color(icon,lv_color_hex(0x87B548),0);lv_obj_set_style_bg_grad_color(icon,lv_color_hex(0x567F2D),0);lv_obj_set_style_bg_grad_dir(icon,LV_GRAD_DIR_VER,0);lv_obj_set_style_bg_opa(icon,255,0);
 auto*bubble=lv_obj_create(icon);screen_strip_obj_chrome(bubble);lv_obj_set_pos(bubble,size/6,size/5);lv_obj_set_size(bubble,size*2/3,size/2);lv_obj_set_style_radius(bubble,size/9,0);lv_obj_set_style_bg_color(bubble,lv_color_hex(0xF5F8E9),0);lv_obj_set_style_bg_opa(bubble,255,0);lv_obj_remove_flag(bubble,LV_OBJ_FLAG_SCROLLABLE);lv_obj_remove_flag(bubble,LV_OBJ_FLAG_CLICKABLE);
 auto*tail=lv_obj_create(icon);screen_strip_obj_chrome(tail);lv_obj_set_pos(tail,size/4,size*3/5);lv_obj_set_size(tail,size/6,size/5);lv_obj_set_style_radius(tail,size/24,0);lv_obj_set_style_bg_color(tail,lv_color_hex(0xF5F8E9),0);lv_obj_set_style_bg_opa(tail,255,0);lv_obj_remove_flag(tail,LV_OBJ_FLAG_CLICKABLE);
 for(int i=0;i<3;++i){auto*line=lv_obj_create(bubble);screen_strip_obj_chrome(line);lv_obj_set_pos(line,size/10,size/10+i*size/10);lv_obj_set_size(line,size*7/15,size/36+1);lv_obj_set_style_bg_color(line,lv_color_hex(0x849876),0);lv_obj_set_style_bg_opa(line,255,0);lv_obj_remove_flag(line,LV_OBJ_FLAG_CLICKABLE);}lv_obj_remove_flag(icon,LV_OBJ_FLAG_CLICKABLE);return icon;
}
