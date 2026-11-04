// The user's light Smartisan-style phone, with persistent contacts and history.
lv_obj_t *s_dial_body=nullptr,*s_phone_list=nullptr,*s_contact_editor=nullptr;
lv_obj_t *s_contact_name=nullptr,*s_contact_number=nullptr,*s_contact_keyboard=nullptr,*s_contact_hint=nullptr;
lv_obj_t *s_contact_rows=nullptr,*s_contact_search=nullptr;
lv_obj_t *s_phone_tabs[4]{};
int s_phone_tab=0;
size_t s_history_page=0;
bool s_history_delete_busy=false;
bool s_edit_favorite=false,s_pick_favorite=false;
std::vector<PhoneService::Contact> s_contacts;
std::vector<PhoneService::Call> s_history;
std::string s_edit_contact_id,s_contact_query;
lv_obj_t *PhoneButton(lv_obj_t *parent,const char *text,int x,int y,int w,int h,lv_event_cb_t cb,void *arg=nullptr){
 auto *b=lv_button_create(parent);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
 lv_obj_set_style_bg_color(b,lv_color_hex(0xF5F5F4),0);lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
 lv_obj_set_style_bg_color(b,lv_color_hex(0xDADDDC),LV_STATE_PRESSED);
 lv_obj_set_style_radius(b,0,0);lv_obj_set_style_border_color(b,lv_color_hex(0xD3D5D5),0);
 lv_obj_set_style_border_width(b,1,0);lv_obj_set_style_shadow_width(b,0,0);lv_obj_set_style_pad_all(b,0,0);
 if(cb)lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,arg);screen_swipe_back_ignore(b,true);
 auto *l=lv_label_create(b);lv_label_set_text(l,text);lv_obj_set_style_text_font(l,&font_puhui_20_4,0);
 lv_obj_set_style_text_color(l,lv_color_hex(0x41474B),0);lv_obj_remove_flag(l,LV_OBJ_FLAG_CLICKABLE);lv_obj_center(l);return b;
}
void ShowPhoneTab(int tab);
void RenderContactRows();
void CloseContactEditor(){if(s_contact_editor)lv_obj_delete(s_contact_editor);
 s_contact_editor=s_contact_name=s_contact_number=s_contact_keyboard=s_contact_hint=nullptr;}
lv_obj_t *EditorRoot(const char *text){
 auto *o=lv_obj_create(lv_layer_top());s_contact_editor=o;screen_strip_obj_chrome(o);
 lv_obj_set_size(o,720,720);lv_obj_set_pos(o,0,0);lv_obj_set_style_bg_color(o,lv_color_hex(0xF3F4F3),0);
 lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);screen_swipe_back_ignore(o,true);
 auto *l=lv_label_create(o);lv_label_set_text(l,text);lv_obj_set_style_text_font(l,&font_puhui_30_4,0);
 lv_obj_set_style_text_color(l,lv_color_hex(0x343A3D),0);lv_obj_set_pos(l,24,16);return o;
}
void EditContact(const PhoneService::Contact &c){
 if(s_contact_editor)return;s_edit_contact_id=c.id;s_edit_favorite=c.favorite;
 auto *o=EditorRoot(c.id.empty()?"新建联系人":"编辑联系人");
 CreateContactAvatar(o,28,80);
 s_contact_name=lv_textarea_create(o);HiInputMethod_Bind(s_contact_name,true);lv_obj_set_pos(s_contact_name,104,65);lv_obj_set_size(s_contact_name,592,65);
 lv_textarea_set_one_line(s_contact_name,true);lv_textarea_set_max_length(s_contact_name,32);
 lv_textarea_set_placeholder_text(s_contact_name,"姓名");lv_textarea_set_text(s_contact_name,c.name.c_str());
 lv_obj_set_style_text_font(s_contact_name,&font_puhui_20_4,0);
 s_contact_number=lv_textarea_create(o);HiInputMethod_Bind(s_contact_number,false);lv_obj_set_pos(s_contact_number,104,143);lv_obj_set_size(s_contact_number,592,65);
 lv_textarea_set_one_line(s_contact_number,true);lv_textarea_set_max_length(s_contact_number,24);
 lv_textarea_set_accepted_chars(s_contact_number,"+0123456789");lv_textarea_set_placeholder_text(s_contact_number,"电话号码");
 lv_textarea_set_text(s_contact_number,c.number.c_str());lv_obj_set_style_text_font(s_contact_number,&font_puhui_20_4,0);
 PhoneButton(o,"取消",24,228,140,56,[](lv_event_t*){CloseContactEditor();});
 if(!c.id.empty())PhoneButton(o,"删除",190,228,140,56,[](lv_event_t*){
   PhoneService::DeleteContact(s_edit_contact_id);CloseContactEditor();ShowPhoneTab(1);
 });
 PhoneButton(o,c.favorite?"取消收藏":"收藏",356,228,140,56,[](lv_event_t *e){
   s_edit_favorite=!s_edit_favorite;lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(e),0),s_edit_favorite?"取消收藏":"收藏");
 });
 PhoneButton(o,"保存",522,228,174,56,[](lv_event_t*){
   if(s_contact_keyboard&&!HiInputMethod_Flush(s_contact_keyboard))return;
   PhoneService::Contact c{s_edit_contact_id,lv_textarea_get_text(s_contact_name),lv_textarea_get_text(s_contact_number),s_edit_favorite};
   if(!PhoneService::SaveContact(c)){lv_label_set_text(s_contact_hint,"请填写姓名和有效号码，并检查 SD 卡");return;}
   s_contact_query.clear();CloseContactEditor();ShowPhoneTab(1);
 });
 s_contact_hint=lv_label_create(o);lv_obj_set_pos(s_contact_hint,24,292);lv_obj_set_width(s_contact_hint,672);
 lv_label_set_text(s_contact_hint,"");lv_obj_set_style_text_font(s_contact_hint,&font_puhui_20_4,0);lv_obj_set_style_text_color(s_contact_hint,lv_color_hex(0xB43F3F),0);
 s_contact_keyboard=HiInputMethod_Create(o);if(s_contact_keyboard){
   lv_obj_set_height(s_contact_keyboard,400);HiInputMethod_SetTextArea(s_contact_keyboard,s_contact_name,true);
   lv_obj_t *fields[]={s_contact_name,s_contact_number};
   for(auto *f:fields)lv_obj_add_event_cb(f,[](lv_event_t *e){auto *field=lv_event_get_target_obj(e);
       HiInputMethod_SetTextArea(s_contact_keyboard,field,field==s_contact_name);
   },LV_EVENT_CLICKED,nullptr);
 }
}
void OpenContactSearch(){
 if(s_contact_editor)return;auto *o=EditorRoot("搜索联系人");
 s_contact_name=lv_textarea_create(o);HiInputMethod_Bind(s_contact_name,true);lv_obj_set_pos(s_contact_name,24,82);lv_obj_set_size(s_contact_name,672,72);
 lv_textarea_set_one_line(s_contact_name,true);lv_textarea_set_max_length(s_contact_name,32);
 lv_textarea_set_text(s_contact_name,s_contact_query.c_str());lv_textarea_set_placeholder_text(s_contact_name,"姓名、拼音首字母或电话号码");
 lv_obj_set_style_text_font(s_contact_name,&font_puhui_20_4,0);
 lv_obj_add_event_cb(s_contact_name,[](lv_event_t*){s_contact_query=lv_textarea_get_text(s_contact_name);
   if(s_contact_search)lv_textarea_set_text(s_contact_search,s_contact_query.c_str());RenderContactRows();},LV_EVENT_VALUE_CHANGED,nullptr);
 PhoneButton(o,"清空",24,196,210,64,[](lv_event_t*){lv_textarea_set_text(s_contact_name,"");});
 PhoneButton(o,"完成",486,196,210,64,[](lv_event_t*){
   if(s_contact_keyboard)HiInputMethod_Flush(s_contact_keyboard);CloseContactEditor();
 });
 s_contact_keyboard=HiInputMethod_Create(o);if(s_contact_keyboard){lv_obj_set_height(s_contact_keyboard,400);HiInputMethod_SetTextArea(s_contact_keyboard,s_contact_name,true);}
}
void UsePhoneNumber(const std::string &number){snprintf(s_number,sizeof(s_number),"%s",number.c_str());ShowPhoneTab(0);RefreshNumberDisplay();}
void RenderContactRows(){
 if(!s_contact_rows)return;lv_obj_clean(s_contact_rows);int y=0;char group=0;size_t visible=0;
 for(size_t i=0;i<s_contacts.size();i++){const auto &c=s_contacts[i];if(s_phone_tab==3&&(s_pick_favorite?c.favorite:!c.favorite))continue;if(!PhoneService::Matches(c,s_contact_query))continue;
   char initial=PhoneService::Initials(c.name)[0];
   if(initial!=group){group=initial;char label[2]={group,0};auto *l=lv_label_create(s_contact_rows);lv_label_set_text(l,label);
     lv_obj_set_style_text_font(l,&font_puhui_20_4,0);lv_obj_set_style_text_color(l,lv_color_hex(0x7A8386),0);lv_obj_set_pos(l,18,y+5);y+=34;}
   auto *row=PhoneButton(s_contact_rows,(c.name+"\n"+c.number).c_str(),0,y,566,86,[](lv_event_t *e){
     size_t i=(size_t)lv_event_get_user_data(e);if(i<s_contacts.size()){if(s_phone_tab==3&&s_pick_favorite){auto c=s_contacts[i];c.favorite=true;if(PhoneService::SaveContact(c)){s_pick_favorite=false;ShowPhoneTab(3);}}else UsePhoneNumber(s_contacts[i].number);}
   },(void*)i);
   auto *l=lv_obj_get_child(row,0);lv_obj_set_width(l,470);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_obj_align(l,LV_ALIGN_LEFT_MID,74,0);
   CreateContactAvatar(row,12,18);
   PhoneButton(s_contact_rows,s_phone_tab==3?(s_pick_favorite?"收藏":"取消收藏"):"编辑",566,y,110,86,[](lv_event_t *e){size_t i=(size_t)lv_event_get_user_data(e);if(i<s_contacts.size()){if(s_phone_tab==3){auto c=s_contacts[i];c.favorite=s_pick_favorite;if(PhoneService::SaveContact(c)){s_pick_favorite=false;ShowPhoneTab(3);}}else EditContact(s_contacts[i]);}},(void*)i);
   y+=87;++visible;
 }
 if(!visible){auto *l=lv_label_create(s_contact_rows);lv_label_set_text(l,s_contact_query.empty()?(s_phone_tab==3?(s_pick_favorite?"通讯录中没有可添加的联系人":"暂无收藏联系人"):"暂无联系人，点击上方新建"):"没有匹配的联系人");
   lv_obj_set_pos(l,18,24);lv_obj_set_style_text_font(l,&font_puhui_20_4,0);lv_obj_set_style_text_color(l,lv_color_hex(0x7A8386),0);}
}
void ShowPhoneTab(int tab){
 if(s_call_state!=CallState::kIdle)tab=0;s_phone_tab=tab;
 for(int i=0;i<4;i++)if(s_phone_tabs[i])lv_obj_set_style_bg_color(s_phone_tabs[i],lv_color_hex(tab==i?0x242728:0x474A4B),0);
 if(tab==0){lv_obj_remove_flag(s_dial_body,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(s_phone_list,LV_OBJ_FLAG_HIDDEN);return;}
 lv_obj_add_flag(s_dial_body,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(s_phone_list,LV_OBJ_FLAG_HIDDEN);
 lv_obj_clean(s_phone_list);s_contact_rows=s_contact_search=nullptr;lv_obj_scroll_to_y(s_phone_list,0,LV_ANIM_OFF);
 if(tab==1||tab==3){s_contacts=PhoneService::Contacts();lv_obj_remove_flag(s_phone_list,LV_OBJ_FLAG_SCROLLABLE);
   if(tab==3){
     PhoneButton(s_phone_list,"从通讯录添加",12,10,696,56,[](lv_event_t*){s_pick_favorite=!s_pick_favorite;RenderContactRows();});
   }else{
     s_pick_favorite=false;
     PhoneButton(s_phone_list,"新建联系人",12,10,270,56,[](lv_event_t*){EditContact({"","",""});});
     PhoneButton(s_phone_list,"保存拨号号码",298,10,410,56,[](lv_event_t*){for(auto c:PhoneService::Contacts())if(c.number==s_number){EditContact(c);return;}EditContact({"","",s_number});});
   }
   s_contact_search=lv_textarea_create(s_phone_list);HiInputMethod_Bind(s_contact_search,true);lv_obj_set_pos(s_contact_search,12,80);lv_obj_set_size(s_contact_search,696,60);
   lv_textarea_set_one_line(s_contact_search,true);lv_textarea_set_text(s_contact_search,s_contact_query.c_str());
   lv_textarea_set_placeholder_text(s_contact_search,"搜索姓名、首字母或号码");lv_obj_set_style_text_font(s_contact_search,&font_puhui_20_4,0);
   lv_obj_add_event_cb(s_contact_search,[](lv_event_t*){OpenContactSearch();},LV_EVENT_CLICKED,nullptr);screen_swipe_back_ignore(s_contact_search,true);
   s_contact_rows=lv_obj_create(s_phone_list);screen_strip_obj_chrome(s_contact_rows);lv_obj_set_pos(s_contact_rows,12,152);lv_obj_set_size(s_contact_rows,696,408);
   lv_obj_set_scroll_dir(s_contact_rows,LV_DIR_VER);RenderContactRows();
 }else{s_history=PhoneService::History();auto contacts=PhoneService::Contacts();lv_obj_add_flag(s_phone_list,LV_OBJ_FLAG_SCROLLABLE);
   PhoneButton(s_phone_list,"清空通话记录",12,10,270,56,[](lv_event_t*){PhoneService::ClearHistory();s_history_page=0;ShowPhoneTab(2);});
   size_t pages=std::max<size_t>(1,(s_history.size()+49)/50);s_history_page=std::min(s_history_page,pages-1);
   PhoneButton(s_phone_list,"上一页",300,10,120,56,[](lv_event_t*){if(s_history_page)--s_history_page;ShowPhoneTab(2);});
   PhoneButton(s_phone_list,"下一页",432,10,120,56,[](lv_event_t*){++s_history_page;ShowPhoneTab(2);});
   char page[32];snprintf(page,sizeof(page),"%u / %u",(unsigned)(s_history_page+1),(unsigned)pages);
   PhoneButton(s_phone_list,page,564,10,144,56,nullptr);int y=82;
   for(size_t i=s_history_page*50;i<std::min(s_history.size(),(s_history_page+1)*50);i++,y+=148){const auto &c=s_history[i];char stamp[64]="时间未同步";
     if(c.when>1700000000){time_t ts=c.when;struct tm tm{};localtime_r(&ts,&tm);strftime(stamp,sizeof(stamp),"%Y-%m-%d %H:%M:%S",&tm);}
     std::string name=c.number.empty()?"未提供号码":c.number;for(const auto &p:contacts)if(!p.name.empty()&&!c.number.empty()&&CallMatchNumber(p.number)==CallMatchNumber(c.number)){name=p.name;break;}
     char duration[24];snprintf(duration,sizeof(duration),"%02d:%02d:%02d",c.seconds/3600,c.seconds/60%60,c.seconds%60);
     const auto region=PhoneService::NumberRegion(c.number);
     std::string identity=name==c.number?region:c.number+(region.empty()?"":" · ")+region;
     std::string label=name+"\n"+identity+"\n"+(c.kind=="missed"?"未接来电":c.kind=="incoming"?"已接来电":"已拨电话")+"  "+stamp+"\n时长 "+duration;
     auto *row=PhoneButton(s_phone_list,label.c_str(),12,y,580,140,[](lv_event_t *e){size_t i=(size_t)lv_event_get_user_data(e);if(i<s_history.size())UsePhoneNumber(s_history[i].number);},(void*)i);
     PhoneButton(s_phone_list,"删除",606,y+45,94,50,[](lv_event_t *e){
       size_t i=(size_t)lv_event_get_user_data(e);if(i>=s_history.size()||s_history_delete_busy)return;
       struct Job{uint64_t id;bool ok;};auto *job=new Job{s_history[i].id,false};s_history_delete_busy=true;
       pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,24576);
       int rc=pthread_create(&thread,&attr,[](void *data)->void*{auto *j=(Job*)data;j->ok=PhoneService::DeleteHistory(j->id);
         if(lv_async_call([](void *data){auto*j=(Job*)data;s_history_delete_busy=false;
           if(s_screen_active&&s_phone_list&&s_phone_tab==2)ShowPhoneTab(2);delete j;},j)!=LV_RESULT_OK){s_history_delete_busy=false;delete j;}return nullptr;},job);
       pthread_attr_destroy(&attr);if(!rc)pthread_detach(thread);else{s_history_delete_busy=false;delete job;}
     },(void*)i);
     CreateContactAvatar(row,12,45);
     auto *l=lv_obj_get_child(row,0);lv_obj_set_width(l,488);lv_obj_align(l,LV_ALIGN_LEFT_MID,78,0);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);if(c.kind=="missed")lv_obj_set_style_text_color(l,lv_color_hex(0xB44B48),0);
   }
   if(s_history.empty()){auto *l=lv_label_create(s_phone_list);lv_label_set_text(l,"暂无通话记录");lv_obj_set_pos(l,24,112);lv_obj_set_style_text_font(l,&font_puhui_20_4,0);lv_obj_set_style_text_color(l,lv_color_hex(0x7A8386),0);}
 }
}
void BuildPhoneTabs(lv_obj_t *screen){
 s_phone_list=lv_obj_create(screen);screen_strip_obj_chrome(s_phone_list);lv_obj_set_pos(s_phone_list,0,64);lv_obj_set_size(s_phone_list,720,572);
 lv_obj_set_style_bg_color(s_phone_list,lv_color_hex(0xF3F4F3),0);lv_obj_set_style_bg_opa(s_phone_list,LV_OPA_COVER,0);lv_obj_set_style_pad_all(s_phone_list,0,0);
 lv_obj_set_scroll_dir(s_phone_list,LV_DIR_VER);lv_obj_set_scrollbar_mode(s_phone_list,LV_SCROLLBAR_MODE_AUTO);
 const char *labels[]={"通话记录","联系人","收藏夹","拨号盘"};
 const char *icons[]={FONT_AWESOME_CLOCK,FONT_AWESOME_USER,FONT_AWESOME_STAR,FONT_AWESOME_PHONE};int ids[]={2,1,3,0};
 for(int i=0;i<4;i++){
   auto *b=PhoneButton(screen,labels[i],i*180,636,180,84,[](lv_event_t *e){ShowPhoneTab((int)(intptr_t)lv_event_get_user_data(e));},(void*)(intptr_t)ids[i]);s_phone_tabs[ids[i]]=b;
   lv_obj_set_style_border_color(b,lv_color_hex(0x242728),0);auto *label=lv_obj_get_child(b,0);lv_obj_set_style_text_color(label,lv_color_hex(0xECEDEC),0);lv_obj_align(label,LV_ALIGN_BOTTOM_MID,0,-5);
   auto *icon=lv_label_create(b);lv_label_set_text(icon,icons[i]);lv_obj_set_style_text_font(icon,&font_awesome_30_4,0);lv_obj_set_style_text_color(icon,lv_color_hex(0xDADDDD),0);lv_obj_align(icon,LV_ALIGN_TOP_MID,0,9);
 }
 ShowPhoneTab(0);
}
