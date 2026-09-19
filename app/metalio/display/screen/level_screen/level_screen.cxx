/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * LevelScreen — ported 1:1 from MetalioClaw4 level_screen.cc
 */

#include "level_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "home_screen/home_screen.h"
#include "settings.h"
#include <metalio/metalio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);


namespace level_detail {

constexpr const char* TAG = "LevelScreen";

// ---------------------------------------------------------------------------
// SC7A20H accelerometer (I2C 0x19) -- LIS2DH12-compatible register map.
//
// 上电后默认是 power-down，必须显式写 CTRL_REG1 才会出数据。我们用 ±2g
// 高分辨率模式（HR=1，BDU=1），输出 12-bit 左对齐到 16-bit 寄存器，灵敏度
// 1 mg/LSB。读 6 字节连续输出寄存器时必须把寄存器地址的 MSB 置 1 触发
// 自增，否则只会反复读到 OUT_X_L。
// ---------------------------------------------------------------------------
constexpr uint8_t kSc7a20hAddr  = 0x19;
constexpr uint8_t kRegWhoAmI    = 0x0F;
constexpr uint8_t kRegCtrlReg1  = 0x20;
constexpr uint8_t kRegCtrlReg4  = 0x23;
constexpr uint8_t kRegOutXL     = 0x28;
constexpr uint8_t kAutoIncMask  = 0x80;

// CTRL_REG1 = 0x57 -> ODR=100Hz, LPen=0 (HR-capable), Z/Y/X enable.
constexpr uint8_t kCtrlReg1Val  = 0x57;
// CTRL_REG4 = 0x88 -> BDU=1, BLE=0, FS=00 (±2g), HR=1, ST=00, SIM=0.
constexpr uint8_t kCtrlReg4Val  = 0x88;
// 在 ±2g + HR(12-bit) 模式下，1 LSB ≈ 1 mg。
constexpr float   kMgPerLsb     = 1.0f;
constexpr int     kGravityMg    = 1000;

// SC7A20H 跟 ST 家系列共用 0x33 / 0x32 / 0x11 等 WHO_AM_I；不强校验，只要能
// 读得到东西就当成在线。把已知值列出来仅用于日志，便于排查。
constexpr uint8_t kWhoAmIExpected[] = {0x11, 0x33, 0x32, 0x44};

// ---------------------------------------------------------------------------
// 几何 / 视觉常量（720x720 面板）
// ---------------------------------------------------------------------------
constexpr int kPanelW          = 720;
constexpr int kPanelH          = 720;
constexpr int kHeaderH         = 90;
constexpr int kFooterTopY      = 540;
constexpr int kFooterH         = kPanelH - kFooterTopY;

constexpr int kCenterX         = kPanelW / 2;     // 360
constexpr int kCenterY         = (kHeaderH + kFooterTopY) / 2;  // 315
constexpr int kOuterRadius     = 210;             // 外参考圈
constexpr int kMidRadius       = 110;             // 第二圈刻度
constexpr int kTargetRadius    = 30;              // 中心目标小圈
constexpr int kBubbleRadius    = 26;              // 气泡半径
constexpr int kBubbleMaxOffset = kOuterRadius - kBubbleRadius - 6;

// 判定 “水平” 的阈值，单位：度。两轴 |角度| 都小于这个值则视为水平。
constexpr float kLevelTolDeg   = 0.5f;

/* No LV_RADIUS_CIRCLE / canvas / RGB565 dial image — those flush solid-blue
 * on this NuttX port when a press invalidates. Opaque square frames only. */
constexpr uint32_t kSamplePeriodMs = 100;
constexpr uint32_t kSensorPeriodUs = 50000;
constexpr uint32_t kArmDelayMs     = 600;
constexpr uint32_t kTextEveryN     = 2;
constexpr uint32_t kUiWarmTicks    = 4;
constexpr float    kBubbleSmoothing = 0.25f;
constexpr int      kBubbleMinDelta = 3;
constexpr uint32_t kBubbleMinMs    = 100;
constexpr int      kDialSize       = kOuterRadius * 2;
constexpr uint32_t kScreenBg       = 0x0E1116;
constexpr uint32_t kDialFill       = 0x101723;
constexpr uint32_t kOuterRing      = 0x3B82F6;
constexpr uint32_t kMidRing        = 0x60A5FA;
constexpr uint32_t kTargetRing     = 0xF87171;
constexpr uint32_t kBubbleCyan     = 0x3DDCFF;
constexpr uint32_t kBubbleGreen    = 0x22C55E;

// ---------------------------------------------------------------------------
// SC7A20H via board bringup (metalio_sc7a20h_read_mg) — worker thread only
// ---------------------------------------------------------------------------
struct CalOffsets {
    int ox = 0;     // ax 轴在水平静止时的零点偏移 (mg)
    int oy = 0;
    int oz = 0;     // az 轴减去 1g(1000mg) 之后的偏移
};

bool       s_sensor_init = false;     // probe + configure 是否成功
CalOffsets s_cal;
bool       s_cal_loaded  = false;

std::atomic<bool> s_worker_run{false};
std::atomic<bool> s_sample_ok{false};
std::atomic<int>  s_raw_x{0};
std::atomic<int>  s_raw_y{0};
std::atomic<int>  s_raw_z{kGravityMg};
pthread_t         s_worker_tid{};
bool              s_worker_joined = true;
bool              s_sensor_lbl_set = false;
std::atomic<bool> s_cal_request{false};

void *PersistCalWorker(void * /*arg*/)
{
    Settings settings("level", true);
    settings.SetInt("ox", s_cal.ox);
    settings.SetInt("oy", s_cal.oy);
    settings.SetInt("oz", s_cal.oz);
    ESP_LOGI(TAG, "persist cal: ox=%d oy=%d oz=%d", s_cal.ox, s_cal.oy, s_cal.oz);
    write(1, "LVL_CAL_SAVE\n", 13);
    return nullptr;
}

void SchedulePersistCal()
{
    pthread_t tid{};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4096);
    if (pthread_create(&tid, &attr, PersistCalWorker, nullptr) != 0) {
        write(1, "LVL_CAL_SAVE_F\n", 15);
    }
    pthread_attr_destroy(&attr);
}

void LoadCalOffsetsOnce() {
    if (s_cal_loaded) return;
    Settings settings("level", false);
    s_cal.ox = settings.GetInt("ox", 0);
    s_cal.oy = settings.GetInt("oy", 0);
    s_cal.oz = settings.GetInt("oz", 0);
    s_cal_loaded = true;
    ESP_LOGI(TAG, "load cal: ox=%d oy=%d oz=%d", s_cal.ox, s_cal.oy, s_cal.oz);
}

void SaveCalOffsets(const CalOffsets& c) {
    s_cal = c;
    s_cal_loaded = true;
    ESP_LOGI(TAG, "cal offsets (RAM): ox=%d oy=%d oz=%d", c.ox, c.oy, c.oz);
    SchedulePersistCal();
}

void ResetCalOffsets() {
    s_cal = {0, 0, 0};
    s_cal_loaded = true;
    ESP_LOGI(TAG, "cal offsets reset (RAM)");
    SchedulePersistCal();
}

void *SensorWorker(void * /*arg*/)
{
    write(1, "LVL_WRK\n", 8);
    while (s_worker_run.load(std::memory_order_relaxed)) {
        int ax = 0, ay = 0, az = 0;
        if (metalio_sc7a20h_read_mg(&ax, &ay, &az) == 0) {
            s_raw_x.store(ax, std::memory_order_relaxed);
            s_raw_y.store(ay, std::memory_order_relaxed);
            s_raw_z.store(az, std::memory_order_relaxed);
            s_sample_ok.store(true, std::memory_order_relaxed);
            s_sensor_init = true;
        }
        usleep(kSensorPeriodUs);
    }
    write(1, "LVL_WRK_END\n", 12);
    return nullptr;
}

void StopSensorWorker()
{
    if (s_worker_joined) {
        return;
    }
    s_worker_run.store(false, std::memory_order_relaxed);
    pthread_join(s_worker_tid, nullptr);
    s_worker_joined = true;
    write(1, "LVL_WRK_JOIN\n", 13);
}

void StartSensorWorker()
{
    StopSensorWorker();
    s_sample_ok.store(false, std::memory_order_relaxed);
    s_worker_run.store(true, std::memory_order_relaxed);
    s_worker_joined = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);
    if (pthread_create(&s_worker_tid, &attr, SensorWorker, nullptr) != 0) {
        pthread_attr_destroy(&attr);
        s_worker_run.store(false, std::memory_order_relaxed);
        s_worker_joined = true;
        write(1, "LVL_WRK_FAIL\n", 13);
        return;
    }
    pthread_attr_destroy(&attr);
    write(1, "LVL_WRK_OK\n", 11);
}

// ---------------------------------------------------------------------------
// UI 状态
// ---------------------------------------------------------------------------
struct UiState {
    lv_obj_t* screen        = nullptr;
    lv_obj_t* bubble        = nullptr;
    lv_obj_t* status_lbl    = nullptr;
    lv_obj_t* xyz_lbl       = nullptr;
    lv_obj_t* angle_lbl     = nullptr;
    lv_obj_t* sensor_lbl    = nullptr;
    lv_obj_t* cal_btn       = nullptr;
    lv_obj_t* reset_btn     = nullptr;
    lv_obj_t* reset_cover   = nullptr;

    lv_timer_t* sample_timer = nullptr;
    lv_timer_t* dialog_timer = nullptr;
    lv_timer_t* arm_timer    = nullptr;

    float bubble_x_px = 0.0f;
    float bubble_y_px = 0.0f;
    int32_t bubble_px_x = 0;
    int32_t bubble_px_y = 0;
    uint32_t bubble_color = 0;
    uint32_t last_bubble_ms = 0;
    uint32_t tick_n = 0;
    uint32_t ui_freeze_until_ms = 0;
    int status_kind = -1;
    bool cal_busy = false;
};

UiState s_ui;

void FreeDialAssets() {}

// 校准对话框的状态机：IDLE -> CONFIRM(等用户点确认) -> SAMPLING(取样中) -> DONE
enum class CalState : uint8_t {
    kIdle      = 0,
    kConfirm   = 1,   // 已弹出，等待用户点击“开始校准”
    kSampling  = 2,   // 正在取样累加
    kDone      = 3,   // 取样完成，显示 “校准完成” 一会儿后关闭
};

CalState s_cal_state = CalState::kIdle;
int      s_cal_count = 0;     // 累计采样次数
int64_t  s_cal_sum_x = 0;     // 累加和（整数 mg），32 位足够
int64_t  s_cal_sum_y = 0;
int64_t  s_cal_sum_z = 0;
constexpr int kCalSamples = 32;

// ---------------------------------------------------------------------------
// 工具：取标度后的传感器读数（带偏移修正）
//
// 返回一次 “世界坐标” 加速度（单位 mg）。如果传感器未在线，把 az 设为
// 1000、ax/ay 设为 0，让 UI 维持在“水平”态而不是乱跳。
//
// raw_* 输出原始未校准的 mg 值，校准流程 (OnSampleTick / kSampling) 需要
// 它来累加；UI 渲染只关心已校准的 ax/ay/az。
// ---------------------------------------------------------------------------
bool ReadCorrectedAccel(int* ax, int* ay, int* az,
                        int* raw_x = nullptr, int* raw_y = nullptr, int* raw_z = nullptr) {
    if (!s_sample_ok.load(std::memory_order_relaxed)) {
        *ax = 0;
        *ay = 0;
        *az = kGravityMg;
        if (raw_x != nullptr) *raw_x = 0;
        if (raw_y != nullptr) *raw_y = 0;
        if (raw_z != nullptr) *raw_z = kGravityMg;
        return false;
    }
    const int rx = s_raw_x.load(std::memory_order_relaxed);
    const int ry = s_raw_y.load(std::memory_order_relaxed);
    const int rz = s_raw_z.load(std::memory_order_relaxed);
    *ax = rx - s_cal.ox;
    *ay = ry - s_cal.oy;
    *az = rz - s_cal.oz;
    if (raw_x != nullptr) *raw_x = rx;
    if (raw_y != nullptr) *raw_y = ry;
    if (raw_z != nullptr) *raw_z = rz;
    return true;
}

// ---------------------------------------------------------------------------
// UI 同步：与 MetalioClaw4 level_screen.cc 一致
// ---------------------------------------------------------------------------
void UpdateUiFromAccel(int ax, int ay, int az, bool update_text) {
    /* Modal open or post-reset peel: skip bubble+glyph churn. */
    if (s_ui.cal_busy) {
        return;
    }
    if (s_ui.ui_freeze_until_ms != 0 &&
        lv_tick_get() < s_ui.ui_freeze_until_ms) {
        return;
    }
    s_ui.ui_freeze_until_ms = 0;

    float ax_g = static_cast<float>(ax) / kGravityMg;
    float ay_g = static_cast<float>(ay) / kGravityMg;
    float az_g = static_cast<float>(az) / kGravityMg;

    float pitch_deg = std::atan2(ax_g, std::sqrt(ay_g * ay_g + az_g * az_g)) * 180.0f /
                      static_cast<float>(M_PI);
    float roll_deg  = std::atan2(ay_g, std::sqrt(ax_g * ax_g + az_g * az_g)) * 180.0f /
                      static_cast<float>(M_PI);

    float target_x = -ax_g * kBubbleMaxOffset;
    float target_y =  ay_g * kBubbleMaxOffset;

    float r = std::sqrt(target_x * target_x + target_y * target_y);
    if (r > kBubbleMaxOffset) {
        float k = kBubbleMaxOffset / r;
        target_x *= k;
        target_y *= k;
    }

    s_ui.bubble_x_px += (target_x - s_ui.bubble_x_px) * kBubbleSmoothing;
    s_ui.bubble_y_px += (target_y - s_ui.bubble_y_px) * kBubbleSmoothing;

    const int32_t px = static_cast<int32_t>(s_ui.bubble_x_px);
    const int32_t py = static_cast<int32_t>(s_ui.bubble_y_px);
    const bool level = (std::fabs(pitch_deg) < kLevelTolDeg) &&
                       (std::fabs(roll_deg) < kLevelTolDeg) && s_sensor_init;
    const uint32_t color = level ? 0x22C55E : 0x3DDCFF;
    const int ddx = px - s_ui.bubble_px_x;
    const int ddy = py - s_ui.bubble_px_y;
    const bool moved =
        (ddx * ddx + ddy * ddy) >= (kBubbleMinDelta * kBubbleMinDelta);
    const bool recolor = (color != s_ui.bubble_color);
    const uint32_t now = lv_tick_get();
    const bool rate_ok =
        (now - s_ui.last_bubble_ms) >= kBubbleMinMs || s_ui.last_bubble_ms == 0;

    if (s_ui.bubble != nullptr && (moved || recolor) && rate_ok) {
        s_ui.bubble_px_x = px;
        s_ui.bubble_px_y = py;
        lv_obj_set_pos(s_ui.bubble, kCenterX + px - kBubbleRadius,
                       kCenterY + py - kBubbleRadius);
        if (recolor) {
            s_ui.bubble_color = color;
            lv_obj_set_style_bg_color(s_ui.bubble, lv_color_hex(color), LV_PART_MAIN);
        }
        s_ui.last_bubble_ms = now;
        static uint32_t s_mv;
        if ((++s_mv % 5u) == 1u) {
            write(1, "LVL_MV\n", 7);
        }
    }

    if (moved) {
        update_text = false;
    }

    if (!update_text) {
        return;
    }

    if (s_ui.status_lbl != nullptr) {
        int kind;
        if (!s_sensor_init) {
            kind = 0;
        } else if (std::fabs(pitch_deg) < kLevelTolDeg &&
                   std::fabs(roll_deg) < kLevelTolDeg) {
            kind = 1;
        } else {
            kind = 2;
        }
        if (kind != s_ui.status_kind || kind == 2) {
            s_ui.status_kind = kind;
            if (kind == 0) {
                lv_label_set_text(s_ui.status_lbl, I18n::T("未检测到传感器"));
                lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(0xF87171),
                                            LV_PART_MAIN);
            } else if (kind == 1) {
                lv_label_set_text(s_ui.status_lbl, I18n::T("水平"));
                lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(0x22C55E),
                                            LV_PART_MAIN);
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), I18n::T("未水平  %.1f°"),
                              std::sqrt(pitch_deg * pitch_deg + roll_deg * roll_deg));
                lv_label_set_text(s_ui.status_lbl, buf);
                lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(0xFBBF24),
                                            LV_PART_MAIN);
            }
        }
    }

    if (s_ui.xyz_lbl != nullptr) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "X: %5d  Y: %5d  Z: %5d  (mg)", ax, ay, az);
        lv_label_set_text(s_ui.xyz_lbl, buf);
    }

    if (s_ui.angle_lbl != nullptr) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "Pitch: %+5.1f°   Roll: %+5.1f°", pitch_deg, roll_deg);
        lv_label_set_text(s_ui.angle_lbl, buf);
    }
}

// ---------------------------------------------------------------------------
// 主采样 timer：只读 worker 缓存并刷新 UI。
// ---------------------------------------------------------------------------
void OnSampleTick(lv_timer_t* /*t*/) {
    if (s_ui.screen == nullptr) {
        return;
    }

    const uint32_t n = ++s_ui.tick_n;
    if (n == 1) {
        write(1, "LVL_T1\n", 7);
    } else if (n == 5) {
        write(1, "LVL_T5\n", 7);
    } else if (n == 10) {
        write(1, "LVL_T10\n", 8);
    } else if ((n % 20) == 0) {
        write(1, "LVL_TK\n", 7);
    }

    int ax = 0, ay = 0, az = kGravityMg;
    int rx = 0, ry = 0, rz = kGravityMg;
    bool ok = ReadCorrectedAccel(&ax, &ay, &az, &rx, &ry, &rz);

    if (s_cal_request.exchange(false) && s_cal_state == CalState::kIdle &&
        !s_ui.cal_busy) {
        write(1, "LVL_CAL_RUN\n", 12);
        s_ui.cal_busy = true;
        s_cal_state = CalState::kSampling;
        s_cal_count = 0;
        s_cal_sum_x = s_cal_sum_y = s_cal_sum_z = 0;
    }

    if (s_cal_state == CalState::kSampling && ok) {
        s_cal_sum_x += rx;
        s_cal_sum_y += ry;
        s_cal_sum_z += rz;
        s_cal_count++;
        if ((s_cal_count % 8) == 0) {
            write(1, "LVL_CAL_P\n", 10);
        }
        if (s_cal_count >= kCalSamples) {
            CalOffsets c;
            c.ox = static_cast<int>(s_cal_sum_x / s_cal_count);
            c.oy = static_cast<int>(s_cal_sum_y / s_cal_count);
            c.oz = static_cast<int>(s_cal_sum_z / s_cal_count) - kGravityMg;
            SaveCalOffsets(c); /* RAM + detached pthread file write */
            s_cal_state = CalState::kIdle;
            s_cal_count = 0;
            s_cal_sum_x = s_cal_sum_y = s_cal_sum_z = 0;
            s_ui.cal_busy = false;
            write(1, "LVL_CAL_DONE\n", 13);
            write(1, "LVL_CAL_END\n", 12);
        }
        return; /* zero UI during sampling */
    }

    if (n <= kUiWarmTicks) {
        return;
    }

    if (ok && s_ui.sensor_lbl != nullptr && s_sensor_init && !s_sensor_lbl_set) {
        char buf[32];
        uint8_t who = 0;
        if (metalio_sc7a20h_whoami(&who) == 0) {
            std::snprintf(buf, sizeof(buf), "SC7A20H  ID 0x%02X", who);
        } else {
            std::snprintf(buf, sizeof(buf), "SC7A20H  ID --");
        }
        lv_label_set_text(s_ui.sensor_lbl, buf);
        lv_obj_set_style_text_color(s_ui.sensor_lbl, lv_color_hex(0x9AA3B2),
                                    LV_PART_MAIN);
        s_sensor_lbl_set = true;
        write(1, "LVL_SENS\n", 9);
    }

    if (n == kUiWarmTicks + 1) {
        write(1, "LVL_UI\n", 7);
    }

    const bool update_text = ((n % kTextEveryN) == 0);
    UpdateUiFromAccel(ax, ay, az, update_text);

    if (n == kUiWarmTicks + 1) {
        write(1, "LVL_UI_OK\n", 10);
    }
}

// ---------------------------------------------------------------------------
// 校准：静默取样。不弹窗、不遮罩、不隐藏表盘、不改中文标签（按压路径零 UI）。
// ---------------------------------------------------------------------------
void OnCalibrateClicked(lv_event_t* /*e*/) {
    write(1, "LVL_CAL_TAP\n", 12);
    /* Flag only — any LVGL mutate on this press path solid-blues. */
    s_cal_request.store(true, std::memory_order_relaxed);
}

void ShowResetCover()
{
    if (s_ui.screen == nullptr) {
        return;
    }
    if (s_ui.reset_cover == nullptr) {
        lv_obj_t *cover = lv_obj_create(s_ui.screen);
        s_ui.reset_cover = cover;
        screen_strip_obj_chrome(cover);
        lv_obj_add_flag(cover, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(cover, kPanelW, kPanelH);
        lv_obj_set_pos(cover, 0, 0);
        lv_obj_set_style_bg_color(cover, lv_color_hex(0x0E1116), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(cover, 0, LV_PART_MAIN);
        lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);
        screen_swipe_back_ignore(cover, true);
    }
    lv_obj_remove_flag(s_ui.reset_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui.reset_cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_ui.reset_cover);
}

void OnResetCoverPeel(lv_timer_t * /*t*/)
{
    if (s_ui.reset_cover != nullptr) {
        lv_obj_add_flag(s_ui.reset_cover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.reset_cover, LV_OBJ_FLAG_CLICKABLE);
    }
    s_ui.ui_freeze_until_ms = 0;
    /* After peel — Claw4 reset feedback without press-path glyph blue. */
    if (s_ui.status_lbl != nullptr) {
        lv_label_set_text(s_ui.status_lbl, I18n::T("已重置校准"));
        lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(0xFBBF24),
                                    LV_PART_MAIN);
        s_ui.status_kind = -1;
    }
    SchedulePersistCal();
    write(1, "LVL_RST_OK\n", 11);
}

void DoResetCalAsync(void * /*user*/)
{
    write(1, "LVL_RST\n", 8);
    ShowResetCover();
    ResetCalOffsets();
    /* No Chinese label rewrite — glyph invalidate under press blues this port. */
    s_ui.status_kind = -1;
    s_ui.ui_freeze_until_ms = lv_tick_get() + 600;
    lv_timer_t *t = lv_timer_create(OnResetCoverPeel, 350, nullptr);
    if (t != nullptr) {
        lv_timer_set_repeat_count(t, 1);
    }
}

void OnResetCalClicked(lv_event_t* /*e*/) {
    write(1, "LVL_RST_TAP\n", 12);
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    if (lv_async_call(DoResetCalAsync, nullptr) != LV_RESULT_OK) {
        DoResetCalAsync(nullptr);
    }
}

// ---------------------------------------------------------------------------
// 屏幕导航
// ---------------------------------------------------------------------------
void OnSwipeBack() {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    HomeScreen::SwitchToHome();
}

void OnBackBtnClicked(lv_event_t* /*e*/) { OnSwipeBack(); }

void OnScreenUnloaded(lv_event_t* /*e*/) {
    if (s_ui.arm_timer != nullptr) {
        lv_timer_delete(s_ui.arm_timer);
        s_ui.arm_timer = nullptr;
    }
    if (s_ui.sample_timer != nullptr) {
        lv_timer_delete(s_ui.sample_timer);
        s_ui.sample_timer = nullptr;
    }
    if (s_ui.dialog_timer != nullptr) {
        lv_timer_delete(s_ui.dialog_timer);
        s_ui.dialog_timer = nullptr;
    }
    StopSensorWorker();
    FreeDialAssets();
    s_ui = UiState{};
    s_cal_state = CalState::kIdle;
    s_sample_ok.store(false, std::memory_order_relaxed);
    s_sensor_lbl_set = false;
}

void ArmLevelSensor(lv_timer_t *t)
{
    s_ui.arm_timer = nullptr;
    if (t != nullptr) {
        lv_timer_delete(t);
    }
    if (s_ui.screen == nullptr) {
        return;
    }
    write(1, "LVL_ARM\n", 8);
    StartSensorWorker();
    if (s_ui.sample_timer == nullptr) {
        s_ui.sample_timer =
            lv_timer_create(OnSampleTick, kSamplePeriodMs, nullptr);
    }
    write(1, "LVL_ARM_OK\n", 11);
}

// ---------------------------------------------------------------------------
// UI 构建子函数
// ---------------------------------------------------------------------------
void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // 左上角返回按钮：透明圆形按钮 + "←" 图标，按下时白色半透明叠加
    constexpr int kBackBtnSize = 72;
    lv_obj_t* back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackBtnClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("水平仪"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    lv_obj_t* sensor_lbl = lv_label_create(header);
    s_ui.sensor_lbl = sensor_lbl;
    /* ID filled after warm ticks — avoid I2C+glyph on create path. */
    lv_label_set_text(sensor_lbl, "SC7A20H  --");
    lv_obj_set_style_text_color(sensor_lbl, lv_color_hex(0xF87171), LV_PART_MAIN);
    lv_obj_set_style_text_font(sensor_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(sensor_lbl, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16 + 130, 0);
}

void BuildLevelGraphic(lv_obj_t* parent) {
    /* Opaque square frames only — MagnetScreen-style. No image / circle radius. */
    constexpr int kDialX = kCenterX - kOuterRadius;
    constexpr int kDialY = kCenterY - kOuterRadius;

    lv_obj_t *outer = lv_obj_create(parent);
    lv_obj_remove_style_all(outer);
    lv_obj_set_size(outer, kOuterRadius * 2, kOuterRadius * 2);
    lv_obj_set_pos(outer, kDialX, kDialY);
    lv_obj_set_style_bg_color(outer, lv_color_hex(0x101723), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(outer, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(outer, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_radius(outer, 0, LV_PART_MAIN);
    lv_obj_remove_flag(outer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(outer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *mid = lv_obj_create(parent);
    lv_obj_remove_style_all(mid);
    lv_obj_set_size(mid, kMidRadius * 2, kMidRadius * 2);
    lv_obj_set_pos(mid, kCenterX - kMidRadius, kCenterY - kMidRadius);
    lv_obj_set_style_bg_color(mid, lv_color_hex(0x101723), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mid, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(mid, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(mid, lv_color_hex(0x60A5FA), LV_PART_MAIN);
    lv_obj_set_style_radius(mid, 0, LV_PART_MAIN);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hl = lv_obj_create(parent);
    lv_obj_remove_style_all(hl);
    lv_obj_set_size(hl, kOuterRadius * 2 - 20, 2);
    lv_obj_set_pos(hl, kDialX + 10, kCenterY - 1);
    lv_obj_set_style_bg_color(hl, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(hl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *vl = lv_obj_create(parent);
    lv_obj_remove_style_all(vl);
    lv_obj_set_size(vl, 2, kOuterRadius * 2 - 20);
    lv_obj_set_pos(vl, kCenterX - 1, kDialY + 10);
    lv_obj_set_style_bg_color(vl, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(vl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *target = lv_obj_create(parent);
    lv_obj_remove_style_all(target);
    lv_obj_set_size(target, kTargetRadius * 2, kTargetRadius * 2);
    lv_obj_set_pos(target, kCenterX - kTargetRadius, kCenterY - kTargetRadius);
    lv_obj_set_style_bg_color(target, lv_color_hex(0x101723), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(target, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(target, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(target, lv_color_hex(0xF87171), LV_PART_MAIN);
    lv_obj_set_style_radius(target, 0, LV_PART_MAIN);
    lv_obj_remove_flag(target, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(target, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *bubble = lv_obj_create(parent);
    s_ui.bubble = bubble;
    lv_obj_remove_style_all(bubble);
    lv_obj_set_size(bubble, kBubbleRadius * 2, kBubbleRadius * 2);
    lv_obj_set_pos(bubble, kCenterX - kBubbleRadius, kCenterY - kBubbleRadius);
    lv_obj_set_style_radius(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(kBubbleCyan), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    s_ui.bubble_color = kBubbleCyan;
    s_ui.bubble_px_x = 0;
    s_ui.bubble_px_y = 0;
    write(1, "LVL_DIAL_OK\n", 12);
    write(1, "LVL_BUB_OK\n", 11);
}

void BuildFooter(lv_obj_t* parent) {
    // 状态大字（“水平” / “未水平 X°”）
    lv_obj_t* status = lv_label_create(parent);
    s_ui.status_lbl = status;
    lv_label_set_text(status, I18n::T("等待数据..."));
    lv_obj_set_style_text_color(status, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_pos(status, 0, kFooterTopY - 2);
    lv_obj_set_width(status, kPanelW);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // pitch / roll 中字
    lv_obj_t* angle = lv_label_create(parent);
    s_ui.angle_lbl = angle;
    lv_label_set_text(angle, "Pitch: --   Roll: --");
    lv_obj_set_style_text_color(angle, lv_color_hex(0xC7CDD9), LV_PART_MAIN);
    lv_obj_set_style_text_font(angle, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_pos(angle, 0, kFooterTopY + 44);
    lv_obj_set_width(angle, kPanelW);
    lv_obj_set_style_text_align(angle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // raw mg 三轴
    lv_obj_t* xyz = lv_label_create(parent);
    s_ui.xyz_lbl = xyz;
    lv_label_set_text(xyz, "X: --   Y: --   Z: --   (mg)");
    lv_obj_set_style_text_color(xyz, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(xyz, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_pos(xyz, 0, kFooterTopY + 70);
    lv_obj_set_width(xyz, kPanelW);
    lv_obj_set_style_text_align(xyz, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // ---- 底部两个操作按钮 ----
    constexpr int kBtnW   = 280;
    constexpr int kBtnH   = 70;
    constexpr int kBtnY   = kPanelH - kBtnH - 12;
    constexpr int kBtnGap = 24;
    constexpr int kBtnTotalW = kBtnW * 2 + kBtnGap;
    constexpr int kBtnStartX = (kPanelW - kBtnTotalW) / 2;

    /* Visual is not clickable — press on Chinese glyphs solid-blues this port.
     * Empty hit overlay sits on top and only sets a flag. */
    lv_obj_t* cal = lv_obj_create(parent);
    s_ui.cal_btn = cal;
    lv_obj_remove_style_all(cal);
    lv_obj_set_size(cal, kBtnW, kBtnH);
    lv_obj_set_pos(cal, kBtnStartX, kBtnY);
    lv_obj_set_style_radius(cal, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cal, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cal, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(cal, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cal, 0, LV_PART_MAIN);
    lv_obj_remove_flag(cal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* cal_lbl = lv_label_create(cal);
    lv_label_set_text(cal_lbl, I18n::T("校准"));
    lv_obj_set_style_text_color(cal_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(cal_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(cal_lbl);
    lv_obj_remove_flag(cal_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* cal_hit = lv_obj_create(parent);
    lv_obj_remove_style_all(cal_hit);
    lv_obj_set_size(cal_hit, kBtnW, kBtnH);
    lv_obj_set_pos(cal_hit, kBtnStartX, kBtnY);
    lv_obj_set_style_bg_opa(cal_hit, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(cal_hit, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cal_hit, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cal_hit, 0, LV_PART_MAIN);
    lv_obj_add_flag(cal_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cal_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cal_hit, OnCalibrateClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(cal_hit, true);

    lv_obj_t* reset = lv_obj_create(parent);
    s_ui.reset_btn = reset;
    lv_obj_remove_style_all(reset);
    lv_obj_set_size(reset, kBtnW, kBtnH);
    lv_obj_set_pos(reset, kBtnStartX + kBtnW + kBtnGap, kBtnY);
    lv_obj_set_style_radius(reset, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(reset, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(reset, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(reset, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(reset, lv_color_hex(0x3A4050), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(reset, 0, LV_PART_MAIN);
    lv_obj_add_flag(reset, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(reset, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(reset, OnResetCalClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(reset, true);
    lv_obj_t* reset_lbl = lv_label_create(reset);
    lv_label_set_text(reset_lbl, I18n::T("重置校准"));
    lv_obj_set_style_text_color(reset_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(reset_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(reset_lbl);
    lv_obj_remove_flag(reset_lbl, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t *CreateStaticImpl()
{
    write(1, "LVL_CR\n", 7);
    lv_obj_t *scr = lv_obj_create(nullptr);
    s_ui = UiState{};
    s_ui.screen = scr;
    /* Same as HomeScreen — theme primary blue paints any unstripped screen. */
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

    LoadCalOffsetsOnce();
    BuildHeader(scr);
    BuildLevelGraphic(scr);
    BuildFooter(scr);
    s_ui.bubble_x_px = 0.0f;
    s_ui.bubble_y_px = 0.0f;
    /* I2C + sample after first paint — not inside APP_CREATE tick. */
    s_ui.arm_timer = lv_timer_create(ArmLevelSensor, kArmDelayMs, nullptr);
    if (s_ui.arm_timer != nullptr) {
        lv_timer_set_repeat_count(s_ui.arm_timer, 1);
    }

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    write(1, "LVL_CR_OK\n", 10);
    return scr;
}

}  // namespace level_detail

lv_obj_t *LevelScreen::CreateStatic()
{
    return level_detail::CreateStaticImpl();
}

void LevelScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI("LevelScreen", "load: level_screen");
        /* Claw4: reload cal on each LOAD (no-op after first). */
        level_detail::LoadCalOffsetsOnce();
    } else {
        ESP_LOGI("LevelScreen", "unload: level_screen");
    }
}

lv_obj_t *LevelScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}
