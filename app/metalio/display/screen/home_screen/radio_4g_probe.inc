/* Engineering image only: real radio downloads; no calls/SMS or volume changes. */
extern "C" int metalio_app_active_data_path(void);
extern "C" int metalio_app_select_data_path(int);
extern "C" int metalio_nt26_get_call_state(void);
static void StabilityArm(PagerState *)
{
    static bool armed=false;if(armed)return;armed=true;
    lv_timer_create([](lv_timer_t*t){
        lv_timer_delete(t);
        pthread_t th;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,32768);pthread_attr_setdetachstate(&a,PTHREAD_CREATE_DETACHED);
        hi_task_create_legacy(&th,&a,[](void*)->void*{
            if(metalio_nt26_get_call_state()!=0){printf("RADIO4G_PROBE_COMPLETE skipped_call=1\n");return nullptr;}
            const int previous=metalio_app_active_data_path();
            int rc=previous==1?0:metalio_app_select_data_path(1);
            printf("RADIO4G_BEGIN previous=%d rc=%d\n",previous,rc);fflush(stdout);
            unsigned failures=0;
            auto &radio=RadioPlayer::Instance();const int original=radio.station_index();
            const int stations[]={0,8};
            for(int index:stations){
                if(rc<0)break;
                radio.SelectStation(index);
                printf("RADIO4G_STATION index=%d name=%s\n",index,radio.station().name);fflush(stdout);
                unsigned played=0;uint32_t first_submitted=0,first_produced=0;
                for(unsigned sec=0;sec<180;++sec){
                    usleep(1000000);auto stats=radio.GetProbeStats();
                    printf("RADIO4G_STATS sec=%u state=%d produced=%lu submitted=%lu buffered=%u\n",sec,(int)radio.status(),(unsigned long)stats.produced,(unsigned long)stats.submitted,(unsigned)stats.buffered);fflush(stdout);
                    if(radio.status()==RadioStatus::Playing) {
                        if(!played){first_submitted=stats.submitted;first_produced=stats.produced;}
                        if(++played==60)break;
                    }
                    if(radio.status()==RadioStatus::Failed)break;
                }
                const auto end=radio.GetProbeStats();
                const uint32_t actual=end.submitted-first_submitted,produced=end.produced-first_produced;
                bool ok=played>=60 && actual>=16000*55 && produced>=16000*50;
                printf("RADIO4G_RESULT index=%d success=%d played=%u actual=%lu produced=%lu\n",index,ok,played,(unsigned long)actual,(unsigned long)produced);fflush(stdout);
                if(!ok)++failures;
                radio.Stop();
                for(int n=0;n<100&&radio.is_session_running();++n)usleep(100000);
            }
            // Restore the original selection while retaining a stopped player.
            radio.SelectStation(original);radio.Stop();
            if(previous==0)metalio_app_select_data_path(0);
            printf("RADIO4G_PROBE_COMPLETE failures=%u setup=%d\n",failures,rc);fflush(stdout);
            return nullptr;
        },nullptr);pthread_attr_destroy(&a);
    },10000,nullptr);
}
