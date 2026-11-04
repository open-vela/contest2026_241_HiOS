#include "hi_task_policy.h"
/* SPDX-License-Identifier: Apache-2.0 */
#include "usb_virtual_disk.h"
#include "usb_secondary_display.h"
#include "usb_secondary_touch.h"
#include <algorithm>
#include <sched.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <syslog.h>
#include <metalio/metalio.h>
#include <lvgl.h>
#include "tusb.h"
#include "device/dcd.h"

namespace {
using Hint = UsbVirtualDisk::UiHint;
std::atomic<bool> active{false}, busy{false}, exported{false}, requested{false};
std::atomic<bool> started{false}, force_stop{false};
std::atomic<Hint> hint{Hint::Disabled};
std::atomic<bool> display_mode{false},notify_pending{false};
uint32_t enumeration_start=0;
bool enumeration_failed=false;
std::mutex notify_mutex;
UsbVirtualDisk::UiNotifyFn notify;
// These fields are only touched by the single USB worker/callback thread.
uint32_t block_count;
bool prevent_removal;
uint32_t stop_after;
uint32_t reads, writes, io_errors, bounds_errors, host_stop, last_sector;
int last_io_result;

void Notify(Hint value)
{
    hint.store(value);
    notify_pending.store(true);
}

// UI notification never runs on the USB transport thread. Waiting for a
// JPEG decode or LVGL refresh here used to starve enumeration/control EP0.
void *NotificationWorker(void *) {
    for(;;){
        if(notify_pending.exchange(false)){
            UsbVirtualDisk::UiNotifyFn fn;
            {std::lock_guard<std::mutex> lock(notify_mutex);fn=notify;}
            if(fn){lv_lock();fn();lv_unlock();}
        }
        usleep(20000);
    }
    return nullptr;
}

void *Worker(void *)
{
    int policy=0;sched_param priority{};
    if(pthread_getschedparam(pthread_self(),&policy,&priority)==0){
        priority.sched_priority=CONFIG_METALIO_APP_PRIORITY+13;
        (void)pthread_setschedparam(pthread_self(),SCHED_RR,&priority);
    }
    while (true) {
        if (requested.load() && !active.load()) {
            Notify(Hint::Enabling);
            const bool screen=display_mode.load();
            int ret = screen ? 0 : metalio_sdcard_export_begin(&block_count);
            if (ret == 0) {
                exported.store(!screen);
                UsbSecondary::Reset();
                ret = metalio_usb_msc_phy(true);
                if (ret == 0) {
                    tusb_rhport_init_t init = {};
                    init.role = TUSB_ROLE_DEVICE;
                    init.speed = TUSB_SPEED_FULL;
                    if (!tusb_init(0, &init)) ret = -EIO;
                }
                if (ret < 0) {
                    if (tud_inited()) tusb_deinit(0);
                    metalio_usb_msc_phy(false);
                    if(!screen)metalio_sdcard_export_end();
                    exported.store(false);
                }
            }
            enumeration_failed=false;
            enumeration_start=tusb_time_millis_api();
            active.store(ret == 0);
            if (ret < 0) requested.store(false);
            prevent_removal = false;
            stop_after = 0;
            busy.store(false);
            Notify(ret == 0 ? Hint::EnabledHost :
                   ret == -EBUSY ? Hint::HostBusy :
                   ret == -ENODEV || ret == -ENOENT ? Hint::NoSdCard : Hint::EnableFailed);
            syslog(LOG_INFO, "USB_MSC enable=%d blocks=%lu\n", ret, (unsigned long)block_count);
        }
        if (active.load()) {
            // Both events and block I/O run here. LVGL never waits for SD I/O.
            dcd_int_handler(0);
            tud_task_ext(0, false);
            if(display_mode.load()&&tud_mounted())UsbSecondary::ServiceTouch();
            // Restore USB serial automatically if Windows cannot enumerate.
            // Never leave the device unreachable indefinitely after a failed
            // descriptor handshake. Once mounted, no lease expires during I/O.
            if(requested.load() && !tud_mounted() && tusb_time_millis_api()-enumeration_start>12000){
                enumeration_failed=true;force_stop=true;requested=false;Notify(Hint::EnableFailed);
            }
            if (!requested.load()) {
                if (prevent_removal && !force_stop.load()) {
                    requested.store(true);
                    busy.store(false);
                    Notify(Hint::HostBusy);
                } else {
                    uint32_t now = tusb_time_millis_api();
                    if (stop_after == 0) stop_after = now + 250;
                    if ((int32_t)(now - stop_after) >= 0) {
                        Notify(Hint::Disabling);
                        tud_disconnect();
                        tusb_deinit(0);
                        metalio_usb_msc_phy(false);
                        int ret = display_mode.load()?0:metalio_sdcard_export_end();
                        UsbSecondary::Reset();
                        active.store(false);
                        exported.store(false);
                        force_stop.store(false);
                        busy.store(false);
                        Notify(ret == 0 ? (enumeration_failed?Hint::EnableFailed:Hint::Disabled) : Hint::DisableFailed);
                        syslog(LOG_INFO, "USB_MSC stopped remount=%d\n", ret);
                    }
                }
            }
            if (active.load()) metalio_usb_msc_wait_event();
        } else {
            usleep(20000);
        }
    }
    return nullptr;
}

const tusb_desc_device_t descriptor = {
    sizeof(tusb_desc_device_t), TUSB_DESC_DEVICE, 0x0200,
    0, 0, 0, CFG_TUD_ENDPOINT0_SIZE, 0x303a, 0x4002, 0x0100, 1, 2, 3, 1
};
const tusb_desc_device_t display_descriptor = {
    sizeof(tusb_desc_device_t),TUSB_DESC_DEVICE,0x0200,
    0,0,0,CFG_TUD_ENDPOINT0_SIZE,0x303a,0x2986,0x0101,1,2,3,1
};
const uint8_t touch_descriptor[]={TUD_HID_REPORT_DESC_TOUCH_SCREEN(1,720,720)};
const uint8_t display_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1,2,0,TUD_CONFIG_DESC_LEN+TUD_VENDOR_DESC_LEN+TUD_HID_DESC_LEN,0,100),
    TUD_VENDOR_DESCRIPTOR(0,4,0x01,0x81,64),
    TUD_HID_DESCRIPTOR(1,0,HID_ITF_PROTOCOL_NONE,sizeof(touch_descriptor),0x82,64,10)
};
const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN, 0, 100),
    TUD_MSC_DESCRIPTOR(0, 0, 0x01, 0x81, 64)
};
}

void UsbVirtualDisk::Init()
{
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    pthread_t thread;
    int ret = hi_task_create_legacy(&thread, &attr, Worker, nullptr);
    pthread_attr_destroy(&attr);
    if (ret == 0) {
        pthread_detach(thread);
        pthread_t notifications;pthread_attr_t nattr;pthread_attr_init(&nattr);
        pthread_attr_setstacksize(&nattr,8192);
        if(hi_task_create_legacy(&notifications,&nattr,NotificationWorker,nullptr)==0)pthread_detach(notifications);
        pthread_attr_destroy(&nattr);
    }
    else { started.store(false); hint.store(Hint::EnableFailed); }
}
bool UsbVirtualDisk::IsSupported() const { return true; }
bool UsbVirtualDisk::IsGadgetActive() const { return active.load(); }
bool UsbVirtualDisk::IsBusy() const { return busy.load(); }
bool UsbVirtualDisk::IsSdExportedToHost() const { return exported.load(); }
UsbVirtualDisk::UiHint UsbVirtualDisk::GetUiHint() const { return hint.load(); }
void UsbVirtualDisk::Toggle()
{
    Init();
    if (!started.load()) return;
    bool expected = false;
    if (!busy.compare_exchange_strong(expected, true)) return;
    if(active.load()&&display_mode.load()){busy=false;return;}
    if(!active.load())display_mode=false;
    requested.store(!active.load());
}
bool UsbVirtualDisk::StartDisplay(){
    Init();if(!started.load())return false;
    bool expected=false;if(!busy.compare_exchange_strong(expected,true))return false;
    if(active.load()){busy=false;return display_mode.load();}
    display_mode=true;requested=true;return true;
}
void UsbVirtualDisk::StopDisplay(){if(display_mode.load())DisableIfActive();}
bool UsbVirtualDisk::IsDisplayActive() const {return display_mode.load()&&active.load()&&requested.load();}
bool UsbVirtualDisk::IsDisplayRequested() const {return display_mode.load()&&requested.load();}
void UsbVirtualDisk::DisableIfActive()
{
    if (active.load() || busy.load()) {
        force_stop.store(true);
        requested.store(false);
    }
}
void UsbVirtualDisk::SetUiNotify(UiNotifyFn fn)
{
    std::lock_guard<std::mutex> lock(notify_mutex);
    notify = std::move(fn);
}
const char *UsbVirtualDisk::HintMsgid(UiHint value)
{
    switch (value) {
    case Hint::Enabling: return "正在启用虚拟 U 盘…";
    case Hint::Disabling: return "正在停用虚拟 U 盘…";
    case Hint::EnabledHost: return "虚拟 U 盘已启用，请在电脑上访问；停用前请先弹出";
    case Hint::EnableFailed: return "启用失败，请检查 USB 线并重试";
    case Hint::DisableFailed: return "SD 卡未能重新挂载，请重试";
    case Hint::NoSdCard: return "未检测到 SD 卡";
    case Hint::HostBusy: return "SD 卡正在使用，请关闭文件或在电脑上弹出 U 盘";
    default: return "可浏览 SD 卡；启用虚拟 U 盘后占用 USB，停用后恢复串口";
    }
}

extern "C" {
uint32_t tusb_time_millis_api(void)
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
void tusb_time_delay_ms_api(uint32_t ms) { usleep(ms * 1000); }
const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)(display_mode.load()?&display_descriptor:&descriptor); }
const uint8_t *tud_descriptor_configuration_cb(uint8_t) { return display_mode.load()?display_configuration:configuration; }
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t)
{
    static uint16_t text[96];
    const char *strings[] = {"", "HiOS", "HiOS SD Reader", "HiOS-P4-SD-01"};
    if (index == 0) { text[0] = 0x0304; text[1] = 0x0409; return text; }
    if(index>=4 && !(display_mode.load()&&index==4))return nullptr;
    const char *value=index==4?UsbSecondary::Description():strings[index];
    if(display_mode.load()){if(index==1)value="Espressif";if(index==2)value="ESP_Extern_Screen";if(index==3)value="012-2021";}
    size_t n=std::min<size_t>(95,strlen(value));
    for(size_t i=0;i<n;++i)text[i+1]=value[i];
    text[0] = (TUSB_DESC_STRING << 8) | (2 * n + 2);
    return text;
}
const uint8_t *tud_hid_descriptor_report_cb(uint8_t){return touch_descriptor;}
uint16_t tud_hid_get_report_cb(uint8_t,uint8_t id,hid_report_type_t type,uint8_t *buffer,uint16_t size){
    if(id==2&&type==HID_REPORT_TYPE_FEATURE&&size){buffer[0]=1;return 1;}return 0;
}
void tud_hid_set_report_cb(uint8_t,uint8_t,hid_report_type_t,const uint8_t*,uint16_t){}
void tud_vendor_rx_cb(uint8_t itf,const uint8_t *,uint32_t){
    uint8_t packet[640];
    while(tud_vendor_n_available(itf)){
        uint32_t n=tud_vendor_n_read(itf,packet,sizeof(packet));
        if(!n)break;
        if(display_mode.load()&&requested.load())UsbSecondary::Receive(packet,n);
    }
}
void tud_msc_inquiry_cb(uint8_t, uint8_t vendor[8], uint8_t product[16], uint8_t rev[4])
{
    memcpy(vendor, "HiOS    ", 8);
    memcpy(product, "SD Reader       ", 16);
    memcpy(rev, "1.00", 4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!exported.load() || !requested.load()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0);
        return false;
    }
    return true;
}
void tud_msc_capacity_cb(uint8_t, uint32_t *blocks, uint16_t *size)
{ *blocks = block_count; *size = 512; }
bool tud_msc_is_writable_cb(uint8_t) { return exported.load() && requested.load(); }
bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool start, bool eject)
{
    host_stop = (host_stop << 4) | (start ? 1 : 0) | (eject ? 2 : 0);
    if (eject && !start) {
        prevent_removal = false;
        busy.store(true);
        requested.store(false);
    }
    return true;
}
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t, uint8_t prohibit, uint8_t)
{
    prevent_removal = prohibit != 0;
    return true;
}
static int32_t Transfer(uint8_t lun, uint32_t lba, uint32_t offset,
                        void *buffer, uint32_t size, bool write_data)
{
    uint64_t first = (uint64_t)lba + offset / 512;
    if (!exported.load() || !requested.load() || offset % 512 || size % 512 ||
        first + size / 512 > block_count) {
        ++bounds_errors;
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0);
        return -1;
    }
    int ret = metalio_sdcard_export_io((uint32_t)first, buffer, size / 512, write_data);
    last_sector = (uint32_t)first;
    last_io_result = ret;
    if (write_data) ++writes; else ++reads;
    if (ret < 0) {
        ++io_errors;
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, write_data ? 0x0c : 0x11, 0);
        return -1;
    }
    return size;
}
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t size)
{ return Transfer(lun, lba, offset, buffer, size, false); }
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size)
{ return Transfer(lun, lba, offset, buffer, size, true); }
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t command[16], void *, uint16_t)
{
    // SDMMC writes are synchronous; there is no write-back cache to flush.
    if (command[0] == 0x35) return 0; // SYNCHRONIZE CACHE
    if (command[0] == 0x1e) { prevent_removal = (command[4] & 1) != 0; return 0; }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0);
    return -1;
}
void metalio_usb_msc_report(void)
{
    // Diagnostic caller invokes this after the USB worker has stopped.
    printf("MSC_IO reads=%lu writes=%lu io_errors=%lu bounds_errors=%lu last=%lu result=%d stop=%lx\n",
           (unsigned long)reads, (unsigned long)writes, (unsigned long)io_errors,
           (unsigned long)bounds_errors, (unsigned long)last_sector, last_io_result,
           (unsigned long)host_stop);
}
}
