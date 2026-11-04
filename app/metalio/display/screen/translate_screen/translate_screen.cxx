// SPDX-License-Identifier: Apache-2.0
// Translation shell retained; recording and remote service calls removed.
#include "translate_screen.h"
#include "i18n.h"
#include "screen_util.h"
#include "home_screen/home_screen.h"
#include <cstdint>
#include <string>
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);
namespace {
constexpr int32_t kPanelW = 720;
constexpr int32_t kPanelH = 720;
constexpr int32_t kHeaderH = 88;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kFooterH = 108;
constexpr int32_t kLangRowH = 64;
constexpr int32_t kStatusH = 36;
constexpr int32_t kBodyH = kPanelH - kHeaderH - kFooterH;
constexpr int32_t kHeaderSidePad = 8;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorHeaderBg = 0x12151C;
constexpr uint32_t kColorDivider = 0x2A2F3A;
constexpr uint32_t kColorCard = 0x1A1F2A;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorHintText = 0x9AA3B2;
constexpr uint32_t kColorTrans = 0x93C5FD;
constexpr uint32_t kColorBtnIdle = 0x2563EB;
constexpr uint32_t kColorBtnActive = 0xDC2626;
constexpr uint32_t kColorBtnBusy = 0x4B5563;

// Sonicloud 语种表 lanid 0–99（fromlan/tolan 不支持 >=100）
// https://open.sinicloud.com/documents/webapi/stream-asr/#语种编号列表lanid
struct LangEntry {
    int lanid;
    const char* code;
    const char* name_zh;
};

constexpr LangEntry kLangs[] = {
#include "sinicloud_lang_table.inc"
};
constexpr size_t kLangCount = sizeof(kLangs) / sizeof(kLangs[0]);
constexpr int kDefaultFromIdx = 0;  // zh
constexpr int kDefaultToIdx = 1;    // en

lv_obj_t *s_screen=nullptr,*s_from_dd=nullptr,*s_to_dd=nullptr,*s_status_lbl=nullptr;
lv_obj_t *s_source_lbl=nullptr,*s_trans_lbl=nullptr,*s_action_btn=nullptr,*s_action_lbl=nullptr;
std::string s_lang_options;
void refresh_result_ui_locked(){
    if(s_source_lbl)lv_label_set_text(s_source_lbl,I18n::T("识别原文将显示在这里"));
    if(s_trans_lbl)lv_label_set_text(s_trans_lbl,I18n::T("译文将显示在这里"));
}
void on_action_clicked(lv_event_t*){if(s_status_lbl)lv_label_set_text(s_status_lbl,I18n::T("翻译服务尚未接入"));}
void on_swipe_back(){HomeScreen::SwitchToHome();}
void on_back_clicked(lv_event_t*){on_swipe_back();}
void on_screen_unloaded(lv_event_t*e){
    if(lv_event_get_target_obj(e)!=s_screen)return;
    s_screen=s_from_dd=s_to_dd=s_status_lbl=s_source_lbl=s_trans_lbl=s_action_btn=s_action_lbl=nullptr;
}
void build_lang_options() {
    if (!s_lang_options.empty()) {
        return;
    }
    s_lang_options.reserve(kLangCount * 18);
    for (size_t i = 0; i < kLangCount; ++i) {
        if (i > 0) {
            s_lang_options.push_back('\n');
        }
        s_lang_options.append(kLangs[i].name_zh);
        s_lang_options.append(" (");
        s_lang_options.append(kLangs[i].code);
        s_lang_options.push_back(')');
    }
}
void on_dropdown_ready(lv_event_t* e) {
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
    lv_obj_set_style_text_font(list, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 10, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 360, LV_PART_MAIN);
    screen_swipe_back_ignore(list, true);
}

void style_dropdown(lv_obj_t* dd) {
    lv_obj_set_style_radius(dd, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dd, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, lv_color_hex(0x4B5563), LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, &font_puhui_20_4, LV_PART_MAIN);
    lv_dropdown_set_symbol(dd, nullptr);
    lv_obj_set_style_text_font(dd, &font_awesome_20_4, LV_PART_INDICATOR);
    lv_obj_add_event_cb(dd, on_dropdown_ready, LV_EVENT_READY, nullptr);
    screen_swipe_back_ignore(dd, true);
}

lv_obj_t* make_text_card(lv_obj_t* parent, const char* title,
                         lv_obj_t** out_label, uint32_t title_color) {
    lv_obj_t* card = lv_obj_create(parent);
    screen_strip_obj_chrome(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(card, true);

    lv_obj_t* title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(title_color),
                                LV_PART_MAIN);
    screen_make_input_passive(title_lbl);

    lv_obj_t* body = lv_label_create(card);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(body, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    screen_make_input_passive(body);
    *out_label = body;
    return card;
}

void build_header(lv_obj_t* parent) {
    lv_obj_t* top = lv_obj_create(parent);
    screen_strip_obj_chrome(top);
    lv_obj_set_size(top, kPanelW, kHeaderH);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(top, lv_color_hex(kColorHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* divider = lv_obj_create(top);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider),
                              LV_PART_MAIN);
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
    lv_obj_add_event_cb(back, on_back_clicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(top);
    lv_label_set_text(title, I18n::T("翻译"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, kHeaderSidePad + kBackBtnSize + 8,
                 0);
    screen_make_input_passive(title);
}

void build_body(lv_obj_t* parent) {
    build_lang_options();

    lv_obj_t* body = lv_obj_create(parent);
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kPanelW, kBodyH);
    lv_obj_set_pos(body, 0, kHeaderH);
    lv_obj_set_style_bg_color(body, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_right(body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_top(body, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(body, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, LV_PART_MAIN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lang_row = lv_obj_create(body);
    screen_strip_obj_chrome(lang_row);
    lv_obj_set_size(lang_row, LV_PCT(100), kLangRowH);
    lv_obj_set_style_bg_opa(lang_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(lang_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lang_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(lang_row, LV_OBJ_FLAG_SCROLLABLE);

    s_from_dd = lv_dropdown_create(lang_row);
    lv_obj_set_size(s_from_dd, 280, 52);
    style_dropdown(s_from_dd);
    lv_dropdown_set_options(s_from_dd, s_lang_options.c_str());
    lv_dropdown_set_selected(s_from_dd, kDefaultFromIdx);

    lv_obj_t* swap = lv_button_create(lang_row);lv_obj_remove_style_all(swap);lv_obj_set_size(swap,52,52);
    static const lv_point_precise_t right[]={{4,10},{28,10},{22,4},{28,10},{22,16}};
    static const lv_point_precise_t left[]={{28,28},{4,28},{10,22},{4,28},{10,34}};
    const lv_point_precise_t* points[]={right,left};
    for(auto* p:points){auto* line=lv_line_create(swap);lv_line_set_points(line,p,5);lv_obj_set_pos(line,8,6);
        lv_obj_set_style_line_width(line,3,0);lv_obj_set_style_line_color(line,lv_color_hex(kColorHintText),0);screen_make_input_passive(line);}
    lv_obj_add_event_cb(swap,[](lv_event_t*){if(s_from_dd&&s_to_dd){auto from=lv_dropdown_get_selected(s_from_dd),to=lv_dropdown_get_selected(s_to_dd);lv_dropdown_set_selected(s_from_dd,to);lv_dropdown_set_selected(s_to_dd,from);}},LV_EVENT_CLICKED,nullptr);

    s_to_dd = lv_dropdown_create(lang_row);
    lv_obj_set_size(s_to_dd, 280, 52);
    style_dropdown(s_to_dd);
    lv_dropdown_set_options(s_to_dd, s_lang_options.c_str());
    lv_dropdown_set_selected(s_to_dd, kDefaultToIdx);

    s_status_lbl = lv_label_create(body);
    lv_obj_set_width(s_status_lbl, LV_PCT(100));
    lv_obj_set_height(s_status_lbl, kStatusH);
    lv_label_set_text(s_status_lbl, I18n::T("翻译服务尚未接入"));
    lv_obj_set_style_text_font(s_status_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    screen_make_input_passive(s_status_lbl);

    make_text_card(body, I18n::T("识别原文"), &s_source_lbl, kColorHintText);
    make_text_card(body, I18n::T("译文"), &s_trans_lbl, kColorTrans);
    refresh_result_ui_locked();
}

void build_footer(lv_obj_t* parent) {
    lv_obj_t* footer = lv_obj_create(parent);
    screen_strip_obj_chrome(footer);
    lv_obj_set_size(footer, kPanelW, kFooterH);
    lv_obj_set_pos(footer, 0, kPanelH - kFooterH);
    lv_obj_set_style_bg_color(footer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int32_t kBtnW = 400;
    constexpr int32_t kBtnH = 72;
    s_action_btn = lv_button_create(footer);
    lv_obj_set_size(s_action_btn, kBtnW, kBtnH);
    lv_obj_align(s_action_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_action_btn, kBtnH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kColorBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_action_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_action_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_action_btn, 0, LV_PART_MAIN);

    s_action_lbl = lv_label_create(s_action_btn);
    lv_label_set_text(s_action_lbl, I18n::T("实时翻译"));
    lv_obj_set_style_text_color(s_action_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_action_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(s_action_lbl);

    lv_obj_add_event_cb(s_action_btn, on_action_clicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(s_action_btn, true);
}

}
lv_obj_t* TranslateScreen::CreateStatic(){
    auto*scr=lv_obj_create(nullptr);s_screen=scr;screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr,kPanelW,kPanelH);lv_obj_set_style_bg_color(scr,lv_color_hex(kColorBg),LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr,LV_OPA_COVER,LV_PART_MAIN);lv_obj_remove_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    build_header(scr);build_body(scr);build_footer(scr);
    lv_obj_add_event_cb(scr,on_screen_unloaded,LV_EVENT_SCREEN_UNLOADED,nullptr);
    screen_attach_swipe_back(scr,on_swipe_back);return scr;
}
lv_obj_t* TranslateScreen::Create(){root_=CreateStatic();return root_;}
void TranslateScreen::LifecycleCallback(screen_lifecycle_event_t){}
