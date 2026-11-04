#include "hi_task_policy.h"
// SPDX-License-Identifier: Apache-2.0
#include "app_manager_screen.h"
#include "hiapp.h"
#include "runtime.h"
#include "registry.h"
#include "home_screen/home_screen.h"
#include "system_chrome.h"
#include "pwr_key_handler.h"
#include <pthread.h>
#include <sched.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <ctime>
#include <cstring>
LV_FONT_DECLARE(font_puhui_full_20);
LV_FONT_DECLARE(font_puhui_30_4);
namespace {
int64_t TimeMs(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return(int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
unsigned active_jobs=0;
struct Job:HiApp::RefCounted{std::atomic<bool>done{false},cancel{false};std::atomic<int>phase{0};std::atomic<unsigned>updated{0};bool installed=false;std::string operation,target,message;std::vector<HiApp::Record>records;};
struct View{lv_obj_t*root,*status,*list,*packages,*installed_tab;lv_timer_t*timer;HiApp::Ref<Job>job;bool installed=false,timed_out=false;int shown_phase=-1;};
void Refresh(View*,const char*op="",const std::string&target={});
lv_obj_t*Label(lv_obj_t*p,const std::string&t,int w,bool large=false){auto*l=lv_label_create(p);lv_label_set_text(l,t.c_str());lv_obj_set_width(l,w);lv_obj_set_style_text_font(l,large?&font_puhui_30_4:&font_puhui_full_20,0);lv_obj_set_style_text_color(l,lv_color_hex(0x22324B),0);return l;}
struct Action{View*v;std::string op,target,name;};
lv_obj_t*Button(lv_obj_t*,const std::string&,int,int,int,lv_event_cb_t,void*);
void Act(lv_event_t*e){auto*a=(Action*)lv_event_get_user_data(e);if(a->op=="open"){HiApp::Launch(("hiapp:"+a->target).c_str());return;}
 if(a->op=="remove"){
  auto*modal=lv_obj_create(a->v->root);lv_obj_set_size(modal,592,214);lv_obj_center(modal);lv_obj_set_style_pad_all(modal,18,0);lv_obj_set_style_radius(modal,18,0);lv_obj_set_style_bg_color(modal,lv_color_hex(0xFFFFFF),0);lv_obj_remove_flag(modal,LV_OBJ_FLAG_SCROLLABLE);
  auto*l=Label(modal,"卸载“"+a->name+"”？\n安装包和用户数据会保留。",540);lv_obj_set_pos(l,8,6);
  Button(modal,"取消",88,116,160,[](lv_event_t*e){lv_obj_delete(lv_obj_get_parent(lv_event_get_target_obj(e)));},nullptr);
  auto*copy=new Action{a->v,"uninstall",a->target,a->name};auto*c=Button(modal,"确认卸载",280,116,188,[](lv_event_t*e){auto*a=(Action*)lv_event_get_user_data(e);Refresh(a->v,"uninstall",a->target);lv_obj_delete(lv_obj_get_parent(lv_event_get_target_obj(e)));},copy);lv_obj_add_event_cb(c,[](lv_event_t*e){delete(Action*)lv_event_get_user_data(e);},LV_EVENT_DELETE,copy);return;
 }Refresh(a->v,a->op.c_str(),a->target);
}
lv_obj_t*Button(lv_obj_t*p,const std::string&t,int x,int y,int w,lv_event_cb_t cb,void*ud){auto*b=lv_button_create(p);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,40);lv_obj_set_style_pad_all(b,0,0);lv_obj_set_style_border_width(b,0,0);lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);lv_obj_set_style_bg_color(b,lv_color_hex(0x247FE5),0);lv_obj_set_style_bg_color(b,lv_color_hex(0x1C68BF),LV_STATE_PRESSED);lv_obj_set_style_radius(b,10,0);lv_obj_set_style_shadow_width(b,0,0);auto*l=Label(b,t,w-12);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_color(l,lv_color_white(),0);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_obj_center(l);lv_obj_set_ext_click_area(b,5);lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,ud);screen_swipe_back_ignore(b);return b;}
void Row(View*v,const HiApp::Record&r){
 const bool ready=r.present,update=ready&&r.installed_version!=r.version;auto*row=lv_obj_create(v->list);lv_obj_set_width(row,660);lv_obj_set_height(row,r.error.empty()?138:102);lv_obj_set_style_bg_color(row,lv_color_hex(0xFFFFFF),0);lv_obj_set_style_border_width(row,1,0);lv_obj_set_style_border_color(row,lv_color_hex(0xE3EAF4),0);lv_obj_set_style_pad_all(row,14,0);lv_obj_set_style_radius(row,16,0);lv_obj_remove_flag(row,LV_OBJ_FLAG_SCROLLABLE);
 auto*icon=HiApp::RecordIcon(row,r,56);lv_obj_set_pos(icon,0,2);
 auto*l=Label(row,r.name,410);lv_obj_set_pos(l,76,0);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);
 std::string state=r.error.empty()?(ready?(update?"可更新":"已安装"):"待安装"):"安装包无效";
 auto*d=Label(row,r.error.empty()?"版本 "+r.version:r.error,410);lv_obj_set_pos(d,76,30);lv_obj_set_style_text_color(d,lv_color_hex(r.error.empty()?0x6B7E94:0xC24B43),0);lv_label_set_long_mode(d,LV_LABEL_LONG_DOT);
 auto*badge=Label(row,state,116);lv_obj_set_pos(badge,504,0);lv_obj_set_style_text_align(badge,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_bg_color(badge,lv_color_hex(ready?0xE4F6EE:0xEAF2FF),0);lv_obj_set_style_bg_opa(badge,LV_OPA_COVER,0);lv_obj_set_style_text_color(badge,lv_color_hex(ready?0x248160:0x2375CA),0);lv_obj_set_style_radius(badge,8,0);lv_obj_set_style_pad_ver(badge,4,0);
 if(r.error.empty()){
  auto*meta=Label(row,r.installed?"已加入系统主页":r.present?"保留安装包，便于再次安装":"安装后可从主页打开",370);lv_obj_set_pos(meta,0,76);lv_obj_set_style_text_color(meta,lv_color_hex(0x788A9E),0);
  auto add=[&](const char*text,const char*op,const std::string&target,int x,int w){auto*a=new Action{v,op,target,r.name};auto*b=Button(row,text,x,64,w,Act,a);lv_obj_add_event_cb(b,[](lv_event_t*e){delete(Action*)lv_event_get_user_data(e);},LV_EVENT_DELETE,a);return b;};
  if(ready){add("打开","open",r.id,416,94);if(r.installed)add("卸载","remove",r.id,522,94);else if(update)add("更新","install",r.file,522,94);}
  else add("安装应用","install",r.file,490,126);
 }
}
void Tabs(View*v){for(int i=0;i<2;i++){auto*b=i?v->installed_tab:v->packages;bool selected=v->installed==(i==1);lv_obj_set_style_bg_color(b,lv_color_hex(selected?0x247FE5:0xFFFFFF),0);lv_obj_set_style_bg_color(b,lv_color_hex(selected?0x1C68BF:0xE7F1FE),LV_STATE_PRESSED);lv_obj_set_style_text_color(lv_obj_get_child(b,0),selected?lv_color_white():lv_color_hex(0x526C89),0);lv_obj_set_style_border_width(b,selected?0:1,0);lv_obj_set_style_border_color(b,lv_color_hex(0xD7E3F1),0);}}

void*Work(void*ptr){
 auto j=*(HiApp::Ref<Job>*)ptr;delete(HiApp::Ref<Job>*)ptr;HiApp::RegistryLoadOrder();
 if(j->operation=="install")HiApp::Install(j->target,j->message);else if(j->operation=="uninstall")HiApp::Uninstall(j->target,j->message);
 j->updated=(unsigned)TimeMs();std::string error;
 j->records=HiApp::Scan(j->installed,error,[](void*p,int phase)->bool{auto*j=(Job*)p;j->phase=phase;j->updated=(unsigned)TimeMs();return !j->cancel;},j.get());
 if(!error.empty())j->message=error;else if(j->message.empty())j->message=std::string(j->installed?"已安装应用：":"SD卡 /app 中的安装包：")+std::to_string(j->records.size());
 __atomic_store_n(&active_jobs,0u,__ATOMIC_RELEASE);j->done=true;return nullptr;
}
void Refresh(View*v,const char*op,const std::string&target){
 if(v->job)return;if(__atomic_exchange_n(&active_jobs,1u,__ATOMIC_ACQ_REL)){lv_label_set_text(v->status,"上次SD卡任务尚未返回，请稍后刷新");return;}
 auto j=HiApp::Ref<Job>(new Job);j->updated=(unsigned)TimeMs();j->installed=v->installed;j->operation=op;j->target=target;v->shown_phase=-1;v->timed_out=false;v->job=j;lv_label_set_text(v->status,*op?"正在处理，请稍候…":"正在读取SD卡 /app…");
 auto*arg=new HiApp::Ref<Job>(j);pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,32768);sched_param priority{};priority.sched_priority=81;pthread_attr_setschedparam(&a,&priority);pthread_attr_setinheritsched(&a,PTHREAD_EXPLICIT_SCHED);pthread_t t;int rc=hi_task_create_legacy(&t,&a,Work,arg);pthread_attr_destroy(&a);
 if(rc){delete arg;v->job.reset();__atomic_store_n(&active_jobs,0u,__ATOMIC_RELEASE);lv_label_set_text(v->status,"无法启动应用管理任务");}else pthread_detach(t);
}
}
lv_obj_t*AppManagerScreen::CreateIcon(lv_obj_t*p,int size){auto*o=lv_obj_create(p);screen_strip_obj_chrome(o);lv_obj_set_size(o,size,size);lv_obj_set_style_radius(o,size/4,0);lv_obj_set_style_bg_color(o,lv_color_hex(0x3979DB),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);for(int n=0;n<4;n++){auto*s=lv_obj_create(o);screen_strip_obj_chrome(s);int w=size*23/100;lv_obj_set_size(s,w,w);lv_obj_set_pos(s,size*20/100+(n%2)*size*36/100,size*20/100+(n/2)*size*36/100);lv_obj_set_style_bg_color(s,lv_color_hex(0xFFFFFF),0);lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);lv_obj_set_style_radius(s,w/4,0);lv_obj_remove_flag(s,LV_OBJ_FLAG_CLICKABLE);}return o;}
lv_obj_t*AppManagerScreen::CreateStatic(){auto*v=new View;v->root=lv_obj_create(nullptr);screen_strip_obj_chrome(v->root);lv_obj_set_size(v->root,720,720);lv_obj_set_style_bg_color(v->root,lv_color_hex(0xF2F6FC),0);lv_obj_remove_flag(v->root,LV_OBJ_FLAG_SCROLLABLE);
 auto*t=Label(v->root,"应用管理",400,true);lv_obj_set_style_text_align(t,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_pos(t,160,17);Button(v->root,"返回",20,16,80,[](lv_event_t*){HomeScreen::SwitchToHome();},nullptr);
 Button(v->root,"刷新",608,16,92,[](lv_event_t*e){Refresh((View*)lv_event_get_user_data(e));},v);
 auto*intro=Label(v->root,"SD 卡 /app  ·  安装与管理你的应用",650);lv_obj_set_pos(intro,26,67);lv_obj_set_style_text_color(intro,lv_color_hex(0x7B8DA4),0);
 v->packages=Button(v->root,"安装包",24,106,144,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);if(v->job)return;v->installed=false;Tabs(v);Refresh(v);},v);
 v->installed_tab=Button(v->root,"已安装",180,106,144,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);if(v->job)return;v->installed=true;Tabs(v);Refresh(v);},v);
 auto*format=Label(v->root,".hiapp 应用",160);lv_obj_set_pos(format,536,116);lv_obj_set_style_text_align(format,LV_TEXT_ALIGN_RIGHT,0);lv_obj_set_style_text_color(format,lv_color_hex(0x8797AA),0);
 Tabs(v);v->status=Label(v->root,"",668);lv_obj_set_pos(v->status,26,163);lv_obj_set_height(v->status,30);lv_label_set_long_mode(v->status,LV_LABEL_LONG_DOT);lv_obj_set_style_text_color(v->status,lv_color_hex(0x6B8099),0);
 v->list=lv_obj_create(v->root);screen_strip_obj_chrome(v->list);lv_obj_set_pos(v->list,20,204);lv_obj_set_size(v->list,680,456);lv_obj_set_style_bg_opa(v->list,LV_OPA_TRANSP,0);lv_obj_set_style_pad_row(v->list,10,0);lv_obj_set_flex_flow(v->list,LV_FLEX_FLOW_COLUMN);lv_obj_add_flag(v->list,LV_OBJ_FLAG_SCROLLABLE);screen_swipe_back_ignore(v->list);
 v->timer=lv_timer_create([](lv_timer_t*t){auto*v=(View*)lv_timer_get_user_data(t);if(!v->job)return;
  if(v->job->done){auto j=v->job;v->job.reset();HomeScreen::SyncInstalledAppOrder();lv_label_set_text(v->status,j->message.c_str());lv_obj_clean(v->list);for(const auto&r:j->records)Row(v,r);if(j->records.empty()){auto*l=Label(v->list,v->installed?"尚未安装独立应用。\n切换到安装包，选择.hiapp进行安装。":"将.hiapp文件放入SD卡根目录的app文件夹，\n然后点击刷新。",650);lv_obj_set_style_pad_all(l,20,0);}}
  else {int phase=v->job->phase;const char*const steps[]={"等待读取任务","检查SD卡和app目录","打开app目录","读取安装包列表","解析安装包信息"};const char*step=steps[phase>=0&&phase<5?phase:0];
   if((unsigned)((unsigned)TimeMs()-v->job->updated.load())>15000){v->job->cancel=true;if(!v->timed_out){lv_label_set_text(v->status,(std::string("SD卡响应超时：")+step+"。可先返回，等待后重试。").c_str());v->timed_out=true;}}
   else if(phase&&!v->timed_out&&v->shown_phase!=phase){lv_label_set_text(v->status,(std::string("正在")+step+"…").c_str());v->shown_phase=phase;}
  }
 },100,v);
 lv_obj_add_event_cb(v->root,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);if(v->job)v->job->cancel=true;lv_timer_delete(v->timer);delete v;},LV_EVENT_DELETE,v);
 screen_attach_lifecycle(v->root,[](screen_lifecycle_event_t e){PwrKey_OnScreenLifecycle("app_manager",e);});SystemChrome_Attach(v->root,false);screen_ensure_swipe_back(v->root,[](){HomeScreen::SwitchToHome();});Refresh(v);return v->root;
}
