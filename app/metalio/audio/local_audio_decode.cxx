#include "local_audio_decode.h"
#include "esp_mp3_dec.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
namespace {
 static uint16_t U16(const uint8_t*p){return p[0]|p[1]<<8;}
 static uint32_t U32(const uint8_t*p){return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
 static uint32_t BE32(const uint8_t*p){return(uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3];}
 struct Meta{bool wav=false,toc_valid=false;long begin=0,end=0;int rate=0,channels=0,bits=16,format=1,bitrate=0,total=0;uint8_t toc[100]{};};
 bool Metadata(FILE*f,Meta &m){
  fseek(f,0,SEEK_END);m.end=ftell(f);fseek(f,0,SEEK_SET);uint8_t h[4096]{};size_t n=fread(h,1,12,f);
  if(n==12&&!memcmp(h,"RIFF",4)&&!memcmp(h+8,"WAVE",4)){
   m.wav=true;bool format=false;long at=12;
   for(int chunks=0;chunks<128&&at+8<m.end;chunks++){
    fseek(f,at,SEEK_SET);if(fread(h,1,8,f)!=8)return false;uint32_t size=U32(h+4);long next=at+8+(long)size+(size&1);
    if(size>(uint32_t)(m.end-at-8)||next<=at)return false;
    if(!memcmp(h,"fmt ",4)){
     if(size<16||fread(h,1,std::min((uint32_t)40,size),f)<16)return false;
     m.format=U16(h);m.channels=U16(h+2);m.rate=U32(h+4);m.bits=U16(h+14);
     if(m.format==0xfffe&&size>=40)m.format=U16(h+24);
     format=true;
    }else if(!memcmp(h,"data",4)){
     m.begin=at+8;m.end=m.begin+size;
     if(!format||m.channels<1||m.channels>2||m.rate<8000||m.rate>192000||
       (m.format!=1&&m.format!=3)||(m.bits!=8&&m.bits!=16&&m.bits!=24&&m.bits!=32)||(m.format==3&&m.bits!=32))return false;
     m.total=(int)((int64_t)size*1000/(m.rate*m.channels*(m.bits/8)));fseek(f,m.begin,SEEK_SET);return true;
    }at=next;
   }return false;
  }
  long tag=0;fseek(f,0,SEEK_SET);if(fread(h,1,10,f)==10&&!memcmp(h,"ID3",3)){
   for(int i=6;i<10;i++)if(h[i]&128)return false;
   tag=10+((h[6]&127)<<21)+((h[7]&127)<<14)+((h[8]&127)<<7)+(h[9]&127)+((h[5]&16)?10:0);
  }
  if(tag<0||tag>=m.end)return false;fseek(f,tag,SEEK_SET);n=fread(h,1,sizeof(h),f);
  for(size_t i=0;i+4<n;i++){
   uint32_t b=BE32(h+i);int ver=(b>>19)&3,layer=(b>>17)&3,br=(b>>12)&15,sr=(b>>10)&3;
   if((b&0xffe00000)!=0xffe00000||ver==1||layer!=1||br==0||br==15||sr==3)continue;
   const int r[]={44100,48000,32000},b1[]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320},b2[]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
   m.rate=r[sr]/(ver==3?1:ver==2?2:4);m.channels=((b>>6)&3)==3?1:2;m.bitrate=(ver==3?b1[br]:b2[br])*1000;m.begin=tag+i;
   if(m.end>=128){uint8_t tail[3];fseek(f,m.end-128,SEEK_SET);if(fread(tail,1,3,f)==3&&!memcmp(tail,"TAG",3))m.end-=128;}
   m.total=(int)((int64_t)(m.end-m.begin)*8000/m.bitrate);
   size_t x=i+4+(((b>>16)&1)?0:2)+(ver==3?(m.channels==1?17:32):(m.channels==1?9:17));
   if(x+12<n&&(!memcmp(h+x,"Xing",4)||!memcmp(h+x,"Info",4))){
    uint32_t flags=BE32(h+x+4);size_t p=x+8;
    if((flags&1)&&p+4<=n){uint32_t frames=BE32(h+p);m.total=(int)((int64_t)frames*(ver==3?1152:576)*1000/m.rate);p+=4;}
    if(flags&2)p+=4;if((flags&4)&&p+100<=n){memcpy(m.toc,h+p,100);m.toc_valid=true;}
   }
   fseek(f,m.begin,SEEK_SET);return true;
  }return false;
 }
 // The raw ESP MP3 decoder accepts a complete Layer III frame, not an
 // arbitrary file-read chunk. Keep partial frames and tags outside the codec.
 static size_t Mp3FrameBytes(const uint8_t *p){
  uint32_t b=BE32(p);int ver=(b>>19)&3,layer=(b>>17)&3,br=(b>>12)&15,sr=(b>>10)&3;
  if((b&0xffe00000u)!=0xffe00000u||ver==1||layer!=1||!br||br==15||sr==3)return 0;
  const int rates[]={44100,48000,32000},v1[]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320},v2[]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
  int rate=rates[sr]/(ver==3?1:ver==2?2:4),bitrate=(ver==3?v1[br]:v2[br])*1000;
  return (ver==3?144:72)*bitrate/rate+((b>>9)&1);
 }
}
bool DecodeLocalAudio(const std::string &path,const std::function<bool(const int16_t*,size_t)> &sink,int limit_ms){
 if(path.compare(0,8,"/sdcard/")!=0||path.find("/../")!=std::string::npos)return false;
 FILE*f=fopen(path.c_str(),"rb");if(!f)return false;Meta m;if(!Metadata(f,m)){fclose(f);return false;}
 void*decoder=nullptr;if(!m.wav&&esp_mp3_dec_open(nullptr,0,&decoder)!=ESP_AUDIO_ERR_OK){fclose(f);return false;}
 std::vector<uint8_t>input(8192),raw_pcm(16384);std::vector<int16_t>wave,staged;staged.reserve(960);size_t used=0;bool eof=false,ok=true,delivered=false;uint64_t phase=0,emitted=0;int oldrate=0,failures=0;
 const uint64_t maximum=limit_ms>0?(uint64_t)limit_ms*16:UINT64_MAX;
 while(ok&&emitted<maximum){
  int frames=0,ch=m.channels,rate=m.rate;const int16_t*pcm=nullptr;
  if(m.wav){long left=m.end-ftell(f);if(left<=0)break;int width=m.bits/8,align=width*ch;size_t bytes=fread(input.data(),1,std::min((long)(input.size()/align*align),left),f);frames=bytes/align;if(!frames)break;wave.resize(frames*ch);
   for(int i=0;i<frames*ch;++i){auto*p=input.data()+i*width;int v;if(m.format==3){float n;memcpy(&n,p,4);if(!__builtin_isfinite(n))n=0;v=(int)(std::max(-1.f,std::min(1.f,n))*32767);}else if(width==1)v=((int)p[0]-128)*256;else if(width==2)v=(int16_t)U16(p);else if(width==3)v=(int32_t)((uint32_t)p[0]<<8|p[1]<<16|(uint32_t)p[2]<<24)>>16;else v=(int32_t)U32(p)>>16;wave[i]=(int16_t)v;}pcm=wave.data();
  }else{
   auto Fill=[&](bool force=false){if(!eof&&(force||used<2048)&&used<input.size()){long left=m.end-ftell(f);size_t n=left>0?fread(input.data()+used,1,std::min((long)(input.size()-used),left),f):0;used+=n;if(!n||ftell(f)>=m.end)eof=true;}};
   Fill();if(used<4)break;size_t at=0,bytes=0;while(at+4<=used&&!(bytes=Mp3FrameBytes(input.data()+at)))++at;
   if(at){used-=at;memmove(input.data(),input.data()+at,used);Fill();}if(!bytes){if(eof)break;continue;}if(used<bytes){Fill(true);if(used<bytes){if(eof)break;continue;}}
   esp_audio_dec_in_raw_t raw{};raw.buffer=input.data();raw.len=bytes;esp_audio_dec_out_frame_t out{};out.buffer=raw_pcm.data();out.len=raw_pcm.size();esp_audio_dec_info_t info{};int result=esp_mp3_dec_decode(decoder,&raw,&out,&info);
   if(result==ESP_AUDIO_ERR_BUFF_NOT_ENOUGH&&raw.consumed==0&&out.needed_size>raw_pcm.size()&&out.needed_size<=65536){raw_pcm.resize(out.needed_size);continue;}
   if(result==ESP_AUDIO_ERR_OK&&out.decoded_size&&info.bits_per_sample==16&&info.channel>=1&&info.channel<=2&&info.sample_rate>=8000&&info.sample_rate<=192000){ch=info.channel;rate=info.sample_rate;frames=out.decoded_size/(ch*2);pcm=(const int16_t*)raw_pcm.data();failures=0;}
   else if(result!=ESP_AUDIO_ERR_DATA_LACK&&++failures>=64){ok=false;break;}
   used-=bytes;memmove(input.data(),input.data()+bytes,used);if(!frames)continue;
  }
  if(rate!=oldrate){phase=0;oldrate=rate;}uint64_t step=((uint64_t)rate<<16)/16000,limit=(uint64_t)frames<<16;
  while(phase<limit&&emitted<maximum){int a=phase>>16,b=std::min(frames-1,a+1),frac=phase&65535;int va=pcm[a*ch],vb=pcm[b*ch];if(ch==2){va=(va+pcm[a*2+1])/2;vb=(vb+pcm[b*2+1])/2;}staged.push_back(va+(int)(((int64_t)(vb-va)*frac)>>16));phase+=step;++emitted;
   if(staged.size()==960){if(!sink(staged.data(),staged.size())){ok=false;break;}delivered=true;staged.clear();}
  }phase-=limit;
 }
 if(ok&&!staged.empty()){ok=sink(staged.data(),staged.size());delivered=delivered||ok;}
 if(decoder)esp_mp3_dec_close(decoder);fclose(f);return ok&&delivered;
}
