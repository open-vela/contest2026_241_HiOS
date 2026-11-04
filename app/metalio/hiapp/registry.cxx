#include "hi_task_policy.h"
// SPDX-License-Identifier: Apache-2.0
#include "registry.h"
#include "app_manager_screen/app_manager_screen.h"
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <cstdio>
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "src/draw/lv_image_decoder_private.h"
namespace HiApp {namespace {
struct Registry {std::mutex lock;std::vector<Record>entries;unsigned revision=1;bool started=false,order_loaded=false,order_worker=false,order_dirty=false;unsigned order_epoch=0;std::vector<std::string>order;};
Registry&Get(){static Registry*r=new Registry;return *r;}
bool Same(const std::vector<Record>&a,const std::vector<Record>&b){if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();i++)if(a[i].id!=b[i].id||a[i].name!=b[i].name||a[i].version!=b[i].version||a[i].icon!=b[i].icon||(a[i].png?a[i].png->fingerprint:0)!=(b[i].png?b[i].png->fingerprint:0))return false;return true;}
}
void RegistryPublish(const std::vector<Record>&all,unsigned expected_revision){std::vector<Record>valid;for(const auto&r:all)if(r.error.empty()&&ValidId(r.id))valid.push_back(r);std::sort(valid.begin(),valid.end(),[](const Record&a,const Record&b){return a.id<b.id;});auto&r=Get();std::lock_guard<std::mutex>g(r.lock);if(expected_revision&&expected_revision!=r.revision)return;if(!Same(r.entries,valid)){r.entries=std::move(valid);++r.revision;}}
void RegistryUpsert(const Record&record){auto&r=Get();std::lock_guard<std::mutex>g(r.lock);for(auto&e:r.entries)if(e.id==record.id){e=record;++r.revision;return;}r.entries.push_back(record);std::sort(r.entries.begin(),r.entries.end(),[](const Record&a,const Record&b){return a.id<b.id;});++r.revision;}
void RegistryRemove(const std::string&id){auto&r=Get();std::lock_guard<std::mutex>g(r.lock);r.entries.erase(std::remove_if(r.entries.begin(),r.entries.end(),[&](const Record&e){return e.id==id;}),r.entries.end());++r.revision;}

unsigned RegistryRevision(){auto&r=Get();std::lock_guard<std::mutex>g(r.lock);return r.revision;}
std::vector<Record>RegistrySnapshot(){auto&r=Get();std::lock_guard<std::mutex>g(r.lock);return r.entries;}
// Keep FAT lock owners just above wake inference, below UI and audio. This build
// has no priority inheritance; a low-priority writer can otherwise starve
// readers behind the volume lock while the voice worker is runnable. Readers use the same I/O priority.
void RegistryStart(){auto&r=Get();{std::lock_guard<std::mutex>g(r.lock);if(r.started)return;r.started=true;}pthread_t t;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,32768);sched_param p{};p.sched_priority=81;pthread_attr_setschedparam(&a,&p);pthread_attr_setinheritsched(&a,PTHREAD_EXPLICIT_SCHED);int rc=hi_task_create_compute(&t,&a,[](void*)->void*{usleep(15*1000000);std::string error;Scan(true,error);RegistryLoadOrder();return nullptr;},nullptr);pthread_attr_destroy(&a);if(!rc)pthread_detach(t);else{std::lock_guard<std::mutex>g(r.lock);r.started=false;}}
void RegistryLoadOrder(){auto&r=Get();{std::lock_guard<std::mutex>g(r.lock);if(r.order_loaded||r.order_epoch)return;}std::string raw;bool found=ReadLauncherOrder(raw)&&raw.compare(0,5,"HLO1\n")==0;std::vector<std::string>ids;if(found){size_t at=5;while(at<raw.size()&&ids.size()<512){auto end=raw.find('\n',at);auto id=raw.substr(at,end==raw.npos?raw.npos:end-at);if(!id.empty()&&id.size()<=40&&id.find_first_of("\r\t ")==id.npos)ids.push_back(id);if(end==raw.npos)break;at=end+1;}}std::lock_guard<std::mutex>g(r.lock);if(!r.order_epoch&&!r.order_loaded){r.order=std::move(ids);r.order_loaded=true;r.order_dirty=!found;++r.revision;}}
std::vector<std::string>RegistryOrder(bool*loaded){auto&r=Get();std::lock_guard<std::mutex>g(r.lock);if(loaded)*loaded=r.order_loaded;return r.order;}
void RegistrySaveOrder(const std::vector<std::string>&ids){auto&r=Get();{std::lock_guard<std::mutex>g(r.lock);if(r.order_loaded&&r.order==ids&&(!r.order_dirty||r.order_worker))return;r.order=ids;r.order_loaded=true;r.order_dirty=true;++r.order_epoch;++r.revision;if(r.order_worker)return;r.order_worker=true;}
 pthread_t t;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,16384);sched_param p{};p.sched_priority=81;pthread_attr_setschedparam(&a,&p);pthread_attr_setinheritsched(&a,PTHREAD_EXPLICIT_SCHED);int rc=hi_task_create_compute(&t,&a,[](void*)->void*{auto&r=Get();for(;;){std::vector<std::string>ids;unsigned epoch;{std::lock_guard<std::mutex>g(r.lock);ids=r.order;epoch=r.order_epoch;}std::string raw="HLO1\n";for(const auto&id:ids){if(raw.size()+id.size()+1>16*1024)break;raw+=id+"\n";}bool ok=WriteLauncherOrder(raw);printf("HIAPP_ORDER_SAVE ok=%d epoch=%u\n",ok,epoch);std::lock_guard<std::mutex>g(r.lock);if(epoch==r.order_epoch){r.order_worker=false;r.order_dirty=!ok;return nullptr;}}},nullptr);pthread_attr_destroy(&a);if(!rc)pthread_detach(t);else{std::lock_guard<std::mutex>g(r.lock);r.order_worker=false;}
}
namespace {
struct ImageOwner{lv_image_dsc_t image{};Ref<PngIcon>png;uint8_t* baked=nullptr;};
struct IconBounds{int x=0,y=0,w=0,h=0;};
bool MeasureIcon(const void*source,IconBounds&bounds){
 lv_image_decoder_dsc_t decoder{};lv_image_decoder_args_t args{};args.no_cache=true;
 if(lv_image_decoder_open(&decoder,source,&args)!=LV_RESULT_OK)return false;
 const auto*p=decoder.decoded;bool ok=p&&p->header.w&&p->header.h&&p->header.w<=256&&p->header.h<=256;
 if(ok){bounds={0,0,(int)p->header.w,(int)p->header.h};
  if(p->header.cf==LV_COLOR_FORMAT_ARGB8888){int left=p->header.w,top=p->header.h,right=-1,bottom=-1;
   for(int y=0;y<(int)p->header.h;y++)for(int x=0;x<(int)p->header.w;x++)if(p->data[y*p->header.stride+x*4+3]>=16){left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);}
   ok=right>=left&&bottom>=top;if(ok)bounds={left,top,right-left+1,bottom-top+1};
  }
 }
 lv_image_decoder_close(&decoder);return ok;
}
bool PackageBounds(const Ref<PngIcon>&png,const void*source,IconBounds&bounds){
 // Keep render-only data out of the package's shared ABI. Eight entries
 // retain at most 512 KiB of compressed assets, and only LVGL accesses them.
 struct Entry{Ref<PngIcon>png;IconBounds bounds;};
 static auto*cache=new std::vector<Entry>;
 for(const auto&e:*cache)if(e.png.get()==png.get()){bounds=e.bounds;return true;}
 if(!MeasureIcon(source,bounds))return false;
 if(cache->size()>=8)cache->erase(cache->begin());cache->push_back({png,bounds});return true;
}
lv_obj_t*IconFrame(lv_obj_t*parent,const void*source,int width,int height,const IconBounds&bounds,int size){
 auto*frame=lv_obj_create(parent);lv_obj_remove_style_all(frame);lv_obj_set_size(frame,size,size);
 lv_obj_remove_flag(frame,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));
 auto*i=lv_image_create(frame);lv_image_set_src(i,source);lv_obj_set_size(i,width,height);lv_image_set_pivot(i,0,0);
 const int scale=std::max(1,size*256/std::max(bounds.w,bounds.h));lv_image_set_scale(i,scale);
 lv_obj_set_pos(i,(size-bounds.w*scale/256)/2-bounds.x*scale/256,(size-bounds.h*scale/256)/2-bounds.y*scale/256);
 lv_obj_remove_flag(i,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));return frame;
}
// Folder tiles clip children, so the shared frame above loses its artwork there.
// This keeps the same scale and keeps the image widget inside the slot.
// Default image align is center, which would crop a different region.
lv_obj_t*IconFrameInSlot(lv_obj_t*parent,const void*source,const IconBounds&bounds,int size){
 auto*frame=lv_obj_create(parent);lv_obj_remove_style_all(frame);lv_obj_set_size(frame,size,size);
 lv_obj_remove_flag(frame,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));
 auto*i=lv_image_create(frame);lv_image_set_src(i,source);lv_obj_set_size(i,size,size);
 lv_image_set_inner_align(i,LV_IMAGE_ALIGN_TOP_LEFT);lv_image_set_pivot(i,0,0);
 const int span=std::max(bounds.w,bounds.h);const int scale=std::max(1,size*256/span);
 const int fitted_w=std::max(1,bounds.w*scale/256),fitted_h=std::max(1,bounds.h*scale/256);
 lv_image_set_offset_x(i,(size-fitted_w)/2-bounds.x*scale/256);
 lv_image_set_offset_y(i,(size-fitted_h)/2-bounds.y*scale/256);
 lv_image_set_scale(i,static_cast<uint32_t>(scale));
 lv_obj_remove_flag(i,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));return frame;
}
lv_obj_t*IconFramePlain(lv_obj_t*parent,const void*source,int size){
 auto*frame=lv_obj_create(parent);lv_obj_remove_style_all(frame);lv_obj_set_size(frame,size,size);
 lv_obj_remove_flag(frame,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));
 auto*i=lv_image_create(frame);lv_obj_remove_style_all(i);lv_obj_set_size(i,size,size);
 lv_image_set_src(i,source);lv_image_set_inner_align(i,LV_IMAGE_ALIGN_TOP_LEFT);lv_image_set_pivot(i,0,0);lv_image_set_scale(i,256);
 lv_obj_remove_flag(i,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));return frame;
}
bool BakeSlot(ImageOwner*owner,const IconBounds&bounds,int size){
 if(!owner||size<=0||size>256||bounds.w<=0||bounds.h<=0)return false;
 lv_image_decoder_dsc_t decoder{};lv_image_decoder_args_t args{};args.no_cache=true;
 if(lv_image_decoder_open(&decoder,&owner->image,&args)!=LV_RESULT_OK)return false;
 const lv_draw_buf_t*p=decoder.decoded;
 if(!p||p->header.cf!=LV_COLOR_FORMAT_ARGB8888||!p->data||!p->header.w||!p->header.h){lv_image_decoder_close(&decoder);return false;}
 const int span=std::max(bounds.w,bounds.h);
 const int fw=std::max(1,bounds.w*size/span),fh=std::max(1,bounds.h*size/span);
 const int ox=(size-fw)/2,oy=(size-fh)/2;
 auto*buf=static_cast<uint8_t*>(std::malloc((size_t)size*size*4));
 if(!buf){lv_image_decoder_close(&decoder);return false;}
 std::memset(buf,0,(size_t)size*size*4);
 const int sw=(int)p->header.w,sh=(int)p->header.h,stride=(int)p->header.stride;
 for(int y=0;y<fh;++y){
  int sy=bounds.y+(fh<=1?0:y*(bounds.h-1)/(fh-1));if(sy<0)sy=0;if(sy>=sh)sy=sh-1;
  const uint8_t*row=p->data+(size_t)sy*stride;
  uint8_t*dst=buf+((size_t)(oy+y)*size+ox)*4;
  for(int x=0;x<fw;++x){
   int sx=bounds.x+(fw<=1?0:x*(bounds.w-1)/(fw-1));if(sx<0)sx=0;if(sx>=sw)sx=sw-1;
   const uint8_t*s=row+sx*4;dst[0]=s[0];dst[1]=s[1];dst[2]=s[2];dst[3]=s[3];dst+=4;
  }
 }
 const uint32_t flags=p->header.flags&LV_IMAGE_FLAGS_PREMULTIPLIED;
 lv_image_decoder_close(&decoder);
 lv_image_cache_drop(&owner->image);lv_image_header_cache_drop(&owner->image);
 owner->baked=buf;owner->image={};
 owner->image.header.magic=LV_IMAGE_HEADER_MAGIC;owner->image.header.cf=LV_COLOR_FORMAT_ARGB8888;
 owner->image.header.flags=flags;owner->image.header.w=size;owner->image.header.h=size;owner->image.header.stride=size*4;
 owner->image.data_size=(uint32_t)size*size*4;owner->image.data=buf;return true;
}
}
lv_obj_t*RecordIcon(lv_obj_t*parent,const Record&record,int size,bool in_slot){
 if(record.png&&record.png->bytes.size()>=24){auto&data=record.png->bytes;auto*p=(const unsigned char*)data.data();uint32_t w=((uint32_t)p[16]<<24)|((uint32_t)p[17]<<16)|((uint32_t)p[18]<<8)|p[19],h=((uint32_t)p[20]<<24)|((uint32_t)p[21]<<16)|((uint32_t)p[22]<<8)|p[23];if(w&&h&&w<=256&&h<=256){auto*owner=new ImageOwner;owner->png=record.png;owner->image.header.magic=LV_IMAGE_HEADER_MAGIC;owner->image.header.cf=LV_COLOR_FORMAT_RAW;owner->image.header.w=w;owner->image.header.h=h;owner->image.data_size=data.size();owner->image.data=(const uint8_t*)data.data();IconBounds bounds;
  if(PackageBounds(record.png,&owner->image,bounds)&&bounds.w>0&&bounds.h>0&&bounds.x>=0&&bounds.y>=0&&bounds.x+bounds.w<=(int)w&&bounds.y+bounds.h<=(int)h){
   const bool baked=in_slot&&BakeSlot(owner,bounds,size);
   auto*frame=baked?IconFramePlain(parent,&owner->image,size):in_slot?IconFrameInSlot(parent,&owner->image,bounds,size):IconFrame(parent,&owner->image,w,h,bounds,size);
   auto*i=lv_obj_get_child(frame,0);lv_obj_add_event_cb(i,[](lv_event_t*e){auto*owner=(ImageOwner*)lv_event_get_user_data(e);auto*i=lv_event_get_target_obj(e);lv_image_set_src(i,nullptr);lv_image_cache_drop(&owner->image);lv_image_header_cache_drop(&owner->image);std::free(owner->baked);delete owner;},LV_EVENT_DELETE,owner);return frame;}
  lv_image_cache_drop(&owner->image);lv_image_header_cache_drop(&owner->image);delete owner;
 }}
 // Scanning/installing already supplies owned PNG bytes. Never perform
 // SD I/O while LVGL holds its UI lock; show a safe fallback on bad assets.
 return AppManagerScreen::CreateIcon(parent,size);
}
lv_obj_t*RegistryIcon(lv_obj_t*parent,const char*id,int size,bool in_slot){std::string key=id?id:"";if(key.compare(0,6,"hiapp:")==0)key.erase(0,6);Record record;{auto&r=Get();std::lock_guard<std::mutex>g(r.lock);for(const auto&e:r.entries)if(e.id==key){record=e;break;}}return RecordIcon(parent,record,size,in_slot);}
}
