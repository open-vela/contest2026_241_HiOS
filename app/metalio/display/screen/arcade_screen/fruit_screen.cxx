#include "arcade_screen.h"

#include "arcade_engine.h"
#include "home_screen/home_screen.h"
#include "screen_util.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>

LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr int kBoardSide = 8;
constexpr int kTileCount = kBoardSide * kBoardSide;
constexpr int kGlyphParts = 12;
constexpr uint32_t kGoalBase = 500;

struct FruitGlyph {
    lv_obj_t *part[kGlyphParts]{};
};

struct Fruit {
    Arcade::Match game;
    lv_obj_t *root = nullptr;
    lv_obj_t *status = nullptr;
    lv_obj_t *hint = nullptr;
    lv_obj_t *tiles[kTileCount]{};
    FruitGlyph glyphs[kTileCount]{};
    int shown[kTileCount]{};
    int shown_mark[kTileCount]{};
    int selected = -1;
    int pressed = -1;
    int hint_a = -1;
    int hint_b = -1;
    lv_point_t press_point{};
    bool skip_click = false;
    bool swapping = false;
    int swap_a = -1;
    int swap_b = -1;
};

constexpr uint32_t kCellColors[] = {0, 0xFFF1EB, 0xFFF7D9, 0xFFEAF1,
                                    0xF0EAFF, 0xFFF0DA, 0xFFEAF0};
constexpr uint32_t kCellEdges[] = {0, 0xC95C68, 0xD5B94D, 0xE37D99,
                                   0x9C83E2, 0xE9973C, 0xDD5269};

void Place(lv_obj_t *obj, int x, int y, int w, int h,
           uint32_t color, int radius)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
}

void Put(FruitGlyph &glyph, int index, int x, int y, int w, int h,
         uint32_t color, int radius)
{
    Place(glyph.part[index], x, y, w, h, color, radius);
}

void HideGlyph(FruitGlyph &glyph)
{
    for (lv_obj_t *part : glyph.part) {
        lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
    }
}

void DrawFruit(FruitGlyph &glyph, int type, int x_offset = 0,
               int y_offset = 0, float scale = 1.0f)
{
    HideGlyph(glyph);
    auto put = [&](int i, int x, int y, int w, int h,
                   uint32_t color, int radius) {
        Put(glyph, i, x_offset + static_cast<int>(x * scale + 0.5f),
            y_offset + static_cast<int>(y * scale + 0.5f),
            static_cast<int>(w * scale + 0.5f),
            static_cast<int>(h * scale + 0.5f), color,
            static_cast<int>(radius * scale + 0.5f));
    };

    switch (type) {
    case 1: // glossy red apple
        put(0, 18, 17, 36, 34, 0xAE3548, 17);
        put(1, 20, 16, 32, 31, 0xF04452, 16);
        put(2, 21, 18, 14, 21, 0xFF6870, 8);
        put(3, 38, 18, 12, 21, 0xD93349, 7);
        put(4, 34, 8, 5, 13, 0x68422F, 2);
        put(5, 38, 6, 17, 9, 0x51B879, 7);
        put(6, 25, 21, 5, 12, 0xFFE0D2, 2);
        put(7, 26, 38, 8, 3, 0xF7A6A2, 2);
        break;
    case 2: // golden pear
        put(0, 20, 19, 34, 34, 0xB77B2B, 17);
        put(1, 26, 10, 22, 28, 0xF4C83F, 11);
        put(2, 20, 27, 34, 25, 0xF1B72D, 16);
        put(3, 27, 13, 11, 18, 0xFFE16B, 6);
        put(4, 35, 7, 4, 12, 0x68422F, 2);
        put(5, 39, 6, 16, 8, 0x54B56D, 7);
        put(6, 27, 27, 4, 12, 0xFFF1A0, 2);
        put(7, 47, 34, 3, 4, 0xD9952A, 2);
        break;
    case 3: // blush peach
        put(0, 18, 16, 37, 36, 0xBB5471, 18);
        put(1, 20, 15, 33, 34, 0xF58DA7, 17);
        put(2, 21, 17, 15, 25, 0xFFB1BC, 9);
        put(3, 37, 18, 14, 25, 0xE96C8D, 8);
        put(4, 35, 7, 4, 13, 0x68422F, 2);
        put(5, 39, 6, 16, 8, 0x54B56D, 7);
        put(6, 35, 20, 2, 21, 0xCE5C7C, 1);
        put(7, 25, 22, 4, 9, 0xFFF0DD, 2);
        break;
    case 4: // purple grape bunch
        put(0, 24, 13, 15, 15, 0x6045A5, 8);
        put(1, 39, 13, 15, 15, 0x503994, 8);
        put(2, 17, 26, 15, 15, 0x7454C6, 8);
        put(3, 32, 26, 15, 15, 0x5941AA, 8);
        put(4, 47, 26, 12, 15, 0x7554C4, 7);
        put(5, 24, 39, 15, 13, 0x5B43AD, 8);
        put(6, 39, 39, 15, 13, 0x704FC0, 8);
        put(7, 29, 7, 23, 10, 0x51B56C, 7);
        put(8, 27, 16, 4, 7, 0xC5B4FF, 2);
        put(9, 42, 17, 4, 7, 0xB5A1F4, 2);
        put(10, 22, 29, 4, 7, 0xC5B4FF, 2);
        put(11, 34, 29, 4, 7, 0xB5A1F4, 2);
        break;
    case 5: // sun-ripened orange
        put(0, 19, 14, 37, 38, 0xBB5F24, 19);
        put(1, 20, 13, 36, 36, 0xF58A24, 18);
        put(2, 24, 17, 27, 27, 0xFFAA35, 14);
        put(3, 36, 6, 4, 12, 0x68422F, 2);
        put(4, 39, 6, 17, 9, 0x4FAC68, 7);
        put(5, 26, 21, 5, 11, 0xFFF0B8, 2);
        put(6, 22, 35, 5, 4, 0xFFD071, 2);
        put(7, 47, 30, 4, 4, 0xFFD071, 2);
        break;
    case 6: // ripe strawberry
        put(0, 21, 16, 31, 37, 0xAD314C, 16);
        put(1, 20, 17, 32, 27, 0xF0445E, 15);
        put(2, 23, 30, 26, 20, 0xE93857, 12);
        put(3, 24, 8, 26, 11, 0x43A864, 6);
        put(4, 31, 4, 12, 10, 0x59BD74, 6);
        put(5, 27, 21, 3, 4, 0xFFE493, 2);
        put(6, 40, 22, 3, 4, 0xFFE493, 2);
        put(7, 23, 30, 3, 4, 0xFFE493, 2);
        put(8, 34, 32, 3, 4, 0xFFE493, 2);
        put(9, 45, 31, 3, 4, 0xFFE493, 2);
        put(10, 31, 43, 3, 4, 0xFFE493, 2);
        put(11, 36, 17, 4, 9, 0xFF8590, 2);
        break;
    default:
        break;
    }
}

lv_obj_t *MakeLabel(lv_obj_t *parent, const char *value, uint32_t color,
                    int font_size = 20)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, value);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    (void)font_size;
    return label;
}

int Goal(const Fruit *fruit)
{
    return static_cast<int>(kGoalBase + fruit->game.level * 100);
}

void Paint(Fruit *fruit)
{
    char status[96];
    std::snprintf(status, sizeof(status), "第 %d 关   步数 %d   分数 %d / %d",
                  fruit->game.level, fruit->game.moves,
                  fruit->game.score, Goal(fruit));
    lv_label_set_text(fruit->status, status);

    for (int i = 0; i < kTileCount; ++i) {
        const int type = fruit->game.cells[i / kBoardSide][i % kBoardSide];
        const bool marked = i == fruit->selected || i == fruit->hint_a ||
                            i == fruit->hint_b;
        const int mark = i == fruit->selected ? 1 :
                         (i == fruit->hint_a || i == fruit->hint_b) ? 2 : 0;
        if (fruit->shown[i] != type || fruit->shown_mark[i] != mark) {
            lv_obj_set_style_bg_color(fruit->tiles[i],
                                      lv_color_hex(kCellColors[type]), LV_PART_MAIN);
            lv_obj_set_style_border_width(fruit->tiles[i], marked ? 4 : 2,
                                          LV_PART_MAIN);
            lv_obj_set_style_border_color(fruit->tiles[i],
                lv_color_hex(mark == 2 ? 0xFFE281 :
                             mark == 1 ? 0xFFFFFF : kCellEdges[type]), LV_PART_MAIN);
            fruit->shown_mark[i] = mark;
        }
        if (fruit->shown[i] != type) {
            DrawFruit(fruit->glyphs[i], type, 2, 0, 0.90f);
            fruit->shown[i] = type;
        }
    }

    if (fruit->game.score >= Goal(fruit)) {
        lv_label_set_text(fruit->hint, "过关成功！点击下一关");
    } else if (fruit->game.moves <= 0) {
        lv_label_set_text(fruit->hint, "步数用完，点击重开再挑战");
    }
}

void SetHint(Fruit *fruit, const char *message)
{
    lv_label_set_text(fruit->hint, message);
}

void FinishSwapAnimation(lv_anim_t *anim)
{
    auto *fruit = static_cast<Fruit *>(lv_anim_get_user_data(anim));
    if (!fruit || !fruit->swapping) return;
    const int a = fruit->swap_a;
    const int b = fruit->swap_b;
    if (fruit->game.swap(a, b)) SetHint(fruit, "消除成功，水果已补齐");
    fruit->swapping = false;
    fruit->selected = -1;
    lv_obj_set_pos(fruit->tiles[a], 44 + (a % kBoardSide) * 80,
                   76 + (a / kBoardSide) * 64);
    lv_obj_set_pos(fruit->tiles[b], 44 + (b % kBoardSide) * 80,
                   76 + (b / kBoardSide) * 64);
    Paint(fruit);
}

void AnimateSwap(Fruit *fruit, int a, int b)
{
    if (fruit->swapping || !fruit->game.canSwap(a, b)) return;
    fruit->swapping = true;
    fruit->swap_a = a;
    fruit->swap_b = b;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, fruit);
    lv_anim_set_user_data(&anim, fruit);
    lv_anim_set_values(&anim, 0, 1000);
    lv_anim_set_duration(&anim, 125);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, [](void *value, int32_t progress) {
        auto *state = static_cast<Fruit *>(value);
        const int a = state->swap_a, b = state->swap_b;
        const int ax = 44 + (a % kBoardSide) * 80;
        const int ay = 76 + (a / kBoardSide) * 64;
        const int bx = 44 + (b % kBoardSide) * 80;
        const int by = 76 + (b / kBoardSide) * 64;
        lv_obj_set_pos(state->tiles[a], ax + (bx - ax) * progress / 1000,
                       ay + (by - ay) * progress / 1000);
        lv_obj_set_pos(state->tiles[b], bx + (ax - bx) * progress / 1000,
                       by + (ay - by) * progress / 1000);
    });
    lv_anim_set_completed_cb(&anim, FinishSwapAnimation);
    lv_anim_start(&anim);
}

void SwapBySwipe(Fruit *fruit, lv_event_t *event)
{
    if (fruit->pressed < 0 || lv_indev_active() == nullptr) return;
    lv_point_t point{};
    lv_indev_get_point(lv_indev_active(), &point);
    const int dx = point.x - fruit->press_point.x;
    const int dy = point.y - fruit->press_point.y;
    if (std::abs(dx) < 24 && std::abs(dy) < 24) return;

    int other = fruit->pressed;
    if (std::abs(dx) >= std::abs(dy)) {
        if (dx > 0 && other % kBoardSide < kBoardSide - 1) ++other;
        else if (dx < 0 && other % kBoardSide > 0) --other;
        else return;
    } else {
        if (dy > 0 && other / kBoardSide < kBoardSide - 1) other += kBoardSide;
        else if (dy < 0 && other / kBoardSide > 0) other -= kBoardSide;
        else return;
    }

    fruit->skip_click = true;
    fruit->selected = -1;
    fruit->hint_a = fruit->hint_b = -1;
    if (fruit->game.score < Goal(fruit) && fruit->game.canSwap(fruit->pressed, other)) {
        SetHint(fruit, "图案交换中…");
        AnimateSwap(fruit, fruit->pressed, other);
    } else {
        SetHint(fruit, "交换未形成消除");
        Paint(fruit);
    }
    (void)event;
}

void OnTilePressed(lv_event_t *event)
{
    Fruit *fruit = static_cast<Fruit *>(lv_event_get_user_data(event));
    lv_obj_t *tile = lv_event_get_current_target_obj(event);
    fruit->pressed = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_obj_get_user_data(tile)));
    fruit->skip_click = false;
    if (lv_indev_active() != nullptr) {
        lv_indev_get_point(lv_indev_active(), &fruit->press_point);
    }
}

void OnTileReleased(lv_event_t *event)
{
    Fruit *fruit = static_cast<Fruit *>(lv_event_get_user_data(event));
    SwapBySwipe(fruit, event);
    fruit->pressed = -1;
}

void OnTileClicked(lv_event_t *event)
{
    Fruit *fruit = static_cast<Fruit *>(lv_event_get_user_data(event));
    if (fruit->swapping) return;
    if (fruit->skip_click) {
        fruit->skip_click = false;
        return;
    }
    if (fruit->game.moves <= 0 || fruit->game.score >= Goal(fruit)) return;

    lv_obj_t *tile = lv_event_get_current_target_obj(event);
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_obj_get_user_data(tile)));
    fruit->hint_a = fruit->hint_b = -1;
    if (fruit->selected < 0) {
        fruit->selected = index;
        SetHint(fruit, "再点一个相邻水果交换");
    } else {
        const int previous = fruit->selected;
        if (fruit->game.canSwap(previous, index) && fruit->game.score < Goal(fruit)) {
            fruit->hint_a = fruit->hint_b = -1;
            SetHint(fruit, "图案交换中…");
            AnimateSwap(fruit, previous, index);
        } else {
            fruit->selected = index;
            SetHint(fruit, previous == index ? "再点一个相邻水果交换" :
                    "请选择相邻水果，交换后需连成三个");
        }
    }
    Paint(fruit);
}

void OnAction(lv_event_t *event)
{
    Fruit *fruit = static_cast<Fruit *>(lv_event_get_user_data(event));
    lv_obj_t *button = lv_event_get_current_target_obj(event);
    const int action = static_cast<int>(reinterpret_cast<intptr_t>(
        lv_obj_get_user_data(button)));

    if (fruit->swapping) return;
    if (action == 0) {
        HomeScreen::SwitchToHome();
        return;
    }
    if (action == 1) {
        fruit->game.reset(fruit->game.level);
    } else if (action == 2) {
        if (!fruit->game.hint(fruit->hint_a, fruit->hint_b)) {
            SetHint(fruit, "没有可消除的一组");
        } else {
            SetHint(fruit, "白框标出可交换的一对");
        }
    } else if (action == 3) {
        fruit->game.shuffle();
        SetHint(fruit, "已重新排列水果");
    } else if (action == 4) {
        if (fruit->game.score < Goal(fruit)) {
            SetHint(fruit, "达到目标分数后可进入下一关");
            return;
        }
        fruit->game.reset(fruit->game.level + 1);
    }

    fruit->selected = -1;
    if (action != 2) fruit->hint_a = fruit->hint_b = -1;
    Paint(fruit);
}

void OnDelete(lv_event_t *event)
{
    auto *fruit = static_cast<Fruit *>(lv_event_get_user_data(event));
    lv_anim_delete(fruit, nullptr);
    delete fruit;
}

void SwipeBackToHome()
{
    HomeScreen::SwitchToHome();
}

void MakeGlyph(FruitGlyph &glyph, lv_obj_t *parent)
{
    for (lv_obj_t *&part : glyph.part) {
        part = lv_obj_create(parent);
        lv_obj_remove_style_all(part);
        lv_obj_remove_flag(part, static_cast<lv_obj_flag_t>(
            LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *MakeTextButton(lv_obj_t *parent, const char *caption, int x,
                         lv_event_cb_t callback, int action, Fruit *fruit)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, 654);
    lv_obj_set_size(button, 124, 48);
    static const uint32_t colors[] = {0x6878BC, 0x6878BC, 0x6978CE,
                                      0x6978CE, 0xA167B4};
    lv_obj_set_style_bg_color(button, lv_color_hex(colors[action]), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x6F7892), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(0x64718A), LV_PART_MAIN);
    lv_obj_set_style_radius(button, 22, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(button, lv_color_hex(0x080C13), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(button, LV_OPA_30, LV_PART_MAIN);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(button, reinterpret_cast<void *>(
        static_cast<intptr_t>(action)));
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, fruit);
    screen_swipe_back_ignore(button, true);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, caption);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFF7FA), LV_PART_MAIN);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(label);
    return button;
}

} // namespace

lv_obj_t *FruitGameScreen::CreateStatic()
{
    Fruit *fruit = new Fruit{};
    fruit->root = lv_obj_create(nullptr);
    lv_obj_remove_flag(fruit->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(fruit->root, lv_color_hex(0xEAF0FC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fruit->root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(fruit->root, OnDelete, LV_EVENT_DELETE, fruit);
    fruit->game.reset();

    lv_obj_t *header = lv_obj_create(fruit->root);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 16, 10);
    lv_obj_set_size(header, 688, 54);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xD7E1F6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(header, lv_color_hex(0xBBCDEB), LV_PART_MAIN);
    lv_obj_set_style_radius(header, 22, LV_PART_MAIN);
    lv_obj_remove_flag(header, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t *board = lv_obj_create(fruit->root);
    lv_obj_remove_style_all(board);
    lv_obj_set_pos(board, 22, 68);
    lv_obj_set_size(board, 676, 516);
    lv_obj_set_style_bg_color(board, lv_color_hex(0xD7E1F6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(board, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(board, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(board, lv_color_hex(0xBBCDEB), LV_PART_MAIN);
    lv_obj_set_style_radius(board, 28, LV_PART_MAIN);
    lv_obj_remove_flag(board, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    fruit->status = MakeLabel(fruit->root, "", 0x303E75);
    lv_obj_set_pos(fruit->status, 34, 25);
    fruit->hint = MakeLabel(fruit->root, "点两个相邻水果交换，凑成三个", 0x4D5B7B);
    lv_obj_set_pos(fruit->hint, 24, 594);
    lv_obj_set_style_bg_color(fruit->hint, lv_color_hex(0xD7E1F6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fruit->hint, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(fruit->hint, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(fruit->hint, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(fruit->hint, 8, LV_PART_MAIN);
    lv_obj_set_width(fruit->hint, 672);

    for (int i = 0; i < kTileCount; ++i) {
        lv_obj_t *tile = lv_button_create(fruit->root);
        fruit->tiles[i] = tile;
        fruit->shown[i] = -1;
        fruit->shown_mark[i] = -1;
        lv_obj_set_pos(tile, 44 + (i % kBoardSide) * 80,
                       76 + (i / kBoardSide) * 64);
        lv_obj_set_size(tile, 72, 56);
        lv_obj_set_style_radius(tile, 13, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(tile, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x9A5360), LV_STATE_PRESSED);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_state(tile, LV_STATE_DISABLED);
        lv_obj_set_user_data(tile, reinterpret_cast<void *>(
            static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(tile, OnTilePressed, LV_EVENT_PRESSED, fruit);
        lv_obj_add_event_cb(tile, OnTileReleased, LV_EVENT_RELEASED, fruit);
        lv_obj_add_event_cb(tile, OnTileClicked, LV_EVENT_PRESSED, fruit);
        screen_swipe_back_ignore(tile, true);
        MakeGlyph(fruit->glyphs[i], tile);
    }

    static const char *captions[] = {"返回", "重开", "提示", "洗牌", "下一关"};
    for (int i = 0; i < 5; ++i) {
        MakeTextButton(fruit->root, captions[i], 20 + i * 140,
                       OnAction, i, fruit);
    }

    Paint(fruit);
    screen_attach_swipe_back(fruit->root, SwipeBackToHome);
    return fruit->root;
}

lv_obj_t *FruitGameScreen::CreateIcon(lv_obj_t *parent, int size) {
    auto *icon=lv_image_create(parent);
    lv_image_set_src(icon,"A:ic_app_home_theme1_fruit_game.spng");
    lv_obj_set_size(icon,size,size);
    lv_image_set_scale(icon,size*256/128);
    lv_image_set_pivot(icon,0,0);
    lv_obj_remove_flag(icon,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));
    return icon;
}
