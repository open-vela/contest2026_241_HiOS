#include "hi_task_policy.h"
#include "hi_music_screen.h"
#include "local_music_player.h"
#include "application.h"
#include "audio_service.h"
#include "board_shim.h"
#include "display.h"
#include "home_screen/home_screen.h"
#include "font_awesome.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <vector>

LV_FONT_DECLARE(font_awesome_30_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
namespace {
constexpr double pi=3.141592653589793;
constexpr uint32_t white=0xFCFCFA,ink=0x555754,subtle=0x9B9D99;
std::atomic<unsigned> serial_source{0},active_serial{0};
std::mutex open_lock;std::string pending_file;
struct View{
 lv_obj_t *root=nullptr,*title=nullptr,*subtitle=nullptr,*position=nullptr,*remaining=nullptr,*progress=nullptr,*volume=nullptr,*stage=nullptr,*disc=nullptr,*library=nullptr,*play=nullptr,*repeat=nullptr,*shuffle=nullptr,*noise=nullptr,*volume_value=nullptr;
 lv_timer_t *pump=nullptr,*status_timer=nullptr;lv_display_t *display=nullptr;
 std::vector<uint32_t> pixels;lv_image_dsc_t bitmap{};
 unsigned serial=0;bool active=false,scanned=false,resume=false,slider_drag=false,volume_drag=false,was_playing=false;
 int gesture=0,start_position=0,scrub_position=0;double rotation=0,needle=80,last_angle=0;
 uint32_t last_frame=0,last_touch=0,last_seek=0,last_volume_apply=0;bool touch_still=false,needle_lifted=false,library_requested=false;
};
View *g=nullptr;
LocalMusicPlayer &Player(){return LocalMusicPlayer::Instance();}
lv_obj_t *Label(lv_obj_t*p,const char*text,int x,int y,int width,const lv_font_t*font=&font_puhui_20_4,uint32_t color=ink){
 auto*l=lv_label_create(p);lv_label_set_text(l,text);lv_obj_set_pos(l,x,y);lv_obj_set_width(l,width);lv_obj_set_style_text_font(l,font,0);lv_obj_set_style_text_color(l,lv_color_hex(color),0);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_obj_remove_flag(l,LV_OBJ_FLAG_CLICKABLE);return l;
}
lv_obj_t *Button(lv_obj_t*p,const char*text,int x,int y,int w,int h,lv_event_cb_t cb,void*arg=nullptr,bool symbol=false){
 auto*b=lv_button_create(p);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);lv_obj_set_style_radius(b,h/2,0);lv_obj_set_style_bg_color(b,lv_color_hex(0xFAFAF8),0);lv_obj_set_style_bg_color(b,lv_color_hex(0xE6E7E3),LV_STATE_PRESSED);lv_obj_set_style_border_width(b,2,0);lv_obj_set_style_border_color(b,lv_color_hex(0xE3E4E0),0);lv_obj_set_style_shadow_width(b,5,0);lv_obj_set_style_shadow_ofs_y(b,3,0);lv_obj_set_style_shadow_opa(b,LV_OPA_20,0);lv_obj_set_style_pad_all(b,0,0);
 lv_obj_set_ext_click_area(b,8);lv_obj_add_flag(b,LV_OBJ_FLAG_PRESS_LOCK);
 lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,arg);screen_swipe_back_ignore(b,true);auto*l=Label(b,text,0,0,w,symbol?&font_awesome_30_4:&font_puhui_20_4);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(l);return b;
}
void SetButton(lv_obj_t*b,const char*text){if(b){auto*l=lv_obj_get_child(b,0);if(strcmp(lv_label_get_text(l),text))lv_label_set_text(l,text);}}
// Bounded integer rendering: no software-double trig/pow in the UI thread.
unsigned IntegerRoot(unsigned n){unsigned result=0,bit=1u<<30;while(bit>n)bit>>=2;while(bit){if(n>=result+bit){n-=result+bit;result=(result>>1)+bit;}else result>>=1;bit>>=2;}return result;}
void MakeDisc(View*v){
 const int side=320;v->pixels.resize(side*side);
 for(int y=0;y<side;y++)for(int x=0;x<side;x++){
  int dx=x*400/320*2-399,dy=y*400/320*2-399;unsigned distance=(unsigned)(dx*dx+dy*dy);
  if(distance>159201u){v->pixels[y*side+x]=0;continue;}
  int r=(int)IntegerRoot(distance)/2,gray,alpha=255;
  if(r>194)gray=135+(r-194)*20;
  else {int reflection=std::max(0,18-std::abs(dx-dy/2)/14);gray=25+(r%4==0?4:0)+reflection;
   if(r<79)gray=x%5<2?184:57;if(r<12)gray=80+(12-r)*100/12;if(r<4)gray=220;}
  if(distance>157609u)alpha=(159201u-distance)*255u/(159201u-157609u);
  gray=std::max(0,std::min(255,gray));v->pixels[y*side+x]=((uint32_t)alpha<<24)|(gray<<16)|(gray<<8)|gray;
 }
 v->bitmap.header.magic=LV_IMAGE_HEADER_MAGIC;v->bitmap.header.cf=LV_COLOR_FORMAT_ARGB8888;v->bitmap.header.w=side;v->bitmap.header.h=side;v->bitmap.header.stride=side*4;v->bitmap.data_size=v->pixels.size()*4;v->bitmap.data=(const uint8_t*)v->pixels.data();
 lv_image_set_src(v->disc,&v->bitmap);lv_image_set_pivot(v->disc,160,160);
}

void Line(lv_layer_t*layer,double x1,double y1,double x2,double y2,int width,uint32_t color,lv_opa_t opacity=255){
 lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.p1.x=x1;d.p1.y=y1;d.p2.x=x2;d.p2.y=y2;d.width=width;d.color=lv_color_hex(color);d.opa=opacity;d.round_start=1;d.round_end=1;lv_draw_line(layer,&d);
}
void Needle(lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);lv_area_t bounds;lv_obj_get_coords(v->stage,&bounds);double angle=v->needle*pi/180,dx=::cosf(angle),dy=::sinf(angle);double px=bounds.x1+503,py=bounds.y1+24,ex=px+270*dx,ey=py+270*dy;
 auto*l=lv_event_get_layer(e);Line(l,px+2,py+3,ex+2,ey+3,11,0x303431,38);Line(l,px,py,ex-20*dx,ey-20*dy,8,0x979B97);Line(l,px-1,py,ex-20*dx-1,ey-20*dy,3,0xFFFFFF);
 Line(l,ex-15*dx,ey-15*dy,ex+18*dx,ey+18*dy,24,0xB7BAB6);Line(l,ex-15*dx-1,ey-15*dy-1,ex+16*dx-1,ey+16*dy-1,20,0xFFFFFF);
 lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.radius=LV_RADIUS_CIRCLE;d.bg_color=lv_color_hex(0xF8F8F6);d.bg_opa=255;d.border_width=2;d.border_color=lv_color_hex(0xD9DCD6);lv_area_t a{(int32_t)px-15,(int32_t)py-15,(int32_t)px+15,(int32_t)py+15};lv_draw_rect(l,&d,&a);
}
void UpdateStatus(View*v){if(!v||!v->active)return;auto&p=Player();auto state=p.GetState();std::string title=p.CurrentTitle();if(title!=lv_label_get_text(v->title))lv_label_set_text(v->title,title.c_str());
 const char*icon=state==LocalMusicPlayer::State::Playing?FONT_AWESOME_PAUSE:FONT_AWESOME_PLAY;SetButton(v->play,icon);
 std::string text=state==LocalMusicPlayer::State::Error||state==LocalMusicPlayer::State::Loading?p.StatusText():"SD 卡 · 本地音乐";if(text!=lv_label_get_text(v->subtitle))lv_label_set_text(v->subtitle,text.c_str());
 int pos=p.PositionMillis()/1000,total=p.DurationMillis()/1000;char a[24],b[24];snprintf(a,sizeof(a),"%02d:%02d",pos/60,pos%60);int remain=std::max(0,total-pos);snprintf(b,sizeof(b),"-%02d:%02d",remain/60,remain%60);lv_label_set_text(v->position,a);lv_label_set_text(v->remaining,b);
 if(!v->slider_drag&&v->gesture==0)lv_slider_set_value(v->progress,total?std::min(10000,pos*10000/total):0,LV_ANIM_OFF);
 if(!v->volume_drag)lv_slider_set_value(v->volume,Application::GetInstance().GetAudioService().GetVolume(),LV_ANIM_OFF);
 char volume_text[32];snprintf(volume_text,sizeof(volume_text),"音量 %d",Application::GetInstance().GetAudioService().GetVolume());if(v->volume_value)lv_label_set_text(v->volume_value,volume_text);
 SetButton(v->repeat,p.RepeatMode()==2?"单曲":p.RepeatMode()==1?"循环":"顺序");SetButton(v->shuffle,p.Shuffle()?"随机开":"随机关");SetButton(v->noise,p.VinylNoise()?"爆豆音开":"爆豆音关");
}
void OnFrame(lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);if(!v->active)return;uint32_t now=lv_tick_get(),dt=lv_tick_elaps(v->last_frame);v->last_frame=now;dt=std::min(dt,(uint32_t)100);
 auto&p=Player();if(v->gesture==0&&!v->slider_drag){if(p.GetState()==LocalMusicPlayer::State::Playing)v->rotation+=dt*.2;
  double target=v->needle_lifted?80.:(p.GetState()==LocalMusicPlayer::State::Playing||p.GetState()==LocalMusicPlayer::State::Paused)?116.+22.*p.PositionMillis()/std::max(1,p.DurationMillis()):80.;double step=dt*.12;v->needle+=std::max(-step,std::min(step,target-v->needle));}
 if(v->rotation>36000||v->rotation< -36000)v->rotation=::fmodf(v->rotation,360.);int angle=(int)::fmodf(v->rotation*10,3600.);if(angle<0)angle+=3600;lv_image_set_rotation(v->disc,angle);
 if(v->gesture==2&&!v->touch_still&&lv_tick_elaps(v->last_touch)>100){p.ScrubTo(v->scrub_position,0);v->touch_still=true;}
}
void Pump(lv_timer_t*t){auto*v=(View*)lv_timer_get_user_data(t);if(!v->active)return;
 auto state=Player().GetState();double target=v->needle_lifted?80.:(state==LocalMusicPlayer::State::Playing||state==LocalMusicPlayer::State::Paused)?116.+22.*Player().PositionMillis()/std::max(1,Player().DurationMillis()):80.;
 if(state==LocalMusicPlayer::State::Playing||v->gesture||std::abs(v->needle-target)>.2)lv_obj_invalidate(v->stage);
}
void StatusTimer(lv_timer_t*t){UpdateStatus((View*)lv_timer_get_user_data(t));}
void Gesture(lv_event_t*e){
 auto*v=(View*)lv_event_get_user_data(e);auto code=lv_event_get_code(e);auto*input=lv_indev_active();if(!input)return;
 lv_point_t point;lv_indev_get_point(input,&point);lv_area_t b;lv_obj_get_coords(v->stage,&b);double x=point.x-b.x1,y=point.y-b.y1;auto&p=Player();
 if(code==LV_EVENT_PRESSED){
  double angle=v->needle*pi/180,ex=503+270*::cosf(angle),ey=24+270*::sinf(angle),r=::sqrtf((x-290)*(x-290)+(y-164)*(y-164));
  v->gesture=::sqrtf((x-ex)*(x-ex)+(y-ey)*(y-ey))<55?1:r<160?2:0;if(!v->gesture)return;
  v->was_playing=p.GetState()==LocalMusicPlayer::State::Playing;v->start_position=p.PositionMillis();v->scrub_position=v->start_position;
  if(v->gesture==1){p.SetPaused(true);v->last_touch=lv_tick_get();}
  else{if(p.GetState()==LocalMusicPlayer::State::Idle){p.Toggle();v->was_playing=true;}v->last_angle=::atan2f(y-164,x-290);v->last_touch=lv_tick_get();v->last_seek=v->last_touch;v->touch_still=false;p.BeginScrub();}
 }else if(code==LV_EVENT_PRESSING&&v->gesture){
  if(v->gesture==1)v->needle=std::max(72.,std::min(140.,::atan2f(y-24,x-503)*180/pi));
  else{uint32_t now=lv_tick_get();double angle=::atan2f(y-164,x-290),delta=(angle-v->last_angle)*180/pi;
   while(delta>180)delta-=360;while(delta< -180)delta+=360;v->last_angle=angle;v->rotation+=delta;
   v->scrub_position=std::max(0,std::min(p.DurationMillis(),v->scrub_position+(int)(delta*5)));
   uint32_t dt=std::max((uint32_t)1,lv_tick_elaps(v->last_touch));if(std::abs(delta)>.1){v->last_touch=now;v->touch_still=false;
    if(lv_tick_elaps(v->last_seek)>=70){p.ScrubTo(v->scrub_position,(int)(delta*5000/dt));v->last_seek=now;}}
  }lv_obj_invalidate(v->stage);
 }else if((code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST)&&v->gesture){
  if(v->gesture==1){if(code==LV_EVENT_RELEASED){v->needle_lifted=v->needle<112.;if(v->needle_lifted)p.SetPaused(true);
    else if(p.GetState()==LocalMusicPlayer::State::Idle)p.Toggle();else p.SetPaused(false);}
   else p.SetPaused(!v->was_playing);
  }else{p.SeekTo(code==LV_EVENT_RELEASED?v->scrub_position:v->start_position);p.EndScrub(v->was_playing);}
  v->gesture=0;v->last_frame=lv_tick_get();
 }
}
struct ScanJob{unsigned serial;std::string file;};
lv_result_t MusicPostUi(lv_async_cb_t callback,void *data){lv_lock();auto result=lv_async_call(callback,data);lv_unlock();return result;}
void BuildLibrary(View*v){if(v->library)lv_obj_clean(v->library);else{v->library=lv_obj_create(v->root);screen_strip_obj_chrome(v->library);lv_obj_set_pos(v->library,20,100);lv_obj_set_size(v->library,680,490);lv_obj_set_style_bg_color(v->library,lv_color_hex(white),0);lv_obj_set_style_bg_opa(v->library,255,0);lv_obj_set_style_pad_all(v->library,12,0);lv_obj_set_flex_flow(v->library,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(v->library,8,0);lv_obj_set_scroll_dir(v->library,LV_DIR_VER);screen_swipe_back_ignore(v->library,true);}
 auto*header=Label(v->library,"歌曲与铃声 · SD 卡 /Music",0,0,620);lv_obj_set_height(header,40);
 for(size_t i=0;i<Player().Count();i++){auto*row=Button(v->library,Player().Title(i).c_str(),0,0,640,66,[](lv_event_t*e){if(!g)return;Player().PlayIndex((int)(intptr_t)lv_event_get_user_data(e));g->library_requested=false;g->needle_lifted=false;lv_obj_add_flag(g->library,LV_OBJ_FLAG_HIDDEN);UpdateStatus(g);},(void*)(intptr_t)i);lv_obj_set_style_radius(row,8,0);auto*l=lv_obj_get_child(row,0);lv_obj_set_width(l,600);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_LEFT,0);}
 if(!Player().Count())Label(v->library,"未找到 MP3/WAV，请检查 SD 卡 /Music",0,0,620);
 if(v->library_requested)lv_obj_remove_flag(v->library,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(v->library,LV_OBJ_FLAG_HIDDEN);
}
void Scan(View*v){auto*job=new ScanJob{v->serial,{}};{std::lock_guard<std::mutex>l(open_lock);job->file.swap(pending_file);}pthread_t t;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,32768);
 int rc=hi_task_create_legacy(&t,&a,[](void*arg)->void*{auto*j=(ScanJob*)arg;Player().ScanLibrary();if(!j->file.empty())Player().PlayFile(j->file,[id=j->serial]{return active_serial==id;});else Player().RestorePlayback([id=j->serial]{return active_serial==id;});
  if(MusicPostUi([](void*arg){auto*j=(ScanJob*)arg;if(g&&g->serial==j->serial&&g->active){BuildLibrary(g);UpdateStatus(g);}delete j;},j)!=LV_RESULT_OK)delete j;return nullptr;},job);pthread_attr_destroy(&a);if(!rc)pthread_detach(t);else{delete job;lv_label_set_text(v->subtitle,"无法启动歌曲扫描");}}
void Suspend(View*v){v->active=false;if(active_serial==v->serial)active_serial=0;if(v->display)lv_display_remove_event_cb_with_user_data(v->display,OnFrame,v);if(v->pump)lv_timer_pause(v->pump);if(v->status_timer)lv_timer_pause(v->status_timer);
 if(g==v){v->resume=Player().GetState()==LocalMusicPlayer::State::Playing;Player().RememberPlayback();Player().EndScrub(false);Player().SetPaused(true);}v->gesture=0;v->slider_drag=false;v->volume_drag=false;
}
void Lifecycle(lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);if(lv_event_get_target_obj(e)!=v->root)return;if(lv_event_get_code(e)==LV_EVENT_SCREEN_UNLOADED)Suspend(v);
 else if(lv_event_get_code(e)==LV_EVENT_SCREEN_LOADED){v->active=true;active_serial=v->serial;v->last_frame=lv_tick_get();v->display=lv_display_get_default();lv_display_add_event_cb(v->display,OnFrame,LV_EVENT_REFR_START,v);lv_timer_resume(v->pump);lv_timer_resume(v->status_timer);v->resume=false;if(!v->scanned){v->scanned=true;Scan(v);}UpdateStatus(v);}
 else if(lv_event_get_code(e)==LV_EVENT_DELETE){Suspend(v);if(v->pump)lv_timer_delete(v->pump);if(v->status_timer)lv_timer_delete(v->status_timer);if(g==v){g=nullptr;Player().Stop();}lv_async_call([](void*p){auto*v=(View*)p;lv_image_cache_drop(&v->bitmap);delete v;},v);}}
}
lv_obj_t*HiMusicScreen::CreateStatic(){
 auto*v=new View;v->serial=++serial_source;g=v;auto*root=lv_obj_create(nullptr);v->root=root;screen_strip_obj_chrome(root);lv_obj_set_size(root,720,720);lv_obj_set_style_bg_color(root,lv_color_hex(white),0);lv_obj_set_style_bg_opa(root,255,0);lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
 Button(root,"返回",12,36,106,56,[](lv_event_t*){HomeScreen::SwitchToHome();});v->title=Label(root,"Hi 音乐",126,32,468,&font_puhui_30_4);lv_obj_set_style_text_align(v->title,LV_TEXT_ALIGN_CENTER,0);v->subtitle=Label(root,"本地歌曲与铃声",126,70,468,&font_puhui_20_4,subtle);lv_obj_set_style_text_align(v->subtitle,LV_TEXT_ALIGN_CENTER,0);
 Button(root,"列表",602,36,106,56,[](lv_event_t*){if(g){g->library_requested=!g->library_requested;if(!g->library)BuildLibrary(g);if(g->library_requested)lv_obj_remove_flag(g->library,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(g->library,LV_OBJ_FLAG_HIDDEN);}});
 v->position=Label(root,"00:00",22,102,80);v->remaining=Label(root,"-00:00",626,102,80);v->progress=lv_slider_create(root);lv_obj_set_pos(v->progress,110,115);lv_obj_set_size(v->progress,500,6);lv_slider_set_range(v->progress,0,10000);screen_swipe_back_ignore(v->progress,true);lv_obj_set_ext_click_area(v->progress,20);lv_obj_add_flag(v->progress,LV_OBJ_FLAG_PRESS_LOCK);
 lv_obj_set_style_bg_color(v->progress,lv_color_hex(0xDDDFDC),LV_PART_MAIN);lv_obj_set_style_bg_color(v->progress,lv_color_hex(0xA7ACA5),LV_PART_INDICATOR);lv_obj_set_style_bg_color(v->progress,lv_color_hex(white),LV_PART_KNOB);lv_obj_set_style_shadow_width(v->progress,3,LV_PART_KNOB);
 lv_obj_add_event_cb(v->progress,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);auto c=lv_event_get_code(e);if(c==LV_EVENT_PRESSED){v->slider_drag=true;v->was_playing=Player().GetState()==LocalMusicPlayer::State::Playing;v->start_position=Player().PositionMillis();Player().BeginScrub();}
  else if(c==LV_EVENT_RELEASED||c==LV_EVENT_PRESS_LOST){if(!v->slider_drag)return;Player().SeekTo(c==LV_EVENT_RELEASED?(int)((int64_t)lv_slider_get_value(v->progress)*Player().DurationMillis()/10000):v->start_position);Player().EndScrub(v->was_playing);v->slider_drag=false;}
 },LV_EVENT_ALL,v);
 v->stage=lv_obj_create(root);screen_strip_obj_chrome(v->stage);lv_obj_set_pos(v->stage,50,132);lv_obj_set_size(v->stage,620,340);lv_obj_remove_flag(v->stage,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_opa(v->stage,0,0);lv_obj_add_flag(v->stage,LV_OBJ_FLAG_CLICKABLE);screen_swipe_back_ignore(v->stage,true);
 v->disc=lv_image_create(v->stage);lv_obj_set_pos(v->disc,130,4);lv_obj_remove_flag(v->disc,LV_OBJ_FLAG_CLICKABLE);MakeDisc(v);lv_obj_add_event_cb(v->stage,Needle,LV_EVENT_DRAW_POST,v);lv_obj_add_event_cb(v->stage,Gesture,LV_EVENT_ALL,v);
 v->repeat=Button(root,"顺序",26,505,104,48,[](lv_event_t*){Player().SetRepeat(Player().RepeatMode()+1);if(g)UpdateStatus(g);});Button(root,"\xEF\x81\x88",158,488,88,88,[](lv_event_t*){Player().Previous();},nullptr,true);
 v->play=Button(root,FONT_AWESOME_PLAY,304,476,112,112,[](lv_event_t*){if(g)g->needle_lifted=false;Player().Toggle();if(g)UpdateStatus(g);},nullptr,true);Button(root,"\xEF\x81\x91",474,488,88,88,[](lv_event_t*){Player().Next();},nullptr,true);
 v->shuffle=Button(root,"随机关",586,505,108,48,[](lv_event_t*){Player().SetShuffle(!Player().Shuffle());if(g)UpdateStatus(g);});
 v->noise=Button(root,"爆豆音关",26,614,120,38,[](lv_event_t*){Player().SetVinylNoise(!Player().VinylNoise());if(g)UpdateStatus(g);});
 v->volume_value=Label(root,"音量",220,602,400);lv_obj_set_style_text_align(v->volume_value,LV_TEXT_ALIGN_CENTER,0);
 Button(root,"-",158,610,46,42,[](lv_event_t*){auto &a=Application::GetInstance().GetAudioService();a.SetVolume(std::max(0,a.GetVolume()-5));if(g)UpdateStatus(g);});
 Button(root,"+",650,610,46,42,[](lv_event_t*){auto &a=Application::GetInstance().GetAudioService();a.SetVolume(std::min(100,a.GetVolume()+5));if(g)UpdateStatus(g);});
 v->volume=lv_slider_create(root);lv_obj_set_pos(v->volume,222,646);lv_obj_set_size(v->volume,400,8);lv_slider_set_range(v->volume,0,100);screen_swipe_back_ignore(v->volume,true);lv_obj_set_ext_click_area(v->volume,22);lv_obj_add_flag(v->volume,LV_OBJ_FLAG_PRESS_LOCK);
 lv_obj_set_style_bg_color(v->volume,lv_color_hex(0xD5D7D2),LV_PART_MAIN);lv_obj_set_style_bg_color(v->volume,lv_color_hex(0xA7ACA4),LV_PART_INDICATOR);lv_obj_set_style_bg_color(v->volume,lv_color_hex(white),LV_PART_KNOB);
 lv_obj_add_event_cb(v->volume,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);auto code=lv_event_get_code(e);
   if(code==LV_EVENT_PRESSED){v->volume_drag=true;v->last_volume_apply=lv_tick_get()-60;return;}
   const bool finished=code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST;
   if(code!=LV_EVENT_VALUE_CHANGED&&!finished)return;
   if(finished||lv_tick_elaps(v->last_volume_apply)>=60){v->last_volume_apply=lv_tick_get();Application::GetInstance().GetAudioService().SetVolume(lv_slider_get_value(v->volume),finished);}
   if(finished){v->volume_drag=false;UpdateStatus(v);}},LV_EVENT_ALL,v);
 v->pump=lv_timer_create(Pump,40,v);v->status_timer=lv_timer_create(StatusTimer,250,v);lv_timer_pause(v->pump);lv_timer_pause(v->status_timer);
 lv_obj_add_event_cb(root,Lifecycle,LV_EVENT_SCREEN_LOADED,v);lv_obj_add_event_cb(root,Lifecycle,LV_EVENT_SCREEN_UNLOADED,v);lv_obj_add_event_cb(root,Lifecycle,LV_EVENT_DELETE,v);screen_attach_swipe_back(root,[]{HomeScreen::SwitchToHome();});screen_set_back_sensitivity(root,56,64);return root;
}
lv_obj_t*HiMusicScreen::Create(){root_=CreateStatic();return root_;}
bool HiMusicScreen::OpenFile(const char*path){
 if(!path||strncmp(path,"/sdcard/",8)!=0)return false;
 lv_lock();
 if(g&&g->active){Player().PlayFile(path);g->needle_lifted=false;g->library_requested=false;if(g->library)lv_obj_add_flag(g->library,LV_OBJ_FLAG_HIDDEN);UpdateStatus(g);lv_unlock();return true;}
 {std::lock_guard<std::mutex>l(open_lock);pending_file=path;}
 const bool ok=HomeScreen::LaunchApp("hi_music");lv_unlock();return ok;
}
bool HiMusicScreen::PlayLocalQuery(const std::string &query){
 struct Requests{std::mutex mutex;std::string query;bool running=false;unsigned generation=0;};
 static auto *requests=new Requests;
 std::unique_lock<std::mutex>lock(requests->mutex);requests->query=query;++requests->generation;
 if(requests->running)return true;requests->running=true;
 pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,32768);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
 pthread_t worker;int rc=hi_task_create_legacy(&worker,&attr,[](void *arg)->void*{
  auto *r=static_cast<Requests*>(arg);
  for(;;){std::string query;unsigned generation;{std::lock_guard<std::mutex>l(r->mutex);query.swap(r->query);generation=r->generation;}
   std::string path,title;const bool found=Player().FindLocalTrack(query,path,title);
   {std::unique_lock<std::mutex>l(r->mutex);if(generation!=r->generation)continue;
    if(found){Application::GetInstance().RequestVoiceUiDesired(false);HiMusicScreen::OpenFile(path.c_str());}
    else if(auto *d=Board::GetInstance().GetDisplay())d->SetChatMessage("assistant",("本地 /Music 中未找到："+query).c_str());
    r->running=false;return nullptr;}
  }
 },requests);pthread_attr_destroy(&attr);
 if(rc){requests->running=false;return false;}return true;
}
void HiMusicScreen::LifecycleCallback(screen_lifecycle_event_t event){PwrKey_OnScreenLifecycle("hi_music",event);}
