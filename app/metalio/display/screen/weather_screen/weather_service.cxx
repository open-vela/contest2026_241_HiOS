#include "weather_service.h"
#include "weather_city_list.h"
#include "weather_direct.h"
#include "api_endpoints.h"
#include "settings.h"
#include "system_info.h"
#include "ota.h"
#include "hi_task_policy.h"
#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
uint64_t NowMs() {
    timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);
    return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;
}
bool ValidDistrict(const std::string& id) {
    if(id.size()!=6 || id.find_first_not_of("0123456789")!=std::string::npos) return false;
    return weather_cities::FindDistrictById(id.c_str()).province_idx<weather_cities::kProvinceCount;
}
}
WeatherService& WeatherService::Instance() { static auto* service=new WeatherService;return *service; }
WeatherService::WeatherService() {
    Settings saved("weather");
    state_.district_id=saved.GetString("district_id",kDefaultDistrictId);
    if(!ValidDistrict(state_.district_id)) state_.district_id=kDefaultDistrictId;
}
std::string WeatherService::DistrictId() { std::lock_guard<std::mutex> g(lock_);return state_.district_id; }
bool WeatherService::SetDistrictId(const std::string& id) {
    if(!ValidDistrict(id)) return false;
    // Verify persistence before publishing the selected city to either view.
    const std::string previous=DistrictId();
    Settings saved("weather",true);
    if(!saved.SetStringSaved("district_id",id)) {
        saved.SetString("district_id",previous);return false;
    }
    {
        std::lock_guard<std::mutex> g(lock_);
        if(state_.district_id==id) return true;
        state_.district_id=id;state_.data=WeatherDistrictData{};
        state_.error.clear();++state_.generation;
        success_ms_=0;attempt_ms_=0;requested_=true;
    }
    Request();
    return true;
}
WeatherService::Snapshot WeatherService::GetSnapshot() { std::lock_guard<std::mutex> g(lock_);return state_; }
namespace {
bool EndsWithShi(const char* text) {
    const size_t n=std::strlen(text);
    return n>=3 && std::memcmp(text+n-3,"\xe5\xb8\x82",3)==0;
}
void FormatPlace(const char* province,const char* city,const char* district,char* top,size_t top_n,char* bottom,size_t bottom_n) {
    const char* prefix=city&&city[0]?city:"";
    if(prefix[0]=='\0' && province && EndsWithShi(province)) prefix=province;
    if(!district || district[0]=='\0' || std::strcmp(district,prefix)==0) {
        std::snprintf(top,top_n,"%s",prefix);if(bottom_n) bottom[0]='\0';return;
    }
    if(prefix[0]=='\0') { std::snprintf(top,top_n,"%s",district);if(bottom_n) bottom[0]='\0';return; }
    std::snprintf(top,top_n,"%s",prefix);
    std::snprintf(bottom,bottom_n,"%s",district);
}
}
void WeatherService::CopyPanel(PanelFields* out) {
    if(!out) return;
    std::lock_guard<std::mutex> g(lock_);
    char province[64]{},city[64]{},district[64]{};
    std::snprintf(province,sizeof(province),"%s",state_.data.province.c_str());
    std::snprintf(city,sizeof(city),"%s",state_.data.city.c_str());
    std::snprintf(district,sizeof(district),"%s",state_.data.district.c_str());
    if(district[0]=='\0') {
        char id[16];
        std::snprintf(id,sizeof(id),"%s",state_.district_id.c_str());
        const auto found=weather_cities::FindDistrictById(id);
        const auto* entry=weather_cities::GetDistrict(found.province_idx,found.city_idx,found.district_idx);
        if(entry && found.province_idx<weather_cities::kProvinceCount) {
            const auto& pe=weather_cities::kProvinces[found.province_idx];
            std::snprintf(province,sizeof(province),"%s",pe.name?pe.name:"");
            if(found.city_idx<pe.city_count) std::snprintf(city,sizeof(city),"%s",weather_cities::kCities[pe.city_start+found.city_idx].name);
            std::snprintf(district,sizeof(district),"%s",entry->name?entry->name:"");
        }
    }
    if(state_.data.valid && !state_.error.empty()) std::snprintf(out->description,sizeof(out->description),"%s · 上次",state_.data.text.c_str());
    else if(state_.data.valid) std::snprintf(out->description,sizeof(out->description),"%s",state_.data.text.c_str());
    else if(state_.busy) std::snprintf(out->description,sizeof(out->description),"更新中");
    else std::snprintf(out->description,sizeof(out->description),"暂无数据");
    if(state_.data.valid) std::snprintf(out->temperature,sizeof(out->temperature),"%d",int(state_.data.temp));
    else std::snprintf(out->temperature,sizeof(out->temperature),"--");
    FormatPlace(province,city,district,out->place_top,sizeof(out->place_top),out->place_bottom,sizeof(out->place_bottom));
    out->generation=state_.generation;
}
WeatherDistrictData WeatherService::Cached() { return GetSnapshot().data; }
void WeatherService::Request(bool force) {
    std::lock_guard<std::mutex> g(lock_);
    uint64_t now=NowMs();
    if(!state_.busy && (force || !attempt_ms_ ||
       (now-attempt_ms_>=60000 && (!success_ms_ || now-success_ms_>=600000)))) requested_=true;
    if(started_) return;
    pthread_attr_t attr;pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr,32768);
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    started_=hi_task_create_legacy(&thread,&attr,Worker,this)==0;
    pthread_attr_destroy(&attr);
    if(!started_) { state_.error="天气线程启动失败";++state_.generation; }
}
void* WeatherService::Worker(void* p) { static_cast<WeatherService*>(p)->Run();return nullptr; }
void WeatherService::Run() {
    for(;;) {
        std::string id;
        {
            std::lock_guard<std::mutex> g(lock_);
            if(requested_) {
                requested_=false;state_.busy=true;state_.error.clear();
                id=state_.district_id;attempt_ms_=NowMs();
            }
        }
        if(id.empty()) { usleep(200000);continue; }
        WeatherDistrictData data;
        std::string error;
        WeatherDirect::Fetch(id,data,error);
        {
            std::lock_guard<std::mutex> g(lock_);
            state_.busy=false;
            // Never let a slow response overwrite a newly selected district.
            if(id==state_.district_id) {
                state_.error=error;
                if(error.empty()) { state_.data=data;success_ms_=NowMs(); }
                ++state_.generation;
            }
        }
        // Show the primary forecast immediately; optional air quality must
        // never hold the whole weather card blank while another host waits.
        if(error.empty()&&WeatherDirect::AddAir(id,data)){
            std::lock_guard<std::mutex> g(lock_);
            if(id==state_.district_id&&state_.data.uptime==data.uptime){state_.data=std::move(data);++state_.generation;}
        }
    }
}
