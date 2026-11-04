// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "hiapp.h"
#include <atomic>
#include <mutex>
namespace HiApp {
struct VideoScanResult{std::string url,hint;bool finished=false,has_frame=false;};
class VideoScanner:public RefCounted {
public:
 bool Start();void Stop();VideoScanResult Result();
 static void Observe(const uint8_t*,int,int);
private:
 std::atomic<bool>stop_{false},finished_{false},seen_{false};
 std::mutex frame_mutex_,result_mutex_;uint8_t*gray_=nullptr;bool ready_=false;
 std::string url_,hint_;void Frame(const uint8_t*,int,int);void Run();static void*Entry(void*);
};
}
