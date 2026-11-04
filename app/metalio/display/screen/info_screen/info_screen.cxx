/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * InfoScreen — ported from MetalioClaw4
 * main/display/screen/info_screen/info_screen.cc.
 *
 * Hardware rows are read from the board that is running. The IDF row is
 * the firmware name "openvela" on every board. MAC, UUID and the settings
 * journal are resolved off the interface thread: a new board has nothing
 * saved yet, and erasing that journal on the interface thread drops the
 * panel to solid blue.
 */

#include "info_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "board_shim.h"
#include "system_info.h"
#include "home_screen/home_screen.h"
#include "settings.h"
#include "hi_task_policy.h"
#include <metalio/flash_layout.h>
#include <metalio/metalio.h>
#include <netutils/netlib.h>
#include <nuttx/config.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

extern "C" {
uint32_t metalio_flash_bytes(void);
size_t metalio_psram_bytes(void);
size_t metalio_sram_bytes(void);
}

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char *TAG = "InfoScreen";

constexpr int kPanelSize = 720;
constexpr int kHeaderH = 90;
constexpr int kBackBtnSize = 72;
constexpr int kPad = 16;
constexpr int kRowH = 72;
constexpr int kRowGap = 8;
constexpr int kBrandPanelH = 280;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorCard = 0x1B2030;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorLabel = 0x9AA3B2;
constexpr uint32_t kColorValue = 0xE5E7EB;
constexpr uint32_t kColorBrand = 0xE65A3B;

struct InfoItem {
    const char *label;
    char value[96];
};

struct UiState {
    lv_obj_t *screen = nullptr;
};
UiState s_ui;
struct MacState {
    lv_obj_t *screen = nullptr;
    lv_obj_t *value = nullptr;
    lv_obj_t *uuid = nullptr;
    lv_timer_t *timer = nullptr;
    uint32_t epoch = 0;
    bool worker_busy = false;
    bool uuid_done = false;
    bool mac_saved = false;
    bool save_mac = false;
    char mac[18]{};
    char mac_text[32]{};
    char uuid_text[40]{};
};
MacState *MacFor(lv_obj_t *screen){return screen?static_cast<MacState*>(lv_obj_get_user_data(screen)):nullptr;}
uint32_t s_epoch_seq = 1;

struct PersistJob {
    uint32_t epoch = 0;
    bool save_mac = false;
    bool resolve_uuid = false;
    char mac[18]{};
};

struct PersistResult {
    uint32_t epoch = 0;
    bool mac_attempted = false;
    bool uuid_attempted = false;
    bool have_mac = false;
    bool have_uuid = false;
    char mac[18]{};
    char uuid[40]{};
};

void ApplyPersistResult(void *arg)
{
    std::unique_ptr<PersistResult> result(static_cast<PersistResult *>(arg));
    MacState *state = s_ui.screen && lv_screen_active() == s_ui.screen ? MacFor(s_ui.screen) : nullptr;
    if (state == nullptr || state->epoch == 0 || state->epoch != result->epoch)
        return;
    state->worker_busy = false;
    if (result->mac_attempted)
        state->mac_saved = true;
    if (result->uuid_attempted)
        state->uuid_done = true;
    if (result->have_mac && state->value != nullptr)
        lv_label_set_text(state->value, result->mac);
    if (result->have_uuid && state->uuid != nullptr) {
        std::snprintf(state->uuid_text, sizeof(state->uuid_text), "%s", result->uuid);
        lv_label_set_text(state->uuid, result->uuid);
    }
}

void *PersistIdentity(void *arg)
{
    std::unique_ptr<PersistJob> job(static_cast<PersistJob *>(arg));
    auto *result = new PersistResult{};
    result->epoch = job->epoch;
    // Identifying a new board writes its settings journal. That erase has
    // to stay off the interface thread or the panel latches solid blue.
    if (job->save_mac && job->mac[0] != '\0') {
        result->mac_attempted = true;
        Settings settings("hardware", true);
        if (settings.GetString("wifi_mac") != job->mac)
            settings.SetString("wifi_mac", job->mac);
        std::snprintf(result->mac, sizeof(result->mac), "%s", job->mac);
        result->have_mac = true;
    }
    if (job->resolve_uuid) {
        result->uuid_attempted = true;
        const std::string id = Board::GetInstance().GetUuid();
        if (!id.empty() && id.size() < sizeof(result->uuid)) {
            std::snprintf(result->uuid, sizeof(result->uuid), "%s", id.c_str());
            result->have_uuid = true;
        }
    }
    if (hi_task_post_ui(ApplyPersistResult, result) != 0)
        delete result;
    return nullptr;
}

void StartIdentity(MacState *state)
{
    if (state == nullptr || state->worker_busy || state->epoch == 0)
        return;
    if (state->screen != lv_screen_active())
        return;
    const bool resolve_uuid = !state->uuid_done;
    const bool save_mac = state->save_mac && !state->mac_saved && state->mac[0] != '\0';
    if (!resolve_uuid && !save_mac)
        return;
    auto *job = new PersistJob{};
    job->epoch = state->epoch;
    job->save_mac = save_mac;
    job->resolve_uuid = resolve_uuid;
    std::snprintf(job->mac, sizeof(job->mac), "%s", state->mac);
    pthread_t thread{};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 16384);
    if (hi_task_create_legacy(&thread, &attr, PersistIdentity, job) != 0)
        delete job;
    else
        state->worker_busy = true;
    pthread_attr_destroy(&attr);
}

void ReadLiveMac(MacState *state)
{
    uint8_t mac[6]{};
    static constexpr uint8_t placeholder[] = {2, 0x11, 0x22, 0x33, 0x44, 0x55};
    if (netlib_getmacaddr("eth0", mac) == 0 && !(mac[0] & 3) &&
        std::memcmp(mac, "\0\0\0\0\0\0", 6) != 0 &&
        std::memcmp(mac, placeholder, 6) != 0) {
        std::snprintf(state->mac_text, sizeof(state->mac_text),
                      "%02X:%02X:%02X:%02X:%02X:%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        if (!state->mac_saved && std::strcmp(state->mac, state->mac_text) != 0) {
            state->save_mac = true;
            std::snprintf(state->mac, sizeof(state->mac), "%s", state->mac_text);
        }
        return;
    }
    if (state->mac_text[0] == '\0')
        std::snprintf(state->mac_text, sizeof(state->mac_text), "%s", "识别中");
}

inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state)
{
    return static_cast<lv_style_selector_t>(part | state);
}

void FormatFlashSize(char *buf, size_t buf_size, size_t bytes)
{
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, buf_size, "%u MB",
                      static_cast<unsigned>(bytes / (1024 * 1024)));
    } else if (bytes >= 1024) {
        std::snprintf(buf, buf_size, "%u KB",
                      static_cast<unsigned>(bytes / 1024));
    } else {
        std::snprintf(buf, buf_size, "%u B", static_cast<unsigned>(bytes));
    }
}

void CollectInfoItems(InfoItem *items, int *count)
{
    int idx = 0;
    const std::string model = Board::GetInstance().GetBoardType();
    std::string chip = SystemInfo::GetChipModelName();
    if (chip == "esp32p4")
        chip = "ESP32-P4";
    const std::string firmware = Board::GetInstance().GetFirmwareVersion();
    // Product name, not this unit's serial. Every identical Claw 4 reports
    // the same board type, so every one of them shows the same two rows.
    const bool claw4 = model == "metalio-claw-4";

    items[idx].label = I18n::T("设备型号");
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s",
                  model.empty() ? "识别中" : (claw4 ? "MetalioClaw 4" : model.c_str()));
    ++idx;

    items[idx].label = I18n::T("芯片型号");
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s",
                  chip.empty() ? "识别中" : chip.c_str());
    ++idx;

    items[idx].label = I18n::T("CPU 核心");
    std::snprintf(items[idx].value, sizeof(items[idx].value), I18n::T("%u 核"),
                  static_cast<unsigned>(CONFIG_SMP_NCPUS));
    ++idx;

    items[idx].label = I18n::T("固件版本");
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s",
                  claw4 ? "HiOS-V1.0" : (firmware.empty() ? "识别中" : firmware.c_str()));
    ++idx;

    items[idx].label = I18n::T("编译时间");
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s %s",
                  __DATE__, __TIME__);
    ++idx;

    items[idx].label = I18n::T("IDF 版本");
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", "openvela");
    ++idx;

    const MacState *identity = MacFor(s_ui.screen);
    items[idx].label = I18n::T("MAC 地址");
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s",
                  identity != nullptr ? identity->mac_text : "--");
    ++idx;

    items[idx].label = I18n::T("设备 UUID");
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s",
                  identity != nullptr ? identity->uuid_text : "--");
    ++idx;

    items[idx].label = I18n::T("Flash 容量");
    {
        const uint32_t flash_bytes = metalio_flash_bytes();
        if (flash_bytes == 0)
            std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", "识别中");
        else
            FormatFlashSize(items[idx].value, sizeof(items[idx].value), flash_bytes);
    }
    ++idx;

    items[idx].label = I18n::T("SRAM 总大小");
    FormatFlashSize(items[idx].value, sizeof(items[idx].value), metalio_sram_bytes());
    ++idx;

    items[idx].label = I18n::T("PSRAM 总大小");
    {
        const size_t psram_total = metalio_psram_bytes();
        if (psram_total == 0) {
            std::snprintf(items[idx].value, sizeof(items[idx].value),
                          I18n::T("无"));
        } else {
            FormatFlashSize(items[idx].value, sizeof(items[idx].value),
                            psram_total);
        }
    }
    ++idx;

    items[idx].label = I18n::T("OTA 分区");
    std::snprintf(items[idx].value, sizeof(items[idx].value), "app@0x%X",
                  static_cast<unsigned>(METALIO_APP_START));
    ++idx;

    *count = idx;
}

void OnSwipeBack()
{
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    HomeScreen::SwitchToHome();
}

void OnBackClicked(lv_event_t * /*e*/) { OnSwipeBack(); }

void OnScreenUnloaded(lv_event_t *e) {
    auto*screen=lv_event_get_target_obj(e);auto*mac=MacFor(screen);
    if (mac != nullptr)
        mac->epoch = 0;
    if(mac&&mac->timer){lv_timer_delete(mac->timer);mac->timer=nullptr;}
    if(screen==s_ui.screen)s_ui.screen=nullptr;
}

void BuildHeader(lv_obj_t *parent)
{
    lv_obj_t *header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("系统信息"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);
}

lv_obj_t *CreateInfoRow(lv_obj_t *parent, const InfoItem &item)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, kRowH);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, item.label);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorLabel), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, item.value);
    lv_obj_set_width(value, LV_PCT(100));
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(value, lv_color_hex(kColorValue), LV_PART_MAIN);
    lv_obj_set_style_text_font(value, &font_puhui_20_4, LV_PART_MAIN);
    if (auto *mac = MacFor(s_ui.screen)) {
        if (!std::strcmp(item.label, I18n::T("MAC 地址")))
            mac->value = value;
        if (!std::strcmp(item.label, I18n::T("设备 UUID")))
            mac->uuid = value;
    }

    return row;
}

void CreateBrandPanel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_height(panel, kBrandPanelH);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kColorBrand), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 12, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *logo = lv_image_create(panel);
    lv_image_set_src(logo, "A:ic_info_hios_logo.spng");
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -22);

    /* Two one-pixel-offset copies give the regular CJK font a clean bold
     * treatment without adding another font asset. */
    for (int i = 0; i < 2; ++i)
    {
        lv_obj_t *tagline = lv_label_create(panel);
        lv_label_set_text(tagline, "为智联而生");
        lv_obj_set_style_text_color(tagline, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(tagline, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_align(tagline, LV_ALIGN_CENTER, i, 92);
    }
}

void BuildInfoList(lv_obj_t *parent)
{
    InfoItem items[16];
    int count = 0;
    CollectInfoItems(items, &count);

    lv_obj_t *list = lv_obj_create(parent);
    screen_strip_obj_chrome(list);
    lv_obj_set_size(list, kPanelSize - 2 * kPad, kPanelSize - kHeaderH - kPad);
    lv_obj_set_pos(list, kPad, kHeaderH);
    // Opaque viewport avoids redrawing the screen beneath every scroll.
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, kRowGap, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(list, kPad, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    CreateBrandPanel(list);
    for (int i = 0; i < count; ++i) {
        CreateInfoRow(list, items[i]);
    }
}

}  // namespace

lv_obj_t *InfoScreen::CreateStatic()
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    auto *mac = new MacState;
    mac->screen = scr;
    mac->epoch = s_epoch_seq++;
    std::snprintf(mac->uuid_text, sizeof(mac->uuid_text), "%s", "识别中");
    ReadLiveMac(mac);
    lv_obj_set_user_data(scr, mac);
    lv_obj_add_event_cb(scr, [](lv_event_t *e) {
        auto *state = static_cast<MacState *>(lv_event_get_user_data(e));
        state->epoch = 0;
        if (state->timer)
            lv_timer_delete(state->timer);
        delete state;
    }, LV_EVENT_DELETE, mac);
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildInfoList(scr);
    mac->timer = lv_timer_create([](lv_timer_t *timer) {
        auto *state = static_cast<MacState *>(lv_timer_get_user_data(timer));
        if (state->epoch == 0 || state->screen != lv_screen_active())
            return;
        ReadLiveMac(state);
        if (state->value != nullptr &&
            std::strcmp(lv_label_get_text(state->value), state->mac_text) != 0)
            lv_label_set_text(state->value, state->mac_text);
        StartIdentity(state);
    }, 1000, mac);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}

lv_obj_t *InfoScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void InfoScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: info_screen");
    } else {
        ESP_LOGI(TAG, "unload: info_screen");
    }
}
