// Private cellular analog routing. No PCM pin assumptions or mode resets.
// The user's board wiring carries NT26 receive/transmit audio to the BT codec.
bool PhoneRequestValid(unsigned generation,unsigned request){auto&r=PhoneRouting();return s_phone_audio&&CellularAudioActive()&&!r.ending&&r.generation==generation&&r.request==request;}
int PhoneSpeakerFallback(unsigned generation,unsigned request){
 if(!PhoneRequestValid(generation,request))return -ECANCELED;
 int ret=metalio_nt26_set_call_output(true);PhoneRouting().route=ret<0?0:1;return ret;
}
int ApplyPhoneOutput(bool headset,unsigned generation,unsigned request){
 auto&r=PhoneRouting();if(!PhoneRequestValid(generation,request))return -ECANCELED;
 if(s_mode_busy||s_request_busy||s_profile_busy)return -EBUSY;
 if(!headset){
  int ret=PhoneSpeakerFallback(generation,request);if(ret<0)return ret;
  if(s_active_mode==BtMode::kMode2&&s_headset_linked&&s_sco_up&&
      !ProfileCommand(false,true,generation,request))return -EIO;
  printf("PHONE_AUDIO_ROUTE speaker generation=%u\n",generation);return 0;
 }
 if(!BluetoothScreen::IsHeadsetConnected()){PhoneSpeakerFallback(generation,request);return -ENOTCONN;}
 const unsigned link=s_link_epoch;
 if(!ProfileCommand(true,true,generation,request)){
  int ret=PhoneRequestValid(generation,request)?-EIO:-ECANCELED;
  if(ret!=-ECANCELED)PhoneSpeakerFallback(generation,request);return ret;
 }
 if(!PhoneRequestValid(generation,request)||s_link_epoch!=link||!s_headset_linked||!s_sco_up){
  PhoneSpeakerFallback(generation,request);return -ECANCELED;
 }
 int ret=metalio_nt26_set_call_output(false);
 if(ret<0){PhoneSpeakerFallback(generation,request);return ret;}
 r.route=2;printf("PHONE_AUDIO_ROUTE headset_sco generation=%u link=%u\n",generation,link);return 0;
}
void*PhoneRouteWorker(void*){
 auto&r=PhoneRouting();unsigned applied_generation=0,applied_request=0,applied_link=0;bool tried=false;
 for(;;){
  if(!s_phone_audio){
   unsigned generation,request;BtMode before;bool sco;
   {std::lock_guard<std::mutex>lock(s_phone_audio_mutex);generation=r.generation;request=r.request;before=r.before;sco=r.before_sco;}
   {std::lock_guard<std::mutex>lock(r.operation);
    {std::lock_guard<std::mutex>focus(s_phone_audio_mutex);if(!s_phone_audio&&r.generation==generation)metalio_bt_set_scan_mode(s_active_mode==BtMode::kMode2);}
    if(!s_phone_audio&&r.generation==generation&&metalio_nt26_get_call_state()==METALIO_CALL_IDLE){
     // Serialize the final source restore with the start of the next call.
     {std::lock_guard<std::mutex>focus(s_phone_audio_mutex);
      if(!s_phone_audio&&r.generation==generation&&metalio_nt26_get_call_state()==METALIO_CALL_IDLE)
       (void)metalio_nt26_set_call_audio_path(false);
     }
     if(before==BtMode::kMode2&&s_active_mode==BtMode::kMode2&&s_headset_linked)
      (void)ProfileCommand(sco,false,generation,request);
     else if(!s_phone_audio&&r.generation==generation)
      Application::GetInstance().ArmAssistantWakeWord();
    }
   }
   {std::lock_guard<std::mutex>lock(s_phone_audio_mutex);if(!s_phone_audio){r.route=0;r.worker=false;return nullptr;}}
   tried=false;continue;
  }
  if(CellularAudioActive()&&!r.ending){
   metalio_bt_set_scan_mode(true);
   unsigned generation=r.generation,request=r.request,link=s_link_epoch;
   bool headset=r.preferred!=0&&BluetoothScreen::IsHeadsetConnected();
   bool lost=r.route==2&&(!s_headset_linked||!s_sco_up);
   bool changed=!tried||generation!=applied_generation||request!=applied_request||link!=applied_link||lost;
   if(changed&&!s_mode_busy&&!s_request_busy&&!s_profile_busy&&r.operation.try_lock()){
    // A manual request may have arrived while acquiring this mutex.
    if(PhoneRequestValid(generation,request)){
     int ret=ApplyPhoneOutput(headset,generation,request);
     if(ret!=-EBUSY){tried=true;applied_generation=generation;applied_request=request;applied_link=link;}
    }
    r.operation.unlock();
   }
  }
  usleep(200000);
 }
}
void StartPhoneRouteWorker(){
 auto&r=PhoneRouting();if(r.worker.exchange(true))return;
 pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,24576);pthread_attr_setdetachstate(&a,PTHREAD_CREATE_DETACHED);
 sched_param priority{};priority.sched_priority=90;pthread_attr_setschedparam(&a,&priority);pthread_attr_setinheritsched(&a,PTHREAD_EXPLICIT_SCHED);
 pthread_t t;int ret=pthread_create(&t,&a,PhoneRouteWorker,nullptr);pthread_attr_destroy(&a);
 if(ret){r.worker=false;printf("PHONE_AUDIO_ROUTE worker_failed=%d\n",ret);}
}
