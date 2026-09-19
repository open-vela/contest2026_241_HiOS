#include "sc7a20h_test.h"
#include "i18n.h"

#include <cstdio>

#include "esp_log_shim.h"
#include "test_ui_common.h"
#include <metalio/metalio.h>

namespace {

constexpr const char* TAG = "Sc7a20hTest";

lv_obj_t* s_value_lbl   = nullptr;
lv_obj_t* s_status_icon = nullptr;

void SetErrorText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorError),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, false);
}

void SetValueText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, true);
}

}  // namespace

namespace Sc7a20hTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "SC7A20HTR", &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, I18n::T("初始化中..."));
}

void OnLoad() {
    Poll();
}

void OnUnload() {
    s_value_lbl   = nullptr;
    s_status_icon = nullptr;
}

void Poll() {
    if (s_value_lbl == nullptr) {
        return;
    }

    int ax = 0;
    int ay = 0;
    int az = 0;
    if (metalio_sc7a20h_read_mg(&ax, &ay, &az) < 0) {
        SetErrorText(I18n::T("I2C未检测到"));
        return;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "X:%dmg Y:%dmg Z:%dmg", ax, ay, az);
    SetValueText(buf);
}

}  // namespace Sc7a20hTest
