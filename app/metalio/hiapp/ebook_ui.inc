// Dedicated, opt-in ebook layout. Uses existing fonts and vector widgets;
// no cover bitmaps, extra fonts, network services or global theme changes.
lv_obj_t*BookPanel(lv_obj_t*p,int x,int y,int w,int h,uint32_t color,int radius=0){
 auto*o=lv_obj_create(p);screen_strip_obj_chrome(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_style_radius(o,radius,0);lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);return o;
}
lv_obj_t*BookText(lv_obj_t*p,const std::string&t,int x,int y,int w,uint32_t color,bool large=false){
 auto*l=Label(p,t,w,large);lv_obj_set_pos(l,x,y);lv_obj_set_style_text_color(l,lv_color_hex(color),0);return l;
}
void BookButtonStyle(lv_obj_t*b,bool accent=false){
 lv_obj_set_style_bg_color(b,lv_color_hex(accent?0xDB6653:0xF0F1F3),0);lv_obj_set_style_radius(b,12,0);
 lv_obj_set_style_bg_color(b,lv_color_hex(accent?0xC65443:0xE0E3E8),LV_STATE_PRESSED);
 lv_obj_set_style_text_color(lv_obj_get_child(b,0),lv_color_hex(accent?0xFFFFFF:0x505763),0);
}
void BookTile(View*v,const Item&i,unsigned index){
 const bool folder=i.kind=="folder";auto*b=Button(v->body,"",new Action{v,i.id,i.value,false},208,286);lv_obj_delete(lv_obj_get_child(b,0));
 lv_obj_set_style_bg_opa(b,LV_OPA_TRANSP,0);lv_obj_set_style_bg_opa(b,LV_OPA_20,LV_STATE_PRESSED);lv_obj_set_style_pad_all(b,0,0);lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE);
 static const uint32_t colors[]={0x45686A,0xB56851,0x596884,0xA58856,0x6C6A82,0x8B7066};
 unsigned key=0;for(unsigned char c:i.text)key=key*33+c;uint32_t color=colors[key%6];
 auto*cover=BookPanel(b,24,4,160,188,folder?0xEBE9E3:color,5);lv_obj_set_style_shadow_width(cover,10,0);lv_obj_set_style_shadow_opa(cover,LV_OPA_10,0);lv_obj_set_style_shadow_ofs_y(cover,4,0);
 if(folder){BookPanel(cover,18,40,50,15,0xD7C5A4,4);BookPanel(cover,18,50,124,76,0xE1D2B5,7);BookPanel(cover,24,57,112,3,0xF7F0E0,1);auto*l=BookText(cover,"分类",0,144,160,0x8C806D);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);}
 else{
  BookPanel(cover,0,0,6,188,0xFFFFFF,0);lv_obj_set_style_bg_opa(lv_obj_get_child(cover,0),LV_OPA_20,0);
  auto*name=BookText(cover,i.text,20,29,122,0xFFFDF7);lv_obj_set_height(name,104);lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);lv_obj_set_style_text_line_space(name,10,0);
  BookPanel(cover,20,148,32,2,0xF0D9AE);BookText(cover,"Hi 读",20,157,122,0xEEE5D5);
 }
 auto*name=BookText(b,i.text,12,202,184,0x282D35);lv_obj_set_height(name,50);lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);lv_obj_set_style_text_align(name,LV_TEXT_ALIGN_CENTER,0);
 auto*detail=BookText(b,i.detail,5,258,198,0x93989F);lv_label_set_long_mode(detail,LV_LABEL_LONG_DOT);lv_obj_set_style_text_align(detail,LV_TEXT_ALIGN_CENTER,0);
 (void)index;
}
void RenderEbook(View*v,Model&m){
 const bool reading=m.selected=="reader",shelf=m.selected=="shelf";const uint32_t paper=reading?0xF5EEDF:0xFAFAF8;
 lv_obj_set_style_bg_color(v->root,lv_color_hex(paper),0);
 lv_obj_set_style_bg_color(v->back,lv_color_hex(paper),0);lv_obj_set_style_border_width(v->back,0,0);lv_obj_set_style_shadow_width(v->back,0,0);lv_obj_set_style_text_color(lv_obj_get_child(v->back,0),lv_color_hex(0x62666C),0);
 lv_obj_set_size(v->back,82,44);auto*title=BookText(v->content,m.title,114,10,574,0x252A31,!reading);lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);lv_obj_set_height(title,42);
 const int body_y=reading?60:shelf?146:m.input?156:96;const int body_end=reading?544:shelf?570:m.buttons.empty()?628:570;
 if(!reading){
  auto*path=BookText(v->content,m.status,24,57,672,0x93989F);lv_label_set_long_mode(path,LV_LABEL_LONG_DOT);lv_obj_set_height(path,26);v->status=path;
 }
 if(m.input){v->input=lv_textarea_create(v->content);lv_obj_set_pos(v->input,24,92);lv_obj_set_size(v->input,544,48);lv_textarea_set_one_line(v->input,true);lv_textarea_set_max_length(v->input,120);lv_obj_set_style_text_font(v->input,&font_puhui_full_20,0);lv_obj_set_style_radius(v->input,12,0);lv_obj_set_style_border_width(v->input,1,0);lv_obj_set_style_border_color(v->input,lv_color_hex(0xE5E5E1),0);lv_textarea_set_placeholder_text(v->input,m.placeholder.c_str());lv_textarea_set_text(v->input,m.address.c_str());HiInputMethod_Bind(v->input,true);
  auto*b=Button(v->content,m.submit=="goto"?"跳转":"搜索",new Action{v,m.submit,"",true},112,48);lv_obj_set_pos(b,584,92);BookButtonStyle(b,true);
  auto*a=new Action{v,m.submit,"",true};lv_obj_add_event_cb(v->input,Click,LV_EVENT_READY,a);lv_obj_add_event_cb(v->input,[](lv_event_t*e){delete(Action*)lv_event_get_user_data(e);},LV_EVENT_DELETE,a);screen_swipe_back_ignore(v->input);
 }
 for(size_t j=0;j<m.buttons.size();++j){const auto&i=m.buttons[j];const unsigned total=m.buttons.size();int w,x,y;
  if(reading){w=(672-(int(total)-1)*8)/std::max(1u,total);x=24+j*(w+8);y=590;}
  else if(shelf){w=213;x=24+(j%3)*229;y=j<3?92:584;}
  else{w=(672-(int(total)-1)*8)/std::max(1u,total);x=24+j*(w+8);y=584;}
  auto*b=Button(v->content,i.text,new Action{v,i.id,i.value,false},w,42);lv_obj_set_pos(b,x,y);BookButtonStyle(b,shelf&&j==0);
 }
 if(reading){auto*l=BookText(v->content,m.status,24,552,672,0x8D877C);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);v->status=l;}
 auto*body=lv_obj_create(v->content);v->body=body;screen_strip_obj_chrome(body);lv_obj_set_pos(body,16,body_y);lv_obj_set_size(body,688,body_end-body_y);lv_obj_set_style_bg_color(body,lv_color_hex(paper),0);lv_obj_set_style_bg_opa(body,LV_OPA_COVER,0);lv_obj_set_style_pad_all(body,reading?22:12,0);lv_obj_set_style_pad_row(body,shelf?20:12,0);lv_obj_set_style_pad_column(body,16,0);lv_obj_set_flex_flow(body,shelf?LV_FLEX_FLOW_ROW_WRAP:LV_FLEX_FLOW_COLUMN);lv_obj_add_flag(body,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(body,LV_DIR_VER);
 unsigned index=0;for(const auto&i:m.items){
  if(shelf&&(i.kind=="book"||i.kind=="folder")){BookTile(v,i,index++);continue;}
  if(i.kind=="hero")continue;
  if(i.kind=="image"){WebImageCreate(body,i.value,"",i.text,false);continue;}
  if(i.kind=="button"||i.kind=="link"||i.kind=="note"){
   auto*b=Button(body,i.text,new Action{v,i.id,i.value,false},shelf?208:640,LV_SIZE_CONTENT);lv_obj_set_style_pad_ver(b,14,0);BookButtonStyle(b);auto*l=lv_obj_get_child(b,0);lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);lv_obj_set_height(l,LV_SIZE_CONTENT);continue;
  }
  auto*l=Label(body,i.text,reading?640:640,false);lv_obj_set_style_text_color(l,lv_color_hex(reading?0x413C32:0x434A54),0);lv_obj_set_style_text_line_space(l,reading?11:9,0);
 }
 screen_swipe_back_ignore(body);
 if(reading&&!m.swipe_left.empty()&&!m.swipe_right.empty()){v->reader_swipe.context=v;v->reader_swipe.turn=[](void*p,bool next){auto*v=(View*)p;return CommitFields(v,next?v->swipe_left:v->swipe_right,"");};BindReaderSwipe(body,&v->reader_swipe);}
 v->items=std::move(m.items);v->cursor=v->items.size();
}
