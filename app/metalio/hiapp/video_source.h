// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace HiApp { namespace VideoSource {
bool HttpUrl(const std::string&);
bool SdPath(const std::string&);
bool VideoPath(const std::string&);
bool QrUrl(const uint8_t*,size_t,std::string&);
std::string Resolve(const std::string&,const std::string&);
struct Segment{std::string url;uint64_t sequence=0;uint32_t begin=0,duration=0;bool discontinuity=false;};
struct Playlist{std::vector<Segment>segments;std::string variant;uint32_t duration=0;bool live=true;};
bool ParsePlaylist(const std::string&,const std::string&,uint64_t after,uint32_t seek,Playlist&,std::string&);
// Converts avcC SPS/PPS or a length-prefixed sample into Annex B.
bool AvccConfig(const uint8_t*,size_t,std::vector<uint8_t>&,unsigned&);
bool AnnexB(const uint8_t*,size_t,unsigned,std::vector<uint8_t>&);
struct AacConfig {unsigned object=0,rate=0,channels=0;};
bool AacAsc(const uint8_t*,size_t,AacConfig&);
bool AacRawPayload(const uint8_t*,size_t,const uint8_t*&,size_t&);
// Bounded, allocation-free original ASC lookup for SDKs that omit FLV/MOV ASC.
using ReadAt=bool(*)(void*,uint32_t,void*,size_t);
bool ProbeAacConfig(ReadAt,void*,uint32_t,bool flv,AacConfig&);
}}
