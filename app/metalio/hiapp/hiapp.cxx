// SPDX-License-Identifier: Apache-2.0
#include "hiapp.h"
#include "resources.h"
#include "registry.h"
#include "cJSON.h"
#include <metalio/metalio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <strings.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <memory>
#include <set>
#include <ctime>
namespace HiApp {
namespace {
int64_t StorageMs(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return(int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
// In this FAT configuration a lowercase 8.3 path is parsed as an LFN.
// Windows can store "app" as the short name APP without an LFN entry.
// Use the actual directory spelling returned by readdir, locally to HiApp.
std::string ChildDirectory(const std::string&parent,const char*wanted){
 DIR*d=opendir(parent.c_str());if(d){while(auto*e=readdir(d)){if(!strcasecmp(e->d_name,wanted)){std::string found=parent+"/"+e->d_name;struct stat st{};if(stat(found.c_str(),&st)==0&&S_ISDIR(st.st_mode)){closedir(d);return found;}}}closedir(d);}return parent+"/"+wanted;
}
std::string Root(){return ChildDirectory("/sdcard","app");}
std::string Area(const char*name){return ChildDirectory(Root(),name);}
struct JsonDelete{void operator()(cJSON*p)const{cJSON_Delete(p);}};
using Json=std::unique_ptr<cJSON,JsonDelete>;
std::string Field(cJSON*j,const char*k){auto*v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)&&v->valuestring?v->valuestring:"";}
bool Leaf(const std::string&n){if(n.empty()||n.size()>180||n=="."||n=="..")return false;for(char c:n)if(!c||c=='/'||c=='\\')return false;return true;}
bool Read(const std::string&p,std::string&out,size_t limit){
 int fd=open(p.c_str(),O_RDONLY);if(fd<0)return false;struct stat s{};
 if(fstat(fd,&s)<0||!S_ISREG(s.st_mode)||s.st_size<0||(size_t)s.st_size>limit){close(fd);return false;}
 out.resize(s.st_size);size_t at=0;while(at<out.size()){ssize_t n=read(fd,&out[at],std::min(size_t(16*1024),out.size()-at));if(n>0)at+=n;else if(n<0&&errno==EINTR)continue;else break;}
 close(fd);return at==out.size();
}
bool Write(const std::string&p,const std::string&data){
 int fd=open(p.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0666);if(fd<0)return false;size_t at=0;
 while(at<data.size()){ssize_t n=write(fd,data.data()+at,std::min(size_t(16*1024),data.size()-at));if(n>0)at+=n;else if(n<0&&errno==EINTR)continue;else break;}
 bool ok=at==data.size()&&fsync(fd)==0;close(fd);return ok;
}
bool Directory(const std::string&p){struct stat s{};if(stat(p.c_str(),&s)==0)return S_ISDIR(s.st_mode);return mkdir(p.c_str(),0777)==0;}
// An interrupted replacement leaves either the live file or its .previous
// sibling. Never remove the old application before the staged file is synced.
bool Commit(const std::string&path,const std::string&data){
 const std::string temp=path+".installing",prev=path+".previous";
 if(!Write(temp,data)){unlink(temp.c_str());return false;}
 struct stat st{};bool old=stat(path.c_str(),&st)==0;
 if(old){unlink(prev.c_str());if(rename(path.c_str(),prev.c_str())<0){unlink(temp.c_str());return false;}}
 if(rename(temp.c_str(),path.c_str())<0){if(old)rename(prev.c_str(),path.c_str());unlink(temp.c_str());return false;}
 // Retain .previous until a later successful update; Prepare() can recover
 // it after a power loss between the two renames on FAT.
 return true;
}
bool JsonDepth(const std::string&s){int depth=0;bool quote=false,escape=false;for(unsigned char c:s){if(!c)return false;if(quote){if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quote=false;}else if(c=='"')quote=true;else if(c=='{'||c=='['){if(++depth>24)return false;}else if(c=='}'||c==']'){if(--depth<0)return false;}}return !quote&&depth==0;}
}
uint32_t Crc32(const void*data,size_t n){uint32_t crc=~0u;auto*p=(const unsigned char*)data;while(n--){crc^=*p++;for(int b=0;b<8;b++)crc=(crc>>1)^(0xedb88320u&-(crc&1));}return ~crc;}
bool ValidId(const std::string&id){if(id.empty()||id.size()>25||id.front()=='.'||id.back()=='.'||id.find("..")!=id.npos)return false;for(char c:id)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return false;return true;}
std::string InstalledPath(const std::string&id){return Area("installed")+"/"+id+".hiapp";}
std::string IconPath(const std::string&id){return Area("icons")+"/"+id+".png";}
bool Load(const std::string&path,Package&p,std::string&error,bool metadata_only){
 const int64_t started=StorageMs();p=Package{};std::string raw;if(!Read(path,raw,kPackageLimit)){error="无法读取安装包，或包超过256KiB";return false;}
 if(raw.size()<17||raw.compare(0,7,"HIAPP1\n")||raw[15]!='\n'){error="不是HiOS .hiapp安装包";return false;}
 uint32_t expected=0;for(int i=7;i<15;i++){char c=raw[i];int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(d<0){error="安装包校验字段无效";return false;}expected=(expected<<4)|d;}
 std::string body=raw.substr(16);if(Crc32(body.data(),body.size())!=expected||!JsonDepth(body)){error="安装包损坏，校验失败";return false;}
 const char*end=nullptr;Json j(cJSON_ParseWithOpts(body.c_str(),&end,true));if(!j||!cJSON_IsObject(j.get())){error="安装包信息无法解析";return false;}
 auto*m=cJSON_GetObjectItemCaseSensitive(j.get(),"manifest");auto*f=cJSON_GetObjectItemCaseSensitive(j.get(),"files");
 p.id=Field(m,"id");p.name=Field(m,"name");p.version=Field(m,"version");p.entry=Field(m,"entry");p.icon=Field(m,"icon");
 auto*sdk=cJSON_GetObjectItemCaseSensitive(m,"sdk");
 if(!ValidId(p.id)||p.name.empty()||p.name.size()>36||p.version.empty()||p.version.size()>32||!cJSON_IsNumber(sdk)||(sdk->valuedouble!=1&&sdk->valuedouble!=2)||!cJSON_IsObject(f)){error="应用信息不完整，或需要其它SDK版本";return false;}
 auto*permissions=cJSON_GetObjectItemCaseSensitive(m,"permissions");if(permissions&&!cJSON_IsArray(permissions)){error="权限声明无效";return false;}
 for(auto*v=permissions?permissions->child:nullptr;v;v=v->next){if(!cJSON_IsString(v)){error="权限声明无效";return false;}std::string perm=v->valuestring;if(perm=="network")p.network=true;else if(perm=="storage")p.storage=true;else if(perm=="camera")p.camera=true;else{error="当前系统不支持权限："+perm;return false;}}
 // A directory listing needs the manifest and icon, not copies of every
 // Lua/HTML asset or the complete package. Validate those fields in-place.
 size_t count=0;std::set<std::string>names;for(auto*v=f->child;v;v=v->next){std::string name=v->string?v->string:"";
  if(++count>64||name.empty()||name.size()>128||name[0]=='/'||name.find("..")!=name.npos||name.find('\\')!=name.npos||!cJSON_IsString(v)||!v->valuestring||!names.insert(name).second){error="安装包文件列表无效";return false;}
  if(!metadata_only||name==p.icon)p.files[name]=v->valuestring;
 }
 auto*entry=cJSON_GetObjectItemCaseSensitive(f,p.entry.c_str());const char*source=cJSON_IsString(entry)?entry->valuestring:nullptr;
 if(!source||p.entry.size()<4||p.entry.substr(p.entry.size()-4)!=".lua"||strlen(source)>96*1024||!*source||(unsigned char)*source==27){error="缺少有效Lua源码入口";return false;}
 auto*resources=cJSON_GetObjectItemCaseSensitive(j.get(),"resources");if(resources&&!cJSON_IsObject(resources)){error="资源列表无效";return false;}
 size_t total=0;for(auto*v=resources?resources->child:nullptr;v;v=v->next){std::string name=v->string?v->string:"";auto*n=cJSON_GetObjectItemCaseSensitive(v,"size");if(++count>64||name.empty()||name[0]=='/'||name.find("..")!=name.npos||name.find('\\')!=name.npos||!names.insert(name).second||!cJSON_IsNumber(n)||n->valuedouble<0||n->valuedouble>192*1024||(total+=(size_t)n->valuedouble)>512*1024){error="资源尺寸或路径无效";return false;}
  if(!metadata_only||name==p.icon){if(name==p.icon&&n->valuedouble>64*1024){error="图标资源过大";return false;}std::string out;if(!UnpackResource(Field(v,"data"),Field(v,"encoding"),(size_t)n->valuedouble,out)){error="资源解码失败："+name;return false;}p.files[name]=std::move(out);}
 }
 printf("HIAPP_LOAD bytes=%u metadata=%d ms=%lld\n",(unsigned)raw.size(),metadata_only,(long long)(StorageMs()-started));if(!metadata_only)p.raw=std::move(raw);return true;
}
bool Prepare(bool write,std::string&error){
 int rc=write?metalio_sdcard_ensure_rw():metalio_sdcard_mount_ro();if(rc<0){error=rc==-EBUSY?"SD卡正被占用，请先关闭虚拟U盘后刷新":"SD卡未就绪（错误"+std::to_string(-rc)+"），请检查内存卡";return false;}
 const std::string root=Root();struct stat s{};if(stat(root.c_str(),&s)<0||!S_ISDIR(s.st_mode)){error="请先在SD卡根目录创建app文件夹（app或APP均支持）";return false;}
 if(!write)return true;
 if(!Directory(Area("installed"))||!Directory(Area("data"))||!Directory(Area("cache"))||!Directory(Area("icons"))){error="无法创建应用管理目录，请检查SD卡";return false;}
 DIR*d=opendir(Area("installed").c_str());if(d){while(auto*e=readdir(d)){std::string n=e->d_name;if(n.size()>15&&n.substr(n.size()-15)==".hiapp.previous"){std::string id=n.substr(0,n.size()-15);if(ValidId(id)){std::string p=InstalledPath(id);if(stat(p.c_str(),&s)<0)rename((p+".previous").c_str(),p.c_str());}}}closedir(d);}return true;
}
std::vector<Record> Scan(bool installed,std::string&error,ScanProgress progress,void*context){
 const int64_t started=StorageMs();unsigned revision=RegistryRevision();size_t icon_budget=2*1024*1024;std::vector<Record>out;
 auto step=[&](int phase){if(progress&&!progress(context,phase)){error="本次扫描已停止，可稍后刷新重试";return false;}return true;};
 if(!step(1))return out;
 write(1,"HIAPP_SCAN_READONLY\n",20);
 // Enumeration must never require a writable mount, unmount a live music
 // stream, or create FAT directories. Only Install/WriteData need writes.
 if(!Prepare(false,error))return out;
 if(!step(2))return out;
 const std::string root=Root(),installed_dir=ChildDirectory(root,"installed"),icon_dir=ChildDirectory(root,"icons");
 std::map<std::string,std::string>versions;std::string dir=installed?installed_dir:root;DIR*d=opendir(dir.c_str());if(!d){if(!installed||errno!=ENOENT)error="无法读取"+dir+"（错误"+std::to_string(errno)+"）";return out;}
 write(1,"HIAPP_SCAN_DIRECTORY\n",21);
 for(;;){if(!step(3))break;errno=0;auto*e=readdir(d);int read_error=errno;if(!e){if(read_error)error="目录读取失败（错误"+std::to_string(read_error)+"）";break;}
  std::string name=e->d_name;if(name.size()<6||strcasecmp(name.c_str()+name.size()-6,".hiapp")||!Leaf(name))continue;
  struct stat st{};std::string path=dir+"/"+name;if(stat(path.c_str(),&st)<0||!S_ISREG(st.st_mode))continue;
  if(!step(4))break;
  Record r;r.file=name;r.name=name;r.installed=installed;Package p;if(Load(path,p,r.error,true)){r.id=p.id;r.name=p.name;r.version=p.version;r.icon=p.icon.empty()?"":icon_dir+"/"+p.id+".png";auto image=p.files.find(p.icon);if(image!=p.files.end()&&image->second.size()>=24&&image->second.size()<=64*1024&&image->second.size()<=icon_budget&&image->second.compare(0,8,"\x89PNG\r\n\x1a\n")==0){r.png=Ref<PngIcon>(new PngIcon);r.png->bytes=std::move(image->second);r.png->fingerprint=Crc32(r.png->bytes.data(),r.png->bytes.size());icon_budget-=r.png->bytes.size();}if(installed&&name!=p.id+".hiapp")r.error="安装记录与应用ID不一致";if(installed){r.present=true;r.installed_version=p.version;}else{auto cached=versions.find(p.id);if(cached==versions.end()){Package existing;std::string ignored;const bool present=Load(installed_dir+"/"+p.id+".hiapp",existing,ignored,true);versions[p.id]=present?existing.version:std::string();cached=versions.find(p.id);}r.installed_version=cached->second;r.present=!r.installed_version.empty();}}out.push_back(std::move(r));
 }closedir(d);std::sort(out.begin(),out.end(),[](const Record&a,const Record&b){return a.name<b.name;});if(installed&&error.empty())RegistryPublish(out,revision);char trace[120];int n=snprintf(trace,sizeof(trace),"HIAPP_SCAN_COMPLETE count=%u error=%d ms=%lld\n",(unsigned)out.size(),!error.empty(),(long long)(StorageMs()-started));if(n>0)write(1,trace,std::min((size_t)n,sizeof(trace)-1));return out;
}
bool Install(const std::string&file,std::string&message){
 if(!Leaf(file)||file.size()<6||strcasecmp(file.c_str()+file.size()-6,".hiapp")){message="安装包文件名无效";return false;}
 if(!Prepare(true,message))return false;Package p;if(!Load(Root()+"/"+file,p,message)||!ValidateLua(p,message))return false;
 if(!p.icon.empty()){auto icon=p.files.find(p.icon);if(icon==p.files.end()||icon->second.size()>64*1024||icon->second.size()<24||icon->second.compare(0,8,"\x89PNG\r\n\x1a\n")){message="图标必须为64KiB以内的PNG";return false;}auto*b=(const unsigned char*)icon->second.data();uint32_t w=(b[16]<<24)|(b[17]<<16)|(b[18]<<8)|b[19],h=(b[20]<<24)|(b[21]<<16)|(b[22]<<8)|b[23];if(!w||!h||w>256||h>256){message="图标尺寸必须在256像素以内";return false;}}
 if(!Commit(InstalledPath(p.id),p.raw)){message="安装失败，原应用已保留";return false;}Record r;r.id=p.id;r.name=p.name;r.version=p.version;r.icon=p.icon.empty()?"":IconPath(p.id);auto image=p.files.find(p.icon);if(image!=p.files.end()){r.png=Ref<PngIcon>(new PngIcon);r.png->bytes=std::move(image->second);r.png->fingerprint=Crc32(r.png->bytes.data(),r.png->bytes.size());if(!Commit(r.icon,r.png->bytes))r.icon.clear();}r.installed=r.present=true;RegistryUpsert(r);message="已安装："+p.name+" "+p.version;return true;
}
bool Uninstall(const std::string&id,std::string&message){
 if(!ValidId(id)||!Prepare(true,message))return false;std::string path=InstalledPath(id);
 // Remove the rollback copy first so a power failure cannot resurrect an
 // app whose live package has already been removed by the user.
 if(unlink((path+".previous").c_str())<0&&errno!=ENOENT){message="卸载失败，无法清理旧版本";return false;}
 if(unlink(path.c_str())<0){message="卸载失败，请刷新列表";return false;}unlink((path+".installing").c_str());RegistryRemove(id);message="已卸载，用户数据保留";return true;
}
bool ReadData(const std::string&id,std::string&v){std::string e;if(!ValidId(id)||!Prepare(false,e))return false;std::string p=Area("data")+"/"+id+".data";return Read(p,v,32*1024)||Read(p+".previous",v,32*1024);}
bool WriteData(const std::string&id,const std::string&v){std::string e;return ValidId(id)&&v.size()<=32*1024&&Prepare(true,e)&&Commit(Area("data")+"/"+id+".data",v);}
bool ReadLauncherOrder(std::string&v){std::string error;if(!Prepare(false,error))return false;auto p=Area("data")+"/launcher.order";return Read(p,v,16*1024)||Read(p+".previous",v,16*1024);}
bool WriteLauncherOrder(const std::string&v){std::string error;return v.size()<=16*1024&&Prepare(true,error)&&Commit(Area("data")+"/launcher.order",v);}

}
