#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include "cJSON_compat.h"
namespace HiImagePrompt {
inline bool Chinese(const std::string& text){
    for(size_t i=0;i+2<text.size();++i){const auto a=(unsigned char)text[i],b=(unsigned char)text[i+1],c=(unsigned char)text[i+2];
        if(a>=0xe4&&a<=0xe9&&(b&0xc0)==0x80&&(c&0xc0)==0x80){unsigned code=((a&15)<<12)|((b&63)<<6)|(c&63);if(code>=0x4e00&&code<=0x9fff)return true;}}
    return false;
}
inline std::string Escape(const std::string& text){const char*hex="0123456789ABCDEF";std::string out;
    for(unsigned char c:text){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out.push_back(c);else{out.push_back('%');out.push_back(hex[c>>4]);out.push_back(hex[c&15]);}}return out;}
inline std::vector<std::string> Chunks(const std::string& text){std::vector<std::string> chunks;
    for(size_t at=0;at<text.size();){size_t end=std::min(text.size(),at+450);while(end<text.size()&&end>at&&((unsigned char)text[end]&0xc0)==0x80)--end;if(end==at)return {};chunks.push_back(text.substr(at,end-at));at=end;}
    return chunks;
}
inline bool Translation(const std::string& raw,std::string& output){
    cJSON*root=cJSON_Parse(raw.c_str());if(!root)return false;auto*status=cJSON_GetObjectItemCaseSensitive(root,"responseStatus");auto*data=cJSON_GetObjectItemCaseSensitive(root,"responseData");auto*text=cJSON_GetObjectItemCaseSensitive(data,"translatedText");
    bool ok=cJSON_IsNumber(status)&&status->valueint==200&&cJSON_IsString(text)&&text->valuestring&&text->valuestring[0];if(ok)output=text->valuestring;cJSON_Delete(root);
    if(ok){const char*entities[]={"&quot;","&#39;","&amp;","&lt;","&gt;"};const char*values[]={"\"","'","&","<",">"};for(int i=0;i<5;++i){size_t at=0;while((at=output.find(entities[i],at))!=output.npos){output.replace(at,std::char_traits<char>::length(entities[i]),values[i]);at+=std::char_traits<char>::length(values[i]);}}}
    return ok;
}
}
