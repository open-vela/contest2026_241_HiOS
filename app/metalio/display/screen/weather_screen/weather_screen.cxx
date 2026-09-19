/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * WeatherScreen — ported 1:1 from MetalioClaw4 weather_screen.cc
 */

#include "weather_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"
#include "weather_city_list.h"
#include "weather_icon_map.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace weather_detail {

// --- Weather data structures + offline demo service (from Weather.hpp) ---
struct WeatherForecastDay {
    std::string date;
    std::string week;
    std::string text_day;
    std::string text_night;
    int32_t high = 0;
    int32_t low = 0;
    std::string wc_day;
    std::string wd_day;
    std::string wc_night;
    std::string wd_night;
    int32_t wind_angle_day = 0;
    int32_t wind_angle_night = 0;
    int32_t uvi = 0;
    int32_t pressure = 0;
    int32_t dpt = 0;
};

struct WeatherForecastHour {
    std::string data_time;
    std::string text;
    int32_t temp_fc = 0;
    std::string wind_class;
    std::string wind_dir;
    int32_t rh = 0;
    float prec1h = 0.f;
    int32_t clouds = 0;
    int32_t wind_angle = 0;
    int32_t pop = 0;
    int32_t uvi = 0;
    int32_t pressure = 0;
    int32_t dpt = 0;
};

struct WeatherLifeIndex {
    std::string name;
    std::string brief;
    std::string detail;
};

struct WeatherAlert {
    std::string title;
    std::string content;
};

struct WeatherDistrictData {
    std::string country;
    std::string province;
    std::string city;
    std::string district;
    std::string district_id;
    std::string text;
    int32_t temp = 0;
    int32_t feels_like = 0;
    int32_t rh = 0;
    std::string wind_class;
    std::string wind_dir;
    int32_t wind_angle = 0;
    float prec1h = 0.f;
    int32_t clouds = 0;
    int32_t vis = 0;
    int32_t aqi = 0;
    int32_t pm25 = 0;
    int32_t pm10 = 0;
    int32_t no2 = 0;
    int32_t so2 = 0;
    int32_t o3 = 0;
    float co = 0.f;
    int32_t uvi = 0;
    int32_t pressure = 0;
    int32_t dpt = 0;
    std::string uptime;
    std::vector<WeatherForecastDay> forecasts;
    std::vector<WeatherForecastHour> forecast_hours;
    std::vector<WeatherLifeIndex> indexes;
    std::vector<WeatherAlert> alerts;
    bool valid = false;
};

class WeatherService {
public:
    static constexpr const char* kDefaultDistrictId = "440306";
    static WeatherService& Instance() {
        static WeatherService instance;
        return instance;
    }
    void SetDistrictId(const std::string& id) {
        if (!id.empty()) district_id_ = id;
    }
    const std::string& DistrictId() const { return district_id_; }
    int Fetch(WeatherDistrictData& out) {
        out = MakeDemoData(district_id_);
        cached_ = out;
        return 0;
    }
    const WeatherDistrictData& Cached() const { return cached_; }
private:
    WeatherService() : district_id_(kDefaultDistrictId) {}
    static WeatherDistrictData MakeDemoData(const std::string& district_id) {
        WeatherDistrictData d;
        d.valid = true;
        d.country = "中国";
        const std::string id =
            district_id.empty() ? kDefaultDistrictId : district_id;
        d.district_id = id;

        /* Bind UI city line to the selected district — hardcoded 深圳宝安
         * made every save look like weather never updated. */
        const weather_cities::LookupResult found =
            weather_cities::FindDistrictById(id.c_str());
        if (found.province_idx < weather_cities::kProvinceCount) {
            const weather_cities::ProvinceEntry& pe =
                weather_cities::kProvinces[found.province_idx];
            d.province = pe.name;
            const weather_cities::CityEntry& ce =
                weather_cities::kCities[pe.city_start + found.city_idx];
            d.city = ce.name;
            const weather_cities::DistrictEntry* de =
                weather_cities::GetDistrict(found.province_idx, found.city_idx,
                                            found.district_idx);
            d.district = (de != nullptr) ? de->name : "";
        } else {
            d.province = "广东省";
            d.city = "深圳市";
            d.district = "宝安区";
            d.district_id = kDefaultDistrictId;
        }

        unsigned hash = 0;
        for (char c : id) {
            hash = hash * 33u + static_cast<unsigned char>(c);
        }
        d.text = (hash % 3u == 0) ? "晴" : ((hash % 3u == 1) ? "多云" : "阴");
        d.temp = static_cast<int32_t>(18 + (hash % 15u));
        d.feels_like = d.temp + static_cast<int32_t>(hash % 3u);
        d.rh = static_cast<int32_t>(55 + (hash % 30u));
        d.wind_class = "3级";
        d.wind_dir = "东南风";
        d.wind_angle = static_cast<int32_t>(90 + (hash % 180u));
        d.prec1h = 0.f;
        d.clouds = static_cast<int32_t>(30 + (hash % 50u));
        d.vis = 12000;
        d.aqi = static_cast<int32_t>(30 + (hash % 60u));
        d.pm25 = static_cast<int32_t>(10 + (hash % 40u));
        d.pm10 = static_cast<int32_t>(20 + (hash % 50u));
        d.no2 = 12;
        d.so2 = 6;
        d.o3 = 48;
        d.co = 0.6f;
        d.uvi = static_cast<int32_t>(3 + (hash % 6u));
        d.pressure = 1012;
        d.dpt = 19;
        d.uptime = "20260913120000";
        const char* weeks[] = {"周一","周二","周三","周四","周五","周六","周日"};
        const char* day_texts[] = {"晴","多云","阴","小雨","多云","晴","阵雨"};
        for (int i = 0; i < 7; ++i) {
            WeatherForecastDay day;
            day.date = "2026-09-" + std::to_string(13 + i);
            day.week = weeks[i % 7];
            day.text_day = day_texts[(i + static_cast<int>(hash)) % 7];
            day.text_night = (i % 2) ? "多云" : "晴";
            day.high = d.temp + 2 + (i % 3);
            day.low = d.temp - 4 + (i % 2);
            day.wc_day = "3级";
            day.wd_day = "东南风";
            day.wc_night = "2级";
            day.wd_night = "东风";
            day.uvi = 4 + (i % 4);
            day.pressure = 1010 + i;
            day.dpt = 18 + (i % 3);
            d.forecasts.push_back(std::move(day));
        }
        for (int h = 0; h < 24; ++h) {
            WeatherForecastHour hour;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "2026-09-13 %02d:00:00", h);
            hour.data_time = buf;
            hour.text = (h < 6 || h > 18) ? "多云" : "晴";
            hour.temp_fc = static_cast<int32_t>(d.temp - 2 + (h % 6));
            hour.wind_class = "3级";
            hour.wind_dir = "东南风";
            hour.rh = static_cast<int32_t>(60 + (h % 10));
            hour.prec1h = (h % 7 == 0) ? 0.2f : 0.f;
            hour.clouds = 40 + (h % 20);
            hour.wind_angle = 120;
            hour.pop = h % 5 * 10;
            hour.uvi = (h >= 10 && h <= 16) ? 6 : 2;
            hour.pressure = 1012;
            hour.dpt = 19;
            d.forecast_hours.push_back(std::move(hour));
        }
        WeatherLifeIndex idx1;
        idx1.name = "穿衣";
        idx1.brief = "舒适";
        idx1.detail = "建议穿短袖或薄长袖。";
        WeatherLifeIndex idx2;
        idx2.name = "紫外线";
        idx2.brief = "中等";
        idx2.detail = "外出请做好防晒。";
        WeatherLifeIndex idx3;
        idx3.name = "运动";
        idx3.brief = "适宜";
        idx3.detail = "天气不错，适合户外运动。";
        d.indexes.push_back(idx1);
        d.indexes.push_back(idx2);
        d.indexes.push_back(idx3);
        return d;
    }
    std::string district_id_;
    WeatherDistrictData cached_;
};



constexpr int32_t kPanelW = 720;
constexpr int32_t kHeaderH  = 88;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kHeaderSidePad = 16;
constexpr int32_t kRegionBtnW = 72;
constexpr int32_t kRefreshBtnW = 72;
constexpr int32_t kHeaderBtnH = 56;
constexpr int32_t kHeaderBtnGap = 8;
constexpr int32_t kModalCardW = 560;
constexpr int32_t kModalCardPad = 24;
constexpr int32_t kModalDdW = kModalCardW - kModalCardPad * 2;
constexpr int32_t kModalDdH = 44;
constexpr int32_t kTabBarH = 52;
constexpr int32_t kTabPad  = 14;
constexpr int32_t kInnerW = kPanelW - kTabPad * 2;

// 资源原生 128×128；6 日预报（跳过今天）3 列网格
constexpr int32_t kHeroIconSize      = 128;
constexpr int32_t kForecastIconSize  = 128;
constexpr int32_t kForecastCols        = 3;
constexpr int32_t kForecastDayCount    = 6;
constexpr int32_t kForecastGridPadH  = 4;
constexpr int32_t kForecastColGap    = 10;
constexpr int32_t kForecastRowGap    = 10;
constexpr int32_t kForecastCardPad   = 6;
constexpr int32_t kForecastUsableW =
    kInnerW - kForecastGridPadH * 2;
constexpr int32_t kForecastCardW =
    (kForecastUsableW - kForecastColGap * (kForecastCols - 1)) / kForecastCols;
constexpr int32_t kForecastCardH = 244;

constexpr uint32_t kColorBg         = 0x0E1116;
constexpr uint32_t kColorBgGrad     = 0x161A22;
constexpr uint32_t kColorCard       = 0x2A2F3A;
constexpr uint32_t kColorCardAccent = 0xE0FB3C;
constexpr uint32_t kColorText       = 0xFFFFFF;
constexpr uint32_t kColorSubtle     = 0x9AA3B2;
constexpr uint32_t kColorAccent     = 0x60A5FA;
constexpr uint32_t kColorTabActive  = 0x3B82F6;
constexpr uint32_t kColorHeaderBg   = 0x12151C;
constexpr uint32_t kColorDivider    = 0x2A2F3A;
constexpr uint32_t kColorHeaderBtn  = 0x2A2F3A;
constexpr uint32_t kColorHeaderBtnBorder = 0x3B4556;

lv_obj_t* s_screen       = nullptr;
lv_obj_t* s_tabview      = nullptr;
lv_obj_t* s_tab_overview  = nullptr;
lv_obj_t* s_tab_forecast  = nullptr;
lv_obj_t* s_tab_hours     = nullptr;
lv_obj_t* s_tab_index     = nullptr;
lv_obj_t* s_status_lbl   = nullptr;
lv_obj_t* s_prov_dd      = nullptr;
lv_obj_t* s_city_dd      = nullptr;
lv_obj_t* s_dist_dd      = nullptr;
lv_obj_t* s_city_dlg_mask = nullptr;
lv_obj_t* s_reveal_cover  = nullptr;
std::string s_prov_options;
std::string s_city_options;
std::string s_dist_options;
bool s_dd_syncing        = false;
std::atomic<uint32_t> s_session{0};
lv_timer_t *s_save_timer  = nullptr;
bool s_mounted = false;
bool s_overview_dirty = false;
bool s_forecast_dirty = false;
bool s_hours_dirty = false;
bool s_index_dirty = false;
bool s_save_busy = false;
bool s_hide_busy = false;
bool s_hide_full_overview = false;
/* Soft-hide + overview rebuild must not share the Save CLICKED stack. */
constexpr uint32_t kSaveApplyDelayMs = 900;

const lv_font_t* font_main() { return &font_puhui_30_4; }
const lv_font_t* font_sub()  { return &font_puhui_20_4; }

void TriggerFetch();
void ShowStatus(const char *text);
void SoftHideCityPicker();
void DestroyCityPickerHard();
void CloseDropdownLists();
void EnsureRevealCover();
void SoftHideRevealCover();
void ArmSafeCityPickerHide(bool after_save);
void ApplyOverviewOnly(const WeatherDistrictData& data);
void ApplyOverviewLite(const WeatherDistrictData& data);
void ClearContainer(lv_obj_t* container);
void ClearTabMessage(lv_obj_t* tab, const char* msg);

void StyleDropdown(lv_obj_t* dd) {
    lv_obj_set_size(dd, kModalDdW, kModalDdH);
    lv_obj_set_style_radius(dd, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dd, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, lv_color_hex(0x4B5563), LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_pad_left(dd, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(dd, 16, LV_PART_MAIN);
    /* LV_SYMBOL_DOWN is not in font_puhui — renders as □ / 乱码.
     * Pass nullptr (not "") so the indicator is not drawn at all. */
    lv_dropdown_set_symbol(dd, nullptr);
    screen_swipe_back_ignore(dd, true);
}

void StyleHeaderBtn(lv_obj_t* btn) {
    lv_obj_set_style_radius(btn, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorHeaderBtn), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorHeaderBtnBorder),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3B4556),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    screen_swipe_back_ignore(btn, true);
}

void OnDropdownListOpened(lv_event_t* e) {
    lv_obj_t* dd = lv_event_get_target_obj(e);
    if (dd == nullptr) {
        return;
    }
    lv_obj_t* list = lv_dropdown_get_list(dd);
    if (list == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(list, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_radius(list, 10, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 320, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorTabActive),
                              static_cast<lv_part_t>(LV_PART_SELECTED) |
                                  static_cast<lv_state_t>(LV_STATE_CHECKED));
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER,
                            static_cast<lv_part_t>(LV_PART_SELECTED) |
                                static_cast<lv_state_t>(LV_STATE_CHECKED));
    screen_swipe_back_ignore(list, true);
}

void AppendDropdownOption(std::string& buf, const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    if (!buf.empty()) {
        buf += '\n';
    }
    buf += name;
}

void FillProvinceOptions() {
    if (s_prov_dd == nullptr) {
        return;
    }
    s_prov_options.clear();
    for (size_t i = 0; i < weather_cities::kProvinceCount; ++i) {
        AppendDropdownOption(s_prov_options, weather_cities::kProvinces[i].name);
    }
    lv_dropdown_set_options(s_prov_dd, s_prov_options.c_str());
}

void FillCityOptions(size_t province_idx) {
    if (s_city_dd == nullptr || province_idx >= weather_cities::kProvinceCount) {
        return;
    }
    const weather_cities::ProvinceEntry& pe =
        weather_cities::kProvinces[province_idx];
    s_city_options.clear();
    for (uint16_t i = 0; i < pe.city_count; ++i) {
        AppendDropdownOption(s_city_options,
                             weather_cities::kCities[pe.city_start + i].name);
    }
    lv_dropdown_set_options(s_city_dd, s_city_options.c_str());
}

void FillDistrictOptions(size_t province_idx, size_t city_idx) {
    if (s_dist_dd == nullptr || province_idx >= weather_cities::kProvinceCount) {
        return;
    }
    const weather_cities::ProvinceEntry& pe =
        weather_cities::kProvinces[province_idx];
    if (city_idx >= pe.city_count) {
        return;
    }
    const weather_cities::CityEntry& ce =
        weather_cities::kCities[pe.city_start + city_idx];
    s_dist_options.clear();
    for (uint16_t i = 0; i < ce.district_count; ++i) {
        AppendDropdownOption(s_dist_options,
                             weather_cities::kDistricts[ce.district_start + i].name);
    }
    lv_dropdown_set_options(s_dist_dd, s_dist_options.c_str());
}

std::string LoadSavedDistrictId() {
    /* Persist for the process via WeatherService singleton — remount must
     * not snap back to the default Shenzhen id. */
    return WeatherService::Instance().DistrictId();
}

void SaveDistrictId(const char* district_id) {
    if (district_id == nullptr || district_id[0] == '\0') {
        return;
    }
    WeatherService::Instance().SetDistrictId(district_id);
}

const char* SelectedDistrictId() {
    if (s_prov_dd == nullptr || s_city_dd == nullptr || s_dist_dd == nullptr) {
        return WeatherService::kDefaultDistrictId;
    }
    const size_t prov = lv_dropdown_get_selected(s_prov_dd);
    const size_t city = lv_dropdown_get_selected(s_city_dd);
    const size_t dist = lv_dropdown_get_selected(s_dist_dd);
    const weather_cities::DistrictEntry* entry =
        weather_cities::GetDistrict(prov, city, dist);
    if (entry == nullptr) {
        return WeatherService::kDefaultDistrictId;
    }
    return entry->id;
}

void SoftHideCityPicker()
{
    if (s_city_dlg_mask == nullptr) {
        return;
    }
    lv_obj_add_flag(s_city_dlg_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_city_dlg_mask, LV_OBJ_FLAG_CLICKABLE);
}

void SoftHideRevealCover()
{
    if (s_reveal_cover == nullptr) {
        return;
    }
    lv_obj_add_flag(s_reveal_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_reveal_cover, LV_OBJ_FLAG_CLICKABLE);
}

void CloseDropdownLists()
{
    auto close_one = [](lv_obj_t *dd) {
        if (dd != nullptr) {
            lv_dropdown_close(dd);
        }
    };
    close_one(s_prov_dd);
    close_one(s_city_dd);
    close_one(s_dist_dd);
}

void EnsureRevealCover()
{
    if (s_screen == nullptr) {
        return;
    }
    if (s_reveal_cover != nullptr) {
        lv_obj_set_style_bg_color(s_reveal_cover, lv_color_hex(kColorBg),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_reveal_cover, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(s_reveal_cover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_reveal_cover, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(s_reveal_cover);
        return;
    }
    lv_obj_t *cover = lv_obj_create(s_screen);
    s_reveal_cover = cover;
    screen_strip_obj_chrome(cover);
    lv_obj_add_flag(cover, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(cover, kPanelW, kPanelW);
    lv_obj_set_pos(cover, 0, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(cover, true);
}

void ArmSafeCityPickerHide(bool after_save)
{
    if (s_hide_busy || s_screen == nullptr) {
        if (after_save) {
            s_save_busy = false;
        }
        return;
    }
    s_hide_busy = true;
    s_hide_full_overview = after_save;

    /* Serial: WX_HIDE_OK completed while panel stayed solid blue — SoftHide
     * of the translucent mask forced a full weather-tree repaint. Cover the
     * screen opaque first, rebuild under it, then peel. */
    write(1, "WX_COVER\n", 9);
    CloseDropdownLists();
    EnsureRevealCover();
    SoftHideCityPicker();

    lv_timer_t *strip = lv_timer_create(
        [](lv_timer_t *t) {
            lv_timer_delete(t);
            if (s_screen == nullptr) {
                s_hide_busy = false;
                s_save_busy = false;
                return;
            }

            if (s_hide_full_overview) {
                write(1, "WX_STRIP\n", 9);
                ClearContainer(s_tab_overview);
                ClearContainer(s_tab_forecast);
                ClearContainer(s_tab_hours);
                ClearContainer(s_tab_index);
                s_forecast_dirty = true;
                s_hours_dirty = true;
                s_index_dirty = true;

                write(1, "WX_FULL\n", 8);
                const auto &cached = WeatherService::Instance().Cached();
                if (cached.valid) {
                    ApplyOverviewOnly(cached);
                } else {
                    ClearTabMessage(s_tab_overview, I18n::T("暂无天气数据"));
                    s_overview_dirty = true;
                }
                write(1, "WX_FULL_OK\n", 11);
            }

            /* Give full overview one tick under the cover before peel. */
            lv_timer_t *peel = lv_timer_create(
                [](lv_timer_t *pt) {
                    lv_timer_delete(pt);
                    write(1, "WX_HIDE\n", 8);
                    SoftHideRevealCover();
                    write(1, "WX_HIDE_OK\n", 11);
                    s_hide_busy = false;
                    s_save_busy = false;
                    s_hide_full_overview = false;
                },
                s_hide_full_overview ? 250 : 200, nullptr);
            if (peel != nullptr) {
                lv_timer_set_repeat_count(peel, 1);
            } else {
                SoftHideRevealCover();
                s_hide_busy = false;
                s_save_busy = false;
                s_hide_full_overview = false;
            }
        },
        200, nullptr);
    if (strip != nullptr) {
        lv_timer_set_repeat_count(strip, 1);
    } else {
        SoftHideRevealCover();
        s_hide_busy = false;
        s_save_busy = false;
        s_hide_full_overview = false;
    }
}

void DestroyCityPickerHard()
{
    if (s_save_timer != nullptr) {
        lv_timer_delete(s_save_timer);
        s_save_timer = nullptr;
    }
    if (s_reveal_cover != nullptr) {
        lv_obj_t *cover = s_reveal_cover;
        s_reveal_cover = nullptr;
        lv_obj_delete(cover);
    }
    if (s_city_dlg_mask == nullptr) {
        s_prov_dd = nullptr;
        s_city_dd = nullptr;
        s_dist_dd = nullptr;
        return;
    }
    lv_obj_t *mask = s_city_dlg_mask;
    s_city_dlg_mask = nullptr;
    s_prov_dd = nullptr;
    s_city_dd = nullptr;
    s_dist_dd = nullptr;
    lv_obj_delete(mask);
}

void CloseCityPickerDialog() {
    /* Opaque cover → strip tabs → peel — plain SoftHide blues. */
    ArmSafeCityPickerHide(false);
}

void OnCityPickerSaveClicked(lv_event_t* /*e*/) {
    if (s_save_busy || s_hide_busy || s_screen == nullptr) {
        return;
    }
    s_save_busy = true;

    const char* district_id = SelectedDistrictId();
    char pending[32];
    pending[0] = '\0';
    if (district_id != nullptr && district_id[0] != '\0') {
        std::strncpy(pending, district_id, sizeof(pending) - 1);
        pending[sizeof(pending) - 1] = '\0';
    }
    SaveDistrictId(pending[0] != '\0' ? pending
                                      : WeatherService::kDefaultDistrictId);

    /* Click stack: id + status only. Soft-hide / ClearContainer after save
     * completed markers then still solid-blued the next flush. */
    ShowStatus(I18n::T("已切换"));
    write(1, "WX_SAVE\n", 8);
    CloseDropdownLists();

    if (s_save_timer != nullptr) {
        lv_timer_delete(s_save_timer);
        s_save_timer = nullptr;
    }
    s_save_timer = lv_timer_create(
        [](lv_timer_t *t) {
            s_save_timer = nullptr;
            lv_timer_delete(t);
            if (s_screen == nullptr) {
                s_save_busy = false;
                return;
            }

            write(1, "WX_FETCH\n", 9);
            WeatherDistrictData data;
            WeatherService::Instance().Fetch(data);

            write(1, "WX_APPLY\n", 9);
            if (data.valid) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s %s  %" PRId32 "°",
                              data.city.c_str(), data.district.c_str(),
                              data.temp);
                ShowStatus(buf);
            } else {
                ShowStatus(I18n::T("加载失败"));
            }
            s_overview_dirty = true;
            s_forecast_dirty = true;
            s_hours_dirty = true;
            s_index_dirty = true;
            write(1, "WX_APPLY_OK\n", 12);

            ArmSafeCityPickerHide(true);
        },
        kSaveApplyDelayMs, nullptr);
    if (s_save_timer != nullptr) {
        lv_timer_set_repeat_count(s_save_timer, 1);
    } else {
        s_save_busy = false;
    }
}

void SyncDropdownsFromDistrictId(const char* district_id) {
    if (s_prov_dd == nullptr || s_city_dd == nullptr || s_dist_dd == nullptr) {
        return;
    }
    const weather_cities::LookupResult found =
        weather_cities::FindDistrictById(district_id);
    size_t prov = found.province_idx;
    size_t city = found.city_idx;
    size_t dist = found.district_idx;
    if (prov >= weather_cities::kProvinceCount) {
        const weather_cities::LookupResult fallback =
            weather_cities::FindDistrictById(WeatherService::kDefaultDistrictId);
        prov = fallback.province_idx;
        city = fallback.city_idx;
        dist = fallback.district_idx;
    }

    s_dd_syncing = true;
    FillProvinceOptions();
    lv_dropdown_set_selected(s_prov_dd, static_cast<uint32_t>(prov));
    FillCityOptions(prov);
    lv_dropdown_set_selected(s_city_dd, static_cast<uint32_t>(city));
    FillDistrictOptions(prov, city);
    lv_dropdown_set_selected(s_dist_dd, static_cast<uint32_t>(dist));
    s_dd_syncing = false;
}

void OnProvinceChanged(lv_event_t* /*e*/) {
    if (s_dd_syncing || s_prov_dd == nullptr) {
        return;
    }
    const size_t prov = lv_dropdown_get_selected(s_prov_dd);
    s_dd_syncing = true;
    FillCityOptions(prov);
    lv_dropdown_set_selected(s_city_dd, 0);
    FillDistrictOptions(prov, 0);
    lv_dropdown_set_selected(s_dist_dd, 0);
    s_dd_syncing = false;
}

void OnCityChanged(lv_event_t* /*e*/) {
    if (s_dd_syncing || s_prov_dd == nullptr || s_city_dd == nullptr) {
        return;
    }
    const size_t prov = lv_dropdown_get_selected(s_prov_dd);
    const size_t city = lv_dropdown_get_selected(s_city_dd);
    s_dd_syncing = true;
    FillDistrictOptions(prov, city);
    lv_dropdown_set_selected(s_dist_dd, 0);
    s_dd_syncing = false;
}

void OnDistrictChanged(lv_event_t* /*e*/) {}

void OnCityPickerCancelClicked(lv_event_t* /*e*/) { CloseCityPickerDialog(); }

void OnCityPickerMaskClicked(lv_event_t* e) {
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    CloseCityPickerDialog();
}

void OpenCityPickerDialog() {
    if (s_screen == nullptr || s_hide_busy) {
        return;
    }

    SoftHideRevealCover();

    /* Reuse soft-hidden dialog — delete+recreate on every open blues on save. */
    if (s_city_dlg_mask != nullptr) {
        SyncDropdownsFromDistrictId(
            WeatherService::Instance().DistrictId().c_str());
        lv_obj_remove_flag(s_city_dlg_mask, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_city_dlg_mask, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(s_city_dlg_mask);
        return;
    }

    constexpr int32_t kCardH = 480;
    constexpr int32_t kBtnW = 200;
    constexpr int32_t kBtnH = 56;
    constexpr int32_t kBtnRowH = 72;

    lv_obj_t* mask = lv_obj_create(s_screen);
    s_city_dlg_mask = mask;
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelW, kPanelW);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    lv_obj_add_event_cb(mask, OnCityPickerMaskClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kModalCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, kModalCardPad, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);

    lv_obj_t* dlg_title = lv_label_create(card);
    lv_label_set_text(dlg_title, I18n::T("选择地区"));
    lv_obj_set_style_text_color(dlg_title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(dlg_title, font_main(), LV_PART_MAIN);
    lv_obj_set_style_text_align(dlg_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(dlg_title, LV_PCT(100));
    lv_obj_set_style_pad_bottom(dlg_title, 8, LV_PART_MAIN);
    lv_obj_remove_flag(dlg_title, LV_OBJ_FLAG_CLICKABLE);

    auto make_picker_row = [&](const char* label, lv_obj_t** out_dd) {
        lv_obj_t* row = lv_obj_create(card);
        screen_strip_obj_chrome(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(row, 4, LV_PART_MAIN);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, font_sub(), LV_PART_MAIN);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        *out_dd = lv_dropdown_create(row);
        StyleDropdown(*out_dd);
        lv_obj_set_width(*out_dd, LV_PCT(100));
        lv_obj_add_event_cb(*out_dd, OnDropdownListOpened, LV_EVENT_READY, nullptr);
    };

    make_picker_row(I18n::T("省份"), &s_prov_dd);
    lv_obj_add_event_cb(s_prov_dd, OnProvinceChanged, LV_EVENT_VALUE_CHANGED,
                        nullptr);

    make_picker_row(I18n::T("城市"), &s_city_dd);
    lv_obj_add_event_cb(s_city_dd, OnCityChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    make_picker_row(I18n::T("区县"), &s_dist_dd);
    lv_obj_add_event_cb(s_dist_dd, OnDistrictChanged, LV_EVENT_VALUE_CHANGED,
                        nullptr);

    SyncDropdownsFromDistrictId(WeatherService::Instance().DistrictId().c_str());

    lv_obj_t* btn_row = lv_obj_create(card);
    screen_strip_obj_chrome(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, kBtnRowH);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(btn_row, 12, LV_PART_MAIN);

    lv_obj_t* cancel = lv_button_create(btn_row);
    lv_obj_set_size(cancel, kBtnW, kBtnH);
    StyleHeaderBtn(cancel);
    lv_obj_add_event_cb(cancel, OnCityPickerCancelClicked, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_t* cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, I18n::T("取消"));
    lv_obj_set_style_text_font(cancel_lbl, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(cancel_lbl);
    lv_obj_remove_flag(cancel_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* save = lv_button_create(btn_row);
    lv_obj_set_size(save, kBtnW, kBtnH);
    lv_obj_set_style_radius(save, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(save, lv_color_hex(kColorTabActive), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(save, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(save, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(save, true);
    lv_obj_add_event_cb(save, OnCityPickerSaveClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* save_lbl = lv_label_create(save);
    lv_label_set_text(save_lbl, I18n::T("保存"));
    lv_obj_set_style_text_font(save_lbl, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(save_lbl);
    lv_obj_remove_flag(save_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void OnCityPickerOpenClicked(lv_event_t* /*e*/) { OpenCityPickerDialog(); }

void SetWeatherIcon(lv_obj_t* icon, const std::string& text, int32_t display_size) {
    if (icon == nullptr) {
        return;
    }
    const char* code = WeatherIconCodeForText(text);
    if (code == nullptr) {
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char path[48];
    std::snprintf(path, sizeof(path), "A:ic_s_weather_%s.spng", code);
    lv_obj_set_size(icon, display_size, display_size);
    lv_image_set_src(icon, path);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                    uint32_t color, lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, align, LV_PART_MAIN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    screen_make_input_passive(lbl);
    return lbl;
}

lv_obj_t* MakeSectionTitle(lv_obj_t* parent, const char* title) {
    lv_obj_t* lbl = MakeLabel(parent, title, font_main(), kColorText);
    lv_obj_set_width(lbl, kInnerW);
    lv_obj_set_style_pad_top(lbl, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(lbl, 8, LV_PART_MAIN);
    return lbl;
}

void AppendDetailRow(lv_obj_t* grid, const char* key, const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "%s  %s", key, value);
    lv_obj_t* lbl = MakeLabel(grid, line, font_sub(), kColorSubtle);
    lv_obj_set_width(lbl, (kInnerW - 12) / 2);
}

void FormatUptime(const std::string& uptime, char* buf, size_t len) {
    if (uptime.size() >= 12) {
        std::snprintf(buf, len, "%s-%s %s:%s", uptime.substr(4, 2).c_str(),
                      uptime.substr(6, 2).c_str(), uptime.substr(8, 2).c_str(),
                      uptime.substr(10, 2).c_str());
        return;
    }
    std::snprintf(buf, len, "%s", uptime.c_str());
}

void FormatVis(int32_t vis_m, char* buf, size_t len) {
    if (vis_m >= 1000) {
        std::snprintf(buf, len, "%.1f km", vis_m / 1000.f);
    } else {
        std::snprintf(buf, len, "%" PRId32 " m", vis_m);
    }
}

void ClearContainer(lv_obj_t* container) {
    if (container == nullptr) {
        return;
    }
    while (lv_obj_get_child_count(container) > 0) {
        lv_obj_delete(lv_obj_get_child(container, 0));
    }
}

void StyleScrollTab(lv_obj_t* tab) {
    lv_obj_set_style_pad_all(tab, kTabPad, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
}

lv_obj_t* CreateForecastCard(lv_obj_t* parent, const WeatherForecastDay& day,
                             int index) {
    const bool accent = (index % 2 == 1);
    const uint32_t bg = accent ? kColorCardAccent : kColorCard;
    const uint32_t fg = accent ? 0x1A1A1A : kColorText;
    const uint32_t fg2 = accent ? 0x404040 : kColorSubtle;

    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, kForecastCardW, kForecastCardH);
    screen_strip_obj_chrome(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, kForecastCardPad, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);

    const int32_t text_w = kForecastCardW - kForecastCardPad * 2;

    lv_obj_t* week_lbl =
        MakeLabel(card, day.week.empty() ? "--" : I18n::T(day.week.c_str()),
                  font_sub(), fg, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(week_lbl, text_w);

    char date_buf[16];
    if (day.date.size() >= 10) {
        const int month = std::atoi(day.date.substr(5, 2).c_str());
        const int day_num = std::atoi(day.date.substr(8, 2).c_str());
        std::snprintf(date_buf, sizeof(date_buf), "%02d/%02d", month, day_num);
    } else {
        std::snprintf(date_buf, sizeof(date_buf), "--");
    }
    lv_obj_t* date_lbl = MakeLabel(card, date_buf, font_sub(), fg2, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(date_lbl, text_w);

    lv_obj_t* icon = lv_image_create(card);
    SetWeatherIcon(icon, day.text_day, kForecastIconSize);
    screen_make_input_passive(icon);

    char temp_buf[32];
    std::snprintf(temp_buf, sizeof(temp_buf), "%" PRId32 "~%" PRId32 "°", day.low,
                  day.high);
    lv_obj_t* temp_lbl = MakeLabel(card, temp_buf, font_sub(), fg, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(temp_lbl, text_w);

    lv_obj_t* day_lbl = MakeLabel(card, I18n::T(day.text_day.c_str()), font_sub(),
                                  fg2, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(day_lbl, text_w);
    lv_label_set_long_mode(day_lbl, LV_LABEL_LONG_WRAP);

    lv_obj_t* night_lbl =
        MakeLabel(card, I18n::T(day.text_night.c_str()), font_sub(), fg2,
                  LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(night_lbl, text_w);
    lv_label_set_long_mode(night_lbl, LV_LABEL_LONG_WRAP);

    screen_make_input_passive(card);
    return card;
}

lv_obj_t* CreateHourRow(lv_obj_t* parent, const WeatherForecastHour& hour) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);

    lv_obj_t* row1 = lv_obj_create(card);
    screen_strip_obj_chrome(row1);
    lv_obj_set_width(row1, LV_PCT(100));
    lv_obj_set_height(row1, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(row1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row1, 12, LV_PART_MAIN);

    std::string time_str = hour.data_time;
    if (time_str.size() >= 16) {
        time_str = time_str.substr(11, 5);
    }
    lv_obj_t* time_lbl = MakeLabel(row1, time_str.c_str(), font_sub(), kColorText);
    lv_obj_set_width(time_lbl, 56);

    char temp_buf[16];
    std::snprintf(temp_buf, sizeof(temp_buf), "%" PRId32 "°", hour.temp_fc);
    MakeLabel(row1, temp_buf, font_sub(), kColorAccent);

    MakeLabel(row1, I18n::T(hour.text.c_str()), font_sub(), kColorText);

    char detail[240];
    // 完整 format 必须是单一字符串；不能把 I18n::T() 与 PRId32 字面量拼接。
    std::snprintf(detail, sizeof(detail),
                  I18n::T("%s %s  湿度%d%%  降水%.1f  云量%d%%  降水概率%d%%  UV%d  %dhPa 露点%d°  风向角%d°"),
                  I18n::T(hour.wind_dir.c_str()), I18n::T(hour.wind_class.c_str()),
                  static_cast<int>(hour.rh), hour.prec1h,
                  static_cast<int>(hour.clouds), static_cast<int>(hour.pop),
                  static_cast<int>(hour.uvi), static_cast<int>(hour.pressure),
                  static_cast<int>(hour.dpt), static_cast<int>(hour.wind_angle));
    lv_obj_t* det = MakeLabel(card, detail, font_sub(), kColorSubtle);
    lv_obj_set_width(det, LV_PCT(100));

    screen_make_input_passive(card);
    return card;
}

lv_obj_t* CreateIndexCard(lv_obj_t* parent, const WeatherLifeIndex& idx) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, LV_PART_MAIN);

    char title[64];
    std::snprintf(title, sizeof(title), "%s · %s", idx.name.c_str(), idx.brief.c_str());
    MakeLabel(card, title, font_sub(), kColorText);

    if (!idx.detail.empty()) {
        lv_obj_t* det = MakeLabel(card, idx.detail.c_str(), font_sub(), kColorSubtle);
        lv_obj_set_width(det, LV_PCT(100));
    }
    screen_make_input_passive(card);
    return card;
}

void ShowStatus(const char* text) {
    if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, text);
    }
}

void BuildOverviewTab(const WeatherDistrictData& data) {
    if (s_tab_overview == nullptr) {
        return;
    }
    ClearContainer(s_tab_overview);

    lv_obj_t* hero = lv_obj_create(s_tab_overview);
    lv_obj_set_width(hero, LV_PCT(100));
    lv_obj_set_height(hero, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(hero);
    lv_obj_remove_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(hero, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 8, LV_PART_MAIN);

    lv_obj_t* icon = lv_image_create(hero);
    SetWeatherIcon(icon, data.text, kHeroIconSize);
    screen_make_input_passive(icon);

    lv_obj_t* hero_text = lv_obj_create(hero);
    screen_strip_obj_chrome(hero_text);
    lv_obj_set_size(hero_text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hero_text, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(hero_text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hero_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hero_text, 4, LV_PART_MAIN);

    char loc[128];
    std::snprintf(loc, sizeof(loc), "%s %s %s", data.province.c_str(),
                  data.city.c_str(), data.district.c_str());
    MakeLabel(hero_text, loc, font_main(), kColorText);

    char temp_line[48];
    std::snprintf(temp_line, sizeof(temp_line), "%" PRId32 "°  %s", data.temp,
                  I18n::T(data.text.c_str()));
    MakeLabel(hero_text, temp_line, font_main(), kColorAccent);

    char feel_line[48];
    std::snprintf(feel_line, sizeof(feel_line), I18n::T("体感 %d°  湿度 %d%%"),
                  static_cast<int>(data.feels_like), static_cast<int>(data.rh));
    MakeLabel(hero_text, feel_line, font_sub(), kColorSubtle);

    char wind_line[64];
    std::snprintf(wind_line, sizeof(wind_line), I18n::T("%s %s  风向角 %d°"),
                  I18n::T(data.wind_dir.c_str()),
                  I18n::T(data.wind_class.c_str()),
                  static_cast<int>(data.wind_angle));
    MakeLabel(hero_text, wind_line, font_sub(), kColorSubtle);

    screen_make_input_passive(hero);

    MakeSectionTitle(s_tab_overview, I18n::T("实况详情"));
    lv_obj_t* detail_grid = lv_obj_create(s_tab_overview);
    lv_obj_set_width(detail_grid, LV_PCT(100));
    lv_obj_set_height(detail_grid, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(detail_grid);
    lv_obj_remove_flag(detail_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(detail_grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(detail_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(detail_grid, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(detail_grid, 6, LV_PART_MAIN);

    char buf[64];
    FormatVis(data.vis, buf, sizeof(buf));
    AppendDetailRow(detail_grid, I18n::T("能见度"), buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 "%%", data.clouds);
    AppendDetailRow(detail_grid, I18n::T("云量"), buf);
    std::snprintf(buf, sizeof(buf), "%.1f mm/h", data.prec1h);
    AppendDetailRow(detail_grid, I18n::T("1h降水"), buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " hPa", data.pressure);
    AppendDetailRow(detail_grid, I18n::T("气压"), buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 "°", data.dpt);
    AppendDetailRow(detail_grid, I18n::T("露点"), buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32, data.uvi);
    AppendDetailRow(detail_grid, I18n::T("紫外线"), buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32, data.aqi);
    AppendDetailRow(detail_grid, "AQI", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " µg/m³", data.pm25);
    AppendDetailRow(detail_grid, "PM2.5", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " µg/m³", data.pm10);
    AppendDetailRow(detail_grid, "PM10", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " µg/m³", data.no2);
    AppendDetailRow(detail_grid, "NO₂", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " µg/m³", data.so2);
    AppendDetailRow(detail_grid, "SO₂", buf);
    std::snprintf(buf, sizeof(buf), "%" PRId32 " µg/m³", data.o3);
    AppendDetailRow(detail_grid, "O₃", buf);
    std::snprintf(buf, sizeof(buf), "%.1f mg/m³", data.co);
    AppendDetailRow(detail_grid, "CO", buf);
    FormatUptime(data.uptime, buf, sizeof(buf));
    AppendDetailRow(detail_grid, I18n::T("更新"), buf);
    std::snprintf(buf, sizeof(buf), "%s", data.district_id.c_str());
    AppendDetailRow(detail_grid, I18n::T("区划ID"), buf);
    std::snprintf(buf, sizeof(buf), "%s", data.country.c_str());
    AppendDetailRow(detail_grid, I18n::T("国家"), buf);
    screen_make_input_passive(detail_grid);

    if (!data.alerts.empty()) {
        MakeSectionTitle(s_tab_overview, I18n::T("预警"));
        for (const auto& alert : data.alerts) {
            lv_obj_t* card = lv_obj_create(s_tab_overview);
            lv_obj_set_width(card, LV_PCT(100));
            lv_obj_set_height(card, LV_SIZE_CONTENT);
            screen_strip_obj_chrome(card);
            lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(card, lv_color_hex(0x7F1D1D), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
            lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            MakeLabel(card, alert.title.c_str(), font_sub(), kColorText);
            if (!alert.content.empty()) {
                lv_obj_t* c = MakeLabel(card, alert.content.c_str(), font_sub(),
                                        kColorSubtle);
                lv_obj_set_width(c, LV_PCT(100));
            }
            screen_make_input_passive(card);
        }
    }

    lv_obj_set_style_pad_bottom(s_tab_overview, 24, LV_PART_MAIN);
}

void BuildForecastTab(const WeatherDistrictData& data) {
    if (s_tab_forecast == nullptr) {
        return;
    }
    ClearContainer(s_tab_forecast);

    if (data.forecasts.size() <= 1) {
        MakeLabel(s_tab_forecast, I18n::T("暂无6日预报"), font_main(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);
        return;
    }

    const size_t start = 1;
    const size_t end = std::min(
        data.forecasts.size(),
        start + static_cast<size_t>(kForecastDayCount));

    lv_obj_t* fc_grid = lv_obj_create(s_tab_forecast);
    lv_obj_set_width(fc_grid, LV_PCT(100));
    lv_obj_set_height(fc_grid, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(fc_grid);
    lv_obj_remove_flag(fc_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(fc_grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(fc_grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(fc_grid, kForecastRowGap, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(fc_grid, kForecastGridPadH, LV_PART_MAIN);

    for (int row = 0; row < 2; ++row) {
        lv_obj_t* fc_row = lv_obj_create(fc_grid);
        lv_obj_set_width(fc_row, LV_PCT(100));
        lv_obj_set_height(fc_row, kForecastCardH);
        screen_strip_obj_chrome(fc_row);
        lv_obj_set_style_bg_opa(fc_row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(fc_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(fc_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(fc_row, kForecastColGap, LV_PART_MAIN);
        lv_obj_set_flex_align(fc_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);

        for (int col = 0; col < kForecastCols; ++col) {
            const size_t idx = start + static_cast<size_t>(row * kForecastCols + col);
            if (idx >= end) {
                break;
            }
            CreateForecastCard(fc_row, data.forecasts[idx],
                               static_cast<int>(idx - start));
        }
    }
    screen_make_input_passive(fc_grid);
    lv_obj_set_style_pad_bottom(s_tab_forecast, 24, LV_PART_MAIN);
}

void BuildIndexTab(const WeatherDistrictData& data) {
    if (s_tab_index == nullptr) {
        return;
    }
    ClearContainer(s_tab_index);

    if (data.indexes.empty()) {
        MakeLabel(s_tab_index, I18n::T("暂无生活指数"), font_main(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);
        return;
    }

    lv_obj_t* idx_box = lv_obj_create(s_tab_index);
    lv_obj_set_width(idx_box, LV_PCT(100));
    lv_obj_set_height(idx_box, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(idx_box);
    lv_obj_remove_flag(idx_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(idx_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(idx_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(idx_box, 8, LV_PART_MAIN);
    for (const auto& idx : data.indexes) {
        CreateIndexCard(idx_box, idx);
    }
    screen_make_input_passive(idx_box);
    lv_obj_set_style_pad_bottom(s_tab_index, 24, LV_PART_MAIN);
}

void BuildHoursTab(const WeatherDistrictData& data) {
    if (s_tab_hours == nullptr) {
        return;
    }
    ClearContainer(s_tab_hours);

    if (data.forecast_hours.empty()) {
        MakeLabel(s_tab_hours, I18n::T("暂无24小时预报"), font_main(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);
        return;
    }

    MakeSectionTitle(s_tab_hours, I18n::T("24小时预报"));
    lv_obj_t* hour_box = lv_obj_create(s_tab_hours);
    lv_obj_set_width(hour_box, LV_PCT(100));
    lv_obj_set_height(hour_box, LV_SIZE_CONTENT);
    screen_strip_obj_chrome(hour_box);
    lv_obj_remove_flag(hour_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(hour_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(hour_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hour_box, 8, LV_PART_MAIN);
    for (const auto& hour : data.forecast_hours) {
        CreateHourRow(hour_box, hour);
    }
    screen_make_input_passive(hour_box);
    lv_obj_set_style_pad_bottom(s_tab_hours, 24, LV_PART_MAIN);
}

void BuildWeatherUi(const WeatherDistrictData& data) {
    BuildOverviewTab(data);
    BuildForecastTab(data);
    BuildHoursTab(data);
    BuildIndexTab(data);
}

void ClearTabMessage(lv_obj_t* tab, const char* msg) {
    if (tab == nullptr) {
        return;
    }
    ClearContainer(tab);
    if (msg != nullptr && msg[0] != '\0') {
        MakeLabel(tab, msg, font_main(), kColorSubtle, LV_TEXT_ALIGN_CENTER);
    }
}

void ApplyOverviewLite(const WeatherDistrictData& data) {
    if (s_tab_overview == nullptr) {
        return;
    }
    if (!data.valid) {
        ClearTabMessage(s_tab_overview, I18n::T("暂无天气数据"));
        ShowStatus(I18n::T("加载失败"));
        return;
    }
    /* Minimal overview after city save — no weather icon decode, no detail
     * grid. Full BuildOverviewTab when the user leaves/re-enters 今日. */
    ClearContainer(s_tab_overview);

    char loc[128];
    std::snprintf(loc, sizeof(loc), "%s %s %s", data.province.c_str(),
                  data.city.c_str(), data.district.c_str());
    MakeLabel(s_tab_overview, loc, font_main(), kColorText);

    char temp_line[48];
    std::snprintf(temp_line, sizeof(temp_line), "%" PRId32 "°  %s", data.temp,
                  I18n::T(data.text.c_str()));
    MakeLabel(s_tab_overview, temp_line, font_main(), kColorAccent);

    char feel_line[48];
    std::snprintf(feel_line, sizeof(feel_line), I18n::T("体感 %d°  湿度 %d%%"),
                  static_cast<int>(data.feels_like), static_cast<int>(data.rh));
    MakeLabel(s_tab_overview, feel_line, font_sub(), kColorSubtle);

    s_overview_dirty = true;
    s_forecast_dirty = true;
    s_hours_dirty = true;
    s_index_dirty = true;
    ShowStatus("");
}

void ApplyOverviewOnly(const WeatherDistrictData& data) {
    if (!data.valid) {
        ClearTabMessage(s_tab_overview, I18n::T("暂无天气数据"));
        ShowStatus(I18n::T("加载失败"));
        return;
    }
    BuildOverviewTab(data);
    s_overview_dirty = false;
    s_forecast_dirty = true;
    s_hours_dirty = true;
    s_index_dirty = true;
    ShowStatus("");
}

void ApplyWeatherData(const WeatherDistrictData& data) {
    ApplyOverviewOnly(data);
}

void ShowLoadingPlaceholder() {
    /* Status only — clearing four tabs during mount spikes heap. */
    ShowStatus(I18n::T("加载中..."));
}

struct FetchTimerCtx {
    uint32_t session;
};

void OnFetchTimer(lv_timer_t* t) {
    auto* ctx = static_cast<FetchTimerCtx*>(lv_timer_get_user_data(t));
    const uint32_t my_session = ctx ? ctx->session : 0;
    delete ctx;
    lv_timer_delete(t);

    WeatherDistrictData data;
    const int err = WeatherService::Instance().Fetch(data);

    if (s_session.load() == my_session && s_screen != nullptr) {
        if (err == 0) {
            ApplyOverviewOnly(data);
        } else {
            ApplyOverviewOnly(WeatherDistrictData());
            ShowStatus(I18n::T("加载失败"));
        }
    }
}

void TriggerFetch() {
    const uint32_t session =
        s_session.fetch_add(1, std::memory_order_relaxed) + 1;
    ShowStatus(I18n::T("加载中..."));
    ShowLoadingPlaceholder();

    auto* ctx = new FetchTimerCtx{session};
    lv_timer_t* tm = lv_timer_create(OnFetchTimer, 1, ctx);
    if (tm == nullptr) {
        delete ctx;
        WeatherDistrictData data;
        WeatherService::Instance().Fetch(data);
        ApplyOverviewOnly(data);
    } else {
        lv_timer_set_repeat_count(tm, 1);
    }
}

void OnTabChanged(lv_event_t* /*e*/) {
    if (s_tabview == nullptr || s_screen == nullptr) {
        return;
    }
    const auto& data = WeatherService::Instance().Cached();
    if (!data.valid) {
        return;
    }
    const uint32_t idx = lv_tabview_get_tab_active(s_tabview);
    if (idx == 0 && s_overview_dirty) {
        BuildOverviewTab(data);
        s_overview_dirty = false;
    } else if (idx == 1 && s_forecast_dirty) {
        BuildForecastTab(data);
        s_forecast_dirty = false;
    } else if (idx == 2 && s_hours_dirty) {
        BuildHoursTab(data);
        s_hours_dirty = false;
    } else if (idx == 3 && s_index_dirty) {
        BuildIndexTab(data);
        s_index_dirty = false;
    }
}

void OnSwipeBack();

void OnRefreshClicked(lv_event_t* /*e*/) { TriggerFetch(); }

void OnBackClicked(lv_event_t* /*e*/) { OnSwipeBack(); }

void BuildTabView(lv_obj_t* scr) {
    const int32_t body_h = kPanelW - kHeaderH;

    lv_obj_t* tv = lv_tabview_create(scr);
    s_tabview = tv;
    lv_obj_set_size(tv, kPanelW, body_h);
    lv_obj_set_pos(tv, 0, kHeaderH);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, kTabBarH);
    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorTabActive),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t* content = lv_tabview_get_content(tv);
    screen_swipe_back_ignore(content, true);

    s_tab_overview = lv_tabview_add_tab(tv, I18n::T("今日天气"));
    StyleScrollTab(s_tab_overview);

    s_tab_forecast = lv_tabview_add_tab(tv, I18n::T("6日天气"));
    StyleScrollTab(s_tab_forecast);

    s_tab_hours = lv_tabview_add_tab(tv, I18n::T("24时天气"));
    StyleScrollTab(s_tab_hours);

    s_tab_index = lv_tabview_add_tab(tv, I18n::T("生活指数"));
    StyleScrollTab(s_tab_index);

    lv_obj_add_event_cb(tv, OnTabChanged, LV_EVENT_VALUE_CHANGED, nullptr);
}

void OnSwipeBack() {
    HomeScreen::SwitchToHome();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    s_session.fetch_add(1, std::memory_order_relaxed);
    DestroyCityPickerHard();
    s_screen = nullptr;
    s_tabview = nullptr;
    s_tab_overview = nullptr;
    s_tab_forecast = nullptr;
    s_tab_hours = nullptr;
    s_tab_index = nullptr;
    s_status_lbl = nullptr;
    s_mounted = false;
    s_overview_dirty = false;
    s_forecast_dirty = false;
    s_hours_dirty = false;
    s_index_dirty = false;
    s_save_busy = false;
    s_hide_busy = false;
}

void BuildChrome(lv_obj_t *scr)
{
    lv_obj_t* top = lv_obj_create(scr);
    lv_obj_set_size(top, kPanelW, kHeaderH);
    lv_obj_set_pos(top, 0, 0);
    screen_strip_obj_chrome(top);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(top, lv_color_hex(kColorHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* divider = lv_obj_create(top);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

    lv_obj_t* back = lv_button_create(top);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, kHeaderSidePad, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = MakeLabel(top, I18n::T("天气"), font_main(), kColorText);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, kHeaderSidePad + kBackBtnSize + 12, 0);

    s_status_lbl = MakeLabel(top, "", font_sub(), kColorSubtle);
    lv_obj_align_to(s_status_lbl, title, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    lv_obj_t* refresh = lv_button_create(top);
    lv_obj_set_size(refresh, kRefreshBtnW, kHeaderBtnH);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, -kHeaderSidePad, 0);
    StyleHeaderBtn(refresh);
    lv_obj_add_event_cb(refresh, OnRefreshClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* refresh_lbl = lv_label_create(refresh);
    lv_label_set_text(refresh_lbl, I18n::T("刷新"));
    lv_obj_set_style_text_font(refresh_lbl, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_color(refresh_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(refresh_lbl);
    lv_obj_remove_flag(refresh_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* region = lv_button_create(top);
    lv_obj_set_size(region, kRegionBtnW, kHeaderBtnH);
    lv_obj_align(region, LV_ALIGN_RIGHT_MID,
                 -(kHeaderSidePad + kRefreshBtnW + kHeaderBtnGap), 0);
    StyleHeaderBtn(region);
    lv_obj_add_event_cb(region, OnCityPickerOpenClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* region_lbl = lv_label_create(region);
    lv_label_set_text(region_lbl, I18n::T("地区"));
    lv_obj_set_style_text_font(region_lbl, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_color(region_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(region_lbl);
    lv_obj_remove_flag(region_lbl, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t *CreateStaticImpl()
{
    write(1, "WX_CR\n", 6);
    s_session.fetch_add(1, std::memory_order_relaxed);
    s_mounted = false;

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelW);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Full chrome — fetch after first paint, not inside APP_CREATE. */
    BuildTabView(scr);
    BuildChrome(scr);
    WeatherService::Instance().SetDistrictId(LoadSavedDistrictId());
    ShowLoadingPlaceholder();
    s_mounted = true;
    lv_timer_t *arm = lv_timer_create(
        [](lv_timer_t *t) {
            lv_timer_delete(t);
            if (s_screen != nullptr) {
                write(1, "WX_ARM\n", 7);
                TriggerFetch();
            }
        },
        200, nullptr);
    if (arm != nullptr) {
        lv_timer_set_repeat_count(arm, 1);
    }

    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(scr, OnSwipeBack);

    write(1, "WX_CR_OK\n", 9);
    return scr;
}

}  // namespace weather_detail

lv_obj_t* WeatherScreen::CreateStatic() {
    return weather_detail::CreateStaticImpl();
}

lv_obj_t* WeatherScreen::Create() {
    root_ = CreateStatic();
    return root_;
}
