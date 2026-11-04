#pragma once
#include <string>
#include <functional>
#include <cstdint>
// Bounded streaming decode. Callback receives 16 kHz mono; false cancels.
bool DecodeLocalAudio(const std::string&,const std::function<bool(const int16_t*,size_t)>&,int limit_ms=12000);
