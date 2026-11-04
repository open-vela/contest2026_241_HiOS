#pragma once
#include "weather_data.h"
#include <cmath>
#include <cstdio>
namespace WeatherDirect {
struct Coordinates{double latitude=0,longitude=0;std::string name;};
inline bool Properties(const std::string& prefix,std::string& object){
    auto at=prefix.find("\"properties\"");if(at==prefix.npos)return false;
    at=prefix.find('{',at+12);if(at==prefix.npos)return false;
    unsigned depth=0;bool quoted=false,escaped=false;
    for(size_t i=at;i<prefix.size();++i){char c=prefix[i];
        if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;continue;}
        if(c=='"')quoted=true;else if(c=='{')++depth;else if(c=='}'&&!--depth){object=prefix.substr(at,i-at+1);return true;}
    }return false;
}
inline bool Number(const cJSON* item,double& value){if(!cJSON_IsNumber(item)||!__builtin_isfinite(item->valuedouble))return false;value=item->valuedouble;return true;}
inline int Rounded(double value){return int(value>=0?value+.5:value-.5);}
inline int Num(const cJSON* obj,const char* key,int missing=-1){double value;return Number(cJSON_GetObjectItemCaseSensitive(obj,key),value)&&value>-1000000&&value<10000000?Rounded(value):missing;}
inline double Real(const cJSON* obj,const char* key,double missing=-1){double value;return Number(cJSON_GetObjectItemCaseSensitive(obj,key),value)&&value>-1000000&&value<10000000?value:missing;}
inline const cJSON* At(const cJSON* obj,const char* key,int index){return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(obj,key),index);}
inline int Value(const cJSON* obj,const char* key,int index,int missing=-1){double value;return Number(At(obj,key,index),value)&&value>-1000000&&value<10000000?Rounded(value):missing;}
inline std::string Text(const cJSON* item){return cJSON_IsString(item)&&item->valuestring?item->valuestring:"";}
inline bool ParseCoordinates(const std::string& object,const std::string& id,Coordinates& out){
    cJSON* root=cJSON_Parse(object.c_str());if(!root)return false;
    const cJSON* center=cJSON_GetObjectItemCaseSensitive(root,"center");
    if(!cJSON_IsArray(center))center=cJSON_GetObjectItemCaseSensitive(root,"centroid");
    double lon=0,lat=0;bool ok=Num(root,"adcode")==std::atoi(id.c_str())&&Number(cJSON_GetArrayItem(center,0),lon)&&Number(cJSON_GetArrayItem(center,1),lat)&&lat>=-90&&lat<=90&&lon>=-180&&lon<=180;
    if(ok){out.latitude=lat;out.longitude=lon;out.name=weather_json::JsonStr(root,"name");ok=!out.name.empty();}
    cJSON_Delete(root);return ok;
}
inline const char* Description(int code){
    switch(code){case 0:return "晴";case 1:case 2:return "多云";case 3:return "阴";case 45:case 48:return "雾";
    case 51:case 53:case 55:case 56:case 57:return "毛毛雨";case 61:case 80:return "小雨";case 63:case 81:return "中雨";case 65:case 82:return "大雨";
    case 66:case 67:return "冻雨";case 71:case 73:case 75:case 77:case 85:case 86:return "雪";case 95:case 96:case 99:return "雷阵雨";default:return "天气未知";}
}
inline std::string WindClass(double speed){if(speed<0)return "--";const int limits[]={1,6,12,20,29,39,50,62,75,89,103,118};int level=0;for(int limit:limits)if(speed>=limit)++level;return std::to_string(level)+"级";}
inline std::string Wind(int degrees){if(degrees<0)return "--";const char* names[]={"北风","东北风","东风","东南风","南风","西南风","西风","西北风"};return names[((degrees+22)/45)%8];}
inline std::string RegionName(const std::string& province,const std::string& city,const std::string& district){
    std::string prefix=city;
    if(prefix.empty()&&province.size()>=3&&province.compare(province.size()-3,3,"市")==0)prefix=province;
    if(district.empty()||district==prefix)return prefix;
    return prefix.empty()?district:prefix+"\n"+district;
}
inline const char* Weekday(const std::string& date){int y=0,m=0,d=0;if(sscanf(date.c_str(),"%d-%d-%d",&y,&m,&d)!=3||y<1900||y>9999||m<1||m>12||d<1||d>31)return "--";
    const int table[]={0,3,2,5,0,3,5,1,4,6,2,4};if(m<3)--y;const char* names[]={"周日","周一","周二","周三","周四","周五","周六"};return names[(y+y/4-y/100+y/400+table[m-1]+d)%7];}
inline bool ParseForecast(const std::string& raw,const std::string& id,const std::string& province,const std::string& city,const std::string& district,WeatherDistrictData& out){
    cJSON* root=cJSON_Parse(raw.c_str());if(!root)return false;
    auto* current=cJSON_GetObjectItemCaseSensitive(root,"current");auto* hourly=cJSON_GetObjectItemCaseSensitive(root,"hourly");auto* daily=cJSON_GetObjectItemCaseSensitive(root,"daily");
    double temp=0;const auto time=weather_json::JsonStr(current,"time");
    if(!Number(cJSON_GetObjectItemCaseSensitive(current,"temperature_2m"),temp)||temp< -90||temp>65||Num(current,"weather_code")<0||time.size()<16){cJSON_Delete(root);return false;}
    out=WeatherDistrictData{};out.country="中国";out.province=province;out.city=city;out.district=district;out.district_id=id;
    out.temp=Rounded(temp);out.text=Description(Num(current,"weather_code"));out.uptime=time;
    out.feels_like=Num(current,"apparent_temperature");out.rh=Num(current,"relative_humidity_2m");out.wind_angle=Num(current,"wind_direction_10m");out.wind_dir=Wind(out.wind_angle);out.wind_class=WindClass(Real(current,"wind_speed_10m"));
    out.prec1h=Real(current,"precipitation");out.clouds=Num(current,"cloud_cover");out.pressure=Num(current,"surface_pressure");
    out.aqi=out.pm25=out.pm10=out.no2=out.so2=out.o3=-1;out.co=-1;
    const int days=std::min(7,cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(daily,"time")));
    for(int i=0;i<days;++i){double high=0,low=0;if(!Number(At(daily,"temperature_2m_max",i),high)||!Number(At(daily,"temperature_2m_min",i),low))continue;
        WeatherForecastDay day;day.date=Text(At(daily,"time",i));day.week=Weekday(day.date);day.high=Rounded(high);day.low=Rounded(low);day.text_day=day.text_night=Description(Value(daily,"weather_code",i));
        day.wc_day=day.wc_night=WindClass(Value(daily,"wind_speed_10m_max",i));day.wd_day=day.wd_night=Wind(Value(daily,"wind_direction_10m_dominant",i));day.uvi=Value(daily,"uv_index_max",i);out.forecasts.push_back(std::move(day));}
    const int count=cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(hourly,"time"));int start=0;
    for(int i=0;i<count;++i)if(Text(At(hourly,"time",i)).substr(0,13)>=time.substr(0,13)){start=i;break;}
    for(int i=start;i<count&&out.forecast_hours.size()<48;++i){double value=0;if(!Number(At(hourly,"temperature_2m",i),value))continue;
        WeatherForecastHour hour;hour.data_time=Text(At(hourly,"time",i));hour.temp_fc=Rounded(value);hour.text=Description(Value(hourly,"weather_code",i));hour.wind_angle=Value(hourly,"wind_direction_10m",i);hour.wind_dir=Wind(hour.wind_angle);hour.wind_class=WindClass(Value(hourly,"wind_speed_10m",i));
        hour.rh=Value(hourly,"relative_humidity_2m",i);hour.pop=Value(hourly,"precipitation_probability",i);hour.prec1h=Value(hourly,"precipitation",i);hour.clouds=Value(hourly,"cloud_cover",i);hour.uvi=Value(hourly,"uv_index",i);hour.pressure=Value(hourly,"surface_pressure",i);hour.dpt=Value(hourly,"dew_point_2m",i);out.forecast_hours.push_back(std::move(hour));}
    out.vis=Value(hourly,"visibility",start);out.dpt=Value(hourly,"dew_point_2m",start);out.uvi=Value(hourly,"uv_index",start);
    out.indexes.push_back({"数据来源","Open-Meteo","设备直连 Open-Meteo。当前天气为模型估计；预警和生活指数未提供时不编造数据。"});out.valid=true;cJSON_Delete(root);return true;
}
inline void ParseAir(const std::string& raw,WeatherDistrictData& out){
    cJSON* root=cJSON_Parse(raw.c_str());if(!root)return;auto* current=cJSON_GetObjectItemCaseSensitive(root,"current");
    out.aqi=Num(current,"us_aqi");out.pm25=Num(current,"pm2_5");out.pm10=Num(current,"pm10");out.no2=Num(current,"nitrogen_dioxide");out.so2=Num(current,"sulphur_dioxide");out.o3=Num(current,"ozone");double co=Real(current,"carbon_monoxide");out.co=co>=0?co/1000:-1;
    if(!out.indexes.empty())out.indexes[0].detail+=" 空气质量为 CAMS / Open-Meteo 模型数据，AQI 采用美国标准。";cJSON_Delete(root);
}
bool Fetch(const std::string& id,WeatherDistrictData& out,std::string& error);
bool AddAir(const std::string& id,WeatherDistrictData& out);
}
