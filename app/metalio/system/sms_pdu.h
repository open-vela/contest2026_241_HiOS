#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace SmsPdu {
struct Part {
    std::string number,text;
    int64_t when=0;
    unsigned reference=0,total=1,sequence=1;
};
bool Decode(const std::string &hex,Part &part);
bool Encode(const std::string &number,const std::string &text,unsigned reference,std::vector<std::string> &pdus);
}
