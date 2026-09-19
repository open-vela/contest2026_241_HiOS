#include "cell_4g_test.h"
#include "i18n.h"

#include "esp_log_shim.h"
#include "test_ui_common.h"

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
    // openvela 尚未移植 NT26 AT 命令栈，无法执行 SIM 卡 / 搜网 / PING 流程。
    SetText(I18n::T("4G模块未移植"), false);
    ESP_LOGW(TAG, "cell 4g test: NT26 AT stack not ported");
}

void OnUnload() {
    s_value_lbl   = nullptr;
    s_status_icon = nullptr;
}

}  // namespace Cell4gTest
