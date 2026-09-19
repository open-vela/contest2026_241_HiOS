#include "pin_gpio_test.h"
#include "i18n.h"

#include "esp_log_shim.h"
#include "test_ui_common.h"

namespace {

constexpr const char* TAG = "PinGpioTest";

lv_obj_t* s_value_lbl   = nullptr;
lv_obj_t* s_status_icon = nullptr;

}  // namespace

namespace PinGpioTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, I18n::T("引脚测试"), &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, I18n::T("检测中..."));
}

void OnLoad() {
    if (s_value_lbl != nullptr) {
        // 原版通过 ESP-IDF driver/gpio 直接配置并读写 GPIO；openvela 尚无
        // /dev/gpio 用户态驱动，引脚逐路切换暂不可用。
        lv_label_set_text(s_value_lbl, I18n::T("需 /dev/gpio 驱动"));
        lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorError),
                                    LV_PART_MAIN);
        TestUiUpdateStatus(s_status_icon, false);
    }
    ESP_LOGW(TAG, "pin gpio test: /dev/gpio driver not available");
}

void OnUnload() {
    s_value_lbl   = nullptr;
    s_status_icon = nullptr;
}

void Poll() {
    // 引脚输入轮询依赖 GPIO 驱动，openvela 暂无。
}

}  // namespace PinGpioTest
