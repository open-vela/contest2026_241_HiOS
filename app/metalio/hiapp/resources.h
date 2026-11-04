// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
namespace HiApp {
struct Package;
bool UnpackResource(const std::string&,const std::string&encoding,size_t expected,std::string&);
bool InflateBody(const std::string&,const std::string&encoding,std::string&,size_t limit=256*1024);
std::string DecodeWebText(const Package&,const std::string&,const std::string&encoding,std::string&error);
}
