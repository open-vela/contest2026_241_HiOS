#include "weather_direct.h"
#include "weather_city_list.h"
#include "ota.h"
#include <map>
#include <unistd.h>
namespace WeatherDirect {namespace {
std::map<std::string,Coordinates> cache;
bool Get(const std::string& url,std::string& body,std::string& error,unsigned deadline,bool properties=false){
    Http http;http.SetDeadlineMs(deadline);http.SetHeader("Accept","application/json");http.SetHeader("Accept-Encoding","identity");http.SetHeader("Connection","close");
    if(!http.Open("GET",url)){error="天气云端连接失败";return false;}
    if(http.GetStatusCode()!=200){error="天气云端返回 HTTP "+std::to_string(http.GetStatusCode());http.Close();return false;}
    char bytes[2048];const size_t limit=properties?16384:96*1024;
    for(;;){int n=http.Read(bytes,sizeof(bytes));if(n==0)break;if(n<0){error="天气数据接收失败";http.Close();return false;}
        if(body.size()+n>limit){error="天气响应超过大小限制";http.Close();return false;}body.append(bytes,n);
        if(properties){std::string object;if(Properties(body,object)){body=std::move(object);http.Close();return true;}}
    }
    http.Close();if(body.empty()||properties){error="所选地区坐标暂不可用";return false;}return true;
}
}
bool Fetch(const std::string& id,WeatherDistrictData& out,std::string& error){
    auto match=weather_cities::FindDistrictById(id.c_str());
    const auto* entry=weather_cities::GetDistrict(match.province_idx,match.city_idx,match.district_idx);
    if(!entry){error="所选地区无效";return false;}
    auto found=cache.find(id);Coordinates coordinates;
    if(found!=cache.end())coordinates=found->second;
    else{
        if(entry->longitude>73&&entry->longitude<136&&entry->latitude>3&&entry->latitude<55){
            coordinates.longitude=entry->longitude;coordinates.latitude=entry->latitude;coordinates.name=entry->name;
        }else{
            std::string properties;
            if(!Get("https://geo.datav.aliyun.com/areas_v3/bound/"+id+".json",properties,error,30000,true)||!ParseCoordinates(properties,id,coordinates)){if(error.empty())error="所选地区坐标不匹配";return false;}
        }
        if(cache.size()>=16)cache.erase(cache.begin());cache[id]=coordinates;
    }
    char point[120];snprintf(point,sizeof(point),"latitude=%.5f&longitude=%.5f",coordinates.latitude,coordinates.longitude);
    const std::string query=std::string(point)+"&timezone=Asia%2FShanghai";
    const std::string url="https://api.open-meteo.com/v1/forecast?"+query+
        "&forecast_days=7&forecast_hours=48&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,weather_code,cloud_cover,surface_pressure,wind_speed_10m,wind_direction_10m"
        "&hourly=temperature_2m,relative_humidity_2m,dew_point_2m,precipitation_probability,precipitation,weather_code,cloud_cover,visibility,surface_pressure,wind_speed_10m,wind_direction_10m,uv_index"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,uv_index_max,wind_speed_10m_max,wind_direction_10m_dominant";
    std::string body;
    if(!Get(url,body,error,45000))return false;
    const auto& province=weather_cities::kProvinces[match.province_idx];
    const auto& city=weather_cities::kCities[province.city_start+match.city_idx];
    if(!ParseForecast(body,id,province.name,city.name,coordinates.name,out)){error="天气数据格式不完整";return false;}
    return true;
}
bool AddAir(const std::string& id,WeatherDistrictData& out){
    auto found=cache.find(id);if(found==cache.end())return false;
    char point[140];snprintf(point,sizeof(point),"latitude=%.5f&longitude=%.5f&timezone=Asia%%2FShanghai",found->second.latitude,found->second.longitude);
    std::string body,error;
    if(!Get(std::string("https://air-quality-api.open-meteo.com/v1/air-quality?")+point+"&current=us_aqi,pm10,pm2_5,carbon_monoxide,nitrogen_dioxide,sulphur_dioxide,ozone",body,error,15000))return false;
    ParseAir(body,out);return true;
}
}
