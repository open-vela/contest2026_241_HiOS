#include "hi_task_policy.h"
/* SPDX-License-Identifier: Apache-2.0
 * MetalioClaw4 udisp JPEG stream, bounded RX/decoder/display ownership.
 * USB callbacks only assemble frames; neither JPEG decoding nor LVGL can
 * block endpoint zero or MSC service. No raw framebuffer/DMA reconfiguration.
 */
#include "usb_secondary_display.h"
#include "usb_virtual_disk.h"
#include "lvgl.h"
#include "tusb.h"
#include "screen_util.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <vector>
#include <syslog.h>

namespace UsbSecondary {
namespace {
std::atomic<size_t> limit{100*1024};
std::atomic<int> max_fps{15};
char description[96]="esp32p4udisp0_R720x720_Ejpg5_Fps15_Bl102400";
std::mutex frames_mutex;
std::vector<uint8_t> pending;
unsigned serial=0;
std::atomic<unsigned> epoch{0};
std::atomic<bool> worker_started{false};
std::atomic<bool> has_picture{false};
std::atomic<uint32_t> touch_value{0},touch_sequence{0};
uint32_t touch_sent=0;
lv_obj_t *surface=nullptr,*image=nullptr,*close_button=nullptr;
lv_timer_t *surface_timer=nullptr;
lv_image_dsc_t descriptor{};
std::vector<uint16_t> shown;
uint8_t header[16];size_t header_bytes=0,total=0,received=0;
bool accept=false;
std::vector<uint8_t> incoming;

uint16_t U16(const uint8_t *p){return p[0]|(uint16_t)p[1]<<8;}
uint32_t U32(const uint8_t *p){return U16(p)|(uint32_t)U16(p+2)<<16;}
struct Decode {const uint8_t *bytes;size_t size,at;std::vector<uint16_t> *pixels;unsigned epoch;};
size_t Read(JDEC *d,uint8_t *out,size_t n){
    auto *c=static_cast<Decode*>(d->device);
    if(c->epoch!=epoch.load())return 0;
    n=std::min(n,c->size-c->at);if(out)memcpy(out,c->bytes+c->at,n);c->at+=n;return n;
}
int Write(JDEC *d,void *rgb,JRECT *r){
    auto *c=static_cast<Decode*>(d->device);if(c->epoch!=epoch.load())return 0;
    const auto *p=static_cast<const uint8_t*>(rgb);
    for(unsigned y=r->top;y<=r->bottom;++y)for(unsigned x=r->left;x<=r->right;++x){
        (*c->pixels)[y*720+x]=((p[2]&0xf8)<<8)|((p[1]&0xfc)<<3)|(p[0]>>3);p+=3;
    }
    return 1;
}
void *Decoder(void*){
    int policy=0;sched_param priority{};
    if(pthread_getschedparam(pthread_self(),&policy,&priority)==0){priority.sched_priority=CONFIG_METALIO_APP_PRIORITY-2;(void)pthread_setschedparam(pthread_self(),SCHED_RR,&priority);}
    uint32_t last=0,errors=0;
    for(;;){
        if(!UsbVirtualDisk::GetInstance().IsDisplayActive()){usleep(20000);continue;}
        uint32_t now=lv_tick_get();
        if(now-last<1000u/static_cast<unsigned>(std::max(1,max_fps.load()))){usleep(2000);continue;}
        std::vector<uint8_t> jpeg;unsigned version;
        {std::lock_guard<std::mutex> lock(frames_mutex);jpeg.swap(pending);version=serial;}
        if(jpeg.empty()){usleep(5000);continue;}
        unsigned current=epoch.load();
        std::vector<uint16_t> decoded(720*720);
        Decode ctx{jpeg.data(),jpeg.size(),0,&decoded,current};
        uint8_t work[4096];JDEC decoder{};
        JRESULT result=jd_prepare(&decoder,Read,work,sizeof(work),&ctx);
        if(result==JDR_OK&&decoder.width==720&&decoder.height==720)
            result=jd_decomp(&decoder,Write,0);
        else if(result==JDR_OK) result=JDR_FMT1;
        if(result!=JDR_OK){
            ++errors;
            if((errors&(errors-1))==0)syslog(LOG_WARNING,"USB_SCREEN decode=%d errors=%lu bytes=%lu\n",(int)result,(unsigned long)errors,(unsigned long)jpeg.size());
            continue;
        }
        lv_lock();
        if(current==epoch.load()&&surface&&image&&UsbVirtualDisk::GetInstance().IsDisplayActive()){
            lv_image_cache_drop(&descriptor);
            shown.swap(decoded);
            descriptor.header.magic=LV_IMAGE_HEADER_MAGIC;descriptor.header.cf=LV_COLOR_FORMAT_RGB565;
            descriptor.header.w=720;descriptor.header.h=720;descriptor.header.stride=1440;
            descriptor.data_size=shown.size()*2;descriptor.data=reinterpret_cast<const uint8_t*>(shown.data());
            lv_image_set_src(image,&descriptor);lv_obj_remove_flag(image,LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(close_button,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(close_button);
            if(!has_picture.exchange(true))syslog(LOG_INFO,"USB_SCREEN first picture %ux%u\n",decoder.width,decoder.height);
            last=lv_tick_get();
        }
        lv_unlock();(void)version;
    }
    return nullptr;
}
}
void Configure(int quality,int fps,int bytes){
    limit=std::max(32*1024,std::min(300*1024,bytes));max_fps=std::max(1,std::min(30,fps));
    snprintf(description,sizeof(description),"esp32p4udisp0_R720x720_Ejpg%d_Fps%d_Bl%u",std::max(1,std::min(100,quality)),max_fps.load(),static_cast<unsigned>(limit.load()));
}
size_t FrameLimit(){return limit.load();}
const char *Description(){return description;}
void Attach(lv_obj_t *root){
    ++epoch;has_picture=false;surface=root;
    if(surface_timer){lv_timer_delete(surface_timer);surface_timer=nullptr;}
    if(image){lv_image_set_src(image,nullptr);lv_image_cache_drop(&descriptor);lv_obj_delete(image);image=nullptr;}
    if(close_button){lv_obj_delete(close_button);close_button=nullptr;}
    if(!root)return;
    image=lv_image_create(root);lv_obj_set_pos(image,0,0);lv_obj_set_size(image,720,720);lv_obj_add_flag(image,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(image,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_flag(image,LV_OBJ_FLAG_PRESS_LOCK);
    screen_swipe_back_ignore(image,true);
    lv_obj_add_event_cb(image,[](lv_event_t *e){
        auto code=lv_event_get_code(e);
        if(code!=LV_EVENT_PRESSED&&code!=LV_EVENT_PRESSING&&code!=LV_EVENT_RELEASED&&code!=LV_EVENT_PRESS_LOST)return;
        lv_point_t point{};auto *input=lv_indev_active();if(!input)return;lv_indev_get_point(input,&point);
        uint32_t value=static_cast<uint32_t>(std::max(0,std::min(719,(int)point.x))) |
            (static_cast<uint32_t>(std::max(0,std::min(719,(int)point.y)))<<10);
        if(code==LV_EVENT_PRESSED||code==LV_EVENT_PRESSING)value|=1u<<20;
        touch_value=value;++touch_sequence;lv_event_stop_bubbling(e);
    },LV_EVENT_ALL,nullptr);
    close_button=lv_button_create(root);lv_obj_set_size(close_button,64,50);lv_obj_set_pos(close_button,650,6);
    auto *label=lv_label_create(close_button);lv_label_set_text(label,"X");lv_obj_center(label);
    lv_obj_add_event_cb(close_button,[](lv_event_t*){UsbVirtualDisk::GetInstance().StopDisplay();},LV_EVENT_CLICKED,nullptr);
    lv_obj_add_flag(close_button,LV_OBJ_FLAG_HIDDEN);
    surface_timer=lv_timer_create([](lv_timer_t*){
        if(!UsbVirtualDisk::GetInstance().IsDisplayActive()&&image){lv_obj_add_flag(image,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(close_button,LV_OBJ_FLAG_HIDDEN);}
    },100,nullptr);
    if(!worker_started.exchange(true)){
        pthread_t thread;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,16384);
        int rc=hi_task_create_legacy(&thread,&attr,Decoder,nullptr);pthread_attr_destroy(&attr);
        if(rc==0)pthread_detach(thread);else worker_started=false;
    }
}
// Called only by the USB worker, including reset. Handles split headers and
// several frames in one transfer. A host can pad a bulk write after the JPEG;
// resynchronize byte-by-byte instead of treating padding as a huge frame and
// silently discarding every subsequent picture. Validate before allocating.
void Receive(const uint8_t *data,size_t bytes){
    while(bytes){
        if(header_bytes<16){size_t n=std::min(bytes,16-header_bytes);memcpy(header+header_bytes,data,n);header_bytes+=n;data+=n;bytes-=n;
            if(header_bytes<16)return;
            total=U32(header+12)>>10;received=0;
            const bool framed=header[2]<=3&&U16(header+8)>0&&U16(header+8)<=720&&
                U16(header+10)>0&&U16(header+10)<=720&&U16(header+4)<720&&U16(header+6)<720&&total&&total<=4*1024*1024;
            if(!framed){memmove(header,header+1,15);header_bytes=15;total=0;continue;}
            accept=header[2]==3&&U16(header+4)==0&&U16(header+6)==0&&U16(header+8)==720&&U16(header+10)==720&&total&&total<=limit.load();
            incoming.clear();if(accept)incoming.reserve(total);
            if(!total){header_bytes=0;continue;}
        }
        size_t n=std::min(bytes,total-received);
        if(accept)incoming.insert(incoming.end(),data,data+n);
        data+=n;bytes-=n;received+=n;
        if(received==total){
            if(accept&&incoming.size()>=2&&incoming[0]==0xff&&incoming[1]==0xd8){std::lock_guard<std::mutex> lock(frames_mutex);pending.swap(incoming);++serial;}
            header_bytes=0;total=received=0;accept=false;
        }
    }
}
void ServiceTouch(){
    const uint32_t sequence=touch_sequence.load();
    if(sequence==touch_sent||!tud_hid_n_ready(0))return;
    uint32_t value=touch_value.load();uint16_t x=value&1023,y=(value>>10)&1023;
    uint8_t report[41]{};bool down=(value&(1u<<20))!=0;
    report[0]=down?1:0;report[1]=1;report[2]=x;report[3]=x>>8;report[4]=y;report[5]=y>>8;
    report[6]=8;report[7]=8;report[40]=down?1:0;
    if(tud_hid_n_report(0,1,report,sizeof(report)))touch_sent=sequence;
}
bool HasPicture(){return has_picture.load();}
void Reset(){has_picture=false;touch_value=0;++touch_sequence;++epoch;header_bytes=total=received=0;accept=false;incoming.clear();std::lock_guard<std::mutex> lock(frames_mutex);pending.clear();}
}
