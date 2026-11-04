// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
namespace HiApp {
struct JsResult {std::string html,error,navigate;int delay=-1;std::string title;};
class JavaScript {
 void*state_=nullptr;
public:
 explicit JavaScript(std::atomic<bool>*stop);
 ~JavaScript();
 JsResult Start(const std::string&prelude,const std::string&html,const std::string&url,const std::vector<std::string>&scripts);
 JsResult Event(const std::string&id,const std::string&value);
 JsResult Tick();
 int64_t due=0;
};
}
