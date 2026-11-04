#include "hi_task_policy.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * MagnetScreen — Claw4 layout/功能 on openvela.
 * Anti-blue on this flush path:
 *   - no lv_bar / LV_BAR_MODE_SYMMETRICAL
 *   - no rounded radius / translucent border opa
 *   - no I2C on LVGL create/arm tick (worker pthread)
 *   - remove_style_all + opaque bg on screen
 */

#include "magnet_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "home_screen/home_screen.h"
#include <metalio/metalio.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <mutex>
#include <cmath>
#include <ctime>
#include <unistd.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char *TAG = "MagnetScreen";

constexpr uint8_t kQmcChipId = 0x90;
constexpr uint8_t kQmcStatusDrdy = 0x01;
constexpr uint8_t kQmcStatusOvl = 0x02;

constexpr float kLsbToMicroTesla = 0.1f;
constexpr float kLsbToGauss = 0.001f;

constexpr int kPanelW = 720;
constexpr int kPanelH = 720;
constexpr int kHeaderH = 90;
constexpr int kCardLeft = 36;
constexpr int kCardW = kPanelW - kCardLeft * 2;
constexpr int kCardH = 150;
constexpr int kCardGap = 14;
constexpr int kCardTopY = kHeaderH + 12;
constexpr int kBarH = 12;
constexpr int kTrackW = kCardW - 48;
constexpr int kBarRangeLsb = 5000;
constexpr int kSummaryTopY = kCardTopY + kCardH * 3 + kCardGap * 3;

constexpr uint32_t kSamplePeriodMs = 33;
constexpr uint32_t kSensorPeriodUs = 50000;
constexpr uint32_t kArmDelayMs = 600;
constexpr uint32_t kUiWarmTicks = 4;
constexpr uint32_t kTextEveryN = 3;

struct AxisRow {
    lv_obj_t *card = nullptr;
    lv_obj_t *title_lbl = nullptr;
    lv_obj_t *raw_lbl = nullptr;
    lv_obj_t *phys_lbl = nullptr;
    lv_obj_t *bar_track = nullptr;
    lv_obj_t *bar_fill = nullptr;
    lv_obj_t *cursor = nullptr;
    float displayed = 0;
};

struct UiState {
    lv_obj_t *screen = nullptr;
    lv_obj_t *sensor_lbl = nullptr;
    AxisRow axes[3];
    lv_obj_t *total_lbl = nullptr;
    lv_obj_t *status_lbl = nullptr;
    lv_timer_t *sample_timer = nullptr;
    lv_timer_t *arm_timer = nullptr;
    uint32_t tick_n = 0;
    bool sensor_lbl_set = false;
    int range_lsb = 1000;
    lv_obj_t *range_lbl = nullptr;
};

UiState s_ui;
uint32_t s_frame_cnt = 0;

std::atomic<bool> s_worker_run{false};
struct Sample { int16_t axis[3]{}; uint8_t status=0; bool ok=false; uint32_t at=0; };
std::mutex s_sample_mutex;
Sample s_sample;
uint32_t NowMs() {
    timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);
    return uint32_t(uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000);
}
pthread_t s_worker_tid{};
bool s_worker_started = false;

uint32_t AxisColor(int idx)
{
    static const uint32_t kColors[] = {0xF87171, 0x34D399, 0x60A5FA};
    return kColors[idx];
}

const char *AxisTitle(int idx)
{
    static const char *kNames[] = {I18n::T("X 轴"), I18n::T("Y 轴"),
                                   I18n::T("Z 轴")};
    return kNames[idx];
}

void *SensorWorker(void * /*arg*/)
{
    write(1, "MAG_WRK\n", 8);
    for (;;) {
        if (!s_worker_run.load(std::memory_order_relaxed)) {usleep(kSensorPeriodUs);continue;}
        int16_t mx = 0, my = 0, mz = 0;
        uint8_t st = 0;
        const int rc=metalio_qmc6309_read_raw_ex(&mx,&my,&mz,&st);
        {
            std::lock_guard<std::mutex> guard(s_sample_mutex);
            s_sample.status=st;
            if(rc<0 || (st&kQmcStatusOvl)) s_sample.ok=false;
            else if(st&kQmcStatusDrdy) {
                s_sample.axis[0]=mx;s_sample.axis[1]=my;s_sample.axis[2]=mz;
                s_sample.at=NowMs();s_sample.ok=true;
            }
        }
        usleep(kSensorPeriodUs);
    }
    write(1, "MAG_WRK_END\n", 12);
    return nullptr;
}

void StopSensorWorker()
{
    s_worker_run.store(false, std::memory_order_relaxed);
}

void StartSensorWorker()
{
    s_worker_run.store(true, std::memory_order_relaxed);
    if (s_worker_started) return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);
    if (hi_task_create_legacy(&s_worker_tid, &attr, SensorWorker, nullptr) != 0) {
        pthread_attr_destroy(&attr);
        s_worker_run.store(false, std::memory_order_relaxed);
        write(1, "MAG_WRK_FAIL\n", 13);
        return;
    }
    pthread_attr_destroy(&attr);
    s_worker_started = true;
    pthread_detach(s_worker_tid);
    write(1, "MAG_WRK_OK\n", 11);
}

void UpdateBarFill(int idx, int16_t raw)
{
    AxisRow &row = s_ui.axes[idx];
    if (row.bar_fill == nullptr) {
        return;
    }
    row.displayed += (float(raw)-row.displayed)*.38f;
    if(std::abs(float(raw)-row.displayed)<.5f) row.displayed=raw;
    int v=std::clamp(int(::lroundf(row.displayed)),-s_ui.range_lsb,s_ui.range_lsb);
    const int half = kTrackW / 2;
    int w = (std::abs(v) * half) / s_ui.range_lsb;
    if (w < 2 && v != 0) {
        w = 2;
    }
    if(row.cursor) lv_obj_set_pos(row.cursor,half+v*half/s_ui.range_lsb-2,0);
    if (v == 0) {
        lv_obj_set_size(row.bar_fill, 2, kBarH);
        lv_obj_set_pos(row.bar_fill, half - 1, 0);
        return;
    }
    lv_obj_set_size(row.bar_fill, w, kBarH);
    lv_obj_set_pos(row.bar_fill, v > 0 ? half : (half - w), 0);
}

void UpdateAxisRow(int idx, int16_t raw, bool update_text)
{
    AxisRow &row = s_ui.axes[idx];
    if (update_text && row.raw_lbl != nullptr) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%+d  LSB", raw);
        lv_label_set_text(row.raw_lbl, buf);
    }
    if (update_text && row.phys_lbl != nullptr) {
        float gauss = raw * kLsbToGauss;
        float ut = raw * kLsbToMicroTesla;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%+.3f G    %+.1f uT", gauss, ut);
        lv_label_set_text(row.phys_lbl, buf);
    }
    UpdateBarFill(idx, raw);
}

void MaybeSetSensorLabel(bool sensor_ok)
{
    if (s_ui.sensor_lbl == nullptr) {
        return;
    }
    s_ui.sensor_lbl_set = true;
    char buf[48];
    uint8_t id = 0;
    if (sensor_ok && metalio_qmc6309_chip_id(&id) == 0 && id == kQmcChipId) {
        std::snprintf(buf, sizeof(buf), "QMC6309  ID 0x%02X", id);
        lv_obj_set_style_text_color(s_ui.sensor_lbl, lv_color_hex(0x9AA3B2),
                                    LV_PART_MAIN);
    } else {
        std::snprintf(buf, sizeof(buf), "QMC6309  --");
        lv_obj_set_style_text_color(s_ui.sensor_lbl, lv_color_hex(0xF87171),
                                    LV_PART_MAIN);
    }
    lv_label_set_text(s_ui.sensor_lbl, buf);
}

void UpdateUi(int16_t mx, int16_t my, int16_t mz, uint8_t st, bool sensor_ok,
              bool update_text)
{
    if (sensor_ok) {
        UpdateAxisRow(0, mx, update_text);
        UpdateAxisRow(1, my, update_text);
        UpdateAxisRow(2, mz, update_text);
    } else {
        for (int i = 0; i < 3; ++i) {
            UpdateBarFill(i, 0);
            if (update_text) {
                if (s_ui.axes[i].raw_lbl != nullptr) {
                    lv_label_set_text(s_ui.axes[i].raw_lbl, "-- LSB");
                }
                if (s_ui.axes[i].phys_lbl != nullptr) {
                    lv_label_set_text(s_ui.axes[i].phys_lbl, "-- G   -- uT");
                }
            }
        }
    }

    if (!update_text) {
        return;
    }

    if (s_ui.total_lbl != nullptr) {
        if (sensor_ok) {
            float mag_lsb = __builtin_sqrtf(static_cast<float>(mx) * mx +
                                            static_cast<float>(my) * my +
                                            static_cast<float>(mz) * mz);
            char buf[80];
            std::snprintf(buf, sizeof(buf), "|B| = %.0f LSB   %.3f G   %.1f uT",
                          mag_lsb, mag_lsb * kLsbToGauss,
                          mag_lsb * kLsbToMicroTesla);
            lv_label_set_text(s_ui.total_lbl, buf);
        } else {
            lv_label_set_text(s_ui.total_lbl, "|B| = --");
        }
    }

    if (s_ui.status_lbl != nullptr) {
        if (sensor_ok) {
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "STATUS=0x%02X  DRDY=%d  OVL=%d  FRAME=%lu", st,
                          (st & kQmcStatusDrdy) ? 1 : 0,
                          (st & kQmcStatusOvl) ? 1 : 0,
                          static_cast<unsigned long>(s_frame_cnt));
            lv_label_set_text(s_ui.status_lbl, buf);
        } else {
            lv_label_set_text(s_ui.status_lbl,
                              (st&kQmcStatusOvl)?"磁场过强 · 读数溢出":"等待有效磁场数据");
        }
    }
}

void OnSampleTick(lv_timer_t * /*t*/)
{
    s_ui.tick_n++;
    if (s_ui.tick_n <= kUiWarmTicks) {
        return;
    }
    Sample sample;
    {std::lock_guard<std::mutex> guard(s_sample_mutex);sample=s_sample;}
    const bool sensor_ok=sample.ok && uint32_t(NowMs()-sample.at)<500;
    const int16_t mx=sample.axis[0],my=sample.axis[1],mz=sample.axis[2];
    const uint8_t st=sample.status;
    // Shared scale keeps X/Y/Z comparable. Expand immediately, contract
    // gradually after the displayed values have caught up.
    if(sensor_ok) {
        int peak=std::max(std::abs(int(mx)),std::max(std::abs(int(my)),std::abs(int(mz))));
        for(const auto& row:s_ui.axes) peak=std::max(peak,int(std::abs(row.displayed)));
        int desired=1000;
        while(desired<peak+peak/4 && desired<32768) desired=std::min(32768,desired*2);
        if(desired>s_ui.range_lsb || (s_ui.tick_n%60==0 && desired<s_ui.range_lsb)) s_ui.range_lsb=desired;
    }
    if(s_ui.range_lbl && s_ui.tick_n%kTextEveryN==0) {
        char text[96];std::snprintf(text,sizeof(text),"-%.0f uT                 0                 +%.0f uT",s_ui.range_lsb*.1f,s_ui.range_lsb*.1f);
        lv_label_set_text(s_ui.range_lbl,text);
    }
    s_frame_cnt++;
    const bool update_text = ((s_ui.tick_n % kTextEveryN) == 0);
    if (update_text) {
        MaybeSetSensorLabel(sensor_ok);
    }
    UpdateUi(mx, my, mz, st, sensor_ok, update_text);
}

void OnSwipeBack()
{
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    HomeScreen::SwitchToHome();
}

void OnBackBtnClicked(lv_event_t * /*e*/)
{
    OnSwipeBack();
}

void OnScreenUnloaded(lv_event_t * /*e*/)
{
    if (s_ui.arm_timer != nullptr) {
        lv_timer_delete(s_ui.arm_timer);
        s_ui.arm_timer = nullptr;
    }
    if (s_ui.sample_timer != nullptr) {
        lv_timer_delete(s_ui.sample_timer);
        s_ui.sample_timer = nullptr;
    }
    StopSensorWorker();
    s_ui = UiState{};
    s_frame_cnt = 0;
}

void BuildHeader(lv_obj_t *parent)
{
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_remove_style_all(header);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0E1116), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int kBackBtnSize = 72;
    lv_obj_t *back = lv_obj_create(header);
    lv_obj_remove_style_all(back);
    screen_strip_obj_chrome(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, OnBackBtnClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("磁场"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    lv_obj_t *sensor_lbl = lv_label_create(header);
    s_ui.sensor_lbl = sensor_lbl;
    lv_label_set_text(sensor_lbl, "QMC6309  --");
    lv_obj_set_style_text_color(sensor_lbl, lv_color_hex(0x9AA3B2),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(sensor_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(sensor_lbl, LV_ALIGN_RIGHT_MID, -16, 0);
}

void BuildAxisCard(lv_obj_t *parent, int idx)
{
    int y = kCardTopY + idx * (kCardH + kCardGap);
    uint32_t color = AxisColor(idx);

    lv_obj_t *card = lv_obj_create(parent);
    s_ui.axes[idx].card = card;
    lv_obj_remove_style_all(card);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_set_pos(card, kCardLeft, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 0, LV_PART_MAIN);
    /* Opaque accent edge instead of translucent rounded border. */
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *accent = lv_obj_create(card);
    lv_obj_remove_style_all(accent);
    screen_strip_obj_chrome(accent);
    lv_obj_set_size(accent, 6, kCardH);
    lv_obj_set_pos(accent, 0, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(accent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(card);
    s_ui.axes[idx].title_lbl = title_lbl;
    lv_label_set_text(title_lbl, AxisTitle(idx));
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 24, 12);

    lv_obj_t *raw_lbl = lv_label_create(card);
    s_ui.axes[idx].raw_lbl = raw_lbl;
    lv_label_set_text(raw_lbl, "-- LSB");
    lv_obj_set_style_text_color(raw_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(raw_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(raw_lbl, LV_ALIGN_TOP_RIGHT, -24, 12);

    lv_obj_t *phys_lbl = lv_label_create(card);
    s_ui.axes[idx].phys_lbl = phys_lbl;
    lv_label_set_text(phys_lbl, "-- G   -- uT");
    lv_obj_set_style_text_color(phys_lbl, lv_color_hex(0xC7CDD9), LV_PART_MAIN);
    lv_obj_set_style_text_font(phys_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(phys_lbl, LV_ALIGN_LEFT_MID, 24, 10);

    lv_obj_t *track = lv_obj_create(card);
    s_ui.axes[idx].bar_track = track;
    lv_obj_remove_style_all(track);
    screen_strip_obj_chrome(track);
    lv_obj_set_size(track, kTrackW, kBarH);
    lv_obj_align(track, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x0B0F18), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(track, 0, LV_PART_MAIN);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *fill = lv_obj_create(track);
    s_ui.axes[idx].bar_fill = fill;
    lv_obj_remove_style_all(fill);
    screen_strip_obj_chrome(fill);
    lv_obj_set_size(fill, 2, kBarH);
    lv_obj_set_pos(fill, kTrackW / 2 - 1, 0);
    lv_obj_t *zero=lv_obj_create(track);
    lv_obj_remove_style_all(zero);
    lv_obj_set_size(zero,1,kBarH);lv_obj_set_pos(zero,kTrackW/2,0);
    lv_obj_set_style_bg_color(zero,lv_color_hex(0x9AA3B2),0);
    lv_obj_set_style_bg_opa(zero,LV_OPA_COVER,0);
    auto* cursor=lv_obj_create(track);s_ui.axes[idx].cursor=cursor;
    lv_obj_remove_style_all(cursor);
    lv_obj_set_size(cursor,4,kBarH);lv_obj_set_pos(cursor,kTrackW/2-2,0);
    lv_obj_set_style_bg_color(cursor,lv_color_white(),0);
    lv_obj_set_style_bg_opa(cursor,LV_OPA_COVER,0);
    lv_obj_remove_flag(cursor,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(zero,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(fill, 0, LV_PART_MAIN);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
}

void BuildSummary(lv_obj_t *parent)
{
    s_ui.range_lbl=lv_label_create(parent);
    lv_obj_set_style_text_font(s_ui.range_lbl,&font_puhui_20_4,0);
    lv_obj_set_style_text_color(s_ui.range_lbl,lv_color_hex(0x9AA3B2),0);
    lv_obj_set_width(s_ui.range_lbl,kPanelW);
    lv_obj_set_pos(s_ui.range_lbl,0,kSummaryTopY-14);
    lv_obj_set_style_text_align(s_ui.range_lbl,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_text(s_ui.range_lbl,"-100 uT                 0                 +100 uT");
    lv_obj_t *total = lv_label_create(parent);
    s_ui.total_lbl = total;
    lv_label_set_text(total, "|B| = --");
    lv_obj_set_style_text_color(total, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(total, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_pos(total, 0, kSummaryTopY + 18);
    lv_obj_set_width(total, kPanelW);
    lv_obj_set_style_text_align(total, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *status = lv_label_create(parent);
    s_ui.status_lbl = status;
    lv_label_set_text(status, "STATUS=--  FRAME=0");
    lv_obj_set_style_text_color(status, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_pos(status, 0, kSummaryTopY + 58);
    lv_obj_set_width(status, kPanelW);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

void ArmMagnetSensor(lv_timer_t *t)
{
    s_ui.arm_timer = nullptr;
    if (t != nullptr) {
        lv_timer_delete(t);
    }
    if (s_ui.screen == nullptr) {
        return;
    }
    write(1, "MAG_ARM\n", 8);
    StartSensorWorker();
    if (s_ui.sample_timer == nullptr) {
        s_ui.sample_timer =
            lv_timer_create(OnSampleTick, kSamplePeriodMs, nullptr);
    }
    write(1, "MAG_ARM_OK\n", 11);
    ESP_LOGI(TAG, "magnet armed");
}

}  // namespace

lv_obj_t *MagnetScreen::CreateStatic()
{
    write(1, "MAG_CR\n", 7);
    lv_obj_t *scr = lv_obj_create(nullptr);
    s_ui = UiState{};
    s_ui.screen = scr;
    lv_obj_remove_style_all(scr);
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1116), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1116),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(scr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    write(1, "MAG_H\n", 6);
    BuildAxisCard(scr, 0);
    BuildAxisCard(scr, 1);
    BuildAxisCard(scr, 2);
    write(1, "MAG_AX\n", 7);
    BuildSummary(scr);

    s_ui.arm_timer = lv_timer_create(ArmMagnetSensor, kArmDelayMs, nullptr);
    if (s_ui.arm_timer != nullptr) {
        lv_timer_set_repeat_count(s_ui.arm_timer, 1);
    }

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    write(1, "MAG_CR_OK\n", 10);
    return scr;
}

void MagnetScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: magnet_screen");
    } else {
        ESP_LOGI(TAG, "unload: magnet_screen");
    }
}

lv_obj_t *MagnetScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}
