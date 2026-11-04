
// Engineering-only: no calls, SMS, installs, storage writes or audio-route changes.
#include "hi_task_cpp.h"
#include "freertos_shim.h"
#include "home_screen/home_screen.h"
#include "../hiapp/runtime.h"
#include "../hiapp/registry.h"
#include <atomic>
#include <cmath>
#include <unistd.h>
#include <cstdio>
#include <semaphore.h>
#include <cerrno>
extern "C" int hi_camera_compute_probe();
extern "C" void hi_camera_live_probe();
namespace HiApp{int SmpRuntimeProbe();}
static std::atomic<unsigned>wake_cpu{99},wake_mask{0},wake_frames{0},wake_drops{0};
extern "C" void hi_smp_probe_wake(unsigned cpu,unsigned mask,uint32_t frames,uint32_t drops){wake_cpu=cpu;wake_mask=mask;wake_frames=frames;wake_drops=drops;}
struct Result{unsigned mask=0,cpu=0;int priority=0,policy=0;bool good=false;};
static void*inspect(void*p){auto&r=*(Result*)p;r.mask=hi_task_current_mask();r.cpu=hi_task_current_cpu();sched_param a{};pthread_getschedparam(pthread_self(),&r.policy,&a);r.priority=a.sched_priority;r.good=true;return nullptr;}
struct ShimResult{Result result;sem_t done;};
static void shim_inspect(void*p){auto&r=*(ShimResult*)p;inspect(&r.result);sem_post(&r.done);}
static bool shim_test(){ShimResult normal,io;sem_init(&normal.done,0,0);sem_init(&io.done,0,0);
 bool a=xTaskCreate(shim_inspect,"smp_default",2048,&normal,3,nullptr)==pdPASS;
 bool b=xTaskCreateLegacy(shim_inspect,"smp_legacy",2048,&io,3,nullptr)==pdPASS;
 if(a)while(sem_wait(&normal.done)&&errno==EINTR){}if(b)while(sem_wait(&io.done)&&errno==EINTR){}
 bool ok=a&&b&&normal.result.mask==3&&io.result.mask==1&&io.result.cpu==0;sem_destroy(&normal.done);sem_destroy(&io.done);
 printf("APP_SMP_COMPAT default=%u legacy=%u io_cpu=%u ok=%d\n",normal.result.mask,io.result.mask,io.result.cpu,ok);return ok;}
static bool thread_test(){
 Result normal,normal_cpp,explicit_io,legacy,cpp,neural_child;pthread_t t;int rc=pthread_create(&t,nullptr,inspect,&normal);if(rc)return false;pthread_join(t,nullptr);
 auto regular_cpp=std::thread([&](){inspect(&normal_cpp);});regular_cpp.join();
 pthread_attr_t a;pthread_attr_init(&a);cpu_set_t io=1;pthread_attr_setaffinity_np(&a,sizeof(io),&io);rc=pthread_create(&t,&a,inspect,&explicit_io);pthread_attr_destroy(&a);if(rc)return false;pthread_join(t,nullptr);
 rc=hi_task_create_legacy(&t,nullptr,inspect,&legacy);if(rc)return false;pthread_join(t,nullptr);
 auto th=HiTask::LegacyThread([&](){inspect(&cpp);});th.join();
 rc=hi_task_create_neural(&t,nullptr,[](void*p)->void*{pthread_t child;int e=pthread_create(&child,nullptr,inspect,p);if(!e)pthread_join(child,nullptr);return nullptr;},&neural_child);if(rc)return false;pthread_join(t,nullptr);
 bool ok=normal.mask==3&&normal_cpp.mask==3&&normal_cpp.priority==81&&normal.priority==81&&normal.policy==SCHED_RR&&explicit_io.mask==1&&legacy.mask==1&&cpp.mask==1&&neural_child.mask==2;
 printf("APP_SMP_NATIVE normal=%u normal_cpp=%u priority=%d cpp_priority=%d policy=%d cpp_policy=%d explicit=%u legacy=%u cpp=%u neural_child=%u ok=%d\n",normal.mask,normal_cpp.mask,normal.priority,normal_cpp.priority,normal.policy,normal_cpp.policy,explicit_io.mask,legacy.mask,cpp.mask,neural_child.mask,ok);return ok;
}
struct Stress{std::atomic<unsigned>sum{0},cpus{0},handoff_errors{0};};
static void*stress(void*p){auto&s=*(Stress*)p;pthread_t self=pthread_self();volatile double x=0.125;for(unsigned j=0;j<80;j++){
 unsigned mask=(j&1)?1:2;int rc=hi_task_set_mask_sync(mask);if(rc||pthread_self()!=self||hi_task_current_mask()!=mask||!(mask&(1u<<hi_task_current_cpu())))s.handoff_errors.fetch_add(1);s.cpus.fetch_or(1u<<hi_task_current_cpu());
 for(unsigned i=0;i<1000;i++)x=x*0.5+0.0625;s.sum.fetch_add(x==0.125?1u:1000000u);hi_task_set_mask_sync(3);usleep(1000);
 }return nullptr;}
static void back(void*){HomeScreen::SwitchToHome();}
static void*run(void*){
 usleep(18000000);printf("APP_SMP_PROBE_BEGIN\n");bool ok=thread_test();ok=shim_test()&&ok;Stress s;pthread_t a,b;
 int r1=hi_task_create_compute(&a,nullptr,stress,&s),r2=hi_task_create_compute(&b,nullptr,stress,&s);if(!r1)pthread_join(a,nullptr);if(!r2)pthread_join(b,nullptr);
 bool stress_ok=!r1&&!r2&&s.sum==160&&s.cpus==3&&s.handoff_errors==0;ok=ok&&stress_ok;printf("APP_SMP_FPU rounds=%u cpus=%u ok=%d\n",s.sum.load(),s.cpus.load(),stress_ok);printf("APP_SMP_HANDOFF errors=%u\n",s.handoff_errors.load());
 int lua=HiApp::SmpRuntimeProbe();ok=ok&&!lua;printf("APP_SMP_RUNTIME result=%d\n",lua);
 int camera=hi_camera_compute_probe();ok=ok&&!camera;
 const char*ids[]={"camera","hi_music","chat","digital_people","sd"};
 for(auto id:ids){lv_lock();bool queued=HomeScreen::LaunchApp(id);lv_unlock();printf("APP_SMP_UI id=%s queued=%d\n",id,queued);usleep(7000000);if(!queued)ok=false;hi_camera_live_probe();hi_task_post_ui(back,nullptr);usleep(2000000);}
 for(const auto&r:HiApp::RegistrySnapshot())if(r.name=="Hi 冲浪"||r.name=="Hi 记忆"||r.name=="Hi 读"){
  lv_lock();bool queued=HiApp::Launch(("hiapp:"+r.id).c_str());lv_unlock();printf("APP_SMP_INSTALLED id=%s queued=%d\n",r.id.c_str(),queued);if(!queued)ok=false;usleep(6000000);hi_task_post_ui(back,nullptr);usleep(2000000);
 }
 usleep(10000000);unsigned frames=wake_frames.load();bool ai=frames>0&&wake_mask==2&&wake_cpu==1;ok=ok&&ai;
 printf("APP_SMP_NEURAL cpu=%u mask=%u frames=%u dropped=%u ok=%d\n",wake_cpu.load(),wake_mask.load(),frames,wake_drops.load(),ai);
 printf("APP_SMP_PROBE_DONE ok=%d\n",ok);return nullptr;
}
void hi_task_probe_start(){pthread_t t;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,64*1024);int rc=hi_task_create_legacy(&t,&a,run,nullptr);pthread_attr_destroy(&a);if(!rc)pthread_detach(t);else printf("APP_SMP_PROBE_START_FAIL rc=%d\n",rc);}
