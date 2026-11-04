// Global full-screen call surface. It lives on the top layer and never
// unloads/recreates the app underneath an incoming or established call.
lv_obj_t *s_incoming_card=nullptr,*s_incoming_name=nullptr,*s_incoming_number=nullptr,*s_incoming_hint=nullptr;
lv_obj_t *s_call_float=nullptr,*s_call_grid=nullptr,*s_call_footer=nullptr,*s_call_end=nullptr,*s_call_clock=nullptr;
lv_obj_t *s_call_tiles[6]{};
lv_obj_t *s_call_route_popup=nullptr;
int s_call_volume; // reset to unknown when a call card is created
lv_timer_t *s_incoming_timer=nullptr;
bool s_incoming_dragging=false,s_incoming_accept_requested=false,s_incoming_action_busy=false;
bool s_call_muted=false,s_call_speaker=true,s_call_feature_busy=false;
int s_incoming_start_y=0,s_incoming_start_x=0,s_incoming_card_y=432,s_call_drag_delta=0,s_global_call_state=METALIO_CALL_IDLE,s_call_subpage=0;
uint32_t s_global_call_generation=0,s_incoming_job_epoch=0,s_call_connected_tick=0,s_call_poll_tick=0;
bool s_call_clock_started=false;
bool s_call_powering_off=false;
std::string s_incoming_last_number,s_call_dtmf,s_call_record_last_status;
std::vector<PhoneService::Contact> s_overlay_contacts;
void UpdateIncoming(int state);
void RenderCallBody(int page);
void IncomingAction(bool answer);
void RefreshCallRoute();

void CallRemoveEdges(lv_obj_t *obj){
 lv_state_t states[]={LV_STATE_DEFAULT,LV_STATE_PRESSED,LV_STATE_FOCUSED,LV_STATE_FOCUS_KEY,LV_STATE_CHECKED,LV_STATE_DISABLED};
 for(auto state:states){lv_obj_set_style_border_width(obj,0,(lv_style_selector_t)state);lv_obj_set_style_outline_width(obj,0,(lv_style_selector_t)state);lv_obj_set_style_shadow_width(obj,0,(lv_style_selector_t)state);}
 for(uint32_t i=0;i<lv_obj_get_child_count(obj);++i)CallRemoveEdges(lv_obj_get_child(obj,i));
}
lv_obj_t *CallLabel(lv_obj_t *parent,const char *text,int x,int y,int width,bool large=false,uint32_t color=0xF2F2F2){
 auto *l=lv_label_create(parent);lv_label_set_text(l,text);lv_obj_set_pos(l,x,y);lv_obj_set_width(l,width);
 lv_obj_set_style_text_font(l,large?&font_puhui_30_4:&font_puhui_20_4,0);lv_obj_set_style_text_color(l,lv_color_hex(color),0);
 lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_obj_remove_flag(l,LV_OBJ_FLAG_CLICKABLE);return l;
}
void CallLine(lv_layer_t *layer,int x1,int y1,int x2,int y2,int width,uint32_t color){
 lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.p1.x=x1;d.p1.y=y1;d.p2.x=x2;d.p2.y=y2;d.width=width;d.color=lv_color_hex(color);d.round_start=d.round_end=1;lv_draw_line(layer,&d);
}
void CallRect(lv_layer_t *layer,int x,int y,int w,int h,int radius,uint32_t color,bool outline=false){
 lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.radius=radius;d.bg_color=lv_color_hex(color);d.bg_opa=outline?0:255;d.border_color=lv_color_hex(color);d.border_width=outline?3:0;
 lv_area_t a{x,y,x+w-1,y+h-1};lv_draw_rect(layer,&d,&a);
}
void CallArc(lv_layer_t *layer,int x,int y,int radius,int from,int to,int width,uint32_t color){
 lv_draw_arc_dsc_t d;lv_draw_arc_dsc_init(&d);d.center.x=x;d.center.y=y;d.radius=radius;d.start_angle=from;d.end_angle=to;d.width=width;d.color=lv_color_hex(color);d.opa=255;d.rounded=1;lv_draw_arc(layer,&d);
}
void DrawCallIcon(lv_event_t *event){
 auto *obj=lv_event_get_target_obj(event);int kind=(int)(intptr_t)lv_event_get_user_data(event);lv_area_t a;lv_obj_get_coords(obj,&a);
 int x=(a.x1+a.x2)/2,y=a.y1+54;auto *l=lv_event_get_layer(event);uint32_t c=0xDDDEDF;
 if(kind==0){CallRect(l,x-30,y-10,13,22,2,c);for(int i=0;i<22;i++)CallLine(l,x-17+i/2,y-10-i/2,x-17+i/2,y+11+i/2,2,c);CallArc(l,x+3,y,19,305,55,4,c);CallArc(l,x+3,y,30,305,55,4,c);}
 else if(kind==1){CallArc(l,x,y,29,0,360,4,c);CallRect(l,x-12,y-12,25,25,13,s_call_record_running?0xE06B60:c);}
 else if(kind==2){CallRect(l,x-9,y-29,19,36,10,c);CallArc(l,x,y-9,20,0,180,4,c);CallLine(l,x,y+11,x,y+29,4,c);CallLine(l,x-13,y+29,x+13,y+29,4,c);if(s_call_muted)CallLine(l,x-28,y+29,x+28,y-29,5,0xF2BC68);}
 else if(kind==3){CallRect(l,x-12,y-30,25,28,14,c);CallLine(l,x,y-1,x,y+14,13,c);CallLine(l,x-23,y+23,x+23,y+23,16,c);}
 else if(kind==4){for(int r=0;r<3;r++)for(int col=0;col<3;col++)CallRect(l,x-26+col*19,y-26+r*19,13,13,2,c);}
 else if(kind==5){CallRect(l,x-26,y-27,47,51,3,c,true);CallLine(l,x-13,y+14,x+29,y-28,8,c);}
 else { // handset, shown in the floating incoming control
  y=(a.y1+a.y2)/2;CallArc(l,x,y+7,42,205,335,13,0xFFFFFF);CallLine(l,x-35,y-17,x-37,y+3,20,0xFFFFFF);CallLine(l,x+35,y-17,x+37,y+3,20,0xFFFFFF);
 }
}
void IncomingBounce(){
 if(!s_call_float||s_global_call_state!=METALIO_CALL_INCOMING||s_incoming_dragging||s_incoming_action_busy)return;
 lv_anim_delete(s_call_float,nullptr);lv_anim_t anim;lv_anim_init(&anim);lv_anim_set_var(&anim,s_call_float);
 lv_anim_set_exec_cb(&anim,[](void *obj,int32_t y){lv_obj_set_y((lv_obj_t*)obj,y);});lv_anim_set_values(&anim,424,440);
 lv_anim_set_duration(&anim,700);lv_anim_set_reverse_duration(&anim,700);lv_anim_set_repeat_delay(&anim,350);lv_anim_set_repeat_count(&anim,LV_ANIM_REPEAT_INFINITE);lv_anim_set_path_cb(&anim,lv_anim_path_ease_in_out);lv_anim_start(&anim);
}
void ReturnIncomingFloat(){
 if(!s_call_float)return;s_call_drag_delta=0;lv_obj_set_style_bg_color(s_call_float,lv_color_hex(0x658A52),0);lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,s_call_float);
 lv_anim_set_exec_cb(&a,[](void *o,int32_t y){lv_obj_set_y((lv_obj_t*)o,y);});lv_anim_set_values(&a,lv_obj_get_y(s_call_float),432);lv_anim_set_duration(&a,180);lv_anim_set_path_cb(&a,lv_anim_path_ease_out);
 lv_anim_set_completed_cb(&a,[](lv_anim_t*){IncomingBounce();});lv_anim_start(&a);
}
void IncomingGesture(lv_event_t *event){
 if(s_global_call_state!=METALIO_CALL_INCOMING||!s_call_float||s_incoming_action_busy)return;
 auto code=lv_event_get_code(event);if(code!=LV_EVENT_PRESSED&&code!=LV_EVENT_PRESSING&&code!=LV_EVENT_RELEASED&&code!=LV_EVENT_PRESS_LOST)return;
 auto *input=lv_indev_active();if(!input)return;lv_point_t point{};lv_indev_get_point(input,&point);
 if(code==LV_EVENT_PRESSED){s_incoming_dragging=true;s_incoming_start_y=point.y;s_incoming_start_x=point.x;s_incoming_card_y=lv_obj_get_y(s_call_float);lv_anim_delete(s_call_float,nullptr);}
 else if(code==LV_EVENT_PRESSING&&s_incoming_dragging){
  int dy=point.y-s_incoming_start_y,dx=point.x-s_incoming_start_x;if(std::abs(dx)>std::abs(dy)+30)dy=0;
  s_call_drag_delta=std::max(-190,std::min(190,dy));lv_obj_set_y(s_call_float,s_incoming_card_y+s_call_drag_delta);
  lv_obj_set_style_bg_color(s_call_float,lv_color_hex(dy<0?0xAD4E51:0x658A52),0);
  lv_label_set_text(s_incoming_hint,dy>=92?"松开接听":dy<=-92?"松开挂断":"向下滑动接听 · 向上滑动挂断");IdlePower_NotifyActivity();
 }else if((code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST)&&s_incoming_dragging){
  s_incoming_dragging=false;int dy=s_call_drag_delta,dx=point.x-s_incoming_start_x;
  if(code==LV_EVENT_RELEASED&&std::abs(dy)>std::abs(dx)&&std::abs(dy)>=92)IncomingAction(dy>0);
  else{lv_label_set_text(s_incoming_hint,"向下滑动接听 · 向上滑动挂断");ReturnIncomingFloat();}
 }
}
std::string CallMatchNumber(const std::string &number){std::string digits;for(char c:number)if(c>='0'&&c<='9')digits+=c;
 if(digits.size()==15&&digits.compare(0,4,"0086")==0)digits.erase(0,4);
 else if(digits.size()==13&&digits.compare(0,2,"86")==0)digits.erase(0,2);return digits;}
void RefreshIncomingIdentity(){
 if(!s_incoming_card)return;std::string number=PhoneService::IncomingNumber();static uint32_t checked=0;if(number==s_incoming_last_number&&lv_tick_elaps(checked)<1500)return;checked=lv_tick_get();
 s_incoming_last_number=number;std::string name=PhoneService::IncomingName();
 if(name.empty())name=number.empty()?"来电":number;
 const std::string region=PhoneService::NumberRegion(number);
 std::string detail=name==number||number.empty()?"":number;
 if(!region.empty()){if(!detail.empty())detail+=" · ";detail+=region;}
 lv_label_set_text(s_incoming_name,name.c_str());lv_label_set_text(s_incoming_number,detail.c_str());
}
void IncomingAction(bool answer){
 if(s_incoming_action_busy)return;int actual=metalio_nt26_get_call_state();
 if(answer&&actual!=METALIO_CALL_INCOMING)return;if(!answer&&actual==METALIO_CALL_IDLE)return;
 if(!answer)PhoneService::CancelRingtone();
 s_incoming_action_busy=true;s_incoming_accept_requested=answer;s_incoming_job_epoch=0x80000000u|s_global_call_generation;
 if(s_call_float){lv_anim_delete(s_call_float,nullptr);lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,s_call_float);lv_anim_set_exec_cb(&a,[](void*o,int32_t y){lv_obj_set_y((lv_obj_t*)o,y);});lv_anim_set_values(&a,lv_obj_get_y(s_call_float),answer?740:-190);lv_anim_set_duration(&a,240);lv_anim_set_path_cb(&a,lv_anim_path_ease_in);lv_anim_start(&a);}
 if(s_incoming_hint)lv_label_set_text(s_incoming_hint,answer?"正在接听…":"正在结束通话…");
 auto result=answer?s_default_driver.Answer(s_incoming_job_epoch):s_default_driver.Hangup(s_incoming_job_epoch);
 if(result!=ESP_OK){s_incoming_action_busy=false;s_incoming_accept_requested=false;if(s_incoming_hint)lv_label_set_text(s_incoming_hint,"系统忙，请重新滑动");ReturnIncomingFloat();}
}
void CallFeature(CallScreen::JobKind kind,int value){
 if(s_call_feature_busy||metalio_nt26_get_call_state()==METALIO_CALL_IDLE)return;
 s_call_feature_busy=true;if(s_default_driver.Feature(kind,value,s_global_call_generation)!=ESP_OK){s_call_feature_busy=false;if(s_call_footer)lv_label_set_text(s_call_footer,"操作未能提交，请重试");}
 RefreshCallRoute();
}
void CallBodyButton(lv_obj_t *parent,const char *text,int x,int y,int width,int height,lv_event_cb_t cb,void *data=nullptr){
 auto *b=PhoneButton(parent,text,x,y,width,height,cb,data);lv_obj_set_style_bg_color(b,lv_color_hex(0x37393C),0);lv_obj_set_style_bg_color(b,lv_color_hex(0x4C5055),LV_STATE_PRESSED);lv_obj_set_style_bg_grad_color(b,lv_color_hex(0x2C2D30),0);lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_VER,0);lv_obj_set_style_border_color(b,lv_color_hex(0x202124),0);
 lv_obj_set_style_text_color(lv_obj_get_child(b,0),lv_color_hex(0xEBEBEB),0);
}
lv_obj_t*CallRouteChild(unsigned index){
 if(!s_call_route_popup||lv_obj_get_child_count(s_call_route_popup)==0)return nullptr;
 auto*box=lv_obj_get_child(s_call_route_popup,0);return index<lv_obj_get_child_count(box)?lv_obj_get_child(box,index):nullptr;
}
void RefreshCallRoute(){
 auto*s_call_volume_slider=CallRouteChild(3);auto*s_call_volume_label=CallRouteChild(2);
 auto*bt=CallRouteChild(4);auto*s_call_bt_label=bt&&lv_obj_get_child_count(bt)?lv_obj_get_child(bt,0):nullptr;
 if(!s_call_route_popup)return;
 if(s_call_bt_label){
  bool connected=BluetoothScreen::IsHeadsetConnected();int route=BluetoothScreen::PhoneAudioRoute();
  lv_label_set_text(s_call_bt_label,route==2?"蓝牙耳机 · 当前":connected?"蓝牙耳机":"蓝牙耳机未连接");
  auto*button=lv_obj_get_parent(s_call_bt_label);if(!connected||s_call_feature_busy)lv_obj_add_state(button,LV_STATE_DISABLED);else lv_obj_remove_state(button,LV_STATE_DISABLED);
  auto*box=lv_obj_get_parent(button);auto*speaker=lv_obj_get_child(box,1);lv_label_set_text(lv_obj_get_child(speaker,0),route==1?"扬声器 · 当前":"扬声器");
  auto*hint=lv_obj_get_child(box,5);lv_label_set_text(hint,route==2?"通过蓝牙耳机通话":route==1?"通过扬声器通话":"正在确认通话通路…");
 }
 if(s_call_volume_slider){
  if(s_call_feature_busy||s_call_volume<0)lv_obj_add_state(s_call_volume_slider,LV_STATE_DISABLED);
  else lv_obj_remove_state(s_call_volume_slider,LV_STATE_DISABLED);
  if(!s_call_feature_busy&&!lv_obj_has_state(s_call_volume_slider,LV_STATE_PRESSED)&&s_call_volume>=0)
   lv_slider_set_value(s_call_volume_slider,s_call_volume,LV_ANIM_OFF);
 }
 if(s_call_volume_label&&(!s_call_volume_slider||!lv_obj_has_state(s_call_volume_slider,LV_STATE_PRESSED))){char text[80];
  if(s_call_feature_busy)snprintf(text,sizeof(text),"正在读取或设置通话音量…");
  else if(s_call_volume<0)snprintf(text,sizeof(text),"通话音量暂无法读取");
  else snprintf(text,sizeof(text),"通话音量  %d%%",s_call_volume);
  lv_label_set_text(s_call_volume_label,text);
 }
}
void CloseCallRoute(){if(s_call_route_popup)lv_obj_delete(s_call_route_popup);s_call_route_popup=nullptr;}
void OpenCallRoute(){
 if(!s_incoming_card||s_call_route_popup||s_incoming_action_busy)return;
 auto *shade=lv_obj_create(s_incoming_card);s_call_route_popup=shade;screen_strip_obj_chrome(shade);
 lv_obj_set_pos(shade,0,0);lv_obj_set_size(shade,720,720);lv_obj_set_style_bg_color(shade,lv_color_black(),0);lv_obj_set_style_bg_opa(shade,LV_OPA_40,0);
 lv_obj_remove_flag(shade,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(shade,LV_OBJ_FLAG_CLICKABLE);screen_swipe_back_ignore(shade,true);
 lv_obj_add_event_cb(shade,[](lv_event_t *e){if(lv_event_get_target_obj(e)==lv_event_get_current_target_obj(e))CloseCallRoute();},LV_EVENT_CLICKED,nullptr);
 lv_obj_add_event_cb(shade,[](lv_event_t*){s_call_route_popup=nullptr;},LV_EVENT_DELETE,nullptr);
 auto *box=lv_obj_create(shade);screen_strip_obj_chrome(box);lv_obj_set_pos(box,142,214);lv_obj_set_size(box,436,390);
 lv_obj_remove_flag(box,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(box,LV_OBJ_FLAG_CLICKABLE);screen_swipe_back_ignore(box,true);
 lv_obj_set_style_bg_color(box,lv_color_hex(0x292B30),0);lv_obj_set_style_bg_opa(box,255,0);lv_obj_set_style_radius(box,18,0);
 CallLabel(box,"通话声音",18,16,400,true);
 auto *speaker=PhoneButton(box,s_call_speaker?"扬声器 · 当前":"扬声器",18,65,400,60,[](lv_event_t*){CloseCallRoute();CallFeature(CallScreen::JobKind::kSpeaker,1);});
 lv_obj_set_style_bg_color(speaker,lv_color_hex(0x43523F),0);lv_obj_set_style_text_color(lv_obj_get_child(speaker,0),lv_color_white(),0);lv_obj_set_style_radius(speaker,10,0);
 CallLabel(box,"正在读取通话音量…",18,138,400);
 auto*s_call_volume_slider=lv_slider_create(box);lv_obj_set_pos(s_call_volume_slider,42,187);lv_obj_set_size(s_call_volume_slider,352,22);
 lv_slider_set_range(s_call_volume_slider,0,100);lv_obj_add_state(s_call_volume_slider,LV_STATE_DISABLED);
 lv_obj_add_event_cb(s_call_volume_slider,[](lv_event_t *e){
  if(lv_event_get_code(e)==LV_EVENT_VALUE_CHANGED){
   char text[64];snprintf(text,sizeof(text),"通话音量  %ld%% · 松开应用",(long)lv_slider_get_value(lv_event_get_target_obj(e)));
   if(auto*label=CallRouteChild(2))lv_label_set_text(label,text);
  }else if(lv_event_get_code(e)==LV_EVENT_RELEASED)
   CallFeature(CallScreen::JobKind::kVolume,lv_slider_get_value(lv_event_get_target_obj(e)));
 },LV_EVENT_ALL,nullptr);
 // Reuse the existing small route popup and its acknowledged background job.
 auto*headset=PhoneButton(box,"蓝牙耳机",18,235,400,52,[](lv_event_t*){CloseCallRoute();CallFeature(CallScreen::JobKind::kSpeaker,0);});
 lv_obj_set_style_bg_color(headset,lv_color_hex(0x354958),0);lv_obj_set_style_text_color(lv_obj_get_child(headset,0),lv_color_white(),0);lv_obj_set_style_radius(headset,10,0);

 CallLabel(box,"正在确认通话通路…",18,296,400,false,0xA2A5AC);
 auto *cancel=PhoneButton(box,"完成",18,335,400,44,[](lv_event_t*){CloseCallRoute();});
 lv_obj_set_style_bg_color(cancel,lv_color_hex(0x35383E),0);lv_obj_set_style_text_color(lv_obj_get_child(cancel,0),lv_color_hex(0xDDE0E5),0);lv_obj_set_style_radius(cancel,10,0);
 CallRemoveEdges(shade);
 RefreshCallRoute();CallFeature(CallScreen::JobKind::kReadVolume,0);
}
void RenderCallBody(int page){
 if(!s_call_grid)return;s_call_subpage=page;lv_obj_clean(s_call_grid);for(auto &p:s_call_tiles)p=nullptr;
 lv_obj_remove_flag(s_call_grid,LV_OBJ_FLAG_SCROLLABLE);
 if(page==0){const char *names[]={"音频","录音","静音","联系人","拨号键盘","便签"};
  for(int i=0;i<6;i++){
   auto *b=PhoneButton(s_call_grid,names[i],(i%3)*216,(i/3)*146,216,146,[](lv_event_t *e){int i=(int)(intptr_t)lv_event_get_user_data(e);
    if(i==0)OpenCallRoute();else if(i==1)ToggleCallRecording();else if(i==2)CallFeature(CallScreen::JobKind::kMute,!s_call_muted);else if(i==3)RenderCallBody(1);else if(i==4)RenderCallBody(2);
   },(void*)(intptr_t)i);s_call_tiles[i]=b;
   lv_obj_set_style_bg_color(b,lv_color_hex(0x37393C),0);lv_obj_set_style_bg_color(b,lv_color_hex(0x4C5055),LV_STATE_PRESSED);lv_obj_set_style_bg_grad_color(b,lv_color_hex(0x2C2D30),0);lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_VER,0);lv_obj_set_style_border_color(b,lv_color_hex(0x202124),0);
   auto *label=lv_obj_get_child(b,0);lv_obj_set_style_text_color(label,lv_color_hex(0xD8D9DA),0);lv_obj_align(label,LV_ALIGN_BOTTOM_MID,0,-18);lv_obj_add_event_cb(b,DrawCallIcon,LV_EVENT_DRAW_POST,(void*)(intptr_t)i);
   if(i==5){lv_obj_remove_flag(b,LV_OBJ_FLAG_CLICKABLE);lv_obj_set_style_opa(b,LV_OPA_60,0);} // Logo only.
  }
 }else if(page==1){
  lv_obj_add_flag(s_call_grid,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(s_call_grid,LV_DIR_VER);
  s_overlay_contacts=PhoneService::Contacts();CallBodyButton(s_call_grid,"返回通话",0,0,648,44,[](lv_event_t*){RenderCallBody(0);});int y=48;
  for(size_t i=0;i<s_overlay_contacts.size();i++,y+=62){auto &c=s_overlay_contacts[i];CallBodyButton(s_call_grid,(c.name+"  "+c.number).c_str(),0,y,648,60,[](lv_event_t *e){size_t i=(size_t)lv_event_get_user_data(e);if(i<s_overlay_contacts.size()&&s_call_footer)lv_label_set_text(s_call_footer,(s_overlay_contacts[i].name+"  "+s_overlay_contacts[i].number).c_str());},(void*)i);}
  if(s_overlay_contacts.empty())CallLabel(s_call_grid,"暂无联系人",0,110,648);
 }else{
  CallBodyButton(s_call_grid,"返回通话",0,0,648,36,[](lv_event_t*){RenderCallBody(0);});
  const char *digits="123456789*0#";for(int i=0;i<12;i++){char label[2]={digits[i],0};CallBodyButton(s_call_grid,label,(i%3)*216,40+(i/3)*63,216,63,[](lv_event_t *e){int ch=(int)(intptr_t)lv_event_get_user_data(e);if(metalio_nt26_get_call_state()!=METALIO_CALL_ACTIVE)return;CallFeature(CallScreen::JobKind::kDtmf,ch);},(void*)(intptr_t)digits[i]);}
 }
 CallRemoveEdges(s_call_grid);
}
void BuildCallIdentity(bool incoming){
 auto *circle=lv_obj_create(s_incoming_card);screen_strip_obj_chrome(circle);int side=incoming?150:112;
 lv_obj_set_pos(circle,(720-side)/2,incoming?44:20);lv_obj_set_size(circle,side,side);lv_obj_remove_flag(circle,LV_OBJ_FLAG_SCROLLABLE);lv_obj_remove_flag(circle,LV_OBJ_FLAG_CLICKABLE);lv_obj_set_style_radius(circle,LV_RADIUS_CIRCLE,0);lv_obj_set_style_clip_corner(circle,true,0);
 lv_obj_set_style_bg_color(circle,lv_color_hex(0x55575B),0);lv_obj_set_style_bg_grad_color(circle,lv_color_hex(0x34363B),0);lv_obj_set_style_bg_grad_dir(circle,LV_GRAD_DIR_VER,0);lv_obj_set_style_bg_opa(circle,255,0);lv_obj_set_style_border_width(circle,3,0);lv_obj_set_style_border_color(circle,lv_color_hex(0xDDDDE0),0);
 auto *image=CreateContactAvatar(circle,(side-50)/2,(side-50)/2);lv_image_set_scale(image,incoming?650:470);lv_obj_set_style_image_recolor(image,lv_color_hex(0xDFE0E2),0);lv_obj_set_style_image_recolor_opa(image,255,0);
 s_incoming_name=CallLabel(s_incoming_card,"",40,incoming?218:145,640,true);
 s_incoming_number=CallLabel(s_incoming_card,"",40,incoming?266:186,640,false,0xB8BBC1);
 s_incoming_hint=CallLabel(s_incoming_card,incoming?"向下滑动接听 · 向上滑动挂断":"",32,incoming?650:587,656,false,0xD3D5D8);
 s_incoming_last_number="!initial";RefreshIncomingIdentity();
}
void RenderGlobalCall(bool incoming){
 CloseCallRoute();
 if(s_call_float)lv_anim_delete(s_call_float,nullptr);
 s_call_float=s_call_grid=s_call_footer=s_call_clock=s_call_end=nullptr;for(auto &p:s_call_tiles)p=nullptr;
 lv_obj_clean(s_incoming_card);s_incoming_dragging=false;s_call_drag_delta=0;s_incoming_action_busy=false;
 lv_obj_set_style_bg_color(s_incoming_card,lv_color_hex(incoming?0x111A2D:0x39393D),0);lv_obj_set_style_bg_grad_color(s_incoming_card,lv_color_hex(incoming?0x222F46:0x303034),0);lv_obj_set_style_bg_grad_dir(s_incoming_card,LV_GRAD_DIR_VER,0);
 BuildCallIdentity(incoming);
 if(incoming){
  CallLabel(s_incoming_card,"来电",40,310,640,true,0xD6DBE5);s_call_float=lv_obj_create(s_incoming_card);screen_strip_obj_chrome(s_call_float);lv_obj_set_size(s_call_float,146,146);lv_obj_set_pos(s_call_float,287,432);lv_obj_remove_flag(s_call_float,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(s_call_float,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_radius(s_call_float,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(s_call_float,lv_color_hex(0x658A52),0);lv_obj_set_style_bg_opa(s_call_float,255,0);lv_obj_set_style_border_width(s_call_float,4,0);lv_obj_set_style_border_color(s_call_float,lv_color_hex(0xA4B3C6),0);lv_obj_set_style_shadow_width(s_call_float,20,0);lv_obj_set_style_shadow_opa(s_call_float,LV_OPA_30,0);lv_obj_add_event_cb(s_call_float,DrawCallIcon,LV_EVENT_DRAW_POST,(void*)6);IncomingBounce();
 }else{
  s_call_clock=CallLabel(s_incoming_card,"正在拨号…",40,223,640,false,0xA8CE6A);
  s_call_grid=lv_obj_create(s_incoming_card);screen_strip_obj_chrome(s_call_grid);lv_obj_set_pos(s_call_grid,36,274);lv_obj_set_size(s_call_grid,648,292);lv_obj_set_style_radius(s_call_grid,14,0);lv_obj_set_style_clip_corner(s_call_grid,true,0);lv_obj_set_style_border_width(s_call_grid,2,0);lv_obj_set_style_border_color(s_call_grid,lv_color_hex(0x1F2023),0);lv_obj_set_style_bg_opa(s_call_grid,255,0);RenderCallBody(0);
  s_call_footer=s_incoming_hint;
  auto *end=PhoneButton(s_incoming_card,"结束",32,632,656,66,[](lv_event_t*){IncomingAction(false);});s_call_end=end;
  lv_obj_set_style_radius(end,13,0);lv_obj_set_style_border_width(end,3,0);lv_obj_set_style_border_color(end,lv_color_hex(0x202024),0);lv_obj_set_style_bg_color(end,lv_color_hex(0xC85459),0);lv_obj_set_style_bg_color(end,lv_color_hex(0x8A2D35),LV_STATE_PRESSED);lv_obj_set_style_bg_grad_color(end,lv_color_hex(0x9E343B),0);lv_obj_set_style_bg_grad_dir(end,LV_GRAD_DIR_VER,0);auto*l=lv_obj_get_child(end,0);lv_obj_set_style_text_font(l,&font_puhui_30_4,0);lv_obj_set_style_text_color(l,lv_color_white(),0);
 }
}
void CloseIncoming(){
 CloseCallRoute();
 s_call_record_stop=true;
 if(s_incoming_timer){lv_timer_delete(s_incoming_timer);s_incoming_timer=nullptr;}
 if(s_call_float)lv_anim_delete(s_call_float,nullptr);
 if(s_incoming_card)lv_obj_delete(s_incoming_card);
 s_incoming_card=s_incoming_name=s_incoming_number=s_incoming_hint=nullptr;s_call_float=s_call_grid=s_call_footer=s_call_clock=s_call_end=nullptr;for(auto &p:s_call_tiles)p=nullptr;
 s_incoming_last_number.clear();s_incoming_dragging=false;s_incoming_action_busy=false;s_call_feature_busy=false;s_incoming_accept_requested=false;
 s_global_call_state=METALIO_CALL_IDLE;s_call_clock_started=false;s_overlay_contacts.clear();s_call_dtmf.clear();++s_global_call_generation;
 // Restore optional per-call controls; never touch networking or I2S/DMA.
 s_default_driver.Feature(CallScreen::JobKind::kMute,0,s_global_call_generation);
 s_default_driver.Feature(CallScreen::JobKind::kSpeaker,1,s_global_call_generation);
 if(!s_screen_active)s_default_driver.Session(false,s_call_epoch);
 IdlePower_NotifyActivity();
}
void UpdateIncoming(int state){
 if(s_call_powering_off)return;
 if(state==METALIO_CALL_IDLE){if(s_incoming_card)CloseIncoming();if(s_screen_active&&s_contact_editor)lv_obj_remove_flag(s_contact_editor,LV_OBJ_FLAG_HIDDEN);return;}
 bool incoming=state==METALIO_CALL_INCOMING;
 if(!s_incoming_card){
  if(StandbyScreen::IsVisible())StandbyScreen::ReturnHome();
  if(auto *display=lv_display_get_default())lv_display_enable_invalidation(display,true);
  SystemChrome_DismissOverlays();IdlePower_NotifyActivity();if(!s_call_record_running)CallRecordStatus("");s_call_record_last_status.clear();++s_global_call_generation;s_call_muted=false;s_call_speaker=true;s_call_volume=-1;s_call_feature_busy=false;s_call_clock_started=false;s_global_call_state=state;
  if(s_screen_active&&s_contact_editor)lv_obj_add_flag(s_contact_editor,LV_OBJ_FLAG_HIDDEN);
  s_incoming_card=lv_obj_create(lv_layer_sys());screen_strip_obj_chrome(s_incoming_card);lv_obj_set_pos(s_incoming_card,0,0);lv_obj_set_size(s_incoming_card,720,720);lv_obj_set_style_bg_opa(s_incoming_card,255,0);lv_obj_remove_flag(s_incoming_card,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(s_incoming_card,LV_OBJ_FLAG_CLICKABLE);screen_swipe_back_ignore(s_incoming_card,true);lv_obj_add_event_cb(s_incoming_card,IncomingGesture,LV_EVENT_ALL,nullptr);
  RenderGlobalCall(incoming);CallRemoveEdges(s_incoming_card);s_call_poll_tick=lv_tick_get();
  s_incoming_timer=lv_timer_create([](lv_timer_t*){int actual=metalio_nt26_get_call_state();UpdateIncoming(actual);},500,nullptr);
 }else if(incoming!=(s_global_call_state==METALIO_CALL_INCOMING)){s_global_call_state=state;RenderGlobalCall(incoming);CallRemoveEdges(s_incoming_card);}
 s_global_call_state=state;
 int audio_route=BluetoothScreen::PhoneAudioRoute();if(audio_route)s_call_speaker=audio_route==1;
 RefreshCallRoute();
 if(!incoming&&StandbyScreen::IsVisible())lv_obj_add_flag(s_incoming_card,LV_OBJ_FLAG_HIDDEN);
 else{lv_obj_remove_flag(s_incoming_card,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(s_incoming_card);}
 RefreshIncomingIdentity();
 if((state==METALIO_CALL_ACTIVE||state==METALIO_CALL_HELD)&&!s_call_clock_started){s_call_clock_started=true;s_call_connected_tick=lv_tick_get();s_incoming_action_busy=false;}
 if(s_call_clock){char text[64];if(s_call_clock_started){uint32_t sec=lv_tick_elaps(s_call_connected_tick)/1000;snprintf(text,sizeof(text),state==METALIO_CALL_HELD?"保持  %02lu:%02lu":"%02lu:%02lu",(unsigned long)(sec/60),(unsigned long)(sec%60));}else snprintf(text,sizeof(text),"正在拨号…");lv_label_set_text(s_call_clock,text);}
 if(!incoming){
  if(s_call_tiles[0])lv_obj_set_style_bg_color(s_call_tiles[0],lv_color_hex(s_call_speaker?0x485744:0x37393C),0);
  if(s_call_tiles[2])lv_obj_set_style_bg_color(s_call_tiles[2],lv_color_hex(s_call_muted?0x64533C:0x37393C),0);
  if(s_call_tiles[1])lv_obj_invalidate(s_call_tiles[1]);
  auto status=CallRecordStatus();if(status!=s_call_record_last_status&&s_call_footer&&!s_incoming_action_busy){s_call_record_last_status=status;if(!status.empty())lv_label_set_text(s_call_footer,status.c_str());}
 }
 if(lv_tick_elaps(s_call_poll_tick)>=1500){s_call_poll_tick=lv_tick_get();s_default_driver.Poll(s_call_epoch);}
}
void IncomingCommandResult(const CallScreen::CallResult &result){
 if(result.kind==CallScreen::JobKind::kMute||result.kind==CallScreen::JobKind::kSpeaker||result.kind==CallScreen::JobKind::kDtmf||result.kind==CallScreen::JobKind::kVolume||result.kind==CallScreen::JobKind::kReadVolume){
  if(!s_incoming_card||result.epoch!=s_global_call_generation)return;s_call_feature_busy=false;
  if(result.kind==CallScreen::JobKind::kVolume||result.kind==CallScreen::JobKind::kReadVolume){
   s_call_volume=result.status>=0?result.status:-1;
   if(s_call_footer){char text[96];
    if(result.status>=0)snprintf(text,sizeof(text),"通话音量  %d%%",s_call_volume);
    else snprintf(text,sizeof(text),"模块未确认通话音量，请稍后重试");
    lv_label_set_text(s_call_footer,text);
   }
   RefreshCallRoute();
   if(result.status<0&&result.kind==CallScreen::JobKind::kVolume)
    CallFeature(CallScreen::JobKind::kReadVolume,0);
   return;
  }
  if(result.status>=0){if(result.kind==CallScreen::JobKind::kMute)s_call_muted=result.value!=0;else if(result.kind==CallScreen::JobKind::kSpeaker)s_call_speaker=result.value!=0;else{s_call_dtmf+=(char)result.value;if(s_call_dtmf.size()>24)s_call_dtmf.erase(0,1);}
   if(s_call_footer)lv_label_set_text(s_call_footer,result.kind==CallScreen::JobKind::kDtmf?s_call_dtmf.c_str():result.kind==CallScreen::JobKind::kMute?(s_call_muted?"麦克风已静音":"麦克风已恢复"):(s_call_speaker?"已切换到扬声器":"已切换到蓝牙耳机"));
  }else if(s_call_footer){
   const char*message=result.kind==CallScreen::JobKind::kMute?
      (result.status==-EOPNOTSUPP?"当前通话模组未提供麦克风静音接口":"麦克风静音未确认，请重试"):
      result.kind==CallScreen::JobKind::kSpeaker?
      (BluetoothScreen::PhoneAudioRoute()==1?"耳机链路未确认，已回退扬声器":"通话音频切换未确认，请重试"):"模块未确认此操作，状态未改变";
   lv_label_set_text(s_call_footer,message);
  }RefreshCallRoute();return;
 }
 if(!s_incoming_card||!s_incoming_action_busy||result.epoch!=s_incoming_job_epoch)return;
 if(result.kind!=CallScreen::JobKind::kAnswer&&result.kind!=CallScreen::JobKind::kHangup)return;
 int state=metalio_nt26_get_call_state();if((result.kind==CallScreen::JobKind::kAnswer&&state!=METALIO_CALL_INCOMING)||(result.kind==CallScreen::JobKind::kHangup&&state==METALIO_CALL_IDLE)){UpdateIncoming(state);return;}
 s_incoming_action_busy=false;s_incoming_accept_requested=false;if(s_incoming_hint)lv_label_set_text(s_incoming_hint,"操作未完成，请重试");ReturnIncomingFloat();
}
