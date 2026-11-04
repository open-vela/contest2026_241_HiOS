#include "hi_task_policy.h"
// Installs the explicitly supplied Xiaobai assets. No USB mode or radio changes.
#include "xiaobai_resources.h"
#include <metalio/metalio.h>
#include <mbedtls/sha256.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <algorithm>
extern "C" const unsigned char xiaobai_asset_pack[],xiaobai_asset_pack_end[];
namespace XiaobaiResources {namespace {
struct Resource{const char*name;unsigned offset,size;const char*hash;};
#include "xiaobai_assets.inc"
std::atomic<int> state{0};
bool Directory(const char*path){struct stat st{};if(stat(path,&st)==0)return S_ISDIR(st.st_mode);return mkdir(path,0775)==0||errno==EEXIST;}
std::string HashFile(const char*path,size_t expected){FILE*f=fopen(path,"rb");if(!f)return {};struct stat st{};if(fstat(fileno(f),&st)<0||(size_t)st.st_size!=expected){fclose(f);return {};}
 mbedtls_sha256_context ctx;mbedtls_sha256_init(&ctx);mbedtls_sha256_starts(&ctx,0);unsigned char block[4096],sum[32];size_t n;while((n=fread(block,1,sizeof(block),f)))mbedtls_sha256_update(&ctx,block,n);bool ok=!ferror(f);fclose(f);mbedtls_sha256_finish(&ctx,sum);mbedtls_sha256_free(&ctx);char hex[65];for(int i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",sum[i]);return ok?hex:"";
}
bool Existing(const char*path){struct stat st{};return stat(path,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>0;}
bool Install(const Resource&r,bool installed){
 std::string path="/sdcard/System/emotion/"+std::string(r.name);if(installed&&Existing(path.c_str()))return true;
 if(HashFile(path.c_str(),r.size)==r.hash)return true;
 if((size_t)r.offset+r.size>(size_t)(xiaobai_asset_pack_end-xiaobai_asset_pack))return false;
 std::string pending=path+".xiaobai-new";FILE*f=fopen(pending.c_str(),"wb");if(!f)return false;bool ok=true;
 for(unsigned at=0;at<r.size;){size_t n=std::min((size_t)4096,(size_t)(r.size-at));if(fwrite(xiaobai_asset_pack+r.offset+at,1,n,f)!=n){ok=false;break;}at+=n;usleep(1000);}
 if(fflush(f)!=0||fsync(fileno(f))!=0)ok=false;if(fclose(f)!=0)ok=false;
 if(!ok||HashFile(pending.c_str(),r.size)!=r.hash){unlink(pending.c_str());return false;}
 std::string backup;
 if(Existing(path.c_str())){if(!Directory("/sdcard/System/emotion/backup-before-xiaobai")){unlink(pending.c_str());return false;}
  for(int i=0;i<1000;++i){backup="/sdcard/System/emotion/backup-before-xiaobai/"+std::string(r.name)+"."+std::to_string(i);if(access(backup.c_str(),F_OK)<0)break;backup.clear();}
  if(backup.empty()||rename(path.c_str(),backup.c_str())<0){unlink(pending.c_str());return false;}
 }
 if(rename(pending.c_str(),path.c_str())<0){if(!backup.empty())rename(backup.c_str(),path.c_str());unlink(pending.c_str());return false;}
 return HashFile(path.c_str(),r.size)==r.hash;
}
void*Worker(void*){
 // Provisioning needs writes. Establish RW directly, after bringup/UI are
 // ready and before background phone/SMS readers open their stores. Opening
 // RO first introduced an unnecessary unmount/remount handoff and EBUSY.
 int rc=-ENODEV;
 for(int i=0;i<80;++i){rc=metalio_sdcard_ensure_rw();if(rc>=0)break;
  if(i==0)printf("XIAOBAI_ASSETS_WAIT_RW error=%d\n",rc);
  // RO image/message readers may still be closing during the first paint.
  // Never force-unmount their files or reset the shared SD controller.
  usleep(250000);
 }
 if(rc<0){state=-1;printf("XIAOBAI_ASSETS_ERROR sd code=%d\n",rc);return nullptr;}

 if(!Directory("/sdcard/System")||!Directory("/sdcard/System/emotion")){state=-2;printf("XIAOBAI_ASSETS_ERROR directory\n");return nullptr;}
 const char*marker="/sdcard/System/emotion/.xiaobai-installed";char old[80]{};FILE*f=fopen(marker,"rb");if(f){fread(old,1,64,f);fclose(f);}bool installed=strcmp(old,kPackHash)==0;
 for(const auto&r:kResources)if(!Install(r,installed)){state=-3;printf("XIAOBAI_ASSETS_ERROR file=%s\n",r.name);return nullptr;}
 if(!installed){std::string temp=std::string(marker)+".tmp";f=fopen(temp.c_str(),"wb");bool ok=f&&fwrite(kPackHash,1,64,f)==64;if(f){if(fflush(f)!=0||fsync(fileno(f))!=0)ok=false;if(fclose(f)!=0)ok=false;}if(!ok||rename(temp.c_str(),marker)<0){unlink(temp.c_str());state=-4;printf("XIAOBAI_ASSETS_ERROR marker\n");return nullptr;}}
 state=2;printf("XIAOBAI_ASSETS_VERIFIED files=%u pack=%s transparent_png=6\n",(unsigned)(sizeof(kResources)/sizeof(kResources[0])),kPackHash);fflush(stdout);return nullptr;
}
}
void EnsureAsync(){int expected=state.load();if(expected==1||expected==2)return;if(!state.compare_exchange_strong(expected,1))return;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,16384);pthread_attr_setdetachstate(&a,PTHREAD_CREATE_DETACHED);sched_param p{};p.sched_priority=CONFIG_METALIO_APP_PRIORITY-4;pthread_attr_setschedparam(&a,&p);pthread_attr_setschedpolicy(&a,SCHED_RR);pthread_attr_setinheritsched(&a,PTHREAD_EXPLICIT_SCHED);pthread_t t;int rc=hi_task_create_legacy(&t,&a,Worker,nullptr);pthread_attr_destroy(&a);if(rc)state=-5;}
int Status(){return state.load();}
}
