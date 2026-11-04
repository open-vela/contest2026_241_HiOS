#include "phone_service.h"
#include <cctype>
#include <cstdint>
namespace {
#include "phone_number_regions.inc"
}
static std::string RegionOnly(const std::string &number)
{
    std::string n;
    for (unsigned char c:number) if (std::isdigit(c)) n+=static_cast<char>(c);
    if (n.compare(0,4,"0086")==0) n.erase(0,4);
    else if (n.compare(0,2,"86")==0 && n.size()>11) n.erase(0,2);
    else if (number.find('+')!=std::string::npos || n.compare(0,2,"00")==0)
        return n.empty()?"":"境外号码";
    if (n.empty()) return "";
    if (n.compare(0,3,"400")==0 || n.compare(0,3,"800")==0) return "全国服务号码";
    if (n[0]=='0') n.erase(0,1);
    // Mobile numbers have 11 national digits; fixed lines include area code.
    // These are numbering-plan regions, never a caller's live location.
    if (n.size()<7 || n.size()>11) return "归属地未知";
    for (const auto &g:kRegionGroups) {
        if (n.size()<g.length) continue;
        uint32_t prefix=0;
        for (unsigned i=0;i<g.length;++i) prefix=prefix*10+n[i]-'0';
        unsigned lo=g.begin,hi=g.begin+g.count;
        while(lo<hi) {
            unsigned mid=lo+(hi-lo)/2;
            const auto &r=kRegionRanges[mid];
            if (prefix<r.first) hi=mid;
            else if (prefix>r.last) lo=mid+1;
            else return kRegionCities[r.city];
        }
    }
    return "归属地未知";
}

// Copyright (C) 2013 The Libphonenumber Authors. Apache-2.0.
// Prefix allocation data: https://github.com/google/libphonenumber/blob/master/resources/carrier/en/86.txt
// Carrier data reflects numbering allocations, not live number-portability lookup.
namespace {
struct CarrierPrefix {const char *prefix,*name;};
constexpr CarrierPrefix kCarrierPrefixes[]={
    {"861340","中国移动"},
    {"861341","中国移动"},
    {"861342","中国移动"},
    {"861343","中国移动"},
    {"861344","中国移动"},
    {"861345","中国移动"},
    {"861346","中国移动"},
    {"861347","中国移动"},
    {"861348","中国移动"},
    {"861700","中国电信"},
    {"861701","中国电信"},
    {"861702","中国电信"},
    {"861703","中国移动"},
    {"861704","中国联通"},
    {"861705","中国移动"},
    {"861706","中国移动"},
    {"861707","中国联通"},
    {"861708","中国联通"},
    {"861709","中国联通"},
    {"86130","中国联通"},
    {"86131","中国联通"},
    {"86132","中国联通"},
    {"86133","中国电信"},
    {"86135","中国移动"},
    {"86136","中国移动"},
    {"86137","中国移动"},
    {"86138","中国移动"},
    {"86139","中国移动"},
    {"86145","中国联通"},
    {"86147","中国移动"},
    {"86150","中国移动"},
    {"86151","中国移动"},
    {"86152","中国移动"},
    {"86153","中国电信"},
    {"86155","中国联通"},
    {"86156","中国联通"},
    {"86157","中国移动"},
    {"86158","中国移动"},
    {"86159","中国移动"},
    {"86161","中国广电"},
    {"86166","中国联通"},
    {"86167","中国联通"},
    {"86171","中国联通"},
    {"86172","中国移动"},
    {"86173","中国电信"},
    {"86174","中国电信"},
    {"86175","中国联通"},
    {"86176","中国联通"},
    {"86177","中国电信"},
    {"86178","中国移动"},
    {"86180","中国电信"},
    {"86181","中国电信"},
    {"86182","中国移动"},
    {"86183","中国移动"},
    {"86184","中国移动"},
    {"86185","中国联通"},
    {"86186","中国联通"},
    {"86187","中国移动"},
    {"86188","中国移动"},
    {"86189","中国电信"},
    {"86190","中国电信"},
    {"86191","中国电信"},
    {"86192","中国广电"},
    {"86193","中国电信"},
    {"86195","中国移动"},
    {"86196","中国联通"},
    {"86197","中国移动"},
    {"86198","中国移动"},
    {"86199","中国电信"},
};
std::string NumberCarrier(const std::string &number){
 std::string digits;for(unsigned char c:number)if(std::isdigit(c))digits+=static_cast<char>(c);
 if(digits.size()==15&&digits.compare(0,4,"0086")==0)digits.erase(0,4);
 else if(digits.size()==13&&digits.compare(0,2,"86")==0)digits.erase(0,2);
 else if(number.find('+')!=std::string::npos||digits.compare(0,2,"00")==0)return "";
 if(digits.size()!=11||digits[0]!='1')return "";
 const std::string international="86"+digits;
 for(const auto &p:kCarrierPrefixes)if(international.compare(0,std::char_traits<char>::length(p.prefix),p.prefix)==0)return p.name;
 return "";
}
}
std::string PhoneService::NumberRegion(const std::string &number){
 const auto region=RegionOnly(number),carrier=NumberCarrier(number);
 return carrier.empty()?region:carrier+"(号段)"+(region.empty()?"":" · "+region);
}
