// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <map>
#include <vector>
#include <cstdint>
namespace HiApp {
// The project's small C++ library has a non-atomic std::shared_ptr. Keep
// cross-thread ownership local to HiApp, without changing other subsystems.
struct RefCounted {unsigned references=0;};
template<class T>class Ref {
 T*p_=nullptr;
 void Retain(){if(p_)__atomic_add_fetch(&p_->references,1u,__ATOMIC_RELAXED);}
 void Release(){if(p_&&__atomic_sub_fetch(&p_->references,1u,__ATOMIC_ACQ_REL)==0)delete p_;}
public:
 Ref()=default;explicit Ref(T*p):p_(p){Retain();}
 Ref(const Ref&r):p_(r.p_){Retain();}
 Ref(Ref&&r):p_(r.p_){r.p_=nullptr;}
 ~Ref(){Release();}
 Ref&operator=(Ref r){T*t=p_;p_=r.p_;r.p_=t;return *this;}
 void reset(){Ref empty;*this=empty;}
 T*get()const{return p_;}T*operator->()const{return p_;}T&operator*()const{return *p_;}
 explicit operator bool()const{return p_!=nullptr;}
};
constexpr size_t kPackageLimit=256*1024;
struct Package {
 std::string id,name,version,entry,raw,icon;
 std::map<std::string,std::string> files;
 bool network=false,storage=false,camera=false;
};
struct PngIcon:RefCounted {std::string bytes;uint32_t fingerprint=0;};
struct Record {Ref<PngIcon> png;std::string file,id,name,version,error,icon,installed_version;bool installed=false,present=false;};
bool ValidId(const std::string&);
bool Load(const std::string&path,Package&,std::string&error,bool metadata_only=false);
bool Prepare(bool write,std::string&error);
using ScanProgress=bool(*)(void*,int);
std::vector<Record> Scan(bool installed,std::string&error,ScanProgress progress=nullptr,void*context=nullptr);
bool Install(const std::string&file,std::string&message);
bool Uninstall(const std::string&id,std::string&message);
bool ReadData(const std::string&id,std::string&value);
bool WriteData(const std::string&id,const std::string&value);
bool ValidateLua(const Package&,std::string&error);
std::string InstalledPath(const std::string&id);
uint32_t Crc32(const void*,size_t);
std::string IconPath(const std::string&id);
bool ReadLauncherOrder(std::string&);
bool WriteLauncherOrder(const std::string&);
}
