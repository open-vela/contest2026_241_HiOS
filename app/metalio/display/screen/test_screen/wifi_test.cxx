#include "wifi_test.h"
#include "i18n.h"

#include "esp_log_shim.h"
#include "test_ui_common.h"

namespace {

constexpr const char* TAG = "WifiTest";

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

namespace WifiTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "WiFi", &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, I18n::T("扫描中…"));
}

void OnLoad() {
    // openvela 尚未移植 esp_wifi 扫描栈（esp-hosted SPI slave 仅提供 STA
    // 连接路径，无用户态 scan API）。
    SetText(I18n::T("WiFi扫描未移植"), false);
    ESP_LOGW(TAG, "wifi test: scan stack not ported");
}

void OnUnload() {
    s_value_lbl   = nullptr;
    s_status_icon = nullptr;
}

}  // namespace WifiTest
