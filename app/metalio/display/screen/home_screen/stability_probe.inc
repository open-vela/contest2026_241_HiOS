/* Diagnostic image only. Actual pointer input; no folder edits or calls. */
static lv_indev_t *st_pointer;
static lv_point_t st_point;
static bool st_down;
static unsigned st_failures;
static std::atomic<bool> st_capture_busy{false};
static uint16_t *st_pixels;
static char st_tag[24];
static void StSnap(const char *tag) {
    if(st_capture_busy.load())return;
    int fd=open("/dev/fb0",O_RDONLY);fb_planeinfo_s pi{};
    if(fd<0)return;
    if(ioctl(fd,FBIOGET_PLANEINFO,(unsigned long)&pi)<0||pi.bpp!=24){close(fd);return;}
    auto*p=(uint16_t*)malloc(240*240*2);if(!p){close(fd);return;}
    const auto*src=(const uint8_t*)pi.fbmem+pi.yoffset*pi.stride;
    for(int y=0;y<240;++y)for(int x=0;x<240;++x){const auto*c=src+y*3*pi.stride+x*9;p[y*240+x]=((c[2]>>3)<<11)|((c[1]>>2)<<5)|(c[0]>>3);}
    close(fd);st_pixels=p;snprintf(st_tag,sizeof(st_tag),"%s",tag);st_capture_busy=true;
    pthread_t worker;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,8192);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    int rc=hi_task_create_legacy(&worker,&attr,[](void*)->void*{
        for(int y=0;y<240;++y)for(int h=0;h<2;++h){
            char data[721],line[810];int n=0;unsigned sum=0;const auto*p=st_pixels+y*240+h*120;
            for(int i=0;i<120;){int count=1;while(i+count<120&&p[i+count]==p[i])++count;n+=snprintf(data+n,sizeof(data)-n,"%02x%04x",count,p[i]);sum=(sum+count*p[i])&65535;i+=count;}
            n=snprintf(line,sizeof(line),"UIRLE %s %d %d %04x %s\n",st_tag,y,h,sum,data);write(1,line,n);write(1,line,n);usleep(1000);
        }
        printf("UI_CAPTURE_DONE %s\n",st_tag);fflush(stdout);free(st_pixels);st_pixels=nullptr;st_capture_busy=false;return nullptr;
    },nullptr);pthread_attr_destroy(&attr);
    if(rc){free(p);st_pixels=nullptr;st_capture_busy=false;}
}
static void StCheck(bool ok,const char *name) {
    printf("STABILITY_CHECK %s %s\n",ok?"PASS":"FAIL",name);fflush(stdout);
    if(!ok)++st_failures;
}
static void StInput(int x,int y,bool down) {
    st_point={x,y};st_down=down;lv_indev_read(st_pointer);
}
static lv_obj_t *StLabel(lv_obj_t *o,const char *text) {
    if(lv_obj_check_type(o,&lv_label_class)&&!strcmp(lv_label_get_text(o),text))return o;
    for(unsigned i=0;i<lv_obj_get_child_count(o);++i){auto*r=StLabel(lv_obj_get_child(o,i),text);if(r)return r;}
    return nullptr;
}
static bool StTap(const char *text,bool down) {
    auto*o=StLabel(lv_screen_active(),text);if(!o)return false;
    lv_area_t a;lv_obj_get_coords(o,&a);StInput((a.x1+a.x2)/2,(a.y1+a.y2)/2,down);return true;
}
static bool StLaunch(const char *id) {
    const int i=FindAppIndexBySuffix(id);return i>=0&&LaunchHomeApp(&kApps[i]);
}
static void StabilityArm(PagerState *pager) {
    static bool armed=false;if(armed)return;armed=true;
    st_pointer=lv_indev_create();lv_indev_set_type(st_pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(st_pointer,lv_display_get_default());
    lv_indev_set_read_cb(st_pointer,[](lv_indev_t*,lv_indev_data_t*d){d->point=st_point;d->state=st_down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;});
    lv_timer_create([](lv_timer_t*t){
        static int tick=-300,stage=0,round=0,item=0,sub=0;
        static lv_obj_t *screen;
        static int volume=50,repeat=0;static bool shuffle=false,noise=false;
        static uint32_t frames=0,underruns=0,recoveries=0,radio_samples=0,radio_rebuffer=0;
        auto*pg=(PagerState*)lv_timer_get_user_data(t);
        IdlePower_NotifyActivity();lv_display_trigger_activity(nullptr);
        if(st_capture_busy.load())return;
        if(tick++<0)return;
        if(stage==0){
            printf("STABILITY_PROBE_BEGIN\n");fflush(stdout);
            HomeScreen::OpenGrid();GoToPage(pg,0);
#ifdef HI_RADIO_ONLY
            printf("STABILITY_SCOPE radio_only\n");stage=6;
#else
            stage=1;
#endif
            sub=0;return;
        }
        if(stage==1){
            const int points[][2]={{70,110},{280,210},{95,300}};
            if(sub==0){PanelScreen::Show();screen=lv_screen_active();}
            if(sub==3)StInput(points[item][0],points[item][1],true);
            if(sub==5)StInput(points[item][0],points[item][1],false);
            if(sub==12){StCheck(lv_screen_active()!=screen&&s_native_app_entry&&!strcmp(s_native_app_entry->icon_suffix,"calendar"),"date_card_calendar_pointer");HomeScreen::SwitchToHome();sub=-1;if(++item==3){stage=2;item=0;}}
            ++sub;return;
        }
        if(stage==2){
            const char*ids[]={"spirit_level","magnet","calculator","gps","weather","ai_image_gen","hi_music","compass","info"};
            if(sub==0){StCheck(StLaunch(ids[item]),ids[item]);screen=lv_screen_active();}
            if(sub==4){StCheck(StLaunch(ids[item])&&lv_screen_active()==screen,"active_app_reused");}
            if(sub==9){HomeScreen::SwitchToHome();sub=-1;if(++item==9){item=0;if(++round==3){stage=3;round=0;}}}
            ++sub;return;
        }
        if(stage==3){
            const char*keys[]={"C","9","9","9","9","9","9","9","9","9","9","9","9","+","8","=","−","7","=","×","6","=","÷","5","="};
            if(sub==0){StLaunch("calculator");screen=lv_screen_active();}
            if(sub>=3&&sub<53){const int n=(sub-3)/2;StCheck(StTap(keys[n],(sub&1)!=0),"calculator_key_pointer");}
            if(sub==32)StSnap("calculator_operator");
            if(sub==56){
                lv_obj_update_layout(screen);bool inside=true;unsigned labels=0;
                for(unsigned i=0;i<lv_obj_get_child_count(screen);++i){auto*o=lv_obj_get_child(screen,i);lv_area_t a;lv_obj_get_coords(o,&a);
                    if(a.x1==16&&a.y1==64){for(unsigned j=0;j<lv_obj_get_child_count(o);++j){auto*c=lv_obj_get_child(o,j);lv_area_t b;lv_obj_get_coords(c,&b);inside&=b.x1>=a.x1+5&&b.x2<=a.x2-5&&b.y1>=a.y1+5&&b.y2<=a.y2-5;++labels;}}
                }
                StCheck(inside&&labels>=2,"calculator_labels_inside_border");StSnap("calculator");HomeScreen::SwitchToHome();stage=4;sub=-1;
            }++sub;return;
        }
        if(stage==4){
            auto&p=LocalMusicPlayer::Instance();auto&a=Application::GetInstance().GetAudioService();
            if(sub==0){volume=a.GetVolume();repeat=p.RepeatMode();shuffle=p.Shuffle();noise=p.VinylNoise();StLaunch("hi_music");screen=lv_screen_active();}
            if(sub==15)StInput(78,528,true);if(sub==17)StInput(78,528,false);
            if(sub==20)StCheck(p.RepeatMode()==(repeat+1)%3,"music_repeat_pointer");
            if(sub==22)StInput(640,528,true);if(sub==24)StInput(640,528,false);
            if(sub==27)StCheck(p.Shuffle()!=shuffle,"music_shuffle_pointer");
            if(sub==29)StInput(80,630,true);if(sub==31)StInput(80,630,false);
            if(sub==34)StCheck(p.VinylNoise()!=noise,"music_noise_pointer");
            if(sub==36)StTap("列表",true);if(sub==38)StTap("列表",false);
            if(sub==41)StCheck(StLabel(screen,"歌曲与铃声 · SD 卡 /Music")!=nullptr,"music_library_pointer");
            if(sub==43)StTap("列表",true);if(sub==45)StTap("列表",false);
            if(sub==48)StInput(250,632,true);
            if(sub>=49&&sub<=58)StInput(250+(sub-48)*30,632,true);
            if(sub==59)StInput(550,632,false);
            if(sub==63)StCheck(a.GetVolume()>=65&&a.GetVolume()<=90,"music_volume_extended_hit_drag");
            if(sub==65)StInput(360,530,true);if(sub==67)StInput(360,530,false);
            if(sub==80){printf("STABILITY_MUSIC tracks=%u state=%d duration=%d position=%d\n",(unsigned)p.Count(),(int)p.GetState(),p.DurationMillis(),p.PositionMillis());StSnap("hi_music");}
            if(sub==82)StInput(160,108,true);
            if(sub>=83&&sub<=92)StInput(160+(sub-82)*35,108,true);
            if(sub==93)StInput(510,108,false);
            if(sub==100){StCheck(p.Count()==0||p.DurationMillis()==0||p.PositionMillis()>p.DurationMillis()/2,"music_seek_extended_hit_drag");}
            if(sub==102)StInput(200,530,true);if(sub==104)StInput(200,530,false);
            if(sub==110)StInput(518,530,true);if(sub==112)StInput(518,530,false);
            if(sub==120)StInput(40,450,true);
            if(sub>=121&&sub<=127)StInput(40+(sub-120)*25,450,true);
            if(sub==128)StInput(215,450,false);
            if(sub==135){StCheck(lv_screen_active()!=screen,"music_edge_back_pointer");p.Stop();p.SetRepeat(repeat);p.SetShuffle(shuffle);p.SetVinylNoise(noise);a.SetVolume(volume);HomeScreen::SwitchToHome();stage=5;sub=-1;}
            ++sub;return;
        }
        if(stage==5){
            if(sub%3==0)StLaunch(item%2?"magnet":"spirit_level");
            if(sub%3==2){HomeScreen::SwitchToHome();if(++item==100){stage=6;sub=-1;item=0;}}
            ++sub;return;
        }
        if(stage==6){
            if(sub==0){StLaunch("radio");RadioPlayer::Instance().Start();auto stats=RadioPlayer::Instance().GetProbeStats();radio_samples=stats.submitted;radio_rebuffer=stats.rebuffer;}
            if(sub==150){StandbyScreen::Show();}
            if(sub==300)StSnap("flip_clock");
            if(sub%10==0){
                auto stats=RadioPlayer::Instance().GetProbeStats();
                printf("STABILITY_RADIO samples=%lu produced=%lu buffered=%u rebuffer=%lu\n",(unsigned long)stats.submitted,(unsigned long)stats.produced,(unsigned)stats.buffered,(unsigned long)stats.rebuffer);
                int fd=open("/dev/fb0",O_RDONLY);metalio_display_health_s h{};
                if(fd>=0&&ioctl(fd,METALIO_FBIO_HEALTH,(unsigned long)&h)==0){
                    printf("STABILITY_HEALTH tick=%d frames=%lu underruns=%lu recoveries=%lu radio=%d\n",sub,(unsigned long)h.frames,(unsigned long)h.underruns,(unsigned long)h.recoveries,(int)RadioPlayer::Instance().status());
                    if(sub>0)StCheck(h.frames>frames,"scanout_advances");frames=h.frames;underruns=h.underruns;recoveries=h.recoveries;
                }if(fd>=0)close(fd);
            }
            if(sub==1200){auto stats=RadioPlayer::Instance().GetProbeStats();StCheck(stats.submitted-radio_samples>16000*60 && stats.rebuffer==radio_rebuffer,"radio_60s_pcm_without_rebuffer");StCheck(RadioPlayer::Instance().status()==RadioStatus::Playing,"radio_continuous_playback");RadioPlayer::Instance().Stop();StandbyScreen::ReturnHome();HomeScreen::OpenGrid();GoToPage(pg,0);stage=7;sub=-1;}
            ++sub;return;
        }
        if(stage==7){
            int fd=open("/dev/fb0",O_RDONLY);metalio_display_health_s h{};
            if(sub==10&&fd>=0){ioctl(fd,METALIO_FBIO_HEALTH,(unsigned long)&h);recoveries=h.recoveries;StCheck(ioctl(fd,METALIO_FBIO_STALL,0)==0,"display_boundary_stall_injected");}
            if(sub==30&&fd>=0){ioctl(fd,METALIO_FBIO_HEALTH,(unsigned long)&h);StCheck(h.frames>frames&&h.recoveries>recoveries,"display_recovery_resumes_frames");StCheck(pg->current_page==0,"home_first_page");
                printf("STABILITY_PROBE_COMPLETE failures=%u frames=%lu underruns=%lu recoveries=%lu\n",st_failures,(unsigned long)h.frames,(unsigned long)h.underruns,(unsigned long)h.recoveries);fflush(stdout);lv_indev_delete(st_pointer);st_pointer=nullptr;lv_timer_delete(t);
            }if(fd>=0)close(fd);++sub;
        }
    },100,pager);
}
