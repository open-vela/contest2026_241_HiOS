#pragma once
#include <cstdint>
#include <cstdio>
#include <cstddef>
// Uses the same lunar year table as the existing wisdom screen.
namespace HiOSLunar {
const uint32_t kLunarInfo[] = {
    0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
    0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
    0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
    0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
    0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
    0x06ca0,0x0b550,0x15355,0x04da0,0x0a5b0,0x14573,0x052b0,0x0a9a8,0x0e950,0x06aa0,
    0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
    0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b6a0,0x195a6,
    0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
    0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x055c0,0x0ab60,0x096d5,0x092e0,
    0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
    0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
    0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
    0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
    0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0,
    0x14b63,
};


struct Date{int month=0,day=0;bool leap=false;};
inline int LeapDays(int y){return(kLunarInfo[y-1900]&15)?((kLunarInfo[y-1900]&0x10000)?30:29):0;}
inline int MonthDays(int y,int m){return(kLunarInfo[y-1900]&(0x10000u>>m))?30:29;}
inline int YearDays(int y){int n=LeapDays(y);for(int m=1;m<=12;m++)n+=MonthDays(y,m);return n;}
inline bool Convert(int y,int m,int d,Date &out){
 if(y<1901||y>2049||m<1||m>12||d<1||d>31)return false;
 int offset=0;for(int a=1900;a<y;a++)offset+=365+(a%4==0&&(a%100!=0||a%400==0));
 const int md[]={31,28,31,30,31,30,31,31,30,31,30,31};
 for(int a=1;a<m;a++)offset+=md[a-1]+(a==2&&y%4==0&&(y%100!=0||y%400==0));offset+=d-31;
 int ly=1900;while(ly<2050&&offset>=YearDays(ly)){offset-=YearDays(ly);ly++;}if(ly>2050||offset<0)return false;
 for(int lm=1;lm<=12;lm++){
   int n=MonthDays(ly,lm);if(offset<n){out={lm,offset+1,false};return true;}offset-=n;
   if((kLunarInfo[ly-1900]&15)==(unsigned)lm){n=LeapDays(ly);if(offset<n){out={lm,offset+1,true};return true;}offset-=n;}
 }return false;
}
inline void Format(int y,int m,int d,char *out,size_t size,bool full=false){
 Date date;if(!Convert(y,m,d,date)){if(size)out[0]=0;return;}
 const char *months[]={"正","二","三","四","五","六","七","八","九","十","冬","腊"};
 const char *digits[]={"十","一","二","三","四","五","六","七","八","九"};char day[16];
 if(date.day<=10)snprintf(day,sizeof(day),"初%s",digits[date.day==10?0:date.day]);
 else if(date.day<20)snprintf(day,sizeof(day),"十%s",digits[date.day-10]);else if(date.day==20)snprintf(day,sizeof(day),"二十");
 else if(date.day<30)snprintf(day,sizeof(day),"廿%s",digits[date.day-20]);else snprintf(day,sizeof(day),"三十");
 if(full)snprintf(out,size,"农历%s%s月%s",date.leap?"闰":"",months[date.month-1],day);
 else if(date.day==1)snprintf(out,size,"%s%s月",date.leap?"闰":"",months[date.month-1]);else snprintf(out,size,"%s",day);
}
}
