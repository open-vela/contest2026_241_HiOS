/* Engineering-only pointer regression. Not compiled into production. */
#include "system_chrome/control_center.h"
static void StabilityArm(PagerState *pager)
{
    static bool armed;
    if (armed) return;
    armed = true;
    struct Probe {
        PagerState *pager;
        lv_indev_t *pointer;
        lv_point_t point;
        bool down;
        int tick = -100, stage = 0, step = 0, rounds = 0;
        unsigned failures = 0, clicks = 0, pages = 0;
        uint32_t frames = 0, underruns = 0, recoveries = 0;
        int original_mode = 0, original_volume = 0;
        bool restored = false;
        std::string pressed_label;
    };
    static Probe p{};
    p.pager = pager;
    p.pointer = lv_indev_create();
    lv_indev_set_type(p.pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(p.pointer, lv_display_get_default());
    lv_indev_set_user_data(p.pointer, &p);
    lv_indev_set_read_cb(p.pointer, [](lv_indev_t *i, lv_indev_data_t *d) {
        auto *p = (Probe *)lv_indev_get_user_data(i);
        d->point = p->point;
        d->state = p->down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    });
    lv_timer_create([](lv_timer_t *timer) {
        auto &p = *(Probe *)lv_timer_get_user_data(timer);
        auto input = [&p](int x, int y, bool down) {
            p.point = {x,y}; p.down = down; lv_indev_read(p.pointer);
        };
        auto check = [&p](bool ok, const char *name) {
            printf("CC_CHECK %s %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok) ++p.failures;
        };
        auto health = [&]() {
            metalio_display_health_s h{};
            int fd = open("/dev/fb0", O_RDONLY);
            int rc = fd < 0 ? -1 : ioctl(fd, METALIO_FBIO_HEALTH, (unsigned long)&h);
            if (fd >= 0) close(fd);
            check(rc == 0 && h.frames > p.frames, "scanout_advances");
            printf("CC_HEALTH frames=%lu underruns=%lu recoveries=%lu\n",
                (unsigned long)h.frames, (unsigned long)h.underruns, (unsigned long)h.recoveries);
            p.frames = h.frames;
            p.underruns = h.underruns; p.recoveries = h.recoveries;
        };
        auto find = [](auto &&self, lv_obj_t *o) -> lv_obj_t * {
            if (lv_obj_check_type(o, &lv_label_class)) {
                const char *s = lv_label_get_text(o);
                if (!strcmp(s,I18n::T("响铃")) || !strcmp(s,I18n::T("震动")) || !strcmp(s,I18n::T("静音"))) return o;
            }
            for (unsigned n=0; n<lv_obj_get_child_count(o); ++n)
                if (auto *r = self(self,lv_obj_get_child(o,n))) return r;
            return nullptr;
        };
        IdlePower_NotifyActivity();
        lv_display_trigger_activity(nullptr);
        if (p.tick++ < 0) return;
        if (p.stage == 0) {
            Settings s("network"); p.original_mode=s.GetInt("sound_mode",0);
            p.original_volume=Application::GetInstance().GetAudioService().GetVolume();
            printf("CC_PROBE_BEGIN mode=%d volume=%d\n",p.original_mode,p.original_volume);
            HomeScreen::OpenGrid(); GoToPage(p.pager,0); health(); p.stage=1; p.step=0; return;
        }
        if (p.stage == 1) {
            const int target = p.rounds % 2 ? 0 : std::min(2,p.pager->page_count-1);
            if (p.step==0) GoToPage(p.pager,target);
            if (p.step==10) {check(p.pager->current_page==target,"animated_page_target"); ++p.pages; health(); p.step=-1;
                if (++p.rounds == 40) {p.stage=2;p.rounds=0;}}
            ++p.step; return;
        }
        if (p.stage == 2) {
            if (p.step==0) ControlCenter_Show(lv_layer_top());
            if (p.step==3 || p.step==8 || p.step==13) {
                lv_obj_t *label=find(find,lv_layer_top()); check(label!=nullptr,"sound_tile_found");
                if (label) {p.pressed_label=lv_label_get_text(label);lv_area_t a;lv_obj_get_coords(label,&a);input((a.x1+a.x2)/2,(a.y1+a.y2)/2,true); ++p.clicks;}
            }
            if (p.step==4 || p.step==9 || p.step==14) input(p.point.x,p.point.y,false);
            if (p.step==6 || p.step==11 || p.step==15) {
                auto *label=find(find,lv_layer_top());
                check(label && p.pressed_label!=lv_label_get_text(label),"sound_mode_actually_changed");
            }
            // Close/reopen while preview/save workers are still pending.
            if (p.step==16) ControlCenter_Hide();
            if (p.step==18 && p.rounds%2==0) ControlCenter_Show(lv_layer_top());
            if (p.step==22) {ControlCenter_Hide();health();p.step=-1;
                if (++p.rounds==30) {p.stage=3;p.rounds=0;}}
            ++p.step; return;
        }
        if (p.stage==3) {
            if (p.step==0) {ControlCenter_Show(lv_layer_top());}
            if (p.step==4) {
                Settings s("network");int mode=s.GetInt("sound_mode",0);
                if (mode!=p.original_mode) {
                    lv_obj_t *label=find(find,lv_layer_top());
                    if (label) {lv_area_t a;lv_obj_get_coords(label,&a);input((a.x1+a.x2)/2,(a.y1+a.y2)/2,true);}
                } else p.restored=true;
            }
            if (p.step==5)input(p.point.x,p.point.y,false);
            if (p.step==22) {ControlCenter_Hide();p.step=-1;
                if(p.restored || ++p.rounds>=4) {p.stage=4;Application::GetInstance().GetAudioService().SetVolume(p.original_volume);}}
            ++p.step;return;
        }
        if (p.stage==4 && ++p.step==80) {
            health(); check(p.clicks==90,"all_sound_mode_clicks");check(p.pages==40,"all_animated_page_changes");check(p.restored,"original_sound_mode_restored");
            HomeScreen::SwitchToHome();
            printf("CC_PROBE_COMPLETE failures=%u clicks=%u pages=%u underruns=%lu recoveries=%lu\n",p.failures,p.clicks,p.pages,(unsigned long)p.underruns,(unsigned long)p.recoveries);
            lv_indev_delete(p.pointer);lv_timer_delete(timer);
        }
        fflush(stdout);
    },100,&p);
}
