#include "sms_pdu.h"
#include <algorithm>
#include <cctype>
#include <ctime>
namespace {
void Utf8(std::string &s,uint32_t c){
 if(c<0x80)s+=char(c);
 else if(c<0x800){s+=char(0xc0|(c>>6));s+=char(0x80|(c&63));}
 else if(c<0x10000){s+=char(0xe0|(c>>12));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63));}
 else{s+=char(0xf0|(c>>18));s+=char(0x80|((c>>12)&63));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63));}
}
const uint16_t gsm[128]={
 0x40,0xa3,0x24,0xa5,0xe8,0xe9,0xf9,0xec,0xf2,0xc7,10,0xd8,0xf8,13,0xc5,0xe5,
 0x394,0x5f,0x3a6,0x393,0x39b,0x3a9,0x3a0,0x3a8,0x3a3,0x398,0x39e,0x1b,0xc6,0xe6,0xdf,0xc9,
 32,33,34,35,0xa4,37,38,39,40,41,42,43,44,45,46,47,
 48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
 0xa1,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,
 80,81,82,83,84,85,86,87,88,89,90,0xc4,0xd6,0xd1,0xdc,0xa7,
 0xbf,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,
 112,113,114,115,116,117,118,119,120,121,122,0xe4,0xf6,0xf1,0xfc,0xe0};
std::string Septets(const uint8_t *b,size_t bytes,size_t count,size_t bit=0){
 std::string out;bool escape=false;
 for(size_t i=0;i<count;++i,bit+=7){if(bit+7>bytes*8)break;unsigned v=b[bit/8]>>(bit%8);
  if(bit%8>1)v|=unsigned(b[bit/8+1])<<(8-bit%8);v&=127;
  if(escape){uint32_t c=0xfffd;switch(v){case 10:c=12;break;case 20:c='^';break;case 40:c='{';break;case 41:c='}';break;case 47:c='\\';break;case 60:c='[';break;case 61:c='~';break;case 62:c=']';break;case 64:c='|';break;case 101:c=0x20ac;break;}Utf8(out,c);escape=false;}
  else if(v==27)escape=true;else Utf8(out,gsm[v]);
 }return out;
}
bool Utf16(const std::string &s,std::vector<uint16_t> &out){
 for(size_t i=0;i<s.size();){uint32_t c=static_cast<uint8_t>(s[i++]);unsigned n=0;
  if(c<0x80){}else if(c>=0xc2&&c<=0xdf){c&=31;n=1;}else if(c>=0xe0&&c<=0xef){c&=15;n=2;}else if(c>=0xf0&&c<=0xf4){c&=7;n=3;}else return false;
  if(i+n>s.size())return false;for(unsigned j=0;j<n;++j){uint8_t v=s[i++];if((v&0xc0)!=0x80)return false;c=(c<<6)|(v&63);}
  if((n==1&&c<0x80)||(n==2&&c<0x800)||(n==3&&c<0x10000)||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return false;
  if(c>0xffff){c-=0x10000;out.push_back(0xd800+(c>>10));out.push_back(0xdc00+(c&1023));}else out.push_back(c);
 }return true;
}
std::string Hex(const std::vector<uint8_t>&b){const char*h="0123456789ABCDEF";std::string s;for(auto v:b){s+=h[v>>4];s+=h[v&15];}return s;}
int Unhex(char c){if(c>='0'&&c<='9')return c-'0';c=std::toupper(static_cast<unsigned char>(c));return c>='A'&&c<='F'?c-'A'+10:-1;}
int Bcd(unsigned v){return (v&15)*10+(v>>4);}
}
bool SmsPdu::Decode(const std::string &hex,Part &p){
 if(hex.size()<6||(hex.size()&1)||hex.size()>1024)return false;
 std::vector<uint8_t>b;for(size_t i=0;i<hex.size();i+=2){int hi=Unhex(hex[i]),lo=Unhex(hex[i+1]);if(hi<0||lo<0)return false;b.push_back((hi<<4)|lo);}
 size_t i=1+b[0];if(i+3>b.size())return false;const unsigned first=b[i++];if((first&3)!=0)return false;
 const unsigned digits=b[i++],ton=b[i++];const size_t address_bytes=(digits+1)/2;if(i+address_bytes+10>b.size())return false;
 p=Part{};
 if((ton&0x70)==0x50)p.number=Septets(b.data()+i,address_bytes,digits*4/7);
 else {if((ton&0x70)==0x10)p.number="+";for(unsigned k=0;k<digits;++k){unsigned digit=(b[i+k/2]>>(4*(k%2)))&15;if(digit>9)return false;p.number+=char('0'+digit);}}
 i+=address_bytes;++i;unsigned dcs=b[i++];
 tm stamp{};stamp.tm_year=100+Bcd(b[i]);stamp.tm_mon=Bcd(b[i+1])-1;stamp.tm_mday=Bcd(b[i+2]);stamp.tm_hour=Bcd(b[i+3]);stamp.tm_min=Bcd(b[i+4]);stamp.tm_sec=Bcd(b[i+5]);stamp.tm_isdst=0;
 unsigned tz=b[i+6];int quarters=Bcd(tz&0xf7);int offset=(tz&8)?-quarters:quarters;
 bool decimal=true;for(size_t k=0;k<6;++k)if((b[i+k]&15)>9||(b[i+k]>>4)>9)decimal=false;
 // SCTS carries its own signed 15-minute timezone. Decode to a true UTC
 // epoch, independent of the device's wall-clock convention or host TZ/DST.
 if(decimal && quarters<=79 && (tz>>4)<=9 && stamp.tm_mon>=0 && stamp.tm_mon<12 && stamp.tm_mday>0 && stamp.tm_mday<=31 && stamp.tm_hour<24 && stamp.tm_min<60 && stamp.tm_sec<60){
  const tm expected=stamp;const time_t utc=::timegm(&stamp);tm roundtrip{};
  if(utc>0 && gmtime_r(&utc,&roundtrip) && roundtrip.tm_year==expected.tm_year && roundtrip.tm_mon==expected.tm_mon && roundtrip.tm_mday==expected.tm_mday && roundtrip.tm_hour==expected.tm_hour && roundtrip.tm_min==expected.tm_min && roundtrip.tm_sec==expected.tm_sec)
   p.when=static_cast<int64_t>(utc)-offset*15*60;
 }
 i+=7;if(i>=b.size())return false;unsigned udl=b[i++];
 const unsigned alphabet=(dcs>>2)&3;const size_t bytes=alphabet==0?(udl*7+7)/8:udl;if(bytes>140||i+bytes>b.size())return false;
 size_t header=0;if(first&0x40){if(!bytes)return false;header=b[i]+1;if(header>bytes)return false;
  for(size_t pos=1;pos+2<=header;){unsigned kind=b[i+pos++],len=b[i+pos++];if(pos+len>header)return false;
   if(kind==0&&len==3){p.reference=b[i+pos];p.total=b[i+pos+1];p.sequence=b[i+pos+2];}
   if(kind==8&&len==4){p.reference=(b[i+pos]<<8)|b[i+pos+1];p.total=b[i+pos+2];p.sequence=b[i+pos+3];}pos+=len;
  }
 }
 if(!p.total||p.total>10||!p.sequence||p.sequence>p.total)return false;
 if((dcs&0xc0)==0&&(dcs&0x20))return false; // compressed SMS is not plain text
 if(alphabet==0){size_t skip=(header*8+6)/7;if(skip>udl)return false;p.text=Septets(b.data()+i,bytes,udl-skip,skip*7);}
 else if(alphabet==2){if((bytes-header)&1)return false;for(size_t k=header;k<bytes;k+=2){uint32_t c=(b[i+k]<<8)|b[i+k+1];if(c>=0xd800&&c<=0xdbff&&k+3<bytes){unsigned low=(b[i+k+2]<<8)|b[i+k+3];if(low>=0xdc00&&low<=0xdfff){c=0x10000+((c-0xd800)<<10)+low-0xdc00;k+=2;}}if(c>=0xd800&&c<=0xdfff)c=0xfffd;Utf8(p.text,c);}}
 else return false;
 return !p.number.empty();
}
bool SmsPdu::Encode(const std::string &number,const std::string &text,unsigned reference,std::vector<std::string> &pdus){
 std::string digits;bool international=!number.empty()&&number[0]=='+';
 for(size_t i=0;i<number.size();++i){char c=number[i];if(i==0&&c=='+')continue;if(c<'0'||c>'9')return false;digits+=c;}
 if(digits.empty()||digits.size()>20)return false;
 std::vector<uint16_t>units;if(text.empty()||!Utf16(text,units)||units.size()>400)return false;
 std::vector<std::vector<uint16_t>> chunks;
 for(size_t i=0;i<units.size();){size_t n=std::min<size_t>(units.size()>70?67:70,units.size()-i);if(i+n<units.size()&&units[i+n-1]>=0xd800&&units[i+n-1]<=0xdbff)--n;chunks.emplace_back(units.begin()+i,units.begin()+i+n);i+=n;}
 pdus.clear();for(size_t part=0;part<chunks.size();++part){bool multi=chunks.size()>1;std::vector<uint8_t>b;b.push_back(0);b.push_back(multi?0x41:0x01);b.push_back(0);b.push_back(digits.size());b.push_back(international?0x91:0x81);
  for(size_t k=0;k<digits.size();k+=2)b.push_back((digits[k]-'0')|((k+1<digits.size()?digits[k+1]-'0':15)<<4));
  b.push_back(0);b.push_back(8);b.push_back(chunks[part].size()*2+(multi?6:0));
  if(multi){b.push_back(5);b.push_back(0);b.push_back(3);b.push_back(reference&255);b.push_back(chunks.size());b.push_back(part+1);}
  for(auto c:chunks[part]){b.push_back(c>>8);b.push_back(c&255);}pdus.push_back(Hex(b));
 }return true;
}
