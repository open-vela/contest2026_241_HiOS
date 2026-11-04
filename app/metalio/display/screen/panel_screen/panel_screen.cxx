#include "weather_screen/weather_service.h"
#include "panel_screen.h"
#include "battery_indicator.h"
#include "smart_home/smart_home.h"
#include "home_screen/home_screen.h"
#include "weather_screen/weather_screen.h"
#include "system_chrome/control_center.h"
#include "screen_util.h"
#include "board_shim.h"
#include "font_awesome.h"
#include "i18n.h"
#include "idle_power_policy/idle_power_policy.h"
#include "pwr_key_handler/pwr_key_handler.h"
#include "system_chrome/system_chrome.h"
#include "application.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_120_4);
LV_FONT_DECLARE(font_chat_cjk_30);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

namespace {

constexpr int kPanel = 720;
constexpr uint32_t kBg = 0x000000;
constexpr uint32_t kCard = 0x2C3138;
constexpr uint32_t kDateCard = 0x3A404A;
constexpr uint32_t kText = 0xFFFFFF;
constexpr uint32_t kSub = 0xA7ADB4;
constexpr uint32_t kGreen = 0x34C759;
constexpr uint32_t kYellow = 0xF5C518;
constexpr uint32_t kTrack = 0x4A5058;
constexpr uint32_t kTrackOff = 0x3A3F46;
constexpr uint32_t kPowerOff = 0x3A3E46;

constexpr int kCardSlots = 2;

lv_obj_t *s_root = nullptr;
bool s_foreground = false;
lv_timer_t *s_timer = nullptr;
lv_timer_t *s_battery_timer = nullptr;
lv_timer_t *s_wake_word_timer = nullptr;
bool s_pull_active = false;
bool s_pull_slider = false;
bool s_swallow_click = false;
int16_t s_pull_x = 0;
int16_t s_pull_y = 0;

enum class PageTransitionTarget : uint8_t { None, Launcher, Panel };
PageTransitionTarget s_page_transition_target = PageTransitionTarget::None;
lv_obj_t *s_page_transition_old = nullptr;
lv_obj_t *s_page_transition_next = nullptr;
lv_timer_t *s_page_transition_timer = nullptr;
bool s_page_transition_active = false;

void BindPull(lv_obj_t *obj);

void FinishPageTransition(lv_timer_t *timer)
{
    if (s_page_transition_old != nullptr &&
        lv_obj_get_parent(s_page_transition_old) != nullptr)
        return;

    if (timer != nullptr)
        lv_timer_delete(timer);
    s_page_transition_timer = nullptr;
    s_page_transition_old = nullptr;
    const PageTransitionTarget target = s_page_transition_target;
    lv_obj_t *next_page = s_page_transition_next;
    s_page_transition_next = nullptr;
    s_page_transition_target = PageTransitionTarget::None;
    s_page_transition_active = false;

    if (target == PageTransitionTarget::Launcher) {
        if (next_page != nullptr && lv_screen_active() != next_page)
            screen_load_safe(next_page);
        if (!HomeScreen::IsHomeObject(lv_screen_active()))
            PanelScreen::Show();
    } else if (target == PageTransitionTarget::Panel) {
        if (next_page != nullptr && lv_screen_active() != next_page)
            screen_load_safe(next_page);
        if (!PanelScreen::IsPanelObject(lv_screen_active()))
            HomeScreen::OpenGrid();
    }
}

void RetryPageTransitionAsync(void *)
{
    FinishPageTransition(nullptr);
    if (s_page_transition_active && s_page_transition_timer == nullptr)
        lv_async_call(RetryPageTransitionAsync, nullptr);
}

bool BeginPageTransition(PageTransitionTarget target, lv_obj_t *old_page)
{
    if (s_page_transition_active || old_page == nullptr ||
        target == PageTransitionTarget::None)
        return false;

    /* Construct the destination while the outgoing page is still visible.
     * Loading a temporary solid-color bridge first exposed a blank frame on
     * every launcher <-> smart-home transition. */
    lv_obj_t *next_page = target == PageTransitionTarget::Launcher
                              ? HomeScreen::CreateStatic()
                              : PanelScreen::CreateStatic();
    if (next_page == nullptr || next_page == old_page)
        return false;

    s_page_transition_active = true;
    s_page_transition_target = target;
    s_page_transition_old = old_page;
    s_page_transition_next = next_page;
    screen_load_safe(next_page);
    /* A previous control-center update may have temporarily suppressed
     * invalidation. Loading the new screen alone is not enough in that case:
     * force a full-screen draw so the DSI framebuffer cannot keep the prior
     * page or fall back to the panel's solid-blue error frame. */
    if (lv_display_t *disp = lv_display_get_default())
        lv_display_enable_invalidation(disp, true);
    lv_obj_invalidate(next_page);
    if (target == PageTransitionTarget::Launcher) {
        /* Keep the outgoing panel alive until the launcher has rendered.
         * The RGB888 DSI scanout can show the default blue layer if the
         * outgoing screen is deleted in the same turn as lv_screen_load().
         * Both principal screens are cached and reused for subsequent swaps. */
    }
    s_page_transition_timer = lv_timer_create(FinishPageTransition, 20, nullptr);
    if (s_page_transition_timer == nullptr) {
        /* Keep the lightweight bridge visible and let LVGL's async queue
         * finish deleting the old screen before allocating the destination. */
        lv_async_call(RetryPageTransitionAsync, nullptr);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Activation-blocked dialog (same UX as home/chat screens)
// ---------------------------------------------------------------------------
lv_obj_t *s_activation_mask = nullptr;
bool s_activation_blocked = false;
bool s_activation_dialog_shows_code = false;
lv_timer_t *s_activation_guard_timer = nullptr;
lv_obj_t *s_activation_code = nullptr;

bool can_use_local_voice_ui()
{
    /* Server binding must not disable local AI entry or wake detection. */
    return true;
}

void close_activation_blocked_dialog()
{
    if (s_activation_mask != nullptr) {
        lv_obj_delete(s_activation_mask);
        s_activation_mask = nullptr;
    }
    s_activation_dialog_shows_code = false;
}

void open_activation_blocked_dialog(lv_obj_t *parent_screen)
{
    /* Activation is shown only in the home header. */
}

void sync_activation_block_state()
{
    auto &app = Application::GetInstance();
    const bool should_block = !can_use_local_voice_ui();
    const bool has_code = app.HasPendingActivation();
    if (should_block == s_activation_blocked) {
        if (should_block) {
            if (has_code && !s_activation_dialog_shows_code) {
                close_activation_blocked_dialog();
                open_activation_blocked_dialog(s_root);
            } else if (s_activation_mask == nullptr) {
                open_activation_blocked_dialog(s_root);
            }
        }
        return;
    }
    s_activation_blocked = should_block;
    if (should_block) {
        open_activation_blocked_dialog(s_root);
    } else {
        close_activation_blocked_dialog();
    }
}

void on_activation_guard_timer(lv_timer_t * /*timer*/)
{
    sync_activation_block_state();
    if (s_activation_code == nullptr) return;
    auto &app = Application::GetInstance();
    if (app.HasPendingActivation()) {
        char text[64];
        std::snprintf(text, sizeof(text), "验证码: %s",
                      app.GetPendingActivationCode().c_str());
        if (std::strcmp(lv_label_get_text(s_activation_code), text) != 0)
            lv_label_set_text(s_activation_code, text);
        lv_obj_remove_flag(s_activation_code, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_activation_code, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *s_time = nullptr;
lv_obj_t *s_wifi = nullptr;
lv_obj_t *s_battery = nullptr;
lv_obj_t *s_airplane = nullptr;
lv_obj_t *s_week = nullptr;
lv_obj_t *s_day = nullptr;
lv_obj_t *s_lunar = nullptr;
lv_obj_t *s_w_text = nullptr;
lv_obj_t *s_w_temp = nullptr;
lv_obj_t *s_w_unit = nullptr;
lv_obj_t *s_w_place = nullptr;
lv_obj_t *s_w_district = nullptr;
lv_obj_t *s_presence_home = nullptr;
lv_obj_t *s_presence_away = nullptr;

struct DevCard {
    lv_obj_t *root = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *name = nullptr;
    lv_obj_t *sub = nullptr;
    lv_obj_t *power = nullptr;
    lv_obj_t *slider = nullptr;
    int index = -1;
    uint32_t power_color = 0xFFFFFFFFu;
    uint32_t track_color = 0xFFFFFFFFu;
};

DevCard s_cards[kCardSlots];

const uint32_t kLunarInfo[] = {
    0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
    0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
    0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
    0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
    0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
    0x06ca0,0x0b550,0x15355,0x04da0,0x0a5b0,0x14573,0x052b0,0x0a9a8,0x0e950,0x06aa0,
    0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
    0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b6a0,0x195a6,
    0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
    0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x055c0,0x0ab60,0x096d5,0x092e0,
    0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
    0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
    0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
    0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
    0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0,
    0x14b63,
};

int LunarLeapMonth(int y)
{
    return static_cast<int>(kLunarInfo[y - 1900] & 0xf);
}

int LunarLeapDays(int y)
{
    if (LunarLeapMonth(y) == 0)
        return 0;
    return (kLunarInfo[y - 1900] & 0x10000) ? 30 : 29;
}

int LunarYearDays(int y)
{
    int sum = 348;
    for (uint32_t mask = 0x8000; mask > 0x8; mask >>= 1) {
        if (kLunarInfo[y - 1900] & mask)
            ++sum;
    }
    return sum + LunarLeapDays(y);
}

int LunarMonthDays(int y, int m)
{
    return (kLunarInfo[y - 1900] & (0x10000u >> m)) ? 30 : 29;
}

void FormatLunarDay(int day, char *out, size_t n)
{
    static const char *kDigit[] = {
        "十", "一", "二", "三", "四", "五", "六", "七", "八", "九",
    };
    if (day <= 0)
        day = 1;
    if (day > 30)
        day = 30;
    if (day <= 10)
        std::snprintf(out, n, "初%s", kDigit[day == 10 ? 0 : day]);
    else if (day < 20)
        std::snprintf(out, n, "十%s", kDigit[day - 10]);
    else if (day == 20)
        std::snprintf(out, n, "二十");
    else if (day < 30)
        std::snprintf(out, n, "廿%s", kDigit[day - 20]);
    else
        std::snprintf(out, n, "三十");
}

int DaysBeforeMonth(int y, int m)
{
    static const int kMd[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = 0;
    for (int mm = 1; mm < m; ++mm) {
        days += kMd[mm];
        if (mm == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)))
            ++days;
    }
    return days;
}

int DaysSince1900Jan31(int y, int m, int d)
{
    int days = 0;
    for (int yy = 1900; yy < y; ++yy) {
        const bool leap = (yy % 4 == 0 && yy % 100 != 0) || (yy % 400 == 0);
        days += leap ? 366 : 365;
    }
    days += DaysBeforeMonth(y, m);
    days += d;
    return days - 31;
}

bool FormatLunar(int y, int m, int d, char *out, size_t n)
{
    if (y < 1901 || y > 2049)
        return false;
    int off = DaysSince1900Jan31(y, m, d);
    int yy = 1900;
    while (yy < 2050 && off >= LunarYearDays(yy)) {
        off -= LunarYearDays(yy);
        ++yy;
    }
    const int leap = LunarLeapMonth(yy);
    bool in_leap = false;
    int mm = 1;
    while (mm <= 12) {
        int md;
        if (leap > 0 && mm == leap + 1 && !in_leap) {
            --mm;
            in_leap = true;
            md = LunarLeapDays(yy);
        } else {
            md = LunarMonthDays(yy, mm);
        }
        if (off < md)
            break;
        off -= md;
        if (in_leap && mm == leap)
            in_leap = false;
        ++mm;
    }
    static const char *kMonth[] = {
        "正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊",
    };
    char day[16];
    FormatLunarDay(off + 1, day, sizeof(day));
    const char *month = (mm >= 1 && mm <= 12) ? kMonth[mm - 1] : "正";
    std::snprintf(out, n, "农历%s%s月%s", in_leap ? "闰" : "", month, day);
    return true;
}

void StyleCard(lv_obj_t *card, uint32_t color)
{
    lv_obj_remove_style_all(card);
    screen_strip_obj_chrome(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    BindPull(card);
    screen_kill_theme_chrome(card);
}

lv_obj_t *MakeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font,
                    uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

const char *KindGlyph(int kind)
{
    switch (kind) {
    case kSmartSwitch:
        return FONT_AWESOME_PLUG;
    case kSmartCurtain:
        return FONT_AWESOME_BARS;
    case kSmartClimate:
        return FONT_AWESOME_WIND;
    case kSmartFan:
        return FONT_AWESOME_REFRESH;
    default:
        return FONT_AWESOME_SUN;
    }
}

bool s_painting_card = false;

void PaintCard(DevCard *card)
{
    if (card == nullptr || card->root == nullptr)
        return;
    const SmartDevice *dev =
        card->index >= 0 ? SmartHome_At(card->index) : nullptr;
    s_painting_card = true;
    if (dev == nullptr) {
        lv_obj_add_flag(card->root, LV_OBJ_FLAG_HIDDEN);
        s_painting_card = false;
        return;
    }
    lv_obj_remove_flag(card->root, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(card->icon, KindGlyph(dev->kind));
    lv_label_set_text(card->name, dev->name);
    char sub[48];
    std::snprintf(sub, sizeof(sub), "%s | %s", dev->room, SmartHome_StatusText(dev));
    lv_label_set_text(card->sub, sub);

    const bool opened =
        (dev->kind == kSmartCurtain) ? dev->level > 0 : dev->on != 0;
    /* Style writes invalidate even when the color is unchanged. The idle
     * card refresh used to do that every 10 s, and a transparent slider
     * blend on that invalidate latches this panel solid blue. */
    const uint32_t power_hex = opened ? kGreen : kPowerOff;
    if (card->power_color != power_hex) {
        card->power_color = power_hex;
        lv_obj_set_style_bg_color(card->power, lv_color_hex(power_hex),
                                  LV_PART_MAIN);
    }
    if (card->slider != nullptr) {
        const bool show = dev->kind != kSmartSwitch;
        if (show)
            lv_obj_remove_flag(card->slider, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(card->slider, LV_OBJ_FLAG_HIDDEN);
        int lo = 0;
        int hi = 100;
        SmartHome_LevelRange(dev->kind, &lo, &hi);
        lv_slider_set_range(card->slider, lo, hi);
        lv_slider_set_value(card->slider, dev->level, LV_ANIM_OFF);
        const uint32_t track_hex = opened ? kYellow : kTrackOff;
        if (card->track_color != track_hex) {
            card->track_color = track_hex;
            lv_obj_set_style_bg_color(card->slider, lv_color_hex(track_hex),
                                      LV_PART_INDICATOR);
        }
    }
    s_painting_card = false;
}

void RefreshCards(void)
{
    int pinned[kCardSlots];
    const int n = SmartHome_Pinned(pinned, kCardSlots);
    for (int i = 0; i < kCardSlots; ++i) {
        s_cards[i].index = (i < n) ? pinned[i] : -1;
        PaintCard(&s_cards[i]);
    }
}

void RefreshClock(void)
{
    BatteryIndicatorUpdate(s_battery);
    const bool airplane = metalio_board_get_network_mode() == 2;
    if (s_airplane != nullptr)
        lv_obj_set_flag(s_airplane, LV_OBJ_FLAG_HIDDEN, !airplane);
    if (s_wifi != nullptr) {
        lv_obj_set_flag(s_wifi, LV_OBJ_FLAG_HIDDEN, airplane);
        const char *icon = metalio_board_get_network_icon();
        if (icon == nullptr || icon[0] == '\0') icon = FONT_AWESOME_WIFI;
        if (std::strcmp(lv_label_get_text(s_wifi), icon) != 0)
            lv_label_set_text(s_wifi, icon);
    }
    if (s_time == nullptr)
        return;
    const time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) == nullptr || tm_info.tm_year < 2025 - 1900) {
        lv_label_set_text(s_time, "--:--");
        // Preserve the date card while the local clock is being initialized.
        return;
    }
    char buf[16];
    if (metalio_board_clock_ready())
        std::strftime(buf, sizeof(buf), "%H:%M", &tm_info);
    else
        std::snprintf(buf, sizeof(buf), "--:--");
    if (std::strcmp(lv_label_get_text(s_time), buf) != 0)
        lv_label_set_text(s_time, buf);
    static const char *kWeek[] = {"星期日", "星期一", "星期二", "星期三",
                                  "星期四", "星期五", "星期六"};
    if (s_week != nullptr &&
        std::strcmp(lv_label_get_text(s_week), kWeek[tm_info.tm_wday]) != 0)
        lv_label_set_text(s_week, kWeek[tm_info.tm_wday]);
    if (s_day != nullptr) {
        std::snprintf(buf, sizeof(buf), "%d", tm_info.tm_mday);
        if (std::strcmp(lv_label_get_text(s_day), buf) != 0)
            lv_label_set_text(s_day, buf);
    }
    if (s_lunar != nullptr) {
        char lunar[40];
        if (FormatLunar(tm_info.tm_year + 1900, tm_info.tm_mon + 1,
                        tm_info.tm_mday, lunar, sizeof(lunar)) &&
            std::strcmp(lv_label_get_text(s_lunar), lunar) != 0)
            lv_label_set_text(s_lunar, lunar);
    }
}

void PaintWeather(void)
{
    WeatherService::PanelFields view{};
    WeatherService::Instance().CopyPanel(&view);
    if(s_w_text && std::strcmp(lv_label_get_text(s_w_text),view.description)) lv_label_set_text(s_w_text,view.description);
    bool temp_changed = false;
    if(s_w_temp && std::strcmp(lv_label_get_text(s_w_temp),view.temperature)) {
        lv_label_set_text(s_w_temp,view.temperature);
        temp_changed = true;
    }
    if(temp_changed && s_w_unit && s_w_temp){lv_obj_update_layout(s_w_temp);lv_obj_align_to(s_w_unit,s_w_temp,LV_ALIGN_OUT_RIGHT_TOP,0,8);}
    if(s_w_place && std::strcmp(lv_label_get_text(s_w_place),view.place_top)) lv_label_set_text(s_w_place,view.place_top);
    if(s_w_district && std::strcmp(lv_label_get_text(s_w_district),view.place_bottom)) lv_label_set_text(s_w_district,view.place_bottom);
}

void RefreshWeather(void)
{
    WeatherService::Instance().Request();
    PaintWeather();
}

void OnTick(lv_timer_t * /*t*/)
{
    if (!s_foreground)
        return;
    RefreshClock();
    RefreshWeather();
    RefreshCards();
}

int CardIndexFromEvent(lv_event_t *e)
{
    auto *card = static_cast<DevCard *>(lv_event_get_user_data(e));
    if (card == nullptr)
        return -1;
    return card->index;
}

bool TakeSwipeClick()
{
    if (!s_swallow_click)
        return false;
    s_swallow_click = false;
    return true;
}

void OnPower(lv_event_t *e)
{
    if (TakeSwipeClick())
        return;
    const int index = CardIndexFromEvent(e);
    const SmartDevice *dev = SmartHome_At(index);
    if (dev == nullptr)
        return;
    const bool opened =
        (dev->kind == kSmartCurtain) ? dev->level > 0 : dev->on != 0;
    SmartHome_SetPower(index, opened ? 0 : 1, true);
    RefreshCards();
    IdlePower_NotifyActivity();
}

void OnSlider(lv_event_t *e)
{
    if (s_painting_card)
        return;
    const int index = CardIndexFromEvent(e);
    if (SmartHome_At(index) == nullptr)
        return;
    auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const int value = static_cast<int>(lv_slider_get_value(slider));
    const bool release = lv_event_get_code(e) == LV_EVENT_RELEASED;
    SmartHome_SetLevel(index, value, release);
    if (release)
        RefreshCards();
    else
        PaintCard(static_cast<DevCard *>(lv_event_get_user_data(e)));
    IdlePower_NotifyActivity();
}

void OnOpenHome(lv_event_t * /*e*/)
{
    if (TakeSwipeClick())
        return;
    HomeScreen::LaunchApp("home");
}

void OnOpenWeather(lv_event_t * /*e*/)
{
    if (TakeSwipeClick())
        return;
    HomeScreen::LaunchApp("weather");
}

void OnOpenCalendar(lv_event_t * /*e*/)
{
    if (TakeSwipeClick())
        return;
    HomeScreen::LaunchApp("calendar");
}

void PaintPresence(void)
{
    const bool home = SmartHome_AtHome();
    if (s_presence_home != nullptr) {
        lv_obj_set_style_bg_color(s_presence_home,
                                  lv_color_hex(home ? kGreen : kCard),
                                  LV_PART_MAIN);
    }
    if (s_presence_away != nullptr) {
        lv_obj_set_style_bg_color(s_presence_away,
                                  lv_color_hex(home ? kCard : 0xE85D4C),
                                  LV_PART_MAIN);
    }
}

void OnPresence(lv_event_t *e)
{
    if (TakeSwipeClick())
        return;
    const int home = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_event_get_user_data(e)));
    SmartHome_SetAtHome(home != 0);
    PaintPresence();
    /* Rebuilding every device card in this click overlaps the settings
     * erase on a board whose journal is still empty and latches the panel. */
    lv_async_call([](void *) {
        RefreshCards();
        PaintPresence();
    }, nullptr);
    IdlePower_NotifyActivity();
}

void OnDock(lv_event_t *e)
{
    if (TakeSwipeClick())
        return;
    const char *id = static_cast<const char *>(lv_event_get_user_data(e));
    if (id == nullptr)
        return;
    if (std::strcmp(id, "grid") == 0) {
        /* Build and load the launcher after LVGL finishes dispatching the
         * dock click. Replacing the active screen inside LV_EVENT_CLICKED
         * can interrupt the DSI first-frame refresh on this board. */
        lv_async_call([](void *) { PanelScreen::OpenLauncher(); }, nullptr);
        return;
    }
    HomeScreen::LaunchApp(id);
}

lv_obj_t *MakeDockIcon(lv_obj_t *parent, uint32_t bg, const char *glyph,
                       uint32_t glyph_color, const char *id)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    screen_strip_obj_chrome(btn);
    lv_obj_set_size(btn, 88, 88);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 26, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    BindPull(btn);
    lv_obj_add_event_cb(btn, OnDock, LV_EVENT_CLICKED,
                        const_cast<char *>(id));
    if (glyph != nullptr) {
        lv_obj_t *lbl = MakeLabel(btn, glyph, &font_awesome_30_4, glyph_color);
        lv_obj_center(lbl);
    }
    return btn;
}

void BuildDeviceCard(lv_obj_t *parent, DevCard *card, int x, int y, int w, int h)
{
    lv_obj_t *box = lv_obj_create(parent);
    card->root = box;
    StyleCard(box, kCard);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);

    card->icon = MakeLabel(box, FONT_AWESOME_SUN, &font_awesome_20_4, kText);
    lv_obj_align(card->icon, LV_ALIGN_TOP_LEFT, 0, 0);

    card->name = MakeLabel(box, "", &font_puhui_30_4, kText);
    lv_obj_align(card->name, LV_ALIGN_TOP_LEFT, 36, -4);
    lv_obj_set_width(card->name, w - 130);
    lv_label_set_long_mode(card->name, LV_LABEL_LONG_DOT);

    card->sub = MakeLabel(box, "", &font_puhui_20_4, kSub);
    lv_obj_align(card->sub, LV_ALIGN_TOP_LEFT, 36, 40);

    card->power = lv_obj_create(box);
    lv_obj_remove_style_all(card->power);
    screen_strip_obj_chrome(card->power);
    lv_obj_set_size(card->power, 56, 56);
    lv_obj_align(card->power, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(card->power, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card->power, lv_color_hex(kPowerOff), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card->power, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(card->power, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card->power, LV_OBJ_FLAG_SCROLLABLE);
    BindPull(card->power);
    lv_obj_t *pwr = MakeLabel(card->power, FONT_AWESOME_POWER_OFF,
                              &font_awesome_20_4, kText);
    lv_obj_center(pwr);
    lv_obj_add_event_cb(card->power, OnPower, LV_EVENT_CLICKED, card);

    card->slider = lv_slider_create(box);
    lv_obj_set_width(card->slider, w - 36);
    lv_obj_set_height(card->slider, 10);
    lv_obj_align(card->slider, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(card->slider, lv_color_hex(kTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_color(card->slider, lv_color_hex(kYellow), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(card->slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(card->slider, 0, LV_PART_KNOB);
    lv_obj_set_style_radius(card->slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(card->slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_add_event_cb(card->slider, OnSlider, LV_EVENT_VALUE_CHANGED, card);
    lv_obj_add_event_cb(card->slider, OnSlider, LV_EVENT_RELEASED, card);
    screen_swipe_back_ignore(card->slider, true);

    lv_obj_t *sun_l = MakeLabel(box, FONT_AWESOME_SUN, &font_awesome_20_4, kSub);
    lv_obj_align(sun_l, LV_ALIGN_BOTTOM_LEFT, 0, -28);
    lv_obj_t *sun_r = MakeLabel(box, FONT_AWESOME_SUN, &font_awesome_30_4, kText);
    lv_obj_align(sun_r, LV_ALIGN_BOTTOM_RIGHT, 0, -22);
}

bool PullStartedOnSlider(lv_obj_t *obj)
{
    return obj == s_cards[0].slider || obj == s_cards[1].slider;
}

void OnPullPressed(lv_event_t *e)
{
    s_swallow_click = false;
    if (s_pull_active)
        return;
    if (SystemChrome_IsControlCenterOpen() || SystemChrome_IsRecentsOpen())
        return;
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == nullptr)
        return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_pull_active = true;
    s_pull_slider = PullStartedOnSlider(lv_event_get_target_obj(e));
    s_pull_x = static_cast<int16_t>(p.x);
    s_pull_y = static_cast<int16_t>(p.y);
}

void OnPullReleased(lv_event_t *e)
{
    if (!s_pull_active)
        return;
    s_pull_active = false;
    if (s_pull_slider)
        return;
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == nullptr)
        return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int dx = static_cast<int>(p.x) - s_pull_x;
    const int dy = static_cast<int>(p.y) - s_pull_y;
    if (dy > 48 && dy > std::abs(dx)) {
        s_swallow_click = true;
        SystemChrome_ShowControlCenter();
    }
}

void BindPull(lv_obj_t *obj)
{
    if (obj == nullptr)
        return;
    lv_obj_add_event_cb(obj, OnPullPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(obj, OnPullReleased, LV_EVENT_RELEASED, nullptr);
}

void OnLoaded(lv_event_t *e)
{
    auto *scr = static_cast<lv_obj_t *>(lv_event_get_target(e));
    s_foreground = true;
    PwrKey_OnScreenLifecycle("panel", SCREEN_LIFECYCLE_LOAD);
        IdlePower_Attach(IdlePowerSession::Home, true);
    SystemChrome_Attach(scr, true);
    RefreshClock();
    PaintWeather();
    RefreshCards();
    PaintPresence();
    /* The first scan of this screen is still in progress here. Starting the
     * weather thread or the wake engine in this same turn races that scan
     * and intermittently latches the panel blue on every board of this type.
     * Wait until later refreshes have completed. */
    if (s_wake_word_timer != nullptr) {
        lv_timer_delete(s_wake_word_timer);
        s_wake_word_timer = nullptr;
    }
    s_wake_word_timer = lv_timer_create([](lv_timer_t *timer) {
        lv_timer_delete(timer);
        s_wake_word_timer = nullptr;
        if (!s_foreground || s_root != lv_screen_active())
            return;
        WeatherService::Instance().Request();
        PaintWeather();
        Application::GetInstance().ArmAssistantWakeWord();
    }, 300, nullptr);
    if (s_wake_word_timer != nullptr)
        lv_timer_set_repeat_count(s_wake_word_timer, 1);

    /* Activation-block guard: show the 6-digit bind-code dialog on the panel
     * screen when WiFi is up but xiaozhi.me binding is still pending. */
    s_activation_blocked = !can_use_local_voice_ui();
    if (s_activation_blocked && Application::GetInstance().HasPendingActivation())
        open_activation_blocked_dialog(scr);
    sync_activation_block_state();
    if (s_activation_guard_timer == nullptr)
        s_activation_guard_timer = lv_timer_create(on_activation_guard_timer, 250, nullptr);

    if (s_timer == nullptr) {
        /* 10 s is enough for HH:MM / date / weather. A 1 Hz timer on this
         * screen invalidated the same regions every second and kept the DSI
         * FIFO on the edge until the panel latched solid blue. The battery
         * corner is refreshed from this same tick, and only when the shown
         * value changes. */
        s_timer = lv_timer_create(OnTick, 10000, nullptr);
    }
}

void OnUnloaded(lv_event_t * /*e*/)
{
    s_foreground = false;
    PwrKey_OnScreenLifecycle("panel", SCREEN_LIFECYCLE_UNLOAD);
    // LVGL can load the destination before unloading this screen.
    // The launcher has already attached its own Home idle session then.
    if (std::strcmp(PwrKey_ActiveScreen(), "home") != 0)
        IdlePower_Detach(IdlePowerSession::Home);
    if (s_wake_word_timer != nullptr) {
        lv_timer_delete(s_wake_word_timer);
        s_wake_word_timer = nullptr;
    }
    if (s_activation_guard_timer != nullptr) {
        lv_timer_delete(s_activation_guard_timer);
        s_activation_guard_timer = nullptr;
    }
    close_activation_blocked_dialog();
}

void OnDeleted(lv_event_t * /*e*/)
{
    if (s_wake_word_timer != nullptr) {
        lv_timer_delete(s_wake_word_timer);
        s_wake_word_timer = nullptr;
    }
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    if (s_battery_timer != nullptr) {
        lv_timer_delete(s_battery_timer);
        s_battery_timer = nullptr;
    }
    if (s_activation_guard_timer != nullptr) {
        lv_timer_delete(s_activation_guard_timer);
        s_activation_guard_timer = nullptr;
    }
    close_activation_blocked_dialog();
    s_activation_blocked = false;
    s_activation_dialog_shows_code = false;
    s_foreground = false;
    s_pull_active = false;
    s_swallow_click = false;
    s_root = nullptr;
    s_time = nullptr;
    s_activation_code = nullptr;
    s_wifi = nullptr;
    s_battery = nullptr;
    s_airplane = nullptr;
    s_week = nullptr;
    s_day = nullptr;
    s_lunar = nullptr;
    s_w_text = nullptr;
    s_w_temp = nullptr;
    s_w_unit = nullptr;
    s_w_place = nullptr;
    s_w_district = nullptr;
    s_presence_home = nullptr;
    s_presence_away = nullptr;
    for (DevCard &c : s_cards)
        c = DevCard{};
}

}  // namespace

lv_obj_t *PanelScreen::CreateStatic()
{
    if (s_root != nullptr)
        return s_root;

    SmartHome_EnsureLoaded();

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_root = scr;
    lv_obj_remove_style_all(scr);
    screen_strip_obj_chrome(scr);
    screen_kill_theme_chrome(scr);
    lv_obj_set_size(scr, kPanel, kPanel);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_time = MakeLabel(scr, "--:--", &font_puhui_20_4, kText);
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 22);
    s_battery = BatteryIndicatorCreate(scr);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -28, 20);
    s_activation_code = MakeLabel(scr, "", &font_puhui_20_4, 0xFBBF24);
    lv_obj_align(s_activation_code, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_add_flag(s_activation_code, LV_OBJ_FLAG_HIDDEN);
    s_wifi = MakeLabel(scr, FONT_AWESOME_WIFI, &font_awesome_20_4, kText);
    lv_obj_align(s_wifi, LV_ALIGN_TOP_LEFT, 28, 22);
    s_airplane = lv_image_create(scr);
    lv_image_set_src(s_airplane, ControlCenter_GetStatusPlaneIcon());
    lv_obj_align(s_airplane, LV_ALIGN_TOP_LEFT, 28, 20);
    lv_obj_add_flag(s_airplane, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *date = lv_obj_create(scr);
    StyleCard(date, kDateCard);
    lv_obj_set_pos(date, 20, 78);
    lv_obj_set_size(date, 328, 268);
    s_week = MakeLabel(date, "星期一", &font_puhui_30_4, kText);
    lv_obj_align(s_week, LV_ALIGN_TOP_LEFT, 8, 8);
    s_day = MakeLabel(date, "1", &font_puhui_number_120_4, 0x4C8DFF);
    lv_obj_align(s_day, LV_ALIGN_RIGHT_MID, -6, -8);
    s_lunar = MakeLabel(date, "农历", &font_puhui_20_4, kSub);
    lv_obj_align(s_lunar, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    screen_make_input_passive(date);
    lv_obj_add_flag(date, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(date, OnOpenCalendar, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *weather = lv_obj_create(scr);
    StyleCard(weather, kCard);
    lv_obj_set_pos(weather, 20, 362);
    lv_obj_set_size(weather, 328, 176);
    lv_obj_add_flag(weather, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(weather, OnOpenWeather, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cloud = MakeLabel(weather, FONT_AWESOME_CLOUD, &font_awesome_30_4, 0x7EB6FF);
    lv_obj_align(cloud, LV_ALIGN_TOP_LEFT, 4, 4);
    s_w_text = MakeLabel(weather, "天气", &font_puhui_30_4, kText);
    lv_obj_set_width(s_w_text, 240);
    lv_label_set_long_mode(s_w_text, LV_LABEL_LONG_DOT);
    lv_obj_align(s_w_text, LV_ALIGN_TOP_LEFT, 52, 8);
    s_w_temp = MakeLabel(weather, "--", &font_puhui_number_120_4, kText);
    lv_obj_align(s_w_temp, LV_ALIGN_BOTTOM_LEFT, 0, 6);
    s_w_unit = MakeLabel(weather, "°C", &font_puhui_20_4, kText);
    lv_obj_align_to(s_w_unit, s_w_temp, LV_ALIGN_OUT_RIGHT_TOP, 0, 8);
    s_w_place = MakeLabel(weather, "点击查看", &font_puhui_30_4, kSub);
    lv_obj_set_width(s_w_place, 180);
    lv_label_set_long_mode(s_w_place, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_w_place, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(s_w_place, LV_ALIGN_TOP_RIGHT, -12, 42);
    s_w_district=MakeLabel(weather,"",&font_puhui_30_4,kSub);
    lv_obj_set_width(s_w_district,180);lv_label_set_long_mode(s_w_district,LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_w_district,LV_TEXT_ALIGN_RIGHT,LV_PART_MAIN);
    lv_obj_align(s_w_district,LV_ALIGN_TOP_RIGHT,-12,78);

    BuildDeviceCard(scr, &s_cards[0], 364, 78, 336, 212);
    BuildDeviceCard(scr, &s_cards[1], 364, 306, 336, 212);
    lv_obj_add_flag(s_cards[0].root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cards[1].root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cards[0].root, OnOpenHome, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_cards[1].root, OnOpenHome, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *music = MakeDockIcon(scr, 0xFF8A1E, FONT_AWESOME_MUSIC, kText, "hi_music");
    lv_obj_set_pos(music, 20, 560);
    lv_obj_t *cal = MakeDockIcon(scr, 0x34C759, FONT_AWESOME_CALENDAR, kText, "calendar");
    lv_obj_set_pos(cal, 124, 560);
    lv_obj_t *gear = MakeDockIcon(scr, kCard, FONT_AWESOME_GEAR, kText, "settings");
    lv_obj_set_pos(gear, 228, 560);

    lv_obj_t *folder = MakeDockIcon(scr, kCard, nullptr, kText, "grid");
    lv_obj_set_pos(folder, 332, 560);
    static const uint32_t kMini[] = {0xFF8A1E, 0x34C759, 0x5B8DEF, 0xF5C518};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *dot = lv_obj_create(folder);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 22, 22);
        lv_obj_set_style_bg_color(dot, lv_color_hex(kMini[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(dot, 6, LV_PART_MAIN);
        lv_obj_set_pos(dot, 16 + (i % 2) * 30, 16 + (i / 2) * 30);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }

    auto make_presence = [](lv_obj_t *parent, const char *text, int x, int home) {
        lv_obj_t *card = lv_obj_create(parent);
        StyleCard(card, kCard);
        lv_obj_set_pos(card, x, 560);
        lv_obj_set_size(card, 124, 88);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, OnPresence, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(home)));
        lv_obj_t *lbl = MakeLabel(card, text, &font_puhui_30_4, kText);
        lv_obj_center(lbl);
        return card;
    };
    s_presence_home = make_presence(scr, "回家", 456, 1);
    s_presence_away = make_presence(scr, "离家", 588, 0);
    PaintPresence();

    BindPull(scr);
    lv_obj_add_event_cb(scr, OnLoaded, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);
    RefreshClock();
    RefreshCards();
    return scr;
}

lv_obj_t *PanelScreen::Show()
{
    if (s_page_transition_active)
        return nullptr;
    lv_obj_t *active = lv_screen_active();
    if (HomeScreen::IsHomeObject(active)) {
        BeginPageTransition(PageTransitionTarget::Panel, active);
        return nullptr;
    }

    lv_obj_t *panel = CreateStatic();
    if (panel == nullptr)
        return nullptr;
    lv_obj_t *old = lv_screen_active();
    if (old == panel)
        return panel;
    screen_load_safe(panel);
    const bool was_home = old != nullptr && HomeScreen::IsHomeObject(old);
    if (old != nullptr && old != panel && was_home)
        lv_obj_delete_async(old);
    return panel;
}

lv_obj_t *PanelScreen::OpenLauncher()
{
    if (s_page_transition_active)
        return nullptr;
    lv_obj_t *active = lv_screen_active();
    if (IsPanelObject(active)) {
        BeginPageTransition(PageTransitionTarget::Launcher, active);
        return nullptr;
    }
    return HomeScreen::OpenGrid();
}

bool PanelScreen::IsForeground()
{
    return s_foreground && s_root != nullptr;
}

void PanelScreen::RefreshDevices()
{
    lv_async_call([](void *) {
        if (PanelScreen::IsForeground())
            RefreshCards();
    }, nullptr);
}

bool PanelScreen::IsPanelObject(lv_obj_t *obj)
{
    return obj != nullptr && obj == s_root;
}

lv_obj_t *PanelScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

#if defined(HI_SERVICES_PROBE) && HI_SERVICES_PROBE
extern "C" bool hi_panel_probe_music_dock(){
    if(!s_root||lv_screen_active()!=s_root)return false;
    for(unsigned i=0;i<lv_obj_get_child_count(s_root);++i){auto* child=lv_obj_get_child(s_root,i);
        for(unsigned j=0;j<lv_obj_get_event_count(child);++j){auto* event=lv_obj_get_event_dsc(child,j);
            const auto* id=static_cast<const char*>(lv_event_dsc_get_user_data(event));
            if(lv_event_dsc_get_cb(event)==OnDock&&id&&!strcmp(id,"hi_music")){lv_obj_send_event(child,LV_EVENT_CLICKED,nullptr);return true;}
        }
    }return false;
}
#endif
