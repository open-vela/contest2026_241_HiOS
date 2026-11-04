/* SPDX-License-Identifier: Apache-2.0
 * App-group policy only. Kernel/boot workers keep their original affinity.
 * Explicit masks always win; opaque children of the neural owner stay pinned.
 */
#include "hi_task_policy.h"
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include "lvgl.h"
static int g_app_group=-1;
#ifdef CONFIG_SMP
extern int __real_pthread_create(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
#define real_create __real_pthread_create
#else
#define real_create pthread_create
#endif
static unsigned role_mask(enum hi_task_role r){
#ifdef CONFIG_SMP
 if(CONFIG_SMP_NCPUS>1)return r==HI_TASK_IO?1:r==HI_TASK_NEURAL?2:3;
#endif
 return 1;
}
unsigned hi_task_supported_cpus(void){
#ifdef CONFIG_SMP
 return CONFIG_SMP_NCPUS;
#else
 return 1;
#endif
}
unsigned hi_task_current_mask(void){
#ifdef CONFIG_SMP
 cpu_set_t set=0;if(!pthread_getaffinity_np(pthread_self(),sizeof(set),&set))return(unsigned)set;
#endif
 return 1;
}
unsigned hi_task_current_cpu(void){
#ifdef CONFIG_SMP
 return(unsigned)sched_getcpu();
#else
 return 0;
#endif
}
int hi_task_post_ui(void(*fn)(void*),void*arg){if(!fn)return EINVAL;lv_lock();int rc=lv_async_call(fn,arg)==LV_RESULT_OK?0:ENOMEM;lv_unlock();return rc;}
void hi_task_enable_app_balance(void){
 __atomic_store_n(&g_app_group,(int)getpid(),__ATOMIC_RELEASE);
 printf("APP_SMP_POLICY group=%d cores=%u default_worker_mask=%u\n",getpid(),hi_task_supported_cpus(),role_mask(HI_TASK_COMPUTE));
}
static int attr_mask(pthread_attr_t*a,unsigned mask){
#ifdef CONFIG_SMP
 cpu_set_t set=mask;return pthread_attr_setaffinity_np(a,sizeof(set),&set);
#else
 (void)a;(void)mask;return 0;
#endif
}
static void background_schedule(pthread_attr_t*a,bool automatic){
 if(automatic||a->inheritsched==PTHREAD_INHERIT_SCHED){
  struct sched_param p={0};p.sched_priority=81;
  pthread_attr_setschedpolicy(a,SCHED_RR);pthread_attr_setschedparam(a,&p);pthread_attr_setinheritsched(a,PTHREAD_EXPLICIT_SCHED);
 }
}
int hi_task_create(pthread_t*t,const pthread_attr_t*original,void*(*fn)(void*),void*arg,enum hi_task_role role){
 pthread_attr_t a;bool owned=!original;int rc=0;
 if(original)a=*original;else if((rc=pthread_attr_init(&a)))return rc;
 if(role==HI_TASK_COMPUTE)background_schedule(&a,owned);
 rc=attr_mask(&a,role_mask(role));if(!rc)rc=real_create(t,&a,fn,arg);
 if(owned)pthread_attr_destroy(&a);return rc;
}
int hi_task_create_legacy(pthread_t*t,const pthread_attr_t*a,void*(*fn)(void*),void*p){
#ifdef CONFIG_SMP
 if(a&&a->affinity)return real_create(t,a,fn,p);
#endif
 return hi_task_create(t,a,fn,p,HI_TASK_IO);}
int hi_task_create_compute(pthread_t*t,const pthread_attr_t*a,void*(*fn)(void*),void*p){return hi_task_create(t,a,fn,p,HI_TASK_COMPUTE);}
int hi_task_create_neural(pthread_t*t,const pthread_attr_t*a,void*(*fn)(void*),void*p){return hi_task_create(t,a,fn,p,HI_TASK_NEURAL);}
int hi_task_set_mask_sync(unsigned mask){
#ifdef CONFIG_SMP
 cpu_set_t set=mask;int rc=pthread_setaffinity_np(pthread_self(),sizeof(set),&set);if(rc)return rc;
 /* A CPU-set update is not an ownership handoff until execution is on an
  * allowed hart. Yield/block only on an observed transition, never spin. */
 for(unsigned tries=0;tries<8;tries++){
  if((mask&(1u<<hi_task_current_cpu()))&&hi_task_current_mask()==mask)return 0;
  sched_yield();usleep(1);
 }
 return ETIMEDOUT;
#else
 return mask==1?0:EINVAL;
#endif
}
int hi_task_bind(enum hi_task_role r){return hi_task_set_mask_sync(role_mask(r));}
#ifdef CONFIG_SMP
int __wrap_pthread_create(pthread_t*t,const pthread_attr_t*original,void*(*fn)(void*),void*p){
 int group=__atomic_load_n(&g_app_group,__ATOMIC_ACQUIRE);
 if(group<0||getpid()!=group||(original&&original->affinity))return real_create(t,original,fn,p);
 pthread_attr_t a;bool owned=!original;int rc=0;
 if(original)a=*original;else if((rc=pthread_attr_init(&a)))return rc;
 unsigned mask=hi_task_current_mask()==2?2:3;if(mask==3)background_schedule(&a,owned);rc=attr_mask(&a,mask);if(!rc)rc=real_create(t,&a,fn,p);
 if(owned)pthread_attr_destroy(&a);return rc;
}
#endif
int hi_task_create_cpp_default(pthread_t*t,const pthread_attr_t*a,void*(*fn)(void*),void*p){
#ifdef CONFIG_SMP
 /* NuttX attr_init defaults to EXPLICIT, even when std::thread's caller
  * never requested scheduling. Mark this specific C++ default explicitly. */
 if(__atomic_load_n(&g_app_group,__ATOMIC_ACQUIRE)==getpid()&&hi_task_current_mask()!=2){
  pthread_attr_t copy;bool owned=!a;int rc=0;if(a)copy=*a;else if((rc=pthread_attr_init(&copy)))return rc;
  background_schedule(&copy,true);rc=__wrap_pthread_create(t,&copy,fn,p);if(owned)pthread_attr_destroy(&copy);return rc;
 }
 return __wrap_pthread_create(t,a,fn,p);
#else
 return real_create(t,a,fn,p);
#endif
}
void hi_task_compute_begin(struct hi_compute_scope*s,int priority){
 *s=(struct hi_compute_scope){0};
#ifdef CONFIG_SMP
 if(CONFIG_SMP_NCPUS<2)return;
 if(pthread_getaffinity_np(pthread_self(),sizeof(s->mask),&s->mask)||pthread_getschedparam(pthread_self(),&s->policy,&s->priority))return;
 if(priority>0&&s->priority.sched_priority>priority){struct sched_param a=s->priority;a.sched_priority=priority;
  if(pthread_setschedparam(pthread_self(),SCHED_RR,&a))return;s->priority_changed=true;}
 /* Do not first force CPU1: a ready neural owner could indefinitely delay
  * that transition while CPU0 is available. Open both and let NuttX choose. */
 if(hi_task_set_mask_sync(3)){
  if(s->priority_changed)pthread_setschedparam(pthread_self(),s->policy,&s->priority);return;}
 s->active=true;
#else
 (void)priority;
#endif
}
void hi_task_compute_end(struct hi_compute_scope*s){
#ifdef CONFIG_SMP
 if(s->active){int rc=hi_task_set_mask_sync((unsigned)s->mask);
  if(rc)printf("APP_SMP_RESTORE_FAIL rc=%d\n",rc);s->active=false;}
 if(s->priority_changed){pthread_setschedparam(pthread_self(),s->policy,&s->priority);s->priority_changed=false;}
#else
 (void)s;
#endif
}
