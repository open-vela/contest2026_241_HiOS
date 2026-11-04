#include "hi_task_policy.h"
#include "wifi_test.h"
#include "i18n.h"

#include "esp_log_shim.h"
#include "test_ui_common.h"
#include <metalio/metalio.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <pthread.h>

namespace {

constexpr const char* TAG = "WifiTest";

lv_obj_t* s_value_lbl   = nullptr;
lv_obj_t* s_status_icon = nullptr;

void SetText(const char* msg, bool pass) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(
        s_value_lbl,
        lv_color_hex(pass ? kTestColorTextDim : kTestColorError),
        LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, pass);
}

struct ScanJob {
    std::atomic<unsigned> references{1};
    std::atomic<bool> done{false};
    int result = -EAGAIN;
    lv_obj_t *label = nullptr;
    lv_timer_t *timer = nullptr;
};
void Release(ScanJob *job) {
    if (job->references.fetch_sub(1)==1) { job->~ScanJob();std::free(job); }
}
void *ScanWorker(void *arg) {
    auto *job=static_cast<ScanJob*>(arg);
    metalio_wifi_ap_s aps[METALIO_WIFI_MAX_AP];
    // Use the same real scan API as Network Settings. Do not change the
    // saved data mode, enable airplane-disabled RF or reconnect the station.
    job->result=metalio_esp_hosted_scan(aps,METALIO_WIFI_MAX_AP);
    job->done.store(true,std::memory_order_release);
    Release(job);return nullptr;
}

}  // namespace

namespace WifiTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "WiFi", &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, I18n::T("扫描中…"));
}

void OnLoad() {
    if (!s_value_lbl || lv_obj_get_user_data(s_value_lbl)) return;
    if (!metalio_esp_hosted_radio_enabled()) {
        SetText(I18n::T("WiFi 已关闭"),false);return;
    }
    void *memory=std::malloc(sizeof(ScanJob));
    if(!memory){SetText(I18n::T("暂时无法开始扫描"),false);return;}
    auto *job=new(memory) ScanJob;
    job->label=s_value_lbl;lv_obj_set_user_data(s_value_lbl,job);
    lv_obj_add_event_cb(s_value_lbl,[](lv_event_t *event){
        auto *job=static_cast<ScanJob*>(lv_event_get_user_data(event));
        if(job->timer)lv_timer_delete(job->timer);
        Release(job);
    },LV_EVENT_DELETE,job);
    job->timer=lv_timer_create([](lv_timer_t *timer){
        auto *job=static_cast<ScanJob*>(lv_timer_get_user_data(timer));
        if(s_value_lbl!=job->label){job->timer=nullptr;lv_timer_delete(timer);return;}
        if(!job->done.load(std::memory_order_acquire))return;
        const int n=job->result;
        if(n>0){char text[64];std::snprintf(text,sizeof(text),"发现 %d 个 WiFi",n);SetText(text,true);}
        else if(n==0)SetText(I18n::T("未发现附近 WiFi"),false);
        else if(n==-ETIMEDOUT)SetText(I18n::T("WiFi 扫描超时"),false);
        else if(n==-ENETDOWN)SetText(I18n::T("WiFi 链路未就绪"),false);
        else SetText(I18n::T("WiFi 扫描暂未成功"),false);
        ESP_LOGI(TAG,"wifi scan result=%d",n);
        job->timer=nullptr;lv_timer_delete(timer);
    },100,job);
    if(!job->timer){SetText(I18n::T("暂时无法开始扫描"),false);return;}
    job->references.fetch_add(1);
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,8192);
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);pthread_t thread;
    const int rc=hi_task_create_legacy(&thread,&attr,ScanWorker,job);pthread_attr_destroy(&attr);
    if(rc){job->result=-rc;job->done.store(true,std::memory_order_release);Release(job);}
}

void OnUnload() {
    s_value_lbl   = nullptr;
    s_status_icon = nullptr;
}

}  // namespace WifiTest
