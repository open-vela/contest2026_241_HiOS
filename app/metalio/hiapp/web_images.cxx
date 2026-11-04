#include "hi_task_policy.h"
// SPDX-License-Identifier: Apache-2.0
#include "web_images.h"
#include "hiapp.h"
#include "resources.h"
#include "ota.h"
#include "screen_util.h"
#define LODEPNG_NO_COMPILE_CPP
#include "src/libs/lodepng/lodepng.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
extern "C" {
#include "../third_party/webgif/AnimatedGIF.h"
int hi_web_svg(char *,uint8_t **,unsigned *,unsigned *);
uint8_t *hi_web_jpeg(const uint8_t *,int,unsigned *,unsigned *);
}
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <ctime>
LV_FONT_DECLARE(font_puhui_full_20);
namespace HiApp { namespace {
constexpr size_t kPixelsBudget=4*1024*1024,kDownloadLimit=512*1024;
std::atomic<size_t> image_bytes{0};
int64_t Clock(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return int64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
struct Frame:RefCounted {
 lv_image_dsc_t dsc{};std::vector<uint8_t>pixels;size_t charged=0;
 ~Frame(){image_bytes.fetch_sub(charged);}
};
Ref<Frame> NewFrame(unsigned w,unsigned h){
 if(!w||!h||w>636||h>480)return {};size_t n=size_t(w)*h*4;
 if(image_bytes.fetch_add(n)>kPixelsBudget-n){image_bytes.fetch_sub(n);return {};}
 Ref<Frame> f(new Frame);f->charged=n;f->pixels.resize(n);
 f->dsc.header.magic=LV_IMAGE_HEADER_MAGIC;f->dsc.header.cf=LV_COLOR_FORMAT_ARGB8888;
 f->dsc.header.w=w;f->dsc.header.h=h;f->dsc.header.stride=w*4;
 f->dsc.data_size=n;f->dsc.data=f->pixels.data();return f;
}
void Size(unsigned w,unsigned h,unsigned &ow,unsigned &oh,unsigned maxw=636,unsigned maxh=480){
 if(w<=maxw&&h<=maxh){ow=w;oh=h;return;}
 if(w*maxh>h*maxw){ow=maxw;oh=std::max(1u,h*maxw/w);}
 else{oh=maxh;ow=std::max(1u,w*maxh/h);}
}
Ref<Frame> Rgba(const uint8_t*p,unsigned w,unsigned h){unsigned ow,oh;Size(w,h,ow,oh);auto f=NewFrame(ow,oh);if(!f)return {};
 for(unsigned y=0;y<oh;y++)for(unsigned x=0;x<ow;x++){const auto*s=p+(size_t(y*h/oh)*w+x*w/ow)*4;auto*d=f->pixels.data()+(size_t(y)*ow+x)*4;d[0]=s[2];d[1]=s[1];d[2]=s[0];d[3]=s[3];}return f;
}
struct Gif {
 std::unique_ptr<GIFIMAGE>decoder;std::string bytes;Ref<Frame>canvas,restore;
 int disposal=0,x=0,y=0,w=0,h=0,loops=0;uint8_t background[4]{};bool touched=false,first=true;int64_t due=0;
 static void Draw(GIFDRAW*d){auto*g=static_cast<Gif*>(d->pUser);if(!g||!g->canvas)return;
  if(!g->touched){g->touched=true;g->disposal=d->ucDisposalMethod;g->x=d->iX;g->y=d->iY;g->w=d->iWidth;g->h=d->iHeight;
   auto*bg=reinterpret_cast<uint8_t*>(g->decoder->pPalette)+d->ucBackground*3;g->background[0]=bg[2];g->background[1]=bg[1];g->background[2]=bg[0];g->background[3]=d->ucHasTransparency&&d->ucTransparent==d->ucBackground?0:255;
   if(g->first){g->first=false;for(size_t at=0;at<g->canvas->pixels.size();at+=4)std::memcpy(g->canvas->pixels.data()+at,g->background,4);}
   if(g->disposal==3){g->restore=NewFrame(g->canvas->dsc.header.w,g->canvas->dsc.header.h);if(g->restore)std::memcpy(g->restore->pixels.data(),g->canvas->pixels.data(),g->canvas->pixels.size());}}
  unsigned cw=g->decoder->iCanvasWidth,ch=g->decoder->iCanvasHeight,ow=g->canvas->dsc.header.w,oh=g->canvas->dsc.header.h;
  int sy=d->iY+d->y;if(sy<0||unsigned(sy)>=ch)return;
  for(unsigned yy=0;yy<oh;yy++){if(yy*ch/oh!=unsigned(sy))continue;
   for(unsigned xx=0;xx<ow;xx++){int sx=int(xx*cw/ow)-d->iX;if(sx<0||sx>=d->iWidth)continue;unsigned c=d->pPixels[sx];if(d->ucHasTransparency&&c==d->ucTransparent)continue;
    auto*p=d->pPalette24+c*3;auto*out=g->canvas->pixels.data()+(size_t(yy)*ow+xx)*4;out[0]=p[2];out[1]=p[1];out[2]=p[0];out[3]=255;}}
 }
 bool Open(std::string data){bytes=std::move(data);decoder.reset(new GIFIMAGE{});HIGIF_begin(decoder.get(),HIGIF_PALETTE_RGB888);
  if(!HIGIF_openRAM(decoder.get(),reinterpret_cast<uint8_t*>(&bytes[0]),bytes.size(),Draw))return false;
  unsigned w=decoder->iCanvasWidth,h=decoder->iCanvasHeight;if(!w||!h||w>1024||h>2048||size_t(w)*h>2*1024*1024)return false;
  unsigned ow,oh;Size(w,h,ow,oh,400,320);canvas=NewFrame(ow,oh);return bool(canvas);
 }
 Ref<Frame> Next(){if(!canvas)return {};unsigned ow=canvas->dsc.header.w,oh=canvas->dsc.header.h;
  if(disposal==3&&restore)std::memcpy(canvas->pixels.data(),restore->pixels.data(),canvas->pixels.size());
  else if(disposal==2){unsigned cw=decoder->iCanvasWidth,ch=decoder->iCanvasHeight;for(unsigned yy=0;yy<oh;yy++)for(unsigned xx=0;xx<ow;xx++){int sx=xx*cw/ow,sy=yy*ch/oh;if(sx>=x&&sx<x+w&&sy>=y&&sy<y+h)std::memcpy(canvas->pixels.data()+(size_t(yy)*ow+xx)*4,background,4);}}
  restore.reset();touched=false;int delay=100;int more=HIGIF_playFrame(decoder.get(),&delay,this);
  if(!touched){if(decoder->iError!=HIGIF_SUCCESS&&decoder->iError!=HIGIF_EMPTY_FRAME)return {};int repeats=HIGIF_getLoopCount(decoder.get());if(repeats<0||(repeats>0&&loops++>=repeats))return {};HIGIF_reset(decoder.get());std::fill(canvas->pixels.begin(),canvas->pixels.end(),0);disposal=0;first=true;more=HIGIF_playFrame(decoder.get(),&delay,this);if(!touched)return {};}
  (void)more;auto f=NewFrame(ow,oh);if(f)std::memcpy(f->pixels.data(),canvas->pixels.data(),canvas->pixels.size());due=Clock()+std::max(80,std::min(delay,10000));return f;
 }
};
struct JpegSource{const std::string*bytes;size_t pos=0;Ref<Frame>frame;std::atomic<bool>*cancel;};
size_t JpegRead(JDEC*j,uint8_t*p,size_t n){auto*s=static_cast<JpegSource*>(j->device);n=std::min(n,s->bytes->size()-s->pos);if(p)std::memcpy(p,s->bytes->data()+s->pos,n);s->pos+=n;return n;}
int JpegDraw(JDEC*j,void*bitmap,JRECT*r){auto*s=static_cast<JpegSource*>(j->device);if(s->cancel->load())return 0;auto f=s->frame;unsigned iw=j->width>>j->scale,ih=j->height>>j->scale,ow=f->dsc.header.w,oh=f->dsc.header.h;
 // The bundled LVGL TJpgDec already emits BGR888 on this target.
 if(!iw||!ih)return 0;
 // Visit only destination pixels covered by this MCU. Scanning the entire
 // output for every JPEG block makes ordinary large photos unnecessarily slow.
 unsigned x0=(unsigned(r->left)*ow+iw-1)/iw,x1=std::min(ow,((unsigned(r->right)+1)*ow+iw-1)/iw);
 unsigned y0=(unsigned(r->top)*oh+ih-1)/ih,y1=std::min(oh,((unsigned(r->bottom)+1)*oh+ih-1)/ih);
 for(unsigned y=y0;y<y1;y++){unsigned sy=y*ih/oh;for(unsigned x=x0;x<x1;x++){unsigned sx=x*iw/ow;auto*p=static_cast<uint8_t*>(bitmap)+((sy-r->top)*(r->right-r->left+1)+sx-r->left)*3;auto*d=f->pixels.data()+(size_t(y)*ow+x)*4;d[0]=p[0];d[1]=p[1];d[2]=p[2];d[3]=255;}}return 1;
}
bool Url(const std::string&u){return u.size()<=2048&&(u.compare(0,7,"http://")==0||u.compare(0,8,"https://")==0)&&u.find_first_of("\r\n\t @")==u.npos;}
std::string Relative(const std::string&b,const std::string&u){if(u.find("://")!=u.npos)return u;auto p=b.find("://");if(p==b.npos)return {};if(u.compare(0,2,"//")==0)return b.substr(0,p+1)+u;auto s=b.find('/',p+3);auto origin=b.substr(0,s);if(!u.empty()&&u[0]=='/')return origin+u;auto cut=b.find_first_of("?#");auto head=b.substr(0,cut);auto last=head.rfind('/');return (last<p+3?origin+"/":head.substr(0,last+1))+u;}
bool DataUrl(const std::string&u,std::string&out){auto comma=u.find(',');if(comma==u.npos||u.size()>96*1024||u.compare(0,11,"data:image/")!=0)return false;bool b64=u.substr(0,comma).find(";base64")!=u.npos;unsigned acc=0,bits=0;
 for(size_t i=comma+1;i<u.size();i++){unsigned char c=u[i];if(b64){if(c=='=')break;if(c=='\r'||c=='\n'||c==' ')continue;const char*alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";auto*p=std::strchr(alphabet,c);if(!c||!p)return false;acc=(acc<<6)|unsigned(p-alphabet);bits+=6;if(bits>=8){bits-=8;out.push_back(char(acc>>bits));}}
  else if(c=='%'&&i+2<u.size()){char hex[3]={u[i+1],u[i+2],0};char*end=nullptr;long n=strtol(hex,&end,16);if(end!=hex+2)return false;out.push_back(char(n));i+=2;}else out.push_back(c);
 }return !out.empty();
}
struct Job:RefCounted{std::string url,page,error;bool network=false,loaded=false;std::atomic<bool>cancel{false},visible{false};std::mutex lock;Ref<Frame>frame;std::unique_ptr<Gif>gif;unsigned sequence=0;};
struct QueueState{std::mutex lock;std::vector<Ref<Job>>jobs;bool worker_active=false;};
QueueState&GetQueue(){static auto*q=new QueueState;return *q;}
void Publish(Job&j,Ref<Frame>f){std::lock_guard<std::mutex>g(j.lock);j.frame=std::move(f);++j.sequence;}
void Fail(Job&j,const char*text){std::lock_guard<std::mutex>g(j.lock);j.error=text;++j.sequence;}
void LoadImage(Job&j){std::string body,url=j.url,type;bool ok=false;
 if(url.compare(0,5,"data:")==0)ok=DataUrl(url,body);
 else if(j.network&&Url(url)){bool permanent=false;for(int attempt=0;attempt<2&&!ok&&!permanent&&!j.cancel;attempt++){
  // Retry an interrupted GET once with fresh socket/TLS state. A startup
  // service or transient radio delay must not leave a permanently blank image.
  body.clear();url=j.url;int64_t deadline=Clock()+6000;for(int redirect=0;redirect<4&&!j.cancel;redirect++){
   if(Clock()>=deadline)break;Http h;h.SetDeadlineMs(deadline-Clock());h.SetHeader("Accept","image/png,image/jpeg,image/gif,image/svg+xml");h.SetHeader("User-Agent","HiOS-HiApp/1.3");
   h.SetHeader("Accept-Encoding","identity");if(Url(j.page))h.SetHeader("Referer",j.page);
   if(!h.Open("GET",url))break;int status=h.GetStatusCode();type=h.GetResponseType();
   if(status>=300&&status<400){url=Relative(url,h.GetResponseLocation());h.Close();if(!Url(url))break;continue;}
   if(status!=200||h.GetBodyLength()>kDownloadLimit){permanent=true;break;}char chunk[2048];int n=0;while(!j.cancel&&(n=h.Read(chunk,sizeof(chunk)))>0){if(body.size()+n>kDownloadLimit){body.clear();permanent=true;break;}body.append(chunk,n);}ok=!j.cancel&&!body.empty()&&n>=0&&h.GetLastError()==0&&(h.GetBodyLength()==0||h.GetBodyLength()==body.size());h.Close();break;
  }}}
 if(j.cancel)return;if(!ok){Fail(j,"图片加载失败，点击可重试");return;}
 Ref<Frame> f;
 if(body.size()>=8&&!memcmp(body.data(),"\x89PNG\r\n\x1a\n",8)){
  auto be=[](const char*p){return (unsigned(uint8_t(p[0]))<<24)|(unsigned(uint8_t(p[1]))<<16)|(unsigned(uint8_t(p[2]))<<8)|uint8_t(p[3]);};
  if(body.size()<24){Fail(j,"PNG数据不完整");return;}unsigned w=be(body.data()+16),h=be(body.data()+20);uint8_t*p=nullptr;
  // LVGL's LodePNG fork returns an owned draw-buffer descriptor, not the
  // upstream library's raw pixel pointer. Keep its allocator paired.
  if(w&&h&&w<=2048&&h<=2048&&size_t(w)*h<=1024*1024)for(int attempt=0;attempt<3&&!j.cancel;attempt++){
   LodePNGState state;lodepng_state_init(&state);state.info_raw.colortype=LCT_RGBA;state.info_raw.bitdepth=8;
   state.decoder.zlibsettings.max_output_size=size_t(w)*h*8+h*8+1024;
#ifdef LODEPNG_COMPILE_ANCILLARY_CHUNKS
   state.decoder.read_text_chunks=0;state.decoder.remember_unknown_chunks=0;
   state.decoder.max_text_size=65536;state.decoder.max_icc_size=65536;
#endif
   unsigned error=lodepng_decode(&p,&w,&h,&state,reinterpret_cast<const uint8_t*>(body.data()),body.size());
   lodepng_state_cleanup(&state);if(!error&&p)f=Rgba(reinterpret_cast<lv_draw_buf_t*>(p)->data,w,h);
   if(p){lv_draw_buf_destroy(reinterpret_cast<lv_draw_buf_t*>(p));p=nullptr;}
   if(f||attempt==2)break;
   // The UI releases offscreen frames on its next tick. Let it run before
   // retrying an allocation/decode, keeping the compressed response in RAM.
   printf("HIAPP_IMAGE_RETRY png_error=%u resident=%u attempt=%d\n",error,(unsigned)image_bytes.load(),attempt+1);
   usleep(200000);
  }
  if(p)lv_draw_buf_destroy(reinterpret_cast<lv_draw_buf_t*>(p));
 }else if(body.size()>=6&&(!memcmp(body.data(),"GIF87a",6)||!memcmp(body.data(),"GIF89a",6))){
  j.gif.reset(new Gif);if(j.gif->Open(std::move(body)))f=j.gif->Next();if(!f)j.gif.reset();
 }else if(body.size()>=2&&uint8_t(body[0])==255&&uint8_t(body[1])==216){
  std::vector<uint8_t>work(16384);JDEC decoder{};JpegSource source;source.bytes=&body;source.cancel=&j.cancel;
  if(jd_prepare(&decoder,JpegRead,work.data(),work.size(),&source)==JDR_OK&&decoder.width&&decoder.height&&decoder.width<=8192&&decoder.height<=8192){unsigned scale=0;while(scale<3&&((decoder.width>>scale)>1272||(decoder.height>>scale)>960)&&(decoder.width>>(scale+1))&&(decoder.height>>(scale+1)))++scale;unsigned ow,oh;Size(decoder.width>>scale,decoder.height>>scale,ow,oh);source.frame=NewFrame(ow,oh);if(source.frame&&jd_decomp(&decoder,JpegDraw,scale)==JDR_OK)f=source.frame;}
  if(!f){unsigned w=0,h=0;uint8_t*p=hi_web_jpeg(reinterpret_cast<const uint8_t*>(body.data()),body.size(),&w,&h);if(p)f=Rgba(p,w,h);std::free(p);}
 }else if(body.size()<=64*1024&&(type.find("svg")!=type.npos||body.find("<svg")!=body.npos)){
  // External resources and SVG scripts are never fetched or executed.
  uint8_t*p=nullptr;unsigned w=0,h=0;if(hi_web_svg(&body[0],&p,&w,&h))f=Rgba(p,w,h);std::free(p);
 }
 if(j.cancel)return;if(f)Publish(j,std::move(f));else Fail(j,"图片格式不支持、数据无效或尺寸超限");
}
void*Run(void*){auto&q=GetQueue();auto&jobs=q.jobs;for(;;){Ref<Job>chosen;int64_t now=Clock();{std::lock_guard<std::mutex>g(q.lock);
  for(size_t i=jobs.size();i>0;i--)if(jobs[i-1]->cancel||(jobs[i-1]->loaded&&!jobs[i-1]->gif))jobs.erase(jobs.begin()+i-1);
  if(jobs.empty()){q.worker_active=false;return nullptr;}
  for(auto&j:jobs)if(j->visible&&!j->loaded){chosen=j;break;}
  if(!chosen)for(auto&j:jobs)if(j->visible&&j->gif&&now>=j->gif->due){chosen=j;break;}
 }if(chosen){if(!chosen->loaded){LoadImage(*chosen);chosen->loaded=true;}else{auto f=chosen->gif->Next();if(f)Publish(*chosen,std::move(f));else chosen->gif.reset();}}usleep(40000);}}
bool Enqueue(const Ref<Job>&j){auto&q=GetQueue();auto&jobs=q.jobs;std::lock_guard<std::mutex>g(q.lock);if(jobs.size()>=24)return false;jobs.push_back(j);if(q.worker_active)return true;
 pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,49152);pthread_attr_setdetachstate(&a,PTHREAD_CREATE_DETACHED);sched_param p{};p.sched_priority=81;pthread_attr_setschedparam(&a,&p);pthread_attr_setinheritsched(&a,PTHREAD_EXPLICIT_SCHED);pthread_t t;int rc=hi_task_create_compute(&t,&a,Run,nullptr);pthread_attr_destroy(&a);if(rc){jobs.pop_back();return false;}q.worker_active=true;return true;
}
struct Widget{lv_obj_t*root,*image,*label;lv_timer_t*timer=nullptr;Ref<Job>job;Ref<Frame>shown;unsigned sequence=0;uint32_t outside=0;bool queued=false;std::string alt;};
void Drop(Widget*w){lv_image_set_src(w->image,nullptr);if(w->shown){lv_image_cache_drop(&w->shown->dsc);lv_image_header_cache_drop(&w->shown->dsc);w->shown.reset();}}
void ResetImage(Widget*w){
 auto previous=w->job;previous->cancel=true;Drop(w);
 Ref<Job>next(new Job);next->url=previous->url;next->page=previous->page;next->network=previous->network;
 w->job=std::move(next);w->sequence=0;w->queued=false;w->outside=0;
 lv_obj_add_flag(w->image,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(w->label,LV_OBJ_FLAG_HIDDEN);
 lv_label_set_text(w->label,w->alt.empty()?"图片加载中…":w->alt.c_str());
 // Preserve the known block height so scrolling does not jump on eviction.
}
void Tick(lv_timer_t*t){auto*w=static_cast<Widget*>(lv_timer_get_user_data(t));lv_area_t a,viewport;lv_obj_get_coords(w->root,&a);lv_obj_get_coords(lv_obj_get_parent(w->root),&viewport);
 bool visible=lv_obj_is_visible(w->root)&&a.y2>=std::max(0,(int)viewport.y1)&&a.y1<=std::min(673,(int)viewport.y2);w->job->visible=visible;
 if(!visible){if(w->queued)ResetImage(w);return;}
 w->outside=0;if(!w->queued){w->queued=true;if(!Enqueue(w->job))Fail(*w->job,"本页图片较多，点击可重试");}
 Ref<Frame>f;std::string error;unsigned seq;{std::lock_guard<std::mutex>g(w->job->lock);seq=w->job->sequence;if(seq==w->sequence)return;f=w->job->frame;error=w->job->error;}
 w->sequence=seq;if(f){Drop(w);w->shown=f;lv_image_set_src(w->image,&f->dsc);lv_obj_set_size(w->image,f->dsc.header.w,f->dsc.header.h);lv_obj_set_height(w->root,f->dsc.header.h+12);lv_obj_center(w->image);lv_obj_remove_flag(w->image,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(w->label,LV_OBJ_FLAG_HIDDEN);}else if(!error.empty())lv_label_set_text(w->label,(w->alt.empty()?error:w->alt+"\n"+error).c_str());
}
} // namespace
lv_obj_t*WebImageCreate(lv_obj_t*parent,const std::string&url,const std::string&page,const std::string&alt,bool network){
 auto*w=new Widget;w->job=Ref<Job>(new Job);w->job->url=url;w->job->page=page;w->job->network=network;w->alt=alt;
 w->root=lv_obj_create(parent);screen_strip_obj_chrome(w->root);lv_obj_set_size(w->root,636,100);lv_obj_remove_flag(w->root,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(w->root,lv_color_hex(0xF2F6FC),0);lv_obj_set_style_radius(w->root,12,0);w->image=lv_image_create(w->root);w->label=lv_label_create(w->root);lv_obj_set_width(w->label,600);lv_obj_set_style_text_font(w->label,&font_puhui_full_20,0);lv_obj_set_style_text_align(w->label,LV_TEXT_ALIGN_CENTER,0);lv_label_set_text(w->label,alt.empty()?"图片加载中…":alt.c_str());lv_obj_center(w->label);
 w->timer=lv_timer_create(Tick,100,w);
 lv_obj_add_event_cb(w->root,[](lv_event_t*e){auto*w=static_cast<Widget*>(lv_event_get_user_data(e));w->job->cancel=true;lv_timer_delete(w->timer);Drop(w);delete w;},LV_EVENT_DELETE,w);
 lv_obj_add_event_cb(w->root,[](lv_event_t*e){auto*w=static_cast<Widget*>(lv_event_get_user_data(e));if(!w->shown)ResetImage(w);},LV_EVENT_CLICKED,w);
 return w->root;
}
} // namespace HiApp
