/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * MusicScreen — ported from MetalioClaw4
 * main/display/screen/music_screen/music_screen.cc.
 *
 * Adaptations for openvela/NuttX:
 *   - ESP-IDF headers (esp_log / freertos) → NuttX shims
 *     (esp_log_shim.h / freertos_shim.h).
 *   - SimpleUart (BT-module AT command channel + UART RX callback) is
 *     not exposed by openvela yet, so the AT command path is stubbed:
 *     the buttons still call SendAt("AT+PREV\r\n") etc., but the call
 *     is a no-op log. The UART RX callback that originally parsed the
 *     JSON song/lyric/MPLAY/MPAUSE stream is replaced by an external
 *     data-push API (PushSong / PushLyric / PushPlayState) so the
 *     upstream owner can feed parsed data in once SimpleUart lands.
 *   - lv_eaf.h (EAF vector animation widget used for the spinning
 *     album art) is not available on NuttX. The album container is
 *     kept (round clip mask + 240x240 image) and a static image is
 *     used in its place. sync_album_eaf() becomes a no-op stub so
 *     callers keep their original shape.
 *   - LV_IMAGE_ALIGN_CONTAIN does not exist in LVGL v9; replaced with
 *     LV_IMAGE_ALIGN_CENTER (per the openvela port convention).
 *   - HomeScreen::Create() → HomeScreen::CreateStatic() (openvela
 *     port convention — every screen returns through the static
 *     entry point).
 *   - TaskHandle_t is pid_t (int) on NuttX; assignments use 0
 *     instead of nullptr. vTaskDelete(nullptr) is provided by the
 *     freertos shim.
 *
 * Preserved verbatim:
 *   - Full 720x720 layout: back button, song title, circular album
 *     mask, three-row fading lyric stack, control row (vol-/prev/
 *     play-pause/next/vol+), bottom usage hint.
 *   - All event handlers (prev/next/play/volup/voldown, swipe-back,
 *     screen-unloaded).
 *   - Async UI update marshalling (lv_async_call) — song / lyric /
 *     play-state pushes are still threaded onto the LVGL thread.
 *   - Lyric fade animation (three-line opa cascade).
 *   - Lifecycle hook wiring (LOAD → would switch BT to mode 3,
 *     UNLOAD → would switch back to mode 1; both stubbed).
 */

#include "music_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "freertos_shim.h"
#include "home_screen/home_screen.h"
#include "simple_uart.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char *TAG = "MusicScreen";

// ---------------------------------------------------------------------------
// 720x720 layout (see header doc for the full ASCII map).
// ---------------------------------------------------------------------------
constexpr int32_t kPanelSize = 720;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorBgGrad = 0x161A22;
constexpr uint32_t kColorTextPrimary = 0xFFFFFF;
constexpr uint32_t kColorAccent = 0xE0FB3C;
constexpr uint32_t kColorCtrlBtnBg = 0x232732;
constexpr uint32_t kColorCtrlBtnBgPressed = 0x303644;
constexpr uint32_t kColorPlayBtnBg = 0x3A4150;
constexpr uint32_t kColorPlayBtnBgPressed = 0x4A5260;

constexpr int32_t kTitleY = 48;
constexpr int32_t kAlbumSize = 240;
constexpr int32_t kHintBottomMargin = 16;
// GIF圆角抗锯齿白边裁切：每边缩 4px 把白边一起切掉。
constexpr int32_t kAlbumMaskShrink = 4;
constexpr int32_t kAlbumMaskSize = kAlbumSize - kAlbumMaskShrink * 2;
constexpr int32_t kAlbumY = 140;
constexpr int32_t kCtrlRowY = 560;
constexpr int32_t kCtrlRowWidth = 700;
constexpr int32_t kCtrlRowHeight = 120;

constexpr int32_t kCtrlSideBtnSize = 80;
constexpr int32_t kCtrlPlayBtnSize = 112;

constexpr int32_t kLyricLineCount = 3;
constexpr int32_t kLyricY = 410;
constexpr int32_t kLyricLineGap = 34;
constexpr int32_t kLyricLineWidth = kPanelSize - 80;
// 顶 -> 底：100% / 60% / 30%。
constexpr lv_opa_t kLyricTargetOpa[kLyricLineCount] = {255, 153, 76};
constexpr uint32_t kLyricFadeDurationMs = 380;

struct MusicUi {
    lv_obj_t *lbl_song = nullptr;
    lv_obj_t *lbl_lyric[kLyricLineCount] = {nullptr, nullptr, nullptr};
    lv_obj_t *img_play_icon = nullptr;
    lv_obj_t *album_img = nullptr;  // was album_eaf; lv_eaf not on NuttX
    // 进入界面时蓝牙端通常还没在播放，默认按钮显示"▶ 播放"，
    // 用户点击后才切到"❚❚ 暂停"图标。
    bool playing = false;
};

MusicUi s_ui;
bool s_screen_active = false;
std::string s_rx_buffer;

// 把 (part | state) 显式转成 lv_style_selector_t，规避
// -Wdeprecated-enum-enum-conversion 告警。
inline lv_style_selector_t Sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part | state);
}

// ---------------------------------------------------------------------------
// AT command TX — real /dev/ttyS1 write (was a stub).
// ---------------------------------------------------------------------------
void send_at(const char *cmd) {
    if (cmd == nullptr) {
        return;
    }
    if (!SimpleUart::getInstance().begin()) {
        ESP_LOGW(TAG, "TX dropped (uart unavailable): %s", cmd);
        return;
    }
    std::string line(cmd);
    if (line.empty() || line.back() != '\n') {
        line += "\r\n";
    }
    SimpleUart::getInstance().sendString(line);
    ESP_LOGI(TAG, "TX: %s", cmd);
}

// Stub for the original lv_eaf_resume / lv_eaf_pause calls. With lv_eaf
// gone, the album art is a static image and there is nothing to pause.
void sync_album_eaf(bool /*playing*/) {}

// ---------------------------------------------------------------------------
// 异步 UI 更新（外部线程 -> LVGL 主线程）
//
// 原实现里这些回调由 SimpleUart 的 RX 任务触发；openvela 改成由
// PushSong / PushLyric / PushPlayState 公共 API 触发，但仍然走
// lv_async_call 把工作切到 LVGL 线程。
// ---------------------------------------------------------------------------
struct AsyncTextMsg {
    char text[192];
};

void async_set_song(void *user_data) {
    auto *msg = static_cast<AsyncTextMsg *>(user_data);
    if (s_screen_active && s_ui.lbl_song != nullptr) {
        lv_label_set_text(s_ui.lbl_song, msg->text);
    }
    delete msg;
}

// 单行 opacity 动画：把 label 当前 opa 平滑过渡到 to。
void anim_label_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var),
                         static_cast<lv_opa_t>(v), LV_PART_MAIN);
}

void start_opa_anim(lv_obj_t *obj, int32_t from, int32_t to) {
    if (obj == nullptr) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, kLyricFadeDurationMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_label_opa_cb);
    lv_anim_start(&a);
}

// 推动一行新歌词进队列：
//   line[2] <- line[1]   (旧 -> 更旧)
//   line[1] <- line[0]   (上 -> 中)
//   line[0] <- new       (新词在顶部)
// 同时把每行的 opacity 从「上一格的目标值」缓慢渐入到「自己格子的目标值」，
// 视觉上像是文字向下沉、越来越淡。
void push_lyric_line(const char *new_text) {
    if (s_ui.lbl_lyric[0] == nullptr) {
        return;
    }
    const char *cur_top = lv_label_get_text(s_ui.lbl_lyric[0]);
    const char *cur_mid = lv_label_get_text(s_ui.lbl_lyric[1]);
    std::string s_top = cur_top != nullptr ? cur_top : "";
    std::string s_mid = cur_mid != nullptr ? cur_mid : "";

    lv_label_set_text(s_ui.lbl_lyric[2], s_mid.c_str());
    lv_label_set_text(s_ui.lbl_lyric[1], s_top.c_str());
    lv_label_set_text(s_ui.lbl_lyric[0], new_text);

    // 顶行：从 0 渐入到 100% （新词淡入）
    // 中行：从 100% 渐落到 60%
    // 底行：从 60%  渐落到 30%
    start_opa_anim(s_ui.lbl_lyric[0], 0, kLyricTargetOpa[0]);
    start_opa_anim(s_ui.lbl_lyric[1], kLyricTargetOpa[0], kLyricTargetOpa[1]);
    start_opa_anim(s_ui.lbl_lyric[2], kLyricTargetOpa[1], kLyricTargetOpa[2]);
}

void async_set_lyric(void *user_data) {
    auto *msg = static_cast<AsyncTextMsg *>(user_data);
    if (s_screen_active && s_ui.lbl_lyric[0] != nullptr) {
        push_lyric_line(msg->text);
    }
    delete msg;
}

// ---- 播放 / 暂停图标同步（外部线程 -> LVGL 主线程） -----------------------
// 按钮图标的语义：图标本身就是「点了之后会发生的动作」。
//   playing == true  -> 当前正在播 -> 图标显示 ❚❚（点了就暂停）
//   playing == false -> 当前已暂停 -> 图标显示 ▶ （点了就播放）
struct AsyncPlayStateMsg {
    bool playing;
};

void async_set_play_icon(void *user_data) {
    auto *msg = static_cast<AsyncPlayStateMsg *>(user_data);
    if (s_screen_active && s_ui.img_play_icon != nullptr) {
        s_ui.playing = msg->playing;
        lv_image_set_src(s_ui.img_play_icon,
                         msg->playing ? "A:ic_s_player_pause.spng"
                                      : "A:ic_s_player_play.spng");
        sync_album_eaf(msg->playing);
    }
    delete msg;
}

void post_play_state(bool playing) {
    if (!s_screen_active) {
        return;
    }
    auto *msg = new AsyncPlayStateMsg{playing};
    lv_async_call(async_set_play_icon, msg);
}

void post_song(const std::string &text) {
    if (!s_screen_active) {
        return;
    }
    auto *msg = new AsyncTextMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text.c_str());
    lv_async_call(async_set_song, msg);
}

void post_lyric(const std::string &text) {
    if (!s_screen_active) {
        return;
    }
    auto *msg = new AsyncTextMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text.c_str());
    lv_async_call(async_set_lyric, msg);
}

// ---------------------------------------------------------------------------
// JSON 解析（极简）—— 只支持下面这两种行：
//   {"type":"song",  "data":"..."}
//   {"type":"lyrics","data":"..."}
// 直接做字符串查找，避免引入完整 JSON 解析器。
// ---------------------------------------------------------------------------
bool extract_quoted_value(const std::string &line, const std::string &key,
                          std::string &out) {
    const std::string pattern = "\"" + key + "\"";
    size_t p = line.find(pattern);
    if (p == std::string::npos) {
        return false;
    }
    size_t colon = line.find(':', p + pattern.size());
    if (colon == std::string::npos) {
        return false;
    }
    size_t quote_open = line.find('"', colon + 1);
    if (quote_open == std::string::npos) {
        return false;
    }
    size_t quote_close = line.find('"', quote_open + 1);
    if (quote_close == std::string::npos) {
        return false;
    }
    out = line.substr(quote_open + 1, quote_close - quote_open - 1);
    return true;
}

void handle_json_line(const std::string &line) {
    if (line.empty() || line.front() != '{') {
        return;
    }
    std::string type;
    std::string data;
    if (!extract_quoted_value(line, "type", type)) {
        return;
    }
    if (!extract_quoted_value(line, "data", data)) {
        return;
    }
    if (type == "song") {
        ESP_LOGI(TAG, "song: %s", data.c_str());
        post_song(data);
    } else if (type == "lyrics") {
        ESP_LOGI(TAG, "lyrics: %s", data.c_str());
        post_lyric(data);
    }
}

// BT 模块播放状态回包（+EVT:MPLAY / OK MPAUSE 等，substring 匹配）。
void handle_play_state_line(const std::string &line) {
    if (line.find("MPAUSE") != std::string::npos) {
        ESP_LOGI(TAG, "BT report: paused");
        post_play_state(false);
        return;
    }
    if (line.find("MPLAY") != std::string::npos) {
        ESP_LOGI(TAG, "BT report: playing");
        post_play_state(true);
    }
}

void handle_line(const std::string &line) {
    if (line.empty()) {
        return;
    }
    if (line.front() == '{') {
        handle_json_line(line);
    } else {
        handle_play_state_line(line);
    }
}

void on_uart_data(const std::vector<uint8_t> &data) {
    if (!data.empty()) {
        s_rx_buffer.append(reinterpret_cast<const char *>(data.data()),
                           data.size());
    }

    size_t pos = 0;
    while (true) {
        size_t nl = s_rx_buffer.find('\n', pos);
        if (nl == std::string::npos) {
            break;
        }
        std::string line = s_rx_buffer.substr(pos, nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        handle_line(line);
        pos = nl + 1;
    }
    if (pos > 0) {
        s_rx_buffer.erase(0, pos);
    }
    if (s_rx_buffer.size() > 2048) {
        ESP_LOGW(TAG, "RX buffer overflow, clearing");
        s_rx_buffer.clear();
    }
}

// ---------------------------------------------------------------------------
// BT 模式切换 task：通过 /dev/ttyS1 把 BT 模块切到模式三 / 模式一。
// ---------------------------------------------------------------------------
void switch_to_mode3_task(void * /*arg*/) {
    ESP_LOGI(TAG, "load: switching BT to mode 3");
    send_at("AT+RX=1\r\n");
    vTaskDelay(pdMS_TO_TICKS(700));
    send_at("AT+MODE=3\r\n");
    vTaskDelete(nullptr);
}

void switch_to_mode1_task(void * /*arg*/) {
    ESP_LOGI(TAG, "unload: switching BT back to mode 1");
    send_at("AT+RX=2\r\n");
    vTaskDelay(pdMS_TO_TICKS(700));
    send_at("AT+MODE=1\r\n");
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// 控件回调
// ---------------------------------------------------------------------------
void OnPrevClicked(lv_event_t * /*e*/) { send_at("AT+PREV\r\n"); }
void OnNextClicked(lv_event_t * /*e*/) { send_at("AT+NEXT\r\n"); }

void OnPlayClicked(lv_event_t * /*e*/) {
    // 按钮图标语义 = 「点了之后的动作」。
    //   - 正在播放（playing=true，图标=❚❚） -> 点 = 暂停 -> 发 MPAUSE
    //   - 已暂停  （playing=false，图标=▶ ） -> 点 = 播放 -> 发 MPLAY
    // 先乐观切图标，BT 模块随后会回包 MPLAY / MPAUSE 让外部推送做最终对齐。
    const bool want_playing = !s_ui.playing;
    send_at(want_playing ? "AT+MPLAY=1\r\n" : "AT+MPAUSE=1\r\n");
    s_ui.playing = want_playing;
    if (s_ui.img_play_icon != nullptr) {
        lv_image_set_src(s_ui.img_play_icon,
                         want_playing ? "A:ic_s_player_pause.spng"
                                      : "A:ic_s_player_play.spng");
    }
    sync_album_eaf(want_playing);
}

void OnVolDownClicked(lv_event_t * /*e*/) { send_at("AT+VOLDOWN\r\n"); }
void OnVolUpClicked(lv_event_t * /*e*/) { send_at("AT+VOLUP\r\n"); }

void OnSwipeBack()
{
    HomeScreen::SwitchToHome();
}

void OnScreenUnloaded(lv_event_t * /*e*/) {
    s_screen_active = false;
    s_ui = MusicUi{};
}

// ---------------------------------------------------------------------------
// UI 构造
// ---------------------------------------------------------------------------
lv_obj_t *CreateRoundButton(lv_obj_t *parent, int32_t size, uint32_t bg_color,
                            uint32_t bg_pressed, const char *icon_path,
                            lv_event_cb_t cb) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_pressed),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(btn, 12);

    lv_obj_t *img = lv_image_create(btn);
    lv_image_set_src(img, icon_path);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return img;
}

void BuildBackButton(lv_obj_t *scr) {
    // 透明圆形按钮 + ← 图标，按下时白色半透明叠加。
    lv_obj_t *back_btn = lv_button_create(scr);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 72, 72);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xFFFFFF),
                              Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_20,
                            Sel(LV_PART_MAIN, LV_STATE_PRESSED));
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 32, 36);
    // 返回按钮自身的点击不应被全屏右滑手势拦截。
    screen_swipe_back_ignore(back_btn, true);

    lv_obj_t *back_icon = lv_image_create(back_btn);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_add_event_cb(
        back_btn, [](lv_event_t * /*e*/) { OnSwipeBack(); },
        LV_EVENT_CLICKED, nullptr);
}

void BuildUsageHint(lv_obj_t *scr) {
    // 屏幕最底部一行操作引导：「这是什么 · 怎么用」。
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(
        hint,
        I18n::T("蓝牙音箱模式 · 手机蓝牙连接本设备后，用手机音乐 App 播放歌曲"));
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8B92A3), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(hint, kPanelSize - 60);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -kHintBottomMargin);
    screen_make_input_passive(hint);
}

void BuildSongTitle(lv_obj_t *scr) {
    s_ui.lbl_song = lv_label_create(scr);
    // 默认占位文本，等手机回传 song 字段时被覆盖。
    lv_label_set_text(s_ui.lbl_song, I18n::T("蓝牙音乐"));
    lv_obj_set_style_text_font(s_ui.lbl_song, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.lbl_song, lv_color_hex(kColorTextPrimary),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.lbl_song, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_ui.lbl_song, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.lbl_song, kPanelSize - 80);
    lv_obj_align(s_ui.lbl_song, LV_ALIGN_TOP_MID, 0, kTitleY);
    screen_make_input_passive(s_ui.lbl_song);
}

void BuildAlbum(lv_obj_t *scr) {
    // 用一个跟 album 同样大小（240x240）的容器把 album 包起来：
    //   - radius = CIRCLE + clip_corner = true，所有超出 240 内切圆的像素
    //     都会被裁掉；
    //   - 容器自身 opa = 0，被裁掉的角直接漏出底下的屏幕背景；
    //   - 没有边框 / 描边 / 阴影，避免任何绿色或多余的圈线。
    lv_obj_t *mask = lv_obj_create(scr);
    lv_obj_set_size(mask, kAlbumMaskSize, kAlbumMaskSize);
    lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, kAlbumY + kAlbumMaskShrink);
    screen_strip_obj_chrome(mask);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(mask, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(mask, true, LV_PART_MAIN);

    // lv_eaf 不可用 —— 用一张静态图片占位，保持圆形裁切效果。
    s_ui.album_img = lv_image_create(mask);
    lv_image_set_src(s_ui.album_img, "A:ic_s_music_album.spng");
    lv_obj_set_size(s_ui.album_img, kAlbumSize, kAlbumSize);
    lv_image_set_inner_align(s_ui.album_img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(s_ui.album_img);
    lv_obj_remove_flag(s_ui.album_img, LV_OBJ_FLAG_CLICKABLE);
    // 进入界面默认未播放，专辑动画保持静止（stub: no-op）。
    sync_album_eaf(s_ui.playing);

    screen_make_input_passive(mask);
}

void BuildLyric(lv_obj_t *scr) {
    for (int i = 0; i < kLyricLineCount; ++i) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorAccent),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, kLyricLineWidth);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0,
                     kLyricY + i * kLyricLineGap);
        // 三行各自的初始 opacity：顶 100%、中 60%、底 30%。
        lv_obj_set_style_opa(lbl, kLyricTargetOpa[i], LV_PART_MAIN);
        screen_make_input_passive(lbl);
        s_ui.lbl_lyric[i] = lbl;
    }
}

void BuildControls(lv_obj_t *scr) {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_size(row, kCtrlRowWidth, kCtrlRowHeight);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, kCtrlRowY);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // 顺序：音量减 / 上一曲 / 播放暂停 / 下一曲 / 音量加
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed,
                      "A:ic_s_music_volume_down.spng", OnVolDownClicked);
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed,
                      "A:ic_s_player_previous.spng", OnPrevClicked);
    s_ui.img_play_icon = CreateRoundButton(row, kCtrlPlayBtnSize,
                                           kColorPlayBtnBg,
                                           kColorPlayBtnBgPressed,
                                           "A:ic_s_player_play.spng",
                                           OnPlayClicked);
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed,
                      "A:ic_s_player_next.spng", OnNextClicked);
    CreateRoundButton(row, kCtrlSideBtnSize, kColorCtrlBtnBg,
                      kColorCtrlBtnBgPressed,
                      "A:ic_s_music_volume_up.spng", OnVolUpClicked);
}

}  // namespace

// ---------------------------------------------------------------------------
// 公共 API
// ---------------------------------------------------------------------------

lv_obj_t *MusicScreen::CreateStatic() {
    s_ui = MusicUi{};
    // 进入界面默认按钮是"▶ 播放"，等用户点一次才进入播放状态。
    s_ui.playing = false;
    s_rx_buffer.clear();

    lv_obj_t *scr = lv_obj_create(nullptr);
    screen_strip_obj_chrome(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    BuildSongTitle(scr);
    BuildUsageHint(scr);
    BuildAlbum(scr);
    BuildLyric(scr);
    BuildControls(scr);
    // BackButton 最后建，保证它在 z-order 顶层、可被点中。
    BuildBackButton(scr);

    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(scr, OnSwipeBack);

    s_screen_active = true;
    return scr;
}

lv_obj_t *MusicScreen::Create() {
    root_ = CreateStatic();
    return root_;
}

void MusicScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: music_screen -> switching BT to mode 3");
        // 让 BT 模块切到「音乐接收」模式三；命令需要 700ms 间隔，放后台 task。
        xTaskCreate(switch_to_mode3_task, "mus_mode3", 4096, nullptr, 5, nullptr);
        // 注册 UART RX 回调，监听手机回传的 JSON 与播放状态。
        s_rx_buffer.clear();
        SimpleUart::getInstance().begin();
        SimpleUart::getInstance().registerCallback(on_uart_data);
    } else {
        ESP_LOGI(TAG, "unload: music_screen -> switching BT back to mode 1");
        // 摘掉回调，避免 UART task 仍向已销毁的 UI 投递更新。
        SimpleUart::getInstance().registerCallback(
            std::function<void(const std::vector<uint8_t> &)>());
        s_screen_active = false;
        s_rx_buffer.clear();
        // 切回模式 1 同样需要 700ms 间隔，放后台 task 异步执行。
        xTaskCreate(switch_to_mode1_task, "mus_mode1", 4096, nullptr, 5, nullptr);
    }
}

// ---- 外部数据推送 API（保留兼容；正常路径由 UART RX 回调触发） -------------
// 任何线程都能调用，内部通过 lv_async_call() 把工作切到 LVGL 线程。
// 屏幕不在台上时为 no-op。
void MusicScreen::PushSong(const std::string &text) { post_song(text); }

void MusicScreen::PushLyric(const std::string &text) { post_lyric(text); }

void MusicScreen::PushPlayState(bool playing) { post_play_state(playing); }
