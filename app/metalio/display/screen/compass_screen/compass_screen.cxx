#include "hi_task_policy.h"
#include "compass_screen.h"
#include "compass_math.h"
#include "compass_calibration.h"
#include <unistd.h>
#include "../clock_screen/mechanical_ui.h"
#include "home_screen/home_screen.h"
#include "settings.h"
#include "esp_timer_shim.h"
#include <metalio/metalio.h>
#include <pthread.h>
#include <atomic>
#include <mutex>
#include <string>
using namespace HiDial;
namespace {
struct Sensor {std::mutex lock;std::atomic<bool>active{false},reset{false};bool started=false,valid=false,calibrated=false;float heading=0;double lat=0,lon=0;bool fix=false;std::string status="正在读取传感器…";};
Sensor&Data(){static auto*s=new Sensor;return *s;}
// Work in a right-handed screen frame: X right, Y screen-top, Z outward.
// Gravity and magnetic vectors must share this frame. No yaw angle is inferred
// from gravity alone; north comes only from the measured magnetic field.
using HiCompass::Vec;
using HiCompass::Norm;
using HiCompass::Unit;
void Invalid(const char*text){auto&s=Data();std::lock_guard<std::mutex>l(s.lock);s.valid=false;s.status=text;}
void*Worker(void*){
 auto&s=Data();Settings cfg("compass");HiCompass::FieldTransform transform;
 HiCompass::MatrixCalibration calibration;
 const int version=cfg.GetInt("version",0);const bool current_calibration=version==3||version==4;
 if(current_calibration){
  transform.offset={(float)cfg.GetInt("o0",0),(float)cfg.GetInt("o1",0),(float)cfg.GetInt("o2",0)};
  for(int i=0;i<3;++i)for(int j=0;j<3;++j){
   std::string key="m"+std::to_string(i)+std::to_string(j);
   transform.matrix[i][j]=version==4?cfg.GetInt(key,i==j?10000:0)/10000.f:i==j?cfg.GetInt("s"+std::to_string(i),1000)/1000.f:0;
  }
  transform.strength=cfg.GetInt("field",0);
 }
 bool calibrated=current_calibration&&transform.Valid()&&cfg.GetBool("valid",false),calibrating=!calibrated;
 HiCompass::HeadingFilter filter;bool gravity_ready=false;Vec gravity{0,0,1};
 uint32_t last_sample=0;
 // A level-app zero defines a chosen flat plane, not accelerometer bias.
 // It must never rotate gravity used by the compass.
 const int zero[3]={0,0,0};
 int64_t calibration_started=0;

 for(;;){
  if(!s.active){usleep(200000);filter.ready=false;gravity_ready=false;continue;}
  if(s.reset.exchange(false)){
   // Keep the last valid calibration until a replacement is actually saved.
   calibrating=true;calibration=HiCompass::MatrixCalibration{};calibration_started=0;filter.ready=false;gravity_ready=false;
  }
  if(calibrating){
   const auto now=esp_timer_get_time();if(!calibration_started)calibration_started=now;
   if(now-calibration_started>20000000){calibrating=false;calibration_started=0;}
  }
  if(!calibrated&&!calibrating){Invalid("校准未完成 · 点击校准后按提示翻转设备");usleep(40000);continue;}

  int16_t raw[3]{};uint8_t status=0;int rc=metalio_qmc6309_read_raw_ex(&raw[0],&raw[1],&raw[2],&status);
  if(rc<0||(status&2)||!(status&1)){
   const uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
   if(rc<0||(status&2)||!last_sample||uint32_t(now-last_sample)>250)Invalid(rc<0?"磁力计不可用，请稍后重试":status&2?"磁场过强，请远离磁铁":"等待磁力计数据");
   usleep(40000);continue;
  }
  last_sample=(uint32_t)(esp_timer_get_time()/1000);
  int ax=0,ay=0,az=0;if(metalio_sc7a20h_read_mg(&ax,&ay,&az)<0){Invalid("重力传感器暂不可用，方向未更新");usleep(40000);continue;}
  Vec measured{(float)(ax-zero[0]),(float)(ay-zero[1]),(float)(az-zero[2])};float gn=Norm(measured);
  if(gn<850||gn>1150){Invalid("移动过快，请稍稳住设备");gravity_ready=false;usleep(40000);continue;}
  measured=Unit(measured);if(!gravity_ready){gravity=measured;gravity_ready=true;}else{gravity=Unit({gravity.x*.4f+measured.x*.6f,gravity.y*.4f+measured.y*.6f,gravity.z*.4f+measured.z*.6f});}
  if(calibrating){
   calibration.Add({float(raw[0]),float(raw[1]),float(raw[2])},measured);
   HiCompass::FieldTransform candidate;
   if(calibration.samples%5==0 && calibration.FinishMatrix(candidate)){
    Settings save("compass",true);
    const char*keys[]={"o0","o1","o2","s0","s1","s2","m00","m01","m02","m10","m11","m12","m20","m21","m22","field","valid","planar","version"};
    int32_t values[19]{};values[0]=::lroundf(candidate.offset.x);values[1]=::lroundf(candidate.offset.y);values[2]=::lroundf(candidate.offset.z);
    for(int i=0;i<3;++i){values[i+3]=::lroundf(candidate.matrix[i][i]*1000);for(int j=0;j<3;++j)values[6+i*3+j]=::lroundf(candidate.matrix[i][j]*10000);}
    values[15]=::lroundf(candidate.strength);values[16]=1;values[17]=0;values[18]=4;
    if(save.SetInts(keys,values,19)){
     transform=candidate;calibrated=true;calibrating=false;filter.ready=false;
    }else{Invalid("校准保存失败，请点击校准重试");usleep(40000);continue;}
   }else{
    char text[128];snprintf(text,sizeof(text),"校准 %u%% · %s",std::min(99u,calibration.Progress()),calibration.NextMove());
    Invalid(text);usleep(40000);continue;
   }
  }
  Vec magnetic=transform.Apply({(float)raw[0],(float)raw[1],(float)raw[2]});float mn=Norm(magnetic);
  if(!__builtin_isfinite(mn)||mn<10){Invalid("磁场数据不足，方向未更新");usleep(40000);continue;}
  if(transform.strength>0&&(mn<transform.strength*.70f||mn>transform.strength*1.30f)){
   Invalid("磁场干扰，请远离充电器、扬声器或重新校准");filter.ready=false;usleep(40000);continue;
  }
  float angle=0;
  auto result=HiCompass::Heading(magnetic,gravity,angle);
  if(result!=HiCompass::Result::Ok){
   Invalid(result==HiCompass::Result::PointingVertical?"请将屏幕稍微放平，恢复方向指示":"磁场异常，请远离磁铁或充电器");
   filter.ready=false;usleep(40000);continue;
  }
  const float smooth=filter.Update(angle);
  int fix=0,sats=0;double lat=0,lon=0;metalio_gps_get_snapshot(&fix,&sats,&lat,&lon);
  {std::lock_guard<std::mutex>l(s.lock);s.valid=calibrated;s.heading=smooth;s.calibrated=calibrated;s.fix=fix>0;s.lat=lat;s.lon=lon;s.status="磁北方向 · 倾斜与磁场补偿";}
  usleep(40000);
 }
 return nullptr;
}
void Start(){auto&s=Data();s.active=true;if(s.started)return;pthread_attr_t a;pthread_attr_init(&a);pthread_attr_setstacksize(&a,16384);pthread_attr_setdetachstate(&a,PTHREAD_CREATE_DETACHED);sched_param priority{};priority.sched_priority=CONFIG_METALIO_APP_PRIORITY+2;pthread_attr_setschedparam(&a,&priority);pthread_attr_setschedpolicy(&a,SCHED_RR);pthread_attr_setinheritsched(&a,PTHREAD_EXPLICIT_SCHED);pthread_t t;s.started=hi_task_create_legacy(&t,&a,Worker,nullptr)==0;pthread_attr_destroy(&a);if(!s.started)Invalid("传感器线程启动失败，请重新进入");}
struct View{lv_obj_t*root,*reading,*status,*location,*arrow;Dial face;lv_timer_t*timer;bool shown=false;};
}
lv_obj_t*CompassScreen::CreateStatic(){Start();auto*v=new View;v->root=lv_obj_create(nullptr);screen_strip_obj_chrome(v->root);lv_obj_set_size(v->root,720,720);lv_obj_set_style_bg_color(v->root,lv_color_hex(0xF7F9F6),0);lv_obj_set_style_bg_opa(v->root,255,0);lv_obj_remove_flag(v->root,LV_OBJ_FLAG_SCROLLABLE);
 Label(v->root,"Hi 指南",180,42,360,true);Button(v->root,"返回",16,34,94,52,[](lv_event_t*){HomeScreen::SwitchToHome();});Button(v->root,"校准",610,34,94,52,[](lv_event_t*){Invalid("三维校准 · 缓慢转动并翻转各面");Data().reset=true;});
 v->arrow=Label(v->root,"▲",310,132,100,true,0xC84836);v->face.compass=true;Face(v->root,&v->face,148,174,424);lv_obj_add_flag(v->face.obj,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(v->arrow,LV_OBJ_FLAG_HIDDEN);v->reading=Label(v->root,"--",130,610,460,true);v->location=Label(v->root,"等待 GPS 位置",30,652,660);v->status=Label(v->root,"",30,100,660);
 v->timer=lv_timer_create([](lv_timer_t*t){auto*v=(View*)lv_timer_get_user_data(t);auto&s=Data();std::lock_guard<std::mutex>l(s.lock);lv_label_set_text(v->status,s.status.c_str());if(!s.valid){lv_label_set_text(v->reading,"--");lv_obj_add_flag(v->face.obj,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(v->arrow,LV_OBJ_FLAG_HIDDEN);v->shown=false;return;}lv_obj_remove_flag(v->face.obj,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(v->arrow,LV_OBJ_FLAG_HIDDEN);v->face.rotation=s.heading;v->shown=true;const char*names[]={"北","东北","东","东南","南","西南","西","西北"};char text[96];int degree=(int)(v->face.rotation+.5f)%360;snprintf(text,sizeof(text),"%s %d°",names[((degree+22)/45)%8],degree);lv_label_set_text(v->reading,text);lv_obj_invalidate(v->face.obj);if(s.fix)snprintf(text,sizeof(text),"%.5f° %s   %.5f° %s",std::abs(s.lat),s.lat>=0?"N":"S",std::abs(s.lon),s.lon>=0?"E":"W");else snprintf(text,sizeof(text),"暂无 GPS 位置 · 方向仍可使用");lv_label_set_text(v->location,text);},33,v);
 lv_obj_add_event_cb(v->root,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);Data().active=false;Invalid("正在读取传感器…");v->shown=false;lv_timer_pause(v->timer);},LV_EVENT_SCREEN_UNLOADED,v);
 lv_obj_add_event_cb(v->root,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);Invalid("正在读取传感器…");Start();lv_timer_resume(v->timer);},LV_EVENT_SCREEN_LOADED,v);
 lv_obj_add_event_cb(v->root,[](lv_event_t*e){auto*v=(View*)lv_event_get_user_data(e);Data().active=false;lv_timer_delete(v->timer);delete v;},LV_EVENT_DELETE,v);screen_attach_swipe_back(v->root,[]{HomeScreen::SwitchToHome();});return v->root;
}
lv_obj_t *CompassScreen::CreateIcon(lv_obj_t *parent, int size) {
    auto *icon=lv_image_create(parent);
    lv_image_set_src(icon,"A:ic_app_home_theme1_compass.spng");
    lv_obj_set_size(icon,size,size);
    lv_image_set_scale(icon,size*256/128);
    lv_image_set_pivot(icon,0,0);
    lv_obj_remove_flag(icon,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));
    return icon;
}
