#include "vibrate_motor_test.h"
#include "i18n.h"

#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "esp_log_shim.h"
#include "test_ui_common.h"

#include <nuttx/timers/pwm.h>

namespace {

constexpr const char* TAG = "VibrateMotorTest";
constexpr uint32_t kConfirmDelayMs = 1000;

constexpr uint32_t kLedcFreqHz  = 5000;
constexpr int      kOnDutyPct   = 60;

bool        s_pwm_ready    = false;
bool        s_motor_on     = false;
int         s_pwm_fd       = -1;
lv_obj_t*   s_status_icon  = nullptr;
lv_timer_t* s_confirm_timer = nullptr;

void StopConfirmTimer() {
    if (s_confirm_timer != nullptr) {
        lv_timer_delete(s_confirm_timer);
        s_confirm_timer = nullptr;
    }
}

void OnConfirmResult(bool pass, void* user_data) {
    TestUiUpdateStatus(static_cast<lv_obj_t*>(user_data), pass);
    ESP_LOGI(TAG, "user confirm vibrate motor: %s", pass ? "pass" : "fail");
}

void OnConfirmTimer(lv_timer_t* /*t*/) {
    s_confirm_timer = nullptr;
    TestUiShowConfirmDialog(I18n::T("是否正常震动？"), OnConfirmResult, s_status_icon);
}

void ScheduleConfirmDialog() {
    StopConfirmTimer();
    s_confirm_timer = lv_timer_create(OnConfirmTimer, kConfirmDelayMs, nullptr);
    lv_timer_set_repeat_count(s_confirm_timer, 1);
}

void PwmInitOnce() {
    if (s_pwm_ready) {
        return;
    }

    s_pwm_fd = open("/dev/pwm1", O_WRONLY);
    if (s_pwm_fd < 0) {
        ESP_LOGW(TAG, "open /dev/pwm1 failed: %d (vibrate motor unavailable)", errno);
        return;
    }

    s_pwm_ready = true;
    ESP_LOGI(TAG, "vibrate motor PWM ready on /dev/pwm1");
}

void ApplyDutyPct(int pct) {
    if (!s_pwm_ready || s_pwm_fd < 0) {
        return;
    }
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }

    struct pwm_info_s info;
    memset(&info, 0, sizeof(info));
    info.frequency = kLedcFreqHz;
    info.duty = (uint32_t)((pct * 65535u) / 100u);
    info.cpol = PWM_CPOL_NDEF;
    info.dcpol = PWM_DCPOL_NDEF;

    if (ioctl(s_pwm_fd, PWMIOC_SETCHARACTERISTICS, (unsigned long)&info) < 0) {
        ESP_LOGW(TAG, "SETCHARACTERISTICS failed: %d", errno);
        return;
    }

    if (pct > 0) {
        ioctl(s_pwm_fd, PWMIOC_START, 0);
    } else {
        ioctl(s_pwm_fd, PWMIOC_STOP, 0);
    }
}

void OnSwitchChanged(lv_event_t* e) {
    lv_obj_t* sw = lv_event_get_target_obj(e);
    s_motor_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ApplyDutyPct(s_motor_on ? kOnDutyPct : 0);
    ESP_LOGI(TAG, "vibrate motor %s", s_motor_on ? "ON" : "OFF");

    if (s_motor_on) {
        ScheduleConfirmDialog();
    } else {
        StopConfirmTimer();
    }
}

}  // namespace

namespace VibrateMotorTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, I18n::T("震动马达"), &s_status_icon, &ctrl);
    TestUiCreateSwitch(ctrl, OnSwitchChanged, nullptr);
}

void OnLoad() {
    PwmInitOnce();
    ApplyDutyPct(0);
    s_motor_on = false;
}

void OnUnload() {
    StopConfirmTimer();
    StopMotor();
    if (s_pwm_fd >= 0) {
        close(s_pwm_fd);
        s_pwm_fd = -1;
    }
    s_pwm_ready = false;
    s_status_icon = nullptr;
}

void StartMotor() {
    PwmInitOnce();
    s_motor_on = true;
    ApplyDutyPct(kOnDutyPct);
    ESP_LOGI(TAG, "vibrate motor ON (stress)");
}

void StopMotor() {
    s_motor_on = false;
    ApplyDutyPct(0);
}

}  // namespace VibrateMotorTest
