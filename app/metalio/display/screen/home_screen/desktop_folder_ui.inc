// Included inside the launcher namespace; all widget mutations hold the LVGL lock.
lv_obj_t* s_folder_rename=nullptr;
std::string s_open_folder;
void ResetFolderView();
void SyncOpenFolderOrder();

void CloseDesktopFolder(){
    ResetFolderView();HiInputMethod_Suspend();auto* sheet=s_folder_sheet;s_folder_sheet=nullptr;s_folder_rename=nullptr;s_open_folder.clear();
    if(sheet&&lv_obj_is_valid(sheet))lv_obj_delete(sheet);
}

struct DesktopStorageState {
    DesktopStorage::Queue queue;
    bool started=false;unsigned starts=0;
    lv_timer_t* poll=nullptr;
};
DesktopStorageState& DesktopStorageData(){static auto* data=new DesktopStorageState;return *data;}
void PollDesktopStorage(lv_timer_t*){
    auto& data=DesktopStorageData();
    while(auto* request=data.queue.Poll()){
        if(request->layout){
            if(request->saved){DesktopFolders::Commit(request->model);s_order_dirty=false;
                if(request->refresh){if(!request->keep_folder)CloseDesktopFolder();s_folder_refresh=true;}
            }else{s_folder_resume_edit=false;ShowUnavailableToast("保存失败，请重试",false);}
            s_folder_saving=false;if(request->keep_folder)SyncOpenFolderOrder();
            printf("DESKTOP_FOLDER_SAVE ok=%d folders=%u\n",request->saved,(unsigned)request->model.folders.size());
        }else printf("HOME_ORDER_SAVE ok=%d\n",request->saved);
        delete request;
    }
}
bool EnsureDesktopStorage(){
    auto& data=DesktopStorageData();if(!data.queue.Ready())return false;
    if(!data.poll){data.poll=lv_timer_create(PollDesktopStorage,50,nullptr);if(!data.poll)return false;}
    if(data.started)return true;
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,32768);pthread_t task;
    int rc=hi_task_create_legacy(&task,&attr,[](void*)->void*{
        auto& data=DesktopStorageData();
        for(;;){
            auto* request=data.queue.Wait();if(!request){usleep(1000);continue;}
            // Flash access keeps the same LVGL/cache-off exclusion as before.
            // All widget changes and request destruction happen on the UI loop.
            lv_lock();
            if(request->layout)request->saved=DesktopFolders::Save(request->model);
            else{Settings cfg("ui",true);request->saved=cfg.SetStringSaved("home_order",request->order);}
            lv_unlock();data.queue.Complete(request);
        }
        return nullptr;
    },nullptr);
    pthread_attr_destroy(&attr);
    if(rc)return false;
    data.started=true;++data.starts;pthread_detach(task);
    printf("DESKTOP_STORAGE_WORKER_READY task=%lu\n",(unsigned long)task);return true;
}
void QueueDesktopHomeOrder(std::string order){
    if(!EnsureDesktopStorage()){printf("HOME_ORDER_SAVE ok=0\n");return;}
    auto* request=new DesktopStorage::Request;request->layout=false;request->order=std::move(order);
    if(!DesktopStorageData().queue.Push(request)){delete request;printf("HOME_ORDER_SAVE ok=0\n");}
}
bool SubmitDesktopLayout(const DesktopFolders::Model& model,bool refresh,bool keep_folder){
    if(s_folder_saving||!DesktopFolders::Valid(model)||DesktopFolders::Encode(model).empty()){
        ShowUnavailableToast("桌面正在保存或文件夹已满",false);return false;
    }
    if(!EnsureDesktopStorage()){ShowUnavailableToast("暂时无法保存，请重试",false);return false;}
    auto* request=new DesktopStorage::Request;request->model=model;request->refresh=refresh;request->keep_folder=keep_folder;
    s_folder_saving=true;
    if(!DesktopStorageData().queue.Push(request)){
        s_folder_saving=false;delete request;ShowUnavailableToast("暂时无法保存，请重试",false);return false;
    }
    return true;
}

struct FolderSlotBake{lv_image_dsc_t image{};uint8_t* data=nullptr;};
void BakeFolderImage(lv_obj_t* icon,int size){
    const void* src=lv_image_get_src(icon);
    if(!src||size<=0||size>256)return;
    lv_image_decoder_dsc_t decoder{};lv_image_decoder_args_t args{};args.no_cache=true;
    if(lv_image_decoder_open(&decoder,src,&args)!=LV_RESULT_OK){
        lv_obj_set_size(icon,size,size);lv_image_set_inner_align(icon,LV_IMAGE_ALIGN_CONTAIN);return;
    }
    const lv_draw_buf_t* p=decoder.decoded;
    if(!p||p->header.cf!=LV_COLOR_FORMAT_ARGB8888||!p->data||!p->header.w||!p->header.h||p->header.w>512||p->header.h>512){
        lv_image_decoder_close(&decoder);
        lv_obj_set_size(icon,size,size);lv_image_set_inner_align(icon,LV_IMAGE_ALIGN_CONTAIN);return;
    }
    const int sw=(int)p->header.w,sh=(int)p->header.h,span=std::max(sw,sh);
    const int fw=std::max(1,sw*size/span),fh=std::max(1,sh*size/span);
    const int ox=(size-fw)/2,oy=(size-fh)/2;
    auto* buf=static_cast<uint8_t*>(std::malloc((size_t)size*size*4));
    if(!buf){lv_image_decoder_close(&decoder);lv_obj_set_size(icon,size,size);lv_image_set_inner_align(icon,LV_IMAGE_ALIGN_CONTAIN);return;}
    std::memset(buf,0,(size_t)size*size*4);
    const int stride=(int)p->header.stride;
    for(int y=0;y<fh;++y){
        int sy=fh<=1?0:y*(sh-1)/(fh-1);if(sy>=sh)sy=sh-1;
        const uint8_t* row=p->data+(size_t)sy*stride;
        uint8_t* dst=buf+((size_t)(oy+y)*size+ox)*4;
        for(int x=0;x<fw;++x){
            int sx=fw<=1?0:x*(sw-1)/(fw-1);if(sx>=sw)sx=sw-1;
            const uint8_t* s=row+sx*4;dst[0]=s[0];dst[1]=s[1];dst[2]=s[2];dst[3]=s[3];dst+=4;
        }
    }
    const uint32_t flags=p->header.flags&LV_IMAGE_FLAGS_PREMULTIPLIED;
    lv_image_decoder_close(&decoder);
    auto* bake=new FolderSlotBake;
    if(!bake){std::free(buf);lv_obj_set_size(icon,size,size);lv_image_set_inner_align(icon,LV_IMAGE_ALIGN_CONTAIN);return;}
    bake->data=buf;bake->image.header.magic=LV_IMAGE_HEADER_MAGIC;bake->image.header.cf=LV_COLOR_FORMAT_ARGB8888;
    bake->image.header.flags=flags;bake->image.header.w=size;bake->image.header.h=size;bake->image.header.stride=size*4;
    bake->image.data_size=(uint32_t)size*size*4;bake->image.data=buf;
    lv_obj_remove_style_all(icon);lv_obj_set_size(icon,size,size);
    lv_image_set_src(icon,&bake->image);lv_image_set_inner_align(icon,LV_IMAGE_ALIGN_TOP_LEFT);
    lv_image_set_pivot(icon,0,0);lv_image_set_scale(icon,256);
    lv_obj_add_event_cb(icon,[](lv_event_t* e){auto* bake=(FolderSlotBake*)lv_event_get_user_data(e);auto* i=lv_event_get_target_obj(e);
        lv_image_set_src(i,nullptr);lv_image_cache_drop(&bake->image);lv_image_header_cache_drop(&bake->image);std::free(bake->data);delete bake;
    },LV_EVENT_DELETE,bake);
}
lv_obj_t* FolderAppIcon(lv_obj_t* parent,const std::string& id,int size){
    const int index=HomeCatalogIndex(id);
    lv_obj_t* icon=nullptr;
    if(index>=0&&index<kTotalApps)icon=HomeScreen::CreateAppIcon(parent,kApps[index].icon_suffix,size);
    else if(index>=kTotalApps&&index<kTotalApps+(int)s_external_apps.size())icon=HiApp::RecordIcon(parent,s_external_apps[index-kTotalApps],size,true);
    else if(id.compare(0,6,"hiapp:")==0)icon=HiApp::RegistryIcon(parent,id.c_str(),size,true);
    if(!icon){
        icon=lv_obj_create(parent);lv_obj_remove_style_all(icon);lv_obj_set_size(icon,size,size);
        lv_obj_set_style_bg_color(icon,lv_color_hex(0x374151),0);
        lv_obj_set_style_bg_opa(icon,LV_OPA_COVER,0);lv_obj_set_style_radius(icon,size/5,0);
    }else if(lv_obj_check_type(icon,&lv_image_class)){
        /* Fit the whole logo into the slot once. Later swipes blit it. */
        BakeFolderImage(icon,size);
    }
    HomeIconPassiveTree(icon);
    return icon;
}

lv_obj_t* CreateFolderCell(lv_obj_t* parent,int index){
    const int n=index-kTotalApps-s_external_apps.size();if(n<0||n>=(int)s_desktop_folders.size())return nullptr;
    const auto& folder=s_desktop_folders[n];auto* cell=lv_obj_create(parent);lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell,kCellWidth,kCellHeight);lv_obj_remove_flag(cell,LV_OBJ_FLAG_SCROLLABLE);
    // A small, recessed nine-compartment tray, with warm bevels and shelves.
    auto* preview=lv_obj_create(cell);lv_obj_remove_style_all(preview);lv_obj_set_size(preview,kIconSize,kIconSize);
    lv_obj_set_style_bg_color(preview,lv_color_hex(0xB0A89A),0);lv_obj_set_style_bg_grad_color(preview,lv_color_hex(0x80796F),0);
    lv_obj_set_style_bg_grad_dir(preview,LV_GRAD_DIR_VER,0);lv_obj_set_style_bg_opa(preview,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(preview,2,0);lv_obj_set_style_border_color(preview,lv_color_hex(0xD7CEBE),0);
    lv_obj_set_style_radius(preview,18,0);lv_obj_set_style_shadow_width(preview,6,0);lv_obj_set_style_shadow_ofs_y(preview,3,0);
    lv_obj_set_style_shadow_opa(preview,LV_OPA_30,0);lv_obj_align(preview,LV_ALIGN_TOP_MID,0,0);
    lv_obj_remove_flag(preview,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));
    const int mini=28;
    for(int row=0;row<3;row++){
        auto* shelf=lv_obj_create(preview);lv_obj_remove_style_all(shelf);lv_obj_set_size(shelf,108,3);
        lv_obj_set_pos(shelf,8,42+row*38);lv_obj_set_style_bg_color(shelf,lv_color_hex(0xD2C9B9),0);
        lv_obj_set_style_bg_opa(shelf,LV_OPA_80,0);lv_obj_remove_flag(shelf,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
    }
    for(size_t i=0;i<folder.items.size()&&i<9;i++){
        auto* icon=FolderAppIcon(preview,folder.items[i],mini);lv_obj_set_pos(icon,10+(i%3)*38,10+(i/3)*38);
    }
    auto* label=lv_label_create(cell);lv_label_set_text(label,folder.name.c_str());lv_obj_set_width(label,kCellWidth);
    lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);lv_obj_set_style_text_font(label,&font_puhui_20_4,0);
    lv_obj_set_style_text_color(label,lv_color_hex(0xE5E7EB),0);lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_align(label,LV_ALIGN_TOP_MID,0,kIconSize+kNameGap);lv_obj_remove_flag(label,LV_OBJ_FLAG_CLICKABLE);
    auto* info=new AppCellInfo;info->app_idx=index;info->folder_id=folder.id;lv_obj_set_user_data(cell,info);
    lv_obj_add_event_cb(cell,OnAppCellDeleted,LV_EVENT_DELETE,nullptr);
    lv_obj_add_flag(cell,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|kAppCellFlag|LV_OBJ_FLAG_PRESS_LOCK));return cell;
}

#include "desktop_folder_pager.inc"

void RenameDesktopFolder(){
    const auto model=DesktopFolders::Snapshot();const auto* folder=DesktopFolders::Find(model,s_open_folder);if(!folder||!s_folder_sheet)return;
    if(s_folder_rename)return;
    auto* panel=lv_obj_create(s_folder_sheet);s_folder_rename=panel;lv_obj_set_size(panel,620,210);lv_obj_align(panel,LV_ALIGN_TOP_MID,0,18);
    lv_obj_set_style_bg_color(panel,lv_color_hex(0xEAE3D7),0);lv_obj_set_style_border_color(panel,lv_color_hex(0xAB9C83),0);lv_obj_remove_flag(panel,LV_OBJ_FLAG_SCROLLABLE);
    auto* field=lv_textarea_create(panel);lv_obj_set_size(field,560,70);lv_obj_align(field,LV_ALIGN_TOP_MID,0,0);
    lv_textarea_set_one_line(field,true);lv_textarea_set_max_length(field,16);lv_textarea_set_text(field,folder->name.c_str());
    lv_obj_set_style_text_font(field,&font_puhui_30_4,0);lv_obj_set_style_text_color(field,lv_color_hex(0x4B4235),0);
    lv_obj_set_style_bg_color(field,lv_color_hex(0xFFFCF7),0);HiInputMethod_Bind(field,true);
    auto* save=lv_button_create(panel);lv_obj_set_size(save,160,58);lv_obj_align(save,LV_ALIGN_BOTTOM_RIGHT,0,0);
    auto* label=lv_label_create(save);lv_label_set_text(label,"保存名称");lv_obj_set_style_text_font(label,&font_puhui_20_4,0);lv_obj_center(label);
    lv_obj_add_event_cb(save,[](lv_event_t* e){auto* field=static_cast<lv_obj_t*>(lv_event_get_user_data(e));
        if(!HiInputMethod_Flush(field))return;
        DesktopFolders::Model next;if(DesktopFolders::Rename(DesktopFolders::Snapshot(),s_open_folder,lv_textarea_get_text(field),next))SubmitDesktopLayout(next);
        else ShowUnavailableToast("请输入有效名称",false);
    },LV_EVENT_CLICKED,field);HiInputMethod_Show(field,true);
}

void OpenDesktopFolder(const std::string& id){
    if(s_folder_saving)return;const auto model=DesktopFolders::Snapshot();const auto* folder=DesktopFolders::Find(model,id);if(!folder)return;
    CloseDesktopFolder();s_open_folder=id;
    lv_display_t* disp=lv_display_get_default();
    if(disp) lv_display_enable_invalidation(disp,false);
    auto* sheet=lv_obj_create(s_home_root);s_folder_sheet=sheet;lv_obj_remove_style_all(sheet);lv_obj_set_size(sheet,kPanelSize,kPanelSize);
    lv_obj_set_style_bg_color(sheet,lv_color_hex(0x12100E),0);lv_obj_set_style_bg_opa(sheet,LV_OPA_COVER,0);
    lv_obj_remove_flag(sheet,LV_OBJ_FLAG_SCROLLABLE);screen_swipe_back_ignore(sheet,true);
    lv_obj_add_event_cb(sheet,[](lv_event_t* e){if(s_folder_sheet==lv_event_get_target_obj(e)){HiInputMethod_Suspend();ResetFolderView();s_folder_sheet=nullptr;s_folder_rename=nullptr;s_open_folder.clear();}},LV_EVENT_DELETE,nullptr);
    lv_obj_add_event_cb(sheet,[](lv_event_t*e){if(lv_event_get_target_obj(e)==lv_event_get_current_target_obj(e))CloseDesktopFolder();},LV_EVENT_CLICKED,nullptr);
    auto* card=lv_obj_create(sheet);lv_obj_remove_style_all(card);lv_obj_set_size(card,656,656);lv_obj_center(card);
    lv_obj_set_style_radius(card,22,0);lv_obj_set_style_bg_color(card,lv_color_hex(0xF4EFE6),0);
    lv_obj_set_style_bg_grad_color(card,lv_color_hex(0xD7CCB9),0);lv_obj_set_style_bg_grad_dir(card,LV_GRAD_DIR_VER,0);
    lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);lv_obj_set_style_border_width(card,2,0);
    lv_obj_set_style_border_color(card,lv_color_hex(0xA1937D),0);lv_obj_set_style_shadow_width(card,18,0);
    lv_obj_set_style_shadow_opa(card,LV_OPA_40,0);lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);
    auto* title=lv_button_create(card);lv_obj_remove_style_all(title);lv_obj_set_size(title,470,64);lv_obj_align(title,LV_ALIGN_TOP_MID,0,10);
    auto* text=lv_label_create(title);lv_label_set_text(text,folder->name.c_str());lv_obj_set_width(text,458);
    lv_label_set_long_mode(text,LV_LABEL_LONG_DOT);lv_obj_set_style_text_align(text,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_font(text,&font_puhui_30_4,0);lv_obj_set_style_text_color(text,lv_color_hex(0x4B4235),0);lv_obj_center(text);
    lv_obj_add_event_cb(title,[](lv_event_t*){RenameDesktopFolder();},LV_EVENT_CLICKED,nullptr);
    auto* close=lv_button_create(card);lv_obj_remove_style_all(close);lv_obj_set_size(close,72,64);lv_obj_align(close,LV_ALIGN_TOP_RIGHT,-18,10);
    static const lv_point_precise_t cross_a[]={{0,0},{24,24}},cross_b[]={{24,0},{0,24}};
    for(int i=0;i<2;i++){auto* stroke=lv_line_create(close);lv_line_set_points(stroke,i?cross_b:cross_a,2);
        lv_obj_set_style_line_width(stroke,3,0);lv_obj_set_style_line_color(stroke,lv_color_hex(0x675B48),0);
        lv_obj_set_style_line_rounded(stroke,true,0);lv_obj_center(stroke);lv_obj_remove_flag(stroke,LV_OBJ_FLAG_CLICKABLE);}
    lv_obj_add_event_cb(close,[](lv_event_t*){CloseDesktopFolder();},LV_EVENT_CLICKED,nullptr);
    auto* line=lv_obj_create(card);lv_obj_remove_style_all(line);lv_obj_set_size(line,604,2);lv_obj_set_pos(line,26,78);
    lv_obj_set_style_bg_color(line,lv_color_hex(0xB4A58D),0);lv_obj_set_style_bg_opa(line,LV_OPA_60,0);
    lv_obj_remove_flag(line,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
    CreateFolderPager(card,*folder);
    auto* ungroup=lv_button_create(card);lv_obj_remove_style_all(ungroup);
    lv_obj_set_style_bg_color(ungroup,lv_color_hex(0xBEB09A),0);lv_obj_set_style_bg_opa(ungroup,LV_OPA_COVER,0);lv_obj_set_style_border_color(ungroup,lv_color_hex(0x9A8B74),0);
    lv_obj_set_style_border_width(ungroup,1,0);lv_obj_set_style_radius(ungroup,12,0);lv_obj_set_style_shadow_width(ungroup,0,0);lv_obj_set_size(ungroup,164,40);lv_obj_align(ungroup,LV_ALIGN_BOTTOM_LEFT,20,-8);
    text=lv_label_create(ungroup);lv_label_set_text(text,"解散文件夹");lv_obj_set_style_text_color(text,lv_color_hex(0x514635),0);lv_obj_set_style_text_font(text,&font_puhui_20_4,0);lv_obj_center(text);
    auto* done=lv_button_create(card);lv_obj_remove_style_all(done);lv_obj_set_size(done,120,40);lv_obj_align(done,LV_ALIGN_BOTTOM_RIGHT,-20,-8);
    lv_obj_set_style_bg_color(done,lv_color_hex(0xBEB09A),0);lv_obj_set_style_bg_opa(done,LV_OPA_COVER,0);lv_obj_set_style_radius(done,10,0);lv_obj_set_style_shadow_width(done,0,0);
    auto* done_text=lv_label_create(done);lv_label_set_text(done_text,"完成");lv_obj_set_style_text_font(done_text,&font_puhui_20_4,0);
    lv_obj_set_style_text_color(done_text,lv_color_hex(0x514635),0);lv_obj_center(done_text);lv_obj_add_event_cb(done,[](lv_event_t*){CloseDesktopFolder();},LV_EVENT_CLICKED,nullptr);
    lv_obj_add_event_cb(ungroup,[](lv_event_t*){DesktopFolders::Model next;if(DesktopFolders::Ungroup(DesktopFolders::Snapshot(),s_open_folder,next))SubmitDesktopLayout(next);},LV_EVENT_CLICKED,nullptr);
    if(disp) lv_display_enable_invalidation(disp,true);
    lv_obj_invalidate(sheet);
}
