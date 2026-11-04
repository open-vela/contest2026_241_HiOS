// VLC-inspired media library: bounded static rows, existing logo and fonts.
void VideoButtonStyle(lv_obj_t*b,bool selected=false){
 lv_obj_set_style_bg_color(b,lv_color_hex(selected?0xF08B32:0x292D36),0);lv_obj_set_style_bg_color(b,lv_color_hex(selected?0xD97520:0x3C424E),LV_STATE_PRESSED);lv_obj_set_style_radius(b,12,0);lv_obj_set_style_text_color(lv_obj_get_child(b,0),lv_color_hex(selected?0x171A20:0xE4E7ED),0);
}
void RenderVideoLibrary(View*v,Model&m){
 lv_obj_set_style_bg_color(v->root,lv_color_hex(0x171A20),0);lv_obj_set_style_bg_color(v->back,lv_color_hex(0x242830),0);lv_obj_set_style_border_width(v->back,0,0);lv_obj_set_style_text_color(lv_obj_get_child(v->back,0),lv_color_hex(0xE4E7ED),0);lv_obj_set_size(v->back,82,44);
 Record r;r.png=m.icon;auto*logo=RecordIcon(v->content,r,40);lv_obj_set_pos(logo,110,8);BookText(v->content,"Hi 播",162,8,500,0xF2F3F5,true);
 auto*status=BookText(v->content,m.status,24,58,672,0x9BA4B2);lv_label_set_long_mode(status,LV_LABEL_LONG_DOT);lv_obj_set_height(status,26);v->status=status;
 for(size_t j=0;j<m.buttons.size();++j){const auto&i=m.buttons[j];auto*b=Button(v->content,i.text,new Action{v,i.id,i.value,false},104,46);lv_obj_set_pos(b,24+j*114,98);VideoButtonStyle(b,i.id==m.selected);}
 if(m.input){v->input=lv_textarea_create(v->content);lv_obj_set_pos(v->input,24,158);lv_obj_set_size(v->input,538,52);lv_textarea_set_one_line(v->input,true);lv_textarea_set_max_length(v->input,1024);lv_obj_set_style_text_font(v->input,&font_puhui_full_20,0);lv_obj_set_style_bg_color(v->input,lv_color_hex(0x242830),0);lv_obj_set_style_text_color(v->input,lv_color_hex(0xFFFFFF),0);lv_obj_set_style_border_color(v->input,lv_color_hex(0x414956),0);lv_obj_set_style_radius(v->input,12,0);lv_textarea_set_text(v->input,m.address.c_str());lv_textarea_set_placeholder_text(v->input,m.placeholder.c_str());HiInputMethod_Bind(v->input,false);screen_swipe_back_ignore(v->input);
  auto*b=Button(v->content,"播放",new Action{v,m.submit,"",true},116,52);lv_obj_set_pos(b,580,158);VideoButtonStyle(b,true);auto*a=new Action{v,m.submit,"",true};lv_obj_add_event_cb(v->input,Click,LV_EVENT_READY,a);lv_obj_add_event_cb(v->input,[](lv_event_t*e){delete(Action*)lv_event_get_user_data(e);},LV_EVENT_DELETE,a);
 }
 const int y=m.input?224:158;auto*body=lv_obj_create(v->content);v->body=body;screen_strip_obj_chrome(body);lv_obj_set_pos(body,16,y);lv_obj_set_size(body,688,600-y);lv_obj_set_style_bg_opa(body,LV_OPA_TRANSP,0);lv_obj_set_style_pad_all(body,8,0);lv_obj_set_style_pad_row(body,12,0);lv_obj_set_flex_flow(body,LV_FLEX_FLOW_COLUMN);lv_obj_add_flag(body,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(body,LV_DIR_VER);
 for(const auto&i:m.items){
  if(i.kind=="hero"){
   auto*card=lv_obj_create(body);screen_strip_obj_chrome(card);lv_obj_set_size(card,664,132);lv_obj_set_style_radius(card,18,0);lv_obj_set_style_bg_color(card,lv_color_hex(0x252A33),0);lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);lv_obj_remove_flag(card,LV_OBJ_FLAG_CLICKABLE);auto*icon=RecordIcon(card,r,88);lv_obj_set_pos(icon,20,20);BookText(card,i.text,132,20,510,0xF7F8FA,true);BookText(card,i.value,132,76,510,0xC1CAD5);continue;
  }
  if(i.kind=="button"||i.kind=="link"){
   auto*b=Button(body,i.text,new Action{v,i.id,i.value,false},664,LV_SIZE_CONTENT);VideoButtonStyle(b);auto*l=lv_obj_get_child(b,0);lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);lv_obj_set_height(l,LV_SIZE_CONTENT);lv_obj_set_style_pad_ver(b,i.id=="remove"?8:16,0);lv_obj_set_style_pad_hor(b,20,0);lv_obj_set_width(l,624);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_LEFT,0);if(i.id=="remove")lv_obj_set_style_text_color(l,lv_color_hex(0xF0A66A),0);continue;
  }
  auto*l=Label(body,i.text,660,false);lv_obj_set_style_text_color(l,lv_color_hex(i.kind=="heading"?0xF1A25D:0xAEB8C6),0);lv_obj_set_style_text_line_space(l,8,0);
 }
 auto*tag=BookText(v->content,"随身视频 本地与网络",24,612,672,0x7E8999);lv_obj_set_style_text_align(tag,LV_TEXT_ALIGN_CENTER,0);screen_swipe_back_ignore(body);v->items=std::move(m.items);v->cursor=v->items.size();
}
