#include "sd_card_test.h"
#include "i18n.h"

#include <cstdio>
#include <cerrno>
#include <cstdint>

#include <sys/statfs.h>

#include "esp_log_shim.h"
#include "test_ui_common.h"

namespace {

constexpr const char* TAG = "SdCardTest";
constexpr const char* kSdMount = "/sdcard";

lv_obj_t* s_value_lbl   = nullptr;
lv_obj_t* s_status_icon = nullptr;

void FormatSize(uint64_t bytes, char* buf, size_t buf_size) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        const unsigned gb_int =
            static_cast<unsigned>(bytes / (1024ULL * 1024 * 1024));
        const unsigned gb_frac =
            static_cast<unsigned>((bytes % (1024ULL * 1024 * 1024)) /
                                  (10ULL * 1024 * 1024));
        std::snprintf(buf, buf_size, "%u.%02u GB", gb_int, gb_frac);
    } else if (bytes >= 1024 * 1024) {
        const unsigned mb_int = static_cast<unsigned>(bytes / (1024 * 1024));
        const unsigned mb_frac =
            static_cast<unsigned>((bytes % (1024 * 1024)) / (10 * 1024));
        std::snprintf(buf, buf_size, "%u.%02u MB", mb_int, mb_frac);
    } else if (bytes >= 1024) {
        const unsigned kb_int = static_cast<unsigned>(bytes / 1024);
        std::snprintf(buf, buf_size, "%u KB", kb_int);
    } else {
        std::snprintf(buf, buf_size, "%lu B",
                      static_cast<unsigned long>(bytes));
    }
}

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

namespace SdCardTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, I18n::T("SD卡"), &s_status_icon, &ctrl);
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

    struct statfs st;
    if (statfs(kSdMount, &st) < 0) {
        SetErrorText(I18n::T("未插卡或挂载失败"));
        return;
    }

    const uint64_t total_bytes =
        static_cast<uint64_t>(st.f_blocks) * st.f_bsize;
    const uint64_t free_bytes =
        static_cast<uint64_t>(st.f_bfree) * st.f_bsize;

    char total_str[32];
    char free_str[32];
    FormatSize(total_bytes, total_str, sizeof(total_str));
    FormatSize(free_bytes, free_str, sizeof(free_str));

    char buf[96];
    std::snprintf(buf, sizeof(buf), I18n::T("剩余 %s / 总 %s"), free_str,
                  total_str);
    SetValueText(buf);
}

}  // namespace SdCardTest
