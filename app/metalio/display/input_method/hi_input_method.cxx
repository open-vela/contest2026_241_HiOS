// SPDX-License-Identifier: Apache-2.0
#include "hi_input_method.h"
#include "pinyin_engine.h"
#include "../../../graphics/lvgl/lvgl/src/widgets/buttonmatrix/lv_buttonmatrix_private.h"
#include "standby_screen/standby_screen.h"
#include "clock_service.h"
#include <cstdio>
#include <cstring>
#include <new>
#include <algorithm>
#include <string>
#include <vector>
#include <initializer_list>
#include <cctype>
#include <cstdlib>
#include "screen_util.h"
#include "settings.h"
#include "system_chrome/control_center.h"
#include "system_chrome/recents_overlay.h"

lv_obj_t *CreateKeyboard(lv_obj_t*);
void CoreSetTextArea(lv_obj_t*,lv_obj_t*,bool);
bool CoreFlush(lv_obj_t*);
void CoreSetLayout(lv_obj_t*,HiInputLayout);
HiInputLayout CoreGetLayout(lv_obj_t*);
namespace { void GlobalHide(bool); void GlobalReady(); void BoundField(lv_event_t*); }
static void EnsureGlobalPanel();
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
namespace {
constexpr size_t kPage = 12;
constexpr uint32_t kBg = 0xECEDEB, kKey = 0xFFFFFF, kBlue = 0x5E7750;
const char *const kNine[] = {"分词", "ABC", "DEF", "删除", "\n",
    "GHI", "JKL", "MNO", "全键", "\n", "PQRS", "TUV", "WXYZ", "符号", "\n",
    "123", "中/英", "空格", "换行", "完成", ""};
const char *const kLower[] = {"q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","\n",
    "大写","z","x","c","v","b","n","m","删除","\n",
    "123","符号","中/英","空格","九键","换行","完成",""};
const char *const kUpper[] = {"Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","\n",
    "小写","Z","X","C","V","B","N","M","删除","\n",
    "123","符号","中/英","空格","九键","换行","完成",""};
const char *const kNumber[] = {"1","2","3","删除","\n","4","5","6",".","\n",
    "7","8","9","-","\n","返回","0","空格","完成",""};
const char *const kSymbols[] = {"!","?",",",".","'","删除","\n",
    "@","#","/",":",";","_","\n","(",")","+","-","=","%","\n",
    "返回","123","中/英","空格","完成",""};
const char *const kChineseSymbols[] = {"，","。","？","！","'","删除","\n",
    "、","：","；","（","）","@","\n","!","?","/",":","-","_","\n",
    "返回","123","中/英","空格","完成",""};
enum class Mode { Text, Numbers, Symbols };
HiInputLayout s_preferred_layout = HiInputLayout::Qwerty;
struct Context {
    lv_obj_t *panel = nullptr, *header = nullptr, *hide = nullptr,*mirror=nullptr,*tools=nullptr;
    lv_obj_t *preedit = nullptr, *candidates = nullptr, *spelling = nullptr;
    lv_obj_t *keyboard = nullptr, *expanded = nullptr, *textarea = nullptr, *about = nullptr;
    lv_obj_t *handle_start=nullptr,*handle_end=nullptr;
    lv_obj_t *candidate_labels[3]{},*expanded_labels[12]{};
    lv_obj_t *contact_matrix=nullptr;uint32_t contact_id=LV_BUTTONMATRIX_BUTTON_NONE;
    bool contact_delivered=false;lv_point_t contact_point{};
    int layout_width=-1,layout_height=-1,layout_mode=-1;
    bool edit=false,extending=false;
    int anchor=0;
    lv_obj_t *delete_icon=nullptr,*expand_icon=nullptr;
    const char *key_map[52]{};uint32_t delete_id=LV_BUTTONMATRIX_BUTTON_NONE;
    std::string *clipboard=nullptr;const char *const *normal_tools_map=nullptr;
    hios::PinyinEngine engine;
    hios::NineKeyEngine nine;
    HiInputLayout layout = s_preferred_layout;
    Mode mode = Mode::Text;
    bool chinese = true, allowed = true, upper = false, more = false;
    size_t page = 0, spelling_page = 0;
    size_t count = 0;
    char text[kPage][hios::PinyinEngine::kCandidateBytes] = {};
    char spell[5][7] = {};
    const char *candidate_map[6] = {}, *expanded_map[20] = {}, *spelling_map[8] = {};
};
Context *Get(lv_obj_t *p) { return static_cast<Context *>(lv_obj_get_user_data(p)); }
bool Allowed(Context *c) {
    return c->allowed && c->textarea && !lv_textarea_get_password_mode(c->textarea) &&
           !lv_textarea_get_accepted_chars(c->textarea);
}
bool Chinese(Context *c) { return c->chinese && Allowed(c); }
bool Nine(Context *c) { return Chinese(c) && c->layout == HiInputLayout::NineKey; }
const char *Input(Context *c) { return Nine(c) ? c->nine.Preedit() : c->engine.Preedit(); }
size_t Count(Context *c) { return Nine(c) ? c->nine.Count() : c->engine.Count(); }
bool Candidate(Context *c, size_t index, char *out, size_t cap) {
    return Nine(c) ? c->nine.Candidate(index, out, cap) : c->engine.Candidate(index, out, cap);
}
void Clear(Context *c) { c->engine.Clear(); c->nine.Clear(); c->page = c->spelling_page = 0; }
size_t Characters(const char *text) {
    size_t count = 0;
    for (; *text; ++text) if ((static_cast<unsigned char>(*text) & 0xc0) != 0x80) ++count;
    return count;
}
void Refresh(Context*,bool);
void SelectWord(Context*);
std::string &SharedClipboard(){static std::string text;return text;}
std::string &Clipboard(Context*c){return *c->clipboard;} // Single keyboard owns the existing lazy clipboard.
size_t ByteAt(const std::string &text,size_t character){size_t p=0;while(p<text.size()&&character){++p;while(p<text.size()&&(static_cast<unsigned char>(text[p])&0xc0)==0x80)++p;--character;}return p;}
bool Selection(lv_obj_t *ta,uint32_t &a,uint32_t &b){
    if(!ta)return false;auto*label=lv_textarea_get_label(ta);
    a=lv_label_get_text_selection_start(label);b=lv_label_get_text_selection_end(label);
    if(a==LV_LABEL_TEXT_SELECTION_OFF||b==LV_LABEL_TEXT_SELECTION_OFF||a==b)return false;
    if(a>b)std::swap(a,b);uint32_t count=Characters(lv_textarea_get_text(ta));a=std::min(a,count);b=std::min(b,count);return a<b;
}
void SetSelection(lv_obj_t *ta,uint32_t a,uint32_t b){
    if(!ta)return;auto*label=lv_textarea_get_label(ta);if(a>b)std::swap(a,b);
    lv_label_set_text_selection_start(label,a);lv_label_set_text_selection_end(label,b);lv_obj_invalidate(ta);
}
bool DeleteSelection(Context*c){
    uint32_t a,b;if(!Selection(c->textarea,a,b))return false;
    std::string text=lv_textarea_get_text(c->textarea);text.erase(ByteAt(text,a),ByteAt(text,b)-ByteAt(text,a));
    lv_textarea_set_text(c->textarea,text.c_str());lv_textarea_set_cursor_pos(c->textarea,a);lv_textarea_clear_selection(c->textarea);
    c->extending=false;return true;
}
bool Insert(Context *c,const char *text){
    if(!c->textarea||!text)return false;
    std::string accepted;const char *filter=lv_textarea_get_accepted_chars(c->textarea);
    // Let native LVGL validate each accepted character as well. Filter before
    // erasing a selection, so a rejected numeric/newline paste loses no text.
    for(size_t i=0;text[i];){size_t end=i+1;while(text[end]&&(static_cast<unsigned char>(text[end])&0xc0)==0x80)++end;
        std::string ch(text+i,end-i);bool allow=!filter||std::strstr(filter,ch.c_str());
        if(lv_textarea_get_one_line(c->textarea)&&(ch=="\n"||ch=="\r"))allow=false;
        if(allow)accepted+=ch;i=end;
    }
    if(accepted.empty())return false;
    uint32_t a=0,b=0;size_t selected=Selection(c->textarea,a,b)?b-a:0;
    uint32_t max=lv_textarea_get_max_length(c->textarea);
    if(max && Characters(lv_textarea_get_text(c->textarea))-selected+Characters(accepted.c_str())>max)return false;
    DeleteSelection(c);size_t before=std::strlen(lv_textarea_get_text(c->textarea));
    lv_textarea_add_text(c->textarea,accepted.c_str());
    return std::strlen(lv_textarea_get_text(c->textarea))==before+accepted.size();
}
void Copy(Context*c,bool cut){
    if(!c->textarea||lv_textarea_get_password_mode(c->textarea)||!CoreFlush(c->panel))return;
    uint32_t a,b;if(!Selection(c->textarea,a,b)){SelectWord(c);if(!Selection(c->textarea,a,b))return;}
    std::string text=lv_textarea_get_text(c->textarea);
    const size_t bytes=ByteAt(text,b)-ByteAt(text,a);if(bytes>65536)return;
    Clipboard(c)=text.substr(ByteAt(text,a),bytes);if(cut)DeleteSelection(c);Refresh(c,false);
}
void UpdateHandles(Context*c){
    if(!c->handle_start)return;uint32_t a,b;
    bool visible=c->edit&&Selection(c->textarea,a,b)&&!lv_textarea_get_password_mode(c->textarea);
    for(unsigned end=0;end<2;++end){auto*h=end?c->handle_end:c->handle_start;
        if(!visible){lv_obj_add_flag(h,LV_OBJ_FLAG_HIDDEN);continue;}
        lv_point_t p;auto*label=lv_textarea_get_label(c->mirror);lv_label_get_letter_pos(label,end?b:a,&p);
        lv_area_t la,pa,ma;lv_obj_get_coords(label,&la);lv_obj_get_coords(c->panel,&pa);lv_obj_get_coords(c->mirror,&ma);
        int x=la.x1+p.x;if(x<ma.x1||x>ma.x2){lv_obj_add_flag(h,LV_OBJ_FLAG_HIDDEN);continue;}
        lv_obj_set_pos(h,x-pa.x1-14,ma.y2-pa.y1-7);lv_obj_remove_flag(h,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(h);
    }
}
void ToolsMap(Context*c){
    static const char *const editing[]={"全选","选词","复制","剪切","粘贴","返回",""};
    lv_buttonmatrix_set_map(c->tools,c->edit?editing:c->normal_tools_map);
    lv_buttonmatrix_set_button_ctrl_all(c->tools,static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CLICK_TRIG|LV_BUTTONMATRIX_CTRL_NO_REPEAT));
}
void SelectWord(Context*c){
    if(!c->textarea||lv_textarea_get_password_mode(c->textarea)||!CoreFlush(c->panel))return;
    std::string text=lv_textarea_get_text(c->textarea);
    size_t count=Characters(text.c_str());if(!count)return;
    size_t a=std::min<size_t>(lv_textarea_get_cursor_pos(c->textarea),count-1),b=a+1;
    auto word=[&text](size_t character){unsigned char ch=text[ByteAt(text,character)];return ch<128&&(std::isalnum(ch)||ch=='_');};
    // Ordinary cursor selection stays available without the segmentation UI.
    if(word(a)){while(a&&word(a-1))--a;while(b<count&&word(b))++b;}
    c->edit=true;SetSelection(c->textarea,a,b);ToolsMap(c);Refresh(c,false);
}
void SetHidden(lv_obj_t *obj, bool hidden) {
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
void PlaceCandidate(lv_obj_t*matrix,lv_obj_t*label,uint32_t id){
    if(!label)return;auto*b=reinterpret_cast<lv_buttonmatrix_t*>(matrix);
    if(id>=b->btn_cnt)return;const auto&a=b->button_areas[id];
    lv_obj_set_pos(label,a.x1+6,(a.y1+a.y2+1-24)/2);
    lv_obj_set_size(label,std::max(1,static_cast<int>(a.x2-a.x1-11)),24);
}
void PlaceKeyIcon(lv_obj_t*matrix,lv_obj_t*image,uint32_t id){
    if(!matrix||!image)return;
    auto*buttons=reinterpret_cast<lv_buttonmatrix_t*>(matrix);
    if(id>=buttons->btn_cnt){lv_obj_add_flag(image,LV_OBJ_FLAG_HIDDEN);return;}
    lv_obj_update_layout(matrix);const auto &a=buttons->button_areas[id];
    lv_obj_set_pos(image,(a.x1+a.x2+1-lv_obj_get_width(image))/2,(a.y1+a.y2+1-lv_obj_get_height(image))/2);
    lv_obj_remove_flag(image,LV_OBJ_FLAG_HIDDEN);
}
void Layout(Context *c) {
    int width = lv_obj_get_content_width(c->panel), height = lv_obj_get_content_height(c->panel);
    if (width < 1 || height < 180) return;
    int mode=(Chinese(c)?1:0)|(Nine(c)?2:0)|(c->more?4:0)|(static_cast<int>(c->mode)<<3);
    if(c->layout_width==width&&c->layout_height==height&&c->layout_mode==mode)return;
    c->layout_width=width;c->layout_height=height;c->layout_mode=mode;
    // Compact four-row panel: every row remains inside the visible bounds.
    lv_obj_set_pos(c->tools,6,0);lv_obj_set_size(c->tools,width-12,32);
    lv_obj_set_pos(c->mirror,8,34);lv_obj_set_size(c->mirror,width-16,34);
    // Keep one complete font line inside this fixed-height mirror. The old
    // 6+6 padding plus theme border left less room than the font line height,
    // so cursor updates alternately scrolled to its top and bottom.
    const int mirror_space=std::max(0,32-static_cast<int>(lv_font_get_line_height(&font_puhui_20_4)));
    lv_obj_set_style_pad_top(c->mirror,mirror_space/2,0);
    lv_obj_set_style_pad_bottom(c->mirror,mirror_space-mirror_space/2,0);
    lv_obj_set_pos(c->header,8,70);lv_obj_set_size(c->header,width-16,18);
    lv_obj_set_width(c->preedit,width-32);SetHidden(c->hide,true);
    const bool composition=Chinese(c)&&c->mode==Mode::Text;
    SetHidden(c->header,!composition);SetHidden(c->candidates,!composition);
    lv_obj_set_size(c->candidates,width-16,40);lv_obj_set_pos(c->candidates,8,90);
    // Reserve the nine-key spelling row before the first digit too. Moving
    // the key grid after the first tap made the next tap hit a different row.
    bool spell_space=Nine(c)&&c->mode==Mode::Text;
    SetHidden(c->spelling,!(spell_space&&!c->more));
    lv_obj_set_pos(c->spelling,8,132);lv_obj_set_size(c->spelling,width-16,28);
    int y=composition?(spell_space?162:132):74;
    lv_obj_set_pos(c->keyboard,6,y);lv_obj_set_size(c->keyboard,width-12,height-y-6);
    lv_obj_set_pos(c->expanded,8,134);lv_obj_set_size(c->expanded,width-16,height-140);
    SetHidden(c->keyboard, c->more);
    SetHidden(c->expanded, !c->more);
    PlaceKeyIcon(c->keyboard,c->delete_icon,c->delete_id);PlaceKeyIcon(c->candidates,c->expand_icon,3);
    for(size_t i=0;i<3;++i)PlaceCandidate(c->candidates,c->candidate_labels[i],i);
    for(size_t i=0;i<12;++i)PlaceCandidate(c->expanded,c->expanded_labels[i],i);
    if(c->expand_icon)lv_image_set_rotation(c->expand_icon,c->more?1800:0);
}
alignas(32) const uint8_t kImeDeletePixels[]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,51,0,0,0,213,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,226,0,0,0,72,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,13,0,0,0,226,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,226,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,143,0,0,0,255,0,0,0,202,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,51,0,0,0,252,0,0,0,251,0,0,0,48,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,105,0,0,0,22,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,22,0,0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,204,0,0,0,255,0,0,0,139,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,106,0,0,0,255,0,0,0,211,0,0,0,22,0,0,0,0,0,0,0,0,0,0,0,22,0,0,0,211,0,0,0,255,0,0,0,106,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,112,0,0,0,255,0,0,0,225,0,0,0,12,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,25,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,29,0,0,0,242,0,0,0,255,0,0,0,76,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,22,0,0,0,211,0,0,0,255,0,0,0,211,0,0,0,211,0,0,0,255,0,0,0,211,0,0,0,22,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,175,0,0,0,255,0,0,0,171,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,255,0,0,0,215,0,0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,175,0,0,0,255,0,0,0,171,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,255,0,0,0,215,0,0,0,25,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,29,0,0,0,242,0,0,0,255,0,0,0,76,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,22,0,0,0,211,0,0,0,255,0,0,0,211,0,0,0,211,0,0,0,255,0,0,0,211,0,0,0,22,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,112,0,0,0,255,0,0,0,225,0,0,0,12,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,24,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,25,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,204,0,0,0,255,0,0,0,139,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,106,0,0,0,255,0,0,0,211,0,0,0,22,0,0,0,0,0,0,0,0,0,0,0,22,0,0,0,211,0,0,0,255,0,0,0,106,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,51,0,0,0,252,0,0,0,251,0,0,0,48,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,105,0,0,0,22,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,22,0,0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,143,0,0,0,255,0,0,0,202,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,13,0,0,0,226,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,226,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,51,0,0,0,212,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,226,0,0,0,72,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
const lv_image_dsc_t kImeDeleteIcon=[]{lv_image_dsc_t d{};d.header.magic=LV_IMAGE_HEADER_MAGIC;d.header.cf=LV_COLOR_FORMAT_ARGB8888;d.header.w=24;d.header.h=24;d.header.stride=128;d.data=kImeDeletePixels;d.data_size=sizeof(kImeDeletePixels);return d;}();
alignas(32) const uint8_t kImeExpandPixels[]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,194,0,0,0,204,0,0,0,22,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,22,0,0,0,205,0,0,0,195,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,202,0,0,0,255,0,0,0,211,0,0,0,22,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,22,0,0,0,211,0,0,0,255,0,0,0,201,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,25,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,25,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,25,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,25,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,25,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,25,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,25,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,215,0,0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,22,0,0,0,211,0,0,0,255,0,0,0,211,0,0,0,211,0,0,0,255,0,0,0,211,0,0,0,22,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,0,0,0,210,0,0,0,255,0,0,0,255,0,0,0,215,0,0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,22,0,0,0,203,0,0,0,203,0,0,0,22,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
const lv_image_dsc_t kImeExpandIcon=[]{lv_image_dsc_t d{};d.header.magic=LV_IMAGE_HEADER_MAGIC;d.header.cf=LV_COLOR_FORMAT_ARGB8888;d.header.w=30;d.header.h=30;d.header.stride=128;d.data=kImeExpandPixels;d.data_size=sizeof(kImeExpandPixels);return d;}();
void StyleMatrix(lv_obj_t *obj, int font_size, bool keys) {
    lv_obj_remove_style_all(obj); // No theme grow/translate/transition on input surfaces.
    lv_obj_remove_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE |
        LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_flag(obj,LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, keys ? 3 : 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(obj, keys ? 3 : 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(obj, keys ? 3 : 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(keys ? kKey : kBg), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xD1D6CA), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(obj, keys?1:0, LV_PART_ITEMS);
    lv_obj_set_style_border_color(obj,lv_color_hex(0xCED1CB),LV_PART_ITEMS);
    if(keys){lv_obj_set_style_bg_grad_color(obj,lv_color_hex(0xF3F4F0),LV_PART_ITEMS);lv_obj_set_style_bg_grad_dir(obj,LV_GRAD_DIR_VER,LV_PART_ITEMS);}
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(obj, keys ? 5 : 3, LV_PART_ITEMS);
    lv_obj_set_style_text_color(obj, lv_color_hex(0x464A47), LV_PART_ITEMS);
    lv_obj_set_style_text_color(obj, lv_color_hex(kBlue), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xDFE6D9), LV_PART_ITEMS | LV_STATE_CHECKED);
    if (keys) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(kBlue), LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFF), LV_PART_ITEMS | LV_STATE_CHECKED);
    }
    lv_obj_set_style_text_opa(obj, LV_OPA_30, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(obj, lv_color_hex(kBg), LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_text_font(obj, font_size == 30 ? &font_puhui_30_4 : &font_puhui_20_4, LV_PART_ITEMS);
}
void Controls(lv_obj_t *obj, size_t buttons, bool on_release=true) {
    // LVGL copies this array. Apply one control map, rather than recomputing
    // all button geometry once per button during every text refresh.
    lv_buttonmatrix_ctrl_t controls[40];
    if(buttons>40)return;
    for (size_t i = 0; i < buttons; ++i) {
        controls[i]=static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_1 |
            LV_BUTTONMATRIX_CTRL_NO_REPEAT | (on_release?LV_BUTTONMATRIX_CTRL_CLICK_TRIG:0));
    }
    lv_buttonmatrix_set_ctrl_map(obj,controls);
}
void Map(Context *c) {
    const char *const *map = c->mode == Mode::Numbers ? kNumber : c->mode == Mode::Symbols ?
        (Chinese(c) ? kChineseSymbols : kSymbols) : Nine(c) ? kNine : c->upper ? kUpper : kLower;
    size_t position=0;uint32_t key_id=0;c->delete_id=LV_BUTTONMATRIX_BUTTON_NONE;
    do {const char*key=map[position];c->key_map[position]=key;
        if(*key&&std::strcmp(key,"\n")){if(!std::strcmp(key,"删除")){c->delete_id=key_id;c->key_map[position]=" ";}++key_id;}
    }while(*map[position++] && position<52);
    c->contact_matrix=nullptr;c->layout_mode=-1;
    lv_buttonmatrix_set_map(c->keyboard,c->key_map);
    size_t count = 0;
    for (size_t i = 0; *map[i]; ++i) {
        if (!std::strcmp(map[i], "\n")) continue;
        ++count;
    }
    Controls(c->keyboard, count);
    size_t id = 0;
    for (size_t i = 0; *map[i]; ++i) {
        if (!std::strcmp(map[i], "\n")) continue;
        // Characters commit on the first press, once per touch. A small
        // release-coordinate wobble must not discard an already typed key.
        // Mode switches and Done stay on release so a newly installed map
        // cannot receive the remainder of the old touch as another command.
        const char *key=map[i];
        const bool command=!std::strcmp(key,"完成")||!std::strcmp(key,"123")||
            !std::strcmp(key,"符号")||!std::strcmp(key,"返回")||
            !std::strcmp(key,"中/英")||!std::strcmp(key,"九键")||
            !std::strcmp(key,"全键")||!std::strcmp(key,"大写")||!std::strcmp(key,"小写");
        if(!command)lv_buttonmatrix_clear_button_ctrl(c->keyboard,id,LV_BUTTONMATRIX_CTRL_CLICK_TRIG);
        if (!std::strcmp(map[i], "空格") && map != kNine && map != kNumber)
            lv_buttonmatrix_set_button_width(c->keyboard, id, 3);
        if (!std::strcmp(map[i], "删除")) {
            lv_buttonmatrix_clear_button_ctrl(c->keyboard, id, static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT));
        }
        if (!std::strcmp(map[i], "完成")) lv_buttonmatrix_set_button_ctrl(c->keyboard, id, LV_BUTTONMATRIX_CTRL_CHECKED);
        if (!std::strcmp(map[i], "中/英") && !Allowed(c))
            lv_buttonmatrix_set_button_ctrl(c->keyboard, id, LV_BUTTONMATRIX_CTRL_DISABLED);
        if (!std::strcmp(map[i], "九键") && !Chinese(c))
            lv_buttonmatrix_set_button_ctrl(c->keyboard, id, LV_BUTTONMATRIX_CTRL_DISABLED);
        ++id;
    }
}
void Refresh(Context *c, bool map_changed = false) {
    if(c->mirror){const char *text=c->textarea?lv_textarea_get_text(c->textarea):"";if(std::strcmp(text,lv_textarea_get_text(c->mirror)))lv_textarea_set_text(c->mirror,text);if(c->textarea)lv_textarea_set_cursor_pos(c->mirror,lv_textarea_get_cursor_pos(c->textarea));}
    if(c->mirror&&c->textarea){uint32_t a,b;if(Selection(c->textarea,a,b))SetSelection(c->mirror,a,b);else lv_textarea_clear_selection(c->mirror);}
    UpdateHandles(c);
    if (!Allowed(c)) Clear(c);
    c->count = Count(c);
    if (c->page * kPage >= c->count) c->page = 0;
    char hint[100];
    if (*Input(c)) {
        if (!Nine(c) || !c->nine.Preview(hint, sizeof(hint)))
            std::snprintf(hint, sizeof(hint), "%s", Input(c));
    } else std::snprintf(hint, sizeof(hint), "%s", !Chinese(c) ? "English" : Nine(c) ? "九宫格拼音" : c->engine.IsDoublePinyin()?"小鹤双拼 · 连续输入":"全拼 · 连续输入");
    lv_label_set_text(c->preedit, hint);
    for (size_t i = 0; i < (c->more?kPage:3); ++i)
        if (!Candidate(c, c->page * kPage + i, c->text[i], sizeof(c->text[i]))) std::strcpy(c->text[i], " ");
    for(size_t i=0;i<3;++i){
        if(std::strcmp(lv_label_get_text(c->candidate_labels[i]),c->text[i]))lv_label_set_text(c->candidate_labels[i],c->text[i]);
        const bool disabled=c->page*kPage+i>=c->count;
        if(disabled)lv_buttonmatrix_set_button_ctrl(c->candidates,i,LV_BUTTONMATRIX_CTRL_DISABLED);
        else lv_buttonmatrix_clear_button_ctrl(c->candidates,i,LV_BUTTONMATRIX_CTRL_DISABLED);
    }
    if(c->count)lv_buttonmatrix_clear_button_ctrl(c->candidates,3,LV_BUTTONMATRIX_CTRL_DISABLED);
    else lv_buttonmatrix_set_button_ctrl(c->candidates,3,LV_BUTTONMATRIX_CTRL_DISABLED);
    if(c->more){
        for(size_t i=0;i<kPage;++i){
            if(std::strcmp(lv_label_get_text(c->expanded_labels[i]),c->text[i]))lv_label_set_text(c->expanded_labels[i],c->text[i]);
            if(c->page*kPage+i>=c->count)lv_buttonmatrix_set_button_ctrl(c->expanded,i,LV_BUTTONMATRIX_CTRL_DISABLED);
            else lv_buttonmatrix_clear_button_ctrl(c->expanded,i,LV_BUTTONMATRIX_CTRL_DISABLED);
        }
        for(unsigned i=0;i<2;++i){const bool disabled=i?(c->page+1)*kPage>=c->count:!c->page;
            if(disabled)lv_buttonmatrix_set_button_ctrl(c->expanded,kPage+i*2,LV_BUTTONMATRIX_CTRL_DISABLED);
            else lv_buttonmatrix_clear_button_ctrl(c->expanded,kPage+i*2,LV_BUTTONMATRIX_CTRL_DISABLED);}
    }
    size_t spell_count = Nine(c)?c->nine.SpellingCount():0;
    if (c->spelling_page * 5 >= spell_count) c->spelling_page = 0;
    c->spelling_map[0] = "<";
    for (size_t i = 0; i < 5; ++i) {
        if (!c->nine.Spelling(c->spelling_page * 5 + i, c->spell[i], sizeof(c->spell[i]))) std::strcpy(c->spell[i], " ");
        c->spelling_map[i + 1] = c->spell[i];
    }
    c->spelling_map[6] = ">"; c->spelling_map[7] = "";
    // The map and button rectangles remain installed; only text is invalidated.
    lv_obj_invalidate(c->spelling);
    lv_buttonmatrix_clear_button_ctrl_all(c->spelling,static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_DISABLED|LV_BUTTONMATRIX_CTRL_CHECKED));
    for (size_t i = 0; i < 5; ++i) {
        if (c->spelling_page * 5 + i >= spell_count) lv_buttonmatrix_set_button_ctrl(c->spelling, i + 1, LV_BUTTONMATRIX_CTRL_DISABLED);
        if (!std::strcmp(c->spell[i], c->nine.SelectedSpelling())) lv_buttonmatrix_set_button_ctrl(c->spelling, i + 1, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
    if (!c->spelling_page) lv_buttonmatrix_set_button_ctrl(c->spelling, 0, LV_BUTTONMATRIX_CTRL_DISABLED);
    if ((c->spelling_page + 1) * 5 >= spell_count) lv_buttonmatrix_set_button_ctrl(c->spelling, 6, LV_BUTTONMATRIX_CTRL_DISABLED);
    if (map_changed) Map(c);
    Layout(c);
}
bool Choose(Context *c, size_t index) {
    char text[hios::PinyinEngine::kCandidateBytes];
    if (!Candidate(c, index, text, sizeof(text)) || !Insert(c, text)) {
        lv_label_set_text(c->preedit, "文字已满，请先删除"); return false;
    }
    if (Nine(c)) c->nine.Consume(index); else c->engine.Consume(index);
    c->page = c->spelling_page = 0;
    if (!*Input(c)) c->more = false;
    Refresh(c); return true;
}
void ShowAbout(Context *c) {
    if (c->about) return;
    c->about = lv_obj_create(lv_obj_get_parent(c->panel));
    lv_obj_set_size(c->about, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(c->about, 0, 0);
    lv_obj_remove_flag(c->about, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(c->about, 0, 0);
    lv_obj_set_style_radius(c->about, 0, 0);
    lv_obj_set_style_bg_color(c->about, lv_color_hex(0x080D18), 0);
    lv_obj_set_style_bg_opa(c->about, LV_OPA_70, 0);
    auto *card = lv_obj_create(c->about);
    lv_obj_set_size(card, 520, 286); lv_obj_center(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 26, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x252D3C), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x435778), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_shadow_width(card, 32, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_40, 0);
    auto *title = lv_label_create(card);
    lv_label_set_text(title, "关于 Hi 输入法");
    lv_obj_set_style_text_font(title, &font_puhui_30_4, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(kBlue), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    auto *message = lv_label_create(card);
    lv_label_set_text(message, "Hi 输入法，欢迎您的使用。");
    lv_obj_set_style_text_font(message, &font_puhui_30_4, 0);
    lv_obj_set_style_text_color(message, lv_color_hex(0x464A47), 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -2);
    auto *close = lv_button_create(card);
    lv_obj_set_size(close, 220, 52);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_remove_flag(close, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_radius(close, 16, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(kBlue), 0);
    auto *label = lv_label_create(close);
    lv_label_set_text(label, "好的");
    lv_obj_set_style_text_font(label, &font_puhui_20_4, 0); lv_obj_center(label);
    lv_obj_add_event_cb(close, [](lv_event_t *e) {
        auto *c = static_cast<Context *>(lv_event_get_user_data(e));
        auto *about = c->about; c->about = nullptr; lv_obj_delete(about);
    }, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(c->about, [](lv_event_t *e) {
        static_cast<Context *>(lv_event_get_user_data(e))->about = nullptr;
    }, LV_EVENT_DELETE, c);
}
void MatrixPressed(lv_event_t *e) {
    auto *c = static_cast<Context *>(lv_event_get_user_data(e));
    lv_obj_t *obj = lv_event_get_target_obj(e);
    if(c->contact_matrix==obj)c->contact_delivered=true;
    uint32_t id = lv_buttonmatrix_get_selected_button(obj);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE || lv_buttonmatrix_has_button_ctrl(obj, id, LV_BUTTONMATRIX_CTRL_DISABLED)) return;
    if (obj == c->spelling) {
        if (!id && c->spelling_page) --c->spelling_page;
        else if (id == 6) ++c->spelling_page;
        else if (id >= 1 && id <= 5) { c->nine.SelectSpelling(c->spelling_page * 5 + id - 1); c->page = 0; }
        Refresh(c); return;
    }
    if (obj == c->candidates) {
        if (id < 3) { Choose(c, c->page * kPage + id); return; }
        if (id == 4) { ShowAbout(c); return; }
        c->more = !c->more;
    } else {
        if (id < kPage) { Choose(c, c->page * kPage + id); return; }
        if (id == kPage && c->page) --c->page;
        if (id == kPage + 1) c->more = false;
        if (id == kPage + 2) ++c->page;
    }
    Refresh(c);
}
void KeyPressed(lv_event_t *e) {
    auto *c = static_cast<Context *>(lv_event_get_user_data(e));
    if(c->contact_matrix==c->keyboard)c->contact_delivered=true;
    uint32_t id = lv_buttonmatrix_get_selected_button(c->keyboard);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE || lv_buttonmatrix_has_button_ctrl(c->keyboard, id, LV_BUTTONMATRIX_CTRL_DISABLED)) return;
    const char *key = lv_buttonmatrix_get_button_text(c->keyboard, id); if (!key) return;
    if (!std::strcmp(key, "完成")) {
        if (CoreFlush(c->panel)) GlobalReady();
        return;
    }
    if(!std::strcmp(key,"换行")){if(c->textarea&&!lv_textarea_get_one_line(c->textarea)&&CoreFlush(c->panel)){Insert(c,"\n");Refresh(c);}return;}
    if (id==c->delete_id) {
        if (*Input(c)) { if (Nine(c)) c->nine.Backspace(); else c->engine.Backspace(); }
        else if (c->textarea && !DeleteSelection(c)) lv_textarea_delete_char(c->textarea);
        c->page = c->spelling_page = 0; Refresh(c); return;
    }
    if (!std::strcmp(key, "九键") || !std::strcmp(key, "全键")) {
        CoreSetLayout(c->panel, !std::strcmp(key, "九键") ? HiInputLayout::NineKey : HiInputLayout::Qwerty); return;
    }
    if (!std::strcmp(key, "中/英")) {
        if (!Allowed(c) || !CoreFlush(c->panel)) return;
        c->chinese = !c->chinese; c->mode = Mode::Text; c->more = false; Refresh(c, true); return;
    }
    if (!std::strcmp(key, "123") || !std::strcmp(key, "符号") || !std::strcmp(key, "返回")) {
        c->mode = !std::strcmp(key, "123") ? Mode::Numbers : !std::strcmp(key, "符号") ? Mode::Symbols : Mode::Text;
        c->more = false; Refresh(c, true); return;
    }
    if (!std::strcmp(key, "大写") || !std::strcmp(key, "小写")) {
        c->upper = !c->upper; Refresh(c, true); return;
    }
    if (!std::strcmp(key, "空格")) {
        if (*Input(c)) {
            if (Count(c)) Choose(c, c->page * kPage);
            else if (!Nine(c) && Insert(c, Input(c))) { Clear(c); Refresh(c); }
            else lv_label_set_text(c->preedit, "无匹配，退格修改");
        } else {Insert(c, " ");Refresh(c);}
        return;
    }
    if (c->mode == Mode::Text && Nine(c)) {
        const char *groups[] = {"ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ"};
        for (size_t i = 0; i < 8; ++i) if (!std::strcmp(key, groups[i])) {
            if (!c->nine.Append('2' + i)) { lv_label_set_text(c->preedit, "拼音已满，请先选词"); return; }
            c->page = c->spelling_page = 0; Refresh(c); return;
        }
        if (!std::strcmp(key, "分词")) {
            c->nine.Append('\''); c->page = c->spelling_page = 0; Refresh(c); return;
        }
    }
    if (Chinese(c) && c->mode == Mode::Text && !Nine(c) && key[0] && !key[1] &&
        ((key[0] >= 'a' && key[0] <= 'z') || (key[0] >= 'A' && key[0] <= 'Z'))) {
        if (!c->engine.Append(key[0])) { lv_label_set_text(c->preedit, "拼音已满，请先选词"); return; }
        c->page = 0; Refresh(c); return;
    }
    if (Chinese(c) && !std::strcmp(key, "'") && *Input(c)) {
        if (Nine(c)) c->nine.Append('\''); else c->engine.Append('\'');
        c->page = c->spelling_page = 0; Refresh(c); return;
    }
    if (!CoreFlush(c->panel)) return;
    Insert(c, key);Refresh(c);
}
void TargetDeleted(lv_event_t *e) {
    auto *c = static_cast<Context *>(lv_event_get_user_data(e));
    c->textarea = nullptr; Clear(c); c->more = false; Refresh(c, true);GlobalHide(false);
}
void PanelDeleted(lv_event_t *e) {
    auto *c = static_cast<Context *>(lv_event_get_user_data(e));
    if (c->about) lv_obj_delete(c->about);
    if (c->textarea) lv_obj_remove_event_cb_with_user_data(c->textarea, TargetDeleted, c);
    c->~Context(); lv_free(c);
}
}
lv_obj_t *CreateKeyboard(lv_obj_t *parent) {
    void *memory = lv_malloc(sizeof(Context)); if (!memory) return nullptr;
    auto *c = new(memory) Context;
    c->panel = lv_obj_create(parent); lv_obj_set_user_data(c->panel, c);
    lv_obj_set_size(c->panel, LV_PCT(100), 360); lv_obj_align(c->panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(c->panel, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE));
    lv_obj_add_flag(c->panel,LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_pad_all(c->panel, 0, 0); lv_obj_set_style_border_width(c->panel, 0, 0);
    lv_obj_set_style_radius(c->panel, 0, 0); lv_obj_set_style_bg_color(c->panel, lv_color_hex(kBg), 0);
    lv_obj_set_style_text_font(c->panel, &font_puhui_20_4, 0);
    c->mirror=lv_textarea_create(c->panel);lv_textarea_set_one_line(c->mirror,true);
    lv_obj_set_style_text_font(c->mirror,&font_puhui_20_4,0);lv_obj_set_style_radius(c->mirror,5,0);
    lv_obj_set_style_bg_color(c->mirror,lv_color_hex(0xFDFEFC),0);lv_obj_set_style_pad_all(c->mirror,6,0);
    lv_obj_set_style_border_width(c->mirror,1,0);
    lv_obj_set_scroll_dir(c->mirror,LV_DIR_HOR);
    lv_obj_remove_flag(c->mirror,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLL_ELASTIC|
        LV_OBJ_FLAG_SCROLL_MOMENTUM|LV_OBJ_FLAG_SCROLL_CHAIN_VER|LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    // Cursor visibility may still request horizontal scrolling for long text.
    // Make this local field's scroll instant; retain the cursor blink and
    // selection/clipboard behavior without touching global LVGL animations.
    lv_obj_add_event_cb(c->mirror,[](lv_event_t*e){
        auto*animation=static_cast<lv_anim_t*>(lv_event_get_param(e));
        if(animation)lv_anim_set_duration(animation,0);
    },LV_EVENT_SCROLL_BEGIN,c);
    lv_textarea_set_text_selection(c->mirror,true);
    lv_obj_add_event_cb(c->mirror,[](lv_event_t*e){auto*c=static_cast<Context*>(lv_event_get_user_data(e));if(!c->textarea)return;
        if(lv_event_get_code(e)==LV_EVENT_LONG_PRESSED){SelectWord(c);return;}
        if(lv_event_get_code(e)!=LV_EVENT_RELEASED)return;
        uint32_t a,b;bool selected=Selection(c->mirror,a,b);lv_textarea_set_cursor_pos(c->textarea,lv_textarea_get_cursor_pos(c->mirror));
        if(selected){SetSelection(c->textarea,a,b);c->edit=true;ToolsMap(c);}else lv_textarea_clear_selection(c->textarea);UpdateHandles(c);
    },LV_EVENT_ALL,c);
    c->tools=lv_buttonmatrix_create(c->panel);StyleMatrix(c->tools,20,false);
    // Retain the original six-button mutable table and lazy clipboard. New
    // menus are immutable flash data, avoiding boot static-RAM growth.
    static const char *tools[]={"全/双","编辑","粘贴","左移","右移","收起",""};
    c->normal_tools_map=tools;ToolsMap(c);Controls(c->tools,6);
    lv_obj_add_event_cb(c->tools,[](lv_event_t*e){auto&clipboard=SharedClipboard();auto*c=static_cast<Context*>(lv_event_get_user_data(e));unsigned id=lv_buttonmatrix_get_selected_button(c->tools);c->clipboard=&clipboard;
      if(lv_event_get_param(e)==c)return;
      if(c->edit){
        if(id==0&&c->textarea&&!lv_textarea_get_password_mode(c->textarea)){if(CoreFlush(c->panel))SetSelection(c->textarea,0,Characters(lv_textarea_get_text(c->textarea)));}
        else if(id==1&&c->textarea)SelectWord(c);
        else if(id==2||id==3)Copy(c,id==3);
        else if(id==4){if(CoreFlush(c->panel))Insert(c,Clipboard(c).c_str());}
        else if(id==5){c->edit=false;c->extending=false;ToolsMap(c);}
        Refresh(c);return;
      }
      if(id==0&&Allowed(c)&&CoreFlush(c->panel)){Clear(c);CoreSetLayout(c->panel,HiInputLayout::Qwerty);c->chinese=true;c->mode=Mode::Text;c->more=false;c->engine.SetDoublePinyin(!c->engine.IsDoublePinyin());
        Settings settings("input",true);settings.SetInt("double",c->engine.IsDoublePinyin()?1:0);Refresh(c,true);}
      else if(id==1){c->edit=true;ToolsMap(c);SelectWord(c);}
      else if(id==2){if(CoreFlush(c->panel))Insert(c,Clipboard(c).c_str());Refresh(c);}
      else if((id==3||id==4)&&c->textarea&&CoreFlush(c->panel)){
        if(!c->extending)lv_textarea_clear_selection(c->textarea);
        if(id==3)lv_textarea_cursor_left(c->textarea);else lv_textarea_cursor_right(c->textarea);
        if(c->extending)SetSelection(c->textarea,c->anchor,lv_textarea_get_cursor_pos(c->textarea));Refresh(c);}
      else if(id==5)GlobalHide(true);
    },LV_EVENT_VALUE_CHANGED,c);
    for(unsigned end=0;end<2;++end){auto*h=lv_button_create(c->panel);if(end)c->handle_end=h;else c->handle_start=h;
      lv_obj_set_size(h,28,30);lv_obj_set_style_radius(h,14,0);lv_obj_set_style_bg_color(h,lv_color_hex(kBlue),0);
      lv_obj_remove_flag(h,LV_OBJ_FLAG_CLICK_FOCUSABLE);lv_obj_add_flag(h,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_HIDDEN|LV_OBJ_FLAG_PRESS_LOCK));lv_obj_set_ext_click_area(h,10);
      lv_obj_add_event_cb(h,[](lv_event_t*e){auto*c=static_cast<Context*>(lv_event_get_user_data(e));if(lv_event_get_code(e)!=LV_EVENT_PRESSING||!c->textarea)return;
        auto*indev=lv_indev_active();if(!indev)return;lv_point_t p;lv_indev_get_point(indev,&p);lv_area_t area,mirror;lv_obj_get_coords(c->mirror,&mirror);
        if(p.x<mirror.x1+16)lv_obj_scroll_by(c->mirror,12,0,LV_ANIM_OFF);
        else if(p.x>mirror.x2-16)lv_obj_scroll_by(c->mirror,-12,0,LV_ANIM_OFF);
        lv_obj_get_coords(lv_textarea_get_label(c->mirror),&area);
        p.x-=area.x1;p.y=0;uint32_t pos=lv_label_get_letter_on(lv_textarea_get_label(c->mirror),&p,true),a,b;
        if(!Selection(c->textarea,a,b))return;pos=std::min(pos,static_cast<uint32_t>(Characters(lv_textarea_get_text(c->textarea))));
        if(lv_event_get_target_obj(e)==c->handle_start)a=std::min(pos,b-1);else b=std::max(pos,a+1);
        SetSelection(c->textarea,a,b);SetSelection(c->mirror,a,b);UpdateHandles(c);
      },LV_EVENT_PRESSING,c);
    }
    lv_obj_send_event(c->tools,LV_EVENT_VALUE_CHANGED,c);
    c->header = lv_button_create(c->panel);lv_obj_remove_style_all(c->header);lv_obj_set_style_pad_all(c->header,0,0);
    lv_obj_set_pos(c->header, 8, 2); lv_obj_remove_flag(c->header, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_bg_opa(c->header, LV_OPA_TRANSP, 0); lv_obj_set_style_shadow_width(c->header, 0, 0);
    c->preedit = lv_label_create(c->header); lv_obj_align(c->preedit, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_long_mode(c->preedit, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(c->preedit, lv_color_hex(0x727872), 0);
    lv_obj_add_event_cb(c->header, [](lv_event_t *e) {
        auto *c = static_cast<Context *>(lv_event_get_user_data(e));
        if (*Input(c) && !Nine(c) && Insert(c, Input(c))) { Clear(c); c->more = false; Refresh(c); }
    }, LV_EVENT_CLICKED, c);
    c->candidates = lv_buttonmatrix_create(c->panel); StyleMatrix(c->candidates, 20, false);
    c->spelling = lv_buttonmatrix_create(c->panel); StyleMatrix(c->spelling, 20, false);
    c->keyboard = lv_buttonmatrix_create(c->panel); StyleMatrix(c->keyboard, 20, true);
    c->expanded = lv_buttonmatrix_create(c->panel); StyleMatrix(c->expanded, 20, false);
    // Candidate geometry and single-line labels stay fixed, even when empty.
    // Never animate, remap or vertically recenter a wrapped candidate string.
    for(size_t i=0;i<3;++i)c->candidate_map[i]=" ";c->candidate_map[3]=" ";c->candidate_map[4]="更多";c->candidate_map[5]="";
    lv_buttonmatrix_set_map(c->candidates,c->candidate_map);Controls(c->candidates,5);
    for(unsigned i=0;i<3;++i)lv_buttonmatrix_set_button_width(c->candidates,i,4);
    lv_buttonmatrix_set_button_width(c->candidates,4,2);
    size_t position=0;for(size_t i=0;i<kPage;++i){c->expanded_map[position++]=" ";if(i%3==2)c->expanded_map[position++]="\n";}
    c->expanded_map[position++]="上一页";c->expanded_map[position++]="返回";c->expanded_map[position++]="下一页";c->expanded_map[position]="";
    lv_buttonmatrix_set_map(c->expanded,c->expanded_map);Controls(c->expanded,kPage+3);
    c->spelling_map[0]="<";for(size_t i=0;i<5;++i){std::strcpy(c->spell[i]," ");c->spelling_map[i+1]=c->spell[i];}c->spelling_map[6]=">";c->spelling_map[7]="";
    lv_buttonmatrix_set_map(c->spelling,c->spelling_map);Controls(c->spelling,7);
    for(unsigned group=0;group<2;++group)for(unsigned i=0;i<(group?12:3);++i){
        auto*l=lv_label_create(group?c->expanded:c->candidates);lv_obj_remove_style_all(l);
        lv_label_set_text(l," ");lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_obj_set_style_text_font(l,&font_puhui_20_4,0);
        lv_obj_set_style_text_color(l,lv_color_hex(i?0x464A47:kBlue),0);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_LEFT,0);
        lv_obj_remove_flag(l,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
        if(group)c->expanded_labels[i]=l;else c->candidate_labels[i]=l;
    }
    for(auto*matrix:{c->keyboard,c->candidates,c->spelling,c->expanded})lv_obj_add_event_cb(matrix,[](lv_event_t*e){
        auto*c=static_cast<Context*>(lv_event_get_user_data(e));auto*matrix=lv_event_get_current_target_obj(e);auto code=lv_event_get_code(e);
        auto*input=lv_indev_active();if(!input)return;
        if(code==LV_EVENT_PRESSED){c->contact_matrix=matrix;c->contact_id=lv_buttonmatrix_get_selected_button(matrix);lv_indev_get_point(input,&c->contact_point);
            c->contact_delivered=c->contact_id==LV_BUTTONMATRIX_BUTTON_NONE||!lv_buttonmatrix_has_button_ctrl(matrix,c->contact_id,LV_BUTTONMATRIX_CTRL_CLICK_TRIG);}
        else if(code==LV_EVENT_RELEASED&&c->contact_matrix==matrix){
            auto id=c->contact_id;if(!c->contact_delivered&&id!=LV_BUTTONMATRIX_BUTTON_NONE){
                auto*b=reinterpret_cast<lv_buttonmatrix_t*>(matrix);lv_point_t point;lv_indev_get_point(input,&point);lv_area_t a;lv_obj_get_coords(matrix,&a);
                if(id<b->btn_cnt){auto box=b->button_areas[id];int x=point.x-a.x1,y=point.y-a.y1;
                    if(x>=box.x1-8&&x<=box.x2+8&&y>=box.y1-8&&y<=box.y2+8&&!lv_buttonmatrix_has_button_ctrl(matrix,id,LV_BUTTONMATRIX_CTRL_DISABLED)){
                        lv_buttonmatrix_set_selected_button(matrix,id);lv_obj_send_event(matrix,LV_EVENT_VALUE_CHANGED,&id);
                    }
                }
            }c->contact_matrix=nullptr;
        }else if(code==LV_EVENT_PRESS_LOST&&c->contact_matrix==matrix)c->contact_matrix=nullptr;
    },LV_EVENT_ALL,c);

    c->hide = lv_button_create(c->panel); lv_obj_set_size(c->hide, 88, 38);
    lv_obj_remove_flag(c->hide, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_bg_opa(c->hide, LV_OPA_TRANSP, 0); lv_obj_set_style_shadow_width(c->hide, 0, 0);
    lv_obj_t *label = lv_label_create(c->hide); lv_label_set_text(label, "收起"); lv_obj_center(label);
    lv_obj_add_event_cb(c->hide, [](lv_event_t *e) {
        auto *c = static_cast<Context *>(lv_event_get_user_data(e));
        GlobalHide(true);
    }, LV_EVENT_CLICKED, c);
    c->delete_icon=lv_image_create(c->keyboard);lv_image_set_src(c->delete_icon,&kImeDeleteIcon);
    c->expand_icon=lv_image_create(c->candidates);lv_image_set_src(c->expand_icon,&kImeExpandIcon);
    for(auto*icon:{c->delete_icon,c->expand_icon})lv_obj_remove_flag(icon,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_CLICK_FOCUSABLE|LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_event_cb(c->candidates, MatrixPressed, LV_EVENT_VALUE_CHANGED, c);
    lv_obj_add_event_cb(c->expanded, MatrixPressed, LV_EVENT_VALUE_CHANGED, c);
    lv_obj_add_event_cb(c->spelling, MatrixPressed, LV_EVENT_VALUE_CHANGED, c);
    lv_obj_add_event_cb(c->keyboard, KeyPressed, LV_EVENT_VALUE_CHANGED, c);
    lv_obj_add_event_cb(c->panel, [](lv_event_t *e) {
        Layout(static_cast<Context *>(lv_event_get_user_data(e)));
    }, LV_EVENT_SIZE_CHANGED, c);
    lv_obj_add_event_cb(c->panel, PanelDeleted, LV_EVENT_DELETE, c);
    Refresh(c, true); return c->panel;
}
void CoreSetTextArea(lv_obj_t *panel, lv_obj_t *ta, bool allowed) {
    if (!panel) return;
    auto *c = Get(panel);
    const bool changed=ta != c->textarea || allowed != c->allowed;
    if (changed) {
        if (c->textarea) lv_obj_remove_event_cb_with_user_data(c->textarea, TargetDeleted, c);
        Clear(c);c->edit=false;c->extending=false;ToolsMap(c);
        c->more = false; c->mode = Mode::Text; c->textarea = ta; c->allowed = allowed;
        if (ta) {lv_textarea_set_text_selection(ta,!lv_textarea_get_password_mode(ta));lv_obj_add_event_cb(ta, TargetDeleted, LV_EVENT_DELETE, c);}
    }
    if(ta&&!Allowed(c)){const char*accepted=lv_textarea_get_accepted_chars(ta);if(accepted&&std::strspn(accepted,"0123456789+-. ")==std::strlen(accepted))c->mode=Mode::Numbers;}
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);lv_obj_update_layout(panel);Refresh(c, changed);
}
bool CoreFlush(lv_obj_t *panel) {
    if (!panel) return true;
    auto *c = Get(panel);
    while (*Input(c)) {
        if (Count(c)) { if (!Choose(c, 0)) return false; }
        else if (!Nine(c) && Insert(c, Input(c))) Clear(c);
        else { lv_label_set_text(c->preedit, "无匹配，退格修改"); return false; }
    }
    Refresh(c); return true;
}
void CoreSetLayout(lv_obj_t *panel, HiInputLayout layout) {
    if (!panel) return;
    auto *c = Get(panel); if (c->layout == layout) return;
    if (!CoreFlush(panel)) return;
    Clear(c); c->layout = layout; s_preferred_layout = layout;
    c->mode = Mode::Text; c->more = false; Refresh(c, true);
    Settings settings("input",true);settings.SetInt("layout",layout==HiInputLayout::Qwerty?1:0);
}
HiInputLayout CoreGetLayout(lv_obj_t *panel) { return Get(panel)->layout; }

namespace {
lv_obj_t *global_panel=nullptr,*global_owner=nullptr,*requested_target=nullptr;
bool global_dispatch_pending=false;
void FitGlobal(){
 if(!global_panel)return;auto*d=lv_display_get_default();int w=lv_display_get_horizontal_resolution(d),h=lv_display_get_vertical_resolution(d);
 // Layout follows physical resolution, never a legacy app's proxy dimensions.
 // The existing home-pill hit area occupies the last 40 pixels. Leave a
 // further 8 pixels so a low space/delete tap cannot arm its gestures.
 int safe=std::min(48,h/12);
 int keyboard_h=std::min(h-safe-60,std::max(270,std::min(348,h*48/100)));
 // CreateKeyboard initially anchors to BOTTOM_MID. set_pos is an offset
 // from that anchor in LVGL, not an absolute position: the old combination
 // put the entire keyboard below the 720px display (y=715 instead of 343).
 lv_obj_set_align(global_panel,LV_ALIGN_TOP_LEFT);
 lv_obj_set_size(global_panel,w,keyboard_h);lv_obj_set_pos(global_panel,0,h-safe-keyboard_h);
}
bool Under(lv_obj_t *child,lv_obj_t *parent){for(auto*p=child;p;p=lv_obj_get_parent(p))if(p==parent)return true;return false;}
bool Visible(lv_obj_t *obj){for(auto*p=obj;p;p=lv_obj_get_parent(p))if(lv_obj_has_flag(p,LV_OBJ_FLAG_HIDDEN))return false;return true;}
void GlobalHide(bool commit){
 if(!global_panel)return;
 if(commit&&!CoreFlush(global_panel))return;
 auto*c=Get(global_panel);
 if(c->textarea)lv_obj_remove_event_cb_with_user_data(c->textarea,TargetDeleted,c);
 c->textarea=nullptr;Clear(c);c->more=false;global_owner=nullptr;
 lv_obj_add_flag(global_panel,LV_OBJ_FLAG_HIDDEN);
}
void GlobalReady(){
 if(!global_panel)return;auto*owner=global_owner;
 if(owner&&lv_obj_is_valid(owner))lv_obj_send_event(owner,LV_EVENT_READY,nullptr);
 GlobalHide(false);
}
void ShowTarget(lv_obj_t*target,bool allowed,lv_obj_t*owner){
 if(StandbyScreen::IsVisible()||ClockService::GetAlert().alert)return;
 if(!target||!lv_obj_is_valid(target)||!lv_obj_check_type(target,&lv_textarea_class)||lv_obj_has_state(target,LV_STATE_DISABLED))return;
 EnsureGlobalPanel();if(!global_panel)return;
 // Changing fields must not be blocked by an incomplete old pinyin syllable.
 if(Get(global_panel)->textarea!=target)GlobalHide(false);
 global_owner=owner?owner:target;
 FitGlobal();
 lv_textarea_set_password_mode(Get(global_panel)->mirror,lv_textarea_get_password_mode(target));
 CoreSetTextArea(global_panel,target,allowed);lv_obj_move_foreground(global_panel);
 screen_swipe_back_ignore(global_panel,true);lv_obj_update_layout(global_panel);
}
void RequestedDeleted(lv_event_t *e){if(requested_target==lv_event_get_target_obj(e))requested_target=nullptr;}
void OnPointer(lv_event_t *e){
 if(StandbyScreen::IsVisible()||ClockService::GetAlert().alert)return;
 auto*target=static_cast<lv_obj_t*>(lv_event_get_param(e));
 if(!target||Under(target,global_panel))return;
 while(target&&!lv_obj_check_type(target,&lv_textarea_class))target=lv_obj_get_parent(target);
 if(!target)return;
 // Explicit field handlers run in the original click/focus dispatch. Do
 // not queue a second activation that can retarget a newly opened dialog
 // to the underlying field from the same tap (e.g. contact search).
 for(uint32_t i=0;i<lv_obj_get_event_count(target);++i)
  if(lv_event_dsc_get_cb(lv_obj_get_event_dsc(target,i))==BoundField)return;
 if(requested_target&&lv_obj_is_valid(requested_target))lv_obj_remove_event_cb(requested_target,RequestedDeleted);
 requested_target=target;lv_obj_add_event_cb(target,RequestedDeleted,LV_EVENT_DELETE,nullptr);
 if(!global_dispatch_pending){global_dispatch_pending=true;lv_async_call([](void*){
   global_dispatch_pending=false;auto*ta=requested_target;requested_target=nullptr;
   if(!ta||!lv_obj_is_valid(ta))return;
   lv_obj_remove_event_cb(ta,RequestedDeleted);
   if(StandbyScreen::IsVisible()||ClockService::GetAlert().alert)return;
   if(global_panel&&Get(global_panel)->textarea==ta){FitGlobal();lv_obj_remove_flag(global_panel,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(global_panel);return;}
   ShowTarget(ta,true,ta);
 },nullptr);}
}
}
void HiInputMethod_InitGlobal(){
 // Boot installs pointer observation independently of keyboard objects. Keep
 // construction/allocation out of the concurrent MultiNet/network startup.
 Settings settings("input",false);s_preferred_layout=settings.GetInt("layout",1)?HiInputLayout::Qwerty:HiInputLayout::NineKey;
}
static void EnsureGlobalPanel(){
 if(global_panel)return;
 Settings settings("input",false);s_preferred_layout=settings.GetInt("layout",1)?HiInputLayout::Qwerty:HiInputLayout::NineKey;
 global_panel=CreateKeyboard(lv_layer_sys());if(!global_panel)return;
 Get(global_panel)->engine.SetDoublePinyin(settings.GetInt("double",0)!=0);
 FitGlobal();lv_obj_add_flag(global_panel,LV_OBJ_FLAG_HIDDEN);
 lv_timer_create([](lv_timer_t*){
  if(!global_panel||lv_obj_has_flag(global_panel,LV_OBJ_FLAG_HIDDEN))return;
  if(StandbyScreen::IsVisible()||ClockService::GetAlert().alert){HiInputMethod_Suspend();return;}
  if(ControlCenter_IsOpen()||Recents_IsOpen()){GlobalHide(false);return;}
  auto*c=Get(global_panel);auto*ta=c->textarea;
  if(!ta||!lv_obj_is_valid(ta)||!Visible(ta)|| (global_owner&&(!lv_obj_is_valid(global_owner)||!Visible(global_owner)))){GlobalHide(false);return;}
  auto*root=lv_obj_get_screen(ta);
  if(root!=lv_screen_active()&&root!=lv_layer_sys()&&root!=lv_layer_top()){GlobalHide(false);return;}
  FitGlobal();
 },100,nullptr);
}
lv_obj_t *HiInputMethod_Create(lv_obj_t*parent){
 HiInputMethod_InitGlobal();auto*handle=lv_obj_create(parent);lv_obj_remove_style_all(handle);lv_obj_set_size(handle,0,0);
 lv_obj_remove_flag(handle,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_CLICK_FOCUSABLE|LV_OBJ_FLAG_SCROLLABLE));
 lv_obj_add_event_cb(handle,[](lv_event_t*e){auto*handle=lv_event_get_target_obj(e);if(handle==global_owner)GlobalHide(false);},LV_EVENT_DELETE,nullptr);
 return handle;
}
void HiInputMethod_ObservePointer(lv_event_t*event){
 if(lv_event_get_code(event)==LV_EVENT_CLICKED){OnPointer(event);return;}
 if(lv_event_get_code(event)!=LV_EVENT_LONG_PRESSED||StandbyScreen::IsVisible()||ClockService::GetAlert().alert||ControlCenter_IsOpen()||Recents_IsOpen())return;
 auto*obj=static_cast<lv_obj_t*>(lv_event_get_param(event));if(!obj||Under(obj,global_panel))return;
 auto*ta=obj;while(ta&&!lv_obj_check_type(ta,&lv_textarea_class))ta=lv_obj_get_parent(ta);
 if(ta){if(lv_textarea_get_password_mode(ta))return;
  HiInputMethod_Show(ta,true);if(global_panel)SelectWord(Get(global_panel));}

}
namespace {
void BoundField(lv_event_t*e){
 if(lv_event_get_code(e)==LV_EVENT_CLICKED||lv_event_get_code(e)==LV_EVENT_FOCUSED)
  HiInputMethod_Show(lv_event_get_current_target_obj(e),lv_event_get_user_data(e)!=nullptr);
}
}
void HiInputMethod_Bind(lv_obj_t*ta,bool allowed){
 if(!ta)return;lv_obj_remove_event_cb(ta,BoundField);
 lv_obj_add_event_cb(ta,BoundField,LV_EVENT_FOCUSED,allowed?(void*)1:nullptr);
 lv_obj_add_event_cb(ta,BoundField,LV_EVENT_CLICKED,allowed?(void*)1:nullptr);
 screen_swipe_back_ignore(ta,true);
}
void HiInputMethod_SetTextArea(lv_obj_t*panel,lv_obj_t*ta,bool allowed){
 if(!panel)return;lv_obj_set_user_data(panel,ta);
 if(!ta){if(global_owner==panel)GlobalHide(false);return;}
 lv_obj_remove_flag(panel,LV_OBJ_FLAG_HIDDEN);ShowTarget(ta,allowed,panel);
}
bool HiInputMethod_Flush(lv_obj_t*panel){return !global_panel||!panel||global_owner!=panel?true:CoreFlush(global_panel);}
void HiInputMethod_SetLayout(lv_obj_t*,HiInputLayout layout){EnsureGlobalPanel();CoreSetLayout(global_panel,layout);}
HiInputLayout HiInputMethod_GetLayout(lv_obj_t*){return global_panel?CoreGetLayout(global_panel):s_preferred_layout;}
void HiInputMethod_Show(lv_obj_t*ta,bool allowed){
 // A click on an already bound field must retain the app's READY proxy.
 auto*owner=global_panel&&Get(global_panel)->textarea==ta?global_owner:ta;
 ShowTarget(ta,allowed,owner);
}
void HiInputMethod_Hide(){GlobalHide(true);}
bool HiInputMethod_IsVisible(){return global_panel&&!lv_obj_has_flag(global_panel,LV_OBJ_FLAG_HIDDEN);}

// Overlay suspension preserves composition and never depends on committing it.
void HiInputMethod_Suspend(){if(global_panel){auto*c=Get(global_panel);if(c->about){auto*about=c->about;c->about=nullptr;lv_obj_delete(about);}lv_obj_add_flag(global_panel,LV_OBJ_FLAG_HIDDEN);}if(requested_target&&lv_obj_is_valid(requested_target))lv_obj_remove_event_cb(requested_target,RequestedDeleted);requested_target=nullptr;}

void HiInputMethod_CopyText(const char*text){SharedClipboard()=text?text:"";}
