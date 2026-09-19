/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * GpsScreen — ported from MetalioClaw4
 * main/display/screen/gps_screen/gps_screen.cc (2766 lines).
 *
 * 适配说明详见 gps_screen.h。本文件保留上游的完整 UI 树（tabview /
 * GPS 信息卡 / WiFi 卡 / 基站卡 / 地图全屏窗口 / 模拟定位弹框 / 缩放
 * 控件），把以下平台依赖替换为 stub：
 *
 *   - GpsService → 返回 demo snapshot（fix_valid=true，纬度 39.908823 /
 *     经度 116.397470，天安门），UI 立即展示完整数据。
 *   - HTTP /location/report/cell + static-map 下载 → stub：永远走失败
 *     分支，UI 显示「查询失败：未返回定位数据」。地图区域显示错误 pill。
 *   - Nt26Board AT+ECWIFISCAN / AT+ECBCINFO → stub：返回空响应，
 *     BuildWifiLocateBody / BuildCellLocateBody 走 「无结果」 分支。
 *   - IOExpander::GPS_POWER → no-op（NuttX IO 扩展器尚未端口化）。
 *   - LvglAllocatedImage / heap_caps_malloc → malloc + 不渲染（地图 PNG
 *     永远走 stub 失败路径，所以解码逻辑保留但实际不会触发）。
 *   - esp_lv_adapter_lock / unlock → lv_lock / lv_unlock。
 *   - portMUX_TYPE → std::mutex（更稳，开销可忽略）。
 *   - <cmath> → __builtin_* 内置。
 */

#include "gps_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "freertos_shim.h"
#include "esp_err_shim.h"
#include "settings.h"
#include "api_endpoints.h"
#include "system_info.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"
#include <metalio/metalio.h>

#include "cJSON_compat.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// NuttX libc lacks math.h — use compiler builtins instead of <cmath>.
// std::isnan / std::isinf are still reachable via <cmath> on NuttX newlib,
// but to be safe we provide local wrappers using builtin classification.
#if defined(__GNUC__)
static inline bool GpsIsNan(double v) { return __builtin_isnan(v); }
static inline bool GpsIsInf(double v) { return __builtin_isinf(v); }
static inline double GpsFabs(double v) { return __builtin_fabs(v); }
#else
static inline bool GpsIsNan(double v) { return v != v; }
static inline bool GpsIsInf(double v) { return v == v + 1.0; }
static inline double GpsFabs(double v) { return v < 0 ? -v : v; }
#endif

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char *TAG = "GpsScreen";

// ---------------------------------------------------------------------------
// Layout constants (720x720) — verbatim from upstream
// ---------------------------------------------------------------------------
constexpr int32_t kPanelSize       = 720;
constexpr int32_t kCardWidth       = 672;
constexpr int32_t kCardHeight      = 640;
constexpr int32_t kCardHeightNet   = 380;
constexpr int32_t kCardRadius      = 24;
constexpr int32_t kCardPadHor      = 32;
constexpr int32_t kCardPadVer      = 24;
constexpr int32_t kTitleHeight     = 56;
constexpr int32_t kRowHeight       = 56;
constexpr int32_t kGpsRowCount     = 7;
constexpr int32_t kNetRowCount     = 4;
constexpr int32_t kRefreshPeriodMs = 500;

constexpr int32_t kHeaderH         = 90;
constexpr int32_t kBackBtnSize     = 72;
constexpr int32_t kTabBarH         = 56;
constexpr int32_t kMapToolbarH     = 56;
constexpr int32_t kMapImageSize    = 672;
constexpr int      kMinZoom         = 3;
constexpr int      kMaxZoom         = 18;
constexpr int      kDefaultZoom     = 18;
constexpr int      kHttpTimeoutMs   = 15000;
constexpr size_t   kMapMaxBytes     = 4 * 1024 * 1024;

constexpr const char *kNvsNamespace  = "gps";
constexpr const char *kNvsKeyZoom    = "amap_zoom";

constexpr const char *kNvsKeyMockEn   = "mock_en";
constexpr const char *kNvsKeyMockLat  = "mock_lat";
constexpr const char *kNvsKeyMockLon  = "mock_lon";
constexpr double      kMockDefaultLat = 39.908823;
constexpr double      kMockDefaultLon = 116.397470;

constexpr uint32_t kColorBg          = 0x0E1116;
constexpr uint32_t kColorBgGrad      = 0x161A22;
constexpr uint32_t kColorCard        = 0x1B2030;
constexpr uint32_t kColorDivider     = 0x2C3344;
constexpr uint32_t kColorText        = 0xFFFFFF;
constexpr uint32_t kColorSubtle      = 0x9AA3B2;
constexpr uint32_t kColorAccent      = 0xE0FB3C;
constexpr uint32_t kColorBadgeFix    = 0x1F8A4C;
constexpr uint32_t kColorBadgeSearch = 0xC4761A;
constexpr uint32_t kColorBadgeMock   = 0x7C3AED;

enum GpsInfoRow {
    kRowLat = 0,
    kRowLon,
    kRowAlt,
    kRowSats,
    kRowHdop,
    kRowSpeed,
    kRowAddress,
};

enum NetInfoRow {
    kNetRowLat = 0,
    kNetRowLon,
    kNetRowAcc,
    kNetRowAddress,
};

enum class LocMode : uint8_t {
    kGps  = 0,
    kWifi = 1,
    kCell = 2,
    kCount = 3,
};

struct MockDialogUi {
    lv_obj_t *mask         = nullptr;
    lv_obj_t *card         = nullptr;
    lv_obj_t *status_lbl   = nullptr;
    lv_obj_t *lat_ta       = nullptr;
    lv_obj_t *lon_ta       = nullptr;
    lv_obj_t *keyboard     = nullptr;
};

struct TabLocState {
    lv_obj_t *status_badge = nullptr;
    lv_obj_t *status_label = nullptr;
    lv_obj_t *fetch_btn    = nullptr;
    lv_obj_t *map_entry    = nullptr;
    lv_obj_t *map_entry_sub = nullptr;
    lv_obj_t *value_labels[kGpsRowCount] = {};
    lv_obj_t *net_labels[kNetRowCount]   = {};

    bool location_loading = false;
    bool has_map          = false;
    bool has_locate_result = false;
    double result_lat     = 0.0;
    double result_lon     = 0.0;
    double result_acc     = 0.0;
    std::string result_address;

    std::string cached_at_resp;
};

struct MapWindowState {
    lv_obj_t *overlay         = nullptr;
    lv_obj_t *map_stage       = nullptr;
    lv_obj_t *map_image       = nullptr;
    lv_obj_t *map_loading_scrim = nullptr;
    lv_obj_t *map_status_lbl  = nullptr;
    lv_obj_t *map_zoom_dd     = nullptr;
    lv_obj_t *map_zoom_out_btn = nullptr;
    lv_obj_t *map_zoom_in_btn = nullptr;
    int32_t   stage_pixel_size = 0;
    bool visible              = false;
};

struct ScreenState {
    lv_obj_t   *screen        = nullptr;
    lv_obj_t   *tabview       = nullptr;
    lv_timer_t *refresh_timer = nullptr;
    MockDialogUi mock_dlg;
};

ScreenState s_state;
TabLocState s_tab_state[static_cast<int>(LocMode::kCount)];
MapWindowState s_map_win;
LocMode s_active_tab = LocMode::kGps;

std::atomic<uint32_t> s_location_session{0};
std::atomic<int> s_zoom_level{kDefaultZoom};

// mock lock — std::mutex is plenty (only a few lines of assignment under it).
std::mutex s_mock_lock;
bool         s_mock_enabled = false;
double       s_mock_lat_deg = kMockDefaultLat;
double       s_mock_lon_deg = kMockDefaultLon;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
void StripChrome(lv_obj_t *obj) {
    if (obj == nullptr) return;
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

TabLocState &TabState(LocMode mode) {
    return s_tab_state[static_cast<int>(mode)];
}

TabLocState &ActiveTabState() { return TabState(s_active_tab); }

struct MapViewParams {
    int zoom   = kDefaultZoom;
    int width  = 0;
    int height = 0;
};

int32_t CalcMapStagePixelSize() {
    const int32_t stage_y = kHeaderH + kMapToolbarH + 20;
    const int32_t stage_h = kPanelSize - stage_y - 12;
    return std::min<int32_t>(kMapImageSize, stage_h);
}

int32_t GetMapStagePixelSize() {
    if (s_map_win.stage_pixel_size > 0) {
        return s_map_win.stage_pixel_size;
    }
    return CalcMapStagePixelSize();
}

MapViewParams GetMapViewParams() {
    const int32_t px = GetMapStagePixelSize();
    int zoom = s_zoom_level.load(std::memory_order_relaxed);
    if (zoom < kMinZoom || zoom > kMaxZoom) {
        zoom = kDefaultZoom;
    }
    return MapViewParams{zoom, static_cast<int>(px), static_cast<int>(px)};
}

void UpdateMapStageMetrics(lv_obj_t *stage, int32_t stage_w) {
    s_map_win.map_stage = stage;
    s_map_win.stage_pixel_size = stage_w;
    if (stage != nullptr) {
        lv_obj_set_size(stage, stage_w, stage_w);
    }
}

// ---------------------------------------------------------------------------
// GpsService — pulls a real fix from the board NMEA reader.
// The GNSS module is powered on and its reader thread started by board
// bringup (metalio_gps_initialize); GetSnapshot() reads the latest GGA fix
// through metalio_gps_get_snapshot(). altitude/hdop/speed are not exposed
// by the board API, so they remain 0 and the UI shows "--" for those rows.
// ---------------------------------------------------------------------------
namespace GpsService {

struct Snapshot {
    bool started        = false;
    bool fix_valid      = false;
    uint8_t fix_quality = 0;
    double latitude_deg  = 0.0;
    double longitude_deg = 0.0;
    double altitude_m    = 0.0;
    double hdop          = 0.0;
    double speed_kmh     = 0.0;
    uint16_t satellites_used = 0;
    uint16_t satellites_view = 0;
    char utc_time[24] = {};
};

inline void Start() {
    // The board GPS reader thread runs from boot; nothing to start here.
}

inline Snapshot GetSnapshot() {
    Snapshot s;
    s.started = true;

    int fix_quality = 0;
    int sats = 0;
    double lat = 0.0;
    double lon = 0.0;
    if (metalio_gps_get_snapshot(&fix_quality, &sats, &lat, &lon) < 0) {
        // No GGA sentence received yet.
        return s;
    }

    s.fix_quality = static_cast<uint8_t>(fix_quality);
    s.satellites_used = static_cast<uint16_t>(sats);
    s.satellites_view = static_cast<uint16_t>(sats);
    if (fix_quality > 0 && (lat != 0.0 || lon != 0.0)) {
        s.fix_valid = true;
        s.latitude_deg = lat;
        s.longitude_deg = lon;
    }
    return s;
}

}  // namespace GpsService

// ---------------------------------------------------------------------------
// Device info stubs (replace Nt26Board AT+CGSN + SystemInfo MAC)
// ---------------------------------------------------------------------------
std::string GetDeviceImei() {
    // Stub IMEI — openvela does not yet expose a 4G modem AT shim.
    return "000000000000000";
}

std::string CurrentUnixTimeStr() {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%lu",
                  static_cast<unsigned long>(time(nullptr)));
    return buf;
}

void ToLowerInPlace(std::string &s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
}

unsigned long ParseHexU32(const char *hex) {
    if (hex == nullptr || hex[0] == '\0') {
        return 0;
    }
    return std::strtoul(hex, nullptr, 16);
}

const char *FindCaseInsensitive(const char *hay, const char *needle) {
    if (hay == nullptr || needle == nullptr || needle[0] == '\0') {
        return nullptr;
    }
    const size_t nlen = std::strlen(needle);
    for (const char *p = hay; *p != '\0'; ++p) {
        size_t i = 0;
        for (; i < nlen && p[i] != '\0'; ++i) {
            char a = p[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) break;
        }
        if (i == nlen) {
            return p;
        }
    }
    return nullptr;
}

bool ReadParenCsvToken(const char *&p, std::string &tok) {
    tok.clear();
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '\0' || *p == ')') {
        return false;
    }
    if (*p == '"') {
        ++p;
        while (*p != '\0' && *p != '"') {
            tok.push_back(*p++);
        }
        if (*p == '"') {
            ++p;
        }
    } else {
        while (*p != '\0' && *p != ',' && *p != ')') {
            tok.push_back(*p++);
        }
    }
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == ',') {
        ++p;
    }
    return true;
}

lv_obj_t *MakeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font,
                    lv_color_t color, lv_text_align_t align) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, color, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, align, LV_PART_MAIN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    screen_make_input_passive(lbl);
    return lbl;
}

lv_obj_t *MakeDivider(lv_obj_t *parent) {
    lv_obj_t *line = lv_obj_create(parent);
    StripChrome(line);
    lv_obj_set_size(line, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(kColorDivider), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(line);
    return line;
}

lv_obj_t *MakeInfoRow(lv_obj_t *parent, const char *label_text,
                      bool with_divider) {
    lv_obj_t *wrap = lv_obj_create(parent);
    StripChrome(wrap);
    lv_obj_set_size(wrap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *row = lv_obj_create(wrap);
    StripChrome(row);
    lv_obj_set_size(row, LV_PCT(100), kRowHeight);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = MakeLabel(row, label_text, &font_puhui_20_4,
                              lv_color_hex(kColorSubtle),
                              LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(lbl, 0);

    lv_obj_t *value = MakeLabel(row, "--", &font_puhui_30_4,
                                lv_color_hex(kColorText),
                                LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_style_pad_left(value, 16, LV_PART_MAIN);

    if (with_divider) {
        MakeDivider(wrap);
    }

    return value;
}

// ---------------------------------------------------------------------------
// 「模拟定位」开关 / 经纬度
// ---------------------------------------------------------------------------
bool ParseDoubleSafe(const char *s, double &out) {
    if (s == nullptr || s[0] == '\0') return false;
    errno = 0;
    char *end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s || errno == ERANGE || GpsIsNan(v) || GpsIsInf(v)) {
        return false;
    }
    out = v;
    return true;
}

void LoadMockSettings() {
    Settings settings(kNvsNamespace, false);
    const bool        en   = settings.GetBool(kNvsKeyMockEn, false);
    const std::string lats = settings.GetString(kNvsKeyMockLat, "");
    const std::string lons = settings.GetString(kNvsKeyMockLon, "");
    double lat = kMockDefaultLat;
    double lon = kMockDefaultLon;
    ParseDoubleSafe(lats.c_str(), lat);
    ParseDoubleSafe(lons.c_str(), lon);
    if (lat < -90.0  || lat > 90.0)  lat = kMockDefaultLat;
    if (lon < -180.0 || lon > 180.0) lon = kMockDefaultLon;
    std::lock_guard<std::mutex> g(s_mock_lock);
    s_mock_enabled = en;
    s_mock_lat_deg = lat;
    s_mock_lon_deg = lon;
}

void SaveMockSettings(bool enabled, double lat, double lon) {
    Settings settings(kNvsNamespace, true);
    settings.SetBool(kNvsKeyMockEn, enabled);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", lat);
    settings.SetString(kNvsKeyMockLat, buf);
    std::snprintf(buf, sizeof(buf), "%.6f", lon);
    settings.SetString(kNvsKeyMockLon, buf);
    std::lock_guard<std::mutex> g(s_mock_lock);
    s_mock_enabled = enabled;
    s_mock_lat_deg = lat;
    s_mock_lon_deg = lon;
}

bool MockIsEnabled() {
    std::lock_guard<std::mutex> g(s_mock_lock);
    return s_mock_enabled;
}

GpsService::Snapshot GetEffectiveSnapshot() {
    GpsService::Snapshot snap = GpsService::GetSnapshot();
    bool   en;
    double lat;
    double lon;
    {
        std::lock_guard<std::mutex> g(s_mock_lock);
        en  = s_mock_enabled;
        lat = s_mock_lat_deg;
        lon = s_mock_lon_deg;
    }
    if (!en) {
        return snap;
    }
    snap.started        = true;
    snap.fix_valid      = true;
    snap.fix_quality    = std::max<uint8_t>(snap.fix_quality, 1);
    snap.latitude_deg   = lat;
    snap.longitude_deg  = lon;
    if (snap.satellites_used == 0) snap.satellites_used = 8;
    if (snap.satellites_view == 0) snap.satellites_view = 12;
    if (snap.hdop == 0.0)          snap.hdop            = 1.0;
    if (snap.altitude_m == 0.0)    snap.altitude_m      = 50.0;
    if (snap.utc_time[0] == '\0') {
        std::snprintf(snap.utc_time, sizeof(snap.utc_time), "MOCK");
    }
    return snap;
}

// ---------------------------------------------------------------------------
// Refresh timer — pulls a fresh snapshot and pushes it into every label.
// ---------------------------------------------------------------------------
void SetFetchEnabled(LocMode mode, bool enabled);
void RefreshGpsTabFromService();

void FormatLatitude(double deg, char *buf, size_t buf_size) {
    if (GpsIsNan(deg) || deg == 0.0) {
        std::snprintf(buf, buf_size, "--");
        return;
    }
    const char hemi = (deg >= 0.0) ? 'N' : 'S';
    std::snprintf(buf, buf_size, "%.6f\xC2\xB0 %c", GpsFabs(deg), hemi);
}

void FormatLongitude(double deg, char *buf, size_t buf_size) {
    if (GpsIsNan(deg) || deg == 0.0) {
        std::snprintf(buf, buf_size, "--");
        return;
    }
    const char hemi = (deg >= 0.0) ? 'E' : 'W';
    std::snprintf(buf, buf_size, "%.6f\xC2\xB0 %c", GpsFabs(deg), hemi);
}

void RefreshGpsTabFromService() {
    auto &tab = TabState(LocMode::kGps);
    if (s_state.screen == nullptr) {
        return;
    }
    const GpsService::Snapshot snap = GetEffectiveSnapshot();

    const char *status_text = I18n::T("未启动");
    uint32_t    badge_color = kColorBadgeSearch;
    if (MockIsEnabled()) {
        status_text = I18n::T("模拟定位");
        badge_color = kColorBadgeMock;
    } else if (snap.fix_valid) {
        status_text = I18n::T("已定位");
        badge_color = kColorBadgeFix;
    } else if (snap.started) {
        status_text = I18n::T("搜星中");
        badge_color = kColorBadgeSearch;
    }
    if (tab.status_label != nullptr) {
        lv_label_set_text(tab.status_label, status_text);
    }
    if (tab.status_badge != nullptr) {
        lv_obj_set_style_bg_color(tab.status_badge,
                                  lv_color_hex(badge_color), LV_PART_MAIN);
    }

    char lat_buf[40];
    char lon_buf[40];
    if (snap.fix_valid) {
        FormatLatitude(snap.latitude_deg, lat_buf, sizeof(lat_buf));
        FormatLongitude(snap.longitude_deg, lon_buf, sizeof(lon_buf));
    } else {
        std::snprintf(lat_buf, sizeof(lat_buf), "--");
        std::snprintf(lon_buf, sizeof(lon_buf), "--");
    }
    lv_label_set_text(tab.value_labels[kRowLat], lat_buf);
    lv_label_set_text(tab.value_labels[kRowLon], lon_buf);

    char alt_buf[24];
    if (snap.fix_valid) {
        std::snprintf(alt_buf, sizeof(alt_buf), "%.1f m", snap.altitude_m);
    } else {
        std::snprintf(alt_buf, sizeof(alt_buf), "--");
    }
    lv_label_set_text(tab.value_labels[kRowAlt], alt_buf);

    char sats_buf[24];
    if (snap.satellites_view > 0 || snap.satellites_used > 0) {
        std::snprintf(sats_buf, sizeof(sats_buf), "%u / %u",
                      static_cast<unsigned>(snap.satellites_used),
                      static_cast<unsigned>(snap.satellites_view));
    } else {
        std::snprintf(sats_buf, sizeof(sats_buf), "--");
    }
    lv_label_set_text(tab.value_labels[kRowSats], sats_buf);

    char hdop_buf[16];
    if (snap.hdop > 0.0) {
        std::snprintf(hdop_buf, sizeof(hdop_buf), "%.1f", snap.hdop);
    } else {
        std::snprintf(hdop_buf, sizeof(hdop_buf), "--");
    }
    lv_label_set_text(tab.value_labels[kRowHdop], hdop_buf);

    char speed_buf[24];
    if (snap.fix_valid) {
        std::snprintf(speed_buf, sizeof(speed_buf), "%.1f km/h",
                      snap.speed_kmh);
    } else {
        std::snprintf(speed_buf, sizeof(speed_buf), "--");
    }
    lv_label_set_text(tab.value_labels[kRowSpeed], speed_buf);

    if (tab.fetch_btn != nullptr) {
        const bool can_fetch = snap.fix_valid && !tab.location_loading;
        const bool is_disabled =
            lv_obj_has_state(tab.fetch_btn, LV_STATE_DISABLED);
        if (can_fetch && is_disabled) {
            SetFetchEnabled(LocMode::kGps, true);
        } else if (!can_fetch && !is_disabled) {
            SetFetchEnabled(LocMode::kGps, false);
        }
    }
    if (!snap.fix_valid && tab.value_labels[kRowAddress] != nullptr &&
        !tab.location_loading) {
        const char *cur =
            lv_label_get_text(tab.value_labels[kRowAddress]);
        if (cur == nullptr || std::strcmp(cur, "--") != 0) {
            lv_label_set_text(tab.value_labels[kRowAddress], "--");
        }
    }
}

void RefreshFromService() { RefreshGpsTabFromService(); }

void OnRefreshTimer(lv_timer_t * /*timer*/) {
    RefreshFromService();
}

// zoom 持久化
int LoadZoom() {
    Settings settings(kNvsNamespace, false);
    int v = settings.GetInt(kNvsKeyZoom, kDefaultZoom);
    if (v < kMinZoom || v > kMaxZoom) v = kDefaultZoom;
    return v;
}

void SaveZoom(int v) {
    if (v < kMinZoom || v > kMaxZoom) return;
    Settings settings(kNvsNamespace, true);
    settings.SetInt(kNvsKeyZoom, v);
}

// ---------------------------------------------------------------------------
// 定位上报：地址反查 + 静态地图
// ---------------------------------------------------------------------------
struct LocationReportResult {
    uint32_t    session = 0;
    LocMode     mode    = LocMode::kGps;
    bool        zoom_refresh_only = false;
    bool        ok      = false;
    std::string address;
    double      latitude  = 0.0;
    double      longitude = 0.0;
    double      accuracy  = 0.0;
    void       *map_data = nullptr;
    size_t      map_size = 0;
    std::string err;
    std::string map_err;
};

enum MapStatusStyle {
    kMapStatusHidden  = 0,
    kMapStatusHint,
    kMapStatusLoading,
    kMapStatusError,
    kMapStatusInfo,
};

void SetMapStatus(const char *text, MapStatusStyle style) {
    if (s_map_win.map_status_lbl == nullptr) {
        return;
    }

    const bool hidden = style == kMapStatusHidden || text == nullptr || text[0] == '\0';
    if (hidden) {
        lv_label_set_text(s_map_win.map_status_lbl, "");
        lv_obj_add_flag(s_map_win.map_status_lbl, LV_OBJ_FLAG_HIDDEN);
        if (s_map_win.map_loading_scrim != nullptr) {
            lv_obj_add_flag(s_map_win.map_loading_scrim, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    lv_label_set_text(s_map_win.map_status_lbl, text);
    lv_obj_remove_flag(s_map_win.map_status_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_bg_opa(s_map_win.map_status_lbl, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_map_win.map_status_lbl, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_map_win.map_status_lbl, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_map_win.map_status_lbl, 0, LV_PART_MAIN);
    lv_obj_set_width(s_map_win.map_status_lbl, LV_PCT(90));

    if (s_map_win.map_loading_scrim != nullptr) {
        if (style == kMapStatusLoading) {
            lv_obj_remove_flag(s_map_win.map_loading_scrim, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_map_win.map_loading_scrim, LV_OBJ_FLAG_HIDDEN);
        }
    }

    switch (style) {
    case kMapStatusLoading:
        lv_obj_set_width(s_map_win.map_status_lbl, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(s_map_win.map_status_lbl,
                                   static_cast<int32_t>(kMapImageSize - 48),
                                   LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_map_win.map_status_lbl,
                                  lv_color_hex(0x2563EB), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_map_win.map_status_lbl, LV_OPA_COVER,
                                LV_PART_MAIN);
        lv_obj_set_style_pad_hor(s_map_win.map_status_lbl, 28, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(s_map_win.map_status_lbl, 16, LV_PART_MAIN);
        lv_obj_set_style_radius(s_map_win.map_status_lbl, 18, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_map_win.map_status_lbl,
                                    lv_color_hex(kColorAccent), LV_PART_MAIN);
        break;
    case kMapStatusError:
        lv_obj_set_style_bg_color(s_map_win.map_status_lbl,
                                  lv_color_hex(0x7F1D1D), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_map_win.map_status_lbl, LV_OPA_80,
                                LV_PART_MAIN);
        lv_obj_set_style_pad_hor(s_map_win.map_status_lbl, 20, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(s_map_win.map_status_lbl, 12, LV_PART_MAIN);
        lv_obj_set_style_radius(s_map_win.map_status_lbl, 14, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_map_win.map_status_lbl,
                                    lv_color_hex(0xFCA5A5), LV_PART_MAIN);
        break;
    case kMapStatusInfo:
        lv_obj_set_style_text_color(s_map_win.map_status_lbl,
                                    lv_color_hex(kColorText), LV_PART_MAIN);
        break;
    case kMapStatusHint:
    default:
        lv_obj_set_style_text_color(s_map_win.map_status_lbl,
                                    lv_color_hex(kColorSubtle), LV_PART_MAIN);
        break;
    }
    lv_obj_center(s_map_win.map_status_lbl);
}

// ---------------------------------------------------------------------------
// HTTP / static-map stub. The original streams an HTTP response body to PSRAM
// and decodes it as PNG; the openvela port does not yet expose an HTTP client,
// so we keep the BinaryDownloadOutcome shape and return failure. UI then
// shows the "查询失败" / map-error path.
// ---------------------------------------------------------------------------
struct BinaryDownloadOutcome {
    bool        ok   = false;
    void       *data = nullptr;
    size_t      size = 0;
    std::string err;
};

BinaryDownloadOutcome DownloadBinary(void * /*http*/, const std::string & /*url*/) {
    BinaryDownloadOutcome out;
    out.err = I18n::T("HTTP 不可用");
    return out;
}

void SetFetchEnabled(LocMode mode, bool enabled) {
    auto &tab = TabState(mode);
    if (tab.fetch_btn == nullptr) {
        return;
    }
    if (enabled) {
        lv_obj_remove_state(tab.fetch_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(tab.fetch_btn, lv_color_hex(0x1F8A4C),
                                  LV_PART_MAIN);
    } else {
        lv_obj_add_state(tab.fetch_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(tab.fetch_btn, lv_color_hex(0x4B5563),
                                  LV_PART_MAIN);
    }
}

void UpdateMapEntryState(LocMode mode) {
    auto &tab = TabState(mode);
    if (tab.map_entry == nullptr) {
        return;
    }
    if (tab.has_map) {
        lv_obj_remove_state(tab.map_entry, LV_STATE_DISABLED);
        if (tab.map_entry_sub != nullptr) {
            lv_label_set_text(tab.map_entry_sub, I18n::T("查看 >"));
        }
    } else {
        lv_obj_add_state(tab.map_entry, LV_STATE_DISABLED);
        if (tab.map_entry_sub != nullptr) {
            lv_label_set_text(tab.map_entry_sub, I18n::T("请先获取定位"));
        }
    }
}

void UpdateNetTabLabels(LocMode mode, double lat, double lon, double acc) {
    auto &tab = TabState(mode);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.6f", lat);
    if (tab.net_labels[kNetRowLat] != nullptr) {
        lv_label_set_text(tab.net_labels[kNetRowLat], buf);
    }
    std::snprintf(buf, sizeof(buf), "%.6f", lon);
    if (tab.net_labels[kNetRowLon] != nullptr) {
        lv_label_set_text(tab.net_labels[kNetRowLon], buf);
    }
    std::snprintf(buf, sizeof(buf), "%.0f m", acc);
    if (tab.net_labels[kNetRowAcc] != nullptr) {
        lv_label_set_text(tab.net_labels[kNetRowAcc], buf);
    }
}

lv_obj_t *AddressLabelForMode(LocMode mode) {
    auto &tab = TabState(mode);
    if (mode == LocMode::kGps) {
        return tab.value_labels[kRowAddress];
    }
    return tab.net_labels[kNetRowAddress];
}

// JSON helpers kept verbatim — they only fire if a future HTTP stub returns
// a real response. Currently ParseLocationResponse() is only called from
// SubmitLocationReport(), which is itself stubbed to fail before parsing.
bool JsonApiCodeOk(cJSON *code) {
    if (code == nullptr) {
        return false;
    }
    if (cJSON_IsNumber(code)) {
        return code->valuedouble == 0.0;
    }
    if (cJSON_IsString(code) && code->valuestring != nullptr) {
        return std::strcmp(code->valuestring, "0") == 0;
    }
    return false;
}

std::string TrimAscii(const std::string &s) {
    size_t start = 0;
    while (start < s.size() &&
           std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string JsonItemString(cJSON *item) {
    if (item == nullptr || cJSON_IsNull(item)) {
        return {};
    }
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return TrimAscii(item->valuestring);
    }
    return {};
}

double JsonItemNumber(cJSON *item, double fallback) {
    if (item == nullptr || cJSON_IsNull(item)) {
        return fallback;
    }
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        char *end = nullptr;
        const double v = std::strtod(item->valuestring, &end);
        if (end != item->valuestring && !GpsIsNan(v) && !GpsIsInf(v)) {
            return v;
        }
    }
    return fallback;
}

std::string ExtractDataAddressField(const std::string &resp_json,
                                    cJSON *data) {
    if (data != nullptr) {
        cJSON *addr = cJSON_GetObjectItem(data, "address");
        if (addr == nullptr) {
            addr = cJSON_GetObjectItemCaseSensitive(data, "address");
        }
        if (cJSON_IsString(addr) && addr->valuestring != nullptr) {
            const std::string s = TrimAscii(addr->valuestring);
            if (!s.empty()) {
                return s;
            }
        }
    }

    const char *marker = "\"address\"";
    size_t pos         = resp_json.find(marker);
    if (pos == std::string::npos) {
        return {};
    }
    pos = resp_json.find(':', pos + std::strlen(marker));
    if (pos == std::string::npos) {
        return {};
    }
    ++pos;
    while (pos < resp_json.size() &&
           std::isspace(static_cast<unsigned char>(resp_json[pos]))) {
        ++pos;
    }
    if (pos >= resp_json.size() || resp_json[pos] != '"') {
        return {};
    }
    ++pos;

    std::string out;
    out.reserve(96);
    for (; pos < resp_json.size(); ++pos) {
        const char c = resp_json[pos];
        if (c == '"') {
            break;
        }
        if (c == '\\' && pos + 1 < resp_json.size()) {
            out.push_back(resp_json[++pos]);
        } else {
            out.push_back(c);
        }
    }
    return TrimAscii(out);
}

std::string ReadHttpTextBody(void * /*http*/, size_t max_bytes = 256 * 1024) {
    // Stub — no HTTP client. Always returns empty body.
    (void)max_bytes;
    return {};
}

void SetAddressLabelText(LocMode mode, TabLocState &tab,
                         const std::string &address) {
    lv_obj_t *addr_lbl = AddressLabelForMode(mode);
    if (addr_lbl == nullptr) {
        return;
    }
    if (!address.empty()) {
        tab.result_address = address;
        lv_label_set_text(addr_lbl, address.c_str());
        return;
    }
    if (!tab.result_address.empty()) {
        lv_label_set_text(addr_lbl, tab.result_address.c_str());
        return;
    }
    lv_label_set_text(addr_lbl, "--");
}

bool ParseLocationResponse(const std::string &resp, LocationReportResult *res,
                           std::string &static_map_url) {
    static_map_url.clear();
    res->ok = false;

    cJSON *root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        res->err = I18n::T("响应解析失败");
        return false;
    }

    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON *msg  = cJSON_GetObjectItemCaseSensitive(root, "msg");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");

    if (!JsonApiCodeOk(code) || !cJSON_IsObject(data)) {
        if (cJSON_IsString(msg) && msg->valuestring != nullptr &&
            msg->valuestring[0] != '\0' &&
            !JsonApiCodeOk(code)) {
            res->err = msg->valuestring;
        } else if (!cJSON_IsObject(data)) {
            res->err = I18n::T("未返回 data");
        } else {
            res->err = I18n::T("接口返回错误");
        }
        cJSON_Delete(root);
        return false;
    }

    cJSON *map_url =
        cJSON_GetObjectItemCaseSensitive(data, "staticMapUrl");
    cJSON *lat = cJSON_GetObjectItemCaseSensitive(data, "latitude");
    cJSON *lon = cJSON_GetObjectItemCaseSensitive(data, "longitude");
    cJSON *acc = cJSON_GetObjectItemCaseSensitive(data, "accuracy");

    if (cJSON_IsString(map_url) && map_url->valuestring != nullptr) {
        static_map_url = map_url->valuestring;
    }

    res->address = ExtractDataAddressField(resp, data);
    res->latitude  = JsonItemNumber(lat, res->latitude);
    res->longitude = JsonItemNumber(lon, res->longitude);
    res->accuracy  = JsonItemNumber(acc, res->accuracy);

    if (!res->address.empty()) {
        ESP_LOGI(TAG, "parsed address=%s", res->address.c_str());
    } else {
        ESP_LOGW(TAG, "response missing data.address");
    }

    const bool has_coords =
        !GpsIsNan(res->latitude) && !GpsIsNan(res->longitude);
    const bool has_addr = !res->address.empty();

    if (!has_coords && !has_addr) {
        if (cJSON_IsString(msg) && msg->valuestring != nullptr &&
            msg->valuestring[0] != '\0') {
            res->err = msg->valuestring;
        } else {
            res->err = I18n::T("未返回定位数据");
        }
        cJSON_Delete(root);
        return false;
    }

    res->ok = true;
    res->err.clear();
    cJSON_Delete(root);
    return true;
}

const char *LocModeName(LocMode mode) {
    switch (mode) {
    case LocMode::kGps:
        return "gps";
    case LocMode::kWifi:
        return "wifi";
    case LocMode::kCell:
        return "cell";
    default:
        return "unknown";
    }
}

void LogLocateHttpJson(LocMode mode, const std::string &req_json, int http_status,
                       const std::string &resp_json) {
    ESP_LOGI(TAG, "locate[%s] request json=%s", LocModeName(mode),
             req_json.empty() ? "(empty)" : req_json.c_str());
    const std::string safe_resp = api::RedactClawUrlsForLog(resp_json);
    ESP_LOGI(TAG, "locate[%s] response status=%d json=%s", LocModeName(mode),
             http_status,
             safe_resp.empty() ? "(empty)" : safe_resp.c_str());
}

bool DownloadMapForResult(void * /*network*/,
                          const std::string &static_map_url,
                          LocationReportResult *res) {
    if (static_map_url.empty()) {
        res->map_err = I18n::T("未返回 staticMapUrl");
        return false;
    }
    BinaryDownloadOutcome map_dl =
        DownloadBinary(nullptr, static_map_url);
    if (!map_dl.ok) {
        res->map_err = map_dl.err.empty() ? I18n::T("地图下载失败") : map_dl.err;
        return false;
    }
    res->map_data = map_dl.data;
    res->map_size = map_dl.size;
    return true;
}

// lv_async_call wraps a lambda/function pointer + arg and runs it on the
// LVGL thread. Used by worker tasks to push results back to the UI.
void OnLocationReportDone(void *user_data) {
    auto *res = static_cast<LocationReportResult *>(user_data);
    if (res == nullptr) {
        return;
    }

    const bool stale =
        s_state.screen == nullptr ||
        res->session != s_location_session.load(std::memory_order_acquire);
    if (stale) {
        if (res->map_data != nullptr) {
            free(res->map_data);
        }
        delete res;
        return;
    }

    auto &tab = TabState(res->mode);
    tab.location_loading = false;

    if (!res->zoom_refresh_only) {
        if (res->mode == LocMode::kGps) {
            const GpsService::Snapshot snap = GetEffectiveSnapshot();
            SetFetchEnabled(LocMode::kGps, snap.fix_valid);
        } else {
            SetFetchEnabled(res->mode, true);
            if (tab.status_label != nullptr) {
                lv_label_set_text(tab.status_label,
                                  res->ok ? I18n::T("已定位") : I18n::T("定位失败"));
            }
            if (tab.status_badge != nullptr) {
                lv_obj_set_style_bg_color(
                    tab.status_badge,
                    lv_color_hex(res->ok ? kColorBadgeFix : kColorBadgeSearch),
                    LV_PART_MAIN);
            }
        }

        lv_obj_t *addr_lbl = AddressLabelForMode(res->mode);
        if (addr_lbl != nullptr) {
            if (res->ok) {
                SetAddressLabelText(res->mode, tab, res->address);
            } else {
                const std::string err_text =
                    res->err.empty() ? I18n::T("未知错误") : res->err;
                const std::string msg = std::string(I18n::T("查询失败：")) + err_text;
                lv_label_set_text(addr_lbl, msg.c_str());
            }
        }

        if (res->ok) {
            tab.has_locate_result = true;
            tab.result_lat = res->latitude;
            tab.result_lon = res->longitude;
            tab.result_acc = res->accuracy;
            if (res->mode == LocMode::kWifi || res->mode == LocMode::kCell) {
                UpdateNetTabLabels(res->mode, res->latitude, res->longitude,
                                   res->accuracy);
            }
        }
    } else if (res->ok) {
        tab.has_locate_result = true;
        tab.result_lat = res->latitude;
        tab.result_lon = res->longitude;
        tab.result_acc = res->accuracy;
        if (!res->address.empty()) {
            tab.result_address = res->address;
        }
        if (res->mode == LocMode::kGps) {
            const GpsService::Snapshot snap = GetEffectiveSnapshot();
            SetFetchEnabled(LocMode::kGps, snap.fix_valid);
        } else {
            SetFetchEnabled(res->mode, true);
        }
    } else if (res->mode == LocMode::kGps) {
        const GpsService::Snapshot snap = GetEffectiveSnapshot();
        SetFetchEnabled(LocMode::kGps, snap.fix_valid);
    } else {
        SetFetchEnabled(res->mode, true);
    }

    if (res->map_data != nullptr && res->map_size > 0) {
        // No real PNG decoder wired up yet — just free the buffer.
        free(res->map_data);
        res->map_data = nullptr;
    }
    if (!res->map_err.empty() && s_map_win.visible &&
        s_active_tab == res->mode) {
        SetMapStatus(res->map_err.c_str(), kMapStatusError);
    } else if (s_map_win.visible && s_active_tab == res->mode &&
               tab.location_loading == false) {
        SetMapStatus("", kMapStatusHidden);
    }

    delete res;
}

// ---------------------------------------------------------------------------
// AT-command response parsers (kept verbatim — they only run if the
// Nt26Board stub is replaced with a real AT-capable modem shim).
// ---------------------------------------------------------------------------
struct WifiApEntry {
    std::string ssid;
    int         rssi   = 0;
    std::string mac;
    int         channel = 0;
};

bool ParseWifiScanLine(const char *line, WifiApEntry &out) {
    const char *scan = FindCaseInsensitive(line, "+ECWIFISCAN:");
    if (scan == nullptr) {
        return false;
    }
    const char *p = std::strchr(scan, '(');
    if (p == nullptr) {
        return false;
    }
    ++p;

    std::string t0;
    std::string ssid_tok;
    std::string rssi_str;
    std::string mac;
    std::string channel_str;
    if (!ReadParenCsvToken(p, t0) || !ReadParenCsvToken(p, ssid_tok) ||
        !ReadParenCsvToken(p, rssi_str) || !ReadParenCsvToken(p, mac) ||
        !ReadParenCsvToken(p, channel_str)) {
        return false;
    }
    if (mac.empty() || rssi_str.empty()) {
        return false;
    }

    out.ssid = (ssid_tok == "-" || ssid_tok.empty()) ? "" : ssid_tok;
    out.rssi = std::atoi(rssi_str.c_str());
    out.mac  = mac;
    out.channel = std::atoi(channel_str.c_str());
    return true;
}

bool AtRespHasWifiScan(const std::string &resp) {
    return FindCaseInsensitive(resp.c_str(), "+ECWIFISCAN:") != nullptr;
}

bool AtRespHasCellInfo(const std::string &resp) {
    return FindCaseInsensitive(resp.c_str(), "+ECBCINFOSC:") != nullptr ||
           FindCaseInsensitive(resp.c_str(), "+ECBCINFO:") != nullptr;
}

bool BuildWifiLocateBody(const std::string &at_resp, const MapViewParams &view,
                         std::string &body_out, std::string &err_out) {
    std::vector<WifiApEntry> aps;
    size_t start = 0;
    while (start < at_resp.size()) {
        const size_t end = at_resp.find_first_of("\r\n", start);
        const size_t line_end = (end == std::string::npos) ? at_resp.size() : end;
        std::string line = at_resp.substr(start, line_end - start);
        WifiApEntry ap;
        if (ParseWifiScanLine(line.c_str(), ap)) {
            aps.push_back(ap);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
        while (start < at_resp.size() &&
               (at_resp[start] == '\r' || at_resp[start] == '\n')) {
            ++start;
        }
    }
    if (aps.empty()) {
        err_out = I18n::T("WiFi 扫描无结果");
        ESP_LOGW(TAG, "wifi scan parse failed, AT resp:\n%s", at_resp.c_str());
        return false;
    }

    std::sort(aps.begin(), aps.end(),
              [](const WifiApEntry &a, const WifiApEntry &b) {
                  return a.rssi > b.rssi;
              });

    std::string macs;
    for (const auto &ap : aps) {
        std::string mac = ap.mac;
        ToLowerInPlace(mac);
        char part[96];
        std::snprintf(part, sizeof(part), "%s,%d,|", mac.c_str(), ap.rssi);
        macs += part;
    }

    const WifiApEntry &main_ap = aps.front();
    std::string main_mac = main_ap.mac;
    ToLowerInPlace(main_mac);
    char mmac[160];
    std::snprintf(mmac, sizeof(mmac), "%s,%d,%s,0", main_mac.c_str(),
                  main_ap.rssi,
                  main_ap.ssid.empty() ? "" : main_ap.ssid.c_str());

    const std::string device_id = SystemInfo::GetMacAddress();
    const std::string imei      = GetDeviceImei();
    const std::string ctime     = CurrentUnixTimeStr();
    char body[2048];
    std::snprintf(body, sizeof(body),
                  "{\"deviceId\":\"%s\",\"imei\":\"%s\",\"accesstype\":1,"
                  "\"ctime\":\"%s\",\"macs\":\"%s\",\"mmac\":\"%s\","
                  "\"coor\":\"GCJ02\",\"needRgc\":\"Y\",\"zoom\":%d,"
                  "\"width\":%d,\"height\":%d}",
                  device_id.c_str(), imei.c_str(), ctime.c_str(),
                  macs.c_str(), mmac, view.zoom, view.width, view.height);
    body_out = body;
    return true;
}

bool BuildCellLocateBody(const std::string &at_resp, const MapViewParams &view,
                         std::string &body_out, std::string &err_out) {
    const char *line = FindCaseInsensitive(at_resp.c_str(), "+ECBCINFOSC:");
    if (line == nullptr) {
        line = FindCaseInsensitive(at_resp.c_str(), "+ECBCINFO:");
    }
    if (line == nullptr) {
        err_out = I18n::T("未收到基站信息");
        ESP_LOGW(TAG, "cell info missing, AT resp:\n%s", at_resp.c_str());
        return false;
    }

    const char *p = std::strchr(line, ':');
    if (p == nullptr) {
        err_out = I18n::T("基站信息格式错误");
        return false;
    }
    ++p;

    std::vector<std::string> tokens;
    tokens.reserve(8);
    for (int i = 0; i < 8; ++i) {
        std::string tok;
        if (!ReadParenCsvToken(p, tok)) {
            err_out = I18n::T("基站信息解析失败");
            ESP_LOGW(TAG, "cell parse token %d failed, line: %s", i, line);
            return false;
        }
        tokens.push_back(tok);
    }

    const int         rssi    = std::atoi(tokens[2].c_str());
    const std::string &mcc    = tokens[4];
    const std::string &mnc    = tokens[5];
    const std::string &ci     = tokens[6];
    const std::string &tac    = tokens[7];
    const unsigned long lac_dec = ParseHexU32(tac.c_str());
    const unsigned long ci_dec  = ParseHexU32(ci.c_str());
    const int mnc_num = std::atoi(mnc.c_str());

    char bts[96];
    std::snprintf(bts, sizeof(bts), "%s,%d,%lu,%lu,%d", mcc.c_str(), mnc_num,
                  lac_dec, ci_dec, rssi);

    const std::string device_id = SystemInfo::GetMacAddress();
    const std::string imei      = GetDeviceImei();
    const std::string ctime     = CurrentUnixTimeStr();
    char body[512];
    std::snprintf(body, sizeof(body),
                  "{\"deviceId\":\"%s\",\"imei\":\"%s\",\"accesstype\":0,"
                  "\"ctime\":\"%s\",\"bts\":\"%s\",\"network\":\"LTE\","
                  "\"cdma\":0,\"coor\":\"GCJ02\",\"needRgc\":\"Y\","
                  "\"zoom\":%d,\"width\":%d,\"height\":%d}",
                  device_id.c_str(), imei.c_str(), ctime.c_str(), bts,
                  view.zoom, view.width, view.height);
    body_out = body;
    return true;
}

int EstimateGpsRadiusM(const GpsService::Snapshot &snap) {
    if (snap.hdop > 0.0) {
        int r = static_cast<int>(snap.hdop * 15.0);
        if (r < 10) r = 10;
        if (r > 500) r = 500;
        return r;
    }
    return 30;
}

int EstimateRadiusFromAcc(double acc_m) {
    if (acc_m > 0.0) {
        int r = static_cast<int>(acc_m);
        if (r < 10) r = 10;
        if (r > 500) r = 500;
        return r;
    }
    return 30;
}

bool TabHasCachedCoords(const TabLocState &tab) {
    return tab.has_locate_result && !GpsIsNan(tab.result_lat) &&
           !GpsIsNan(tab.result_lon);
}

std::string BuildStaticMapUrl(double lat_deg, double lon_deg,
                              const MapViewParams &view) {
    char url[512];
    std::snprintf(url, sizeof(url),
                  "%s%s?latitude=%.6f&longitude=%.6f&coordtype=gcj02&width=%d&height=%d&zoom=%d",
                  api::kHost, api::kGpsStaticMap, lat_deg, lon_deg, view.width,
                  view.height, view.zoom);
    return url;
}

bool BuildGpsLocateBody(double lon_deg, double lat_deg, int radius_m,
                        const MapViewParams &view, std::string &body_out,
                        std::string &err_out) {
    if (GpsIsNan(lon_deg) || GpsIsNan(lat_deg)) {
        err_out = I18n::T("经纬度无效");
        return false;
    }
    char gps_field[64];
    std::snprintf(gps_field, sizeof(gps_field), "%.4f|%.4f|%d", lon_deg,
                  lat_deg, radius_m);

    const std::string device_id = SystemInfo::GetMacAddress();
    const std::string imei      = GetDeviceImei();
    const std::string ctime     = CurrentUnixTimeStr();
    char body[512];
    std::snprintf(body, sizeof(body),
                  "{\"deviceId\":\"%s\",\"imei\":\"%s\",\"accesstype\":2,"
                  "\"ctime\":\"%s\",\"gps\":\"%s\",\"coor\":\"GCJ02\","
                  "\"needRgc\":\"Y\",\"zoom\":%d,\"width\":%d,\"height\":%d}",
                  device_id.c_str(), imei.c_str(), ctime.c_str(), gps_field,
                  view.zoom, view.width, view.height);
    body_out = body;
    return true;
}

// ---------------------------------------------------------------------------
// SubmitLocationReport — stubbed. The original POSTs req_json to
// /location/report/cell, parses the JSON, then downloads the static-map PNG.
// openvela does not yet expose an HTTP client; we always go through the
// failure path. UI shows "查询失败：HTTP 不可用" / "未返回定位数据".
// ---------------------------------------------------------------------------
void SubmitLocationReport(LocationReportResult *res,
                          const std::string &req_json) {
    // No HTTP client available — record the failure and ship the result
    // back to the LVGL thread via lv_async_call. The original flow (HTTP
    // POST → JSON parse → PNG download) is preserved below as comments for
    // whoever wires up the real network stack.
    (void)req_json;
    res->err = I18n::T("HTTP 不可用");
    LogLocateHttpJson(res->mode, req_json, 0, "(no http client)");
    lv_async_call(OnLocationReportDone, res);
    vTaskDelete(nullptr);
}

void MapZoomRefreshTask(void *arg) {
    auto *res = static_cast<LocationReportResult *>(arg);
    auto &tab = TabState(res->mode);
    if (!TabHasCachedCoords(tab)) {
        res->err = I18n::T("无缓存坐标");
        lv_async_call(OnLocationReportDone, res);
        vTaskDelete(nullptr);
        return;
    }

    const MapViewParams view = GetMapViewParams();
    const std::string map_url =
        BuildStaticMapUrl(tab.result_lat, tab.result_lon, view);
    ESP_LOGI(TAG, "locate[%s] map zoom GET (stub)", LocModeName(res->mode));

    if (!DownloadMapForResult(nullptr, map_url, res)) {
        if (res->err.empty()) {
            res->err = res->map_err.empty() ? I18n::T("地图下载失败") : res->map_err;
        }
        lv_async_call(OnLocationReportDone, res);
        vTaskDelete(nullptr);
        return;
    }

    res->ok        = true;
    res->latitude  = tab.result_lat;
    res->longitude = tab.result_lon;
    res->accuracy  = tab.result_acc;
    lv_async_call(OnLocationReportDone, res);
    vTaskDelete(nullptr);
}

void GpsLocationReportTask(void *arg) {
    auto *res = static_cast<LocationReportResult *>(arg);
    res->mode = LocMode::kGps;

    const GpsService::Snapshot snap = GetEffectiveSnapshot();
    if (!snap.fix_valid) {
        res->err = I18n::T("尚未定位");
        lv_async_call(OnLocationReportDone, res);
        vTaskDelete(nullptr);
        return;
    }

    std::string body;
    std::string build_err;
    const MapViewParams view = GetMapViewParams();
    if (!BuildGpsLocateBody(snap.longitude_deg, snap.latitude_deg,
                            EstimateGpsRadiusM(snap), view, body, build_err)) {
        res->err = build_err.empty() ? I18n::T("组装请求失败") : build_err;
        ESP_LOGI(TAG, "locate[gps] request json=(build failed: %s)",
                 res->err.c_str());
        lv_async_call(OnLocationReportDone, res);
        vTaskDelete(nullptr);
        return;
    }

    SubmitLocationReport(res, body);
}

void NetLocationReportTask(void *arg) {
    auto *res = static_cast<LocationReportResult *>(arg);

    // Nt26Board 4G 模块未端口化 — 直接走失败路径。
    auto &tab = TabState(res->mode);
    const char *at_cmd =
        (res->mode == LocMode::kWifi) ? "AT+ECWIFISCAN" : "AT+ECBCINFO";
    ESP_LOGI(TAG, "AT %s (stub: no 4G modem)", at_cmd);

    res->err = I18n::T("未检测到 4G 模块");
    tab.cached_at_resp.clear();
    lv_async_call(OnLocationReportDone, res);
    vTaskDelete(nullptr);
}

void StartLocationReport(LocMode mode, bool zoom_refresh_only = false) {
    auto &tab = TabState(mode);
    if (tab.location_loading) {
        return;
    }
    if (zoom_refresh_only) {
        if (!TabHasCachedCoords(tab)) {
            return;
        }
    } else if (mode == LocMode::kGps) {
        const GpsService::Snapshot snap = GetEffectiveSnapshot();
        if (!snap.fix_valid) {
            return;
        }
    }

    tab.location_loading = true;
    lv_obj_t *addr_lbl = AddressLabelForMode(mode);
    if (!zoom_refresh_only) {
        SetFetchEnabled(mode, false);
        if (addr_lbl != nullptr) {
            lv_label_set_text(addr_lbl, I18n::T("查询中..."));
        }
        if (mode != LocMode::kGps && tab.status_label != nullptr) {
            lv_label_set_text(tab.status_label, I18n::T("定位中"));
        }
    }
    if (s_map_win.visible && s_active_tab == mode) {
        SetMapStatus(I18n::T("正在加载地图..."), kMapStatusLoading);
    }

    auto *res = new LocationReportResult{};
    res->mode = mode;
    res->zoom_refresh_only = zoom_refresh_only;
    res->session =
        s_location_session.fetch_add(1, std::memory_order_acq_rel) + 1;

    using TaskFn = void (*)(void *);
    TaskFn fn = nullptr;
    const char *name = nullptr;
    if (zoom_refresh_only) {
        fn   = MapZoomRefreshTask;
        name = "map_zoom_rpt";
    } else if (mode == LocMode::kGps) {
        fn   = GpsLocationReportTask;
        name = "gps_loc_rpt";
    } else {
        fn   = NetLocationReportTask;
        name = "net_loc_rpt";
    }

    TaskHandle_t handle = 0;
    if (xTaskCreate(fn, name, 12288, res, 4, &handle) != pdPASS) {
        delete res;
        tab.location_loading = false;
        SetFetchEnabled(mode, true);
        if (addr_lbl != nullptr) {
            lv_label_set_text(addr_lbl, I18n::T("任务创建失败"));
        }
    }
}

void OnFetchClicked(lv_event_t *e) {
    const auto mode = static_cast<LocMode>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    StartLocationReport(mode);
}

void SyncZoomDropdown(int z) {
    if (s_map_win.map_zoom_dd == nullptr) {
        return;
    }
    lv_dropdown_set_selected(s_map_win.map_zoom_dd,
                             static_cast<uint32_t>(z - kMinZoom));
}

void UpdateZoomStepButtons() {
    const int z = s_zoom_level.load(std::memory_order_relaxed);
    if (s_map_win.map_zoom_out_btn != nullptr) {
        if (z <= kMinZoom) {
            lv_obj_add_state(s_map_win.map_zoom_out_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_map_win.map_zoom_out_btn, LV_STATE_DISABLED);
        }
    }
    if (s_map_win.map_zoom_in_btn != nullptr) {
        if (z >= kMaxZoom) {
            lv_obj_add_state(s_map_win.map_zoom_in_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_map_win.map_zoom_in_btn, LV_STATE_DISABLED);
        }
    }
}

void StartMapZoomRefresh(LocMode mode) {
    auto &tab = TabState(mode);
    if (tab.location_loading) {
        return;
    }
    if (!TabHasCachedCoords(tab)) {
        if (s_map_win.visible && s_active_tab == mode) {
            SetMapStatus(I18n::T("暂无缓存坐标"), kMapStatusError);
        }
        return;
    }
    StartLocationReport(mode, true);
}

void ApplyMapZoom(int z, bool sync_dropdown) {
    if (z < kMinZoom || z > kMaxZoom) {
        return;
    }
    s_zoom_level.store(z, std::memory_order_relaxed);
    SaveZoom(z);
    if (sync_dropdown) {
        SyncZoomDropdown(z);
    }
    UpdateZoomStepButtons();
    auto &tab = ActiveTabState();
    if (tab.location_loading) {
        return;
    }
    StartMapZoomRefresh(s_active_tab);
}

void OnMapZoomChanged(lv_event_t *e) {
    auto *dd = lv_event_get_target_obj(e);
    if (dd == nullptr) return;
    const uint32_t sel = lv_dropdown_get_selected(dd);
    const int z = static_cast<int>(sel) + kMinZoom;
    ApplyMapZoom(z, false);
}

void OnMapZoomOutClicked(lv_event_t * /*e*/) {
    ApplyMapZoom(s_zoom_level.load(std::memory_order_relaxed) - 1, true);
}

void OnMapZoomInClicked(lv_event_t * /*e*/) {
    ApplyMapZoom(s_zoom_level.load(std::memory_order_relaxed) + 1, true);
}

void OnMapZoomDdListOpened(lv_event_t *e) {
    auto *dd = lv_event_get_target_obj(e);
    if (dd == nullptr) return;
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list == nullptr) return;
    lv_obj_set_style_bg_color(list, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_color(list, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 10, LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x3B82F6),
                              LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER,
                            LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_max_height(list, 360, LV_PART_MAIN);
    screen_swipe_back_ignore(list, true);
}

void ShowMapWindow() {
    if (s_map_win.overlay == nullptr) {
        return;
    }
    auto &tab = ActiveTabState();
    if (!tab.has_map) {
        return;
    }
    s_map_win.visible = true;
    lv_obj_remove_flag(s_map_win.overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_map_win.map_image != nullptr) {
        lv_obj_remove_flag(s_map_win.map_image, LV_OBJ_FLAG_HIDDEN);
    }
    SetMapStatus("", kMapStatusHidden);
    screen_swipe_back_ignore(s_map_win.overlay, true);
}

void CloseMapWindow() {
    if (s_map_win.overlay == nullptr) {
        return;
    }
    s_map_win.visible = false;
    lv_obj_add_flag(s_map_win.overlay, LV_OBJ_FLAG_HIDDEN);
    screen_swipe_back_ignore(s_map_win.overlay, false);
}

void OnMapEntryClicked(lv_event_t *e) {
    const auto mode = static_cast<LocMode>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    s_active_tab = mode;
    ShowMapWindow();
}

void OnMapBackClicked(lv_event_t * /*e*/) { CloseMapWindow(); }

void OnTabChanged(lv_event_t *e) {
    auto *tv = lv_event_get_target_obj(e);
    if (tv == nullptr) {
        return;
    }
    s_active_tab = static_cast<LocMode>(lv_tabview_get_tab_active(tv));
}

void StyleDarkKeyboard(lv_obj_t *kb) {
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x12151C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x2A2F3A), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
}

// ---------------------------------------------------------------------------
// 「模拟定位」弹框
// ---------------------------------------------------------------------------
void CloseMockDialog() {
    if (s_state.mock_dlg.mask != nullptr) {
        lv_obj_delete(s_state.mock_dlg.mask);
    }
    s_state.mock_dlg = MockDialogUi{};
}

bool ParseMockInputs(double &lat, double &lon, std::string &err) {
    if (s_state.mock_dlg.lat_ta == nullptr ||
        s_state.mock_dlg.lon_ta == nullptr) {
        err = I18n::T("弹框已销毁");
        return false;
    }
    const char *lat_text = lv_textarea_get_text(s_state.mock_dlg.lat_ta);
    const char *lon_text = lv_textarea_get_text(s_state.mock_dlg.lon_ta);
    if (!ParseDoubleSafe(lat_text, lat)) {
        err = I18n::T("纬度格式有误");
        return false;
    }
    if (!ParseDoubleSafe(lon_text, lon)) {
        err = I18n::T("经度格式有误");
        return false;
    }
    if (lat < -90.0 || lat > 90.0) {
        err = I18n::T("纬度需在 -90 ~ 90 之间");
        return false;
    }
    if (lon < -180.0 || lon > 180.0) {
        err = I18n::T("经度需在 -180 ~ 180 之间");
        return false;
    }
    return true;
}

void OnMockCancelClicked(lv_event_t * /*e*/) { CloseMockDialog(); }

void OnMockSaveClicked(lv_event_t * /*e*/) {
    double lat = 0.0;
    double lon = 0.0;
    std::string err;
    if (!ParseMockInputs(lat, lon, err)) {
        if (s_state.mock_dlg.status_lbl != nullptr) {
            lv_label_set_text(s_state.mock_dlg.status_lbl, err.c_str());
            lv_obj_set_style_text_color(s_state.mock_dlg.status_lbl,
                                        lv_color_hex(0xF87171), LV_PART_MAIN);
        }
        return;
    }
    SaveMockSettings(true, lat, lon);
    ESP_LOGI(TAG, "mock location enabled: lat=%.6f, lon=%.6f", lat, lon);
    CloseMockDialog();
    SetMapStatus(I18n::T("模拟定位已开启，可点击获取定位"), kMapStatusInfo);
    RefreshFromService();
}

void OnMockDisableClicked(lv_event_t * /*e*/) {
    double lat = kMockDefaultLat;
    double lon = kMockDefaultLon;
    std::string err;
    ParseMockInputs(lat, lon, err);
    double cur_lat;
    double cur_lon;
    {
        std::lock_guard<std::mutex> g(s_mock_lock);
        cur_lat = s_mock_lat_deg;
        cur_lon = s_mock_lon_deg;
    }
    if (!err.empty()) {
        lat = cur_lat;
        lon = cur_lon;
    }
    SaveMockSettings(false, lat, lon);
    ESP_LOGI(TAG, "mock location disabled");
    CloseMockDialog();
    SetMapStatus(I18n::T("模拟定位已关闭"), kMapStatusInfo);
    RefreshFromService();
}

void OnMockKeyboardEvent(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        OnMockSaveClicked(nullptr);
    } else if (code == LV_EVENT_CANCEL) {
        CloseMockDialog();
    }
}

void OnMockTaFocused(lv_event_t *e) {
    auto *ta = lv_event_get_target_obj(e);
    if (s_state.mock_dlg.keyboard != nullptr) {
        lv_keyboard_set_textarea(s_state.mock_dlg.keyboard, ta);
    }
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
}

void OpenMockDialog() {
    if (s_state.screen == nullptr) {
        return;
    }
    CloseMockDialog();

    lv_obj_t *mask = lv_obj_create(s_state.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    s_state.mock_dlg.mask = mask;

    constexpr int32_t kCardW = kPanelSize - 60;
    constexpr int32_t kCardH = 380;
    lv_obj_t *card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_set_pos(card, 30, 24);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    s_state.mock_dlg.card = card;

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("模拟定位"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *status = lv_label_create(card);
    {
        const bool en = MockIsEnabled();
        char       buf[96];
        std::snprintf(buf, sizeof(buf),
                      I18n::T("当前：%s（修改后点「保存并启用」生效）"),
                      en ? I18n::T("已开启") : I18n::T("已关闭"));
        lv_label_set_text(status, buf);
    }
    lv_obj_set_style_text_color(status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_TOP_LEFT, 0, 46);
    s_state.mock_dlg.status_lbl = status;

    double init_lat;
    double init_lon;
    {
        std::lock_guard<std::mutex> g(s_mock_lock);
        init_lat = s_mock_lat_deg;
        init_lon = s_mock_lon_deg;
    }

    char init_buf[32];

    lv_obj_t *lat_lbl = lv_label_create(card);
    lv_label_set_text(lat_lbl, I18n::T("纬度"));
    lv_obj_set_style_text_color(lat_lbl, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(lat_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(lat_lbl, LV_ALIGN_TOP_LEFT, 0, 88);

    lv_obj_t *lat_ta = lv_textarea_create(card);
    s_state.mock_dlg.lat_ta = lat_ta;
    lv_obj_set_size(lat_ta, kCardW - 36 - 88, 56);
    lv_obj_align(lat_ta, LV_ALIGN_TOP_LEFT, 88, 80);
    lv_textarea_set_one_line(lat_ta, true);
    lv_textarea_set_max_length(lat_ta, 16);
    lv_textarea_set_placeholder_text(lat_ta, I18n::T("如 39.908823"));
    std::snprintf(init_buf, sizeof(init_buf), "%.6f", init_lat);
    lv_textarea_set_text(lat_ta, init_buf);
    lv_obj_set_style_text_font(lat_ta, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lat_ta, lv_color_hex(0x12151C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lat_ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(lat_ta, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_radius(lat_ta, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(lat_ta, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(lat_ta, lv_color_hex(0x2A2F3A),
                                  LV_PART_MAIN);
    lv_obj_add_state(lat_ta, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(lat_ta, OnMockTaFocused, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(lat_ta, OnMockTaFocused, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lon_lbl = lv_label_create(card);
    lv_label_set_text(lon_lbl, I18n::T("经度"));
    lv_obj_set_style_text_color(lon_lbl, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(lon_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(lon_lbl, LV_ALIGN_TOP_LEFT, 0, 160);

    lv_obj_t *lon_ta = lv_textarea_create(card);
    s_state.mock_dlg.lon_ta = lon_ta;
    lv_obj_set_size(lon_ta, kCardW - 36 - 88, 56);
    lv_obj_align(lon_ta, LV_ALIGN_TOP_LEFT, 88, 152);
    lv_textarea_set_one_line(lon_ta, true);
    lv_textarea_set_max_length(lon_ta, 16);
    lv_textarea_set_placeholder_text(lon_ta, I18n::T("如 116.397470"));
    std::snprintf(init_buf, sizeof(init_buf), "%.6f", init_lon);
    lv_textarea_set_text(lon_ta, init_buf);
    lv_obj_set_style_text_font(lon_ta, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lon_ta, lv_color_hex(0x12151C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lon_ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(lon_ta, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_radius(lon_ta, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(lon_ta, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(lon_ta, lv_color_hex(0x2A2F3A),
                                  LV_PART_MAIN);
    lv_obj_add_event_cb(lon_ta, OnMockTaFocused, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(lon_ta, OnMockTaFocused, LV_EVENT_CLICKED, nullptr);

    constexpr int32_t kBtnW    = 196;
    constexpr int32_t kBtnH    = 56;
    constexpr int32_t kBtnGap  = 12;

    auto make_btn = [&](const char *text, uint32_t bg_hex, lv_event_cb_t cb,
                        int32_t x_offset) {
        lv_obj_t *btn = lv_button_create(card);
        lv_obj_set_size(btn, kBtnW, kBtnH);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, x_offset, 0);
        lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(lbl);
    };
    make_btn(I18n::T("关闭模拟"),   0x4B5563, OnMockDisableClicked, 0);
    make_btn(I18n::T("取消"),       0x2A2F3A, OnMockCancelClicked,  kBtnW + kBtnGap);
    make_btn(I18n::T("保存并启用"), 0x7C3AED, OnMockSaveClicked,
             (kBtnW + kBtnGap) * 2);

    constexpr int32_t kKbH = kPanelSize - kCardH - 24 - 12;
    lv_obj_t *kb = lv_keyboard_create(mask);
    s_state.mock_dlg.keyboard = kb;
    lv_obj_set_size(kb, kPanelSize, kKbH);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    StyleDarkKeyboard(kb);
    lv_keyboard_set_textarea(kb, lat_ta);
    lv_obj_add_event_cb(kb, OnMockKeyboardEvent, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, OnMockKeyboardEvent, LV_EVENT_CANCEL, nullptr);

    screen_swipe_back_ignore(mask, true);
}

void OnMapMockClicked(lv_event_t * /*e*/) { OpenMockDialog(); }

// ---------------------------------------------------------------------------
// Screen lifecycle
// ---------------------------------------------------------------------------
void GoBackToHome() {
    if (s_map_win.visible) {
        CloseMapWindow();
        return;
    }
    if (s_state.mock_dlg.mask != nullptr) {
        CloseMockDialog();
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    HomeScreen::SwitchToHome();
}

void OnBackClicked(lv_event_t * /*e*/) { GoBackToHome(); }

void OnSwipeBack() { GoBackToHome(); }

void OnScreenUnloaded(lv_event_t * /*e*/) {
    if (s_state.refresh_timer != nullptr) {
        lv_timer_delete(s_state.refresh_timer);
        s_state.refresh_timer = nullptr;
    }
    s_location_session.fetch_add(1, std::memory_order_acq_rel);

    s_state.screen        = nullptr;
    s_state.tabview       = nullptr;
    s_state.refresh_timer = nullptr;
    s_state.mock_dlg      = MockDialogUi{};
    s_map_win             = MapWindowState{};
    for (int i = 0; i < static_cast<int>(LocMode::kCount); ++i) {
        s_tab_state[i] = TabLocState{};
    }
}

void BuildCardHeader(lv_obj_t *card, LocMode mode, const char *title_text) {
    auto &tab = TabState(mode);
    lv_obj_t *header = lv_obj_create(card);
    StripChrome(header);
    lv_obj_set_size(header, LV_PCT(100), kTitleHeight);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = MakeLabel(header, title_text, &font_puhui_30_4,
                                lv_color_hex(kColorAccent),
                                LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(title, 1);

    lv_obj_t *badge = lv_obj_create(header);
    StripChrome(badge);
    lv_obj_set_height(badge, 40);
    lv_obj_set_width(badge, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(badge, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(badge, lv_color_hex(kColorBadgeSearch),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(badge, 20, LV_PART_MAIN);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    screen_make_input_passive(badge);

    const char *init_status =
        (mode == LocMode::kGps) ? I18n::T("搜星中") : I18n::T("未定位");
    lv_obj_t *badge_text = MakeLabel(badge, init_status, &font_puhui_20_4,
                                     lv_color_hex(kColorText),
                                     LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(badge_text, LV_SIZE_CONTENT);
    lv_label_set_long_mode(badge_text, LV_LABEL_LONG_CLIP);
    tab.status_badge = badge;
    tab.status_label = badge_text;
    MakeDivider(card);
}

constexpr int32_t kFetchBtnW = 200;
constexpr int32_t kZoomDdW   = 112;

lv_obj_t *MakeActionBtn(lv_obj_t *parent, const char *text, uint32_t bg_hex,
                        lv_event_cb_t on_click, void *user_data,
                        int32_t width) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, kMapToolbarH - 8);
    if (width == LV_SIZE_CONTENT) {
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(btn, 14, LV_PART_MAIN);
    } else {
        lv_obj_set_width(btn, width);
    }
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

void BuildMapEntryRow(lv_obj_t *parent, LocMode mode) {
    auto &tab = TabState(mode);
    lv_obj_t *row = lv_button_create(parent);
    tab.map_entry = row;
    lv_obj_set_size(row, kCardWidth, 72);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(row, OnMapEntryClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(mode)));
    lv_obj_add_state(row, LV_STATE_DISABLED);

    lv_obj_t *left = lv_label_create(row);
    lv_label_set_text(left, I18n::T("地图"));
    lv_obj_set_style_text_color(left, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(left, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 24, 0);

    lv_obj_t *right = lv_label_create(row);
    tab.map_entry_sub = right;
    lv_label_set_text(right, I18n::T("请先获取定位"));
    lv_obj_set_style_text_color(right, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(right, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -24, 0);
}

void BuildGpsTabContent(lv_obj_t *parent) {
    auto &tab = TabState(LocMode::kGps);
    lv_obj_t *scroll = lv_obj_create(parent);
    screen_strip_obj_chrome(scroll);
    lv_obj_set_size(scroll, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_top(scroll, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(scroll, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scroll, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);

    lv_obj_t *outdoor_hint = MakeLabel(
        scroll, I18n::T("请到室外空旷位置，室内无法 GPS 定位"),
        &font_puhui_20_4, lv_color_hex(kColorSubtle), LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(outdoor_hint, kCardWidth);
    lv_label_set_long_mode(outdoor_hint, LV_LABEL_LONG_WRAP);

    lv_obj_t *card = lv_obj_create(scroll);
    StripChrome(card);
    lv_obj_set_size(card, kCardWidth, kCardHeight);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, kCardRadius, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, kCardPadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_top(card, kCardPadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(card, kCardPadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    BuildCardHeader(card, LocMode::kGps, I18n::T("卫星定位"));
    tab.value_labels[kRowLat]   = MakeInfoRow(card, I18n::T("纬度"), true);
    tab.value_labels[kRowLon]   = MakeInfoRow(card, I18n::T("经度"), true);
    tab.value_labels[kRowAlt]   = MakeInfoRow(card, I18n::T("海拔"), true);
    tab.value_labels[kRowSats]  = MakeInfoRow(card, I18n::T("卫星 (使用/可见)"), true);
    tab.value_labels[kRowHdop]  = MakeInfoRow(card, "HDOP", true);
    tab.value_labels[kRowSpeed] = MakeInfoRow(card, I18n::T("速度"), true);
    tab.value_labels[kRowAddress] = MakeInfoRow(card, I18n::T("地址"), false);
    lv_obj_set_style_text_font(tab.value_labels[kRowAddress],
                               &font_puhui_20_4, LV_PART_MAIN);
    screen_make_input_passive(card);

    lv_obj_t *action_row = lv_obj_create(scroll);
    StripChrome(action_row);
    lv_obj_set_size(action_row, kCardWidth, kMapToolbarH);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(action_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    tab.fetch_btn = MakeActionBtn(
        action_row, I18n::T("获取定位"), 0x1F8A4C, OnFetchClicked,
        reinterpret_cast<void *>(static_cast<uintptr_t>(LocMode::kGps)),
        kFetchBtnW);
    SetFetchEnabled(LocMode::kGps, false);
    MakeActionBtn(action_row, I18n::T("模拟"), 0x7C3AED, OnMapMockClicked, nullptr,
                  kFetchBtnW);

    BuildMapEntryRow(scroll, LocMode::kGps);
}

void BuildNetTabContent(lv_obj_t *parent, LocMode mode, const char *title) {
    auto &tab = TabState(mode);
    lv_obj_t *scroll = lv_obj_create(parent);
    screen_strip_obj_chrome(scroll);
    lv_obj_set_size(scroll, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_top(scroll, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(scroll, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scroll, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);

    lv_obj_t *card = lv_obj_create(scroll);
    StripChrome(card);
    lv_obj_set_size(card, kCardWidth, kCardHeightNet);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, kCardRadius, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, kCardPadHor, LV_PART_MAIN);
    lv_obj_set_style_pad_top(card, kCardPadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(card, kCardPadVer, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    BuildCardHeader(card, mode, title);
    tab.net_labels[kNetRowLat] = MakeInfoRow(card, I18n::T("纬度"), true);
    tab.net_labels[kNetRowLon] = MakeInfoRow(card, I18n::T("经度"), true);
    tab.net_labels[kNetRowAcc] = MakeInfoRow(card, I18n::T("精度"), true);
    tab.net_labels[kNetRowAddress] = MakeInfoRow(card, I18n::T("地址"), false);
    lv_obj_set_style_text_font(tab.net_labels[kNetRowAddress],
                               &font_puhui_20_4, LV_PART_MAIN);
    screen_make_input_passive(card);

    tab.fetch_btn = MakeActionBtn(
        scroll, I18n::T("获取定位"), 0x1F8A4C, OnFetchClicked,
        reinterpret_cast<void *>(static_cast<uintptr_t>(mode)), kCardWidth);
    SetFetchEnabled(mode, true);

    BuildMapEntryRow(scroll, mode);
}

void BuildMapWindow(lv_obj_t *scr) {
    lv_obj_t *overlay = lv_obj_create(scr);
    s_map_win.overlay = overlay;
    screen_strip_obj_chrome(overlay);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(overlay, kPanelSize, kPanelSize);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *header = lv_obj_create(overlay);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t *back_btn = lv_button_create(header);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, OnMapBackClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t *map_title = lv_label_create(header);
    lv_label_set_text(map_title, I18n::T("地图"));
    lv_obj_set_style_text_color(map_title, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(map_title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(map_title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    lv_obj_t *toolbar = lv_obj_create(overlay);
    StripChrome(toolbar);
    lv_obj_set_size(toolbar, kCardWidth, kMapToolbarH);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, kHeaderH + 8);
    lv_obj_set_style_bg_opa(toolbar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(toolbar, 8, LV_PART_MAIN);

    lv_obj_t *dd = lv_dropdown_create(toolbar);
    s_map_win.map_zoom_dd = dd;
    std::string opts;
    opts.reserve(220);
    for (int i = kMinZoom; i <= kMaxZoom; ++i) {
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), I18n::T("缩放 %d"), i);
        if (!opts.empty()) opts += '\n';
        opts += tmp;
    }
    lv_dropdown_set_options(dd, opts.c_str());
    const int z = s_zoom_level.load(std::memory_order_relaxed);
    lv_dropdown_set_selected(dd, static_cast<uint32_t>(z - kMinZoom));
    lv_obj_set_size(dd, kZoomDdW, kMapToolbarH - 8);
    lv_obj_set_style_radius(dd, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dd, lv_color_hex(0x4B5563), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_add_event_cb(dd, OnMapZoomChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(dd, OnMapZoomDdListOpened, LV_EVENT_READY, nullptr);

    s_map_win.map_zoom_out_btn =
        MakeActionBtn(toolbar, I18n::T("缩小"), 0x4B5563, OnMapZoomOutClicked, nullptr,
                      LV_SIZE_CONTENT);
    s_map_win.map_zoom_in_btn =
        MakeActionBtn(toolbar, I18n::T("放大"), 0x3B82F6, OnMapZoomInClicked, nullptr,
                      LV_SIZE_CONTENT);
    UpdateZoomStepButtons();

    const int32_t stage_y = kHeaderH + kMapToolbarH + 20;
    const int32_t stage_w = CalcMapStagePixelSize();

    lv_obj_t *stage = lv_obj_create(overlay);
    StripChrome(stage);
    UpdateMapStageMetrics(stage, stage_w);
    lv_obj_align(stage, LV_ALIGN_TOP_MID, 0, stage_y);
    lv_obj_set_style_bg_color(stage, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(stage, 16, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(stage, true, LV_PART_MAIN);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_image_create(stage);
    lv_obj_center(img);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    s_map_win.map_image = img;

    lv_obj_t *scrim = lv_obj_create(stage);
    StripChrome(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
    screen_make_input_passive(scrim);
    s_map_win.map_loading_scrim = scrim;

    lv_obj_t *status = lv_label_create(stage);
    lv_obj_set_style_text_font(status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(status, LV_PCT(90));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_center(status);
    s_map_win.map_status_lbl = status;
}

void BuildTabView(lv_obj_t *scr) {
    const int32_t content_h = kPanelSize - kHeaderH;
    lv_obj_t *tv = lv_tabview_create(scr);
    s_state.tabview = tv;
    lv_obj_set_size(tv, kPanelSize, content_h);
    lv_obj_set_pos(tv, 0, kHeaderH);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, kTabBarH);
    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorAccent),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER,
                            LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t *content = lv_tabview_get_content(tv);
    screen_swipe_back_ignore(content, true);
    lv_obj_add_event_cb(tv, OnTabChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *tab_gps = lv_tabview_add_tab(tv, I18n::T("GPS定位"));
    BuildGpsTabContent(tab_gps);

    lv_obj_t *tab_wifi = lv_tabview_add_tab(tv, I18n::T("WiFi定位"));
    BuildNetTabContent(tab_wifi, LocMode::kWifi, I18n::T("WiFi 定位"));

    lv_obj_t *tab_cell = lv_tabview_add_tab(tv, I18n::T("基站定位"));
    BuildNetTabContent(tab_cell, LocMode::kCell, I18n::T("基站定位"));
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
lv_obj_t *GpsScreen::CreateStatic() {
    // Kick the GPS service the moment the user enters this screen.
    GpsService::Start();
    s_zoom_level.store(LoadZoom(), std::memory_order_relaxed);
    LoadMockSettings();

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_state.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);

    lv_obj_t *header = lv_obj_create(scr);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_button_create(header);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, OnBackClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("定位"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 16, 0);

    BuildTabView(scr);
    BuildMapWindow(scr);

    s_state.refresh_timer = lv_timer_create(OnRefreshTimer,
                                            kRefreshPeriodMs, nullptr);
    RefreshFromService();

    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    return scr;
}

lv_obj_t *GpsScreen::Create() {
    root_ = CreateStatic();
    return root_;
}

// ---------------------------------------------------------------------------
// 生命周期：进入屏幕拉高 GPS_POWER，离开拉低。
// openvela 上 IOExpander 尚未端口化，这里仅打 log；驱动落地后把
// io.setLevel(...) 替换为真实调用即可。
// ---------------------------------------------------------------------------
void GpsScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: gps_screen (GPS_POWER on — stubbed)");
        // TODO(openvela): io.setLevel(IOExpander::Pin::GPS_POWER, true);
    } else {
        ESP_LOGI(TAG, "unload: gps_screen (GPS_POWER off — stubbed)");
        // TODO(openvela): io.setLevel(IOExpander::Pin::GPS_POWER, false);
    }
}
