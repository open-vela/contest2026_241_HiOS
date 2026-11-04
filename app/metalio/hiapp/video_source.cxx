// SPDX-License-Identifier: Apache-2.0
#include "video_source.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
namespace HiApp { namespace VideoSource {
bool HttpUrl(const std::string&s){return s.size()<=2048&&(s.compare(0,7,"http://")==0||s.compare(0,8,"https://")==0)&&s.find_first_of("\r\n\t ")==s.npos&&s.find('@')==s.npos&&s.find('\0')==s.npos;}
bool SdPath(const std::string&s){if(s!="/sdcard"&&s.compare(0,8,"/sdcard/"))return false;if(s.size()>512||s.find('\0')!=s.npos||s.find('\\')!=s.npos)return false;size_t at=1;while(at<s.size()){auto e=s.find('/',at);auto p=s.substr(at,e==s.npos?s.npos:e-at);if(p.empty()||p=="."||p=="..")return false;if(e==s.npos)break;at=e+1;}return s.size()==7||s.back()!='/';}
bool VideoPath(const std::string&s){if(!SdPath(s)||s.size()<13)return false;auto first=s.substr(8,5);for(char&c:first)if(c>='A'&&c<='Z')c+=32;return first=="video"&&(s.size()==13||s[13]=='/');}
bool QrUrl(const uint8_t*p,size_t n,std::string&url){if(!p||!n||n>1100)return false;size_t first=0,last=n;auto space=[](uint8_t c){return c==' '||c=='\r'||c=='\n'||c=='\t';};while(first<last&&space(p[first]))++first;while(last>first&&space(p[last-1]))--last;if(last-first>1024)return false;std::string value((const char*)p+first,last-first);if(!HttpUrl(value))return false;url=std::move(value);return true;}
std::string Resolve(const std::string&base,const std::string&target){
 if(HttpUrl(target))return target;if(target.empty())return {};
 auto protocol=base.find("://");if(protocol==base.npos)return {};
 if(target.compare(0,2,"//")==0)return base.substr(0,protocol+1)+target;
 if(target.find(':')!=target.npos)return {};
 auto slash=base.find('/',protocol+3);std::string origin=base.substr(0,slash);
 std::string path;if(target[0]=='/')path=target;else{auto b=base.substr(0,base.find_first_of("?#"));auto end=b.rfind('/');path=(end<protocol+3?"/":b.substr(slash,end-slash+1))+target;}
 const auto suffix_at=path.find_first_of("?#");auto suffix=suffix_at==path.npos?std::string():path.substr(suffix_at);path=path.substr(0,suffix_at);
 std::vector<std::string>parts;size_t at=0;while(at<path.size()){auto end=path.find('/',at);auto p=path.substr(at,end==path.npos?path.npos:end-at);if(p==".."){if(!parts.empty())parts.pop_back();}else if(!p.empty()&&p!=".")parts.push_back(p);if(end==path.npos)break;at=end+1;}
 std::string result=origin;for(const auto&p:parts)result+="/"+p;if(parts.empty())result+="/";result+=suffix;return HttpUrl(result)?result:std::string();
}
static std::string Attribute(const std::string&s,const std::string&key){auto a=s.find(key+"=");if(a==s.npos)return {};a+=key.size()+1;bool quote=a<s.size()&&s[a]=='"';if(quote)++a;auto e=s.find(quote?'"':',',a);return s.substr(a,e==s.npos?s.npos:e-a);}
bool ParsePlaylist(const std::string&text,const std::string&base,uint64_t after,uint32_t seek,Playlist&out,std::string&error){
 out=Playlist{};if(text.size()>192*1024||text.compare(0,7,"#EXTM3U")){error="无效或过大的m3u8播放列表";return false;}
 out.live=text.find("#EXT-X-ENDLIST")==text.npos;
 uint64_t seq=0;uint32_t elapsed=0,duration=0;bool discontinuity=false,variant=false;unsigned lowest=~0u,lines=0,count=0;size_t at=0;
 std::vector<Segment>tail;
 while(at<text.size()){
  if(++lines>8192){error="播放列表条目超过上限";return false;}
  auto end=text.find('\n',at);auto line=text.substr(at,end==text.npos?text.npos:end-at);at=end==text.npos?text.size():end+1;
  if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty())continue;
  const std::string sequence_tag="#EXT-X-MEDIA-SEQUENCE:";
  if(line.compare(0,sequence_tag.size(),sequence_tag)==0){char*last=nullptr;const char*first=line.c_str()+sequence_tag.size();seq=strtoull(first,&last,10);if(*first<'0'||*first>'9'||!last||*last||seq>UINT64_MAX-4096){error="无效分段序号";return false;}continue;}
  if(line.compare(0,11,"#EXT-X-KEY:")==0&&Attribute(line,"METHOD")!="NONE"){error="首版不支持加密HLS，请使用未加密视频流";return false;}
  if(line.compare(0,11,"#EXT-X-MAP:")==0||line.compare(0,17,"#EXT-X-BYTERANGE:")==0){error="首版仅支持普通TS分段，不支持fMP4或字节范围分段";return false;}
  if(line=="#EXT-X-DISCONTINUITY"){discontinuity=true;continue;}
  if(line.compare(0,8,"#EXTINF:")==0){char*last=nullptr;double seconds=strtod(line.c_str()+8,&last);if(seconds!=seconds||seconds<=0||seconds>120||!last||(*last&&*last!=',')){error="无效分段时长";return false;}duration=(uint32_t)(seconds*1000+.5);if(!duration){error="分段时长过短";return false;}continue;}
  if(line.compare(0,18,"#EXT-X-STREAM-INF:")==0){variant=true;auto res=Attribute(line,"RESOLUTION");unsigned w=0,h=0;if(!res.empty()){auto x=res.find('x');if(x!=res.npos){w=atoi(res.c_str());h=atoi(res.c_str()+x+1);}}unsigned bandwidth=(unsigned)strtoul(Attribute(line,"BANDWIDTH").c_str(),nullptr,10);auto codecs=Attribute(line,"CODECS");bool compatible=(!w||w<=640)&&(!h||h<=368)&&(!w||!h||w*h<=640*368)&&(codecs.empty()||codecs.find("avc1.42")!=codecs.npos);duration=(compatible&&bandwidth<lowest)?(bandwidth?bandwidth:1):~0u;continue;}
  if(line[0]=='#')continue;auto url=Resolve(base,line);if(url.empty()){error="分段网址无效";return false;}
  if(variant){if(duration!=~0u){lowest=duration;out.variant=url;}variant=false;duration=0;continue;}
  if(!duration){error="分段缺少时长信息";return false;}if(++count>4096||elapsed>UINT32_MAX-duration){error="播放列表超过保守上限";return false;}
  Segment s;s.url=std::move(url);s.sequence=seq++;s.begin=elapsed;s.duration=duration;s.discontinuity=discontinuity;
  if(out.live&&!after){tail.push_back(s);if(tail.size()>3)tail.erase(tail.begin());}
  else if((!after||s.sequence>=after)&&s.begin+s.duration>seek&&out.segments.size()<32)out.segments.push_back(s);
  elapsed+=duration;duration=0;discontinuity=false;
 }
 out.duration=elapsed;if(out.live&&!after)out.segments=std::move(tail);
 if(!out.variant.empty())return true;if(!count){error="播放列表没有可播放的视频分段";return false;}return true;
}
bool AvccConfig(const uint8_t*p,size_t n,std::vector<uint8_t>&out,unsigned&length){
 if(n<7||p[0]!=1)return false;length=(p[4]&3)+1;size_t at=6;out.clear();unsigned count=p[5]&31;
 for(int group=0;group<2;group++){if(group){if(at>=n)return false;count=p[at++];}for(unsigned i=0;i<count;i++){if(at+2>n)return false;size_t size=((unsigned)p[at]<<8)|p[at+1];at+=2;if(!size||size>n-at||out.size()+size+4>16384)return false;out.push_back(0);out.push_back(0);out.push_back(0);out.push_back(1);out.insert(out.end(),p+at,p+at+size);at+=size;}}
 return !out.empty();
}
bool AnnexB(const uint8_t*p,size_t n,unsigned length,std::vector<uint8_t>&out){
 if(!p||!n||n>192*1024)return false;out.clear();if(n>=4&&!p[0]&&!p[1]&&(p[2]==1||(!p[2]&&p[3]==1))){out.assign(p,p+n);return true;}
 if(length<1||length>4)return false;size_t at=0;while(at<n){if(n-at<length)return false;uint32_t size=0;for(unsigned i=0;i<length;i++)size=(size<<8)|p[at++];if(!size||size>n-at||out.size()+size+4>192*1024)return false;out.push_back(0);out.push_back(0);out.push_back(0);out.push_back(1);out.insert(out.end(),p+at,p+at+size);at+=size;}return true;
}
bool AacAsc(const uint8_t*p,size_t n,AacConfig&out){
 out=AacConfig{};if(!p||n<2||n>64)return false;unsigned at=0;bool ok=true;
 auto read=[&](unsigned count){unsigned value=0;if(at+count>n*8){ok=false;return 0u;}while(count--){value=(value<<1)|((p[at/8]>>(7-at%8))&1);++at;}return value;};
 out.object=read(5);if(out.object==31)out.object=32+read(6);
 unsigned index=read(4);const unsigned rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
 out.rate=index==15?read(24):index<13?rates[index]:0;out.channels=read(4);
 if(out.object!=2||!ok||read(1)||read(1)||read(1))return false;
 return ok&&out.rate>=8000&&out.rate<=48000&&out.channels>=1&&out.channels<=2;
}
bool AacRawPayload(const uint8_t*p,size_t n,const uint8_t*&payload,size_t&bytes){
 payload=p;bytes=n;if(!p||!n)return false;
 if(n>=2&&p[0]==0xff&&(p[1]&0xf6)==0xf0){
  if(n<7)return false;size_t header=(p[1]&1)?7:9;
  size_t frame=((size_t)(p[3]&3)<<11)|((size_t)p[4]<<3)|(p[5]>>5);
  if(frame!=n||frame<=header||(p[6]&3))return false;payload=p+header;bytes=n-header;
 }return true;
}

namespace {
uint32_t Be32(const uint8_t*p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
bool Descriptor(const uint8_t*p,size_t n,unsigned depth,AacConfig&out){
 if(depth>4)return false;size_t at=0;
 while(at<n){uint8_t tag=p[at++];size_t size=0;unsigned i=0;uint8_t v=0;
  do{if(at==n||i++==4)return false;v=p[at++];size=(size<<7)|(v&127);}while(v&128);
  if(size>n-at)return false;size_t skip=size;
  if(tag==5)return AacAsc(p+at,size,out);
  if(tag==3){if(size<3)return false;uint8_t flags=p[at+2];skip=3;if(flags&128)skip+=2;if(flags&64){if(skip>=size)return false;skip+=1+p[at+skip];}if(flags&32)skip+=2;}
  else if(tag==4){if(size<13||p[at]!=0x40)return false;skip=13;}
  if(skip<size&&Descriptor(p+at+skip,size-skip,depth+1,out))return true;
  at+=size;
 }return false;
}
struct AacProbe {
 ReadAt read;void*ctx;unsigned budget=8192;
 bool Boxes(uint32_t at,uint32_t end,unsigned depth,AacConfig&out){
  if(depth>12)return false;
  while(end-at>=8&&budget){--budget;uint8_t h[48]{};if(!read(ctx,at,h,8))return false;
   uint64_t size=Be32(h);uint32_t header=8;
   if(size==1){if(end-at<16||!read(ctx,at+8,h+8,8))return false;size=(uint64_t)Be32(h+8)<<32|Be32(h+12);header=16;}
   if(!size)size=end-at;if(size<header||size>end-at)return false;
   uint32_t body=at+header,next=at+(uint32_t)size;const uint32_t type=Be32(h+4);uint32_t skip=0;bool descend=false;
   switch(type){
    case 0x6d6f6f76:case 0x7472616b:case 0x6d646961:case 0x6d696e66:case 0x7374626c:case 0x77617665:descend=true;break;
    case 0x73747364:skip=8;descend=true;break; // fullbox and sample-entry count
    case 0x6d703461: // AudioSampleEntry, including QuickTime version 1/2
     if(next-body<28||!read(ctx,body,h,28))break;
     skip=28;if(h[8]==0&&h[9]==1)skip+=16;else if(h[8]==0&&h[9]==2)skip+=36;else if(h[8]||h[9])break;
     descend=true;break;
    case 0x65736473:{uint8_t esds[256];uint32_t bytes=next-body;
     if(bytes>=6&&bytes<=sizeof(esds)&&read(ctx,body,esds,bytes)&&Descriptor(esds+4,bytes-4,0,out))return true;break;}
   }
   if(descend&&skip<=next-body&&Boxes(body+skip,next,depth+1,out))return true;
   at=next;
  }return false;
 }
};
}
bool ProbeAacConfig(ReadAt read,void*ctx,uint32_t size,bool flv,AacConfig&out){
 if(!read||size<9)return false;
 if(!flv){AacProbe probe{read,ctx};return probe.Boxes(0,size,0,out);}
 uint8_t h[16];if(!read(ctx,0,h,9)||h[0]!='F'||h[1]!='L'||h[2]!='V')return false;
 uint32_t at=Be32(h+5);if(at<9||at>size||size-at<4)return false;at+=4;
 for(unsigned tags=0;tags<8192&&at<size&&at<2*1024*1024;tags++){
  if(size-at<15||!read(ctx,at,h,11))return false;
  uint32_t n=(uint32_t)h[1]<<16|(uint32_t)h[2]<<8|h[3];if(n>size-at-15)return false;
  if(h[0]==8&&n>=4&&n<=66){uint8_t payload[66];if(!read(ctx,at+11,payload,n))return false;
   if((payload[0]>>4)==10&&payload[1]==0)return AacAsc(payload+2,n-2,out);}
  at+=15+n;
 }return false;
}
}}
