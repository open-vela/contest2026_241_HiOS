#include "hi_task_cpp.h"
// SPDX-License-Identifier: Apache-2.0
#include "runtime.h"
#include "hiapp.h"
#include "resources.h"
#include "registry.h"
#include "javascript.h"
#include "web_images.h"
#include "video_player.h"
#include "video_source.h"
#include "video_scan.h"
#include "camera_screen/camera_screen.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "app_manager_screen/app_manager_screen.h"
#include "home_screen/home_screen.h"
#include "panel_screen/panel_screen.h"
#include "system_chrome.h"
#include "pwr_key_handler.h"
#include "hi_input_method.h"
#include "screen_util.h"
#include "ota.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include <memory>
#include <mutex>
#include <deque>
#include <atomic>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <strings.h>
#include <initializer_list>
LV_FONT_DECLARE(font_puhui_full_20);
LV_FONT_DECLARE(font_puhui_30_4);
namespace HiApp {
namespace {
int CpuInfo(lua_State*L){lua_createtable(L,0,4);lua_pushinteger(L,hi_task_supported_cpus());lua_setfield(L,-2,"cores");lua_pushinteger(L,hi_task_current_mask());lua_setfield(L,-2,"mask");lua_pushinteger(L,hi_task_current_cpu());lua_setfield(L,-2,"cpu");lua_pushstring(L,"smp_auto");lua_setfield(L,-2,"policy");return 1;}
int64_t Now(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return(int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
struct Item{std::string kind,text,id,value,detail;bool checked=false;};
struct Model{std::string title,address,placeholder,submit,status,app_name,selected,back,swipe_left,swipe_right;std::vector<Item>buttons,items;Ref<PngIcon>icon;Ref<VideoScanner>scanner;bool input=false,background=false,browser=false,notepad=false,ebook=false,media_library=false,video=false,scan=false;};
struct CachedFetch{std::string request,url,body,type;int status=0;int64_t at=0;bool truncated=false;};
struct Session : RefCounted {
 Ref<VideoPlayer>media;
 Ref<VideoScanner>scanner;
 Ref<VideoPlayer> MediaSnapshot(){std::lock_guard<std::mutex>g(lock);return media;}
 Ref<VideoScanner> ScannerSnapshot(){std::lock_guard<std::mutex>g(lock);return scanner;}
 void PublishMedia(Ref<VideoPlayer>p){std::lock_guard<std::mutex>g(lock);media=std::move(p);}
 void PublishScanner(Ref<VideoScanner>p){std::lock_guard<std::mutex>g(lock);scanner=std::move(p);}
 ~Session(){if(media)media->Stop();if(scanner)scanner->Stop();}
 std::vector<CachedFetch>http_cache;Ref<PngIcon>icon;
 std::atomic<bool>stop{false};std::mutex lock;std::deque<std::pair<std::string,std::string>>events;
 std::unique_ptr<JavaScript>js;
 std::unique_ptr<Model>pending,recovery;Package package;std::string id;size_t allocated=0;int instructions=0;int64_t until=0;bool background=false;
 void Show(std::unique_ptr<Model>m){m->scanner=ScannerSnapshot();m->background=background;m->icon=icon;m->app_name=package.name;
  if(m->browser){if(!recovery)recovery.reset(new Model);auto&r=*recovery;r.browser=true;r.input=m->input;r.title=m->title;r.address=m->address;r.placeholder=m->placeholder;r.submit=m->submit;r.buttons=m->buttons;}
  std::lock_guard<std::mutex>g(lock);pending=std::move(m);}
 void Error(const std::string&e){auto m=std::unique_ptr<Model>(recovery?new Model(*recovery):new Model);m->title=package.name.empty()?"应用":package.name;m->status=e;m->items.push_back({"text",e,"",""});Show(std::move(m));}
};
unsigned live_sessions=0;
void*Allocate(void*ud,void*p,size_t old,size_t size){auto*s=(Session*)ud;if(!p)old=0;if(!size){free(p);s->allocated-=std::min(old,s->allocated);return nullptr;}size_t used=s->allocated-std::min(old,s->allocated);if(size>2*1024*1024||used>2*1024*1024-size)return nullptr;void*q=realloc(p,size);if(q)s->allocated=used+size;return q;}
Session*S(lua_State*L){return *(Session**)lua_getextraspace(L);}
void Hook(lua_State*L,lua_Debug*){auto*s=S(L);s->instructions+=10000;if(s->stop||s->instructions>6000000||Now()>s->until)luaL_error(L,"application cancelled or execution budget exceeded");}
std::string Text(lua_State*L,int index,size_t cap=6000){if(!lua_isstring(L,index))return {};size_t n=0;const char*s=lua_tolstring(L,index,&n);n=std::min(n,cap);while(n&&((unsigned char)s[n]&0xc0)==0x80)--n;return std::string(s,n);}
std::string Field(lua_State*L,int index,const char*k,size_t cap=6000){lua_getfield(L,index,k);auto s=Text(L,-1,cap);lua_pop(L,1);return s;}
int Show(lua_State*L){
 if(!lua_istable(L,1)||S(L)->stop)return 0;
 auto m=std::unique_ptr<Model>(new Model);m->scan=Field(L,1,"style",16)=="scan";m->video=Field(L,1,"style",16)=="video";m->selected=Field(L,1,"selected",32);m->browser=Field(L,1,"style",16)=="browser";m->notepad=Field(L,1,"style",16)=="notepad";m->ebook=Field(L,1,"style",16)=="ebook";m->media_library=Field(L,1,"style",16)=="videolib";m->swipe_left=Field(L,1,"swipe_left",80);m->swipe_right=Field(L,1,"swipe_right",80);m->back=Field(L,1,"back",80);m->title=Field(L,1,"title",96);m->status=Field(L,1,"status",240);
 lua_getfield(L,1,"input");if(lua_istable(L,-1)){m->input=true;m->address=Field(L,-1,"value",1024);m->placeholder=Field(L,-1,"placeholder",80);m->submit=Field(L,-1,"action",80);}lua_pop(L,1);
 size_t total=0;for(int which=0;which<2;which++){
  lua_getfield(L,1,which?"items":"buttons");if(lua_istable(L,-1)){
   size_t count=std::min((size_t)lua_rawlen(L,-1),(size_t)(which?120:6));
   for(size_t i=1;i<=count;i++){lua_rawgeti(L,-1,i);if(lua_istable(L,-1)){
    Item item;item.kind=Field(L,-1,"kind",16);item.text=Field(L,-1,"text",which?((m->notepad||m->ebook)?6000:3000):32);item.id=Field(L,-1,"id",80);item.value=Field(L,-1,"value",item.kind=="image"?64*1024:item.kind=="textarea"?6000:2048);item.detail=Field(L,-1,"detail",160);lua_getfield(L,-1,"checked");item.checked=lua_toboolean(L,-1);lua_pop(L,1);total+=item.text.size()+(item.kind=="image"?item.value.size():0);
    if(total<=48*1024)(which?m->items:m->buttons).push_back(std::move(item));
   }lua_pop(L,1);}
  }lua_pop(L,1);
 }S(L)->Show(std::move(m));return 0;
}
bool HttpUrl(const std::string&u){return u.size()<=2048&&(u.compare(0,7,"http://")==0||u.compare(0,8,"https://")==0)&&u.find_first_of("\r\n\t ")==u.npos&&u.find('@')==u.npos;}
std::string Resolve(const std::string&base,const std::string&loc){
 if(loc.find("://")!=loc.npos)return loc;auto scheme=base.find("://");if(scheme==base.npos)return {};
 if(loc.compare(0,2,"//")==0)return base.substr(0,scheme+1)+loc;
 auto slash=base.find('/',scheme+3);std::string origin=slash==base.npos?base:base.substr(0,slash);
 if(!loc.empty()&&loc[0]=='/')return origin+loc;auto cut=base.find_first_of("?#");std::string b=base.substr(0,cut);auto end=b.rfind('/');return (end<scheme+3?origin+"/":b.substr(0,end+1))+loc;
}
int Fetch(lua_State*L){
 auto*s=S(L);std::string url=Text(L,1,2048),body,error,type,encoding;const std::string request=url;int status=0,transport_error=0;bool cached=false,truncated=false;
 const int timeout=lua_isnumber(L,2)?std::max(500,std::min(15000,(int)lua_tointeger(L,2))):10000;
 const size_t cap=lua_isnumber(L,3)?std::max(1024,std::min(192*1024,(int)lua_tointeger(L,3))):192*1024;
 const bool bypass=lua_toboolean(L,4);const size_t inflated=cap<192*1024?cap:256*1024;
 if(s->package.network&&HttpUrl(url)&&!bypass)for(const auto&r:s->http_cache)if(r.request==request&&r.body.size()<=inflated&&Now()-r.at<(cap<=64*1024?300000:30000)){url=r.url;body=r.body;type=r.type;status=r.status;truncated=r.truncated;cached=true;break;}
 if(!s->package.network)error="应用未声明network权限";
 else if(!HttpUrl(url))error="仅支持有效的http或https网址";
 else if(!cached){int64_t deadline=Now()+timeout;for(int redirects=0;redirects<6&&!s->stop;redirects++){
  if(Now()>=deadline){error="网页连接超时";break;}
  Http h;h.SetDeadlineMs((uint32_t)(deadline-Now()));h.SetHeader("Accept-Encoding","gzip, deflate");h.SetHeader("Accept","text/html,text/plain,application/xhtml+xml");h.SetHeader("User-Agent","HiOS-HiApp/1.0");
  if(!h.Open("GET",url)){transport_error=h.GetLastError();error=transport_error==ETIMEDOUT?"网页连接超时，请重试":"网页连接失败（网络错误 "+std::to_string(transport_error)+"）";break;}status=h.GetStatusCode();type=h.GetResponseType();encoding=h.GetResponseEncoding();
  if(status==301||status==302||status==303||status==307||status==308){std::string next=Resolve(url,h.GetResponseLocation());h.Close();if(!HttpUrl(next)){error="重定向地址无效";break;}url=next;if(redirects==5)error="网页重定向次数过多";continue;}
  const bool plain=encoding.empty()||encoding=="identity";
  if(h.GetBodyLength()>cap&&!plain){error="压缩网页超过阅读上限";break;}
  char buf[2048];int n=0;while(!s->stop&&(n=h.Read(buf,sizeof(buf)))>0){if(body.size()+n>cap){if(plain&&cap>=192*1024){body.append(buf,cap-body.size());truncated=true;}else error="网页超过阅读上限";break;}body.append(buf,n);}
  if(n<0&&error.empty()){transport_error=h.GetLastError();error="网页传输中断（网络错误 "+std::to_string(transport_error)+"）";}h.Close();break;
 }}
 if(error.empty()&&!encoding.empty()){std::string plain;if(InflateBody(body,encoding,plain,inflated))body=std::move(plain);else error="网页压缩格式不支持或数据无效";}
 if(s->stop)error="应用已关闭";
 if(error.empty()&&!cached&&status>=200&&status<300){CachedFetch entry;entry.request=request;entry.url=url;entry.type=type;entry.body=body;entry.status=status;entry.at=Now();
  entry.truncated=truncated;for(size_t i=s->http_cache.size();i>0;i--)if(s->http_cache[i-1].request==request)s->http_cache.erase(s->http_cache.begin()+i-1);
  size_t bytes=body.size();for(const auto&r:s->http_cache)bytes+=r.body.size();while(!s->http_cache.empty()&&(bytes>384*1024||s->http_cache.size()>=6)){bytes-=s->http_cache.front().body.size();s->http_cache.erase(s->http_cache.begin());}if(body.size()<=384*1024)s->http_cache.push_back(std::move(entry));
 }
 printf("HIAPP_FETCH status=%d bytes=%u transport=%d truncated=%d error=%d\n",status,(unsigned)body.size(),transport_error,truncated,!error.empty());
 s->until=Now()+4000;lua_createtable(L,0,8);lua_pushboolean(L,cached);lua_setfield(L,-2,"cached");lua_pushboolean(L,truncated);lua_setfield(L,-2,"truncated");
 lua_pushinteger(L,status);lua_setfield(L,-2,"status");lua_pushlstring(L,body.data(),body.size());lua_setfield(L,-2,"body");lua_pushlstring(L,url.data(),url.size());lua_setfield(L,-2,"url");lua_pushlstring(L,type.data(),type.size());lua_setfield(L,-2,"content_type");lua_pushlstring(L,error.data(),error.size());lua_setfield(L,-2,"error");return 1;
}
int Decode(lua_State*L){size_t n=0;const char*p=lua_tolstring(L,1,&n);std::string error,out;if(!p||n>256*1024)error="网页内容超过限制";else out=DecodeWebText(S(L)->package,std::string(p,n),Text(L,2,64),error);S(L)->until=Now()+4000;lua_pushlstring(L,out.data(),out.size());lua_pushlstring(L,error.data(),error.size());return 2;}
int Js(lua_State*L){auto*s=S(L);std::string op=Text(L,1,16);JsResult r;
 if(op=="stop"){s->js.reset();return 0;}
 if(op=="start"){
  std::string html=Text(L,2,256*1024),url=Text(L,3,2048);std::vector<std::string>scripts;size_t bytes=0;
  if(lua_istable(L,4)){for(size_t i=1;i<=std::min((size_t)lua_rawlen(L,4),(size_t)8);i++){lua_rawgeti(L,4,i);auto code=Text(L,-1,64*1024);lua_pop(L,1);if((bytes+=code.size())<=64*1024)scripts.push_back(std::move(code));}}
  auto prelude=s->package.files.find("dom.js");s->js.reset(new JavaScript(&s->stop));if(prelude!=s->package.files.end())r=s->js->Start(prelude->second,html,url,scripts);else r.error="应用缺少DOM脚本";
 }else if(s->js)r=op=="tick"?s->js->Tick():s->js->Event(Text(L,2,32),Text(L,3,2048));
 if(!r.error.empty()&&r.html.empty())s->js.reset();s->until=Now()+4000;
 lua_createtable(L,0,4);lua_pushlstring(L,r.title.data(),r.title.size());lua_setfield(L,-2,"title");lua_pushlstring(L,r.html.data(),r.html.size());lua_setfield(L,-2,"html");lua_pushlstring(L,r.error.data(),r.error.size());lua_setfield(L,-2,"error");lua_pushlstring(L,r.navigate.data(),r.navigate.size());lua_setfield(L,-2,"navigate");return 1;
}
int Clipboard(lua_State*L){auto text=Text(L,1,12000);{HiTask::IoScope ui_owner;lv_lock();HiInputMethod_CopyText(text.c_str());lv_unlock();}lua_pushboolean(L,true);return 1;}
int DateTime(lua_State*L){time_t t=(time_t)lua_tointeger(L,1)+8*3600;tm d{};char out[32]{};if(gmtime_r(&t,&d))strftime(out,sizeof(out),"%Y-%m-%d %H:%M",&d);lua_pushstring(L,out);return 1;}
int WallTime(lua_State*L){lua_pushinteger(L,(lua_Integer)time(nullptr));return 1;}
int TimeMs(lua_State*L){lua_pushinteger(L,(lua_Integer)Now());return 1;}
int Asset(lua_State*L){auto*s=S(L);std::string n=Text(L,1,128);auto it=s->package.files.find(n);if(it==s->package.files.end())lua_pushnil(L);else lua_pushlstring(L,it->second.data(),it->second.size());return 1;}
int ReadStorage(lua_State*L){auto*s=S(L);std::string data;if(!s->stop&&s->package.storage)ReadData(s->id,data);s->until=Now()+4000;lua_pushlstring(L,data.data(),data.size());return 1;}
int WriteStorage(lua_State*L){auto*s=S(L);size_t n=0;const char*p=lua_type(L,1)==LUA_TSTRING?lua_tolstring(L,1,&n):nullptr;if(!p||n>32*1024){lua_pushboolean(L,false);return 1;}std::string data(p,n);bool ok=!s->stop&&s->package.storage&&WriteData(s->id,data);s->until=Now()+4000;lua_pushboolean(L,ok);return 1;}
#include "ebook_api.inc"
int Media(lua_State*L){
 HiTask::IoScope hardware_control;
 auto*s=S(L);auto op=Text(L,1,24);if(s->stop){lua_pushboolean(L,false);lua_pushliteral(L,"应用已关闭");return 2;}
 if(op=="capabilities"){lua_createtable(L,0,4);lua_pushinteger(L,1);lua_setfield(L,-2,"version");lua_pushboolean(L,true);lua_setfield(L,-2,"qr");lua_pushliteral(L,"H264 Baseline + AAC-LC / MP4, FLV, MOV, M4V, TS, plain HLS-TS");lua_setfield(L,-2,"formats");lua_pushinteger(L,4*1024*1024);lua_setfield(L,-2,"decoder_limit");return 1;}
 auto media=s->MediaSnapshot();if(!media){media=Ref<VideoPlayer>(new VideoPlayer(s->package.storage,s->package.network));s->PublishMedia(media);}
 std::string error;bool ok=true;
 if(op=="list"){
  std::vector<std::pair<std::string,bool>>entries;bool more=false;auto path=Text(L,2,512);auto offset=lua_tointeger(L,3);std::string root;
  ok=s->package.storage&&offset>=0&&offset<=4096&&Prepare(false,error);if(ok){root=VideoPlayer::Directory();if(VideoSource::VideoPath(path))path=root+path.substr(13);ok=VideoPlayer::List(path,(unsigned)offset,entries,more,error);}
  lua_createtable(L,0,6);lua_pushlstring(L,path.data(),path.size());lua_setfield(L,-2,"path");lua_pushlstring(L,root.data(),root.size());lua_setfield(L,-2,"root");lua_newtable(L);for(size_t i=0;i<entries.size();i++){lua_createtable(L,0,2);lua_pushlstring(L,entries[i].first.data(),entries[i].first.size());lua_setfield(L,-2,"name");lua_pushboolean(L,entries[i].second);lua_setfield(L,-2,"directory");lua_rawseti(L,-2,i+1);}lua_setfield(L,-2,"entries");lua_pushboolean(L,more);lua_setfield(L,-2,"more");if(!ok&&error.empty())error="应用无存储权限或目录参数无效";lua_pushlstring(L,error.data(),error.size());lua_setfield(L,-2,"error");s->until=Now()+4000;return 1;
 }
 if(op=="status"){
  auto status=media->Status();lua_createtable(L,0,12);
  auto text=[&](const char*key,const std::string&value){lua_pushlstring(L,value.data(),value.size());lua_setfield(L,-2,key);};auto number=[&](const char*key,uint32_t value){lua_pushinteger(L,value);lua_setfield(L,-2,key);};
  text("state",status.state);text("error",status.error);text("source",status.source);text("warning",status.warning);number("audio_samples",status.audio_samples);number("position",status.position);number("duration",status.duration);number("width",status.width);number("height",status.height);number("frames",status.frames);number("dropped",status.dropped);number("decoder_bytes",status.memory);number("volume",status.volume);lua_pushboolean(L,status.live);lua_setfield(L,-2,"live");lua_pushboolean(L,status.seekable);lua_setfield(L,-2,"seekable");return 1;
 }
 if(op=="scan"){if(!s->package.camera||!s->package.network){ok=false;error="扫码播放需要camera和network权限";}else{media->Stop();if(auto scanner=s->ScannerSnapshot())scanner->Stop();s->PublishScanner(Ref<VideoScanner>(new VideoScanner));}}
 else if(op=="cancel_scan"){if(auto scanner=s->ScannerSnapshot())scanner->Stop();}
 else if(op=="open"){auto source=Text(L,2,2048);if(VideoSource::SdPath(source)&&!Prepare(false,error))ok=false;else{if(VideoSource::VideoPath(source))source=VideoPlayer::Directory()+source.substr(13);ok=media->Open(source,error);}}
 else if(op=="stop")media->Stop();else if(op=="pause")media->Pause(lua_toboolean(L,2));else if(op=="volume")media->Volume((int)std::max<lua_Integer>(0,std::min<lua_Integer>(100,lua_tointeger(L,2))));else if(op=="seek")media->Seek((uint32_t)std::max<lua_Integer>(0,std::min<lua_Integer>(INT32_MAX,lua_tointeger(L,2))));else{ok=false;error="未知媒体操作";}
 lua_pushboolean(L,ok);lua_pushlstring(L,error.data(),error.size());s->until=Now()+4000;return 2;
}
void Budget(lua_State*L,Session&s){s.instructions=0;s.until=Now()+4000;lua_sethook(L,Hook,LUA_MASKCOUNT,10000);}
bool Invoke(lua_State*L,Session&s,const char*name,const std::string&a={},const std::string&b={}){
 lua_getglobal(L,"app");if(!lua_istable(L,-1)){lua_pop(L,1);s.Error("安装包未定义app入口");return false;}lua_getfield(L,-1,name);lua_remove(L,-2);
 if(!lua_isfunction(L,-1)){lua_pop(L,1);s.Error(std::string("缺少应用函数：")+name);return false;}
 s.background=a=="__js_tick";Budget(L,s);int n=0;if(!strcmp(name,"event")){lua_pushlstring(L,a.data(),a.size());lua_pushlstring(L,b.data(),b.size());n=2;}
 if(lua_pcall(L,n,0,0)!=LUA_OK){std::string e=Text(L,-1,240);lua_pop(L,1);s.js.reset();if(!s.stop){printf("HIAPP_EVENT_ERROR bytes=%u instructions=%d detail=%s\n",(unsigned)s.allocated,s.instructions,e.c_str());s.Error(e.find("execution budget exceeded")!=e.npos?"网页处理超出限制，可更换网址或刷新重试":"应用运行失败："+e);}
  // A failed page/event must not leave a visible app with a dead event loop.
  // Keep the existing execution/heap limits and let the user navigate again.
  return !strcmp(name,"event")&&!s.stop;
 }return true;
}
void*Worker(void*arg){
 auto s=*(Ref<Session>*)arg;delete(Ref<Session>*)arg;
 std::string error;
#ifdef HI_APP_SMP_PROBE
 const bool probe=s->id=="hios.smp.probe";
 if(!probe)
#endif
 if(!Prepare(false,error)||!Load(InstalledPath(s->id),s->package,error)){s->package=Package{};s->Error(error);__atomic_sub_fetch(&live_sessions,1u,__ATOMIC_ACQ_REL);return nullptr;}
#ifdef HI_APP_SMP_PROBE
 printf("APP_SMP_RUNTIME_READY id=%s cpu=%u mask=%u\n",s->id.c_str(),hi_task_current_cpu(),hi_task_current_mask());
#endif
 auto logo=s->package.files.find(s->package.icon);if(logo!=s->package.files.end()){s->icon=Ref<PngIcon>(new PngIcon);s->icon->bytes=logo->second;s->icon->fingerprint=Crc32(logo->second.data(),logo->second.size());}
 lua_State*L=lua_newstate(Allocate,s.get());if(!L){s->Error("应用运行内存不足");__atomic_sub_fetch(&live_sessions,1u,__ATOMIC_ACQ_REL);return nullptr;}*(Session**)lua_getextraspace(L)=s.get();
 // Libraries are deliberately enumerated: no io, os, debug, package loader,
 // dynamic C modules, arbitrary filesystem paths or shell entry points.
 struct Lib{const char*name;lua_CFunction fn;};const Lib libs[]={{"_G",luaopen_base},{"string",luaopen_string},{"table",luaopen_table},{"math",luaopen_math},{"utf8",luaopen_utf8}};
 for(const auto&lib:libs){luaL_requiref(L,lib.name,lib.fn,1);lua_pop(L,1);}
 for(const char*k:{"dofile","loadfile","load","collectgarbage","print"}){lua_pushnil(L);lua_setglobal(L,k);}
 const luaL_Reg api[]={{"show",Show},{"fetch",Fetch},{"decode",Decode},{"javascript",Js},{"media",Media},{"cpu",CpuInfo},{"time_ms",TimeMs},{"time",WallTime},{"date",DateTime},{"clipboard",Clipboard},{"books",Books},{"asset",Asset},{"read_data",ReadStorage},{"write_data",WriteStorage},{nullptr,nullptr}};
 luaL_newlib(L,api);lua_setglobal(L,"hios");Budget(L,*s);
 const auto&source=s->package.files[s->package.entry];bool ok=luaL_loadbufferx(L,source.data(),source.size(),s->package.entry.c_str(),"t")==LUA_OK;
 if(ok)ok=lua_pcall(L,0,0,0)==LUA_OK;
 if(!ok){s->Error("Lua加载失败："+Text(L,-1,240));lua_pop(L,1);}else ok=Invoke(L,*s,"start");
#ifdef HI_APP_SMP_PROBE
 if(probe){printf("APP_SMP_LUA start_ok=%d cpu=%u mask=%u pending=%d\n",ok,hi_task_current_cpu(),hi_task_current_mask(),s->pending?1:0);s->stop=true;}
#endif
 while(ok&&!s->stop){std::pair<std::string,std::string>event;bool have=false;{std::lock_guard<std::mutex>g(s->lock);if(!s->events.empty()){event=std::move(s->events.front());s->events.pop_front();have=true;}}
  if(have)ok=Invoke(L,*s,"event",event.first,event.second);else if(s->js&&s->js->due&&Now()>=s->js->due){s->js->due=0;ok=Invoke(L,*s,"event","__js_tick","");}else usleep(40000);
 }if(auto media=s->MediaSnapshot())media->Stop();if(auto scanner=s->ScannerSnapshot())scanner->Stop();lua_close(L);__atomic_sub_fetch(&live_sessions,1u,__ATOMIC_ACQ_REL);return nullptr;
}
lv_obj_t*Label(lv_obj_t*p,const std::string&t,int w,bool heading=false){auto*l=lv_label_create(p);lv_label_set_text(l,t.c_str());lv_obj_set_width(l,w);lv_obj_set_style_text_font(l,heading?&font_puhui_30_4:&font_puhui_full_20,0);lv_obj_set_style_text_color(l,lv_color_hex(0x25334A),0);return l;}
#include "reader_swipe.inc"
struct BoundInput{lv_obj_t*obj;std::string id,last;};
struct View{lv_obj_t*root,*content,*input=nullptr,*body=nullptr,*status=nullptr,*back=nullptr;lv_timer_t*timer;Ref<Session>session;Ref<PngIcon>icon;std::vector<Item>items;std::vector<BoundInput>fields;std::vector<lv_obj_t*>primary;size_t cursor=0;int restore_scroll=0;bool notified=false,browser=false,notepad=false,ebook=false,media_library=false;std::string address,selected,back_action,swipe_left,swipe_right;ReaderSwipe reader_swipe;
 lv_obj_t*video_image=nullptr,*video_progress=nullptr,*video_time=nullptr,*video_note=nullptr,*video_pause=nullptr,*video_seek_back=nullptr,*video_seek_forward=nullptr,*video_error_box=nullptr,*video_error_text=nullptr;Ref<VideoFrame>video_frame;int64_t video_updated=0;std::string video_caption;
 Ref<VideoScanner>scanner;lv_obj_t*scan_canvas=nullptr,*scan_hint=nullptr;bool scan_started=false,scan_camera=false;int64_t scan_since=0;std::string scan_url,scan_error;
};

#ifdef HI_VIDEO_TARGET_TEST
static View*target_test_view=nullptr;
#endif
struct Action{View*v;std::string id,value;bool input=false;};
// Snapshot all edited fields before the action, on the UI thread. Lua sees
// the field events first; changing focus never rebuilds an editor mid-tap.
bool CommitFields(View*v,const std::string&id,const std::string&value){
 std::vector<std::pair<std::string,std::string>> events;
 for(auto&f:v->fields){if(!HiInputMethod_Flush(f.obj))return false;std::string text=lv_textarea_get_text(f.obj);if(text!=f.last)events.emplace_back(f.id,std::move(text));}
 auto s=v->session;if(s->stop)return false;std::lock_guard<std::mutex>g(s->lock);
 if(s->events.size()+events.size()+1>128)return false;
 for(auto&e:events){for(auto&f:v->fields)if(f.id==e.first)f.last=e.second;s->events.push_back(std::move(e));}
 s->events.push_back(std::make_pair(id,value));return true;
}
void Click(lv_event_t*e){auto*a=(Action*)lv_event_get_user_data(e);auto*v=a->v;std::string value=a->value;
 if(a->input&&v->input){if(!HiInputMethod_Flush(v->input))return;value=lv_textarea_get_text(v->input);}
 if(CommitFields(v,a->id,value))HiInputMethod_Hide();
}

lv_obj_t*Button(lv_obj_t*p,const std::string&t,Action*a,int w,int h){auto*b=lv_button_create(p);lv_obj_set_size(b,w,h);lv_obj_set_style_pad_hor(b,8,0);lv_obj_set_style_border_width(b,0,0);lv_obj_set_style_bg_color(b,lv_color_hex(0xE8F0FB),0);lv_obj_set_style_radius(b,12,0);lv_obj_set_style_shadow_width(b,0,0);auto*l=Label(b,t,w-16);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_obj_center(l);lv_obj_add_event_cb(b,Click,LV_EVENT_CLICKED,a);lv_obj_add_event_cb(b,[](lv_event_t*e){delete(Action*)lv_event_get_user_data(e);},LV_EVENT_DELETE,a);screen_swipe_back_ignore(b);return b;}
void ClearVideo(View*v){
 if(v->video_image)lv_image_set_src(v->video_image,nullptr);
 if(v->video_frame){lv_image_cache_drop(&v->video_frame->image);lv_image_header_cache_drop(&v->video_frame->image);v->video_frame.reset();}
 v->video_image=v->video_progress=v->video_time=v->video_note=v->video_pause=v->video_seek_back=v->video_seek_forward=nullptr;
 v->video_error_box=v->video_error_text=nullptr;
}
void VideoLabel(lv_obj_t*o,const char*text){if(strcmp(lv_label_get_text(o),text))lv_label_set_text(o,text);}
void UpdateVideo(View*v){
 if(!v->video_image||!v->session->MediaSnapshot())return;auto player=v->session->MediaSnapshot();auto frame=player->PollFrame();
 if(frame){ // Keep the old pixels alive until the replacement is bound.
  auto previous=v->video_frame;const bool changed=!previous||previous->image.header.w!=frame->image.header.w||previous->image.header.h!=frame->image.header.h;
  lv_image_cache_drop(&frame->image);v->video_frame=frame;lv_image_set_src(v->video_image,&frame->image);
  if(previous){lv_image_cache_drop(&previous->image);lv_image_header_cache_drop(&previous->image);}
  if(changed){auto w=frame->image.header.w,h=frame->image.header.h;unsigned scale=std::min<unsigned>(256u,std::min(680u*256/w,350u*256/h));lv_image_set_scale(v->video_image,scale);lv_obj_center(v->video_image);}
 }
 if(Now()-v->video_updated<200)return;v->video_updated=Now();auto state=player->Status();
 const char*status=state.state=="playing"?"播放中":state.state=="paused"?"已暂停":state.state=="opening"?"正在连接与解析":state.state=="ended"?"播放结束":state.state=="error"?"播放失败":state.state=="stopping"?"正在释放资源":"已停止";
 char text[160];unsigned elapsed=state.position/1000,total=state.duration/1000;
 if(state.live)snprintf(text,sizeof(text),"直播 · %s · 音量 %d%%",status,state.volume);
 else snprintf(text,sizeof(text),"%02u:%02u / %02u:%02u · %s · 音量 %d%%",elapsed/60,elapsed%60,total/60,total%60,status,state.volume);
 VideoLabel(v->video_time,text);
 const bool failed=state.state=="error";
 if(failed){lv_obj_remove_flag(v->video_error_box,LV_OBJ_FLAG_HIDDEN);VideoLabel(v->video_note,"上方可上下滑动查看完整原因");
  const auto&detail=state.error;if(detail!=lv_label_get_text(v->video_error_text)){VideoLabel(v->video_error_text,detail.empty()?"暂时无法播放，请返回列表后重试。":detail.c_str());lv_obj_scroll_to_y(v->video_error_box,0,LV_ANIM_OFF);}}
 else{lv_obj_add_flag(v->video_error_box,LV_OBJ_FLAG_HIDDEN);VideoLabel(v->video_note,state.warning.empty()?v->video_caption.c_str():state.warning.c_str());}
 if(!lv_obj_has_state(v->video_progress,LV_STATE_PRESSED)){int value=state.duration?(int)std::min<uint64_t>(1000,(uint64_t)state.position*1000/state.duration):0;if(lv_slider_get_value(v->video_progress)!=value)lv_slider_set_value(v->video_progress,value,LV_ANIM_OFF);}
 for(auto*control:{v->video_progress,v->video_seek_back,v->video_seek_forward}){if(!state.seekable||state.live||!state.duration)lv_obj_add_state(control,LV_STATE_DISABLED);else lv_obj_remove_state(control,LV_STATE_DISABLED);}
 VideoLabel(lv_obj_get_child(v->video_pause,0),failed?"重试":state.state=="paused"||state.state=="ended"||state.state=="idle"?"播放":"暂停");
}
void RenderVideo(View*v,Model&m){
 lv_obj_set_style_bg_color(v->root,lv_color_hex(0x171A20),0);lv_obj_set_style_bg_color(v->back,lv_color_hex(0x242830),0);lv_obj_set_style_text_color(lv_obj_get_child(v->back,0),lv_color_hex(0xE4E7ED),0);
 v->back_action=m.back;v->video_caption=m.status;Record record;record.png=m.icon;auto*icon=RecordIcon(v->content,record,40);lv_obj_set_pos(icon,108,7);auto*name=Label(v->content,m.title,430,true);lv_obj_set_pos(name,160,9);lv_obj_set_style_text_color(name,lv_color_hex(0xF2F3F5),0);
 auto*stage=lv_obj_create(v->content);screen_strip_obj_chrome(stage);lv_obj_set_pos(stage,12,60);lv_obj_set_size(stage,696,360);lv_obj_set_style_radius(stage,16,0);lv_obj_set_style_bg_color(stage,lv_color_hex(0x090B0E),0);lv_obj_set_style_bg_opa(stage,LV_OPA_COVER,0);lv_obj_remove_flag(stage,LV_OBJ_FLAG_SCROLLABLE);screen_swipe_back_ignore(stage);
 v->video_image=lv_image_create(stage);lv_obj_center(v->video_image);
 v->video_error_box=lv_obj_create(stage);screen_strip_obj_chrome(v->video_error_box);lv_obj_set_pos(v->video_error_box,0,0);lv_obj_set_size(v->video_error_box,696,360);lv_obj_set_style_bg_color(v->video_error_box,lv_color_hex(0x252A33),0);lv_obj_set_style_bg_opa(v->video_error_box,LV_OPA_COVER,0);lv_obj_set_style_pad_all(v->video_error_box,20,0);lv_obj_set_style_pad_row(v->video_error_box,14,0);lv_obj_set_flex_flow(v->video_error_box,LV_FLEX_FLOW_COLUMN);lv_obj_add_flag(v->video_error_box,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(v->video_error_box,LV_DIR_VER);screen_swipe_back_ignore(v->video_error_box);
 auto*error_title=Label(v->video_error_box,"播放失败",640,true);lv_obj_set_style_text_color(error_title,lv_color_hex(0xF1A25D),0);v->video_error_text=Label(v->video_error_box,"",640);lv_obj_set_style_text_color(v->video_error_text,lv_color_hex(0xE4E7ED),0);lv_label_set_long_mode(v->video_error_text,LV_LABEL_LONG_WRAP);lv_obj_set_height(v->video_error_text,LV_SIZE_CONTENT);lv_obj_set_style_text_line_space(v->video_error_text,8,0);lv_obj_add_flag(v->video_error_box,LV_OBJ_FLAG_HIDDEN);
 v->video_progress=lv_slider_create(v->content);lv_obj_set_pos(v->video_progress,25,437);lv_obj_set_size(v->video_progress,670,12);lv_slider_set_range(v->video_progress,0,1000);lv_obj_set_style_bg_color(v->video_progress,lv_color_hex(0xF08B32),LV_PART_INDICATOR);lv_obj_set_style_bg_color(v->video_progress,lv_color_hex(0xF08B32),LV_PART_KNOB);lv_obj_set_style_bg_color(v->video_progress,lv_color_hex(0x414956),LV_PART_MAIN);screen_swipe_back_ignore(v->video_progress);
 lv_obj_add_event_cb(v->video_progress,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);CommitFields(v,"seek",std::to_string(lv_slider_get_value(lv_event_get_target_obj(e))));},LV_EVENT_RELEASED,v);
 v->video_time=Label(v->content,"正在准备播放…",680);lv_obj_set_pos(v->video_time,20,463);lv_obj_set_style_text_color(v->video_time,lv_color_hex(0xBCC5D1),0);
 const char*ids[]={"toggle","rewind","forward","stop","library"};const char*names[]={"暂停","退10秒","进10秒","停止","视频列表"};
 for(int i=0;i<5;i++){auto*b=Button(v->content,names[i],new Action{v,ids[i],"",false},128,48);lv_obj_set_pos(b,20+i*138,500);lv_obj_set_style_bg_color(b,lv_color_hex(i?0x292D36:0xF08B32),0);lv_obj_set_style_text_color(lv_obj_get_child(b,0),lv_color_hex(i?0xE4E7ED:0x171A20),0);if(!i)v->video_pause=b;if(i==1)v->video_seek_back=b;if(i==2)v->video_seek_forward=b;}
 auto*down=Button(v->content,"音量 -",new Action{v,"quieter","",false},112,42);lv_obj_set_pos(down,20,562);auto*up=Button(v->content,"音量 +",new Action{v,"louder","",false},112,42);lv_obj_set_pos(up,142,562);for(auto*b:{down,up}){lv_obj_set_style_bg_color(b,lv_color_hex(0x292D36),0);lv_obj_set_style_text_color(lv_obj_get_child(b,0),lv_color_hex(0xE4E7ED),0);}
 v->video_note=Label(v->content,m.status,420);lv_obj_set_pos(v->video_note,274,558);lv_obj_set_height(v->video_note,48);lv_label_set_long_mode(v->video_note,LV_LABEL_LONG_WRAP);lv_obj_set_style_text_color(v->video_note,lv_color_hex(0x9BA4B2),0);v->video_updated=0;UpdateVideo(v);
}
void ClearScan(View*v){
 if(v->scan_camera){CameraScreen::SetPreviewObserver(nullptr);CameraScreen::StopExternalPreview();v->scan_camera=false;}
 if(v->scanner)v->scanner->Stop();v->scanner.reset();v->scan_canvas=v->scan_hint=nullptr;v->scan_url.clear();v->scan_error.clear();v->scan_started=false;
}
void UpdateScan(View*v){
 if(!v->scan_canvas||!v->scanner)return;
 auto result=v->scanner->Result();
 if(!v->scan_url.empty()||!v->scan_error.empty()){
  if(v->scan_camera){CameraScreen::SetPreviewObserver(nullptr);CameraScreen::StopExternalPreview();v->scan_camera=false;}
  v->scanner->Stop();
  if((!v->scan_started||result.finished)&&CameraScreen::PreviewStopped()){
   if(CommitFields(v,v->scan_error.empty()?"__scan_result":"__scan_error",v->selected+"\t"+(v->scan_error.empty()?v->scan_url:v->scan_error))){v->scan_canvas=nullptr;v->scanner.reset();}
  }return;
 }
 if(!v->scan_started){
  if((v->session->MediaSnapshot()&&v->session->MediaSnapshot()->Running())||!CameraScreen::PreviewStopped()){if(Now()-v->scan_since>12000)v->scan_error="上个媒体或摄像头任务尚未结束，请稍后重试";return;}
  CameraScreen::PreviewBuffer preview{};
  if(!CameraScreen::PreparePreviewBuffer(&preview)||!v->scanner->Start()){v->scan_error="无法分配扫码资源，请重试";return;}
  v->scan_started=true;lv_canvas_set_buffer(v->scan_canvas,preview.data,preview.width,preview.height,LV_COLOR_FORMAT_RGB888);lv_image_set_scale(v->scan_canvas,213);lv_obj_center(v->scan_canvas);
  CameraScreen::SetPreviewObserver(VideoScanner::Observe);v->scan_camera=true;
  if(CameraScreen::StartExternalPreview(v->scan_canvas)!=ESP_OK){v->scan_error="摄像头启动失败，请重试";return;}
  v->scan_since=Now();lv_label_set_text(v->scan_hint,"将手机上的视频链接二维码放到画面中央");return;
 }
 if(!result.url.empty()){v->scan_url=result.url;lv_label_set_text(v->scan_hint,"识别成功，关闭摄像头后开始播放…");}
 else if(result.finished){v->scan_error=result.hint.empty()?"扫码已结束，请重试":result.hint;}
 else if(!result.has_frame&&Now()-v->scan_since>12000)v->scan_error="摄像头未输出画面，请重试";
 else if(Now()-v->scan_since>60000)v->scan_error="扫码超过60秒，已停止摄像头，请重试";
 else if(!result.hint.empty())lv_label_set_text(v->scan_hint,result.hint.c_str());
}
void RenderScan(View*v,Model&m){
 lv_obj_set_style_bg_color(v->root,lv_color_hex(0x171A20),0);lv_obj_set_style_bg_color(v->back,lv_color_hex(0x242830),0);lv_obj_set_style_text_color(lv_obj_get_child(v->back,0),lv_color_hex(0xE4E7ED),0);
 v->back_action="cancel_scan";v->scanner=m.scanner;
 Record record;record.png=m.icon;auto*logo=RecordIcon(v->content,record,40);lv_obj_set_pos(logo,108,7);auto*title=Label(v->content,"扫码播放",430,true);lv_obj_set_pos(title,160,9);lv_obj_set_style_text_color(title,lv_color_hex(0xF2F3F5),0);
 auto*stage=lv_obj_create(v->content);screen_strip_obj_chrome(stage);lv_obj_set_pos(stage,12,60);lv_obj_set_size(stage,696,510);lv_obj_set_style_bg_color(stage,lv_color_hex(0x081321),0);lv_obj_set_style_bg_opa(stage,LV_OPA_COVER,0);lv_obj_remove_flag(stage,LV_OBJ_FLAG_SCROLLABLE);
 v->scan_hint=Label(v->content,"等待媒体释放，准备打开摄像头…",680);lv_obj_set_pos(v->scan_hint,20,588);lv_obj_set_height(v->scan_hint,46);lv_obj_set_style_text_color(v->scan_hint,lv_color_hex(0xBCC5D1),0);lv_label_set_long_mode(v->scan_hint,LV_LABEL_LONG_DOT);
 if(!v->session->package.camera||!v->scanner){lv_label_set_text(v->scan_hint,"应用未声明摄像头权限，或扫码会话无效");return;}
 v->scan_canvas=lv_canvas_create(stage);v->scan_since=Now();UpdateScan(v);
}
#include "ebook_ui.inc"
#include "video_ui.inc"
void RenderMore(View*v){
 if(v->ebook||v->media_library||!v->body)return;const size_t end=std::min(v->cursor+8,v->items.size());
 const int text_width=v->browser?652:636;
 for(;v->cursor<end;v->cursor++){const auto&i=v->items[v->cursor];
  if(i.kind=="hero"){
   auto*card=lv_obj_create(v->body);screen_strip_obj_chrome(card);lv_obj_set_size(card,636,170);lv_obj_set_style_bg_color(card,lv_color_hex(0xEAF3FF),0);lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);lv_obj_set_style_radius(card,20,0);lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);
   Record r;r.png=v->icon;auto*logo=RecordIcon(card,r,104);lv_obj_set_pos(logo,20,28);
   auto*l=Label(card,i.text,455);lv_obj_set_pos(l,144,40);lv_obj_set_style_text_color(l,lv_color_hex(0x1477DD),0);v->primary.push_back(l);auto*d=Label(card,i.value,455);lv_obj_set_pos(d,144,83);lv_obj_set_style_text_color(d,lv_color_hex(0x657A96),0);
  }else if(i.kind=="image"){
   auto*image=WebImageCreate(v->body,i.value,v->address,i.text,v->session->package.network);v->primary.push_back(image);
  }else if(i.kind=="note"){
   auto*b=Button(v->body,"",new Action{v,i.id,i.value,false},text_width,94);auto*empty=lv_obj_get_child(b,0);lv_obj_delete(empty);
   lv_obj_set_style_bg_color(b,lv_color_hex(i.checked?0xE5D8BB:0xFFFCF3),0);lv_obj_set_style_radius(b,3,0);lv_obj_set_style_border_width(b,1,0);lv_obj_set_style_border_color(b,lv_color_hex(0xC8BBA1),0);lv_obj_set_style_shadow_width(b,4,0);
   auto*meta=Label(b,i.detail,text_width-48);lv_obj_set_pos(meta,20,6);lv_obj_set_style_text_color(meta,lv_color_hex(0xA3937B),0);lv_label_set_long_mode(meta,LV_LABEL_LONG_DOT);
   auto*title=Label(b,i.text,text_width-48);lv_obj_set_pos(title,20,37);lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);lv_obj_set_style_text_color(title,lv_color_hex(0x715846),0);v->primary.push_back(title);
   auto*mark=Label(b,i.checked?"+":"-",16);lv_obj_set_pos(mark,0,35);lv_obj_set_style_text_color(mark,lv_color_hex(0x9A8D75),0);
  }else if(i.kind=="input"||i.kind=="textarea"){
   const bool multi=i.kind=="textarea";if(!i.text.empty())Label(v->body,i.text,text_width);
   auto*field=lv_textarea_create(v->body);lv_obj_set_size(field,text_width,multi?300:v->browser?50:56);lv_textarea_set_one_line(field,!multi);lv_textarea_set_max_length(field,multi?2000:v->notepad?50:512);lv_textarea_set_text_selection(field,true);lv_obj_set_style_text_font(field,&font_puhui_full_20,0);lv_obj_set_style_text_line_space(field,multi?12:3,0);lv_obj_set_style_radius(field,v->notepad?3:12,0);lv_obj_set_style_bg_color(field,lv_color_hex(v->notepad?0xFFFCF3:0xFFFFFF),0);lv_obj_set_style_border_color(field,lv_color_hex(v->notepad?0xD1C3A8:0xDCE5F0),0);lv_textarea_set_text(field,i.value.c_str());HiInputMethod_Bind(field,true);v->primary.push_back(field);v->fields.push_back({field,i.id,i.value});
  }else if(i.kind=="link"||i.kind=="button"){
   auto*b=Button(v->body,i.text,new Action{v,i.id,i.value,false},text_width,LV_SIZE_CONTENT);lv_obj_set_style_pad_ver(b,v->browser?8:15,0);lv_obj_set_style_radius(b,14,0);auto*l=lv_obj_get_child(b,0);v->primary.push_back(l);lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);lv_obj_set_height(l,LV_SIZE_CONTENT);
   if(v->browser){lv_obj_set_style_bg_color(b,lv_color_hex(i.kind=="link"?0xF0F6FF:0xE8EFF8),0);lv_obj_set_style_text_color(l,lv_color_hex(0x1675D5),0);}
  }else{auto*l=Label(v->body,i.text,text_width,false);v->primary.push_back(l);lv_obj_set_style_text_line_space(l,v->browser?4:7,0);if(v->notepad){lv_obj_set_style_text_color(l,lv_color_hex(0x715846),0);lv_obj_set_style_text_line_space(l,12,0);}if(i.kind=="heading"){lv_obj_set_style_text_color(l,lv_color_hex(v->notepad?0x715846:0x155C9E),0);lv_obj_set_style_pad_top(l,6,0);}}
 }
 if(v->cursor==v->items.size()&&v->restore_scroll>0){lv_obj_update_layout(v->body);lv_obj_scroll_to_y(v->body,v->restore_scroll,LV_ANIM_OFF);v->restore_scroll=0;}
}
void Render(View*v,Model&m){
 const bool same_page=v->browser&&m.browser&&v->address==m.address;v->back_action=m.back;
 bool reuse=same_page&&v->selected==m.selected&&v->cursor==v->items.size()&&v->primary.size()==m.items.size()&&v->items.size()==m.items.size();
 for(size_t i=0;reuse&&i<m.items.size();i++){const auto&a=v->items[i];const auto&b=m.items[i];if(a.kind!=b.kind||a.id!=b.id||(a.kind!="input"&&a.value!=b.value))reuse=false;}
 if(reuse){for(size_t i=0;i<m.items.size();i++){const auto&a=v->items[i];const auto&b=m.items[i];if(b.kind=="input"){if(a.value!=b.value)lv_textarea_set_text(v->primary[i],b.value.c_str());}else if(b.kind!="image"&&a.text!=b.text)lv_label_set_text(v->primary[i],b.text.c_str());}if(v->status)lv_label_set_text(v->status,m.status.c_str());v->items=std::move(m.items);return;}
const int old_scroll=same_page&&v->body?lv_obj_get_scroll_y(v->body):0;
 HiInputMethod_Hide();ClearVideo(v);ClearScan(v);v->input=nullptr;v->body=nullptr;lv_obj_clean(v->content);v->browser=m.browser;v->icon=m.icon;v->address=m.address;v->selected=m.selected;v->notepad=m.notepad;v->ebook=m.ebook;v->media_library=m.media_library;v->swipe_left=m.swipe_left;v->swipe_right=m.swipe_right;v->reader_swipe=ReaderSwipe{};v->primary.clear();v->fields.clear();
 if(m.ebook){RenderEbook(v,m);return;}
 if(m.media_library){RenderVideoLibrary(v,m);return;}
 if(m.scan){v->items.clear();v->cursor=0;RenderScan(v,m);return;}
 if(m.video){v->items.clear();v->cursor=0;RenderVideo(v,m);return;}
 lv_obj_set_style_bg_color(v->root,lv_color_hex(m.notepad?0xE7D4A8:0xF5F8FC),0);
 if(v->back){lv_obj_set_size(v->back,m.browser?76:86,m.browser?40:48);auto*l=lv_obj_get_child(v->back,0);lv_obj_set_width(l,m.browser?60:70);lv_obj_center(l);}
 if(m.browser){Record r;r.png=m.icon;auto*logo=RecordIcon(v->content,r,38);lv_obj_set_pos(logo,102,8);auto*l=Label(v->content,m.title,350,true);lv_obj_set_pos(l,154,9);auto*tag=Label(v->content,"图文阅读",140);lv_obj_set_pos(tag,550,17);lv_obj_set_style_text_color(tag,lv_color_hex(0x8293AA),0);}
 else{if(m.notepad){auto*bar=lv_obj_create(v->content);screen_strip_obj_chrome(bar);lv_obj_set_size(bar,720,54);lv_obj_set_style_bg_color(bar,lv_color_hex(0x796956),0);lv_obj_set_style_bg_opa(bar,LV_OPA_COVER,0);lv_obj_remove_flag(bar,LV_OBJ_FLAG_CLICKABLE);}
 auto*title=Label(v->content,m.title,570,true);lv_obj_set_pos(title,104,8);if(m.notepad){lv_obj_set_style_text_color(title,lv_color_hex(0xFFF9ED),0);lv_obj_set_style_text_font(title,&font_puhui_full_20,0);}}
 if(m.input){v->input=lv_textarea_create(v->content);lv_obj_set_pos(v->input,18,m.browser?56:65);lv_obj_set_size(v->input,m.browser?556:570,m.browser?50:58);lv_textarea_set_one_line(v->input,true);lv_textarea_set_max_length(v->input,1024);lv_obj_set_style_text_font(v->input,&font_puhui_full_20,0);lv_obj_set_style_radius(v->input,14,0);lv_obj_set_style_border_width(v->input,1,0);lv_obj_set_style_border_color(v->input,lv_color_hex(0xDCE5F0),0);lv_obj_set_style_border_color(v->input,lv_color_hex(0x3089EA),LV_STATE_FOCUSED);lv_textarea_set_placeholder_text(v->input,m.placeholder.c_str());lv_textarea_set_text(v->input,m.address.c_str());HiInputMethod_Bind(v->input,!m.browser);
  auto*b=Button(v->content,"前往",new Action{v,m.submit,"",true},m.browser?112:90,m.browser?50:58);lv_obj_set_pos(b,m.browser?586:602,m.browser?56:65);if(m.browser){lv_obj_set_style_bg_color(b,lv_color_hex(0x2384E8),0);lv_obj_set_style_text_color(lv_obj_get_child(b,0),lv_color_white(),0);}
  auto*a=new Action{v,m.submit,"",true};lv_obj_add_event_cb(v->input,Click,LV_EVENT_READY,a);lv_obj_add_event_cb(v->input,[](lv_event_t*e){delete(Action*)lv_event_get_user_data(e);},LV_EVENT_DELETE,a);
 }
 int y=m.browser?116:m.input?136:66;int count=m.buttons.size();int w=count?(680-(count-1)*8)/count:100;
 for(int i=0;i<count;i++){auto&b=m.buttons[i];auto*o=Button(v->content,b.text,new Action{v,b.id,b.value,false},w,m.browser?42:48);lv_obj_set_pos(o,20+i*(w+8),y);if(m.notepad){lv_obj_set_style_bg_color(o,lv_color_hex(0x88745F),0);lv_obj_set_style_text_color(lv_obj_get_child(o,0),lv_color_hex(0xFFF9ED),0);lv_obj_set_style_radius(o,6,0);}if(m.browser){bool selected=b.id==m.selected;lv_obj_set_style_bg_color(o,lv_color_hex(selected?0xDDEEFF:0xFFFFFF),0);lv_obj_set_style_text_color(lv_obj_get_child(o,0),lv_color_hex(selected?0x1374D4:0x52677E),0);}}
 auto*status=Label(v->content,m.status,680);v->status=status;lv_obj_set_pos(status,20,y+(m.browser?50:58));lv_label_set_long_mode(status,LV_LABEL_LONG_DOT);lv_obj_set_height(status,m.browser?26:30);lv_obj_set_style_text_color(status,lv_color_hex(0x71849B),0);
 auto*body=lv_obj_create(v->content);v->body=body;screen_strip_obj_chrome(body);int body_y=y+(m.browser?86:96);lv_obj_set_pos(body,m.browser?12:16,body_y);lv_obj_set_size(body,m.browser?696:688,(m.browser?632:628)-body_y);lv_obj_set_style_bg_color(body,lv_color_hex(m.notepad?0xEBD9B4:0xFFFFFF),0);if(m.notepad){lv_obj_set_style_bg_grad_color(body,lv_color_hex(0xDFCAA0),0);lv_obj_set_style_bg_grad_dir(body,LV_GRAD_DIR_HOR,0);}lv_obj_set_style_bg_opa(body,LV_OPA_COVER,0);lv_obj_set_style_radius(body,18,0);lv_obj_set_style_pad_all(body,m.browser?16:20,0);lv_obj_set_style_pad_row(body,m.browser?10:15,0);lv_obj_set_flex_flow(body,LV_FLEX_FLOW_COLUMN);if(m.browser)lv_obj_set_flex_align(body,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_START);lv_obj_add_flag(body,LV_OBJ_FLAG_SCROLLABLE);screen_swipe_back_ignore(body);
 if(m.notepad){
  lv_obj_set_style_bg_color(v->back,lv_color_hex(0x796956),0);lv_obj_set_style_text_color(lv_obj_get_child(v->back,0),lv_color_white(),0);
  lv_obj_set_style_radius(body,3,0);lv_obj_set_style_pad_all(body,10,0);lv_obj_set_style_pad_row(body,10,0);
 }else{lv_obj_set_style_bg_color(v->back,lv_color_hex(0xE8F0FB),0);lv_obj_set_style_text_color(lv_obj_get_child(v->back,0),lv_color_hex(0x25334A),0);}
 v->items=std::move(m.items);v->cursor=0;v->restore_scroll=old_scroll;RenderMore(v);
 if(!v->swipe_left.empty()&&!v->swipe_right.empty()){
  lv_obj_set_scroll_dir(body,LV_DIR_VER);screen_swipe_back_ignore(body);
  v->reader_swipe.context=v;v->reader_swipe.turn=[](void*p,bool next){auto*v=(View*)p;return CommitFields(v,next?v->swipe_left:v->swipe_right,"");};
  BindReaderSwipe(body,&v->reader_swipe);
 }
}

void Back(lv_event_t*e){
 auto*v=e?(View*)lv_event_get_user_data(e):(View*)lv_obj_get_user_data(lv_screen_active());if(!v)return;
 if(!v->back_action.empty()){if(CommitFields(v,v->back_action,""))HiInputMethod_Hide();return;}
 // Exit immediately, independently of a pending network/Lua event.
 HiInputMethod_Hide();v->session->stop=true;if(v->session->MediaSnapshot())v->session->MediaSnapshot()->Stop();HomeScreen::SwitchToHome();
}
void Navigate(const std::string&id){auto*old=lv_screen_active();auto*next=CreateRuntime(id);if(!next)return;screen_load_safe(next);if(old&&old!=next&&!HomeScreen::IsHomeObject(old)&&!PanelScreen::IsPanelObject(old))lv_obj_delete_async(old);}
}
bool ValidateLua(const Package&p,std::string&error){auto it=p.files.find(p.entry);if(it==p.files.end()){error="缺少Lua入口";return false;}Session s;lua_State*L=lua_newstate(Allocate,&s);if(!L){error="Lua校验内存不足";return false;}const auto&text=it->second;int rc=luaL_loadbufferx(L,text.data(),text.size(),p.entry.c_str(),"t");if(rc!=LUA_OK)error="Lua语法错误："+Text(L,-1,160);lua_close(L);return rc==LUA_OK;}
bool IsLaunchId(const char*id){return id&&!strncmp(id,"hiapp:",6)&&ValidId(id+6);}
bool Launch(const char*id){if(!IsLaunchId(id))return false;auto*s=new std::string(id+6);if(lv_async_call([](void*p){std::unique_ptr<std::string>s((std::string*)p);Navigate(*s);},s)!=LV_RESULT_OK){delete s;return false;}return true;}
#ifdef HI_APP_SMP_PROBE
int SmpRuntimeProbe(){
 auto s=Ref<Session>(new Session);s->id="hios.smp.probe";s->package.name="SMP probe";s->package.entry="main.lua";
 s->package.files["main.lua"]=R"lua(app={}
 function app.start()
 local c=hios.cpu();assert(c.cores==2 and c.mask==3)
 local n=0;for i=1,20000 do n=n+i end;assert(n==200010000)
 hios.show{title="probe",items={{kind="text",text="ok"}}}
 end
 function app.event(a,b) end)lua";
 __atomic_add_fetch(&live_sessions,1u,__ATOMIC_ACQ_REL);
 pthread_t t;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,64*1024);
 auto p=new Ref<Session>(s);int rc=hi_task_create_compute(&t,&a,Worker,p);pthread_attr_destroy(&a);
 if(rc){delete p;__atomic_sub_fetch(&live_sessions,1u,__ATOMIC_ACQ_REL);return rc;}pthread_join(t,nullptr);
 std::lock_guard<std::mutex>g(s->lock);return s->pending&&s->pending->title=="probe"?0:-1;
}
#endif
lv_obj_t*CreateRuntime(const std::string&id){
 if(!ValidId(id))return nullptr;auto*v=new View;v->session=Ref<Session>(new Session);v->session->id=id;
 v->root=lv_obj_create(nullptr);
#ifdef HI_VIDEO_TARGET_TEST
 if(id=="hios.hi_video")target_test_view=v;
#endif
 lv_obj_set_user_data(v->root,v);screen_strip_obj_chrome(v->root);lv_obj_set_size(v->root,720,720);lv_obj_set_style_bg_color(v->root,lv_color_hex(0xF5F8FC),0);
 v->content=lv_obj_create(v->root);screen_strip_obj_chrome(v->content);lv_obj_set_pos(v->content,0,32);lv_obj_set_size(v->content,720,642);lv_obj_set_style_bg_opa(v->content,LV_OPA_TRANSP,0);lv_obj_remove_flag(v->content,LV_OBJ_FLAG_SCROLLABLE);
 auto*back=lv_button_create(v->root);v->back=back;lv_obj_set_pos(back,12,40);lv_obj_set_size(back,86,48);auto*l=Label(back,"返回",70);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(l);lv_obj_add_event_cb(back,Back,LV_EVENT_CLICKED,v);screen_swipe_back_ignore(back);
 v->timer=lv_timer_create([](lv_timer_t*t){auto*v=(View*)lv_timer_get_user_data(t);std::unique_ptr<Model>m;{std::lock_guard<std::mutex>g(v->session->lock);m=std::move(v->session->pending);}if(m&&m->background&&HiInputMethod_IsVisible()){std::lock_guard<std::mutex>g(v->session->lock);if(!v->session->pending)v->session->pending=std::move(m);return;}if(m){Render(v,*m);if(!v->notified&&!m->app_name.empty()){SystemChrome_NotifyAppOpened(("hiapp:"+v->session->id).c_str(),m->app_name.c_str(),"");v->notified=true;}}RenderMore(v);UpdateVideo(v);UpdateScan(v);},40,v);
 lv_obj_add_event_cb(v->root,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);v->session->stop=true;if(v->session->MediaSnapshot())v->session->MediaSnapshot()->Stop();lv_timer_delete(v->timer);ClearVideo(v);ClearScan(v);HiInputMethod_Hide();
#ifdef HI_VIDEO_TARGET_TEST
 if(target_test_view==v)target_test_view=nullptr;
#endif
 delete v;},LV_EVENT_DELETE,v);
 lv_obj_add_event_cb(v->root,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);if(v->session->MediaSnapshot())v->session->MediaSnapshot()->Stop();ClearScan(v);},LV_EVENT_SCREEN_UNLOADED,v);
 screen_attach_lifecycle(v->root,[](screen_lifecycle_event_t e){PwrKey_OnScreenLifecycle("hiapp",e);});SystemChrome_Attach(v->root,false);screen_attach_left_edge_back(v->root,60,[](){Back(nullptr);});
 auto loading=std::unique_ptr<Model>(new Model);loading->title="正在打开应用";loading->status="读取SD卡中的已安装应用…";v->session->Show(std::move(loading));
 if(__atomic_fetch_add(&live_sessions,1u,__ATOMIC_ACQ_REL)>=2){__atomic_sub_fetch(&live_sessions,1u,__ATOMIC_ACQ_REL);v->session->Error("上个应用正在释放网络资源，请稍后重新打开");return v->root;}
 auto*arg=new Ref<Session>(v->session);pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,32768);sched_param priority{};priority.sched_priority=81;pthread_attr_setschedparam(&a,&priority);pthread_attr_setinheritsched(&a,PTHREAD_EXPLICIT_SCHED);pthread_t thread;int rc=hi_task_create_compute(&thread,&a,Worker,arg);pthread_attr_destroy(&a);if(rc){delete arg;__atomic_sub_fetch(&live_sessions,1u,__ATOMIC_ACQ_REL);v->session->Error("无法启动应用任务");}else pthread_detach(thread);return v->root;
}
}

#ifdef HI_VIDEO_TARGET_TEST
#include "target_smoke.inc"
#endif
