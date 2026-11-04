#include "../app_query.h"
#include "hi_task_policy.h"
#include "navigation_index.h"
#include <metalio/metalio.h>
extern "C" int metalio_nt26_get_sim_slot(void);
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
#include "desktop_folders.h"
#include "desktop_storage_queue.h"
#include "desktop_drag.h"
#include "hi_input_method.h"
#include "registry.h"
#include <vector>
#include "battery_indicator.h"
#include "ai_image_gen_screen/ai_image_gen_screen.h"
#include "src/core/lv_obj_private.h"
#include "src/draw/lv_image_decoder_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "panel_screen/panel_screen.h"
#include "system_chrome/control_center.h"
#include "screen_util.h"
#include "home_app_screen/home_app_screen.h"
#include "home_icon.h"
#include "panel_screen/panel_screen.h"
#include "home_app_screen/home_app_screen.h"
#include "home_icon.h"
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
#include "call_screen/call_screen.h"
#include "calendar_screen/calendar_screen.h"
#include "camera_screen/camera_screen.h"
#include "calculator_screen/calculator_screen.h"
#include "pin_test_screen/pin_test_screen.h"
#include "game_2048_screen/game_2048_screen.h"
#include "arcade_screen/arcade_screen.h"
#include "xiangqi_screen/xiangqi_screen.h"
#include "translate_screen/translate_screen.h"
#include "secondary_screen/secondary_screen.h"
#include "digital_people_screen/digital_people_screen.h"
#include "weather_screen/weather_screen.h"
#include "music_screen/music_screen.h"
#include "hi_music_screen/hi_music_screen.h"
#include "sms_screen/sms_screen.h"
#include "clock_screen/clock_screen.h"
#include "compass_screen/compass_screen.h"
#include "app_manager_screen/app_manager_screen.h"
#include "runtime.h"
#include "gps_screen/gps_screen.h"
#include "radio_screen/radio_screen.h"
#include "network_screen/network_screen.h"
#include "openclaw_screen/openclaw_screen.h"
#include "recording_screen/recording_screen.h"
#include "pwr_key_handler/pwr_key_handler.h"
#include "idle_power_policy/idle_power_policy.h"
#include "wifi_required_dialog/wifi_required_dialog.h"
#include "system_chrome/system_chrome.h"
#include "screen_util.h"
#include "settings.h"

#include <cstdlib>
#include <cstdio>
#include <nuttx/arch.h>
#include <unistd.h>
#include <pthread.h>
#include <mutex>
#include <sched.h>
#include <cctype>
#include <ctime>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <stdint.h>
#include <strings.h>

#ifdef HI_STABILITY_PROBE
#include <fcntl.h>
#include <sys/ioctl.h>
#include <nuttx/video/fb.h>
#include <arch/board/metalio/display_health.h>
#include "hi_music_screen/local_music_player.h"
#include "radio_screen/radio_player.h"
#include "standby_screen/standby_screen.h"
#endif

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);

extern "C" {
extern const uint8_t metalio_ic_hi_music_logo_data[];
extern const uint32_t metalio_ic_hi_music_logo_data_size;
}

/* Keep the percentage inside the battery outline even while charging. */
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
void tank_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("tank", event);
}
void arcade_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("arcade", event);
}
void xiangqi_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("xiangqi", event);
    XiangqiScreen::LifecycleCallback(event);
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

void translate_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("translate", event);
    TranslateScreen::LifecycleCallback(event);
}

void ai_image_gen_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("ai_image_gen", event);
    AiImageGenScreen::LifecycleCallback(event);
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

void hi_music_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("hi_music", event);
    HiMusicScreen::LifecycleCallback(event);
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
constexpr int kIndicatorYOffset  = 34;
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
    bool requires_network;
};

void home_app_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("home_app", event);
    HomeAppScreen::LifecycleCallback(event);
}

constexpr AppEntry kApps[] = {
    {"home", "Hi 家", HomeAppScreen::CreateStatic, home_app_lifecycle_cb, false},
    {"chat",            "聊天",     ChatScreen::CreateStatic,       chat_lifecycle_cb,       true},
    {"wifi",            "网络配置", NetworkScreen::CreateStatic,    network_lifecycle_cb,    false},
    {"digital_people",  "小白助手",   DigitalPeopleScreen::CreateStatic, digital_people_lifecycle_cb, true},
    {"call",            "电话",     CallScreen::CreateStatic,       call_lifecycle_cb,       false},
    {"music",           "蓝牙音乐",     MusicScreen::CreateStatic,      music_lifecycle_cb,      false},
    {"hi_music",        "Hi 音乐",  HiMusicScreen::CreateStatic,    hi_music_lifecycle_cb,   false},
    {"calendar",        "日历",     CalendarScreen::CreateStatic,   calendar_lifecycle_cb,   false},
    {"openclaw",        "Hi 万物", OpenClawScreen::CreateStatic,   openclaw_lifecycle_cb,   true},
    {"camera",          "相机",     CameraScreen::CreateStatic,     camera_lifecycle_cb,     false},
    {"gps",             "定位",     GpsScreen::CreateStatic,        gps_lifecycle_cb,        true},
    {"spirit_level",    "水平仪",   LevelScreen::CreateStatic,      level_lifecycle_cb,      false},
    {"magnet",          "磁场",     MagnetScreen::CreateStatic,     magnet_lifecycle_cb,     false},
    {"vibrate",         "震动",     VibrateScreen::CreateStatic,    vibrate_lifecycle_cb,    false},
    {"calculator",      "计算器",   CalculatorScreen::CreateStatic, calculator_lifecycle_cb, false},
    {"weather",         "天气",     WeatherScreen::CreateStatic,    weather_lifecycle_cb,    true},
    {"sd",              "文件管理",     SdCardScreen::CreateStatic,     sd_card_lifecycle_cb,    false},
    {"pin",             "引脚测试", PinTestScreen::CreateStatic,    pin_test_lifecycle_cb,   false},
    {"2048",            "2048",     Game2048Screen::CreateStatic,   game_2048_lifecycle_cb,  false},
    {"tank",            "坦克大战", TankGameScreen::CreateStatic,   tank_lifecycle_cb,       false},
    {"link_game",       "连连看",   LinkGameScreen::CreateStatic,   arcade_lifecycle_cb,     false},
    {"fruit_game",      "水果消消乐", FruitGameScreen::CreateStatic, arcade_lifecycle_cb,    false},
    {"xiangqi",         "中国象棋", XiangqiScreen::CreateStatic,    xiangqi_lifecycle_cb,    false},
    {"info",            "系统信息", InfoScreen::CreateStatic,       info_lifecycle_cb,       false},
    {"theme",           "主题",     ThemeScreen::CreateStatic,      theme_lifecycle_cb,      false},
    {"test",            "测试",     TestScreen::CreateStatic,       test_lifecycle_cb,       false},
    {"settings",        "设置",     SettingsScreen::CreateStatic,   settings_lifecycle_cb,   false},
    {"radio",           "电台",     RadioScreen::CreateStatic,      radio_lifecycle_cb,      true},
    {"recording",       "录音",     RecordingScreen::CreateStatic,  recording_lifecycle_cb,  false},
    {"ai_image_gen",    "AI生图",   AiImageGenScreen::CreateStatic, ai_image_gen_lifecycle_cb, true},
    {"translate",       "翻译",     TranslateScreen::CreateStatic,  translate_lifecycle_cb,  true},
    {"secondary_screen","副屏",    SecondaryScreen::CreateStatic,  secondary_lifecycle_cb,  false},
    {"bluetooth",       "蓝牙",     BluetoothScreen::CreateStatic,  bluetooth_lifecycle_cb,  false},
    {"sms", "短信", SmsScreen::CreateStatic, [](screen_lifecycle_event_t e){PwrKey_OnScreenLifecycle("sms",e);}, false},
    {"compass", "Hi 指南", CompassScreen::CreateStatic, [](screen_lifecycle_event_t e){PwrKey_OnScreenLifecycle("compass",e);}, false},
    {"clock", "时钟", ClockScreen::CreateStatic, [](screen_lifecycle_event_t e){PwrKey_OnScreenLifecycle("clock",e);}, false},
    {"app_manager", "应用管理", AppManagerScreen::CreateStatic, nullptr, false},
};

constexpr int kTotalApps = static_cast<int>(sizeof(kApps) / sizeof(kApps[0]));

// Keep bounded launcher scratch capacity stable when retiring two apps.
constexpr int kAppStorageSlots = kTotalApps + 1;
constexpr int kIconPathBufSize = 56;
/* Two banks so a theme switch hands LVGL a new path pointer. The image
 * cache keys decoded bitmaps by that pointer; rewriting one buffer in
 * place left the previous theme on screen. */
char s_icon_paths[2][kAppStorageSlots][kIconPathBufSize];
int s_icon_bank = 0;
/* Pager clones pages, so one app can own several icon widgets. */
constexpr int kMaxThemeIcons = kAppStorageSlots * 3;
lv_obj_t *s_theme_icons[kMaxThemeIcons];
int s_theme_icon_app[kMaxThemeIcons];
int s_theme_icon_n = 0;
int s_icons_theme_id = -1;

void NoteThemeIcon(int app_idx, lv_obj_t *icon)
{
    if (icon == nullptr || app_idx < 0 || app_idx >= kTotalApps) {
        return;
    }
    if (s_theme_icon_n >= kMaxThemeIcons) {
        return;
    }
    // Deregister at the icon's own DELETE event, before its memory is freed.
    // A parent callback cannot safely inspect already deleted descendants.
    lv_obj_add_event_cb(icon, [](lv_event_t* e) {
        auto* deleted = lv_event_get_target_obj(e);
        for (int i = s_theme_icon_n - 1; i >= 0; --i) {
            if (s_theme_icons[i] != deleted) continue;
            --s_theme_icon_n;
            s_theme_icons[i] = s_theme_icons[s_theme_icon_n];
            s_theme_icon_app[i] = s_theme_icon_app[s_theme_icon_n];
            s_theme_icons[s_theme_icon_n] = nullptr;
        }
    }, LV_EVENT_DELETE, nullptr);
    s_theme_icons[s_theme_icon_n] = icon;
    s_theme_icon_app[s_theme_icon_n] = app_idx;
    ++s_theme_icon_n;
}

void EnsureIconPathsBuilt()
{
    static bool built = false;
    static int s_built_theme_id = 0;
    const int tid = ThemeManager::GetCurrentThemeId();
    if (built && s_built_theme_id == tid) {
        return;
    }
    for (int i = 0; i < kTotalApps; ++i) {
        std::snprintf(s_icon_paths[s_icon_bank][i], kIconPathBufSize,
                      "A:ic_app_home_theme%d_%s.spng",
                      tid, kApps[i].icon_suffix);
    }
    s_built_theme_id = tid;
    built = true;
    ESP_LOGI(TAG_HOME, "icon paths built for theme%d", tid);
}

const char *AppIconPath(int idx)
{
    if (idx < 0 || idx >= kTotalApps) return "";
    if (std::strcmp(kApps[idx].icon_suffix, "hi_music") == 0)
        return "A:ic_app_home_hi_music.png";
    return s_icon_paths[s_icon_bank][idx];
}

const void *AppIconSource(int idx)
{
    if (idx < 0 || idx >= kTotalApps) return "";
    if (std::strcmp(kApps[idx].icon_suffix, "hi_music") != 0)
        return AppIconPath(idx);

    /* Keep Hi Music independent of the A: filesystem and filename lookup.
     * LODEPNG accepts a PNG byte array through an LVGL image descriptor. */
    static const lv_image_dsc_t hi_music_logo = [] {
        lv_image_dsc_t image{};
        image.header.magic = LV_IMAGE_HEADER_MAGIC;
        image.header.cf = LV_COLOR_FORMAT_RAW;
        image.header.w = 128;
        image.header.h = 128;
        image.data_size = metalio_ic_hi_music_logo_data_size;
        image.data = metalio_ic_hi_music_logo_data;
        return image;
    }();
    return &hi_music_logo;
}

struct HomeIconBitmap {
    lv_draw_buf_t *pixels = nullptr;
    lv_image_dsc_t image{};
};
HomeIconBitmap s_home_icon_bitmaps[2][kAppStorageSlots];

void ReleaseHomeIconBank(int bank)
{
    for (auto &icon : s_home_icon_bitmaps[bank]) {
        if (icon.pixels == nullptr) continue;
        lv_image_cache_drop(&icon.image);
        lv_draw_buf_destroy(icon.pixels);
        icon = HomeIconBitmap{};
    }
}

const void *HomeIconSource(int idx)
{
    const void *source = AppIconSource(idx);
    if (idx < 0 || idx >= kTotalApps) return source;
    auto &icon = s_home_icon_bitmaps[s_icon_bank][idx];
    if (icon.pixels != nullptr) return &icon.image;

    /* Launcher cells always sit on opaque black. Composite their PNG once
     * into the panel's native format, instead of blending every icon pixel
     * on every drag frame. Apps still use the original transparent assets.
     * Cloned end pages share the same bounded, per-theme bitmap bank. */
    lv_image_decoder_dsc_t decoder{};
    lv_image_decoder_args_t args{};
    args.no_cache = true;
    if (lv_image_decoder_open(&decoder, source, &args) != LV_RESULT_OK) return source;
    const lv_draw_buf_t *decoded = decoder.decoded;
    if (decoded == nullptr || decoded->header.cf != LV_COLOR_FORMAT_ARGB8888 ||
        decoded->header.w != kIconSize || decoded->header.h != kIconSize ||
        (decoded->header.flags & LV_IMAGE_FLAGS_PREMULTIPLIED)) {
        lv_image_decoder_close(&decoder);
        return source;
    }
    lv_draw_buf_t *pixels = lv_draw_buf_create(kIconSize, kIconSize,
                                              LV_COLOR_FORMAT_RGB888, 0);
    if (pixels == nullptr) {
        lv_image_decoder_close(&decoder);
        return source;
    }
    for (int y = 0; y < kIconSize; ++y) {
        const uint8_t *src = decoded->data + y * decoded->header.stride;
        uint8_t *dst = pixels->data + y * pixels->header.stride;
        for (int x = 0; x < kIconSize; ++x, src += 4, dst += 3) {
            const unsigned alpha = src[3];
            for (int c = 0; c < 3; ++c) {
                dst[c] = alpha >= LV_OPA_MAX ? src[c] : (src[c] * alpha) >> 8;
            }
        }
    }
    lv_image_decoder_close(&decoder);
    icon.pixels = pixels;
    icon.image.header = pixels->header;
    /* This is an image descriptor, not an lv_draw_buf_t. In particular,
     * ALLOCATED would make the binary decoder cast it to the larger type. */
    icon.image.header.flags = 0;
    icon.image.data_size = pixels->data_size;
    icon.image.data = pixels->data;
    return &icon.image;
}

/* Desktop slot → kApps[] index. Identity until the user rearranges. */
std::vector<int> s_app_order;
std::vector<HiApp::Record> s_external_apps;
bool s_order_dirty=false;
std::vector<DesktopFolders::Folder> s_desktop_folders;
bool s_folder_refresh=false, s_folder_saving=false;
lv_obj_t* s_folder_sheet=nullptr;
void OpenDesktopFolder(const std::string& id);
bool SubmitDesktopLayout(const DesktopFolders::Model& model,bool refresh=true,bool keep_folder=false);
lv_obj_t* CreateFolderCell(lv_obj_t* parent,int index);
bool s_app_order_ready = false;
/* Logical pager page to restore after an app returns (not always page 0). */
int s_last_home_page = 0;
/* Page that owns the app we last opened — back button restores this. */
int s_home_return_page = 0;


struct AppCellInfo {
    const AppEntry *app = nullptr;
    std::string external_id;
    std::string folder_id;
    int app_idx = -1;
    int local_slot = -1;
};

void InitIdentityAppOrder()
{
    s_app_order.resize(kTotalApps);
    for (int i = 0; i < kTotalApps; ++i) {
        s_app_order[i] = i;
    }
}

int FindAppIndexBySuffix(const char *suffix)
{
    if (suffix == nullptr) return -1;
    for (int i = 0; i < kTotalApps; ++i) {
        if (kApps[i].icon_suffix != nullptr &&
            std::strcmp(kApps[i].icon_suffix, suffix) == 0) {
            return i;
        }
    }
    return -1;
}

void EnsureAppOrderLoaded()
{
    if (s_app_order_ready) return;
    InitIdentityAppOrder();
    s_app_order_ready = true;

    Settings s("ui", false);
    const std::string raw = s.GetString("home_order", "");
    if (raw.empty()) return;

    int parsed[kTotalApps];
    bool used[kTotalApps] = {};
    int n = 0;
    const char *p = raw.c_str();
    while (*p != '\0' && n < kTotalApps) {
        while (*p == ',' || *p == ' ') ++p;
        if (*p == '\0') break;
        char tok[48];
        int t = 0;
        while (*p != '\0' && *p != ',' && t < 47) {
            tok[t++] = *p++;
        }
        tok[t] = '\0';
        const int idx = FindAppIndexBySuffix(tok);
        // Retired apps must not reset the arrangement of remaining apps.
        if (idx<0 && (!std::strcmp(tok,"espclaw") || !std::strcmp(tok,"backlight"))) continue;
        if (idx < 0 || used[idx]) {
            InitIdentityAppOrder();
            return;
        }
        used[idx] = true;
        parsed[n++] = idx;
    }
    /* Preserve existing arrangements when a firmware restores/adds an app. */
    for (int i = 0; i < kTotalApps; ++i) {
        if (!used[i]) parsed[n++] = i;
    }
    for (int i = 0; i < kTotalApps; ++i) {
        s_app_order[i] = parsed[i];
    }
}

std::string HomeCatalogId(int index)
{
    if(index>=0 && index<kTotalApps)return kApps[index].icon_suffix;
    index-=kTotalApps;
    if(index>=0 && index<(int)s_external_apps.size())return "hiapp:"+s_external_apps[index].id;
    index-=s_external_apps.size();return index>=0&&index<(int)s_desktop_folders.size()?s_desktop_folders[index].id:std::string();
}
int HomeCatalogIndex(const std::string&id)
{
    for(int i=0;i<kTotalApps;i++)if(id==kApps[i].icon_suffix)return i;
    for(size_t i=0;i<s_external_apps.size();i++)if(id=="hiapp:"+s_external_apps[i].id)return kTotalApps+i;
    for(size_t i=0;i<s_desktop_folders.size();i++)if(id==s_desktop_folders[i].id)return kTotalApps+s_external_apps.size()+i;
    return -1;
}
std::vector<std::string> HomeOrderedIds()
{
    std::vector<std::string> ids;
    for(int index:s_app_order){auto id=HomeCatalogId(index);if(!id.empty())ids.push_back(std::move(id));}
    return ids;
}
void ReconcileHomeOrder(std::vector<HiApp::Record> external)
{
    auto wanted=HomeOrderedIds();bool loaded=false;auto saved=HiApp::RegistryOrder(&loaded);
    if(loaded){if(s_order_dirty)wanted.insert(wanted.end(),saved.begin(),saved.end());else{saved.insert(saved.end(),wanted.begin(),wanted.end());wanted=std::move(saved);}}
    const auto layout=DesktopFolders::Snapshot();
    s_desktop_folders=layout.folders;
    if(layout.loaded&&!s_order_dirty)wanted=layout.desktop;
    s_external_apps=std::move(external);
    for(int i=0;i<kTotalApps;i++)wanted.push_back(kApps[i].icon_suffix);
    for(const auto&r:s_external_apps)wanted.push_back("hiapp:"+r.id);
    std::vector<int> order;std::vector<int> used(kTotalApps+s_external_apps.size()+s_desktop_folders.size(),0);
    for(const auto&id:wanted){if(DesktopFolders::Grouped(layout,id))continue;int index=HomeCatalogIndex(id);if(index>=0&&!used[index]){used[index]=1;order.push_back(index);}}
    s_app_order.swap(order);
    if(loaded&&s_order_dirty){HiApp::RegistrySaveOrder(HomeOrderedIds());s_order_dirty=false;}
}

void ScheduleAppOrderSave();

AppCellInfo *CellInfoOf(lv_obj_t *cell)
{
    if (cell == nullptr) return nullptr;
    return static_cast<AppCellInfo *>(lv_obj_get_user_data(cell));
}

struct HomeEditState {
    bool active = false;
    bool dragging = false;
    lv_obj_t *done_btn = nullptr;
    lv_obj_t *drag_cell = nullptr;
    lv_obj_t *hole = nullptr;
    lv_obj_t *page = nullptr;
    lv_obj_t *pager = nullptr;
    int page_index = 0;
    int gap_slot = 0;
    int grab_ox = 0;
    int grab_oy = 0;
    int n_slots = 0;
    uint32_t last_shift_tick = 0;
    int edge_direction = 0;
    uint32_t edge_tick = 0;
    std::string folder_target;
    uint32_t folder_tick=0;
    bool folder_ready=false;
    lv_obj_t* folder_hint=nullptr;
    lv_timer_t *save_timer = nullptr;
};
HomeEditState s_edit;
bool s_folder_resume_edit=false;

void ForgetDeletedHomeCell(lv_obj_t* cell);
void OnAppCellDeleted(lv_event_t *e)
{
    lv_obj_t *cell = lv_event_get_target_obj(e);
    ForgetDeletedHomeCell(cell);
    if (s_edit.drag_cell == cell) {
        s_edit.drag_cell = nullptr;
        s_edit.dragging = false;
    }
    AppCellInfo *info = CellInfoOf(cell);
    if (cell != nullptr) {
        lv_obj_set_user_data(cell, nullptr);
    }
    delete info;
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

void ShowUnavailableToast(const char *app_name,bool unavailable=true)
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
    std::snprintf(buf, sizeof(buf), unavailable?I18n::T("%s 敬请期待"):"%s", app_name);

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

void HighlightDot(PagerState *state, int page);
int32_t PagerScrollXForPage(const PagerState *state, int logical_page);
bool PagerLoopEnabled(const PagerState *state);
void GoToPage(PagerState *state, int target_page);
void CancelCellScaleTimer();
void ExitHomeEditMode();

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

lv_obj_t *s_home_root = nullptr;
bool s_home_foreground = false;
lv_obj_t *s_native_app_root = nullptr;
const AppEntry *s_native_app_entry = nullptr;

void DeleteInactiveApp(void *data)
{
    auto *screen = static_cast<lv_obj_t *>(data);
    /* Rapid queued launches can reuse a cached screen before its deferred
     * deletion runs. Never delete a screen that has become active again. */
    if (screen != nullptr && screen != lv_screen_active() &&
        screen != s_home_root && lv_obj_is_valid(screen) &&
        !PanelScreen::IsPanelObject(screen)) {
        lv_obj_delete(screen);
    }
}

void DeleteInactiveAppLater(lv_obj_t *screen)
{
    lv_async_call_cancel(DeleteInactiveApp, screen);
    if (lv_async_call(DeleteInactiveApp, screen) != LV_RESULT_OK) {
        ESP_LOGW(TAG_HOME, "deferred screen cleanup unavailable");
    }
}

bool LaunchHomeApp(const AppEntry *app)
{
    if (app == nullptr) return false;
    // Voice and recents can request an app that is already in front. Reuse
    // that instance; constructing another would overwrite its global state
    // before the old instance receives SCREEN_UNLOADED.
    if (app == s_native_app_entry && s_native_app_root &&
        lv_screen_active() == s_native_app_root) return true;

    if (app->requires_network && WifiRequired_ShouldBlock()) {
        ESP_LOGW(TAG_HOME, "block app '%s': WiFi and cellular unavailable",
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
    s_native_app_root = dest;
    s_native_app_entry = app;
    lv_obj_add_event_cb(dest, [](lv_event_t *event) {
        if (lv_event_get_target_obj(event) == s_native_app_root) {
            s_native_app_root = nullptr;
            s_native_app_entry = nullptr;
        }
    }, LV_EVENT_DELETE, nullptr);
    if (app->lifecycle_cb != nullptr) {
        screen_attach_lifecycle(dest, app->lifecycle_cb);
    }
    screen_ensure_swipe_back(dest, []() { HomeScreen::SwitchToHome(); });
    write(1, "APP_CREATED\n", 12);

    {
        int idx = -1;
        for (int i = 0; i < kTotalApps; ++i) {
            if (&kApps[i] == app) {
                idx = i;
                break;
            }
        }
        SystemChrome_NotifyAppOpened(
            app->icon_suffix,
            I18n::T(app->name),
            (idx >= 0) ? AppIconPath(idx) : "");
        SystemChrome_Attach(dest, false);
        EnsureAppOrderLoaded();
        if (idx >= 0) {
            for (int slot = 0; slot < static_cast<int>(s_app_order.size()); ++slot) {
                if (s_app_order[slot] == idx) {
                    s_last_home_page = slot / kAppsPerPage;
                    s_home_return_page = s_last_home_page;
                    break;
                }
            }
        }
    }

    /* Each app owns its widget styles. Recursively clearing borders here
     * erased calendar badges and settings selection outlines after creation. */
    screen_load_safe(dest);
    /* Keep the home screen object. Recreating the 720 pager on every
     * return is what painted a solid-blue default-theme frame. */
    if (old_scr != nullptr && old_scr != dest && old_scr != s_home_root &&
        !PanelScreen::IsPanelObject(old_scr)) {
        DeleteInactiveAppLater(old_scr);
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
    if (s_cell_scale_timer != nullptr) {
        lv_timer_set_repeat_count(s_cell_scale_timer, 1);
    } else {
        s_cell_scale_ctx = CellScaleCtx{};
        lv_obj_remove_state(cell, LV_STATE_PRESSED);
    }
}

constexpr lv_obj_flag_t kAppCellFlag = LV_OBJ_FLAG_USER_2;

// Every icon, including composite miniature icons, belongs to its outer cell's
// single PRESS_LOCK touch target. Nested widgets must never capture a drag.
void HomeIconPassiveTree(lv_obj_t* obj){
    if(!obj)return;
    lv_obj_remove_flag(obj,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);++i)HomeIconPassiveTree(lv_obj_get_child(obj,i));
}

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

    const char *suffix = entry.icon_suffix;
    const bool tank = suffix != nullptr && std::strcmp(suffix, "tank") == 0;
    const bool link = suffix != nullptr && std::strcmp(suffix, "link_game") == 0;
    const bool fruit = suffix != nullptr && std::strcmp(suffix, "fruit_game") == 0;
    const bool xiangqi = suffix != nullptr && std::strcmp(suffix, "xiangqi") == 0;
    const bool sms = suffix != nullptr && std::strcmp(suffix, "sms") == 0;
    const bool home_app = suffix != nullptr && std::strcmp(suffix, "home") == 0;
    lv_obj_t *icon = tank ? TankGameScreen::CreateIcon(cell, kIconSize) :
                     link ? LinkGameScreen::CreateIcon(cell, kIconSize) :
                     fruit ? FruitGameScreen::CreateIcon(cell, kIconSize) :
                     xiangqi ? XiangqiScreen::CreateIcon(cell, kIconSize) :
                     sms ? SmsScreen::CreateIcon(cell, kIconSize) :
                     !strcmp(entry.icon_suffix,"compass")?CompassScreen::CreateIcon(cell,kIconSize):
                     !strcmp(entry.icon_suffix,"clock")?ClockScreen::CreateIcon(cell,kIconSize):
                     !strcmp(entry.icon_suffix,"app_manager")?AppManagerScreen::CreateIcon(cell,kIconSize):
                     lv_image_create(cell);
    // Compass/clock/app-manager icons are composite widgets, not lv_image.
    // Calling lv_image_set_src on them corrupts memory beyond lv_obj_t.
    if (!tank && !link && !fruit && !xiangqi && !sms &&
        strcmp(entry.icon_suffix,"compass") && strcmp(entry.icon_suffix,"clock") &&
        strcmp(entry.icon_suffix,"app_manager") && lv_obj_check_type(icon,&lv_image_class)) {
        if (home_app) {
            lv_image_set_src(icon, HomeAppIcon());
        } else {
            lv_image_set_src(icon, HomeIconSource(idx));
            NoteThemeIcon(idx, icon);
        }
    }
    lv_obj_set_size(icon, kIconSize, kIconSize);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
    HomeIconPassiveTree(icon);

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
    lv_obj_add_flag(cell, LV_OBJ_FLAG_PRESS_LOCK);

    auto *info = new AppCellInfo{};
    info->app = &entry;
    info->app_idx = idx;
    info->local_slot = 0;
    lv_obj_set_user_data(cell, info);
    lv_obj_add_event_cb(cell, OnAppCellDeleted, LV_EVENT_DELETE, nullptr);

    return cell;
}

lv_obj_t *CreateOrderedAppCell(lv_obj_t*parent,int index)
{
    if(index>=0&&index<kTotalApps)return CreateAppCell(parent,kApps[index],index);
    if(index>=kTotalApps+(int)s_external_apps.size())return CreateFolderCell(parent,index);
    int n=index-kTotalApps;if(n<0||n>=(int)s_external_apps.size())return nullptr;
    const auto&r=s_external_apps[n];auto*cell=lv_obj_create(parent);lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell,kCellWidth,kCellHeight);lv_obj_remove_flag(cell,LV_OBJ_FLAG_SCROLLABLE);
    auto*icon=HiApp::RecordIcon(cell,r,kIconSize);lv_obj_align(icon,LV_ALIGN_TOP_MID,0,0);
    auto*name=lv_label_create(cell);lv_label_set_text(name,r.name.c_str());lv_obj_set_width(name,kCellWidth);
    lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);lv_obj_set_style_text_font(name,&font_puhui_20_4,0);
    lv_obj_set_style_text_color(name,lv_color_hex(0xE5E7EB),0);lv_obj_set_style_text_align(name,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_align(name,LV_ALIGN_TOP_MID,0,kIconSize+kNameGap);lv_obj_remove_flag(name,LV_OBJ_FLAG_CLICKABLE);
    auto*info=new AppCellInfo;info->app_idx=index;info->external_id=r.id;lv_obj_set_user_data(cell,info);
    lv_obj_add_event_cb(cell,OnAppCellDeleted,LV_EVENT_DELETE,nullptr);lv_obj_add_flag(cell,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(cell,kAppCellFlag);lv_obj_add_flag(cell,LV_OBJ_FLAG_PRESS_LOCK);return cell;
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------
struct HomeStatusState {
    lv_obj_t *bar = nullptr;
    lv_obj_t *network_icon_lbl = nullptr;
    lv_obj_t *airplane_icon = nullptr;
    lv_obj_t *network_type_lbl = nullptr;
    lv_obj_t *sim_slot_lbl = nullptr;
    lv_obj_t *battery_icon_lbl = nullptr;
    lv_obj_t *battery_body = nullptr;
    lv_obj_t *battery_bolt = nullptr;
    bool last_charging = false;
    lv_obj_t *battery_pct_lbl = nullptr;
    lv_obj_t *time_lbl = nullptr;
    lv_obj_t *activation_code_lbl = nullptr;
    lv_timer_t *update_timer = nullptr;
    lv_timer_t *arm_timer = nullptr;
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

    const int mode = metalio_board_get_network_mode();
    const char *type = mode == 2 ? "" : mode == 1 ? "4G" : "WiFi";
    if (st->network_type_lbl && strcmp(lv_label_get_text(st->network_type_lbl), type))
        lv_label_set_text(st->network_type_lbl, type);
    if (st->network_type_lbl)
        lv_obj_set_flag(st->network_type_lbl, LV_OBJ_FLAG_HIDDEN, mode == 2);
    if (st->network_icon_lbl)
        lv_obj_set_flag(st->network_icon_lbl, LV_OBJ_FLAG_HIDDEN, mode == 2);
    if (st->airplane_icon)
        lv_obj_set_flag(st->airplane_icon, LV_OBJ_FLAG_HIDDEN, mode != 2);
    if (st->sim_slot_lbl) {
        if (mode == 1) {
            const int slot = metalio_nt26_get_sim_slot();
            const char *text = slot == 1 ? I18n::T("内卡") : slot == 0 ? I18n::T("外卡") : "";
            if (strcmp(lv_label_get_text(st->sim_slot_lbl), text)) lv_label_set_text(st->sim_slot_lbl, text);
            lv_obj_remove_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    const char *icon = metalio_board_get_network_icon();
    if (icon != nullptr && st->network_icon_lbl != nullptr &&
        icon != st->last_icon) {
        st->last_icon = icon;
        lv_label_set_text(st->network_icon_lbl, icon);
    }

    // ---- Battery ----
#if HOME_STATUS_SHOW_BATTERY_ICON
    BatteryIndicatorUpdate(st->battery_body);
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
        if (metalio_board_clock_ready() && localtime_r(&now, &tm_info) != nullptr &&
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
    if (st->arm_timer != nullptr) {
        lv_timer_delete(st->arm_timer);
        st->arm_timer = nullptr;
    }
    delete st;
}

void OnDeskBackStop(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

bool s_desk_back_pending = false;

void OnDeskBack(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    if (s_desk_back_pending) return;
    s_desk_back_pending = true;
    const lv_result_t queued = lv_async_call([](void *) {
        s_desk_back_pending = false;
        if (HomeScreen::IsHomeObject(lv_screen_active()))
            PanelScreen::Show();
    }, nullptr);
    if (queued != LV_RESULT_OK) s_desk_back_pending = false;
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
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *left = lv_obj_create(bar);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, kStatusLeftWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 10, LV_PART_MAIN);

    lv_obj_t *desk = lv_button_create(left);
    lv_obj_set_size(desk, 84, 36);
    lv_obj_set_style_bg_color(desk, lv_color_hex(0x2C3138), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(desk, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(desk, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(desk, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(desk, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(desk, OnDeskBack, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(desk, OnDeskBackStop, LV_EVENT_PRESSED, nullptr);
    lv_obj_t *desk_lbl = lv_label_create(desk);
    lv_label_set_text(desk_lbl, "桌面");
    lv_obj_set_style_text_font(desk_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(desk_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(desk_lbl);

    st->network_icon_lbl = lv_label_create(left);
    lv_label_set_text(st->network_icon_lbl, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(st->network_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->network_icon_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    st->airplane_icon = lv_image_create(left);
    lv_image_set_src(st->airplane_icon, ControlCenter_GetStatusPlaneIcon());
    lv_obj_add_flag(st->airplane_icon, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_remove_flag(right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 12, LV_PART_MAIN);
#if HOME_STATUS_SHOW_BATTERY_ICON
    st->battery_body = BatteryIndicatorCreate(right);
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

    // The desktop shortcut stays at the leading (top-left) edge.
    lv_obj_remove_flag(desk, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(desk, LV_OBJ_FLAG_PRESS_LOCK);

    // Center floating container: time + activation code
    lv_obj_t *center = lv_obj_create(bar);
    lv_obj_add_flag(center, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_CLICKABLE);
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
    st->arm_timer = lv_timer_create(
        [](lv_timer_t *t) {
            auto *hst = static_cast<HomeStatusState *>(lv_timer_get_user_data(t));
            if (hst != nullptr && hst->arm_timer == t) {
                hst->arm_timer = nullptr;
            }
            if (hst != nullptr && hst->update_timer == nullptr &&
                s_home_status == hst) {
                hst->update_timer = lv_timer_create(OnHomeStatusTimer, 1000, hst);
                UpdateHomeStatusBar(hst);
            }
        },
        2500, st);
    if (st->arm_timer != nullptr) {
        lv_timer_set_repeat_count(st->arm_timer, 1);
    }
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
    std::vector<lv_obj_t *> dots;
    unsigned revision=0;
    bool rebuilding=false;
    unsigned rebuilds=0;
    lv_timer_t *apps_timer=nullptr;
    int page_count;
    int current_page;
};

void HighlightDot(PagerState *state, int page)
{
    if (!state || state->rebuilding || page < 0 || page >= state->page_count) return;
    state->current_page = page;
    for (int i = 0; i < state->page_count && i < static_cast<int>(state->dots.size()); ++i) {
        if (state->dots[i] == nullptr || !lv_obj_is_valid(state->dots[i])) continue;
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
    if (!PagerLoopEnabled(state) || state->pager == nullptr || state->rebuilding) return;
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

constexpr uint32_t kPageSlideAnimMs = 320;

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
    int32_t scroll_origin = 0;
    uint32_t last_scroll_tick = 0;
    uint32_t press_tick = 0;
    lv_obj_t *press_cell = nullptr;
    const AppEntry *app = nullptr;
};
HomeTouchSession s_home_touch;

void ForgetDeletedHomeCell(lv_obj_t* cell)
{
    if(s_cell_scale_ctx.cell==cell)CancelCellScaleTimer();
    if(s_home_touch.press_cell==cell){
        s_home_touch.press_cell=nullptr;s_home_touch.app=nullptr;
        s_home_touch.consumed=true;
    }
}


void HomeTouchFlushPendingScroll(PagerState *state, bool force)
{
    if (state == nullptr || state->pager == nullptr) return;
    if (s_home_touch.pending_scroll_dx == 0) return;
    const uint32_t now = lv_tick_get();
    if (!force) {
        const uint32_t dt = now - s_home_touch.last_scroll_tick;
        if (dt < 16 && std::abs(s_home_touch.pending_scroll_dx) < 28) {
            return;
        }
    }
    /* One gesture may travel at most one page. Unclamped scroll_by
     * plus a short snap made a single swipe jump several pages. */
    const int32_t cur = lv_obj_get_scroll_x(state->pager);
    /* scroll_by moves content; scroll_x is the opposite signed offset. */
    int32_t next = cur - s_home_touch.pending_scroll_dx;
    const int32_t lo = s_home_touch.scroll_origin - kPanelSize;
    const int32_t hi = s_home_touch.scroll_origin + kPanelSize;
    if (next < lo) next = lo;
    if (next > hi) next = hi;
    const int apply = static_cast<int>(cur - next);
    s_home_touch.pending_scroll_dx = 0;
    s_home_touch.last_scroll_tick = now;
    if (apply == 0) return;
    lv_obj_scroll_by(state->pager, apply, 0, LV_ANIM_OFF);
}

void OnPagerScrollEnd(lv_event_t *e)
{
    auto *state = static_cast<PagerState *>(lv_event_get_user_data(e));
    if (state == nullptr || state->pager == nullptr || state->rebuilding) return;
    if (s_home_touch.active && s_home_touch.paging) {
        return;
    }
    if (!lv_obj_is_scrolling(state->pager)) {
        PagerMaybeWrapAfterScroll(state);
    }
}

void GoToPage(PagerState *state, int target_page)
{
    if (state == nullptr || state->pager == nullptr || state->rebuilding) return;
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
        return;
    }
    lv_obj_scroll_to_x(state->pager, target_x, LV_ANIM_ON);
    HighlightDot(state, target_page);
    s_last_home_page = target_page;
    ResetHomeIdleTimer();
}

constexpr int kHomeMoveThreshold       = 5;
constexpr int kHomeAxisLockThreshold   = 12;
constexpr int kPageSnapThreshold       = kPanelSize / 5;
constexpr int kHomeFlickThreshold      = 24;
constexpr uint32_t kHomeLongPressMs    = 600;

void SnapPagerToNearestPage(PagerState *state, int release_dx)
{
    if (state == nullptr || state->pager == nullptr) return;
    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    const int anchor_x = PagerScrollXForPage(state, state->current_page);
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
    if (s_edit.active || s_folder_saving || s_folder_refresh) return;
    if (s_home_touch.press_cell == nullptr) return;
    if (kind == HomeTouchKind::Click) {
        if (s_home_touch.app == nullptr) {auto *info=CellInfoOf(s_home_touch.press_cell);if(info&&!info->folder_id.empty()){OpenDesktopFolder(info->folder_id);return;}if(info&&!info->external_id.empty()){int page=(int)(intptr_t)lv_obj_get_user_data(lv_obj_get_parent(s_home_touch.press_cell));s_last_home_page=s_home_return_page=page;HiApp::Launch(("hiapp:"+info->external_id).c_str());}return;}
        PlayAppCellPressScale(s_home_touch.press_cell, s_home_touch.app);
    }
}

constexpr uint32_t kIconGapShiftMs = 380;

int HomePageSlotCount(int page_index)
{
    const int start = page_index * kAppsPerPage;
    int n = static_cast<int>(s_app_order.size()) - start;
    if (n > kAppsPerPage) n = kAppsPerPage;
    if (n < 0) n = 0;
    return n;
}

int HomePageIndexOf(lv_obj_t *page)
{
    if (page == nullptr) return -1;
    return static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(page)));
}

void PlaceCellOnGrid(lv_obj_t *obj, int local_slot)
{
    if (obj == nullptr || local_slot < 0) return;
    const int col = local_slot % kPageCols;
    const int row = local_slot / kPageCols;
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_STRETCH, col, 1,
                         LV_GRID_ALIGN_STRETCH, row, 1);
    AppCellInfo *info = CellInfoOf(obj);
    if (info != nullptr) {
        info->local_slot = local_slot;
    }
}

void CapturePageOrder(lv_obj_t *page, int page_index)
{
    if (page == nullptr) return;
    const int start = page_index * kAppsPerPage;
    const int n = HomePageSlotCount(page_index);
    const uint32_t count = lv_obj_get_child_count(page);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *cell = lv_obj_get_child(page, i);
        if (cell == nullptr || !lv_obj_has_flag(cell, kAppCellFlag)) continue;
        AppCellInfo *info = CellInfoOf(cell);
        if (info == nullptr) continue;
        if (info->local_slot < 0 || info->local_slot >= n) continue;
        const int slot = start + info->local_slot;
        if (slot >= 0 && slot < static_cast<int>(s_app_order.size()) &&
            info->app_idx >= 0 && info->app_idx < kTotalApps+static_cast<int>(s_external_apps.size()+s_desktop_folders.size())) {
            s_app_order[slot] = info->app_idx;
        }
    }
}

void EnableHomeEventBubble(lv_obj_t *obj);
void ScheduleAppOrderSave();

void SyncClonePageSlots(lv_obj_t *pager, int page_index)
{
    if (pager == nullptr) return;
    const int n = HomePageSlotCount(page_index);
    const int start = page_index * kAppsPerPage;
    const uint32_t pages = lv_obj_get_child_count(pager);
    for (uint32_t p = 0; p < pages; ++p) {
        lv_obj_t *page = lv_obj_get_child(pager, p);
        if (page == nullptr || page == s_edit.page) continue;
        if (HomePageIndexOf(page) != page_index) continue;
        bool present[kAppsPerPage] = {};
        for (int i = static_cast<int>(lv_obj_get_child_count(page)) - 1; i >= 0; --i) {
            lv_obj_t *cell = lv_obj_get_child(page, i);
            if (!lv_obj_has_flag(cell, kAppCellFlag)) continue;
            AppCellInfo *info = CellInfoOf(cell);
            int want = -1;
            for (int slot = 0; info && slot < n; ++slot)
                if (s_app_order[start + slot] == info->app_idx) { want = slot; break; }
            if (want < 0) { lv_obj_delete(cell); continue; }
            PlaceCellOnGrid(cell, want);
            present[want] = true;
        }
        for (int slot = 0; slot < n; ++slot) {
            if (present[slot]) continue;
            const int idx = s_app_order[start + slot];
            auto *cell = CreateOrderedAppCell(page,idx);
            if(!cell)continue;
            PlaceCellOnGrid(cell, slot);
            EnableHomeEventBubble(cell);
        }
    }
}

int HomeSlotAtPoint(lv_obj_t *page, lv_point_t p, int n_slots)
{
    if (page == nullptr || n_slots <= 0) return 0;
    lv_area_t a;
    lv_obj_get_coords(page, &a);
    int x = p.x - a.x1 - kPagePadHor;
    int y = p.y - a.y1 - kPagePadVer;
    const int cell_stride_x = kCellWidth + kGridColGap;
    const int cell_stride_y = kCellHeight + kGridRowGap;
    int col = 0;
    int row = 0;
    if (cell_stride_x > 0) {
        col = x / cell_stride_x;
    }
    if (cell_stride_y > 0) {
        row = y / cell_stride_y;
    }
    if (col < 0) col = 0;
    if (col >= kPageCols) col = kPageCols - 1;
    if (row < 0) row = 0;
    if (row >= kPageRows) row = kPageRows - 1;
    int slot = row * kPageCols + col;
    if (slot >= n_slots) slot = n_slots - 1;
    if (slot < 0) slot = 0;
    return slot;
}

void HomeEditShiftGap(int to_slot)
{
    if (!s_edit.dragging || s_edit.page == nullptr) return;
    if (to_slot < 0 || to_slot >= s_edit.n_slots) return;
    if (to_slot == s_edit.gap_slot) return;
    if (lv_tick_elaps(s_edit.last_shift_tick) < kIconGapShiftMs) return;

    lv_obj_t *by_slot[kAppsPerPage] = {};
    const uint32_t count = lv_obj_get_child_count(s_edit.page);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *cell = lv_obj_get_child(s_edit.page, i);
        if (cell == nullptr || cell == s_edit.drag_cell || cell == s_edit.hole) {
            continue;
        }
        if (!lv_obj_has_flag(cell, kAppCellFlag)) continue;
        AppCellInfo *info = CellInfoOf(cell);
        if (info == nullptr) continue;
        if (info->local_slot >= 0 && info->local_slot < kAppsPerPage) {
            by_slot[info->local_slot] = cell;
        }
    }

    const int from = s_edit.gap_slot;
    if (to_slot > from) {
        for (int s = from; s < to_slot; ++s) {
            if (by_slot[s + 1] != nullptr) {
                PlaceCellOnGrid(by_slot[s + 1], s);
            }
        }
    } else {
        for (int s = from; s > to_slot; --s) {
            if (by_slot[s - 1] != nullptr) {
                PlaceCellOnGrid(by_slot[s - 1], s);
            }
        }
    }
    PlaceCellOnGrid(s_edit.hole, to_slot);
    AppCellInfo *drag_info = CellInfoOf(s_edit.drag_cell);
    if (drag_info != nullptr) {
        drag_info->local_slot = to_slot;
    }
    s_edit.gap_slot = to_slot;
    s_edit.last_shift_tick = lv_tick_get();
    CapturePageOrder(s_edit.page, s_edit.page_index);
    if (drag_info) s_app_order[s_edit.page_index * kAppsPerPage + to_slot] = drag_info->app_idx;
}

void HomeEditStartDrag(lv_obj_t *cell, lv_point_t p)
{
    if (s_edit.dragging || s_folder_saving || s_folder_refresh || cell == nullptr || !s_edit.pager) return;
    CancelCellScaleTimer();
    lv_obj_t *page = lv_obj_get_parent(cell);
    if (page == nullptr) return;
    AppCellInfo *info = CellInfoOf(cell);
    if (info == nullptr || info->app_idx<0) return;

    lv_area_t ca;
    lv_obj_get_coords(cell, &ca);
    s_edit.grab_ox = p.x - ca.x1;
    s_edit.grab_oy = p.y - ca.y1;
    s_edit.page = page;
    s_edit.page_index = HomePageIndexOf(page);
    s_edit.n_slots = HomePageSlotCount(s_edit.page_index);
    s_edit.gap_slot = info->local_slot;
    if (s_edit.gap_slot < 0) s_edit.gap_slot = 0;

    lv_display_t *disp = lv_display_get_default();
    if (disp) lv_display_enable_invalidation(disp, false);
    lv_obj_t *hole = lv_obj_create(page);
    lv_obj_remove_style_all(hole);
    lv_obj_set_size(hole, kCellWidth, kCellHeight);
    lv_obj_set_style_bg_opa(hole, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(hole, 0, LV_PART_MAIN);
    lv_obj_remove_flag(hole, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(hole, LV_OBJ_FLAG_SCROLLABLE);
    PlaceCellOnGrid(hole, s_edit.gap_slot);
    s_edit.hole = hole;

    lv_obj_add_flag(cell, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_parent(cell, lv_obj_get_parent(s_edit.pager));
    /* Grid stretch leaves layout flags set, and set_size is then ignored.
     * A stretched cell can cover the screen and show the theme for a frame. */
    cell->w_layout = 0;
    cell->h_layout = 0;
    lv_obj_set_size(cell, kCellWidth, kCellHeight);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_move_foreground(cell);
    if (s_home_root) {
        lv_obj_set_style_bg_color(s_home_root, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_home_root, LV_OPA_COVER, LV_PART_MAIN);
    }
    if (disp) lv_display_enable_invalidation(disp, true);
    s_edit.drag_cell = cell;
    s_edit.dragging = true;
    s_order_dirty=true;
    s_edit.last_shift_tick = lv_tick_get();

    lv_area_t pa;
    lv_obj_get_coords(lv_obj_get_parent(cell), &pa);
    s_edit.edge_direction = 0;
    s_edit.folder_target.clear();
    lv_obj_set_pos(cell, p.x - pa.x1 - s_edit.grab_ox,
                   p.y - pa.y1 - s_edit.grab_oy);
}

void HomeEditFolderTarget(const std::string& target){
    const bool changed=target!=s_edit.folder_target;
    if(changed)s_edit.folder_tick=lv_tick_get();
    const bool ready=!target.empty()&&DesktopDrag::Ready(lv_tick_get(),s_edit.folder_tick,DesktopFolders::IsFolder(target));
    if(!changed&&ready==s_edit.folder_ready)return;
    s_edit.folder_target=target;s_edit.folder_ready=ready;
    if(target.empty()) {
        if(s_edit.folder_hint)lv_obj_add_flag(s_edit.folder_hint,LV_OBJ_FLAG_HIDDEN);
    } else if(s_edit.page) {
        for(uint32_t i=0;i<lv_obj_get_child_count(s_edit.page);++i){
            auto* cell=lv_obj_get_child(s_edit.page,i);if(!lv_obj_has_flag(cell,kAppCellFlag))continue;
            auto* info=CellInfoOf(cell);if(!info||HomeCatalogId(info->app_idx)!=target)continue;
            if(!s_edit.folder_hint) {
                lv_display_t* disp=lv_display_get_default();
                if(disp) lv_display_enable_invalidation(disp,false);
                auto* hint=lv_obj_create(lv_obj_get_parent(s_edit.pager));s_edit.folder_hint=hint;
                lv_obj_remove_style_all(hint);lv_obj_set_size(hint,kIconSize+20,kIconSize+20);
                lv_obj_add_flag(hint,LV_OBJ_FLAG_FLOATING);
                lv_obj_remove_flag(hint,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
                lv_obj_set_style_radius(hint,25,0);lv_obj_set_style_border_width(hint,3,0);
                lv_obj_set_style_bg_opa(hint,LV_OPA_20,0);
                lv_obj_set_style_bg_color(hint,lv_color_hex(0xE8CD97),0);
                lv_obj_set_style_border_color(hint,lv_color_hex(0xE8CD97),0);
                // Independent overlay: feedback never resizes or displaces the target icon.
                lv_obj_move_to_index(hint,lv_obj_get_index(s_edit.drag_cell));
                if(disp) lv_display_enable_invalidation(disp,true);
            }
            lv_area_t a,pa;lv_obj_get_coords(cell,&a);lv_obj_get_coords(lv_obj_get_parent(s_edit.folder_hint),&pa);
            lv_obj_set_pos(s_edit.folder_hint,(a.x1+a.x2-kIconSize)/2-pa.x1-10,a.y1-pa.y1-10);
            lv_obj_set_style_border_color(s_edit.folder_hint,lv_color_hex(ready?0x91D9A6:0xE8CD97),0);
            lv_obj_set_style_bg_color(s_edit.folder_hint,lv_color_hex(ready?0x91D9A6:0xE8CD97),0);
            lv_obj_remove_flag(s_edit.folder_hint,LV_OBJ_FLAG_HIDDEN);break;
        }
    }
    if(s_edit.done_btn&&lv_obj_get_child_count(s_edit.done_btn))lv_label_set_text(lv_obj_get_child(s_edit.done_btn,0),I18n::T(target.empty()?"完成":ready?"松手合并":"即将合并"));
}

void HomeEditMoveDrag(PagerState *state, lv_point_t p)
{
    if (!s_edit.dragging || s_edit.drag_cell == nullptr || s_edit.page == nullptr) {
        return;
    }
    lv_area_t pa;
    lv_obj_get_coords(lv_obj_get_parent(s_edit.drag_cell), &pa);
    lv_obj_set_pos(s_edit.drag_cell, p.x - pa.x1 - s_edit.grab_ox,
                   p.y - pa.y1 - s_edit.grab_oy);
    const int direction = p.x < 36 ? -1 : p.x > kPanelSize - 36 ? 1 : 0;
    if (direction != s_edit.edge_direction) {
        s_edit.edge_direction = direction;
        s_edit.edge_tick = lv_tick_get();
    }
    if(direction)HomeEditFolderTarget("");
    if (direction && state && lv_tick_elaps(s_edit.edge_tick) >= 650) {
        const int target = PagerLoopEnabled(state)?
            HiNavigation::Adjacent(s_edit.page_index,direction,state->page_count):s_edit.page_index+direction;
        s_edit.edge_tick = lv_tick_get();
        if (target >= 0 && target < state->page_count && HomePageSlotCount(target)>0) {
            auto *next = lv_obj_get_child(state->pager, PagerLoopEnabled(state) ? target + 1 : target);
            const int target_slot = direction > 0 ? 0 : HomePageSlotCount(target) - 1;
            lv_obj_t *displaced = nullptr;
            for (uint32_t i = 0; i < lv_obj_get_child_count(next); ++i) {
                auto *candidate = lv_obj_get_child(next, i);
                auto *info = lv_obj_has_flag(candidate, kAppCellFlag) ? CellInfoOf(candidate) : nullptr;
                if (info && info->local_slot == target_slot) { displaced = candidate; break; }
            }
            if (displaced) {
                const int old_page = s_edit.page_index;
                /* Close the source gap before carrying the boundary icon back. */
                s_edit.last_shift_tick = lv_tick_get() - kIconGapShiftMs;
                HomeEditShiftGap(direction > 0 ? s_edit.n_slots - 1 : 0);
                lv_obj_set_parent(displaced, s_edit.page);
                PlaceCellOnGrid(displaced, s_edit.gap_slot);
                CapturePageOrder(s_edit.page, old_page);
                s_edit.page = next;
                s_edit.page_index = target;
                s_edit.n_slots = HomePageSlotCount(target);
                s_edit.gap_slot = target_slot;
                lv_obj_set_parent(s_edit.hole, next);
                PlaceCellOnGrid(s_edit.hole, target_slot);
                CellInfoOf(s_edit.drag_cell)->local_slot = target_slot;
                s_app_order[target * kAppsPerPage + target_slot] = CellInfoOf(s_edit.drag_cell)->app_idx;
                SyncClonePageSlots(state->pager, old_page);
                HighlightDot(state, target);
                s_last_home_page = target;
                lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, target), LV_ANIM_OFF);
                lv_obj_update_layout(state->pager);
                ESP_LOGI(TAG_HOME, "icon drag page %d -> %d", old_page, target);
            }
        }
    }
    if(!direction&&!DesktopFolders::IsFolder(HomeCatalogId(CellInfoOf(s_edit.drag_cell)->app_idx))){
        std::string target;
        const int centre_x=p.x-s_edit.grab_ox+kCellWidth/2;
        const int centre_y=p.y-s_edit.grab_oy+kIconSize/2;
        // Retain the current target through small hand tremors before testing a new one.
        for(int pass=0;pass<2&&target.empty();++pass)for(uint32_t i=0;i<lv_obj_get_child_count(s_edit.page);++i){
            auto* candidate=lv_obj_get_child(s_edit.page,i);if(candidate==s_edit.hole||!lv_obj_has_flag(candidate,kAppCellFlag))continue;
            auto* info=CellInfoOf(candidate);if(!info)continue;const auto id=HomeCatalogId(info->app_idx);
            const bool retained=id==s_edit.folder_target;
            if((pass==0)!=retained)continue;
            lv_area_t a;lv_obj_get_coords(candidate,&a);
            if(DesktopDrag::Inside(centre_x-(a.x1+a.x2)/2,centre_y-(a.y1+kIconSize/2),retained,DesktopFolders::IsFolder(id))){target=id;break;}
        }
        HomeEditFolderTarget(target);
        if(!target.empty())return; // Centre hold groups; the surrounding cell area reorders.
    }else HomeEditFolderTarget("");
    if (!direction) HomeEditShiftGap(HomeSlotAtPoint(s_edit.page, p, s_edit.n_slots));
}

void HomeEditFinishDrag(PagerState *state)
{
    if (!s_edit.dragging) return;
    lv_display_t *disp = lv_display_get_default();
    if (disp) lv_display_enable_invalidation(disp, false);
    const auto dragged=HomeCatalogId(CellInfoOf(s_edit.drag_cell)->app_idx);
    const auto target=s_edit.folder_target;
    const bool group=!target.empty()&&s_edit.folder_ready;
    if(!group&&!target.empty())for(uint32_t i=0;i<lv_obj_get_child_count(s_edit.page);++i){
        auto* candidate=lv_obj_get_child(s_edit.page,i);if(!lv_obj_has_flag(candidate,kAppCellFlag))continue;
        auto* info=CellInfoOf(candidate);if(info&&HomeCatalogId(info->app_idx)==target){
            s_edit.last_shift_tick=lv_tick_get()-kIconGapShiftMs;HomeEditShiftGap(info->local_slot);break;
        }
    }
    HomeEditFolderTarget("");
    lv_obj_t *cell = s_edit.drag_cell;
    if (s_edit.hole != nullptr) {
        lv_obj_delete(s_edit.hole);
        s_edit.hole = nullptr;
    }
    if (cell != nullptr) {
        cell->w_layout = 0;
        cell->h_layout = 0;
        lv_obj_set_size(cell, kCellWidth, kCellHeight);
        lv_obj_set_parent(cell, s_edit.page);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_FLOATING);
        PlaceCellOnGrid(cell, s_edit.gap_slot);
    }
    if (s_edit.page != nullptr) {
        CapturePageOrder(s_edit.page, s_edit.page_index);
    }
    lv_obj_t *pager = s_edit.pager;
    if (state != nullptr && state->pager != nullptr) {
        pager = state->pager;
    }
    SyncClonePageSlots(pager, s_edit.page_index);
    s_edit.dragging = false;
    s_edit.drag_cell = nullptr;
    s_edit.page = nullptr;
    s_edit.edge_direction = 0;
    if(group){
        auto before=DesktopFolders::Snapshot();before.loaded=true;before.desktop=HomeOrderedIds();
        DesktopFolders::Model next;std::string folder;
        const auto new_id=DesktopFolders::IsFolder(target)?std::string():DesktopFolders::NewId();
        if(DesktopFolders::Drop(before,dragged,target,new_id,next,folder)){
            ExitHomeEditMode();s_folder_resume_edit=SubmitDesktopLayout(next);
            if (s_home_root) {
                lv_obj_set_style_bg_color(s_home_root, lv_color_hex(0x000000), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(s_home_root, LV_OPA_COVER, LV_PART_MAIN);
            }
            if (disp) lv_display_enable_invalidation(disp, true);
            if (s_home_root) lv_obj_invalidate(s_home_root);
            return;
        }
        ShowUnavailableToast("文件夹已满，无法合并",false);
    } else {
        ScheduleAppOrderSave();
    }
    if (s_home_root) {
        lv_obj_set_style_bg_color(s_home_root, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_home_root, LV_OPA_COVER, LV_PART_MAIN);
    }
    if (disp) lv_display_enable_invalidation(disp, true);
    if (s_home_root) lv_obj_invalidate(s_home_root);
}

void QueueDesktopHomeOrder(std::string order);
void QueueHomeOrderSave(std::string order){QueueDesktopHomeOrder(std::move(order));}
void OnSaveAppOrderTimer(lv_timer_t*){
    s_edit.save_timer=nullptr;if(s_edit.active||s_edit.dragging){s_order_dirty=true;return;}
    // The catalog still describes the previous layout until refresh completes.
    // Never persist that old set of indices over the just-committed folder.
    if(s_folder_saving||s_folder_refresh){ScheduleAppOrderSave();return;}
    std::string order;for(int index:s_app_order){if(index<0||index>=kTotalApps)continue;if(!order.empty())order+=",";order+=kApps[index].icon_suffix;}
    QueueHomeOrderSave(std::move(order));HomeScreen::SyncInstalledAppOrder(true);
    auto layout=DesktopFolders::Snapshot();if(layout.loaded&&!s_folder_saving){layout.desktop=HomeOrderedIds();SubmitDesktopLayout(layout,false);}
}

void ScheduleAppOrderSave()
{
    if(s_edit.active||s_edit.dragging){s_order_dirty=true;return;}
    if (s_edit.save_timer != nullptr) {
        lv_timer_delete(s_edit.save_timer);
        s_edit.save_timer = nullptr;
    }
    s_edit.save_timer = lv_timer_create(OnSaveAppOrderTimer, 450, nullptr);
    lv_timer_set_repeat_count(s_edit.save_timer, 1);
}

void OnEditDoneClicked(lv_event_t * /*e*/)
{
    ExitHomeEditMode();
}

void EnterHomeEditMode(PagerState *state)
{
    if (s_edit.active) return;
    s_edit.active = true;
    s_edit.pager = (state != nullptr) ? state->pager : nullptr;
    lv_obj_t *screen = nullptr;
    if (s_edit.pager != nullptr) {
        screen = lv_obj_get_parent(s_edit.pager);
    }
    if (screen == nullptr) return;

    lv_obj_t *btn = lv_obj_create(screen);
    lv_obj_remove_style_all(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(btn, 128, 48);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -16, -52);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, LV_PART_MAIN);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    screen_swipe_back_ignore(btn, true);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, I18n::T("完成"));
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn, OnEditDoneClicked, LV_EVENT_CLICKED, nullptr);
    s_edit.done_btn = btn;
}

void ExitHomeEditMode()
{
    if (s_edit.dragging) {
        HomeEditFinishDrag(nullptr);
    }
    if(s_edit.folder_hint){lv_obj_delete(s_edit.folder_hint);s_edit.folder_hint=nullptr;}
    if (s_edit.done_btn != nullptr) {
        lv_obj_delete(s_edit.done_btn);
        s_edit.done_btn = nullptr;
    }
    s_edit.active = false;
    s_edit.pager = nullptr;
    ScheduleAppOrderSave();
}

void HomeTouchTryStartPageDrag(PagerState *state, int dx, int dy, int current_x)
{
    if (s_edit.dragging) return;
    if (s_home_touch.axis != HomeGestureAxis::Horizontal) return;
    if (s_home_touch.consumed && !s_home_touch.paging) return;
    if (!HomeTouchIsHorizontalSlide(dx, dy)) return;
    if (!s_home_touch.paging) {
        s_home_touch.paging = true;
        s_home_touch.consumed = true;
        if (state != nullptr && state->pager != nullptr) {
            lv_obj_stop_scroll_anim(state->pager);
            s_home_touch.scroll_origin = lv_obj_get_scroll_x(state->pager);
        }
        s_home_touch.last_x = static_cast<int16_t>(current_x);
        s_home_touch.pending_scroll_dx = 0;
        s_home_touch.last_scroll_tick = lv_tick_get();
    }
}

void OnHomePressed(lv_event_t *e)
{
    if (s_home_touch.active || s_folder_saving || s_folder_refresh || s_folder_sheet) return;
    if (SystemChrome_IsControlCenterOpen() || SystemChrome_IsRecentsOpen()) {
        return;
    }
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
    AppCellInfo *info = CellInfoOf(cell);
    s_home_touch.app = (info != nullptr) ? info->app : nullptr;
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

    if (s_edit.dragging) {
        HomeEditMoveDrag(state, p);
        return;
    }

    if (s_home_touch.press_cell != nullptr && !s_home_touch.paging) {
        if (s_edit.active && !HomeTouchIsTapLike(dx, dy)) {
            const lv_point_t pressed{s_home_touch.start_x,s_home_touch.start_y};
            HomeEditStartDrag(s_home_touch.press_cell, pressed);
            if (s_edit.dragging) {
                s_home_touch.consumed = true;
                HomeEditMoveDrag(state,p);
                return;
            }
        } else if (!s_edit.active && HomeTouchIsTapLike(dx, dy) &&
                   lv_tick_elaps(s_home_touch.press_tick) >= kHomeLongPressMs) {
            EnterHomeEditMode(state);
            HomeEditStartDrag(s_home_touch.press_cell, p);
            s_home_touch.consumed = true;
            return;
        }
    }

    HomeTouchUpdateAxisLock(dx, dy);

    if (s_home_touch.axis != HomeGestureAxis::Horizontal) return;

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
    lv_point_t release_point{};
    if (indev != nullptr) {
        lv_indev_get_point(indev, &release_point);
        dx = release_point.x - s_home_touch.start_x;
        dy = release_point.y - s_home_touch.start_y;
    }

    if (s_edit.dragging) {
        // Finish at the sampled release position even if the last PRESSING
        // sample was delayed. A release cannot newly arm an unseen merge.
        const auto armed=s_edit.folder_ready?s_edit.folder_target:std::string();
        if(indev){s_edit.last_shift_tick=lv_tick_get()-kIconGapShiftMs;HomeEditMoveDrag(nullptr,release_point);}
        s_edit.folder_ready=!armed.empty()&&s_edit.folder_target==armed;
        HomeEditFinishDrag(state);
        s_home_touch.consumed = true;
    } else if (s_home_touch.axis == HomeGestureAxis::Vertical) {
        if (dy > 48 && dy > std::abs(dx)) {
            SystemChrome_ShowControlCenter();
        }
    } else if (s_home_touch.paging && state != nullptr) {
        HomeTouchFlushPendingScroll(state, true);
        SnapPagerToNearestPage(state, dx);
    } else if (!s_home_touch.consumed) {
        const uint32_t elapsed = lv_tick_elaps(s_home_touch.press_tick);
        if (HomeTouchIsTapLike(dx, dy)) {
            if (elapsed >= kHomeLongPressMs && !s_edit.active &&
                s_home_touch.press_cell != nullptr) {
                EnterHomeEditMode(state);
            } else if (!s_edit.active) {
                HomeTouchDispatchTapLike(HomeTouchKind::Click);
            }
        } else {
            const HomeTouchKind kind = HomeTouchClassifySwipe(dx, dy);
            if (state != nullptr && kind != HomeTouchKind::None &&
                (kind == HomeTouchKind::SwipeLeft ||
                 kind == HomeTouchKind::SwipeRight) &&
                !lv_obj_is_scrolling(state->pager)) {
                HomeTouchHandleSwipe(state, kind);
            }
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

void OnHomePressLost(lv_event_t* e){
    if(!s_home_touch.active)return;
    // Lost contact/gesture cancellation is not consent to merge. Put the
    // floating cell back exactly once and clear references before the next touch.
    if(s_edit.dragging){HomeEditFolderTarget("");HomeEditFinishDrag(static_cast<PagerState*>(lv_event_get_user_data(e)));}
    s_home_touch=HomeTouchSession{};
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
    lv_display_t *disp = lv_display_get_default();
    if (disp != nullptr)
        lv_display_enable_invalidation(disp, true);

    lv_obj_t *scr = lv_event_get_current_target_obj(e);
    EnableHomeEventBubble(scr);

    auto *state = static_cast<PagerState *>(lv_event_get_user_data(e));
    if (state != nullptr && state->pager != nullptr) {
        int page = s_home_return_page;
        if (page < 0 || page >= state->page_count)
            page = s_last_home_page;
        if (page < 0 || page >= state->page_count)
            page = 0;
        lv_obj_remove_event_cb(state->pager, OnPagerScrollEnd);
        lv_obj_update_layout(state->pager);
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, page),
                           LV_ANIM_OFF);
        HighlightDot(state, page);
        s_last_home_page = page;
        s_home_return_page = -1;
        lv_obj_add_event_cb(state->pager, OnPagerScrollEnd, LV_EVENT_SCROLL_END,
                            state);
    }

    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_LOAD);
    StartHomeIdleTimer();
    s_home_foreground = true;
    SystemChrome_Attach(scr, true);
    /* The transparent top pull zone otherwise covers the desktop button.
     * Only the button consumes taps; passive status containers still let
     * empty status-bar areas reach the existing pull-down gesture zone. */
    if (s_home_status != nullptr && s_home_status->bar != nullptr)
        lv_obj_move_foreground(s_home_status->bar);

    /* Some overlays pause LVGL invalidation while they update the framebuffer
     * directly. Re-enabling invalidation does not replay areas that were
     * discarded during that pause. A screen load can therefore complete
     * without scheduling a full draw, leaving the DSI scanout on its previous
     * frame (or the panel's blue fallback). Explicitly invalidate the complete
     * launcher root after its children and chrome are ready. */
    lv_obj_invalidate(scr);
    write(1, "HOME_INVALIDATE\n", sizeof("HOME_INVALIDATE\n") - 1);
}

void OnHomeScreenUnloaded(lv_event_t *e)
{
    s_folder_resume_edit=false;
    auto *state = static_cast<PagerState *>(lv_event_get_user_data(e));
    s_home_touch = HomeTouchSession{};
    if (state != nullptr && state->page_count > 0) {
        const int page = state->current_page;
        if (page >= 0 && page < state->page_count)
            s_last_home_page = page;
    }
    /* Detach idle policy immediately on unload. Waiting until DELETE left
     * IdlePower in Home session while chat/other apps were already active,
     * so auto-standby could steal the app screen and kill voice wake. */
    StopHomeIdleTimer();
    CancelCellScaleTimer();
    if (s_edit.active || s_edit.dragging) {
        ExitHomeEditMode();
    }
    SystemChrome_DismissOverlays();
    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_UNLOAD);
    s_home_foreground = false;
}

void OnScreenDeleted(lv_event_t *e)
{
    auto*deleted_state=static_cast<PagerState *>(lv_event_get_user_data(e));
    if(deleted_state&&deleted_state->apps_timer)lv_timer_delete(deleted_state->apps_timer);
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    // A previous launcher may be deleted asynchronously after its replacement
    // has loaded. It must not detach the new launcher's idle session.
    if (obj != s_home_root) {
        delete static_cast<PagerState *>(lv_event_get_user_data(e));
        return;
    }
    if (obj == s_home_root) {
        s_folder_resume_edit=false;
        s_home_root = nullptr;
        s_home_foreground = false;
        /* These widgets belong to this screen. Keeping them across a
         * recreation makes the next theme change write through freed objects. */
        for (int i = 0; i < s_theme_icon_n; ++i) s_theme_icons[i] = nullptr;
        s_theme_icon_n = 0;
        s_icons_theme_id = -1;
    }
    write(1, "HOME_DELETE\n", sizeof("HOME_DELETE\n") - 1);
    CancelCellScaleTimer();
    StopHomeIdleTimer();
    s_edit.dragging = false;
    s_edit.active = false;
    s_edit.done_btn = nullptr;
    s_edit.drag_cell = nullptr;
    s_edit.hole = nullptr;
    s_edit.page = nullptr;
    s_edit.pager = nullptr;
    s_edit.folder_hint = nullptr;
    if(s_edit.save_timer){lv_timer_delete(s_edit.save_timer);s_edit.save_timer=nullptr;}
    auto *state = static_cast<PagerState *>(lv_event_get_user_data(e));
    delete state;
    write(1, "HOME_DELETE_DONE\n", sizeof("HOME_DELETE_DONE\n") - 1);
}

// ---------------------------------------------------------------------------
// Power options dialog (PWR_KEY long-press)
// ---------------------------------------------------------------------------
struct PowerDialogUi {
    lv_obj_t *mask = nullptr;
    lv_obj_t *card = nullptr;
    lv_display_t *display = nullptr;
};

PowerDialogUi s_pwr_dlg;

void RaisePowerDialog(lv_event_t *)
{
    if (s_pwr_dlg.mask && lv_obj_get_child(lv_layer_sys(),-1)!=s_pwr_dlg.mask)
        lv_obj_move_foreground(s_pwr_dlg.mask);
}

void ClosePowerDialog()
{
    if (s_pwr_dlg.display) lv_display_remove_event_cb_with_user_data(s_pwr_dlg.display,RaisePowerDialog,nullptr);
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

    /* Constant-length rotating arc — spinner's 200ms arc-length tween
     * looked stepped on this RGB888 path. */
    lv_obj_t *spin = lv_arc_create(box);
    lv_obj_set_size(spin, 140, 140);
    lv_arc_set_bg_angles(spin, 0, 360);
    lv_arc_set_angles(spin, 0, 72);
    lv_arc_set_rotation(spin, 270);
    lv_obj_remove_flag(spin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(spin, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(spin, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(spin, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(spin, true, LV_PART_INDICATOR);
    lv_obj_set_style_opa(spin, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(spin, 0, LV_PART_KNOB);

    lv_anim_t ring;
    lv_anim_init(&ring);
    lv_anim_set_var(&ring, spin);
    lv_anim_set_values(&ring, 0, 360);
    lv_anim_set_duration(&ring, 1600);
    lv_anim_set_repeat_count(&ring, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&ring, lv_anim_path_linear);
    lv_anim_set_exec_cb(&ring, [](void *obj, int32_t v) {
        lv_arc_set_rotation(static_cast<lv_obj_t *>(obj), v);
    });
    lv_anim_start(&ring);

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
    screen_load_safe(s_shutdown_screen);
}

static void *ShutdownPulseThread(void * /*arg*/)
{
    struct sched_param sp;
    std::memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 200;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    metalio_pwr_shutdown_pulse();
    return nullptr;
}

void BeginSystemShutdown(const char *reason)
{
    static bool shutting_down = false;
    if (shutting_down) return;
    shutting_down = true;
    CallScreen::PrepareSystemPowerOff();
    ControlCenter_SaveBeforePowerOff();

    ESP_LOGW(TAG_HOME, "%s: shutdown requested",
             reason != nullptr ? reason : "?");
    IdlePower_Stop();
    SystemChrome_DismissOverlays();
    ShowShutdownScreen();
    lv_refr_now(nullptr);
    write(1, "PWR_OFF\n", 8);

    /* FIFO above the LVGL loop (110) so the pulse train is not starved
     * by the shutdown-ring animation. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024);
    struct sched_param sp;
    std::memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 200;
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_t th;
    int rc = hi_task_create_legacy(&th, &attr, ShutdownPulseThread, nullptr);
    if (rc != 0) {
        pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
        rc = hi_task_create_legacy(&th, &attr, ShutdownPulseThread, nullptr);
    }
    pthread_attr_destroy(&attr);
    if (rc == 0) {
        pthread_detach(th);
    } else {
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
    if (s_pwr_dlg.mask != nullptr) {RaisePowerDialog(nullptr);return;}
    lv_obj_t *parent = lv_layer_sys();
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
    s_pwr_dlg.display=lv_display_get_default();
    lv_display_add_event_cb(s_pwr_dlg.display,RaisePowerDialog,LV_EVENT_REFR_START,nullptr);
    lv_obj_add_event_cb(mask,[](lv_event_t *) {
        if(s_pwr_dlg.display)lv_display_remove_event_cb_with_user_data(s_pwr_dlg.display,RaisePowerDialog,nullptr);
        s_pwr_dlg=PowerDialogUi{};
    },LV_EVENT_DELETE,nullptr);

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
lv_obj_t *CreatePage(lv_obj_t *pager, int page_index, int total_apps, PagerState *state)
{
    lv_obj_t *page = lv_obj_create(pager);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, kPanelSize, kPagerHeight);
    /* The pager already paints the opaque black background. During a swipe
     * neither moving page covers the invalidated area, so opaque pages would
     * paint that same background a second time. A transparent background does
     * not create an opacity layer; icons and labels retain their own alpha. */
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(page, kPagePadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(page, kPagePadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_column(page, kGridColGap, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, kGridRowGap, LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_grid_dsc_array(page, s_col_dsc, s_row_dsc);
    lv_obj_set_layout(page, LV_LAYOUT_GRID);
    lv_obj_set_user_data(page, reinterpret_cast<void *>(
                                   static_cast<intptr_t>(page_index)));
    (void)total_apps;

    const int start=page_index*kAppsPerPage;
    for(int i=0;i<kAppsPerPage;i++){
        const int slot=start+i;if(slot>=(int)s_app_order.size())break;
        auto*cell=CreateOrderedAppCell(page,s_app_order[slot]);if(!cell)continue;
        auto*info=CellInfoOf(cell);if(info)info->local_slot=i;
        lv_obj_set_grid_cell(cell,LV_GRID_ALIGN_STRETCH,i%kPageCols,1,LV_GRID_ALIGN_STRETCH,i/kPageCols,1);
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
    state->dots.assign(state->page_count,nullptr);

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

bool SameInstalledCatalog(const std::vector<HiApp::Record>& left,const std::vector<HiApp::Record>& right)
{
    bool same=left.size()==right.size();
    for(size_t i=0;same&&i<left.size();++i){
        const auto&a=left[i];const auto&b=right[i];
        same=a.id==b.id&&a.name==b.name&&a.version==b.version&&a.icon==b.icon&&
            (a.png?a.png->fingerprint:0)==(b.png?b.png->fingerprint:0);
    }
    return same;
}

void RefreshInstalledPages(PagerState*state)
{
    if(!state || state->rebuilding || (state->revision==HiApp::RegistryRevision()&&!s_folder_refresh) ||
       s_home_touch.active || s_edit.active || s_folder_saving) return;
    const unsigned revision=HiApp::RegistryRevision();
    auto external=HiApp::RegistrySnapshot();
    // Saving icon positions also advances RegistryRevision. It does not change
    // the icon catalog and must not tear down every page after each edit.
    const bool same=SameInstalledCatalog(external,s_external_apps);
    if(!s_folder_refresh&&same){state->revision=revision;return;}
    // lv_obj_clean() synchronously sends SCROLL_END while resetting scroll.
    // Detach before changing page_count/dots; no callback may observe half
    // of the old pager and half of the new one.
    s_folder_refresh=false;
    ++state->rebuilds;
    state->rebuilding=true;
    lv_display_t* disp=lv_display_get_default();
    if(disp) lv_display_enable_invalidation(disp,false);
    lv_obj_remove_event_cb(state->pager,OnPagerScrollEnd);
    CancelCellScaleTimer();
    for(int i=0;i<s_theme_icon_n;i++)s_theme_icons[i]=nullptr;
    s_theme_icon_n=0;
    const int old_page=state->current_page;
    lv_obj_stop_scroll_anim(state->pager);
    lv_obj_clean(state->pager);
    state->dots.clear();
    if(state->indicator){lv_obj_delete(state->indicator);state->indicator=nullptr;}
    ReconcileHomeOrder(std::move(external));
    state->revision=HiApp::RegistryRevision();
    state->page_count=(s_app_order.size()+kAppsPerPage-1)/kAppsPerPage;
    const int target=std::max(0,std::min(old_page,state->page_count-1));
    auto*pager=state->pager;
    if(state->page_count>1)CreatePage(pager,state->page_count-1,kTotalApps,state);
    for(int p=0;p<state->page_count;p++)CreatePage(pager,p,kTotalApps,state);
    if(state->page_count>1)CreatePage(pager,0,kTotalApps,state);
    EnableHomeEventBubble(pager);
    CreateIndicator(lv_obj_get_parent(pager),state);
    state->current_page=target;
    lv_obj_update_layout(pager);
    lv_obj_scroll_to_x(pager,PagerScrollXForPage(state,target),LV_ANIM_OFF);
    state->rebuilding=false;
    lv_obj_add_event_cb(pager,OnPagerScrollEnd,LV_EVENT_SCROLL_END,state);
    if(s_home_root){
        lv_obj_set_style_bg_color(s_home_root,lv_color_hex(0x000000),LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_home_root,LV_OPA_COVER,LV_PART_MAIN);
    }
    if(disp) lv_display_enable_invalidation(disp,true);
    if(s_home_root) lv_obj_invalidate(s_home_root);
    HighlightDot(state,target);
    if(s_folder_resume_edit){s_folder_resume_edit=false;EnterHomeEditMode(state);}
}

#include "desktop_folder_ui.inc"

std::string AppMatchKey(const char *text)
{
    return HiAppQuery::Key(text);
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
        "请帮我打开", "帮我打开", "请打开", "给我打开", "我想打开",
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
        {"蓝牙音乐", "music"},
        {"Hi音乐", "hi_music"},
        {"Hi 音乐", "hi_music"},
        {"嗨音乐", "hi_music"},
        {"本地音乐", "hi_music"},
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
        {"指南针", "compass"},
        {"Hi指南", "compass"},
        {"Hi 指南", "compass"},
        {"时钟", "clock"},
        {"闹钟", "clock"},
        {"秒表", "clock"},
        {"计时器", "clock"},
        {"世界时钟", "clock"},
        {"震动", "vibrate"},
        {"主题", "theme"},
        {"测试", "test"},
        {"数字人", "digital_people"},
        {"小白助手", "digital_people"},
        {"小白", "digital_people"},
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
        {"电话", "call"},
        {"通话", "call"},
        {"系统信息", "info"},
        {"信息", "info"},
        {"2048", "2048"},
        {"坦克大战", "tank"},
        {"连连看", "link_game"},
        {"水果消消乐", "fruit_game"},
        {"中国象棋", "xiangqi"},
        {"象棋", "xiangqi"},
        {"游戏", "2048"},
        {"引脚", "pin"},
        {"OpenClaw", "openclaw"},
        {"Hi万物", "openclaw"},
        {"嗨万物", "openclaw"},
        {"Hi家", "home"},
        {"嗨家", "home"},
        {"文件管理", "sd"},
        {nullptr, nullptr},
    };

    std::string key=AppMatchKey(q);
    for (int i = 0; kAliases[i].alias != nullptr; ++i) {
        if (AppMatchKey(kAliases[i].alias) == key) {
            q = kAliases[i].id;
            key=AppMatchKey(q);
            break;
        }
    }

    for (int i = 0; i < kTotalApps; ++i) {
        if (AppMatchKey(kApps[i].icon_suffix) == key)
            return &kApps[i];
        if (AppMatchKey(kApps[i].name) == key)
            return &kApps[i];
    }
    /* Substring fallback: tolerate partial id like "cam". */
    for (int i = 0; i < kTotalApps; ++i) {
        if (key.size()>=3 && AppMatchKey(kApps[i].name).find(key)!=std::string::npos)
            return &kApps[i];
        if (key.size()>=3 && AppMatchKey(kApps[i].icon_suffix).find(key)!=std::string::npos)
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
    lv_obj_t *home = s_home_root;
    if (home == nullptr) {
        write(1, "HOME_CREATE\n", 12);
        home = HomeScreen::CreateStatic();
    }
    if (home == nullptr) {
        write(1, "HOME_NULL\n", 10);
        return nullptr;
    }
    if (old_scr == home) {
        write(1, "HOME_OK\n", 8);
        return home;
    }
    screen_load_safe(home);
    if (old_scr != nullptr && old_scr != home &&
        !PanelScreen::IsPanelObject(old_scr)) {
        DeleteInactiveAppLater(old_scr);
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

lv_obj_t *HomeScreen::CreateAppIcon(lv_obj_t *parent, const char *id, int size)
{
    if(parent&&size>0&&HiApp::IsLaunchId(id))return HiApp::RegistryIcon(parent,id,size);
    const int idx = FindAppIndexBySuffix(id);
    if (!parent || idx < 0 || size <= 0) return nullptr;
    EnsureIconPathsBuilt();
    lv_obj_t *icon = nullptr;
    if (!strcmp(id, "tank")) icon = TankGameScreen::CreateIcon(parent, size);
    else if (!strcmp(id, "link_game")) icon = LinkGameScreen::CreateIcon(parent, size);
    else if (!strcmp(id, "fruit_game")) icon = FruitGameScreen::CreateIcon(parent, size);
    else if (!strcmp(id, "xiangqi")) icon = XiangqiScreen::CreateIcon(parent, size);
    else if (!strcmp(id, "sms")) icon = SmsScreen::CreateIcon(parent, size);
    else if (!strcmp(id,"compass")) icon=CompassScreen::CreateIcon(parent,size);
    else if (!strcmp(id,"clock")) icon=ClockScreen::CreateIcon(parent,size);
    else if (!strcmp(id,"app_manager")) icon=AppManagerScreen::CreateIcon(parent,size);
    else {
        icon = lv_image_create(parent);
        lv_image_set_src(icon, !strcmp(id, "home") ? HomeAppIcon() : AppIconSource(idx));
        lv_obj_set_size(icon, size, size);
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    }
    HomeIconPassiveTree(icon);
    return icon;
}

void HomeScreen::SyncInstalledAppOrder(bool prefer_current)
{
    EnsureAppOrderLoaded();bool loaded=false;auto saved=HiApp::RegistryOrder(&loaded);if(!loaded)return;
    auto current=HomeOrderedIds();if(prefer_current||s_edit.save_timer||s_order_dirty){current.insert(current.end(),saved.begin(),saved.end());saved=std::move(current);}else saved.insert(saved.end(),current.begin(),current.end());
    std::vector<std::string> available;for(int i=0;i<kTotalApps;i++)available.push_back(kApps[i].icon_suffix);for(const auto&r:HiApp::RegistrySnapshot())available.push_back("hiapp:"+r.id);
    saved.insert(saved.end(),available.begin(),available.end());std::vector<std::string> result;
    for(const auto&id:saved)if(std::find(available.begin(),available.end(),id)!=available.end()&&std::find(result.begin(),result.end(),id)==result.end())result.push_back(id);
    HiApp::RegistrySaveOrder(result);
}

bool HomeScreen::LaunchApp(const char *id_or_name)
{
    if(HiApp::IsLaunchId(id_or_name))return HiApp::Launch(id_or_name);
    const auto key=AppMatchKey(id_or_name);
    if(key.empty())return false;
    for(const auto&r:HiApp::RegistrySnapshot())if(AppMatchKey(r.id.c_str())==key||AppMatchKey(r.name.c_str())==key)return HiApp::Launch(("hiapp:"+r.id).c_str());
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

lv_obj_t *HomeScreen::OpenGrid()
{
    lv_obj_t *home = CreateStatic();
    if (home == nullptr) return nullptr;
    lv_obj_t *old = lv_screen_active();
    if (old == home) return home;
    screen_load_safe(home);
    /* Keep the smart-home panel cached while the launcher draws its first
     * frame. Deleting it immediately after lv_screen_load caused a blue
     * fallback frame on the DSI scanout path; both screens are reused. */
    return home;
}

bool HomeScreen::IsEditing(){return IsActive()&&(s_edit.active||s_folder_saving||s_folder_view.dragging||s_folder_view.editing);}

bool HomeScreen::IsActive()
{
    return s_home_foreground && s_home_root != nullptr;
}

bool HomeScreen::IsHomeObject(lv_obj_t *obj)
{
    return obj != nullptr && obj == s_home_root;
}

#ifdef HI_STABILITY_PROBE
#ifdef HI_RADIO_4G_PROBE
#include "radio_4g_probe.inc"
#elif defined(HI_CC_DISPLAY_PROBE)
#include "cc_display_probe.inc"
#else
#include "stability_probe.inc"
#endif
#endif

lv_obj_t *HomeScreen::CreateStatic()
{
    EnsureIconPathsBuilt();
    EnsureAppOrderLoaded();
    if (s_home_root != nullptr) {
        return s_home_root;
    }
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


    auto *state = new PagerState{};
    state->revision=HiApp::RegistryRevision();ReconcileHomeOrder(HiApp::RegistrySnapshot());HiApp::RegistryStart();
    page_count=(s_app_order.size()+kAppsPerPage-1)/kAppsPerPage;
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
    screen_strip_obj_chrome(pager);
    screen_kill_theme_chrome(pager);

    if (page_count > 1) {
        CreatePage(pager, page_count - 1, kTotalApps, state);
        for (int p = 0; p < page_count; ++p) {
            CreatePage(pager, p, kTotalApps, state);
        }
        CreatePage(pager, 0, kTotalApps, state);
    } else {
        CreatePage(pager, 0, kTotalApps, state);
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
    lv_obj_add_event_cb(screen, OnHomePressLost, LV_EVENT_PRESS_LOST, state);
    lv_obj_add_event_cb(screen, OnHomeScreenLoaded, LV_EVENT_SCREEN_LOADED, state);
    lv_obj_add_event_cb(screen, OnHomeScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        state);
    lv_obj_add_event_cb(screen, OnScreenDeleted, LV_EVENT_DELETE, state);

    state->apps_timer=lv_timer_create([](lv_timer_t*t){auto*s=(PagerState*)lv_timer_get_user_data(t);if(s_home_foreground)RefreshInstalledPages(s);},500,state);
    EnsureDesktopStorage();
    s_home_root = screen;
    s_icons_theme_id = ThemeManager::GetCurrentThemeId();
    write(1, "HS4\n", 4);
#ifdef HI_STABILITY_PROBE
    StabilityArm(state);
#endif
    return screen;
}

void HomeScreen::ApplyCurrentTheme()
{
    const int tid = ThemeManager::GetCurrentThemeId();
    if (s_icons_theme_id == tid || s_home_root == nullptr) {
        return;
    }
    s_icon_bank ^= 1;
    EnsureIconPathsBuilt();
    for (int i = 0; i < s_theme_icon_n; ++i) {
        lv_obj_t *icon = s_theme_icons[i];
        const int app = s_theme_icon_app[i];
        if (icon == nullptr || app < 0 || app >= kTotalApps) {
            continue;
        }
        lv_image_set_src(icon, HomeIconSource(app));
    }
    /* All widgets now reference the new bank, so old pixels can be freed. */
    ReleaseHomeIconBank(s_icon_bank ^ 1);
    s_icons_theme_id = tid;
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
    s_home_return_page = 0;
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
