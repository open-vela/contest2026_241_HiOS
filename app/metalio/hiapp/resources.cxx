// SPDX-License-Identifier: Apache-2.0
#include "resources.h"
#include "hiapp.h"
#include "../audio/esp-gmf/nuttx_shims/miniz.h"
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <initializer_list>
namespace HiApp {
namespace {
uint32_t Le(const std::string&s,size_t at,int n){if(at+n>s.size())return 0;uint32_t v=0;for(int k=0;k<n;k++)v|=(uint32_t)(unsigned char)s[at+k]<<(8*k);return v;}
void Utf8(std::string&o,uint32_t c){if(!c||c>0x10ffff||(c>=0xd800&&c<=0xdfff))c=0xfffd;if(c<128)o.push_back(c);else if(c<2048){o.push_back(0xc0|(c>>6));o.push_back(0x80|(c&63));}else if(c<65536){o.push_back(0xe0|(c>>12));o.push_back(0x80|((c>>6)&63));o.push_back(0x80|(c&63));}else{o.push_back(0xf0|(c>>18));o.push_back(0x80|((c>>12)&63));o.push_back(0x80|((c>>6)&63));o.push_back(0x80|(c&63));}}
std::string Label(std::string s){s.erase(std::remove_if(s.begin(),s.end(),[](char c){return c==' '||c=='\t'||c=='"'||c=='\'';}),s.end());for(char&c:s)if(c>='A'&&c<='Z')c+=32;return s;}
}
bool InflateBody(const std::string&in,const std::string&encoding,std::string&out,size_t limit){
 if(encoding.empty()||encoding=="identity"){if(in.size()>limit)return false;out=in;return true;}
 size_t start=0,end=in.size();int flags=0;bool gzip=encoding=="gzip"||encoding=="x-gzip";
 if(gzip){if(in.size()<18||(unsigned char)in[0]!=31||(unsigned char)in[1]!=139||in[2]!=8)return false;unsigned f=(unsigned char)in[3];if(f&0xe0)return false;start=10;end-=8;if(f&4){if(start+2>end)return false;size_t n=Le(in,start,2);start+=2+n;}for(int b:{8,16})if(f&b){while(start<end&&in[start])++start;++start;}if(f&2)start+=2;if(start>=end||Le(in,end+4,4)>limit)return false;}
 else if(encoding=="deflate")flags=TINFL_FLAG_PARSE_ZLIB_HEADER;else return false;
 out.resize(limit);size_t n=tinfl_decompress_mem_to_mem(&out[0],limit,in.data()+start,end-start,flags);
 if(n==(size_t)-1&&encoding=="deflate")n=tinfl_decompress_mem_to_mem(&out[0],limit,in.data(),in.size(),0);
 if(n==(size_t)-1||n>limit){out.clear();return false;}out.resize(n);
 if(gzip&&(Le(in,end+4,4)!=n||Le(in,end,4)!=Crc32(out.data(),out.size()))){out.clear();return false;}return true;
}
bool UnpackResource(const std::string&data,const std::string&encoding,size_t expected,std::string&out){
 if(expected>192*1024||data.size()>384*1024)return false;
 std::string bin;bin.reserve(data.size()*3/4);uint32_t bits=0;int have=0;bool pad=false;
 for(unsigned char c:data){if(c=='='){pad=true;continue;}if(c=='\r'||c=='\n')continue;if(pad)return false;int d=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='+'?62:c=='/'?63:-1;if(d<0)return false;bits=(bits<<6)|d;have+=6;if(have>=8){have-=8;bin.push_back((bits>>have)&255);}}
 if(encoding=="base64")out=std::move(bin);else if(encoding=="base64+zlib"){if(!InflateBody(bin,"deflate",out,expected?expected:1))return false;}else return false;return out.size()==expected;
}
std::string DecodeWebText(const Package&p,const std::string&body,const std::string&encoding,std::string&error){
 std::string enc=Label(encoding),out;size_t i=0;
 if(body.size()>=3&&body.compare(0,3,"\xef\xbb\xbf")==0){enc="utf-8";i=3;}
 else if(body.size()>=2&&(unsigned char)body[0]==255&&(unsigned char)body[1]==254){enc="utf-16le";i=2;}
 else if(body.size()>=2&&(unsigned char)body[0]==254&&(unsigned char)body[1]==255){enc="utf-16be";i=2;}
 if(enc=="utf8"||enc=="unicode-1-1-utf-8"||enc.empty())enc="utf-8";
 if(enc=="utf-16"||enc=="utf16"||enc=="unicode")enc="utf-16le";
 bool gb=enc=="gbk"||enc=="gb2312"||enc=="gb_2312-80"||enc=="x-gbk"||enc=="gb18030";bool big=enc=="big5"||enc=="big5-hkscs"||enc=="cn-big5"||enc=="x-x-big5";
 bool latin=enc=="windows-1252"||enc=="cp1252"||enc=="iso-8859-1"||enc=="latin1"||enc=="us-ascii"||enc=="ascii";
 bool utf16=enc=="utf-16le"||enc=="utf-16be";
 if(!gb&&!big&&!latin&&!utf16&&enc!="utf-8"){error="暂不支持网页编码："+enc;return {};}
 const std::string*table=nullptr,*ranges=nullptr;if(gb||big){auto t=p.files.find(gb?"assets/gb18030.bin":"assets/big5.bin");if(t==p.files.end()){error="安装包缺少字符映射资源，请更新应用";return {};}table=&t->second;auto r=p.files.find("assets/gb18030-ranges.bin");if(r!=p.files.end())ranges=&r->second;}
 static const uint16_t win[]={0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,0x90,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
 while(i<body.size()){
  uint32_t c=(unsigned char)body[i++];
  if(utf16){if(i>=body.size()){Utf8(out,0xfffd);break;}unsigned a=c,b=(unsigned char)body[i++];c=enc=="utf-16le"?a|(b<<8):(a<<8)|b;
   if(c>=0xd800&&c<=0xdbff&&i+1<body.size()){unsigned n=enc=="utf-16le"?Le(body,i,2):((unsigned char)body[i]<<8)|(unsigned char)body[i+1];if(n>=0xdc00&&n<=0xdfff){c=0x10000+((c-0xd800)<<10)+n-0xdc00;i+=2;}}Utf8(out,c);
  }else if(enc=="utf-8"){
   if(c<128)Utf8(out,c);else {int more=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;uint32_t v=more?c&((1u<<(6-more))-1):0;bool valid=more&&i+more<=body.size();for(int k=0;valid&&k<more;k++){unsigned x=(unsigned char)body[i+k];if((x&0xc0)!=0x80)valid=false;else v=(v<<6)|(x&63);}if(valid&&(v<(more==1?0x80u:more==2?0x800u:0x10000u)||v>0x10ffff||(v>=0xd800&&v<=0xdfff)))valid=false;if(valid){i+=more;Utf8(out,v);}else Utf8(out,0xfffd);}
  }else if(latin)Utf8(out,c>=0x80&&c<=0x9f?win[c-0x80]:c);
  else if(c<128)Utf8(out,c);
  else if(gb&&c==0x80)Utf8(out,0x20ac);
  else if(c>=0x81&&c<=0xfe&&i<body.size()){
   unsigned b=(unsigned char)body[i];uint32_t v=0;
   if(gb&&b>=0x30&&b<=0x39&&i+2<body.size()&&(unsigned char)body[i+1]>=0x81&&(unsigned char)body[i+1]<=0xfe&&(unsigned char)body[i+2]>=0x30&&(unsigned char)body[i+2]<=0x39){uint32_t pointer=(((c-0x81)*10+b-0x30)*126+(unsigned char)body[i+1]-0x81)*10+(unsigned char)body[i+2]-0x30;i+=3;
    if(ranges&&!(pointer>39419&&pointer<189000)&&pointer<=1237575){size_t n=ranges->size()/8,lo=0,hi=n;while(lo<hi){size_t m=(lo+hi)/2;if(Le(*ranges,m*8,4)<=pointer)lo=m+1;else hi=m;}if(lo)v=Le(*ranges,(lo-1)*8+4,4)+pointer-Le(*ranges,(lo-1)*8,4);if(pointer==7457)v=0xe7c7;}
   }else {int pointer=-1;if(gb&&b>=0x40&&b<=0xfe&&b!=0x7f)pointer=(c-0x81)*190+b-(b<0x7f?0x40:0x41);else if(big&&((b>=0x40&&b<=0x7e)||(b>=0xa1&&b<=0xfe)))pointer=(c-0x81)*157+b-(b<0x7f?0x40:0x62);if(pointer>=0){++i;v=Le(*table,pointer*3,3);if(big&&(pointer==1133||pointer==1135||pointer==1164||pointer==1166)){Utf8(out,pointer<1160?0xca:0xea);v=(pointer==1133||pointer==1164)?0x304:0x30c;}}}
   Utf8(out,v?v:0xfffd);
  }else Utf8(out,0xfffd);
  if(out.size()>384*1024){error="转换后的网页过大";return {};}
 }return out;
}
}
