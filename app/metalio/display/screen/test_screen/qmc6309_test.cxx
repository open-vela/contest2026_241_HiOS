#include "qmc6309_test.h"
#include "i18n.h"

#include <cstdio>

#include "esp_log_shim.h"
#include "test_ui_common.h"
#include <metalio/metalio.h>

namespace {

constexpr const char* TAG = "Qmc6309Test";

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

namespace Qmc6309Test {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "QMC6309", &s_status_icon, &ctrl);
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

    int16_t mx = 0;
    int16_t my = 0;
    int16_t mz = 0;
    if (metalio_qmc6309_read_raw(&mx, &my, &mz) < 0) {
        SetErrorText(I18n::T("I2C未检测到"));
        return;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "X:%d Y:%d Z:%d", mx, my, mz);
    SetValueText(buf);
}

}  // namespace Qmc6309Test
