#pragma once
#include "lvgl.h"
#include "screen_util.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
LV_FONT_DECLARE(font_puhui_20_4);LV_FONT_DECLARE(font_puhui_30_4);
namespace HiDial {
constexpr float pi=3.14159265359f;
inline lv_obj_t*Label(lv_obj_t*p,const char*t,int x,int y,int w,bool large=false,uint32_t color=0x5A5D59){auto*l=lv_label_create(p);lv_label_set_text(l,t);lv_obj_set_pos(l,x,y);lv_obj_set_width(l,w);lv_obj_set_style_text_font(l,large?&font_puhui_30_4:&font_puhui_20_4,0);lv_obj_set_style_text_color(l,lv_color_hex(color),0);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);return l;}
inline lv_obj_t*Box(lv_obj_t*p,int x,int y,int w,int h,uint32_t color=0xF7F9F6,int radius=0){auto*b=lv_obj_create(p);screen_strip_obj_chrome(b);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);lv_obj_set_style_bg_color(b,lv_color_hex(color),0);lv_obj_set_style_bg_opa(b,255,0);lv_obj_set_style_radius(b,radius,0);lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE);return b;}
inline lv_obj_t*Button(lv_obj_t*p,const char*t,int x,int y,int w,int h,lv_event_cb_t cb,void*arg=nullptr){auto*b=lv_button_create(p);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);lv_obj_set_style_radius(b,7,0);lv_obj_set_style_bg_color(b,lv_color_hex(0xFAFCF9),0);lv_obj_set_style_bg_grad_color(b,lv_color_hex(0xE2E5E0),0);lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_VER,0);lv_obj_set_style_border_width(b,1,0);lv_obj_set_style_border_color(b,lv_color_hex(0xC8CDC6),0);lv_obj_set_style_shadow_width(b,3,0);lv_obj_set_style_shadow_ofs_y(b,2,0);lv_obj_set_style_pad_all(b,0,0);auto*l=Label(b,t,0,0,w);lv_obj_center(l);lv_obj_set_ext_click_area(b,5);screen_swipe_back_ignore(b,true);if(cb)lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,arg);return b;}
inline void Text(lv_obj_t*b,const char*t){lv_label_set_text(lv_obj_get_child(b,0),t);}
inline void Line(lv_layer_t*l,float x1,float y1,float x2,float y2,int width,uint32_t color){lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.p1.x=x1;d.p1.y=y1;d.p2.x=x2;d.p2.y=y2;d.width=width;d.color=lv_color_hex(color);d.opa=255;d.round_start=1;d.round_end=1;lv_draw_line(l,&d);}
struct Dial {lv_obj_t*obj=nullptr;float hour=0,minute=0,second=0,rotation=0;bool compass=false;};
inline void Draw(lv_event_t*e){auto*d=(Dial*)lv_event_get_user_data(e);auto*l=lv_event_get_layer(e);lv_area_t a;lv_obj_get_coords(d->obj,&a);float cx=(a.x1+a.x2)*.5f,cy=(a.y1+a.y2)*.5f,r=lv_obj_get_width(d->obj)*.5f-14;
 for(int i=0;i<60;++i){float angle=(i*6-d->rotation-90)*pi/180;float inner=r-(i%5==0?12:5);Line(l,cx+inner*cosf(angle),cy+inner*sinf(angle),cx+r*cosf(angle),cy+r*sinf(angle),i%5==0?2:1,0xA4ABA3);}
 auto number=[&](const char*t,float degree,uint32_t color){float angle=(degree-d->rotation-90)*pi/180;lv_draw_label_dsc_t f;lv_draw_label_dsc_init(&f);f.font=&font_puhui_30_4;f.color=lv_color_hex(color);f.text=t;f.align=LV_TEXT_ALIGN_CENTER;int x=cx+(r-34)*cosf(angle),y=cy+(r-34)*sinf(angle);lv_area_t b{x-30,y-18,x+30,y+20};lv_draw_label(l,&f,&b);};
 if(d->compass){number("N",0,0xC84836);number("E",90,0x565C56);number("S",180,0x565C56);number("W",270,0x565C56);Line(l,cx-10,cy,cx+10,cy,1,0xB8BDB6);Line(l,cx,cy-10,cx,cy+10,1,0xB8BDB6);return;}
 number("12",0,0x5A6059);number("3",90,0x5A6059);number("6",180,0x5A6059);number("9",270,0x5A6059);
 auto hand=[&](float degree,float length,int width,uint32_t color){float angle=(degree-90)*pi/180;Line(l,cx+2,cy+3,cx+length*cosf(angle)+2,cy+length*sinf(angle)+3,width+2,0xD5D9D2);Line(l,cx,cy,cx+length*cosf(angle),cy+length*sinf(angle),width,color);};
 hand(d->hour*30+d->minute*.5f,r*.52f,12,0x575F57);hand(d->minute*6,r*.76f,9,0x575F57);hand(d->second*6,r*.86f,3,0xB86950);Line(l,cx-4,cy,cx+4,cy,10,0x545B54);Line(l,cx-2,cy,cx+2,cy,6,0xBF765E);
}
inline lv_obj_t*Face(lv_obj_t*p,Dial*d,int x,int y,int size){d->obj=Box(p,x,y,size,size,0xFCFDFB,size/2);lv_obj_set_style_bg_grad_color(d->obj,lv_color_hex(0xEDF0EB),0);lv_obj_set_style_bg_grad_dir(d->obj,LV_GRAD_DIR_VER,0);lv_obj_set_style_border_width(d->obj,7,0);lv_obj_set_style_border_color(d->obj,lv_color_hex(0xDEE3DC),0);lv_obj_set_style_shadow_width(d->obj,12,0);lv_obj_set_style_shadow_ofs_y(d->obj,5,0);lv_obj_set_style_shadow_opa(d->obj,50,0);lv_obj_remove_flag(d->obj,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(d->obj,Draw,LV_EVENT_DRAW_MAIN,d);return d->obj;}
}
