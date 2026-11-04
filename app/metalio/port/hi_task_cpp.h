// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "hi_task_policy.h"
#include <thread>
#include <utility>
#include <cstdio>
namespace HiTask {
// Legacy I/O callbacks are pinned before any client/device code executes.
template<class F>std::thread LegacyThread(F fn){
 // Match the pre-existing minimal std::thread shim: attr_init captures the
 // parent's priority but uses NuttX's default policy, not INHERIT_SCHED.
 pthread_attr_t original;pthread_attr_init(&original);int policy=0;sched_param priority{};
 bool inherit=pthread_attr_getschedpolicy(&original,&policy)==0&&pthread_attr_getschedparam(&original,&priority)==0;pthread_attr_destroy(&original);
 return std::thread([f=std::move(fn),policy,priority,inherit]()mutable{
  int rc=hi_task_bind(HI_TASK_IO);if(rc){std::printf("APP_SMP_BIND_FAIL rc=%d\n",rc);return;}
  if(inherit)pthread_setschedparam(pthread_self(),policy,&priority);
  f();
 });
}
template<class F,class A>std::thread LegacyThread(F fn,A arg){
 return LegacyThread([f=std::move(fn),a=std::move(arg)]()mutable{f(a);});
}
class IoScope {
 cpu_set_t saved_{};bool active_=false;
public:
 IoScope(){
#ifdef CONFIG_SMP
  if(!pthread_getaffinity_np(pthread_self(),sizeof(saved_),&saved_))active_=hi_task_bind(HI_TASK_IO)==0;
#endif
 }
 ~IoScope(){
#ifdef CONFIG_SMP
  if(active_){int rc=hi_task_set_mask_sync((unsigned)saved_);if(rc)std::printf("APP_SMP_RESTORE_FAIL rc=%d\n",rc);}
#endif
 }
};
class ComputeScope {
 hi_compute_scope scope_{};
public:
 explicit ComputeScope(int priority=79){hi_task_compute_begin(&scope_,priority);}
 ~ComputeScope(){hi_task_compute_end(&scope_);}
 ComputeScope(const ComputeScope&)=delete;ComputeScope&operator=(const ComputeScope&)=delete;
};
}
