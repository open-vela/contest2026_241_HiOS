// SPDX-License-Identifier: Apache-2.0
#include "xiangqi_screen.h"
#include "xiangqi_engine.h"
#include "home_screen/home_screen.h"
#include <stdint.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {
using namespace Xiangqi;
constexpr int Step = 56, BoardX = 70, BoardY = 116;
constexpr uint32_t Ink = 0x594536, Red = 0xB83F3B, Paper = 0xF6EEDF;
Game game; // Retained when returning to the desktop, until reboot.
bool cpu = true;
Difficulty difficulty = Beginner;
const char *LevelName() { return difficulty==Advanced ? "高级" : difficulty==Intermediate ? "中级" : "初级"; }
struct UI {
    lv_obj_t *root, *status, *mode, *undo, *dialog, *level;
    lv_obj_t *cells[90], *labels[90];
    lv_timer_t *timer = nullptr;
    int selected = -1;
    bool switch_mode = false;
    Search search;
};
UI *active = nullptr;
const char *PieceText(int p) {
    const char *red[] = {"", "帅", "仕", "相", "马", "车", "炮", "兵"};
    const char *black[] = {"", "将", "士", "象", "马", "车", "炮", "卒"};
    return p > 0 ? red[p] : black[-p];
}
lv_obj_t *Text(lv_obj_t *parent, const char *text, int x, int y, uint32_t color = Ink) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}
lv_obj_t *Box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y); lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}
lv_obj_t *Button(lv_obj_t *parent, const char *text, int x, int y, int w,
                 lv_event_cb_t cb, void *data) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y); lv_obj_set_size(btn, w, 48);
    lv_obj_set_style_bg_color(btn, lv_color_hex(Ink), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x80654A), LV_STATE_PRESSED);
    lv_obj_t *label = Text(btn, text, 0, 0, Paper);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, data);
    return btn;
}
void CancelCPU(UI *ui) {
    if (ui->timer) { lv_timer_delete(ui->timer); ui->timer = nullptr; }
}
void Refresh(UI *ui) {
    const Position &p = game.position();
    Move last{}; bool has_last = game.last_move(last);
    for (int i = 0; i < 90; ++i) {
        int piece = p.board[i];
        bool legal = ui->selected >= 0 && Legal(p, Move{(uint8_t)ui->selected, (uint8_t)i});
        bool selected = ui->selected == i;
        lv_obj_t *cell = ui->cells[i];
        lv_label_set_text(ui->labels[i], piece ? PieceText(piece) : legal ? "·" : "");
        lv_obj_set_style_text_color(ui->labels[i], lv_color_hex(piece < 0 ? Ink : Red), 0);
        lv_obj_set_style_bg_opa(cell, piece || selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(selected ? 0xF1CA7D : 0xFFF4DE), 0);
        lv_obj_set_style_border_width(cell, selected || legal ? 3 : piece ? 2 : has_last && (i == last.from || i == last.to) ? 2 : 0, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(selected ? Red : legal ? 0x4E8664 : has_last && i == last.to ? 0xC08B36 : Ink), 0);
        lv_obj_center(ui->labels[i]);
    }
    const char *message;
    if (game.result() == RedWins) message = "红方获胜 · 可悔棋或重新开局";
    else if (game.result() == BlackWins) message = "黑方获胜 · 可悔棋或重新开局";
    else if (game.result() == Draw) message = "和棋 · 连续120步未吃子";
    else if (InCheck(p, p.turn)) message = p.turn > 0 ? "红方被将军 · 请应将" : "黑方被将军 · 请应将";
    else if (cpu && p.turn < 0) message = "黑方思考中…";
    else message = p.turn > 0 ? "红方走棋" : "黑方走棋";
    lv_label_set_text(ui->status, message);
    char level_text[40];
    lv_snprintf(level_text,sizeof(level_text),"难度：%s",LevelName());
    lv_label_set_text(ui->level, level_text);
    lv_label_set_text(ui->mode, cpu ? "人机模式\n你执红棋" : "本地双人");
    if (game.move_count()) lv_obj_remove_state(ui->undo, LV_STATE_DISABLED);
    else lv_obj_add_state(ui->undo, LV_STATE_DISABLED);
}
void StartCPU(UI *ui);
void CPU(lv_timer_t *timer) {
    UI *ui = static_cast<UI *>(lv_timer_get_user_data(timer));
    // Never access an unloaded/deleted screen or advance a hidden match.
    if (active != ui || lv_screen_active() != ui->root || ui->dialog) { CancelCPU(ui); return; }
    if (!ui->search.Step()) return;
    CancelCPU(ui);
    Move move{};
    if (cpu && game.result() == Playing && game.position().turn < 0 && ui->search.move(move)) game.Play(move);
    Refresh(ui);
}
void StartCPU(UI *ui) {
    if (!ui->timer && !ui->dialog && cpu && game.result() == Playing && game.position().turn < 0) {
        ui->search.Begin(game.position(), difficulty);
        ui->timer = lv_timer_create(CPU, 10, ui);
    }
}
void CellClicked(lv_event_t *e) {
    UI *ui = active;
    if (!ui || ui->dialog || game.result() != Playing || (cpu && game.position().turn < 0)) return;
    int square = (int)(intptr_t)lv_event_get_user_data(e);
    if (ui->selected == square) ui->selected = -1;
    else if (Side(game.position().board[square]) == game.position().turn) ui->selected = square;
    else if (ui->selected >= 0) {
        if (!game.Play(Move{(uint8_t)ui->selected, (uint8_t)square})) {
            lv_label_set_text(ui->status, InvalidReason(game.position(), Move{(uint8_t)ui->selected, (uint8_t)square}));
            return;
        }
        ui->selected = -1;
    }
    Refresh(ui); StartCPU(ui);
}
void Back() {
    if (active) CancelCPU(active);
    if (lv_indev_active()) lv_indev_wait_release(lv_indev_active());
    HomeScreen::SwitchToHome();
}
void BackClicked(lv_event_t *) { Back(); }
void UndoClicked(lv_event_t *) {
    UI *ui = active;
    CancelCPU(ui);
    // A CPU undo returns to the human's previous decision, including when
    // the AI reply has not run yet or the human delivered checkmate.
    if (game.Undo() && cpu && game.position().turn < 0) game.Undo();
    ui->selected = -1; Refresh(ui); StartCPU(ui);
}
void DialogAnswer(lv_event_t *e) {
    UI *ui = active;
    bool yes = lv_event_get_user_data(e) != nullptr;
    lv_obj_delete(ui->dialog); ui->dialog = nullptr;
    if (yes) {
        if (ui->switch_mode) cpu = !cpu;
        game.Reset(); ui->selected = -1;
    }
    Refresh(ui); StartCPU(ui);
}
void ResetClicked(lv_event_t *e) {
    UI *ui = active;
    if (ui->dialog) return;
    CancelCPU(ui);
    ui->switch_mode = lv_event_get_user_data(e) != nullptr;
    if (!game.move_count()) {
        if (ui->switch_mode) cpu = !cpu;
        game.Reset(); ui->selected = -1; Refresh(ui); return;
    }
    ui->dialog = Box(ui->root, 0, 0, 720, 720, 0x241C16);
    lv_obj_set_style_bg_opa(ui->dialog, LV_OPA_80, 0);
    lv_obj_add_flag(ui->dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *panel = Box(ui->dialog, 110, 245, 500, 225, Paper);
    lv_obj_set_style_radius(panel, 20, 0);
    Text(panel, ui->switch_mode ? "切换模式并重新开局？" : "放弃当前棋局并重新开局？", 32, 35);
    Text(panel, "当前进度将被清除", 32, 80);
    Button(panel, "继续对局", 32, 145, 190, DialogAnswer, nullptr);
    Button(panel, "确定", 270, 145, 190, DialogAnswer, (void *)1);
}
void LevelAnswer(lv_event_t *e) {
    UI *ui=active; if(!ui || !ui->dialog) return;
    int choice=(int)(intptr_t)lv_event_get_user_data(e);
    if(choice>=0 && choice<=2) difficulty=static_cast<Difficulty>(choice);
    lv_obj_delete(ui->dialog); ui->dialog=nullptr;
    Refresh(ui); StartCPU(ui);
}
void LevelClicked(lv_event_t *) {
    UI *ui=active; if(!ui || ui->dialog) return;
    CancelCPU(ui);
    ui->dialog=Box(ui->root,0,0,720,720,0x241C16);
    lv_obj_set_style_bg_opa(ui->dialog,LV_OPA_80,0);
    lv_obj_add_flag(ui->dialog,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *panel=Box(ui->dialog,110,210,500,300,Paper);
    lv_obj_set_style_radius(panel,20,0);
    Text(panel,"选择电脑难度",32,24);
    Text(panel,"初级也会应将、防守和判断交换",32,65);
    Button(panel,"初级",32,112,130,LevelAnswer,(void*)0);
    Button(panel,"中级",185,112,130,LevelAnswer,(void*)1);
    Button(panel,"高级",338,112,130,LevelAnswer,(void*)2);
    Text(panel,"切换难度保留当前棋局",32,182);
    Button(panel,"取消",185,232,130,LevelAnswer,(void*)3);
}
void Deleted(lv_event_t *e) {
    UI *ui = static_cast<UI *>(lv_event_get_user_data(e));
    CancelCPU(ui); if (active == ui) active = nullptr; delete ui;
}
void Board(lv_obj_t *root, UI *ui) {
    lv_obj_t *board = Box(root, BoardX - 34, BoardY - 34, Step * 8 + 68, Step * 9 + 68, 0xEBD2A9);
    lv_obj_set_style_radius(board, 14, 0);
    lv_obj_set_style_outline_width(board, 4, 0);
    lv_obj_set_style_outline_color(board, lv_color_hex(0xA88255), 0);
    // Local origin (34,34); the river interrupts the seven interior files.
    for (int r = 0; r < 10; ++r) Box(board, 34, 34 + r * Step, 8 * Step + 1, 1, Ink);
    for (int c = 0; c < 9; ++c) {
        if (!c || c == 8) Box(board, 34 + c * Step, 34, 1, Step * 9, Ink);
        else {
            Box(board, 34 + c * Step, 34, 1, Step * 4, Ink);
            Box(board, 34 + c * Step, 34 + Step * 5, 1, Step * 4, Ink);
        }
    }
    // Palace diagonals, using native LVGL lines (no external assets).
    static const lv_point_precise_t diagonals[4][2] = {
        {{202,34},{314,146}}, {{314,34},{202,146}},
        {{202,426},{314,538}}, {{314,426},{202,538}}
    };
    for (int d = 0; d < 4; ++d) {
        lv_obj_t *line = lv_line_create(board);
        lv_line_set_points(line, diagonals[d], 2);
        lv_obj_set_style_line_color(line, lv_color_hex(Ink), 0);
        lv_obj_set_style_line_width(line, 1, 0);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
    Text(board, "楚 河", 115, 34 + Step * 4 + 14);
    Text(board, "汉 界", 315, 34 + Step * 4 + 14);
    for (int i = 0; i < 90; ++i) {
        lv_obj_t *cell = Box(board, 10 + i % 9 * Step, 10 + i / 9 * Step, 48, 48, Paper);
        lv_obj_set_style_radius(cell, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        ui->cells[i] = cell;
        ui->labels[i] = Text(cell, "", 0, 0);
        lv_obj_set_style_text_font(ui->labels[i], &font_puhui_30_4, 0);
        lv_obj_add_event_cb(cell, CellClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    screen_swipe_back_ignore(board);
}
}

lv_obj_t *XiangqiScreen::CreateStatic() {
    if (active) CancelCPU(active);
    UI *ui = new UI{};
    ui->root = lv_obj_create(nullptr); active = ui;
    screen_strip_obj_chrome(ui->root);
    lv_obj_set_style_bg_color(ui->root, lv_color_hex(Paper), 0);
    lv_obj_set_style_bg_opa(ui->root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(ui->root, &font_puhui_20_4, 0);
    lv_obj_add_event_cb(ui->root, Deleted, LV_EVENT_DELETE, ui);
    auto *title = Text(ui->root, "中国象棋", 34, 20);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, 0);
    Button(ui->root, "返回", 580, 14, 106, BackClicked, nullptr);
    Board(ui->root, ui);
    ui->mode = Text(ui->root, "", 585, 108);
    lv_obj_set_width(ui->mode, 110); lv_label_set_long_mode(ui->mode, LV_LABEL_LONG_WRAP);
    Text(ui->root, "红方先行\n\n点击棋子\n绿色可走\n金色上步", 585, 195);
    lv_obj_t *level_button=Button(ui->root,"",570,350,126,LevelClicked,nullptr);
    ui->level=lv_obj_get_child(level_button,0);
    Button(ui->root, "切换模式", 570, 420, 126, ResetClicked, (void *)1);
    ui->undo = Button(ui->root, "悔棋", 570, 488, 126, UndoClicked, nullptr);
    Button(ui->root, "新对局", 570, 556, 126, ResetClicked, nullptr);
    ui->status = Text(ui->root, "", 40, 655);
    screen_attach_swipe_back(ui->root, Back);
    screen_attach_left_edge_back(ui->root, 16, Back);
    Refresh(ui);
    return ui->root;
}
lv_obj_t *XiangqiScreen::Create() { root_ = CreateStatic(); return root_; }
void XiangqiScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (!active) return;
    if (event == SCREEN_LIFECYCLE_UNLOAD) CancelCPU(active);
    else StartCPU(active);
}
lv_obj_t *XiangqiScreen::CreateIcon(lv_obj_t *parent, int size) {
    auto *icon=lv_image_create(parent);
    lv_image_set_src(icon,"A:ic_app_home_theme1_xiangqi.spng");
    lv_obj_set_size(icon,size,size);
    lv_image_set_scale(icon,size*256/128);
    lv_image_set_pivot(icon,0,0);
    lv_obj_remove_flag(icon,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));
    return icon;
}
