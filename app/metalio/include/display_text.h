/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <string>
#include <cstddef>
#include <cstdint>
// Text paths and filenames are never changed. This is display-only UTF-8
// normalization, including complete-codepoint truncation for fixed UI buffers.
inline std::string DisplayUtf8(const std::string &text,size_t max_bytes=4096){
    std::string out;out.reserve(text.size()<max_bytes?text.size():max_bytes);
    for(size_t p=0;p<text.size();){
        unsigned char c=text[p];size_t n=c<0x80?1:c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
        bool valid=n&&p+n<=text.size();uint32_t cp=n?c&((1u<<(7-n+(n==1)))-1):0;
        if(n==1)cp=c;
        for(size_t i=1;valid&&i<n;++i){unsigned char b=text[p+i];if((b&0xc0)!=0x80)valid=false;cp=(cp<<6)|(b&63);}
        if(valid&&((n==2&&cp<0x80)||(n==3&&cp<0x800)||(n==4&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)))valid=false;
        if(!valid){++p;continue;}p+=n;
        if(cp==0xfeff||cp==0x200b||cp==0x200c||cp==0x200d||cp<32||cp==127)continue;
        char replacement=0;
        if(cp>=0xff01&&cp<=0xff5e)replacement=cp-0xfee0;
        if(cp==0xa0||cp==0x3000)replacement=' ';
        if(cp>=0x2010&&cp<=0x2014)replacement='-';
        if(cp==0x2018||cp==0x2019)replacement='\'';
        if(cp==0x201c||cp==0x201d)replacement='"';
        if(replacement){if(out.size()+1>max_bytes)break;out+=replacement;}
        else {if(out.size()+n>max_bytes)break;out.append(text,p-n,n);}
    }
    return out;
}
