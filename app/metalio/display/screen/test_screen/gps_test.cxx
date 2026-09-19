#include "gps_test.h"
#include "i18n.h"

#include <cstdio>

#include "esp_log_shim.h"
#include "test_ui_common.h"
#include <metalio/metalio.h>

namespace {

constexpr const char* TAG = "GpsTest";

lv_obj_t* s_status_icon = nullptr;
lv_obj_t* s_value_lbl   = nullptr;

void SetPassText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, true);
}

void SetFailText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorError),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, false);
}

}  // namespace

namespace GpsTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, "GPS", &s_status_icon, &ctrl);
    s_value_lbl = TestUiCreateValueLabel(ctrl);
    lv_label_set_text(s_value_lbl, "HDOP --");
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

    int q = 0;
    int sats = 0;
    double lat = 0.0;
    double lon = 0.0;

    if (metalio_gps_get_snapshot(&q, &sats, &lat, &lon) < 0) {
        SetFailText(I18n::T("未收到NMEA"));
        return;
    }

    char buf[48];
    if (q > 0) {
        std::snprintf(buf, sizeof(buf), "fix=%d sats=%d", q, sats);
        SetPassText(buf);
    } else {
        std::snprintf(buf, sizeof(buf), "q=%d sats=%d", q, sats);
        SetPassText(buf);
    }
}

}  // namespace GpsTest
