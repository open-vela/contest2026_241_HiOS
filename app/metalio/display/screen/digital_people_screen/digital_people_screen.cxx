/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * DigitalPeopleScreen — ported from MetalioClaw4
 * main/display/screen/digital_people_screen/digital_people_screen.cc.
 *
 * Adaptations for NuttX/openvela:
 *   - esp_log.h               -> esp_log_shim.h
 *   - lv_eaf.h / lv_eaf_*     -> portable EAF player with NuttX allocation.
 *   - HomeScreen::Create()    -> HomeScreen::CreateStatic()
 *   - application.h / SdCardManager.hpp / screen_util.h / i18n.h
 *                              already ported.
 *
 * The LVGL widget tree, layout constants, bubble styling, long-press
 * settings gesture, activation-block dialog and emotion resource checks
 * are preserved verbatim from the original.
 */

#include "digital_people_screen.h"
#include "lv_eaf.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "xiaobai_resources.h"
#include <mutex>
#include <atomic>
#include "standby_screen/standby_screen.h"
#include "digital_people_prefs.h"
#include "digital_people_settings_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "application.h"
#include "device_state.h"
#include "settings.h"
#include "SdCardManager.hpp"
#include "home_screen/home_screen.h"
#include "camera_screen/camera_screen.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_chat_cjk_30);

namespace {

constexpr const char* TAG = "DigitalPeopleScreen";

constexpr int32_t  kPanelSize    = 720;
constexpr uint32_t kColorBg      = 0x000000;          // 纯黑背景

// 表情资源目录：完整路径 = kEmotionDir + 大类名 + DigitalPeoplePrefs::GetEmotionExt()
//   例: "S:/sdcard/System/emotion/loving.sjpg"
// 大类名来自 LVAdapterDisplay::SetEmotion 里的 GetEmoteCategory()，
// 取值范围被收敛到 6 个：crying / happy / loving / neutral / surprised /
// thinking。所有资源都放在 SD 卡的 System/emotion/ 目录下。
// 格式（.eaf / .sjpg）与 EAF 帧间隔由 NVS（DigitalPeoplePrefs）决定。
constexpr const char* kEmotionDir = "S:/sdcard/System/emotion/";
constexpr const char* kEmotionPosixDir = "/sdcard/System/emotion/";
constexpr const char* kDefaultEmotion = "neutral";
constexpr uint32_t kLongPressSettingsMs = 5000;
constexpr int32_t kLongPressMoveSlopPx = 24;

// 端侧必须存在的 6 个大类资源；缺任意一个都视为资源包未就绪。
constexpr const char* kRequiredEmotions[] = {
    "crying", "happy", "loving", "neutral", "surprised", "thinking",
};
constexpr size_t kRequiredEmotionCount =
    sizeof(kRequiredEmotions) / sizeof(kRequiredEmotions[0]);

// 记录"当前应该播放哪一张"——LVAdapterDisplay 在屏幕没进前台时也可以
// 调用 SetEmotion 预置；下次 Create() 拿这个值拼路径。
// s_emotion_path_buf 是 lv_eaf_set_src / lv_image_set_src 传入的路径缓冲，
// 必须保证在调用之间一直有效，所以放在 namespace 静态。
constexpr size_t kEmotionPathBufSize = 64;
char s_current_emotion[24] = "neutral";
std::mutex s_emotion_mutex;
char s_requested_emotion[24]="neutral";
unsigned s_requested_revision=0,s_applied_revision=0;
lv_timer_t*s_emotion_ui_timer=nullptr;
uint32_t s_speech_started_ms=0;
DeviceState s_expression_state=kDeviceStateUnknown;
unsigned SnapshotEmotion(char*out){std::lock_guard<std::mutex>l(s_emotion_mutex);strcpy(out,s_requested_emotion);return s_requested_revision;}
void Motion(void*obj,int32_t y){lv_obj_set_style_translate_y((lv_obj_t*)obj,y,0);}
void StartMotion(lv_obj_t*widget,const char*category){lv_anim_delete(widget,Motion);lv_obj_set_style_translate_y(widget,0,0);
 int amplitude=!strcmp(category,"laughing")?6:!strcmp(category,"surprised")?5:3;int duration=!strcmp(category,"laughing")?550:!strcmp(category,"thinking")?1800:1300;
 lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,widget);lv_anim_set_exec_cb(&a,Motion);lv_anim_set_values(&a,-amplitude,amplitude);lv_anim_set_duration(&a,duration);lv_anim_set_reverse_duration(&a,duration);lv_anim_set_repeat_count(&a,LV_ANIM_REPEAT_INFINITE);lv_anim_set_path_cb(&a,lv_anim_path_ease_in_out);lv_anim_start(&a);
}
char s_emotion_path_buf[kEmotionPathBufSize];

const char* EmotionCategoryName(const char* category) {
    return (category != nullptr && category[0] != '\0') ? category
                                                        : kDefaultEmotion;
}

const char* PngPose(const char*category){
 if(!strcmp(category,"thinking"))return "sleepy";if(!strcmp(category,"crying"))return "sad";
 if(!strcmp(category,"laughing")||!strcmp(category,"surprised")||!strcmp(category,"angry"))return category;return "happy";
}
const char* BuildEmotionPath(const char* category) {
    Settings prefs("ui");
    if(prefs.GetBool("dp_custom",false))
        std::snprintf(s_emotion_path_buf,sizeof(s_emotion_path_buf),"%s%s%s",kEmotionDir,EmotionCategoryName(category),DigitalPeoplePrefs::GetEmotionExt());
    else std::snprintf(s_emotion_path_buf,sizeof(s_emotion_path_buf),"%sxiaobai_%s.png",kEmotionDir,PngPose(EmotionCategoryName(category)));
    return s_emotion_path_buf;
}
bool EmotionUsesEaf(){Settings prefs("ui");return prefs.GetBool("dp_custom",false)&&DigitalPeoplePrefs::UsesEaf();}

// ---------------------------------------------------------------------------
// Emotion animation uses the same EAF widget API as upstream.
// ---------------------------------------------------------------------------
void SetEmotionSrc(lv_obj_t *widget, const char *category) {
    if (!widget) return;
    const char *path = BuildEmotionPath(category);
    if (EmotionUsesEaf()) {
        lv_eaf_set_src(widget, path);
        lv_eaf_set_frame_delay(widget, DigitalPeoplePrefs::GetFrameDelayMs());
        if(lv_eaf_get_total_frames(widget)<=1)lv_eaf_pause(widget);
    } else {
        // These packaged PNGs are immutable. Keep their decoded cache during
        // speech instead of decoding the same face from SD on every change.
        lv_image_set_src(widget, path);
    }
    StartMotion(widget,category);
}
lv_obj_t *CreateEmotionWidget(lv_obj_t *parent) {
    return EmotionUsesEaf() ? lv_eaf_create(parent) : lv_image_create(parent);
}

// ---------------------------------------------------------------------------
// 对话气泡视觉参数
//
//   ┌─────────────────────────────────────────┐ 0
//   │ [back]                                  │ ← back btn 占 (16,16) 60x60
//   │   ╭─ system bubble ─╮                   │ ← top-left，y 起点 kSysBubbleTop
//   │   ╰────────────────╯                    │
//   │                                         │
//   │            (gif 动画)                    │
//   │                                         │
//   │        ╭─ user bubble ─╮                │ ← bottom-center
//   │        ╰───────────────╯                │
//   └─────────────────────────────────────────┘ 720
//
//   - 气泡背景：白色 30% 不透明，白色 2px 边框 +
//     圆角。
//   - 文本：深色 puhui_30。
//   - 文本宽度：根据内容 + padding 计算，封顶 max_w 后换行（LV_LABEL_LONG_WRAP）。
//   - 屏幕未在前台时静态指针都被清空，所有显示接口直接 no-op。
// ---------------------------------------------------------------------------
constexpr int32_t  kBubbleRadius     = 18;
constexpr int32_t  kBubblePadX       = 18;
constexpr int32_t  kBubblePadY       = 14;
constexpr int32_t  kBubbleBorder     = 2;
constexpr int32_t  kSideMargin       = 16;
constexpr int32_t  kSysBubbleTop     = 24;            // 顶部安全间距
constexpr int32_t  kUserBubbleBottom = 112;
constexpr int32_t  kSysBubbleMaxW    = kPanelSize - kSideMargin * 2;       // 688
constexpr int32_t  kUserBubbleMaxW   = kPanelSize - kSideMargin * 2;       // 688

constexpr uint32_t kColorBubbleBg     = 0xFFFFFF;
constexpr uint32_t kColorBubbleBorder = 0xFFFFFF;
constexpr uint32_t kColorBubbleText   = 0x1F2937;
constexpr uint32_t kColorHintText     = 0xC8C9CC;
constexpr lv_opa_t kBubbleBgOpa       = LV_OPA_30;

struct UiState {
    lv_obj_t* screen        = nullptr;
    lv_obj_t* eaf           = nullptr;
    lv_obj_t* hint_label    = nullptr;
    lv_obj_t* system_bubble = nullptr;
    lv_obj_t* system_label  = nullptr;
    lv_obj_t* user_bubble   = nullptr;
    lv_obj_t* user_label    = nullptr;
    lv_obj_t* look_and_see_btn = nullptr;
    lv_obj_t* voice_btn = nullptr;
    lv_timer_t* wake_arm_timer = nullptr;
    bool assets_ready=false;
};

UiState s_ui;

void pause_emotion_widget() {
    if (EmotionUsesEaf() && s_ui.eaf) lv_eaf_pause(s_ui.eaf);
    if(s_ui.eaf)lv_anim_delete(s_ui.eaf,Motion);
}

lv_timer_t* s_activation_guard_timer = nullptr;
lv_timer_t* s_long_press_timer = nullptr;
bool s_long_press_armed = false;
int16_t s_long_press_start_x = 0;
int16_t s_long_press_start_y = 0;

// 未激活拦截：全屏模态弹窗，不可关闭，仅能通过返回键离开。
struct ActivationBlockedDialogUi {
    lv_obj_t* mask = nullptr;
};
ActivationBlockedDialogUi s_activation_dlg;
bool s_activation_blocked = false;
bool s_activation_dialog_shows_code = false;
std::atomic<bool> s_voice_ui_held{false};

void cancel_long_press_timer() {
    if (s_long_press_timer != nullptr) {
        lv_timer_delete(s_long_press_timer);
        s_long_press_timer = nullptr;
    }
    s_long_press_armed = false;
}

void digital_people_page_leave();
void open_digital_people_settings();

void on_long_press_settings_timer(lv_timer_t* /*timer*/) {
    s_long_press_timer = nullptr;
    s_long_press_armed = false;
    if (s_ui.screen == nullptr || s_activation_blocked) {
        return;
    }
    ESP_LOGI(TAG, "long-press 5s -> digital people settings");
    open_digital_people_settings();
}

void open_digital_people_settings() {
    cancel_long_press_timer();
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    digital_people_page_leave();

    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* settings = DigitalPeopleSettingsScreen::CreateStatic();
    screen_load_safe(settings);
    if (old_scr != nullptr && old_scr != settings) {
        lv_obj_delete_async(old_scr);
    }
}

void OnLongPressPressed(lv_event_t* e) {
    if (lv_event_get_target(e) == s_ui.look_and_see_btn) {
        lv_event_stop_bubbling(e);
        return;
    }
    if (s_activation_blocked || s_ui.screen == nullptr) {
        return;
    }
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    cancel_long_press_timer();
    s_long_press_armed = true;
    s_long_press_start_x = static_cast<int16_t>(p.x);
    s_long_press_start_y = static_cast<int16_t>(p.y);
    s_long_press_timer =
        lv_timer_create(on_long_press_settings_timer, kLongPressSettingsMs,
                        nullptr);
    lv_timer_set_repeat_count(s_long_press_timer, 1);
}

void OnLongPressPressing(lv_event_t* e) {
    if (lv_event_get_target(e) == s_ui.look_and_see_btn) {
        lv_event_stop_bubbling(e);
        return;
    }
    if (!s_long_press_armed) {
        return;
    }
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int dx = p.x - s_long_press_start_x;
    const int dy = p.y - s_long_press_start_y;
    if (dx > kLongPressMoveSlopPx || dx < -kLongPressMoveSlopPx ||
        dy > kLongPressMoveSlopPx || dy < -kLongPressMoveSlopPx) {
        cancel_long_press_timer();
    }
}

void OnLongPressReleased(lv_event_t* e) {
    if (lv_event_get_target(e) == s_ui.look_and_see_btn) {
        lv_event_stop_bubbling(e);
        return;
    }
    cancel_long_press_timer();
}

void OnLookAndSeeButtonClicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (s_activation_blocked) return;
    if (!CameraScreen::OpenLookAndSee(true)) {
        ESP_LOGW(TAG, "could not open look-and-see camera from digital people");
    }
}

void OnLookAndSeeButtonPress(lv_event_t* e) {
    lv_event_stop_bubbling(e);
}

void schedule_voice_ui_start() {
    if (s_voice_ui_held) {
        return;
    }
    s_voice_ui_held = true;
    Application::GetInstance().RequestVoiceUiDesired(true);
    if(s_ui.wake_arm_timer)lv_timer_delete(s_ui.wake_arm_timer);
    s_ui.wake_arm_timer=lv_timer_create([](lv_timer_t*){
        s_ui.wake_arm_timer=nullptr;
        if(s_voice_ui_held&&DigitalPeopleScreen::IsActive()) {
            // Reassert after the previous page's asynchronous UNLOAD, which
            // may otherwise win the first voice-UI request during navigation.
            Application::GetInstance().RequestVoiceUiDesired(true);
            Application::GetInstance().ArmAssistantWakeWord();
        }
    },120,nullptr);
    if(s_ui.wake_arm_timer)lv_timer_set_repeat_count(s_ui.wake_arm_timer,1);
}

void schedule_voice_ui_stop() {
    if (!s_voice_ui_held) {
        return;
    }
    s_voice_ui_held = false;
    if (!CameraScreen::IsLookAndSeeMode())
        Application::GetInstance().RequestVoiceUiDesired(false);
}

void digital_people_page_leave() {
    if(s_ui.wake_arm_timer){lv_timer_delete(s_ui.wake_arm_timer);s_ui.wake_arm_timer=nullptr;}
    pause_emotion_widget();
    schedule_voice_ui_stop();
}

const lv_font_t* bubble_font() { return &font_chat_cjk_30; }

bool EmotionFileExists(const char* name) {
    char path[96];
    Settings prefs("ui");
    if(prefs.GetBool("dp_custom",false))std::snprintf(path,sizeof(path),"%s%s%s",kEmotionPosixDir,name,DigitalPeoplePrefs::GetEmotionExt());
    else std::snprintf(path, sizeof(path), "%sxiaobai_%s.png",kEmotionPosixDir,PngPose(name));
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void LogMissingEmotionFiles() {
    const char* ext = DigitalPeoplePrefs::GetEmotionExt();
    for (size_t i = 0; i < kRequiredEmotionCount; ++i) {
        if (!EmotionFileExists(kRequiredEmotions[i])) {
            ESP_LOGW(TAG, "missing emotion file: %s%s%s", kEmotionPosixDir,
                     kRequiredEmotions[i], ext);
        }
    }
}

bool CheckEmotionResourcesReady() {
    if (!SdCardManager::GetInstance().IsMounted()) {
        return false;
    }
    for (size_t i = 0; i < kRequiredEmotionCount; ++i) {
        if (!EmotionFileExists(kRequiredEmotions[i])) {
            return false;
        }
    }
    return true;
}

const char* MissingResourceHintText() {
    if (!SdCardManager::GetInstance().IsMounted()) {
        return I18n::T(
            "未检测到 SD 卡\n\n请将小白助手资源包放入 SD 卡\nSystem/emotion/ 目录");
    }
    return I18n::T(
        "小白助手资源缺失\n\n请将资源包复制到 SD 卡\nSystem/emotion/ 目录\n\n"
        "需包含 6 个表情资源文件");
}

lv_obj_t* BuildMissingResourceHint(lv_obj_t* parent) {
    lv_obj_t* hint = lv_label_create(parent);
    lv_label_set_text(hint, MissingResourceHintText());
    lv_obj_set_width(hint, kPanelSize - 80);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, bubble_font(), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(hint);
    return hint;
}

void StyleBubble(lv_obj_t* bubble) {
    screen_strip_obj_chrome(bubble);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(kColorBubbleBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, kBubbleBgOpa, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, kBubbleRadius, LV_PART_MAIN);
    lv_obj_set_style_border_color(bubble, lv_color_hex(kColorBubbleBorder),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, kBubbleBorder, LV_PART_MAIN);
    lv_obj_set_style_border_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bubble, kBubblePadX, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bubble, kBubblePadY, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
}

// 创建带白边的聊天气泡，初始隐藏。dir 决定气泡在父容器内的对齐方式。
struct BubbleHandles {
    lv_obj_t* bubble;
    lv_obj_t* label;
};

BubbleHandles BuildBubble(lv_obj_t* parent, lv_align_t align, int32_t x_ofs,
                          int32_t y_ofs) {
    lv_obj_t* bubble = lv_obj_create(parent);
    StyleBubble(bubble);
    lv_obj_set_width(bubble, 100);  // 占位，AddMessage 时会重算
    lv_obj_align(bubble, align, x_ofs, y_ofs);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* label = lv_label_create(bubble);
    lv_label_set_text(label, "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, bubble_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorBubbleText),
                                LV_PART_MAIN);

    // 气泡不消费输入，让右滑返回手势能穿透。
    screen_make_input_passive(bubble);
    return {bubble, label};
}

void UpdateBubble(lv_obj_t* bubble, lv_obj_t* label, const char* text,
                  int32_t max_w) {
    if (bubble == nullptr || label == nullptr) return;

    const lv_font_t* font = bubble_font();
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t text_w = size.x;
    if (text_w < 32) text_w = 32;
    int32_t bubble_w = text_w + kBubblePadX * 2 + kBubbleBorder * 2;
    if (bubble_w > max_w) bubble_w = max_w;

    lv_obj_set_width(bubble, bubble_w);
    lv_obj_set_width(label, bubble_w - kBubblePadX * 2 - kBubbleBorder * 2);
    lv_label_set_text(label, text);
    lv_obj_update_layout(label);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_HIDDEN);
    // 重新对齐：内容变化后宽高会重算，需要再 align 一次保证锚点正确。
    lv_obj_update_layout(bubble);
}

void OnSwipeBack();

// ---------------------------------------------------------------------------
// 设备激活检查
// ---------------------------------------------------------------------------
bool is_device_activated() {
    return Application::GetInstance().IsDeviceActivated();
}

void log_activation_blocked() {
    auto& app = Application::GetInstance();
    ESP_LOGW(TAG, "DigitalPeople blocked: device not activated (boot_ready=%d pending=%d state=%d)",
             app.IsBootReady() ? 1 : 0,
             app.HasPendingActivation() ? 1 : 0,
             static_cast<int>(app.GetDeviceState()));
    if (app.HasPendingActivation()) {
        ESP_LOGW(TAG, "pending activation code: %s",
                 app.GetPendingActivationCode().c_str());
    }
}

void open_activation_blocked_dialog() {
    if (s_ui.screen == nullptr || s_activation_dlg.mask != nullptr) {
        return;
    }

    auto& app = Application::GetInstance();
    const bool has_code = app.HasPendingActivation();
    s_activation_dialog_shows_code = has_code;

    constexpr int32_t kCardW = 520;
    const int32_t kCardH = has_code ? 420 : 340;
    constexpr int32_t kBackBtnW = 200;
    constexpr int32_t kBackBtnH = 72;

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);
    s_activation_dlg.mask = mask;

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 28, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("设备未激活"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* desc = lv_label_create(card);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, kCardW - 56);
    lv_label_set_text(desc, I18n::T("请先完成设备激活后再使用小白助手。"));
    lv_obj_set_style_text_color(desc, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
    lv_obj_set_style_text_font(desc, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, has_code ? -30 : -10);
    lv_obj_remove_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    if (has_code) {
        char code_buf[64];
        std::snprintf(code_buf, sizeof(code_buf), I18n::T("验证码: %s"),
                      app.GetPendingActivationCode().c_str());
        lv_obj_t* code_lbl = lv_label_create(card);
        lv_label_set_text(code_lbl, code_buf);
        lv_obj_set_style_text_color(code_lbl, lv_color_hex(0xFBBF24),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(code_lbl, &font_puhui_30_4, LV_PART_MAIN);
        lv_obj_align(code_lbl, LV_ALIGN_BOTTOM_MID, 0, -(kBackBtnH + 24));
        lv_obj_remove_flag(code_lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* back = lv_button_create(card);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnW, kBackBtnH);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 16, LV_PART_MAIN);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(back,
                        [](lv_event_t* /*e*/) { OnSwipeBack(); },
                        LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, I18n::T("返回"));
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_font(back_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(back_lbl);
    lv_obj_remove_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void ensure_activation_blocked_dialog() {
    if (!s_activation_blocked) {
        return;
    }
    if (s_activation_dlg.mask == nullptr) {
        open_activation_blocked_dialog();
    }
}

void close_activation_blocked_dialog() {
    if (s_activation_dlg.mask != nullptr) {
        lv_obj_delete(s_activation_dlg.mask);
        s_activation_dlg.mask = nullptr;
    }
    s_activation_dialog_shows_code = false;
}

void sync_activation_block_state() {
    auto& app = Application::GetInstance();
    const bool should_block = !is_device_activated();
    const bool has_code = app.HasPendingActivation();
    if (should_block == s_activation_blocked) {
        if (should_block) {
            if (has_code && !s_activation_dialog_shows_code) {
                close_activation_blocked_dialog();
                open_activation_blocked_dialog();
            } else {
                ensure_activation_blocked_dialog();
            }
        }
        return;
    }
    s_activation_blocked = should_block;
    if (should_block) {
        schedule_voice_ui_stop();
        log_activation_blocked();
        open_activation_blocked_dialog();
    } else {
        ESP_LOGI(TAG, "DigitalPeople unblocked: device activated/ready");
        close_activation_blocked_dialog();
        schedule_voice_ui_start();
    }
}

void on_activation_guard_timer(lv_timer_t* /*timer*/) {
    sync_activation_block_state();
}

void OnSwipeBack() {
    cancel_long_press_timer();
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    digital_people_page_leave();
    HomeScreen::SwitchToHome();
}

void OnScreenUnloaded(lv_event_t* e) {
    // 仅清理当前实例：Create 重入后旧屏 async delete 的 UNLOADED
    // 不能清掉新屏的静态引用。
    if (lv_event_get_target(e) != s_ui.screen) {
        return;
    }
    digital_people_page_leave();
    cancel_long_press_timer();
    if (s_activation_guard_timer != nullptr) {
        lv_timer_delete(s_activation_guard_timer);
        s_activation_guard_timer = nullptr;
    }
    if(s_emotion_ui_timer){lv_timer_delete(s_emotion_ui_timer);s_emotion_ui_timer=nullptr;}
    s_activation_dlg = ActivationBlockedDialogUi{};
    s_activation_blocked = false;
    s_activation_dialog_shows_code = false;
    s_voice_ui_held = false;
    s_ui = UiState{};
}

}  // namespace

// ---- Screen base class interface ----

lv_obj_t* DigitalPeopleScreen::CreateStatic() {
    XiaobaiResources::EnsureAsync();
    if(s_emotion_ui_timer){lv_timer_delete(s_emotion_ui_timer);s_emotion_ui_timer=nullptr;}
    s_applied_revision=SnapshotEmotion(s_current_emotion);
    s_expression_state=kDeviceStateUnknown;
    s_speech_started_ms=0;
    if (s_ui.screen != nullptr) {
        ESP_LOGW(TAG, "Create while previous screen still active, resetting refs");
        digital_people_page_leave();
        cancel_long_press_timer();
        if (s_activation_guard_timer != nullptr) {
            lv_timer_delete(s_activation_guard_timer);
            s_activation_guard_timer = nullptr;
        }
        s_voice_ui_held = false;
        s_ui = UiState{};
    }

    s_activation_blocked = !is_device_activated();
    if (s_activation_blocked) {
        log_activation_blocked();
    }

    ESP_LOGI(TAG, "create digital people screen (fmt=%s delay=%u)",
             DigitalPeoplePrefs::GetEmotionExt(),
             static_cast<unsigned>(DigitalPeoplePrefs::GetFrameDelayMs()));

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);


    s_ui.assets_ready=XiaobaiResources::Status()==2;
    const bool resources_ready = CheckEmotionResourcesReady();
    if (!resources_ready) {
        ESP_LOGW(TAG, "emotion resources not ready (mounted=%d)",
                 SdCardManager::GetInstance().IsMounted() ? 1 : 0);
        LogMissingEmotionFiles();
        s_ui.hint_label = BuildMissingResourceHint(scr);
    } else {
        s_ui.eaf = CreateEmotionWidget(scr);
        SetEmotionSrc(s_ui.eaf, s_current_emotion);
        // The EAF widget manages the image size for each animation frame.
        if (!EmotionUsesEaf()) {
            lv_image_set_inner_align(s_ui.eaf, LV_IMAGE_ALIGN_CENTER);
        }
        lv_obj_center(s_ui.eaf);
        screen_make_input_passive(s_ui.eaf);
    }

    // 两个气泡：system 锚到左上（让出 back 按钮位置），user 锚到底部居中。
    {
        BubbleHandles sys = BuildBubble(scr, LV_ALIGN_TOP_LEFT, kSideMargin,
                                        kSysBubbleTop);
        s_ui.system_bubble = sys.bubble;
        s_ui.system_label  = sys.label;

        BubbleHandles usr = BuildBubble(scr, LV_ALIGN_BOTTOM_MID, 0,
                                        -kUserBubbleBottom);
        s_ui.user_bubble = usr.bubble;
        s_ui.user_label  = usr.label;
    }

    lv_obj_t* look_btn = lv_button_create(scr);
    s_ui.look_and_see_btn = look_btn;
    lv_obj_set_size(look_btn, 184, 68);
    lv_obj_align(look_btn, LV_ALIGN_BOTTOM_MID, 116, -48);
    lv_obj_set_style_radius(look_btn, 34, LV_PART_MAIN);
    lv_obj_set_style_bg_color(look_btn, lv_color_hex(0x1E3A2F), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(look_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(look_btn, lv_color_hex(0x34D399), LV_PART_MAIN);
    lv_obj_set_style_border_width(look_btn, 2, LV_PART_MAIN);
    lv_obj_add_event_cb(look_btn, OnLookAndSeeButtonClicked, LV_EVENT_SHORT_CLICKED,
                        nullptr);
    lv_obj_add_event_cb(look_btn, OnLookAndSeeButtonPress, LV_EVENT_PRESSED,
                        nullptr);
    lv_obj_add_event_cb(look_btn, OnLookAndSeeButtonPress, LV_EVENT_PRESSING,
                        nullptr);
    lv_obj_add_event_cb(look_btn, OnLookAndSeeButtonPress, LV_EVENT_RELEASED,
                        nullptr);
    lv_obj_add_event_cb(look_btn, OnLookAndSeeButtonPress, LV_EVENT_PRESS_LOST,
                        nullptr);
    lv_obj_t* look_lbl = lv_label_create(look_btn);
    lv_label_set_text(look_lbl, I18n::T("Hi 视界"));
    lv_obj_set_style_text_color(look_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(look_lbl, bubble_font(), LV_PART_MAIN);
    lv_obj_center(look_lbl);
    screen_make_input_passive(look_lbl);

    auto*voice=lv_button_create(scr);s_ui.voice_btn=voice;
    lv_obj_set_size(voice,224,68);lv_obj_align(voice,LV_ALIGN_BOTTOM_MID,-112,-48);
    lv_obj_set_style_radius(voice,34,0);lv_obj_set_style_bg_color(voice,lv_color_hex(0x24334B),0);
    lv_obj_set_style_border_color(voice,lv_color_hex(0x83B8ED),0);lv_obj_set_style_border_width(voice,1,0);
    lv_obj_add_event_cb(voice,[](lv_event_t*){if(s_activation_blocked)return;Application::GetInstance().ArmAssistantWakeWord();Application::GetInstance().RequestWakeSession();},LV_EVENT_CLICKED,nullptr);
    auto*voice_label=lv_label_create(voice);lv_label_set_text(voice_label,"待唤醒 · 点按对话");
    lv_obj_set_style_text_font(voice_label,&font_puhui_20_4,0);lv_obj_set_style_text_color(voice_label,lv_color_white(),0);lv_obj_center(voice_label);screen_make_input_passive(voice_label);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnLongPressPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(scr, OnLongPressPressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(scr, OnLongPressReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(scr, OnLongPressReleased, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    if (s_activation_blocked) {
        open_activation_blocked_dialog();
    }
    s_activation_guard_timer =
        lv_timer_create(on_activation_guard_timer, 1000, nullptr);

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
            sync_activation_block_state();
            if(!s_activation_blocked)schedule_voice_ui_start();
        }
    }, LV_EVENT_SCREEN_LOADED, nullptr);

    s_emotion_ui_timer=lv_timer_create([](lv_timer_t*t){
        if(lv_timer_get_user_data(t)!=s_ui.screen||lv_screen_active()!=s_ui.screen)return;
        if(s_ui.voice_btn){auto state=Application::GetInstance().GetDeviceState();const char*text=state==kDeviceStateConnecting?"连接中…":state==kDeviceStateListening?"聆听中…":state==kDeviceStateSpeaking?"正在回答…":"待唤醒 · 点按对话";auto*label=lv_obj_get_child(s_ui.voice_btn,0);if(strcmp(lv_label_get_text(label),text))lv_label_set_text(label,text);}
        if(!s_ui.eaf){
            if(XiaobaiResources::Status()==2&&CheckEmotionResourcesReady()){
                if(s_ui.hint_label){lv_obj_delete(s_ui.hint_label);s_ui.hint_label=nullptr;}
                s_ui.eaf=CreateEmotionWidget(s_ui.screen);SetEmotionSrc(s_ui.eaf,s_current_emotion);lv_obj_center(s_ui.eaf);screen_make_input_passive(s_ui.eaf);lv_obj_move_background(s_ui.eaf);
            }else if(s_ui.hint_label){const char*text=XiaobaiResources::Status()==1?"正在准备小白表情资源…":MissingResourceHintText();if(strcmp(text,lv_label_get_text(s_ui.hint_label)))lv_label_set_text(s_ui.hint_label,text);}
        }
        if(s_ui.eaf&&!s_ui.assets_ready&&XiaobaiResources::Status()==2){SetEmotionSrc(s_ui.eaf,s_current_emotion);s_ui.assets_ready=true;}
        char requested[24];unsigned revision=SnapshotEmotion(requested);
        const auto state=Application::GetInstance().GetDeviceState();
        if(state!=s_expression_state) {
            s_expression_state=state;
            if(state==kDeviceStateSpeaking)s_speech_started_ms=lv_tick_get();
        }
        const char *face=requested;
        if(state==kDeviceStateSpeaking) {
            // Respect the cloud's emotion. A neutral/happy answer gets a
            // gentle happy/laughing cadence until the actual TTS tail drains.
            if(!strcmp(requested,"neutral")||!strcmp(requested,"happy")||!strcmp(requested,"loving"))
                face=((lv_tick_get()-s_speech_started_ms)/1200)%2?"laughing":"happy";
        } else if(state==kDeviceStateConnecting) face="thinking";
        else if(state==kDeviceStateIdle||state==kDeviceStateListening) face="neutral";
        if(s_ui.eaf&&strcmp(s_current_emotion,face)) {
            strcpy(s_current_emotion,face);SetEmotionSrc(s_ui.eaf,s_current_emotion);
        }
        s_applied_revision=revision;
    },150,scr);
    return scr;
}

lv_obj_t* DigitalPeopleScreen::Create() {
    root_ = CreateStatic();
    return root_;
}

bool DigitalPeopleScreen::IsActive() {
    return s_ui.screen != nullptr;
}
bool DigitalPeopleScreen::IsForeground(){return s_voice_ui_held.load(std::memory_order_acquire);}

void DigitalPeopleScreen::ShowUserMessage(const char* text) {
    if (!IsActive() || text == nullptr || text[0] == '\0') return;
    if (s_activation_blocked) return;
    UpdateBubble(s_ui.user_bubble, s_ui.user_label, text, kUserBubbleMaxW);
    // 重新对齐到底部中点，让宽度变化后视觉居中。
    lv_obj_align(s_ui.user_bubble, LV_ALIGN_BOTTOM_MID, 0, -kUserBubbleBottom);
}

void DigitalPeopleScreen::ShowSystemMessage(const char* text) {
    if (!IsActive() || text == nullptr || text[0] == '\0') return;
    if (s_activation_blocked) return;
    UpdateBubble(s_ui.system_bubble, s_ui.system_label, text, kSysBubbleMaxW);
    lv_obj_align(s_ui.system_bubble, LV_ALIGN_TOP_LEFT, kSideMargin,
                 kSysBubbleTop);
}

void DigitalPeopleScreen::ClearMessages() {
    if (s_ui.system_bubble != nullptr) {
        lv_label_set_text(s_ui.system_label, "");
        lv_obj_add_flag(s_ui.system_bubble, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui.user_bubble != nullptr) {
        lv_label_set_text(s_ui.user_label, "");
        lv_obj_add_flag(s_ui.user_bubble, LV_OBJ_FLAG_HIDDEN);
    }
}

void DigitalPeopleScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        if (!is_device_activated()) {
            ESP_LOGW(TAG,
                     "load: digital_people_screen blocked (device not activated)");
            log_activation_blocked();
        } else {
            ESP_LOGI(TAG, "load: digital_people_screen -> schedule voice UI start");
            schedule_voice_ui_start();
        }
    } else {
        ESP_LOGI(TAG, "unload: digital_people_screen -> schedule voice UI stop");
        digital_people_page_leave();
    }
}

void DigitalPeopleScreen::SetEmotion(const char* category) {
    if(!category||!*category)category=kDefaultEmotion;
    static const char*allowed[]={"neutral","happy","loving","thinking","crying","surprised","laughing","angry"};
    bool valid=false;for(const char*name:allowed)if(!strcmp(category,name))valid=true;if(!valid)category=kDefaultEmotion;
    std::lock_guard<std::mutex>lock(s_emotion_mutex);if(!strcmp(s_requested_emotion,category))return;
    strncpy(s_requested_emotion,category,sizeof(s_requested_emotion)-1);s_requested_emotion[sizeof(s_requested_emotion)-1]=0;++s_requested_revision;
    // The producer never touches LVGL. The owning UI timer applies changes.
}
