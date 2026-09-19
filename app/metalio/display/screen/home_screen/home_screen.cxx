/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * HomeScreen — ported from MetalioClaw4
 * main/display/screen/home_screen/home_screen.cc (2512 lines).
 *
 * openvela / NuttX adaptations:
 *   - ESP-IDF headers → shim headers (esp_log_shim, freertos_shim,
 *     esp_err_shim, board_shim).
 *   - esp_partition / esp_ota_ops (ESPClaw OTA switch) → stubbed; the
 *     ESPClaw launcher shows an "unavailable" toast instead of switching
 *     boot partitions.
 *   - Board::GetInstance().GetBatteryLevel / GetNetworkStateIcon →
 *     metalio_board_get_battery / metalio_board_get_network_icon (C
 *     accessors in board_shim.cxx).
 *   - IOExpander PWR_KEY_PULSE shutdown task → stubbed; the shutdown
 *     screen is shown and the request is logged (TODO: wire to NuttX
 *     power driver).
 *   - NT26 / DualNetworkBoard SIM-slot query task → removed (no modem
 *     AT shim yet); SIM-slot label stays hidden.
 *   - Bq27220Gauge voltage read → removed (no fuel-gauge driver yet);
 *     battery shows percentage + charging icon only.
 *   - ThemeManager → display/theme_manager (NVS "ui"/"theme_id").
 *   - App launchers: only screens already ported to openvela are wired
 *     up (chat, settings, info, theme, test, sd_card, vibrate, magnet,
 *     level, bluetooth, backlight). Unported apps show a transient
 *     "敬请期待" toast; the grid layout and icons remain unchanged.
 *   - LVGL widget construction code (pager, grid, status bar, touch
 *     gesture classification, power dialog) is ported verbatim.
 */

#include "home_screen.h"
#include "theme_manager.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "freertos_shim.h"
#include "esp_err_shim.h"
#include "board_shim.h"
#include <metalio/metalio.h>
#include "font_awesome.h"
#include "application.h"
#include "chat_screen/chat_screen.h"
#include "settings_screen/settings_screen.h"
#include "info_screen/info_screen.h"
#include "theme_screen/theme_screen.h"
#include "test_screen/test_screen.h"
#include "sd_card_screen/sd_card_screen.h"
#include "vibrate_screen/vibrate_screen.h"
#include "magnet_screen/magnet_screen.h"
#include "level_screen/level_screen.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "backlight_screen/backlight_screen.h"
#include "call_screen/call_screen.h"
#include "calendar_screen/calendar_screen.h"
#include "camera_screen/camera_screen.h"
#include "calculator_screen/calculator_screen.h"
#include "pin_test_screen/pin_test_screen.h"
#include "game_2048_screen/game_2048_screen.h"
#include "cicada_screen/cicada_screen.h"
#include "ai_image_gen_screen/ai_image_gen_screen.h"
#include "translate_screen/translate_screen.h"
#include "secondary_screen/secondary_screen.h"
#include "digital_people_screen/digital_people_screen.h"
#include "weather_screen/weather_screen.h"
#include "music_screen/music_screen.h"
#include "gps_screen/gps_screen.h"
#include "radio_screen/radio_screen.h"
#include "network_screen/network_screen.h"
#include "openclaw_screen/openclaw_screen.h"
#include "recording_screen/recording_screen.h"
#include "espclaw_screen/espclaw_screen.h"
#include "pwr_key_handler/pwr_key_handler.h"
#include "idle_power_policy/idle_power_policy.h"
#include "wifi_required_dialog/wifi_required_dialog.h"
#include "screen_util.h"

#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <cctype>
#include <ctime>
#include <string>
#include <cstring>
#include <cstdint>
#include <strings.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);

/* Match MetalioClaw4: Font Awesome battery icon only (bolt while charging).
 * No voltage / 「充电中」text in the status bar. */
#define HOME_STATUS_SHOW_BATTERY_ICON 1

namespace {

constexpr const char *TAG_HOME = "HomeScreen";

// ---------------------------------------------------------------------------
// Per-app lifecycle callbacks — forward to PwrKey + the screen's own
// LifecycleCallback. Unported apps have nullptr here.
// ---------------------------------------------------------------------------

void chat_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("chat", event);
    ChatScreen::LifecycleCallback(event);
}

void settings_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("settings", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: settings_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: settings_screen");
    }
    SettingsScreen::LifecycleCallback(event);
}

void info_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("info", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: info_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: info_screen");
    }
    InfoScreen::LifecycleCallback(event);
}

void theme_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("theme", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: theme_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: theme_screen");
    }
    ThemeScreen::LifecycleCallback(event);
}

void test_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("test", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: test_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: test_screen");
    }
    TestScreen::LifecycleCallback(event);
}

void sd_card_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("sd_card", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: sd_card_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: sd_card_screen");
    }
    SdCardScreen::LifecycleCallback(event);
}

void vibrate_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("vibrate", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: vibrate_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: vibrate_screen");
    }
    VibrateScreen::LifecycleCallback(event);
}

void magnet_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("magnet", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: magnet_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: magnet_screen");
    }
    MagnetScreen::LifecycleCallback(event);
}

void level_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("level", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: level_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: level_screen");
    }
    LevelScreen::LifecycleCallback(event);
}

void bluetooth_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("bluetooth", event);
    BluetoothScreen::LifecycleCallback(event);
}

void backlight_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("backlight", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: backlight_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: backlight_screen");
    }
    BacklightScreen::LifecycleCallback(event);
}

void call_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("call", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: call_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: call_screen");
    }
    CallScreen::LifecycleCallback(event);
}

// calendar / calculator / game_2048 have no LifecycleCallback — just
// forward to PwrKey so the power-key handler knows which screen is active.
void calendar_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("calendar", event);
}
void calculator_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("calculator", event);
}
void game_2048_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("2048", event);
}
void cicada_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("cicada", event);
    CicadaScreen::LifecycleCallback(event);
}

void camera_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("camera", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: camera_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: camera_screen");
    }
    CameraScreen::LifecycleCallback(event);
}

void pin_test_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("pin", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: pin_test_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: pin_test_screen");
    }
    PinTestScreen::LifecycleCallback(event);
}

void ai_image_gen_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("ai_image_gen", event);
    AiImageGenScreen::LifecycleCallback(event);
}

void translate_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("translate", event);
    TranslateScreen::LifecycleCallback(event);
}

void secondary_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("secondary_screen", event);
    SecondaryScreen::LifecycleCallback(event);
}

void digital_people_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("digital_people", event);
    DigitalPeopleScreen::LifecycleCallback(event);
}

void weather_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("weather", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: weather_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: weather_screen");
    }
}

void music_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("music", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: music_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: music_screen");
    }
    MusicScreen::LifecycleCallback(event);
}

void gps_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("gps", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: gps_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: gps_screen");
    }
    GpsScreen::LifecycleCallback(event);
}

void radio_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("radio", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: radio_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: radio_screen");
    }
    RadioScreen::LifecycleCallback(event);
}

void network_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("wifi", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: network_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: network_screen");
    }
    NetworkScreen::LifecycleCallback(event);
}

void openclaw_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("openclaw", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: openclaw_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: openclaw_screen");
    }
    OpenClawScreen::LifecycleCallback(event);
}

void recording_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("recording", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: recording_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: recording_screen");
    }
    RecordingScreen::LifecycleCallback(event);
}

void espclaw_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("espclaw", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: espclaw_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: espclaw_screen");
    }
    EspClawScreen::LifecycleCallback(event);
}

// ---------------------------------------------------------------------------
// Layout constants (ported verbatim)
// ---------------------------------------------------------------------------
constexpr int kPanelSize           = 720;
constexpr int kStatusBarHeight      = 48;
constexpr int kIndicatorAreaHeight  = 40;
constexpr int kPagerHeight =
    kPanelSize - kStatusBarHeight - kIndicatorAreaHeight;  // 632
constexpr int kAppsPerPage  = 9;  // 3x3
constexpr int kPageCols     = 3;
constexpr int kPageRows     = 3;
constexpr int kIconSize     = 128;
constexpr int kCellWidth    = 160;
constexpr int kNameGap      = 6;
constexpr int kNameAreaH    = 24;
constexpr int kCellHeight   = kIconSize + kNameGap + kNameAreaH;  // 158
constexpr int kGridColGap   = 60;
constexpr int kGridRowGap   = 36;
constexpr int kPagePadHor =
    (kPanelSize - kPageCols * kCellWidth - (kPageCols - 1) * kGridColGap) / 2;
constexpr int kPagePadVer =
    (kPagerHeight - kPageRows * kCellHeight - (kPageRows - 1) * kGridRowGap) / 2;

constexpr uint32_t kStatusBarBg = 0x000000;
constexpr int kMaxPages = 6;

int32_t s_col_dsc[kPageCols + 1] = {
    kCellWidth, kCellWidth, kCellWidth, LV_GRID_TEMPLATE_LAST,
};
int32_t s_row_dsc[kPageRows + 1] = {
    kCellHeight, kCellHeight, kCellHeight, LV_GRID_TEMPLATE_LAST,
};

// Indicator dot geometry
constexpr int kDotSize           = 8;
constexpr int kDotGap            = 12;
constexpr int kIndicatorPadHor   = 14;
constexpr int kIndicatorPadVer   = 8;
constexpr int kIndicatorYOffset  = 12;
constexpr uint32_t kIndicatorBg  = 0x000000;
constexpr uint32_t kDotColor     = 0xFFFFFF;

// ---------------------------------------------------------------------------
// App entry table
//
// icon_suffix → A:ic_app_home_theme{N}_{suffix}.spng
// create == nullptr → app not yet ported; tapping shows a transient toast.
// ---------------------------------------------------------------------------
typedef lv_obj_t *(*CreateScreenFn)();

struct AppEntry {
    const char *icon_suffix;
    const char *name;
    CreateScreenFn create;               // nullptr = not yet ported
    screen_lifecycle_cb_t lifecycle_cb;  // load / unload observer
    bool requires_wifi;
};

constexpr AppEntry kApps[] = {
    {"chat",            "聊天",     ChatScreen::CreateStatic,       chat_lifecycle_cb,       true},
    {"wifi",            "网络配置", NetworkScreen::CreateStatic,    network_lifecycle_cb,    false},
    {"digital_people",  "数字人",   DigitalPeopleScreen::CreateStatic, digital_people_lifecycle_cb, true},
    {"call",            "电话",     CallScreen::CreateStatic,       call_lifecycle_cb,       false},
    {"music",           "音乐",     MusicScreen::CreateStatic,      music_lifecycle_cb,      false},
    {"calendar",        "日历",     CalendarScreen::CreateStatic,   calendar_lifecycle_cb,   false},
    {"openclaw",        "OpenClaw", OpenClawScreen::CreateStatic,   openclaw_lifecycle_cb,   true},
    {"espclaw",         "ESPClaw",  EspClawScreen::CreateStatic,   espclaw_lifecycle_cb,    false},
    {"camera",          "相机",     CameraScreen::CreateStatic,     camera_lifecycle_cb,     false},
    {"gps",             "定位",     GpsScreen::CreateStatic,        gps_lifecycle_cb,        true},
    {"spirit_level",    "水平仪",   LevelScreen::CreateStatic,      level_lifecycle_cb,      false},
    {"magnet",          "磁场",     MagnetScreen::CreateStatic,     magnet_lifecycle_cb,     false},
    {"vibrate",         "震动",     VibrateScreen::CreateStatic,    vibrate_lifecycle_cb,    false},
    {"calculator",      "计算器",   CalculatorScreen::CreateStatic, calculator_lifecycle_cb, false},
    {"weather",         "天气",     WeatherScreen::CreateStatic,    weather_lifecycle_cb,    true},
    {"sd",              "SD卡",     SdCardScreen::CreateStatic,     sd_card_lifecycle_cb,    false},
    {"pin",             "引脚测试", PinTestScreen::CreateStatic,    pin_test_lifecycle_cb,   false},
    {"2048",            "2048",     Game2048Screen::CreateStatic,   game_2048_lifecycle_cb,  false},
    {"cicada",          "竹知了",   CicadaScreen::CreateStatic,     cicada_lifecycle_cb,    false},
    {"info",            "系统信息", InfoScreen::CreateStatic,       info_lifecycle_cb,       false},
    {"theme",           "主题",     ThemeScreen::CreateStatic,      theme_lifecycle_cb,      false},
    {"test",            "测试",     TestScreen::CreateStatic,       test_lifecycle_cb,       false},
    {"settings",        "设置",     SettingsScreen::CreateStatic,   settings_lifecycle_cb,   false},
    {"radio",           "电台",     RadioScreen::CreateStatic,      radio_lifecycle_cb,      true},
    {"recording",       "录音",     RecordingScreen::CreateStatic,  recording_lifecycle_cb,  false},
    {"ai_image_gen",    "AI生图",   AiImageGenScreen::CreateStatic, ai_image_gen_lifecycle_cb, true},
    {"translate",       "翻译",     TranslateScreen::CreateStatic,  translate_lifecycle_cb,  true},
    {"secondary_screen","副屏",    SecondaryScreen::CreateStatic,  secondary_lifecycle_cb,  false},
    {"backlight",       "背光",     BacklightScreen::CreateStatic,  backlight_lifecycle_cb,  false},
    {"bluetooth",       "蓝牙",     BluetoothScreen::CreateStatic,  bluetooth_lifecycle_cb,  false},
};

constexpr int kTotalApps = static_cast<int>(sizeof(kApps) / sizeof(kApps[0]));

constexpr int kIconPathBufSize = 56;
char s_icon_paths[kTotalApps][kIconPathBufSize];

void EnsureIconPathsBuilt()
{
    static bool built = false;
    static int s_built_theme_id = 0;
    const int tid = ThemeManager::GetCurrentThemeId();
    if (built && s_built_theme_id == tid) {
        return;
    }
    for (int i = 0; i < kTotalApps; ++i) {
        std::snprintf(s_icon_paths[i], kIconPathBufSize,
                      "A:ic_app_home_theme%d_%s.spng",
                      tid, kApps[i].icon_suffix);
    }
    s_built_theme_id = tid;
    built = true;
    ESP_LOGI(TAG_HOME, "icon paths built for theme%d", tid);
}

// ---------------------------------------------------------------------------
// "App not yet ported" toast
// ---------------------------------------------------------------------------
lv_obj_t *s_unavailable_toast = nullptr;
lv_timer_t *s_unavailable_toast_timer = nullptr;

void OnUnavailableToastTimer(lv_timer_t *timer)
{
    s_unavailable_toast_timer = nullptr;
    if (s_unavailable_toast != nullptr) {
        lv_obj_delete(s_unavailable_toast);
        s_unavailable_toast = nullptr;
    }
    lv_timer_delete(timer);
}

void ShowUnavailableToast(const char *app_name)
{
    lv_obj_t *scr = lv_screen_active();
    if (scr == nullptr) return;

    if (s_unavailable_toast != nullptr) {
        lv_obj_delete(s_unavailable_toast);
        s_unavailable_toast = nullptr;
    }
    if (s_unavailable_toast_timer != nullptr) {
        lv_timer_delete(s_unavailable_toast_timer);
        s_unavailable_toast_timer = nullptr;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), I18n::T("%s 敬请期待"), app_name);

    lv_obj_t *toast = lv_obj_create(scr);
    screen_strip_obj_chrome(toast);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_radius(toast, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(toast, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(toast, 16, LV_PART_MAIN);
    lv_obj_align(toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(toast, true);

    lv_obj_t *lbl = lv_label_create(toast);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(lbl);

    s_unavailable_toast = toast;
    s_unavailable_toast_timer =
        lv_timer_create(OnUnavailableToastTimer, 1500, nullptr);
    lv_timer_set_repeat_count(s_unavailable_toast_timer, 1);
}

// ---------------------------------------------------------------------------
// Touch / pager forward declarations
// ---------------------------------------------------------------------------
struct PagerState;
void SetPagerSkeletonMode(PagerState *state, bool active);
void HighlightDot(PagerState *state, int page);
int32_t PagerScrollXForPage(const PagerState *state, int logical_page);
bool PagerLoopEnabled(const PagerState *state);
void GoToPage(PagerState *state, int target_page);
void CancelCellScaleTimer();

// ---------------------------------------------------------------------------
// Home idle timer (delegated to idle_power_policy)
// ---------------------------------------------------------------------------
void ResetHomeIdleTimer() { IdlePower_NotifyActivity(); }
void StopHomeIdleTimer()  { IdlePower_Detach(IdlePowerSession::Home); }
void StartHomeIdleTimer() { IdlePower_Attach(IdlePowerSession::Home, true); }

// ---------------------------------------------------------------------------
// App cell press-scale animation
// ---------------------------------------------------------------------------
constexpr uint32_t kCellPressScaleMs = 16;

const lv_style_prop_t kPressTransProps[] = {
    LV_STYLE_TRANSFORM_SCALE_X,
    LV_STYLE_TRANSFORM_SCALE_Y,
    LV_STYLE_PROP_INV,
};

lv_style_transition_dsc_t &GetPressTransition()
{
    static lv_style_transition_dsc_t dsc;
    static bool inited = false;
    if (!inited) {
        lv_style_transition_dsc_init(&dsc, kPressTransProps,
                                     lv_anim_path_ease_out,
                                     kCellPressScaleMs, 0, nullptr);
        inited = true;
    }
    return dsc;
}

/* Direct launch (Claw4-style): create destination while the outgoing screen
 * is still active, load it, then async-delete the old screen. The dark
 * transition shell made every enter/exit flash black — remove it. */

bool LaunchHomeApp(const AppEntry *app)
{
    if (app == nullptr) return false;

    if (app->requires_wifi && WifiRequired_ShouldBlock()) {
        ESP_LOGW(TAG_HOME, "block app '%s': WiFi not connected",
                 app->icon_suffix != nullptr ? app->icon_suffix : "?");
        WifiRequired_ShowDialog();
        return false;
    }

    if (app->create == nullptr) {
        ESP_LOGI(TAG_HOME, "app '%s' not yet ported on openvela",
                 app->icon_suffix);
        ShowUnavailableToast(I18n::T(app->name));
        return false;
    }

    {
        char go[48];
        std::snprintf(go, sizeof(go), "APP_GO_%s\n",
                      app->icon_suffix != nullptr ? app->icon_suffix : "?");
        write(1, go, std::strlen(go));
    }
    lv_obj_t *old_scr = lv_screen_active();
    StopHomeIdleTimer();
    CancelCellScaleTimer();

    write(1, "APP_CREATE\n", 11);
    lv_obj_t *dest = app->create();
    if (dest == nullptr) {
        ESP_LOGE(TAG_HOME, "app '%s' CreateStatic() returned null",
                 app->icon_suffix);
        write(1, "APP_NULL\n", 9);
        return false;
    }
    if (app->lifecycle_cb != nullptr) {
        screen_attach_lifecycle(dest, app->lifecycle_cb);
    }
    write(1, "APP_CREATED\n", 12);

    lv_screen_load(dest);
    if (old_scr != nullptr && old_scr != dest) {
        lv_obj_delete_async(old_scr);
    }
    write(1, "APP_OK\n", 7);
    return true;
}

struct CellScaleCtx {
    lv_obj_t *cell = nullptr;
    const AppEntry *launch_app = nullptr;
};

lv_timer_t *s_cell_scale_timer = nullptr;
CellScaleCtx s_cell_scale_ctx;

void CancelCellScaleTimer()
{
    if (s_cell_scale_timer == nullptr) return;
    lv_timer_delete(s_cell_scale_timer);
    s_cell_scale_timer = nullptr;
    s_cell_scale_ctx = CellScaleCtx{};
}

void OnCellScaleTimer(lv_timer_t *timer)
{
    const CellScaleCtx ctx = s_cell_scale_ctx;
    s_cell_scale_timer = nullptr;
    s_cell_scale_ctx = CellScaleCtx{};
    lv_timer_delete(timer);

    if (ctx.launch_app != nullptr) {
        if (!LaunchHomeApp(ctx.launch_app) && ctx.cell != nullptr) {
            lv_obj_remove_state(ctx.cell, LV_STATE_PRESSED);
        }
        return;
    }
    if (ctx.cell != nullptr) {
        lv_obj_remove_state(ctx.cell, LV_STATE_PRESSED);
    }
}

void PlayAppCellPressScale(lv_obj_t *cell, const AppEntry *launch_app)
{
    if (cell == nullptr) return;
    CancelCellScaleTimer();
    lv_obj_add_state(cell, LV_STATE_PRESSED);
    s_cell_scale_ctx.cell = cell;
    s_cell_scale_ctx.launch_app = launch_app;
    s_cell_scale_timer =
        lv_timer_create(OnCellScaleTimer, kCellPressScaleMs, nullptr);
    lv_timer_set_repeat_count(s_cell_scale_timer, 1);
}

constexpr lv_obj_flag_t kAppCellFlag = LV_OBJ_FLAG_USER_2;

lv_obj_t *FindAppCellFromTarget(lv_obj_t *target, lv_obj_t *screen)
{
    for (lv_obj_t *obj = target; obj != nullptr && obj != screen;
         obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, kAppCellFlag)) {
            return obj;
        }
    }
    return nullptr;
}

lv_obj_t *CreateAppCellSkeleton(lv_obj_t *cell)
{
    constexpr uint32_t kSkeletonBg = 0x2A2F3A;

    lv_obj_t *skeleton = lv_obj_create(cell);
    lv_obj_remove_style_all(skeleton);
    lv_obj_set_size(skeleton, kIconSize, kIconSize);
    lv_obj_set_style_bg_color(skeleton, lv_color_hex(kSkeletonBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(skeleton, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(skeleton, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(skeleton, 0, LV_PART_MAIN);
    lv_obj_align(skeleton, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
    return skeleton;
}

lv_obj_t *CreateAppCell(lv_obj_t *parent, const AppEntry &entry, int idx)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, kCellWidth, kCellHeight);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(cell, false, LV_PART_MAIN);
    /* No transform_scale on press — LVGL scale redraw during touch
     * amplifies swipe/tap tear on this RGB888 DPI path. */

    lv_obj_t *icon = lv_image_create(cell);
    lv_image_set_src(icon, s_icon_paths[idx]);
    lv_obj_set_size(icon, kIconSize, kIconSize);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    CreateAppCellSkeleton(cell);

    lv_obj_t *name = lv_label_create(cell);
    lv_label_set_text(name, entry.name != nullptr ? I18n::T(entry.name) : "");
    lv_obj_set_width(name, kCellWidth);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, kIconSize + kNameGap);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

    // Every cell is hit-testable so PRESSED locks the target app; the
    // screen-level handler dispatches Click / LongPress.
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(cell, kAppCellFlag);
    lv_obj_set_user_data(cell, const_cast<AppEntry *>(&entry));

    return cell;
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------
struct HomeStatusState {
    lv_obj_t *bar = nullptr;
    lv_obj_t *network_icon_lbl = nullptr;
    lv_obj_t *network_type_lbl = nullptr;
    lv_obj_t *sim_slot_lbl = nullptr;
    lv_obj_t *battery_icon_lbl = nullptr;
    lv_obj_t *battery_pct_lbl = nullptr;
    lv_obj_t *time_lbl = nullptr;
    lv_obj_t *activation_code_lbl = nullptr;
    lv_timer_t *update_timer = nullptr;
    const char *last_icon = nullptr;
    const char *last_battery_icon = nullptr;
    int last_battery_pct = -1;
    bool last_battery_low = false;
    std::string last_activation_text;
    bool last_activation_visible = false;
};

HomeStatusState *s_home_status = nullptr;

void UpdateHomeStatusBar(HomeStatusState *st)
{
    if (st == nullptr || st->bar == nullptr) return;

    // Network type: on openvela the DualNetworkBoard is not yet wired up;
    // default to "WiFi". TODO(openvela): read from network settings.
    constexpr int kNetTypeWifi = 0;
    if (st->network_type_lbl != nullptr) {
        lv_label_set_text(st->network_type_lbl, "WiFi");
    }
    // SIM-slot label hidden (no modem AT shim yet).
    if (st->sim_slot_lbl != nullptr) {
        lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    const char *icon = metalio_board_get_network_icon();
    if (icon != nullptr && st->network_icon_lbl != nullptr &&
        icon != st->last_icon) {
        st->last_icon = icon;
        lv_label_set_text(st->network_icon_lbl, icon);
    }

    // ---- Battery ----
#if HOME_STATUS_SHOW_BATTERY_ICON
    if (st->battery_icon_lbl != nullptr) {
        int battery_level = 0;
        int charging = 0, discharging = 0;
        if (metalio_board_get_battery(&battery_level, &charging, &discharging)) {
            if (battery_level < 0)   battery_level = 0;
            if (battery_level > 100) battery_level = 100;

            const char *bat_icon = nullptr;
            if (charging) {
                bat_icon = FONT_AWESOME_BATTERY_BOLT;
            } else if (battery_level >= 80) {
                bat_icon = FONT_AWESOME_BATTERY_FULL;
            } else if (battery_level >= 60) {
                bat_icon = FONT_AWESOME_BATTERY_THREE_QUARTERS;
            } else if (battery_level >= 40) {
                bat_icon = FONT_AWESOME_BATTERY_HALF;
            } else if (battery_level >= 20) {
                bat_icon = FONT_AWESOME_BATTERY_QUARTER;
            } else {
                bat_icon = FONT_AWESOME_BATTERY_EMPTY;
            }
            if (bat_icon != st->last_battery_icon) {
                st->last_battery_icon = bat_icon;
                lv_label_set_text(st->battery_icon_lbl, bat_icon);
            }

            const bool low = !charging && battery_level < 20;
            if (low != st->last_battery_low) {
                st->last_battery_low = low;
                uint32_t color = low ? 0xF87171 : 0xFFFFFF;
                lv_obj_set_style_text_color(st->battery_icon_lbl,
                                            lv_color_hex(color), LV_PART_MAIN);
            }
        } else {
            /* Claw4: BATTERY_SLASH when gauge unavailable. */
            st->last_battery_icon = FONT_AWESOME_BATTERY_SLASH;
            lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_SLASH);
            if (st->last_battery_low) {
                st->last_battery_low = false;
                lv_obj_set_style_text_color(st->battery_icon_lbl,
                                            lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
        }
    }
#else
    if (st->battery_pct_lbl != nullptr) {
        int battery_level = 0;
        int charging = 0, discharging = 0;
        bool has_battery = metalio_board_get_battery(&battery_level, &charging, &discharging);

        if (has_battery) {
            if (battery_level < 0)   battery_level = 0;
            if (battery_level > 100) battery_level = 100;
            const bool low = !charging && battery_level < 20;

            /* Match Claw4 catalog keys ("电量 %d%% 充电中 %s" / "电量 %d%% %s"). */
            char volt_str[16] = "--V";
            int mv = 0;
            if (metalio_bq27220_read_voltage_mv(&mv) == 0 && mv > 0) {
                std::snprintf(volt_str, sizeof(volt_str), "%.2fV",
                              mv / 1000.0f);
            }

            char buf[64];
            if (charging) {
                std::snprintf(buf, sizeof(buf), I18n::T("电量 %d%% 充电中 %s"),
                              battery_level, volt_str);
            } else {
                std::snprintf(buf, sizeof(buf), I18n::T("电量 %d%% %s"),
                              battery_level, volt_str);
            }
            lv_label_set_text(st->battery_pct_lbl, buf);
            st->last_battery_pct = battery_level;

            if (low != st->last_battery_low) {
                st->last_battery_low = low;
                uint32_t color = low ? 0xF87171 : 0xFFFFFF;
                lv_obj_set_style_text_color(st->battery_pct_lbl,
                                            lv_color_hex(color), LV_PART_MAIN);
            }
        } else {
            if (st->last_battery_pct != -1) {
                st->last_battery_pct = -1;
                lv_label_set_text(st->battery_pct_lbl, I18n::T("电量 --%"));
            }
            if (st->last_battery_low) {
                st->last_battery_low = false;
                lv_obj_set_style_text_color(st->battery_pct_lbl,
                                            lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
        }
    }
#endif

    if (st->time_lbl != nullptr) {
        time_t now = time(nullptr);
        struct tm tm_info = {};
        if (localtime_r(&now, &tm_info) != nullptr &&
            tm_info.tm_year >= 2025 - 1900) {
            char time_str[16];
            /* Claw4: HH:MM only — leave room for「验证码: NNNNNN」on the right. */
            strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);
            lv_label_set_text(st->time_lbl, time_str);
        } else {
            lv_label_set_text(st->time_lbl, "--:--");
        }
    }

    if (st->activation_code_lbl != nullptr) {
        auto &app = Application::GetInstance();
        if (app.HasPendingActivation()) {
            char buf[40];
            /* Literal format — I18n::T("验证码: %s") as snprintf format made
             * garbled glyphs when the locale table remapped the string. */
            std::snprintf(buf, sizeof(buf), "验证码: %s",
                          app.GetPendingActivationCode().c_str());
            if (st->last_activation_text != buf) {
                st->last_activation_text = buf;
                lv_label_set_text(st->activation_code_lbl, buf);
            }
            if (!st->last_activation_visible) {
                st->last_activation_visible = true;
                lv_obj_remove_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (st->last_activation_visible ||
                   !st->last_activation_text.empty()) {
            st->last_activation_visible = false;
            st->last_activation_text.clear();
            lv_label_set_text(st->activation_code_lbl, "");
            lv_obj_add_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        /* Drop stale/non-digit pending so it cannot reappear as 乱码. */
        if (!app.HasPendingActivation() &&
            !app.GetPendingActivationCode().empty()) {
            app.ClearPendingActivation();
        }
    }
}

void OnHomeStatusTimer(lv_timer_t *timer)
{
    UpdateHomeStatusBar(static_cast<HomeStatusState *>(lv_timer_get_user_data(timer)));
}

void OnHomeStatusDeleted(lv_event_t *e)
{
    auto *st = static_cast<HomeStatusState *>(lv_event_get_user_data(e));
    if (st == nullptr) return;
    if (s_home_status == st) {
        s_home_status = nullptr;
    }
    if (st->update_timer != nullptr) {
        lv_timer_delete(st->update_timer);
        st->update_timer = nullptr;
    }
    delete st;
}

lv_obj_t *CreateStatusBar(lv_obj_t *screen, HomeStatusState *st)
{
    constexpr int kStatusLeftWidth  = 300;
    constexpr int kStatusRightWidth = 400;

    lv_obj_t *bar = lv_obj_create(screen);
    st->bar = bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kPanelSize, kStatusBarHeight);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kStatusBarBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, 8, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *left = lv_obj_create(bar);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, kStatusLeftWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 10, LV_PART_MAIN);

    st->network_icon_lbl = lv_label_create(left);
    lv_label_set_text(st->network_icon_lbl, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(st->network_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->network_icon_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    st->network_type_lbl = lv_label_create(left);
    lv_label_set_long_mode(st->network_type_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->network_type_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->network_type_lbl, "WiFi");
    lv_obj_set_style_text_font(st->network_type_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->network_type_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    st->sim_slot_lbl = lv_label_create(left);
    lv_label_set_long_mode(st->sim_slot_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->sim_slot_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->sim_slot_lbl, "");
    lv_obj_set_style_text_font(st->sim_slot_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->sim_slot_lbl, lv_color_hex(0xC9D1D9), LV_PART_MAIN);
    lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, kStatusRightWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
#if HOME_STATUS_SHOW_BATTERY_ICON
    st->battery_icon_lbl = lv_label_create(right);
    lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_FULL);
    lv_obj_set_style_text_font(st->battery_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_icon_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
#else
    lv_obj_set_style_pad_column(right, 14, LV_PART_MAIN);
    st->battery_pct_lbl = lv_label_create(right);
    lv_label_set_long_mode(st->battery_pct_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->battery_pct_lbl, 380);
    lv_label_set_text(st->battery_pct_lbl, I18n::T("电量 --%"));
    lv_obj_set_style_text_align(st->battery_pct_lbl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(st->battery_pct_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_pct_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
#endif

    // Center floating container: time + activation code
    lv_obj_t *center = lv_obj_create(bar);
    lv_obj_add_flag(center, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(center, 8, LV_PART_MAIN);
    lv_obj_align(center, LV_ALIGN_CENTER, 0, 0);

    st->time_lbl = lv_label_create(center);
    lv_label_set_long_mode(st->time_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->time_lbl, 80);
    lv_label_set_text(st->time_lbl, "--:--");
    lv_obj_set_style_text_align(st->time_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(st->time_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->time_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    st->activation_code_lbl = lv_label_create(center);
    lv_label_set_long_mode(st->activation_code_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->activation_code_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->activation_code_lbl, "");
    lv_obj_set_style_text_font(st->activation_code_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->activation_code_lbl, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_add_flag(st->activation_code_lbl, LV_OBJ_FLAG_HIDDEN);

    UpdateHomeStatusBar(st);
    /* Delay the 1 Hz refresh until after boot→home paint settles (starting it
     * immediately raced OTA/MQTT and could solid-blue the panel). */
    st->update_timer = nullptr;
    lv_timer_t *arm = lv_timer_create(
        [](lv_timer_t *t) {
            auto *hst = static_cast<HomeStatusState *>(lv_timer_get_user_data(t));
            if (hst != nullptr && hst->update_timer == nullptr &&
                s_home_status == hst) {
                hst->update_timer = lv_timer_create(OnHomeStatusTimer, 1000, hst);
                UpdateHomeStatusBar(hst);
            }
        },
        2500, st);
    lv_timer_set_repeat_count(arm, 1);
    s_home_status = st;

    lv_obj_add_event_cb(screen, OnHomeStatusDeleted, LV_EVENT_DELETE, st);
    return bar;
}

// ---------------------------------------------------------------------------
// Pager + page-indicator
// ---------------------------------------------------------------------------
struct PagerState {
    lv_obj_t *pager;
    lv_obj_t *indicator = nullptr;
    lv_obj_t *dots[kMaxPages];
    int page_count;
    int current_page;
    bool skeleton_active = false;
};

int s_last_home_page = 0;

void SetPagerSkeletonMode(PagerState *state, bool active)
{
    if (state == nullptr || state->pager == nullptr ||
        state->skeleton_active == active) {
        return;
    }
    state->skeleton_active = active;

    /* Match Claw4: only hide chrome + swap icon/stub. Pages are already
     * opaque black from CreatePage — do not re-style them every swipe. */
    if (state->indicator != nullptr) {
        if (active) {
            lv_obj_add_flag(state->indicator, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(state->indicator, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_home_status != nullptr && s_home_status->bar != nullptr) {
        lv_obj_set_style_bg_opa(s_home_status->bar,
                                active ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    }

    const uint32_t page_child_count = lv_obj_get_child_count(state->pager);
    for (uint32_t p = 0; p < page_child_count; ++p) {
        lv_obj_t *page = lv_obj_get_child(state->pager, p);
        if (page == nullptr) continue;
        const uint32_t cell_count = lv_obj_get_child_count(page);
        for (uint32_t c = 0; c < cell_count; ++c) {
            lv_obj_t *cell = lv_obj_get_child(page, c);
            if (cell == nullptr || !lv_obj_has_flag(cell, kAppCellFlag)) continue;
            lv_obj_t *icon = lv_obj_get_child(cell, 0);
            lv_obj_t *skeleton = lv_obj_get_child(cell, 1);
            lv_obj_t *name = lv_obj_get_child(cell, 2);
            if (icon == nullptr || skeleton == nullptr) continue;
            if (active) {
                lv_obj_remove_state(cell, LV_STATE_PRESSED);
                lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
                if (name != nullptr) {
                    lv_obj_add_flag(name, LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                lv_obj_add_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
                if (name != nullptr) {
                    lv_obj_remove_flag(name, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
}

void HighlightDot(PagerState *state, int page)
{
    if (page < 0 || page >= state->page_count) return;
    state->current_page = page;
    for (int i = 0; i < state->page_count; ++i) {
        lv_opa_t opa = (i == page) ? LV_OPA_COVER : LV_OPA_40;
        lv_obj_set_style_bg_opa(state->dots[i], opa, LV_PART_MAIN);
    }
}

bool PagerLoopEnabled(const PagerState *state)
{
    return state != nullptr && state->page_count > 1;
}

int32_t PagerScrollXForPage(const PagerState *state, int logical_page)
{
    if (state == nullptr) return 0;
    const int physical = PagerLoopEnabled(state) ? logical_page + 1 : logical_page;
    return static_cast<int32_t>(physical) * kPanelSize;
}

void PagerMaybeWrapAfterScroll(PagerState *state)
{
    if (!PagerLoopEnabled(state) || state->pager == nullptr) return;
    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    if (scroll_x == 0) {
        const int last = state->page_count - 1;
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, last),
                           LV_ANIM_OFF);
        HighlightDot(state, last);
        s_last_home_page = last;
    } else if (scroll_x == static_cast<int32_t>(state->page_count + 1) * kPanelSize) {
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, 0),
                           LV_ANIM_OFF);
        HighlightDot(state, 0);
        s_last_home_page = 0;
    }
}

constexpr uint32_t kPageSlideAnimMs = 180;

void OnPagerScrollBegin(lv_event_t *e)
{
    lv_anim_t *a = lv_event_get_scroll_anim(e);
    if (a == nullptr) return;
    lv_anim_set_duration(a, kPageSlideAnimMs);
    lv_anim_set_path_cb(a, lv_anim_path_ease_out);
}

enum class HomeTouchKind {
    None, SwipeLeft, SwipeRight, SwipeUp, SwipeDown, Click, LongPress,
};

enum class HomeGestureAxis { None, Horizontal, Vertical };

struct HomeTouchSession {
    bool active = false;
    bool consumed = false;
    bool paging = false;
    HomeGestureAxis axis = HomeGestureAxis::None;
    int16_t start_x = 0;
    int16_t start_y = 0;
    int16_t last_x = 0;
    int pending_scroll_dx = 0;
    uint32_t last_scroll_tick = 0;
    uint32_t press_tick = 0;
    lv_obj_t *press_cell = nullptr;
    const AppEntry *app = nullptr;
};
HomeTouchSession s_home_touch;

/* Apply coalesced finger deltas — one scroll_by per ~16 ms cuts full-plane
 * FBIO_UPDATE storms that make swipe feel stuck on RGB888 DIRECT. */
void HomeTouchFlushPendingScroll(PagerState *state, bool force)
{
    if (state == nullptr || state->pager == nullptr) return;
    if (s_home_touch.pending_scroll_dx == 0) return;
    const uint32_t now = lv_tick_get();
    if (!force) {
        const uint32_t dt = now - s_home_touch.last_scroll_tick;
        if (dt < 16 &&
            std::abs(s_home_touch.pending_scroll_dx) < 28) {
            return;
        }
    }
    lv_obj_scroll_by(state->pager, s_home_touch.pending_scroll_dx, 0,
                     LV_ANIM_OFF);
    s_home_touch.pending_scroll_dx = 0;
    s_home_touch.last_scroll_tick = now;
}

void OnPagerScrollEnd(lv_event_t *e)
{
    auto *state = static_cast<PagerState *>(lv_event_get_user_data(e));
    if (state == nullptr || state->pager == nullptr) return;
    // Defer skeleton cleanup to OnHomeReleased when a drag is in progress.
    if (s_home_touch.active && s_home_touch.paging) {
        return;
    }
    if (!lv_obj_is_scrolling(state->pager)) {
        PagerMaybeWrapAfterScroll(state);
        SetPagerSkeletonMode(state, false);
    }
}

void GoToPage(PagerState *state, int target_page)
{
    if (state == nullptr || state->pager == nullptr) return;
    if (target_page < 0 || target_page >= state->page_count) {
        if (!PagerLoopEnabled(state)) return;
        target_page = (target_page % state->page_count + state->page_count) %
                      state->page_count;
    }

    const int current = state->current_page;
    int32_t target_x = PagerScrollXForPage(state, target_page);
    if (PagerLoopEnabled(state)) {
        if (target_page == 0 && current == state->page_count - 1) {
            target_x = static_cast<int32_t>(state->page_count + 1) * kPanelSize;
        } else if (target_page == state->page_count - 1 && current == 0) {
            target_x = 0;
        }
    }

    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    if (target_page == current && scroll_x == target_x) {
        SetPagerSkeletonMode(state, false);
        return;
    }
    SetPagerSkeletonMode(state, true);
    lv_obj_scroll_to_x(state->pager, target_x, LV_ANIM_ON);
    HighlightDot(state, target_page);
    s_last_home_page = target_page;
    ResetHomeIdleTimer();
}

constexpr int kHomeMoveThreshold       = 5;
constexpr int kHomeAxisLockThreshold   = 12;
constexpr int kPageSnapThreshold       = kPanelSize / 5;
constexpr int kHomeFlickThreshold      = 24;
constexpr uint32_t kHomeLongPressMs    = 750;

void SnapPagerToNearestPage(PagerState *state, int release_dx)
{
    if (state == nullptr || state->pager == nullptr) return;
    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    const int32_t anchor_x = PagerScrollXForPage(state, state->current_page);
    const int delta = static_cast<int>(scroll_x) - anchor_x;

    int target = state->current_page;
    if (delta > kPageSnapThreshold ||
        (release_dx <= -kHomeFlickThreshold && delta > kHomeMoveThreshold)) {
        target = state->current_page + 1;
    } else if (delta < -kPageSnapThreshold ||
               (release_dx >= kHomeFlickThreshold && delta < -kHomeMoveThreshold)) {
        target = state->current_page - 1;
    }

    if (target < 0) {
        if (PagerLoopEnabled(state) && state->current_page == 0) {
            GoToPage(state, state->page_count - 1);
            return;
        }
        target = 0;
    }
    if (target >= state->page_count) {
        if (PagerLoopEnabled(state) &&
            state->current_page == state->page_count - 1) {
            GoToPage(state, 0);
            return;
        }
        target = state->page_count - 1;
    }
    GoToPage(state, target);
}

bool HomeTouchIsHorizontalSlide(int dx, int dy)
{
    const int adx = std::abs(dx);
    const int ady = std::abs(dy);
    return adx >= kHomeAxisLockThreshold && adx * 2 > ady * 3;
}

bool HomeTouchIsVerticalSlide(int dx, int dy)
{
    const int adx = std::abs(dx);
    const int ady = std::abs(dy);
    return ady >= kHomeAxisLockThreshold && ady * 2 > adx * 3;
}

void HomeTouchUpdateAxisLock(int dx, int dy)
{
    if (s_home_touch.axis != HomeGestureAxis::None) return;
    if (HomeTouchIsHorizontalSlide(dx, dy)) {
        s_home_touch.axis = HomeGestureAxis::Horizontal;
        return;
    }
    if (HomeTouchIsVerticalSlide(dx, dy)) {
        s_home_touch.axis = HomeGestureAxis::Vertical;
        s_home_touch.consumed = true;
    }
}

bool HomeTouchIsTapLike(int dx, int dy)
{
    return std::abs(dx) < kHomeMoveThreshold && std::abs(dy) < kHomeMoveThreshold;
}

HomeTouchKind HomeTouchClassifySwipe(int dx, int dy)
{
    if (HomeTouchIsTapLike(dx, dy)) return HomeTouchKind::None;
    if (HomeTouchIsHorizontalSlide(dx, dy)) {
        return dx < 0 ? HomeTouchKind::SwipeLeft : HomeTouchKind::SwipeRight;
    }
    if (HomeTouchIsVerticalSlide(dx, dy)) {
        return dy < 0 ? HomeTouchKind::SwipeUp : HomeTouchKind::SwipeDown;
    }
    return HomeTouchKind::None;
}

void HomeTouchHandleSwipe(PagerState *state, HomeTouchKind kind)
{
    if (state == nullptr) return;
    switch (kind) {
        case HomeTouchKind::SwipeLeft:
            GoToPage(state, state->current_page + 1);
            break;
        case HomeTouchKind::SwipeRight:
            GoToPage(state, state->current_page - 1);
            break;
        default:
            break;
    }
}

void HomeTouchDispatchTapLike(HomeTouchKind kind)
{
    if (s_home_touch.press_cell == nullptr) return;
    if (kind == HomeTouchKind::Click) {
        if (s_home_touch.app == nullptr) return;
        PlayAppCellPressScale(s_home_touch.press_cell, s_home_touch.app);
        return;
    }
    if (kind == HomeTouchKind::LongPress) {
        PlayAppCellPressScale(s_home_touch.press_cell, nullptr);
    }
}

void HomeTouchTryStartPageDrag(PagerState *state, int dx, int dy, int current_x)
{
    if (s_home_touch.axis != HomeGestureAxis::Horizontal) return;
    if (s_home_touch.consumed && !s_home_touch.paging) return;
    if (!HomeTouchIsHorizontalSlide(dx, dy)) return;
    if (!s_home_touch.paging) {
        s_home_touch.paging = true;
        s_home_touch.consumed = true;
        if (state != nullptr && state->pager != nullptr) {
            /* Do not scroll_by(0,0) — that forced an extra full-frame
             * invalidate. Skeleton stubs hide icons for the drag. */
            SetPagerSkeletonMode(state, true);
        }
        s_home_touch.last_x = static_cast<int16_t>(current_x);
        s_home_touch.pending_scroll_dx = 0;
        s_home_touch.last_scroll_tick = lv_tick_get();
    }
}

void OnHomePressed(lv_event_t *e)
{
    if (s_home_touch.active) return;
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_obj_t *screen = lv_event_get_current_target_obj(e);
    lv_obj_t *cell = FindAppCellFromTarget(lv_event_get_target_obj(e), screen);

    s_home_touch.active = true;
    s_home_touch.consumed = false;
    s_home_touch.paging = false;
    s_home_touch.axis = HomeGestureAxis::None;
    s_home_touch.start_x = static_cast<int16_t>(p.x);
    s_home_touch.start_y = static_cast<int16_t>(p.y);
    s_home_touch.last_x = static_cast<int16_t>(p.x);
    s_home_touch.pending_scroll_dx = 0;
    s_home_touch.last_scroll_tick = lv_tick_get();
    s_home_touch.press_tick = lv_tick_get();
    s_home_touch.press_cell = cell;
    s_home_touch.app =
        cell != nullptr
            ? static_cast<const AppEntry *>(lv_obj_get_user_data(cell))
            : nullptr;
    ResetHomeIdleTimer();
}

void OnHomePressing(lv_event_t *e)
{
    if (!s_home_touch.active) return;
    auto *state = static_cast<PagerState *>(lv_event_get_user_data(e));
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int dx = p.x - s_home_touch.start_x;
    const int dy = p.y - s_home_touch.start_y;
    if (!HomeTouchIsTapLike(dx, dy)) {
        ResetHomeIdleTimer();
    }

    HomeTouchUpdateAxisLock(dx, dy);

    if (s_home_touch.axis != HomeGestureAxis::Horizontal) return;

    /* Skeleton once at drag start (HomeTouchTryStartPageDrag) — not every
     * PRESSING, which re-walked the whole icon tree. */
    if (!s_home_touch.consumed || s_home_touch.paging) {
        HomeTouchTryStartPageDrag(state, dx, dy, p.x);
    }

    if (s_home_touch.paging && state != nullptr && state->pager != nullptr) {
        const int delta_x = p.x - s_home_touch.last_x;
        if (delta_x != 0) {
            s_home_touch.pending_scroll_dx += delta_x;
            HomeTouchFlushPendingScroll(state, false);
        }
        s_home_touch.last_x = static_cast<int16_t>(p.x);
    }
}

void OnHomeReleased(lv_event_t *e)
{
    if (!s_home_touch.active) return;

    auto *state = static_cast<PagerState *>(lv_event_get_user_data(e));

    lv_indev_t *indev = lv_event_get_indev(e);
    int dx = 0;
    int dy = 0;
    if (indev != nullptr) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        dx = p.x - s_home_touch.start_x;
        dy = p.y - s_home_touch.start_y;
    }

    if (s_home_touch.axis == HomeGestureAxis::Vertical) {
        // Vertical swipe: no-op, keep icons visible.
    } else if (s_home_touch.paging && state != nullptr) {
        HomeTouchFlushPendingScroll(state, true);
        SnapPagerToNearestPage(state, dx);
    } else if (!s_home_touch.consumed) {
        const uint32_t elapsed = lv_tick_elaps(s_home_touch.press_tick);
        if (HomeTouchIsTapLike(dx, dy)) {
            const HomeTouchKind kind =
                elapsed < kHomeLongPressMs ? HomeTouchKind::Click
                                           : HomeTouchKind::LongPress;
            HomeTouchDispatchTapLike(kind);
        } else {
            const HomeTouchKind kind = HomeTouchClassifySwipe(dx, dy);
            if (state != nullptr && kind != HomeTouchKind::None) {
                HomeTouchHandleSwipe(state, kind);
            }
        }
    }

    if (!s_home_touch.paging && state != nullptr && state->pager != nullptr &&
        !lv_obj_is_scrolling(state->pager)) {
        const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
        const int32_t expected = PagerScrollXForPage(state, state->current_page);
        if (scroll_x == expected) {
            SetPagerSkeletonMode(state, false);
        }
    }

    s_home_touch.active = false;
    s_home_touch.consumed = false;
    s_home_touch.paging = false;
    s_home_touch.axis = HomeGestureAxis::None;
    s_home_touch.pending_scroll_dx = 0;
    s_home_touch.press_cell = nullptr;
    s_home_touch.app = nullptr;
}

void EnableHomeEventBubble(lv_obj_t *obj)
{
    if (obj == nullptr) return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        EnableHomeEventBubble(lv_obj_get_child(obj, i));
    }
}

void OnHomeScreenLoaded(lv_event_t *e)
{
    write(1, "HOME_LOAD\n", 10);
    lv_obj_t *scr = lv_event_get_current_target_obj(e);
    EnableHomeEventBubble(scr);

    auto *state = static_cast<PagerState *>(lv_event_get_user_data(e));
    if (state != nullptr && state->pager != nullptr) {
        /* Place the looped pager on page 0 without firing SCROLL_END yet. */
        lv_obj_remove_event_cb(state->pager, OnPagerScrollEnd);
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, 0),
                           LV_ANIM_OFF);
        HighlightDot(state, 0);
        s_last_home_page = 0;
        lv_obj_add_event_cb(state->pager, OnPagerScrollEnd, LV_EVENT_SCROLL_END,
                            state);
    }

    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_LOAD);
    StartHomeIdleTimer();
}

void OnHomeScreenUnloaded(lv_event_t * /*e*/)
{
    /* Detach idle policy immediately on unload. Waiting until DELETE left
     * IdlePower in Home session while chat/other apps were already active,
     * so auto-standby could steal the app screen and kill voice wake. */
    StopHomeIdleTimer();
    CancelCellScaleTimer();
    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_UNLOAD);
}

void OnScreenDeleted(lv_event_t *e)
{
    write(1, "HOME_DELETE\n", sizeof("HOME_DELETE\n") - 1);
    CancelCellScaleTimer();
    StopHomeIdleTimer();
    delete static_cast<PagerState *>(lv_event_get_user_data(e));
    write(1, "HOME_DELETE_DONE\n", sizeof("HOME_DELETE_DONE\n") - 1);
}

// ---------------------------------------------------------------------------
// Power options dialog (PWR_KEY long-press)
// ---------------------------------------------------------------------------
struct PowerDialogUi {
    lv_obj_t *mask = nullptr;
    lv_obj_t *card = nullptr;
};

PowerDialogUi s_pwr_dlg;

void ClosePowerDialog()
{
    if (s_pwr_dlg.mask != nullptr) {
        lv_obj_delete(s_pwr_dlg.mask);
    }
    s_pwr_dlg = PowerDialogUi{};
}

void OnPwrMaskClicked(lv_event_t *e);

lv_obj_t *s_shutdown_screen = nullptr;

void AppendShutdownProgressContent(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, 24, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *spin = lv_spinner_create(box);
    lv_obj_set_size(spin, 140, 140);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);
    lv_obj_remove_flag(spin, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(box);
    lv_label_set_text(lbl, I18n::T("正在关机..."));
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t *CreateShutdownScreen()
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    AppendShutdownProgressContent(screen);
    return screen;
}

void ShowShutdownScreen()
{
    ClosePowerDialog();
    if (s_shutdown_screen == nullptr) {
        s_shutdown_screen = CreateShutdownScreen();
    }
    lv_screen_load(s_shutdown_screen);
}

// Drive the board-level power-off pulse (TCA9555 PWR_KEY_PULSE) on a
// separate task so the shutdown screen spinner keeps animating until the
// power path actually cuts.
static void shutdown_pulse_task(void *arg)
{
    (void)arg;
    usleep(300 * 1000);   /* let the shutdown screen render first */
    metalio_pwr_shutdown_pulse();
}

void BeginSystemShutdown(const char *reason)
{
    static bool shutting_down = false;
    if (shutting_down) return;
    shutting_down = true;

    ESP_LOGW(TAG_HOME, "%s: shutdown requested",
             reason != nullptr ? reason : "?");
    IdlePower_Stop();
    ShowShutdownScreen();

    if (xTaskCreate(shutdown_pulse_task, "shutdown", 4096, nullptr, 5,
                    nullptr) != pdPASS)
    {
        metalio_pwr_shutdown_pulse();
    }
}

void OnPwrShutdownClicked(lv_event_t * /*e*/)
{
    BeginSystemShutdown(I18n::T("用户选择 [关机]"));
}

void OnPwrRebootClicked(lv_event_t * /*e*/)
{
    ESP_LOGW(TAG_HOME, "用户选择 [重启]: Application::Reboot()");
    ClosePowerDialog();
    /* Defer past the LVGL click handler so the dialog delete finishes
     * before backlight-off + chip reset. */
    if (lv_async_call(
            [](void * /*user_data*/) {
                write(1, "PWR_REBOOT\n", 11);
                Application::GetInstance().Reboot();
            },
            nullptr) != LV_RESULT_OK)
    {
        Application::GetInstance().Reboot();
    }
}

void OnPwrMaskClicked(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    ESP_LOGI(TAG_HOME, "点击模态框外，关闭电源对话框");
    ClosePowerDialog();
}

lv_obj_t *CreatePowerActionBtn(lv_obj_t *parent, const char *icon_src,
                               const char *text, lv_event_cb_t on_click)
{
    constexpr int kBtnSize     = 180;
    constexpr int kBtnIconSize = 96;

    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kBtnSize, kBtnSize);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_image_create(btn);
    lv_image_set_src(icon, icon_src);
    lv_obj_set_size(icon, kBtnIconSize, kBtnIconSize);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_pad_top(lbl, 12, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, nullptr);
    return btn;
}

void ShowPowerDialog()
{
    if (s_pwr_dlg.mask != nullptr) return;
    lv_obj_t *parent = lv_screen_active();
    if (parent == nullptr) return;

    lv_obj_t *mask = lv_obj_create(parent);
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    lv_obj_add_event_cb(mask, OnPwrMaskClicked, LV_EVENT_CLICKED, nullptr);
    s_pwr_dlg.mask = mask;

    constexpr int kCardW = 480;
    constexpr int kCardH = 360;
    lv_obj_t *card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    s_pwr_dlg.card = card;

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("电源选项"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 32, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    CreatePowerActionBtn(row, "A:ic_s_home_reboot.spng", I18n::T("重启"),
                         OnPwrRebootClicked);
    CreatePowerActionBtn(row, "A:ic_s_home_power.spng", I18n::T("关机"),
                         OnPwrShutdownClicked);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, I18n::T("长按关机键 5 秒可强制关机"));
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9CA3AF), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

// ---------------------------------------------------------------------------
// Page + indicator construction
// ---------------------------------------------------------------------------
lv_obj_t *CreatePage(lv_obj_t *pager, int page_index, int total_apps)
{
    lv_obj_t *page = lv_obj_create(pager);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, kPanelSize, kPagerHeight);
    /* Always opaque black — transparent pages force full-screen blends
     * on every scroll_by and cause swipe tear. */
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(page, kPagePadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(page, kPagePadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_column(page, kGridColGap, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, kGridRowGap, LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_grid_dsc_array(page, s_col_dsc, s_row_dsc);
    lv_obj_set_layout(page, LV_LAYOUT_GRID);

    const int start = page_index * kAppsPerPage;
    for (int i = 0; i < kAppsPerPage; ++i) {
        const int idx = start + i;
        if (idx >= total_apps) break;
        const AppEntry &app = kApps[idx];
        if (app.icon_suffix == nullptr) continue;
        lv_obj_t *cell = CreateAppCell(page, app, idx);
        const int col = i % kPageCols;
        const int row = i / kPageCols;
        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, col, 1,
                             LV_GRID_ALIGN_STRETCH, row, 1);
    }
    return page;
}

void CreateIndicator(lv_obj_t *screen, PagerState *state)
{
    lv_obj_t *indicator = lv_obj_create(screen);
    lv_obj_remove_style_all(indicator);
    lv_obj_set_size(indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, -kIndicatorYOffset);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(kIndicatorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(indicator, kIndicatorPadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(indicator, kIndicatorPadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_column(indicator, kDotGap, LV_PART_MAIN);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    state->indicator = indicator;

    for (int i = 0; i < state->page_count; ++i) {
        lv_obj_t *dot = lv_obj_create(indicator);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kDotSize, kDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(kDotColor), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_40, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        state->dots[i] = dot;
    }

    HighlightDot(state, 0);
}

const AppEntry *FindAppByQuery(const char *query)
{
    if (query == nullptr || query[0] == '\0')
        return nullptr;

    /* Cloud sometimes passes "打开日历" / "打开 相机" — strip verbs. */
    const char *q = query;
    while (*q == ' ' || *q == '\t')
        ++q;
    static const char *kPrefixes[] = {
        "打开", "开启", "启动", "进入", "去", "用", nullptr};
    for (int p = 0; kPrefixes[p] != nullptr; ++p) {
        const size_t n = std::strlen(kPrefixes[p]);
        if (std::strncmp(q, kPrefixes[p], n) == 0) {
            q += n;
            while (*q == ' ' || *q == '\t')
                ++q;
            break;
        }
    }
    if (q[0] == '\0')
        return nullptr;

    /* Voice aliases → canonical app id (icon_suffix). */
    static const struct {
        const char *alias;
        const char *id;
    } kAliases[] = {
        {"小智", "chat"},
        {"聊天", "chat"},
        {"对话", "chat"},
        {"照相", "camera"},
        {"拍照", "camera"},
        {"相机", "camera"},
        {"音乐", "music"},
        {"播放器", "music"},
        {"日历", "calendar"},
        {"日程", "calendar"},
        {"设置", "settings"},
        {"系统设置", "settings"},
        {"天气", "weather"},
        {"电台", "radio"},
        {"广播", "radio"},
        {"定位", "gps"},
        {"导航", "gps"},
        {"GPS", "gps"},
        {"水平仪", "spirit_level"},
        {"计算器", "calculator"},
        {"录音", "recording"},
        {"翻译", "translate"},
        {"磁场", "magnet"},
        {"指南针", "magnet"},
        {"震动", "vibrate"},
        {"主题", "theme"},
        {"测试", "test"},
        {"竹知了", "cicada"},
        {"知了", "cicada"},
        {"数字人", "digital_people"},
        {"背光", "backlight"},
        {"亮度", "backlight"},
        {"音量", "settings"},
        {"声音", "settings"},
        {"蓝牙", "bluetooth"},
        {"网络", "wifi"},
        {"无线", "wifi"},
        {"WiFi", "wifi"},
        {"wifi", "wifi"},
        {"SD卡", "sd"},
        {"存储", "sd"},
        {"副屏", "secondary_screen"},
        {"生图", "ai_image_gen"},
        {"AI生图", "ai_image_gen"},
        {"电话", "call"},
        {"通话", "call"},
        {"系统信息", "info"},
        {"信息", "info"},
        {"2048", "2048"},
        {"游戏", "2048"},
        {"引脚", "pin"},
        {"OpenClaw", "openclaw"},
        {"ESPClaw", "espclaw"},
        {nullptr, nullptr},
    };

    for (int i = 0; kAliases[i].alias != nullptr; ++i) {
        if (strcasecmp(kAliases[i].alias, q) == 0) {
            q = kAliases[i].id;
            break;
        }
    }

    for (int i = 0; i < kTotalApps; ++i) {
        if (strcasecmp(kApps[i].icon_suffix, q) == 0)
            return &kApps[i];
        if (strcmp(kApps[i].name, q) == 0)
            return &kApps[i];
    }
    /* Substring fallback: tolerate partial id like "cam". */
    for (int i = 0; i < kTotalApps; ++i) {
        if (strstr(kApps[i].name, q) != nullptr)
            return &kApps[i];
        if (strstr(kApps[i].icon_suffix, q) != nullptr)
            return &kApps[i];
    }
    return nullptr;
}

void OnMcpLaunchAppAsync(void *user_data)
{
    auto *app = static_cast<const AppEntry *>(user_data);
    if (app == nullptr)
        return;
    /* Voice SoftStop already ran on the Application thread in self.app.open.
     * Only create/load the target screen here (LVGL thread). */
    write(1, "MCP_LAUNCH\n", 11);
    LaunchHomeApp(app);
}

lv_obj_t *SwitchToHomeImpl()
{
    write(1, "HOME_RET\n", 9);
    Application::GetInstance().RequestVoiceUiDesired(false);

    lv_obj_t *old_scr = lv_screen_active();
    write(1, "HOME_CREATE\n", 12);
    lv_obj_t *home = HomeScreen::CreateStatic();
    if (home == nullptr) {
        write(1, "HOME_NULL\n", 10);
        return nullptr;
    }
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
    write(1, "HOME_OK\n", 8);
    return home;
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================

int HomeScreen::GetAppCount()
{
    return kTotalApps;
}

bool HomeScreen::GetAppInfo(int index, AppInfo *out)
{
    if (out == nullptr || index < 0 || index >= kTotalApps)
        return false;
    out->id = kApps[index].icon_suffix;
    out->name = kApps[index].name;
    out->available = (kApps[index].create != nullptr);
    return true;
}

bool HomeScreen::LaunchApp(const char *id_or_name)
{
    auto *app = FindAppByQuery(id_or_name);
    if (app == nullptr || app->create == nullptr)
        return false;
    /* MCP / app thread must not touch LVGL directly — marshal like bubbles. */
    return lv_async_call(OnMcpLaunchAppAsync,
                         const_cast<void *>(static_cast<const void *>(app))) ==
           LV_RESULT_OK;
}

lv_obj_t *HomeScreen::SwitchToHome()
{
    return SwitchToHomeImpl();
}

lv_obj_t *HomeScreen::CreateStatic()
{
    EnsureIconPathsBuilt();
    write(1, "HS0\n", 4);

    lv_obj_t *screen = lv_obj_create(NULL);
    /* Strip default theme (blue primary) before the first refresh. */
    lv_obj_remove_style_all(screen);
    screen_strip_obj_chrome(screen);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    int page_count = (kTotalApps + kAppsPerPage - 1) / kAppsPerPage;
    if (page_count < 1) page_count = 1;
    if (page_count > kMaxPages) page_count = kMaxPages;

    auto *state = new PagerState{};
    state->page_count = page_count;
    state->current_page = 0;

    auto *status = new HomeStatusState{};
    CreateStatusBar(screen, status);
    write(1, "HS1\n", 4);

    lv_obj_t *pager = lv_obj_create(screen);
    state->pager = pager;
    lv_obj_remove_style_all(pager);
    lv_obj_set_size(pager, kPanelSize, kPagerHeight);
    lv_obj_align(pager, LV_ALIGN_TOP_LEFT, 0, kStatusBarHeight);
    lv_obj_set_style_bg_color(pager, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pager, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(pager, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(pager, OnPagerScrollBegin, LV_EVENT_SCROLL_BEGIN, nullptr);

    if (page_count > 1) {
        CreatePage(pager, page_count - 1, kTotalApps);
        for (int p = 0; p < page_count; ++p) {
            CreatePage(pager, p, kTotalApps);
        }
        CreatePage(pager, 0, kTotalApps);
    } else {
        CreatePage(pager, 0, kTotalApps);
    }
    write(1, "HS2\n", 4);

    if (page_count > 1) {
        CreateIndicator(screen, state);
    }
    write(1, "HS3\n", 4);

    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, OnHomePressed, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(screen, OnHomePressing, LV_EVENT_PRESSING, state);
    lv_obj_add_event_cb(screen, OnHomeReleased, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(screen, OnHomeScreenLoaded, LV_EVENT_SCREEN_LOADED, state);
    lv_obj_add_event_cb(screen, OnHomeScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    lv_obj_add_event_cb(screen, OnScreenDeleted, LV_EVENT_DELETE, state);

    write(1, "HS4\n", 4);
    return screen;
}

lv_obj_t *HomeScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void OnRefreshStatusBarAsync(void * /*user_data*/)
{
    if (s_home_status != nullptr) {
        UpdateHomeStatusBar(s_home_status);
    }
}

void HomeScreen::ResetToFirstPage()
{
    s_last_home_page = 0;
}

void HomeScreen::RefreshStatusBar()
{
    lv_async_call(OnRefreshStatusBarAsync, nullptr);
}

void HomeScreen::ShowPowerOptionsDialog()
{
    ShowPowerDialog();
}

int HomeScreen::GetIdleShutdownMinutes()
{
    return IdlePower_GetShutdownMinutes();
}

void HomeScreen::SetIdleShutdownMinutes(int minutes)
{
    IdlePower_SetShutdownMinutes(minutes);
    IdlePower_NotifyActivity();
}

int HomeScreen::GetIdleStandbyMinutes()
{
    return IdlePower_GetStandbyMinutes();
}

void HomeScreen::SetIdleStandbyMinutes(int minutes)
{
    IdlePower_SetStandbyMinutes(minutes);
    IdlePower_NotifyActivity();
}

void HomeScreen::RequestSystemShutdown(const char *reason)
{
    BeginSystemShutdown(reason != nullptr ? reason : I18n::T("系统关机"));
}
