#include "hi_input_method.h"
#include "home_app_screen.h"
#include "smart_home/smart_home.h"
#include "smart_home/ha_client.h"
#include "smart_home/ha_qr_scanner.h"
#include "camera_screen/camera_screen.h"
#include "home_screen/home_screen.h"

#include <cstdio>
#include <cstring>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr int kPanel = 720;
constexpr uint32_t kBg = 0x0E1116;
constexpr uint32_t kCard = 0x1C212B;
constexpr uint32_t kText = 0xFFFFFF;
constexpr uint32_t kSub = 0x9AA3B2;
constexpr uint32_t kAccent = 0x34C759;
constexpr uint32_t kDanger = 0xE85D4C;

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_list_page = nullptr;
lv_obj_t *s_detail_page = nullptr;
lv_obj_t *s_scroll = nullptr;
lv_obj_t *s_count = nullptr;
int s_filter = 0;
int s_detail = -1;
lv_obj_t *s_ha_url = nullptr, *s_ha_token = nullptr, *s_ha_entity = nullptr;
lv_obj_t *s_ha_result = nullptr, *s_ha_keyboard = nullptr;
lv_timer_t *s_state_timer = nullptr;
lv_timer_t *s_build_timer = nullptr;
lv_obj_t *s_detail_body = nullptr, *s_detail_status = nullptr;
lv_obj_t *s_detail_power = nullptr, *s_detail_power_text = nullptr;
lv_obj_t *s_qr_overlay = nullptr, *s_qr_hint = nullptr;
lv_timer_t *s_qr_timer = nullptr;
uint32_t s_qr_started = 0;
lv_obj_t *s_delete_overlay = nullptr;

const char *kRooms[] = {"全部", "主卧", "客厅", "书房", "餐厅", "阳台", "次卧"};
constexpr int kRoomCount = 7;

/* Font Awesome plug/bars are not in font_awesome_30_4, so those badges
 * drew as 乱码. These characters are in the CJK font. */
const char *KindMark(int kind)
{
    switch (kind) {
    case kSmartSwitch:
        return "插";
    case kSmartCurtain:
        return "帘";
    case kSmartClimate:
        return "空";
    case kSmartFan:
        return "扇";
    default:
        return "灯";
    }
}

void ShowList(void);
void CloseQrScan(void);
void CloseDeleteDialog(void);
void ShowDetail(int index);
void RebuildList(void);

lv_obj_t *Label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

void StyleBtn(lv_obj_t *btn, uint32_t bg)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(btn, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_color_hex(kText), LV_PART_MAIN);
}

void OnSwipeBack()
{
    HomeScreen::SwitchToHome();
}

void OnBackHome(lv_event_t * /*e*/)
{
    HomeScreen::SwitchToHome();
}

void OnOpenDetail(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_event_get_user_data(e)));
    ShowDetail(index);
}

void OnFilter(lv_event_t *e)
{
    s_filter = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_event_get_user_data(e)));
    lv_obj_t *row = lv_obj_get_parent(static_cast<lv_obj_t *>(
        lv_event_get_target(e)));
    if (row != nullptr) {
        const int n = static_cast<int>(lv_obj_get_child_count(row));
        for (int i = 0; i < n; ++i) {
            lv_obj_t *chip = lv_obj_get_child(row, i);
            const int id = static_cast<int>(reinterpret_cast<intptr_t>(
                lv_obj_get_user_data(chip)));
            lv_obj_set_style_bg_color(chip,
                                      lv_color_hex(id == s_filter ? kAccent : kCard),
                                      LV_PART_MAIN);
        }
    }
    RebuildList();
}

void OnListPower(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_event_get_user_data(e)));
    const SmartDevice *dev = SmartHome_At(index);
    if (dev == nullptr)
        return;
    const bool opened =
        (dev->kind == kSmartCurtain) ? dev->level > 0 : dev->on != 0;
    SmartHome_SetPower(index, opened ? 0 : 1, true);
    RebuildList();
}

void OnAddKind(lv_event_t *e)
{
    const int kind = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_event_get_user_data(e)));
    const int index = SmartHome_Add(kind);
    if (index < 0)
        return;
    ShowDetail(index);
}

bool RoomVisible(const SmartDevice *dev)
{
    if (dev == nullptr)
        return false;
    if (s_filter <= 0)
        return true;
    return std::strcmp(dev->room, kRooms[s_filter]) == 0;
}

void RebuildList(void)
{
    if (s_scroll == nullptr)
        return;
    lv_obj_clean(s_scroll);
    int shown = 0;
    const int count = SmartHome_Count();
    for (int i = 0; i < count; ++i) {
        const SmartDevice *dev = SmartHome_At(i);
        if (!RoomVisible(dev))
            continue;
        ++shown;
        lv_obj_t *card = lv_obj_create(s_scroll);
        lv_obj_remove_style_all(card);
        screen_strip_obj_chrome(card);
        lv_obj_set_size(card, 640, 132);
        lv_obj_set_style_bg_color(card, lv_color_hex(kCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, OnOpenDetail, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_obj_t *badge = lv_obj_create(card);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 72, 72);
        lv_obj_set_pos(badge, 18, 30);
        lv_obj_set_style_radius(badge, 20, LV_PART_MAIN);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x2A3140), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *glyph = Label(badge, KindMark(dev->kind), &font_puhui_30_4, kText);
        lv_obj_center(glyph);

        lv_obj_t *name = Label(card, dev->name, &font_puhui_30_4, kText);
        lv_obj_set_pos(name, 108, 28);
        lv_obj_set_width(name, 360);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        char sub[48];
        std::snprintf(sub, sizeof(sub), "%s %s %s", dev->room,
                      SmartHome_KindName(dev->kind), SmartHome_StatusText(dev));
        lv_obj_t *line = Label(card, sub, &font_puhui_20_4, kSub);
        lv_obj_set_pos(line, 108, 74);

        lv_obj_t *power = lv_button_create(card);
        StyleBtn(power, (dev->on || (dev->kind == kSmartCurtain && dev->level > 0))
                            ? kAccent
                            : 0x2A3140);
        lv_obj_set_size(power, 88, 56);
        lv_obj_align(power, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_add_event_cb(power, OnListPower, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_t *pt = Label(power, dev->on || (dev->kind == kSmartCurtain && dev->level > 0)
                                      ? "开"
                                      : "关",
                             &font_puhui_20_4, kText);
        lv_obj_center(pt);
    }
    if (shown == 0) {
        lv_obj_t *empty = Label(s_scroll, "这个房间还没有设备", &font_puhui_20_4, kSub);
        lv_obj_set_style_pad_top(empty, 40, LV_PART_MAIN);
    }
    if (s_count != nullptr) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d 个设备", count);
        lv_label_set_text(s_count, buf);
    }
}

void OnDetailBack(lv_event_t * /*e*/)
{
    ShowList();
}

void OnDetailPower(lv_event_t * /*e*/)
{
    const SmartDevice *dev = SmartHome_At(s_detail);
    if (dev == nullptr)
        return;
    const bool opened =
        (dev->kind == kSmartCurtain) ? dev->level > 0 : dev->on != 0;
    SmartHome_SetPower(s_detail, opened ? 0 : 1, true);
    ShowDetail(s_detail);
}

void OnDetailSlider(lv_event_t *e)
{
    if (SmartHome_At(s_detail) == nullptr)
        return;
    auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const int value = static_cast<int>(lv_slider_get_value(slider));
    const bool release = lv_event_get_code(e) == LV_EVENT_RELEASED;
    SmartHome_SetLevel(s_detail, value, release);
    if (release)
        ShowDetail(s_detail);
}

void OnPickRoom(lv_event_t *e)
{
    const char *room = static_cast<const char *>(lv_event_get_user_data(e));
    SmartHome_SetRoom(s_detail, room);
    ShowDetail(s_detail);
}

void OnPickName(lv_event_t *e)
{
    const char *name = static_cast<const char *>(lv_event_get_user_data(e));
    SmartHome_SetName(s_detail, name);
    ShowDetail(s_detail);
}

void OnPin(lv_event_t *e)
{
    auto *sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    SmartHome_SetPinned(s_detail, on ? 1 : 0);
}

void CloseDeleteDialog(void)
{
    if (s_delete_overlay) lv_obj_delete(s_delete_overlay);
    s_delete_overlay = nullptr;
}

void OnDeleteCancel(lv_event_t * /*e*/) { CloseDeleteDialog(); }

void OnDeleteConfirm(lv_event_t * /*e*/)
{
    if (!SmartHome_Remove(s_detail)) return;
    CloseDeleteDialog();
    s_detail = -1;
    ShowList();
}

void OnUnbindConfirm(lv_event_t * /*e*/)
{
    if (!SmartHome_Bind(s_detail, "")) return;
    CloseDeleteDialog();
    ShowDetail(s_detail);
    if (s_ha_result) lv_label_set_text(s_ha_result, "已解除绑定，设备卡片已保留");
}

void OnDelete(lv_event_t * /*e*/)
{
    if (s_delete_overlay || !SmartHome_At(s_detail)) return;
    if (s_ha_keyboard) {
        lv_obj_add_flag(s_ha_keyboard, LV_OBJ_FLAG_HIDDEN);
        HiInputMethod_SetTextArea(s_ha_keyboard, nullptr);
        if (s_detail_body) lv_obj_set_height(s_detail_body, 590);
    }
    s_delete_overlay = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_delete_overlay);
    lv_obj_set_size(s_delete_overlay, kPanel, kPanel);
    lv_obj_set_style_bg_color(s_delete_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_delete_overlay, LV_OPA_70, 0);
    lv_obj_remove_flag(s_delete_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *dialog = lv_obj_create(s_delete_overlay);
    lv_obj_set_size(dialog, 600, 466);
    lv_obj_center(dialog);
    StyleBtn(dialog, kCard);
    lv_obj_set_style_radius(dialog, 28, 0);
    lv_obj_set_style_pad_all(dialog, 28, 0);
    lv_obj_remove_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = Label(dialog, "管理设备", &font_puhui_30_4, kText);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_t *hint = Label(dialog, "请选择删除卡片，或仅解除真实设备绑定", &font_puhui_20_4, kSub);
    lv_obj_set_pos(hint, 0, 48);
    auto option = [&](int y, const char *name, const char *description,
                      uint32_t color, lv_event_cb_t callback) {
        lv_obj_t *button = lv_button_create(dialog);
        lv_obj_set_size(button, 544, 112);
        lv_obj_set_pos(button, 0, y);
        StyleBtn(button, 0x282F3A);
        lv_obj_set_style_pad_all(button, 18, 0);
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *label = Label(button, name, &font_puhui_20_4, color);
        lv_obj_set_pos(label, 0, 0);
        lv_obj_t *desc = Label(button, description, &font_puhui_20_4, kSub);
        lv_obj_set_width(desc, 508);
        lv_obj_set_pos(desc, 0, 34);
    };
    option(90, "删除设备", "解除绑定，并从 Hi 家列表中移除设备卡片", kDanger, OnDeleteConfirm);
    option(214, "清空实体 ID 后保存", "解除绑定，保留 Hi 家中的设备卡片", kAccent, OnUnbindConfirm);
    lv_obj_t *cancel = lv_button_create(dialog);
    lv_obj_set_size(cancel, 544, 56);
    lv_obj_set_pos(cancel, 0, 350);
    StyleBtn(cancel, 0x353D49);
    lv_obj_add_event_cb(cancel, OnDeleteCancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_center(Label(cancel, "取消", &font_puhui_20_4, kText));
}

void OnHaFocus(lv_event_t *e)
{
    if (!s_ha_keyboard) return;
    HiInputMethod_SetTextArea(s_ha_keyboard, static_cast<lv_obj_t *>(lv_event_get_target(e)));
    lv_obj_remove_flag(s_ha_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (s_detail_body) lv_obj_set_height(s_detail_body, 320);
    lv_obj_scroll_to_view(static_cast<lv_obj_t *>(lv_event_get_target(e)), LV_ANIM_OFF);
}

void OnHaKeyboard(lv_event_t * /*e*/)
{
    lv_obj_add_flag(s_ha_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (s_detail_body) lv_obj_set_height(s_detail_body, 600);
}

void OnHaSave(lv_event_t * /*e*/)
{
    const char *entity = lv_textarea_get_text(s_ha_entity);
    if (entity[0] && !HaClient_ValidEntity(entity)) {
        lv_label_set_text(s_ha_result, "实体须为 light.xxx 或 switch.xxx");
        return;
    }
    if (entity[0] && !HaClient_Configure(lv_textarea_get_text(s_ha_url), lv_textarea_get_text(s_ha_token))) {
        lv_label_set_text(s_ha_result, "请填写局域网 http 地址和访问令牌");
        return;
    }
    const bool ok = SmartHome_Bind(s_detail, entity);
    lv_label_set_text(s_ha_result, ok ? "已保存，等待设备同步" : "绑定失败或实体已被使用");
}

void CloseQrScan(void)
{
    if (!s_qr_overlay) return;
    CameraScreen::SetPreviewObserver(nullptr);
    CameraScreen::StopExternalPreview();
    HaQrScanner_Stop();
    if (s_qr_timer) { lv_timer_delete(s_qr_timer); s_qr_timer = nullptr; }
    lv_obj_delete(s_qr_overlay);
    s_qr_overlay = nullptr;
    s_qr_hint = nullptr;
}

void OnQrCancel(lv_event_t * /*e*/) { CloseQrScan(); }

void OnQrPoll(lv_timer_t * /*timer*/)
{
    std::string token;
    const auto result = HaQrScanner_Poll(&token);
    if (result == kHaQrToken) {
        CloseQrScan();
        if (s_ha_token && lv_obj_is_valid(s_ha_token)) {
            lv_textarea_set_text(s_ha_token, token.c_str());
            lv_label_set_text(s_ha_result, "已扫码填写令牌，请保存设备连接");
        }
    } else if (result == kHaQrNotToken && s_qr_hint) {
        lv_label_set_text(s_qr_hint, "此码不是访问令牌，请换令牌二维码");
    } else if (result == kHaQrFailure) {
        CloseQrScan();
        if (s_ha_result) lv_label_set_text(s_ha_result, "二维码识别初始化失败，请重试");
    } else if (s_qr_overlay && !HaQrScanner_HasFrame() && lv_tick_elaps(s_qr_started) >= 12000) {
        CloseQrScan();
        if (s_ha_result) lv_label_set_text(s_ha_result, "摄像头未输出画面，请重新扫码");
    }
}

void OnQrScan(lv_event_t * /*e*/)
{
    if (s_qr_overlay) return;
    if (s_ha_keyboard) OnHaKeyboard(nullptr);
    CameraScreen::PreviewBuffer preview{};
    if (!CameraScreen::PreparePreviewBuffer(&preview) || !HaQrScanner_Start()) {
        lv_label_set_text(s_ha_result, "无法启动扫码，请重试");
        return;
    }
    s_qr_overlay = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_qr_overlay);
    lv_obj_set_size(s_qr_overlay, kPanel, kPanel);
    lv_obj_set_style_bg_color(s_qr_overlay, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_qr_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_qr_overlay, LV_OBJ_FLAG_SCROLLABLE);
    s_qr_hint = Label(s_qr_overlay, "对准访问令牌二维码", &font_puhui_20_4, kText);
    lv_obj_align(s_qr_hint, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *canvas = lv_canvas_create(s_qr_overlay);
    lv_canvas_set_buffer(canvas, preview.data, preview.width, preview.height, LV_COLOR_FORMAT_RGB888);
    lv_obj_set_pos(canvas, 0, 60);
    lv_obj_t *cancel = lv_button_create(s_qr_overlay);
    StyleBtn(cancel, kCard);
    lv_obj_set_size(cancel, 160, 48);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_event_cb(cancel, OnQrCancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_center(Label(cancel, "取消扫码", &font_puhui_20_4, kText));
    CameraScreen::SetPreviewObserver(HaQrScanner_Frame);
    if (CameraScreen::StartExternalPreview(canvas) != ESP_OK) {
        CloseQrScan();
        lv_label_set_text(s_ha_result, "摄像头启动失败，请重试");
        return;
    }
    s_qr_started = lv_tick_get();
    s_qr_timer = lv_timer_create(OnQrPoll, 150, nullptr);
}

void FillDetail(void)
{
    CloseDeleteDialog();
    CloseQrScan();
    if (s_detail_page == nullptr)
        return;
    if (s_ha_keyboard) {
        HiInputMethod_SetTextArea(s_ha_keyboard, nullptr);
        lv_obj_add_flag(s_ha_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clean(s_detail_page);
    const SmartDevice *dev = SmartHome_At(s_detail);
    if (dev == nullptr) {
        ShowList();
        return;
    }

    lv_obj_t *back = lv_button_create(s_detail_page);
    StyleBtn(back, kCard);
    lv_obj_set_size(back, 120, 56);
    lv_obj_set_pos(back, 24, 24);
    lv_obj_add_event_cb(back, OnDetailBack, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bt = Label(back, "返回", &font_puhui_20_4, kText);
    lv_obj_center(bt);

    lv_obj_t *title = Label(s_detail_page, dev->name, &font_puhui_30_4, kText);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 40, 32);

    lv_obj_t *body = lv_obj_create(s_detail_page);
    s_detail_body = body;
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 680, 600);
    lv_obj_set_pos(body, 20, 100);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 8, LV_PART_MAIN);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    char sub[64];
    std::snprintf(sub, sizeof(sub), "%s · %s", dev->room, SmartHome_StatusText(dev));
    s_detail_status = Label(body, sub, &font_puhui_20_4, kSub);

    lv_obj_t *power = lv_button_create(body);
    s_detail_power = power;
    const bool opened =
        (dev->kind == kSmartCurtain) ? dev->level > 0 : dev->on != 0;
    StyleBtn(power, opened ? kAccent : 0x2A3140);
    lv_obj_set_size(power, LV_PCT(100), 72);
    lv_obj_add_event_cb(power, OnDetailPower, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *pt = Label(power, opened ? "关闭" : "打开", &font_puhui_30_4, kText);
    s_detail_power_text = pt;
    lv_obj_center(pt);

    if (dev->kind != kSmartSwitch) {
        int lo = 0;
        int hi = 100;
        SmartHome_LevelRange(dev->kind, &lo, &hi);
        const char *hint = "亮度";
        if (dev->kind == kSmartCurtain)
            hint = "开合";
        else if (dev->kind == kSmartClimate)
            hint = "温度";
        else if (dev->kind == kSmartFan)
            hint = "风速";
        char value[32];
        if (dev->kind == kSmartClimate)
            std::snprintf(value, sizeof(value), "%s  %d°C", hint, dev->level);
        else
            std::snprintf(value, sizeof(value), "%s  %d%%", hint, dev->level);
        Label(body, value, &font_puhui_20_4, kText);
        lv_obj_t *slider = lv_slider_create(body);
        lv_obj_set_width(slider, LV_PCT(100));
        lv_obj_set_height(slider, 16);
        lv_slider_set_range(slider, lo, hi);
        lv_slider_set_value(slider, dev->level, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A3140), LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, lv_color_hex(0xF5C518), LV_PART_INDICATOR);
        lv_obj_add_event_cb(slider, OnDetailSlider, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(slider, OnDetailSlider, LV_EVENT_RELEASED, nullptr);
        screen_swipe_back_ignore(slider, true);
    }

    Label(body, "真实设备 · Home Assistant", &font_puhui_20_4, kSub);
    auto field = [body](const char *hint, const char *text, bool password) {
        lv_obj_t *ta = lv_textarea_create(body);HiInputMethod_Bind(ta,true);
        lv_obj_set_width(ta, LV_PCT(100));
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_placeholder_text(ta, hint);
        lv_textarea_set_text(ta, text);
        lv_textarea_set_password_mode(ta, password);
        lv_obj_set_style_text_font(ta, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_add_event_cb(ta, OnHaFocus, LV_EVENT_FOCUSED, nullptr);
        lv_obj_add_event_cb(ta, OnHaFocus, LV_EVENT_CLICKED, nullptr);
        return ta;
    };
    const std::string ha_url = HaClient_Url();
    s_ha_url = field("Home Assistant 地址", ha_url.empty() ? "http://192.168.31.123:8123" : ha_url.c_str(), false);
    s_ha_token = field("访问令牌（留空保留原令牌）", HaClient_Token().c_str(), true);
    lv_textarea_set_password_show_time(s_ha_token, 0);
    lv_textarea_set_max_length(s_ha_token, 1024);
    lv_obj_t *scan = lv_button_create(body);
    StyleBtn(scan, 0x2A3140);
    lv_obj_set_size(scan, LV_PCT(100), 56);
    lv_obj_add_event_cb(scan, OnQrScan, LV_EVENT_CLICKED, nullptr);
    lv_obj_center(Label(scan, "扫码填写访问令牌", &font_puhui_20_4, kText));
    s_ha_entity = field("light.xxx / switch.xxx（清空解除绑定）", dev->entity, false);
    lv_textarea_set_max_length(s_ha_entity, 95);
    lv_obj_t *save = lv_button_create(body);
    StyleBtn(save, kAccent);
    lv_obj_set_size(save, LV_PCT(100), 56);
    lv_obj_add_event_cb(save, OnHaSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_center(Label(save, "保存设备连接", &font_puhui_20_4, kText));
    s_ha_result = Label(body, "手动打开加入回家预设，手动关闭移出", &font_puhui_20_4, kSub);

    Label(body, "房间", &font_puhui_20_4, kSub);
    lv_obj_t *rooms = lv_obj_create(body);
    lv_obj_remove_style_all(rooms);
    lv_obj_set_width(rooms, LV_PCT(100));
    lv_obj_set_height(rooms, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rooms, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(rooms, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(rooms, 10, LV_PART_MAIN);
    lv_obj_remove_flag(rooms, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 1; i < kRoomCount; ++i) {
        lv_obj_t *chip = lv_button_create(rooms);
        const bool cur = std::strcmp(dev->room, kRooms[i]) == 0;
        StyleBtn(chip, cur ? kAccent : 0x2A3140);
        lv_obj_set_size(chip, 120, 48);
        lv_obj_add_event_cb(chip, OnPickRoom, LV_EVENT_CLICKED,
                            const_cast<char *>(kRooms[i]));
        lv_obj_t *ct = Label(chip, kRooms[i], &font_puhui_20_4, kText);
        lv_obj_center(ct);
    }

    Label(body, "名称", &font_puhui_20_4, kSub);
    static const char *kNames[] = {
        "台灯Pro", "吸顶灯", "床头灯", "筒灯", "窗帘", "空调", "插座", "加湿器",
    };
    lv_obj_t *names = lv_obj_create(body);
    lv_obj_remove_style_all(names);
    lv_obj_set_width(names, LV_PCT(100));
    lv_obj_set_height(names, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(names, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(names, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(names, 10, LV_PART_MAIN);
    lv_obj_remove_flag(names, LV_OBJ_FLAG_SCROLLABLE);
    for (const char *nm : kNames) {
        lv_obj_t *chip = lv_button_create(names);
        const bool cur = std::strcmp(dev->name, nm) == 0;
        StyleBtn(chip, cur ? 0x3B82F6 : 0x2A3140);
        lv_obj_set_size(chip, 140, 48);
        lv_obj_add_event_cb(chip, OnPickName, LV_EVENT_CLICKED,
                            const_cast<char *>(nm));
        lv_obj_t *nt = Label(chip, nm, &font_puhui_20_4, kText);
        lv_obj_center(nt);
    }

    lv_obj_t *pin_row = lv_obj_create(body);
    lv_obj_remove_style_all(pin_row);
    lv_obj_set_width(pin_row, LV_PCT(100));
    lv_obj_set_height(pin_row, 64);
    lv_obj_set_style_bg_color(pin_row, lv_color_hex(kCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pin_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(pin_row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(pin_row, 16, LV_PART_MAIN);
    lv_obj_remove_flag(pin_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *pin_lbl = Label(pin_row, "显示在桌面", &font_puhui_20_4, kText);
    lv_obj_align(pin_lbl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *sw = lv_switch_create(pin_row);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -8, 0);
    if (dev->pinned)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(kAccent),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, OnPin, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *del = lv_button_create(body);
    StyleBtn(del, kDanger);
    lv_obj_set_size(del, LV_PCT(100), 64);
    lv_obj_add_event_cb(del, OnDelete, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *dt = Label(del, "删除设备", &font_puhui_20_4, kText);
    lv_obj_center(dt);
}

void ShowList(void)
{
    CloseDeleteDialog();
    CloseQrScan();
    if (s_ha_keyboard) {
        HiInputMethod_SetTextArea(s_ha_keyboard, nullptr);
        lv_obj_add_flag(s_ha_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_list_page != nullptr)
        lv_obj_remove_flag(s_list_page, LV_OBJ_FLAG_HIDDEN);
    if (s_detail_page != nullptr)
        lv_obj_add_flag(s_detail_page, LV_OBJ_FLAG_HIDDEN);
    RebuildList();
}

void ShowDetail(int index)
{
    s_detail = index;
    if (s_list_page != nullptr)
        lv_obj_add_flag(s_list_page, LV_OBJ_FLAG_HIDDEN);
    if (s_detail_page != nullptr)
        lv_obj_remove_flag(s_detail_page, LV_OBJ_FLAG_HIDDEN);
    FillDetail();
}

void OnAdd(lv_event_t * /*e*/)
{
    if (s_ha_keyboard) {
        HiInputMethod_SetTextArea(s_ha_keyboard, nullptr);
        lv_obj_add_flag(s_ha_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (SmartHome_Count() >= kSmartMaxDevices)
        return;
    s_detail = -1;
    if (s_list_page != nullptr)
        lv_obj_add_flag(s_list_page, LV_OBJ_FLAG_HIDDEN);
    if (s_detail_page == nullptr)
        return;
    lv_obj_remove_flag(s_detail_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_detail_page);
    lv_obj_t *back = lv_button_create(s_detail_page);
    StyleBtn(back, kCard);
    lv_obj_set_size(back, 120, 56);
    lv_obj_set_pos(back, 24, 24);
    lv_obj_add_event_cb(back, OnDetailBack, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bt = Label(back, "返回", &font_puhui_20_4, kText);
    lv_obj_center(bt);
    lv_obj_t *title = Label(s_detail_page, "添加设备", &font_puhui_30_4, kText);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 40, 32);
    struct Item {
        const char *name;
        int kind;
    };
    static const Item kItems[] = {
        {"灯", kSmartLight},
        {"开关", kSmartSwitch},
        {"窗帘", kSmartCurtain},
        {"空调", kSmartClimate},
        {"风扇", kSmartFan},
    };
    for (int i = 0; i < static_cast<int>(sizeof(kItems) / sizeof(kItems[0])); ++i) {
        lv_obj_t *btn = lv_button_create(s_detail_page);
        StyleBtn(btn, kCard);
        lv_obj_set_size(btn, 640, 88);
        lv_obj_set_pos(btn, 40, 120 + i * 108);
        lv_obj_add_event_cb(btn, OnAddKind, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(
                                static_cast<intptr_t>(kItems[i].kind)));
        lv_obj_t *t = Label(btn, kItems[i].name, &font_puhui_30_4, kText);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 24, 0);
    }
}

void OnUnloaded(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_screen)
        return;
    CloseDeleteDialog();
    CloseQrScan();
    if (s_build_timer) { lv_timer_delete(s_build_timer); s_build_timer = nullptr; }
    s_screen = nullptr;
    s_list_page = nullptr;
    s_detail_page = nullptr;
    s_scroll = nullptr;
    s_count = nullptr;
    s_ha_keyboard = nullptr;
    s_detail_body = nullptr;
    if (s_state_timer) { lv_timer_delete(s_state_timer); s_state_timer = nullptr; }
}

}  // namespace

static void BuildHomeAppContents();

lv_obj_t *HomeAppScreen::CreateStatic()
{
    s_filter = 0;
    s_detail = -1;

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    screen_kill_theme_chrome(scr);
    lv_obj_set_size(scr, kPanel, kPanel);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Load the dashboard shell first. The caller keeps the outgoing page
     * alive until this screen is active; defer the large widget tree until
     * that page's queued deletion has run, reducing peak LVGL heap pressure. */
    lv_obj_add_event_cb(scr, OnUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    s_build_timer = lv_timer_create([](lv_timer_t *timer) {
        lv_timer_delete(timer);
        if (s_build_timer == timer) s_build_timer = nullptr;
        if (s_screen == nullptr || lv_screen_active() != s_screen)
            return;
        SmartHome_EnsureLoaded();
        BuildHomeAppContents();
    }, 80, nullptr);
    if (s_build_timer != nullptr)
        lv_timer_set_repeat_count(s_build_timer, 1);
    else {
        /* Keep the screen usable if LVGL cannot allocate a timer. */
        SmartHome_EnsureLoaded();
        BuildHomeAppContents();
    }
    return scr;
}

static void BuildHomeAppContents()
{
    lv_obj_t *scr = s_screen;
    if (scr == nullptr)
        return;

    s_list_page = lv_obj_create(scr);
    lv_obj_remove_style_all(s_list_page);
    lv_obj_set_size(s_list_page, kPanel, kPanel);
    lv_obj_set_style_bg_opa(s_list_page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(s_list_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(s_list_page);
    StyleBtn(back, kCard);
    lv_obj_set_size(back, 88, 56);
    lv_obj_set_pos(back, 20, 20);
    lv_obj_add_event_cb(back, OnBackHome, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *btext = Label(back, "返回", &font_puhui_20_4, kText);
    lv_obj_center(btext);

    lv_obj_t *title = Label(s_list_page, "Hi 家", &font_puhui_30_4, kText);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);
    s_count = Label(s_list_page, "", &font_puhui_20_4, kSub);
    lv_obj_align(s_count, LV_ALIGN_TOP_MID, 0, 68);

    lv_obj_t *add = lv_button_create(s_list_page);
    StyleBtn(add, kAccent);
    lv_obj_set_size(add, 88, 56);
    lv_obj_align(add, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_obj_add_event_cb(add, OnAdd, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *at = Label(add, "添加", &font_puhui_20_4, kText);
    lv_obj_center(at);

    lv_obj_t *filters = lv_obj_create(s_list_page);
    lv_obj_remove_style_all(filters);
    lv_obj_set_size(filters, 680, 56);
    lv_obj_set_pos(filters, 20, 108);
    lv_obj_set_flex_flow(filters, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(filters, 8, LV_PART_MAIN);
    lv_obj_add_flag(filters, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(filters, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(filters, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < kRoomCount; ++i) {
        lv_obj_t *chip = lv_button_create(filters);
        StyleBtn(chip, i == s_filter ? kAccent : kCard);
        lv_obj_set_size(chip, 100, 48);
        lv_obj_set_user_data(chip, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(chip, OnFilter, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_t *ct = Label(chip, kRooms[i], &font_puhui_20_4, kText);
        lv_obj_center(ct);
    }

    s_scroll = lv_obj_create(s_list_page);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, 680, 520);
    lv_obj_set_pos(s_scroll, 20, 176);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_scroll, 12, LV_PART_MAIN);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_OFF);

    s_detail_page = lv_obj_create(scr);
    lv_obj_remove_style_all(s_detail_page);
    lv_obj_set_size(s_detail_page, kPanel, kPanel);
    lv_obj_set_style_bg_color(s_detail_page, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_detail_page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_detail_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_detail_page, LV_OBJ_FLAG_SCROLLABLE);

    s_ha_keyboard = HiInputMethod_Create(scr);
    lv_obj_set_size(s_ha_keyboard, kPanel, 280);
    lv_obj_align(s_ha_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_ha_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ha_keyboard, OnHaKeyboard, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_ha_keyboard, OnHaKeyboard, LV_EVENT_CANCEL, nullptr);
    s_state_timer = lv_timer_create([](lv_timer_t *) {
        if (s_list_page && !lv_obj_has_flag(s_list_page, LV_OBJ_FLAG_HIDDEN)) RebuildList();
        if (s_detail >= 0 && s_detail_page && !lv_obj_has_flag(s_detail_page, LV_OBJ_FLAG_HIDDEN)) {
            const SmartDevice *dev = SmartHome_At(s_detail);
            if (dev && s_detail_status) {
                char status[96];
                std::snprintf(status, sizeof(status), "%s · %s", dev->room, SmartHome_StatusText(dev));
                lv_label_set_text(s_detail_status, status);
                lv_obj_set_style_bg_color(s_detail_power, lv_color_hex(dev->on ? kAccent : 0x2A3140), LV_PART_MAIN);
                lv_label_set_text(s_detail_power_text, dev->on ? "关闭" : "打开");
            }
        }
    }, 2000, nullptr);
    RebuildList();
    screen_attach_swipe_back(scr, OnSwipeBack);
}

lv_obj_t *HomeAppScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void HomeAppScreen::LifecycleCallback(screen_lifecycle_event_t /*event*/)
{
}
