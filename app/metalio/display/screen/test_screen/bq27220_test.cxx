#include "bq27220_test.h"
#include "i18n.h"

#include <cstdio>

#include "esp_log_shim.h"
#include "test_ui_common.h"
#include <metalio/metalio.h>

namespace {

constexpr const char* TAG = "Bq27220Test";

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

namespace Bq27220Test {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "BQ27220", &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, I18n::T("检测中..."));
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

    int mv = 0;
    int pct = 0;
    if (metalio_bq27220_read_voltage_mv(&mv) < 0 ||
        metalio_bq27220_read_soc(&pct) < 0) {
        SetErrorText(I18n::T("I2C未检测到"));
        return;
    }

    char buf[48];
    std::snprintf(buf, sizeof(buf), "%dmV %d%%", mv, pct);
    SetValueText(buf);
}

}  // namespace Bq27220Test
