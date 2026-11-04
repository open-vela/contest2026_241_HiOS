// Folder widgets stay alive throughout a reorder. Persist only after release.
constexpr int kFolderPageSize=9,kFolderPageWidth=608,kFolderPageHeight=444;
constexpr int kFolderCellWidth=196,kFolderCellHeight=140,kFolderRowStride=150;
struct FolderMember {lv_obj_t* cell=nullptr;std::string id;int slot=0;bool consumed=false;};
struct FolderView {
    lv_obj_t *pager=nullptr,*page_label=nullptr,*hint=nullptr,*move_button=nullptr;
    std::vector<lv_obj_t*> pages;
    std::vector<FolderMember*> members;
    FolderMember *pressed=nullptr,*drag=nullptr;
    std::string id,selected;
    lv_point_t press{};
    int page=0,grab_x=0,grab_y=0,edge=0;
    uint32_t edge_tick=0,shift_tick=0;
    bool editing=false,dragging=false;
};
FolderView s_folder_view;
void ResetFolderView(){s_folder_view=FolderView{};}
void FolderSelection(){
    auto& v=s_folder_view;if(!v.pager)return;
    for(auto* member:v.members)lv_obj_set_style_outline_width(member->cell,v.editing&&member->id==v.selected?2:0,0);
    if(v.move_button){if(v.selected.empty()||s_folder_saving)lv_obj_add_state(v.move_button,LV_STATE_DISABLED);else lv_obj_remove_state(v.move_button,LV_STATE_DISABLED);}
    if(v.hint)lv_label_set_text(v.hint,v.editing?"拖动调整顺序 · 边缘停留翻页":"长按拖动排序 · 左右滑动翻页");
}
void FolderPageNumber(){
    auto& v=s_folder_view;if(!v.page_label)return;char text[32];
    snprintf(text,sizeof(text),"%d / %u",v.page+1,(unsigned)v.pages.size());lv_label_set_text(v.page_label,text);
}
void FolderPlaceMembers(){
    auto& v=s_folder_view;
    for(size_t i=0;i<v.members.size();++i){
        auto* m=v.members[i];m->slot=i;if(v.dragging&&m==v.drag)continue;
        auto* page=v.pages[i/kFolderPageSize];
        if(lv_obj_get_parent(m->cell)!=page)lv_obj_set_parent(m->cell,page);
        lv_obj_remove_flag(m->cell,LV_OBJ_FLAG_FLOATING);
        lv_obj_set_pos(m->cell,(i%3)*(kFolderCellWidth+10),((i%9)/3)*kFolderRowStride);
    }
}
void SyncOpenFolderOrder(){
    auto& v=s_folder_view;if(!v.pager||v.dragging)return;
    const auto model=DesktopFolders::Snapshot();const auto* folder=DesktopFolders::Find(model,v.id);if(!folder)return;
    std::vector<FolderMember*> ordered;
    for(const auto& id:folder->items){
        auto found=std::find_if(v.members.begin(),v.members.end(),[&](FolderMember*m){return m->id==id;});
        if(found==v.members.end())return;ordered.push_back(*found);
    }
    if(ordered.size()!=v.members.size())return;
    v.members.swap(ordered);FolderPlaceMembers();FolderSelection();
}
void FolderShiftMember(int target){
    auto& v=s_folder_view;if(!v.dragging||!v.drag||target<0||target>=(int)v.members.size())return;
    const int from=v.drag->slot;if(from==target||lv_tick_elaps(v.shift_tick)<100)return;
    auto* member=v.members[from];v.members.erase(v.members.begin()+from);v.members.insert(v.members.begin()+target,member);
    v.shift_tick=lv_tick_get();FolderPlaceMembers();
}
void FolderStartDrag(FolderMember* member,lv_point_t point){
    auto& v=s_folder_view;if(s_folder_saving||v.dragging||!v.pager||!member)return;
    lv_area_t a,mask;lv_obj_get_coords(member->cell,&a);lv_obj_get_coords(s_folder_sheet,&mask);
    v.grab_x=point.x-a.x1;v.grab_y=point.y-a.y1;v.drag=member;v.dragging=true;v.editing=true;v.selected=member->id;member->consumed=true;
    v.edge=0;v.shift_tick=lv_tick_get();lv_obj_stop_scroll_anim(v.pager);lv_obj_remove_flag(v.pager,LV_OBJ_FLAG_SCROLLABLE);
    lv_display_t* disp=lv_display_get_default();
    if(disp) lv_display_enable_invalidation(disp,false);
    lv_obj_add_flag(member->cell,LV_OBJ_FLAG_FLOATING);lv_obj_set_parent(member->cell,s_folder_sheet);lv_obj_move_foreground(member->cell);
    member->cell->w_layout=0;member->cell->h_layout=0;
    lv_obj_set_size(member->cell,kFolderCellWidth,kFolderCellHeight);
    lv_obj_set_style_bg_opa(member->cell,LV_OPA_TRANSP,0);
    if(disp) lv_display_enable_invalidation(disp,true);
    lv_obj_set_pos(member->cell,point.x-mask.x1-v.grab_x,point.y-mask.y1-v.grab_y);FolderSelection();
}
void FolderMoveDrag(lv_point_t point){
    auto& v=s_folder_view;if(!v.dragging||!v.drag)return;
    lv_area_t mask,area;lv_obj_get_coords(s_folder_sheet,&mask);lv_obj_get_coords(v.pager,&area);
    lv_obj_set_pos(v.drag->cell,point.x-mask.x1-v.grab_x,point.y-mask.y1-v.grab_y);
    const int edge=point.x<area.x1+28?-1:point.x>area.x2-28?1:0;
    if(edge!=v.edge){v.edge=edge;v.edge_tick=lv_tick_get();}
    if(edge&&lv_tick_elaps(v.edge_tick)>=500){
        const int next=v.page+edge;v.edge_tick=lv_tick_get();
        if(next>=0&&next<(int)v.pages.size()){
            v.page=next;lv_obj_scroll_to_x(v.pager,next*kFolderPageWidth,LV_ANIM_OFF);FolderPageNumber();
            v.shift_tick=lv_tick_get()-100;
            FolderShiftMember(edge>0?next*9:std::min((next+1)*9-1,(int)v.members.size()-1));
        }
    }
    if(edge)return;
    const int x=point.x-v.grab_x+kFolderCellWidth/2-area.x1;
    const int y=point.y-v.grab_y+108/2-area.y1;
    FolderShiftMember(DesktopDrag::FolderSlot(v.page,x,y,v.members.size()));
}
void FolderFinishDrag(bool save=true){
    auto& v=s_folder_view;if(!v.dragging)return;
    lv_display_t* disp=lv_display_get_default();
    if(disp) lv_display_enable_invalidation(disp,false);
    v.dragging=false;v.drag=nullptr;v.edge=0;FolderPlaceMembers();
    // While sorting, only the held icon owns movement. Native scrolling would
    // otherwise steal the next drag before its PRESSING handler can run.
    if(v.editing)lv_obj_remove_flag(v.pager,LV_OBJ_FLAG_SCROLLABLE);else lv_obj_add_flag(v.pager,LV_OBJ_FLAG_SCROLLABLE);
    if(!save){SyncOpenFolderOrder();if(disp) lv_display_enable_invalidation(disp,true);if(s_folder_sheet) lv_obj_invalidate(s_folder_sheet);return;}
    std::vector<std::string> order;for(auto* member:v.members)order.push_back(member->id);
    const auto original=DesktopFolders::Snapshot();const auto* folder=DesktopFolders::Find(original,v.id);
    if(folder&&folder->items==order){if(disp) lv_display_enable_invalidation(disp,true);return;}
    DesktopFolders::Model next;
    if(!DesktopFolders::Reorder(original,v.id,order,next)||!SubmitDesktopLayout(next,true,true))SyncOpenFolderOrder();
    FolderSelection();
    if(disp) lv_display_enable_invalidation(disp,true);
    if(s_folder_sheet) lv_obj_invalidate(s_folder_sheet);
}
void FolderMemberEvent(lv_event_t* e){
    auto* member=static_cast<FolderMember*>(lv_event_get_user_data(e));const auto code=lv_event_get_code(e);auto& v=s_folder_view;
    if(code==LV_EVENT_DELETE){
        auto found=std::find(v.members.begin(),v.members.end(),member);if(found!=v.members.end())v.members.erase(found);
        if(v.drag==member){v.drag=nullptr;v.dragging=false;}if(v.pressed==member)v.pressed=nullptr;delete member;return;
    }
    if(s_folder_saving||!v.pager)return;
    auto* input=lv_event_get_indev(e);lv_point_t point{};if(input)lv_indev_get_point(input,&point);
    if(code==LV_EVENT_PRESSED){v.pressed=member;v.press=point;member->consumed=false;return;}
    if(code==LV_EVENT_LONG_PRESSED){if(!v.dragging)FolderStartDrag(member,point);return;}
    if(code==LV_EVENT_PRESSING){
        if(v.dragging){FolderMoveDrag(point);return;}
        if(v.editing&&v.pressed==member&&(std::abs(point.x-v.press.x)>10||std::abs(point.y-v.press.y)>10))FolderStartDrag(member,point);
        return;
    }
    if(code==LV_EVENT_RELEASED){if(v.dragging)FolderFinishDrag();v.pressed=nullptr;return;}
    if(code==LV_EVENT_PRESS_LOST){if(v.dragging)FolderFinishDrag(false);v.pressed=nullptr;return;}
    if(code==LV_EVENT_CLICKED&&!member->consumed){
        if(v.editing){v.selected=member->id;FolderSelection();return;}
        const auto id=member->id;CloseDesktopFolder();HomeScreen::LaunchApp(id.c_str());
    }
}
void FolderPassiveTree(lv_obj_t* root){
    lv_obj_remove_flag(root,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
    for(unsigned i=0;i<lv_obj_get_child_count(root);++i)FolderPassiveTree(lv_obj_get_child(root,i));
}
void CreateFolderPager(lv_obj_t* card,const DesktopFolders::Folder& folder){
    auto& v=s_folder_view;v.id=folder.id;
    auto* pager=lv_obj_create(card);v.pager=pager;lv_obj_remove_style_all(pager);lv_obj_set_size(pager,kFolderPageWidth,kFolderPageHeight);lv_obj_set_pos(pager,24,92);
    lv_obj_set_flex_flow(pager,LV_FLEX_FLOW_ROW);lv_obj_set_scroll_dir(pager,LV_DIR_HOR);lv_obj_set_scroll_snap_x(pager,LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(pager,LV_SCROLLBAR_MODE_OFF);lv_obj_remove_flag(pager,LV_OBJ_FLAG_SCROLL_ELASTIC);
    for(size_t i=0;i<(folder.items.size()+8)/9;i++){
        auto* page=lv_obj_create(pager);lv_obj_remove_style_all(page);lv_obj_set_size(page,kFolderPageWidth,kFolderPageHeight);
        lv_obj_remove_flag(page,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(page,LV_OBJ_FLAG_SNAPPABLE);v.pages.push_back(page);
    }
    lv_obj_add_event_cb(pager,[](lv_event_t* e){auto& v=s_folder_view;
        if(lv_event_get_target_obj(e)!=v.pager||v.dragging)return;
        v.page=std::max(0,std::min((int)v.pages.size()-1,(int)(lv_obj_get_scroll_x(v.pager)+kFolderPageWidth/2)/kFolderPageWidth));FolderPageNumber();
    },LV_EVENT_SCROLL_END,nullptr);
    for(size_t i=0;i<folder.items.size();++i){
        const auto& id=folder.items[i];auto* cell=lv_obj_create(v.pages[i/9]);lv_obj_remove_style_all(cell);lv_obj_set_size(cell,kFolderCellWidth,kFolderCellHeight);
        lv_obj_add_flag(cell,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_PRESS_LOCK));lv_obj_remove_flag(cell,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(cell,14,0);lv_obj_set_style_outline_color(cell,lv_color_hex(0xA38B60),0);lv_obj_set_style_outline_width(cell,0,0);
        auto* icon=FolderAppIcon(cell,id,108);lv_obj_align(icon,LV_ALIGN_TOP_MID,0,0);FolderPassiveTree(icon);
        const int index=HomeCatalogIndex(id);std::string name=index>=0&&index<kTotalApps?kApps[index].name:id;
        if(index>=kTotalApps&&index<kTotalApps+(int)s_external_apps.size())name=s_external_apps[index-kTotalApps].name;
        auto* label=lv_label_create(cell);lv_label_set_text(label,name.c_str());lv_obj_set_width(label,kFolderCellWidth);
        lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);lv_obj_set_style_text_font(label,&font_puhui_20_4,0);lv_obj_set_style_text_color(label,lv_color_hex(0x4B4235),0);
        lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);lv_obj_align(label,LV_ALIGN_BOTTOM_MID,0,-4);lv_obj_remove_flag(label,LV_OBJ_FLAG_CLICKABLE);
        auto* member=new FolderMember{cell,id,(int)i,false};v.members.push_back(member);lv_obj_add_event_cb(cell,FolderMemberEvent,LV_EVENT_ALL,member);
    }
    FolderPlaceMembers();
    auto* page=lv_label_create(card);v.page_label=page;lv_obj_set_style_text_font(page,&font_puhui_20_4,0);
    lv_obj_set_style_text_color(page,lv_color_hex(0x7B6C55),0);lv_obj_align(page,LV_ALIGN_BOTTOM_MID,0,-76);FolderPageNumber();
    auto* hint=lv_label_create(card);v.hint=hint;lv_obj_set_style_text_font(hint,&font_puhui_20_4,0);lv_obj_set_style_text_color(hint,lv_color_hex(0x7B6C55),0);
    lv_obj_align(hint,LV_ALIGN_BOTTOM_MID,0,-50);
    auto* move=lv_button_create(card);v.move_button=move;lv_obj_remove_style_all(move);lv_obj_set_size(move,164,40);lv_obj_align(move,LV_ALIGN_BOTTOM_MID,0,-8);
    lv_obj_set_style_bg_color(move,lv_color_hex(0xBEB09A),0);lv_obj_set_style_bg_opa(move,LV_OPA_COVER,0);lv_obj_set_style_radius(move,10,0);lv_obj_set_style_shadow_width(move,0,0);
    auto* text=lv_label_create(move);lv_label_set_text(text,"移到桌面");lv_obj_set_style_text_font(text,&font_puhui_20_4,0);lv_obj_set_style_text_color(text,lv_color_hex(0x514635),0);lv_obj_center(text);
    lv_obj_add_event_cb(move,[](lv_event_t*){auto& v=s_folder_view;if(s_folder_saving||v.selected.empty())return;DesktopFolders::Model next;
        if(DesktopFolders::Remove(DesktopFolders::Snapshot(),v.id,v.selected,next))SubmitDesktopLayout(next);
    },LV_EVENT_CLICKED,nullptr);
    FolderSelection();
}
