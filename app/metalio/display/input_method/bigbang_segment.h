// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "pinyin_engine.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hios { namespace bigbang {
struct Token { size_t begin,end; };
inline size_t Next(const std::string&s,size_t at){
    size_t end=at+1;while(end<s.size()&&(static_cast<unsigned char>(s[end])&0xc0)==0x80)++end;return end;
}
// User requested no additional dictionary. Reuse the installed input-method
// phrase splitter; keep unrecognized Chinese/emoji at valid UTF8 boundaries.
// A separate character mode lets the user select within any existing phrase.
inline std::vector<Token> Split(const std::string&s,bool characters=false){
    std::vector<Token> result;result.reserve(s.size()/3+1);
    for(size_t at=0;at<s.size();){
        size_t end=Next(s,at);unsigned char ch=s[at];
        if(!characters){
            if(ch<128&&((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9'))){
                while(end<s.size()){unsigned char v=s[end];if(!((v>='a'&&v<='z')||(v>='A'&&v<='Z')||(v>='0'&&v<='9')||v=='_'||v=='-'||v=='@'||v=='.'))break;++end;}
            }else if(ch>=128){size_t bytes=PinyinEngine::SegmentBytes(s.c_str()+at);if(bytes)end=at+std::min(bytes,s.size()-at);}
        }
        result.push_back({at,end});at=end;
    }return result;
}
} }
