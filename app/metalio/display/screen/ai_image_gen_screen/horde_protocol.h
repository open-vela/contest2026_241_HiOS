// SPDX-License-Identifier: Apache-2.0
// AI Horde wire contract, independent of LVGL and the LAN feature adapter.
#pragma once
#include <string>
#include <vector>
#include "cJSON_compat.h"

namespace HiHorde {
constexpr const char* kBase = "https://aihorde.net/api/v2/generate/";
inline bool ValidId(const std::string& id) {
    if (id.size() != 36) return false;
    for (size_t i=0;i<id.size();++i) {
        const char c=id[i];
        if (i==8 || i==13 || i==18 || i==23) { if(c!='-') return false; }
        else if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return false;
    }
    return true;
}
inline std::string Url(const char* action,const std::string& id) {
    return std::string(kBase)+action+"/"+id;
}
inline std::string Request(const std::string& prompt,int count) {
    if(prompt.empty() || prompt.size()>2048 || count<1 || count>4) return {};
    cJSON* root=cJSON_CreateObject();
    if(!root) return {};
    cJSON_AddStringToObject(root,"prompt",prompt.c_str());
    cJSON* params=cJSON_AddObjectToObject(root,"params");
    if(!params){cJSON_Delete(root);return {};}
    cJSON_AddNumberToObject(params,"width",512);
    cJSON_AddNumberToObject(params,"height",512);
    cJSON_AddNumberToObject(params,"steps",20);
    cJSON_AddNumberToObject(params,"n",count);
    cJSON_AddStringToObject(params,"sampler_name","k_euler");
    cJSON_AddBoolToObject(root,"nsfw",false);
    cJSON_AddBoolToObject(root,"censor_nsfw",true);
    cJSON_AddBoolToObject(root,"r2",true);
    cJSON_AddBoolToObject(root,"shared",false);
    char* raw=cJSON_PrintUnformatted(root);cJSON_Delete(root);
    std::string out=raw?raw:"";cJSON_free(raw);return out;
}
inline std::string Message(cJSON* root,const char* fallback) {
    cJSON* message=cJSON_GetObjectItemCaseSensitive(root,"message");
    return cJSON_IsString(message)&&message->valuestring?std::string(message->valuestring).substr(0,240):fallback;
}
inline bool Created(const std::string& body,std::string& id,std::string& error) {
    cJSON* root=cJSON_Parse(body.c_str());
    cJSON* item=cJSON_GetObjectItemCaseSensitive(root,"id");
    id=cJSON_IsString(item)&&item->valuestring?item->valuestring:"";
    const bool ok=ValidId(id);
    if(!ok){error=Message(root,"生图响应缺少有效任务编号");id.clear();}
    cJSON_Delete(root);return ok;
}
struct Status {
    bool ok=false,finished=false,succeeded=false;
    std::vector<std::string> image_urls;
    std::string err;
};
inline Status Parse(const std::string& body,bool results=false) {
    Status out;cJSON* root=cJSON_Parse(body.c_str());
    cJSON* done=cJSON_GetObjectItemCaseSensitive(root,"done");
    cJSON* fault=cJSON_GetObjectItemCaseSensitive(root,"faulted");
    if(!cJSON_IsBool(done)||!cJSON_IsBool(fault)) {
        out.err=Message(root,"任务状态解析失败");cJSON_Delete(root);return out;
    }
    out.ok=true;out.finished=cJSON_IsTrue(done)||cJSON_IsTrue(fault);
    if(cJSON_IsTrue(fault))out.err="AI Horde 生成失败";
    else if(results&&out.finished) {
        cJSON* list=cJSON_GetObjectItemCaseSensitive(root,"generations");
        if(cJSON_IsArray(list))for(int i=0;i<cJSON_GetArraySize(list)&&i<4;++i) {
            cJSON* generation=cJSON_GetArrayItem(list,i);
            if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(generation,"censored")))continue;
            cJSON* img=cJSON_GetObjectItemCaseSensitive(generation,"img");
            if(cJSON_IsString(img)&&img->valuestring) {
                std::string url=img->valuestring;
                if(url.size()<=2048 && url.compare(0,8,"https://")==0 &&
                   url.find_first_of("\r\n \t")==std::string::npos)out.image_urls.push_back(url);
            }
        }
        out.succeeded=!out.image_urls.empty();
        if(!out.succeeded)out.err="没有可显示的图片";
    }
    cJSON_Delete(root);return out;
}
} // namespace HiHorde
