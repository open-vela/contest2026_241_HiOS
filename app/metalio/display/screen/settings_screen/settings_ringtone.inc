enum class ToneKind{Call,Sms,Alarm};
ToneKind s_tone_kind=ToneKind::Call;
lv_obj_t *s_ringtone_tab=nullptr,*s_ringtone_hint=nullptr;
uint32_t s_ringtone_generation=0;
bool s_ringtone_preview=false;
std::atomic<unsigned> s_tone_request{0};
std::mutex s_tone_save_mutex;
std::vector<PhoneService::Tone> s_ringtone_files;
void ShowRingtonePicker();
// The timer list behind lv_async_call is shared with the LVGL render task.
// Workers must take the same display lock even when only scheduling work.
lv_result_t TonePostUi(lv_async_cb_t callback,void *data){lv_lock();auto result=lv_async_call(callback,data);lv_unlock();return result;}
void StopRingtonePreview(){++s_tone_request;if(s_ringtone_preview){LocalMusicPlayer::Instance().StopPreviewPlayback();s_ringtone_preview=false;}}
void BuildRingtoneTab(lv_obj_t *tab){
 s_ringtone_tab=tab;lv_obj_set_style_pad_all(tab,24,0);lv_obj_remove_flag(tab,LV_OBJ_FLAG_SCROLLABLE);
 for(int kind=0;kind<3;++kind){auto*b=lv_button_create(tab);lv_obj_set_size(b,LV_PCT(100),100);lv_obj_set_pos(b,0,8+kind*112);screen_swipe_back_ignore(b,true);
 lv_obj_set_style_bg_color(b,lv_color_hex(kColorCard),0);lv_obj_set_style_radius(b,18,0);
 lv_obj_add_event_cb(b,[](lv_event_t*e){s_tone_kind=static_cast<ToneKind>((intptr_t)lv_event_get_user_data(e));ShowRingtonePicker();},LV_EVENT_CLICKED,(void*)(intptr_t)kind);
 auto*l=lv_label_create(b);lv_label_set_text(l,kind==0?"来电铃声":kind==1?"短信铃声":"闹钟铃声");lv_obj_set_style_text_font(l,&font_puhui_30_4,0);lv_obj_center(l);}
 auto*hint=lv_label_create(tab);lv_label_set_text(hint,"选择 /Music 中的歌曲或铃声，点击即可试听");lv_obj_set_width(hint,LV_PCT(100));lv_obj_set_pos(hint,0,370);lv_obj_set_style_text_font(hint,&font_puhui_20_4,0);
}
struct ToneJob{uint32_t generation;std::string path;bool selected=false;std::vector<PhoneService::Tone> files;unsigned request=0;ToneKind kind=ToneKind::Call;};
void ShowRingtonePicker(){
 if(!s_ringtone_tab)return;lv_obj_clean(s_ringtone_tab);s_ringtone_hint=nullptr;lv_obj_add_flag(s_ringtone_tab,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_flex_flow(s_ringtone_tab,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(s_ringtone_tab,12,0);
 auto*l=lv_label_create(s_ringtone_tab);lv_label_set_text(l,"正在读取 /Music…");lv_obj_set_style_text_font(l,&font_puhui_20_4,0);
 auto*job=new ToneJob{++s_ringtone_generation,{}};pthread_t t;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,24576);
 int rc=pthread_create(&t,&attr,[](void*arg)->void*{
  auto*j=(ToneJob*)arg;j->files=PhoneService::Ringtones();
  if(TonePostUi([](void*arg){auto*j=(ToneJob*)arg;if(!s_ringtone_tab||j->generation!=s_ringtone_generation){delete j;return;}
   s_ringtone_files=std::move(j->files);delete j;lv_obj_clean(s_ringtone_tab);
   s_ringtone_hint=lv_label_create(s_ringtone_tab);lv_label_set_text(s_ringtone_hint,"点击设置并试听 12 秒");lv_obj_set_style_text_font(s_ringtone_hint,&font_puhui_20_4,0);lv_obj_set_width(s_ringtone_hint,LV_PCT(100));
   if(s_ringtone_files.empty())lv_label_set_text(s_ringtone_hint,"未找到音频，请检查 SD 卡 /Music");
   auto*back=lv_button_create(s_ringtone_tab);lv_obj_set_size(back,LV_PCT(100),58);auto*bl=lv_label_create(back);lv_label_set_text(bl,"返回铃声分类");lv_obj_set_style_text_font(bl,&font_puhui_20_4,0);lv_obj_center(bl);lv_obj_add_event_cb(back,[](lv_event_t*){StopRingtonePreview();++s_ringtone_generation;lv_obj_clean(s_ringtone_tab);lv_obj_set_layout(s_ringtone_tab,LV_LAYOUT_NONE);BuildRingtoneTab(s_ringtone_tab);},LV_EVENT_CLICKED,nullptr);
   std::string selected=s_tone_kind==ToneKind::Call?PhoneService::SelectedRingtone():NotificationSound::Selected(s_tone_kind==ToneKind::Sms?NotificationSound::Kind::Sms:NotificationSound::Kind::Alarm);
   for(size_t i=0;i<s_ringtone_files.size();i++){
    auto*b=lv_button_create(s_ringtone_tab);lv_obj_set_size(b,LV_PCT(100),90);lv_obj_set_style_radius(b,14,0);lv_obj_set_style_bg_color(b,lv_color_hex(s_ringtone_files[i].path==selected?kColorAccent:kColorCard),0);screen_swipe_back_ignore(b,true);
    auto*l=lv_label_create(b);lv_label_set_text(l,((s_ringtone_files[i].path==selected?"已选  ":"")+s_ringtone_files[i].title).c_str());lv_obj_set_style_text_font(l,&font_puhui_20_4,0);lv_obj_set_width(l,LV_PCT(95));lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_obj_center(l);lv_obj_remove_flag(l,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b,[](lv_event_t*e){size_t i=(size_t)lv_event_get_user_data(e);if(i>=s_ringtone_files.size())return;
     auto*job=new ToneJob{s_ringtone_generation,s_ringtone_files[i].path};job->request=++s_tone_request;job->kind=s_tone_kind;
     pthread_t t;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,24576);
     int rc=pthread_create(&t,&a,[](void*arg)->void*{auto*j=(ToneJob*)arg;{std::lock_guard<std::mutex>lock(s_tone_save_mutex);if(j->request==s_tone_request)j->selected=j->kind==ToneKind::Call?PhoneService::SelectRingtone(j->path):NotificationSound::Select(j->kind==ToneKind::Sms?NotificationSound::Kind::Sms:NotificationSound::Kind::Alarm,j->path);}
      if(TonePostUi([](void*arg){auto*j=(ToneJob*)arg;if(s_ringtone_tab&&j->generation==s_ringtone_generation&&j->request==s_tone_request){
       if(j->selected){s_ringtone_preview=true;LocalMusicPlayer::Instance().PreviewFile(j->path);ShowRingtonePicker();}
       else if(s_ringtone_hint)lv_label_set_text(s_ringtone_hint,"未能保存，请检查 SD 卡音频文件");
      }delete j;},j)!=LV_RESULT_OK)delete j;return nullptr;},job);
     pthread_attr_destroy(&a);if(!rc)pthread_detach(t);else{delete job;if(s_ringtone_hint)lv_label_set_text(s_ringtone_hint,"系统忙，请重试");}
    },LV_EVENT_CLICKED,(void*)i);
   }
   auto*stop=lv_button_create(s_ringtone_tab);lv_obj_set_size(stop,LV_PCT(100),60);auto*l=lv_label_create(stop);lv_label_set_text(l,"停止试听");lv_obj_set_style_text_font(l,&font_puhui_20_4,0);lv_obj_center(l);lv_obj_add_event_cb(stop,[](lv_event_t*){StopRingtonePreview();},LV_EVENT_CLICKED,nullptr);
  },j)!=LV_RESULT_OK)delete j;return nullptr;
 },job);pthread_attr_destroy(&attr);if(!rc)pthread_detach(t);else{delete job;lv_label_set_text(l,"无法读取音频列表，请重试");}
}
