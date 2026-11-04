#include "cell_4g_test.h"
#include "i18n.h"

#include "esp_log_shim.h"
#include "test_ui_common.h"

#include <metalio/metalio.h>
#include <cstdio>

namespace {

constexpr const char* TAG = "Cell4gTest";

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

}  // namespace

namespace Cell4gTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, I18n::T("4G"), &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, I18n::T("检测中..."));
}

void OnLoad() {
    /* NT26 is implemented by the board driver and started only when the
     * selected network mode is cellular.  Do not label an idle modem as
     * missing or start/reset it merely by opening the hardware test screen. */
    const int sim_slot = metalio_nt26_get_sim_slot();
    if (sim_slot >= 0) {
        char status[48];
        snprintf(status, sizeof(status), "NT26已启动（SIM卡槽 %d）", sim_slot + 1);
        SetText(status, true);
        ESP_LOGI(TAG, "cell 4g test: NT26 control channel ready, SIM slot %d",
                 sim_slot + 1);
    } else {
        SetText(I18n::T("NT26驱动已集成，当前未启用"), false);
        ESP_LOGI(TAG, "cell 4g test: NT26 driver present, cellular mode idle");
    }
}

void OnUnload() {
    s_value_lbl   = nullptr;
    s_status_icon = nullptr;
}

}  // namespace Cell4gTest
