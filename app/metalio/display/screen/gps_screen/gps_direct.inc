// Device-side public service adapters. No services.url / LAN bridge dependency.
// Included inside gps_screen.cxx's anonymous namespace.
bool DirectJson(const char *service, const std::string &url,
                const std::string &payload, std::string &body, std::string &error) {
    Http http;
    http.SetDeadlineMs(20000);
    http.SetHeader("User-Agent", "HiOS-Location/1.0");
    http.SetHeader("Accept", "application/json");
    http.SetHeader("Connection", "close");
    if (!payload.empty()) { http.SetHeader("Content-Type", "application/json"); http.SetContent(payload); }
    if (!http.Open(payload.empty() ? "GET" : "POST", url)) {
        error = std::string(service) + "连接失败，错误码 " + std::to_string(http.GetLastError());
        return false;
    }
    const int status = http.GetStatusCode();
    body = ReadHttpTextBody(&http, 64 * 1024);
    if (status != 200 || body.empty()) {
        error = std::string(service) + " HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

bool DirectLocate(const std::string &request, LocationReportResult *res) {
    cJSON *input = cJSON_Parse(request.c_str());
    if (!input) { res->err = "定位参数无效"; return false; }
    const int access = static_cast<int>(JsonItemNumber(cJSON_GetObjectItemCaseSensitive(input,"accesstype"), -1));
    bool valid = false;
    if (access == 2) {
        auto *gps = cJSON_GetObjectItemCaseSensitive(input,"gps");
        valid = cJSON_IsString(gps) && std::sscanf(gps->valuestring,"%lf|%lf", &res->longitude, &res->latitude) == 2;
    } else {
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddBoolToObject(payload,"considerIp",false);
        if (access == 1) {
            auto *macs = cJSON_GetObjectItemCaseSensitive(input,"macs");
            cJSON *aps = cJSON_AddArrayToObject(payload,"wifiAccessPoints");
            if (cJSON_IsString(macs)) {
                std::string values(macs->valuestring);
                size_t start = 0;
                int count = 0;
                while (start < values.size() && count < 32) {
                    char mac[18]{}; int signal = 0;
                    if (std::sscanf(values.c_str()+start,"%17[^,],%d",mac,&signal)==2 && std::strlen(mac)==17 && signal>=-128 && signal<=0) {
                        auto *ap=cJSON_CreateObject(); cJSON_AddStringToObject(ap,"macAddress",mac);
                        cJSON_AddNumberToObject(ap,"signalStrength",signal); cJSON_AddItemToArray(aps,ap); ++count;
                    }
                    auto end=values.find('|',start); if(end==std::string::npos)break; start=end+1;
                }
            }
            valid=cJSON_GetArraySize(aps)>0;
        } else if (access == 0) {
            auto *bts=cJSON_GetObjectItemCaseSensitive(input,"bts");
            unsigned mcc,mnc,lac,cell;int signal;
            if(cJSON_IsString(bts) && std::sscanf(bts->valuestring,"%u,%u,%u,%u,%d",&mcc,&mnc,&lac,&cell,&signal)==5) {
                auto *towers=cJSON_AddArrayToObject(payload,"cellTowers");auto *tower=cJSON_CreateObject();
                cJSON_AddStringToObject(payload,"radioType","lte");
                cJSON_AddNumberToObject(tower,"mobileCountryCode",mcc);cJSON_AddNumberToObject(tower,"mobileNetworkCode",mnc);
                cJSON_AddNumberToObject(tower,"locationAreaCode",lac);cJSON_AddNumberToObject(tower,"cellId",cell);
                cJSON_AddNumberToObject(tower,"signalStrength",signal);cJSON_AddItemToArray(towers,tower);valid=true;
            }
        }
        char *encoded=cJSON_PrintUnformatted(payload);
        std::string response;
        if(valid && encoded) valid=DirectJson("beaconDB", "https://api.beacondb.net/v1/geolocate", encoded,response,res->err);
        else valid=false;
        cJSON_free(encoded);cJSON_Delete(payload);
        if(valid) {
            auto *root=cJSON_Parse(response.c_str());auto *loc=cJSON_GetObjectItemCaseSensitive(root,"location");
            res->latitude=JsonItemNumber(cJSON_GetObjectItemCaseSensitive(loc,"lat"),__builtin_nan(""));
            res->longitude=JsonItemNumber(cJSON_GetObjectItemCaseSensitive(loc,"lng"),__builtin_nan(""));
            res->accuracy=JsonItemNumber(cJSON_GetObjectItemCaseSensitive(root,"accuracy"),0);
            cJSON_Delete(root);
        }
    }
    cJSON_Delete(input);
    valid=valid && !GpsIsNan(res->latitude) && !GpsIsInf(res->latitude) && GpsFabs(res->latitude)<=90 &&
                  !GpsIsNan(res->longitude) && !GpsIsInf(res->longitude) && GpsFabs(res->longitude)<=180;
    if(!valid) { if(res->err.empty())res->err="定位数据无效或服务未覆盖该区域";return false; }
    res->ok=true;
    char url[200];std::snprintf(url,sizeof(url),"https://photon.komoot.io/reverse?lat=%.7f&lon=%.7f&limit=1&lang=en",res->latitude,res->longitude);
    std::string response,error;
    res->address="地址查询暂不可用";
    if(DirectJson("Photon",url,"",response,error)) {
        auto *root=cJSON_Parse(response.c_str());auto *features=cJSON_GetObjectItemCaseSensitive(root,"features");
        auto *properties=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(features,0),"properties");
        std::string address;
        const char *keys[]={"country","state","city","district","street","name"};
        for(const char *key : keys) {
            auto *v=cJSON_GetObjectItemCaseSensitive(properties,key);
            if(cJSON_IsString(v) && v->valuestring && *v->valuestring) { if(!address.empty())address+=" · ";address+=v->valuestring; }
        }
        if(!address.empty())res->address=address;
        cJSON_Delete(root);
    }
    return true;
}

bool DownloadMapForResult(void *, const std::string &url, LocationReportResult *res) {
    double lat,lon;int zoom,width,height;
    if(std::sscanf(url.c_str(),"hios-map://%lf/%lf/%d/%d/%d",&lat,&lon,&zoom,&width,&height)!=5 ||
       GpsIsNan(lat)||GpsIsInf(lat)||GpsIsNan(lon)||GpsIsInf(lon)||GpsFabs(lat)>90||GpsFabs(lon)>180||
       zoom<3||zoom>18||width<1||height<1||width>672||height>672) {res->map_err="地图参数无效";return false;}
    // Persistent cache is mandatory: do not repeatedly download tiles if SD is unavailable.
    if(metalio_sdcard_ensure_rw()<0) {res->map_err="地图缓存需要可写SD卡";return false;}
    mkdir("/sdcard/mapcache",0777);
    const int count=1<<zoom;
    const double pi=3.141592653589793;
    lat=std::max(-85.05112878,std::min(85.05112878,lat));
    double sine=__builtin_sin(lat*pi/180);
    const double cx=(lon+180)/360*count*256;
    const double cy=(.5-__builtin_log((1+sine)/(1-sine))/(4*pi))*count*256;
    const int left=static_cast<int>(__builtin_floor(cx-width/2.0)),top=static_cast<int>(__builtin_floor(cy-height/2.0));
    const size_t size=static_cast<size_t>(width)*height*2;
    auto *pixels=static_cast<uint16_t*>(malloc(size));
    if(!pixels){res->map_err="地图内存不足";return false;}
    std::memset(pixels,0xDF,size);
    bool ok=true;
    for(int ty=static_cast<int>(__builtin_floor(top/256.0));ty<=static_cast<int>(__builtin_floor((top+height-1)/256.0)) && ok;++ty)
    for(int tx=static_cast<int>(__builtin_floor(left/256.0));tx<=static_cast<int>(__builtin_floor((left+width-1)/256.0)) && ok;++tx) {
        if(res->session!=s_location_session.load()){res->map_err="地图请求已取消";ok=false;break;}
        if(ty<0||ty>=count)continue;
        int wrapped=(tx%count+count)%count;
        char path[120],remote[180];std::snprintf(path,sizeof(path),"/sdcard/mapcache/%d-%d-%d.png",zoom,wrapped,ty);
        std::snprintf(remote,sizeof(remote),"https://tile.openstreetmap.org/%d/%d/%d.png",zoom,wrapped,ty);
        std::vector<unsigned char> bytes;
        struct stat st{};
        if(stat(path,&st)==0 && st.st_size>0 && st.st_size<=512*1024 && std::time(nullptr)-st.st_mtime<7*86400) {
            FILE *f=::fopen(path,"rb");if(f){bytes.resize(st.st_size);if(::fread(bytes.data(),1,bytes.size(),f)!=bytes.size())bytes.clear();::fclose(f);}
        }
        if(bytes.empty()) {
            Http http;http.SetDeadlineMs(15000);http.SetHeader("User-Agent","HiOS-Location/1.0");http.SetHeader("Connection","close");
            if(!http.Open("GET",remote)){res->map_err="OSM连接失败，错误码 "+std::to_string(http.GetLastError());ok=false;break;}
            if(http.GetStatusCode()!=200){res->map_err="OSM HTTP "+std::to_string(http.GetStatusCode());ok=false;break;}
            auto body=ReadHttpTextBody(&http,512*1024);bytes.assign(body.begin(),body.end());
            if(bytes.empty()){res->map_err="地图瓦片为空";ok=false;break;}
            // Atomic cache publication, so power loss never leaves a valid-looking partial tile.
            std::string temp=std::string(path)+".tmp";FILE *f=::fopen(temp.c_str(),"wb");
            bool saved=false;if(f){saved=::fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();if(::fclose(f)!=0)saved=false;}
            if(!saved || ::rename(temp.c_str(),path)!=0){::remove(temp.c_str());res->map_err="地图缓存写入失败";ok=false;break;}
        }
        unsigned w=0,h=0;unsigned char *decoded=nullptr;
        // Reject oversized PNGs before the decoder allocates pixels.
        auto be=[](const unsigned char *p){return (unsigned(p[0])<<24)|(unsigned(p[1])<<16)|(unsigned(p[2])<<8)|p[3];};
        if(bytes.size()<24 || be(bytes.data()+16)!=256 || be(bytes.data()+20)!=256){res->map_err="地图瓦片尺寸无效";ok=false;break;}
        LodePNGState state;lodepng_state_init(&state);state.info_raw.colortype=LCT_RGBA;state.info_raw.bitdepth=8;
        state.decoder.zlibsettings.max_output_size=256*256*8+4096;
        unsigned error=lodepng_decode(&decoded,&w,&h,&state,bytes.data(),bytes.size());lodepng_state_cleanup(&state);
        auto *draw=reinterpret_cast<lv_draw_buf_t*>(decoded);
        if(error||!draw||w!=256||h!=256){if(draw)lv_draw_buf_destroy(draw);::remove(path);res->map_err="地图瓦片解码失败";ok=false;break;}
        for(int y=std::max(top,ty*256);y<std::min(top+height,ty*256+256);++y)
        for(int x=std::max(left,tx*256);x<std::min(left+width,tx*256+256);++x) {
            auto *p=draw->data+(y-ty*256)*draw->header.stride+(x-tx*256)*4;
            pixels[(y-top)*width+x-left]=uint16_t(((p[0]>>3)<<11)|((p[1]>>2)<<5)|(p[2]>>3));
        }
        lv_draw_buf_destroy(draw);
    }
    if(!ok){free(pixels);return false;}
    // Mark the actual coordinate (the tile grid is not centred on its own).
    for(int y=height/2-5;y<=height/2+5;++y)for(int x=width/2-5;x<=width/2+5;++x)
        if(x>=0&&y>=0&&x<width&&y<height)pixels[y*width+x]=0xF800;
    res->map_data=pixels;res->map_size=size;res->map_width=width;res->map_height=height;return true;
}
