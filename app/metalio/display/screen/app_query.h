#pragma once
#include <string>
#include <cstring>
namespace HiAppQuery {
inline std::string Key(const char* text) {
    std::string key;
    if(!text)return key;
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(text);*p;++p) {
        if(*p<=0x20)continue;
        key.push_back(*p>='A'&&*p<='Z'?*p-'A'+'a':*p);
    }
    const char* prefixes[]={"请帮我打开","帮我打开","给我打开","我想打开","请打开","去打开","打开","开启","启动","进入"};
    for(auto* prefix:prefixes){size_t n=std::strlen(prefix);if(key.compare(0,n,prefix)==0){key.erase(0,n);break;}}
    // ASR may append punctuation after the word app/software; remove both.
    const char* tails[]={"。","，","！","？","!",".",",","?","应用程序","应用","软件","app"};
    bool removed=true;
    while(removed){removed=false;for(auto* tail:tails){size_t n=std::strlen(tail);if(key.size()>n&&key.compare(key.size()-n,n,tail)==0){key.resize(key.size()-n);removed=true;break;}}}
    return key;
}
}
