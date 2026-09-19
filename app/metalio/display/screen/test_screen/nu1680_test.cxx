#include "nu1680_test.h"
#include "i18n.h"

#include "esp_log_shim.h"
#include "test_ui_common.h"
#include <metalio/metalio.h>

namespace {

constexpr const char* TAG = "Nu1680Test";

lv_obj_t* s_value_lbl    = nullptr;
lv_obj_t* s_status_icon  = nullptr;
bool      s_pass_latched = false;

void SetErrorText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorError),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, false);
}

void SetPassText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, true);
}

}  // namespace

namespace Nu1680Test {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "NU1680", &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, I18n::T("检测中..."));
}

void OnLoad() {
    s_pass_latched = false;
    Poll();
}

void OnUnload() {
    s_value_lbl    = nullptr;
    s_status_icon  = nullptr;
    s_pass_latched = false;
}

void Poll() {
    if (s_value_lbl == nullptr || s_pass_latched) {
        return;
    }

    if (metalio_nu1680_probe() < 0) {
        // 未放上无线充时地址不可见，持续轮询，放上后即锁定通过。
        SetErrorText(I18n::T("未检测到(需无线充)"));
        return;
    }

    s_pass_latched = true;
    SetPassText(I18n::T("无线充电成功"));
    ESP_LOGI(TAG, "NU1680 probed, stop further detection");
}

}  // namespace Nu1680Test
