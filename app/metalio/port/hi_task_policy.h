/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HIOS_HI_TASK_POLICY_H
#define HIOS_HI_TASK_POLICY_H
#include <nuttx/config.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
enum hi_task_role { HI_TASK_IO=0, HI_TASK_COMPUTE=1, HI_TASK_NEURAL=2 };
void hi_task_enable_app_balance(void);
int hi_task_create(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*,enum hi_task_role);
int hi_task_create_legacy(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
int hi_task_create_compute(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
int hi_task_create_neural(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
int hi_task_create_cpp_default(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
int hi_task_bind(enum hi_task_role);
int hi_task_set_mask_sync(unsigned);
unsigned hi_task_supported_cpus(void);
unsigned hi_task_current_mask(void);
unsigned hi_task_current_cpu(void);
int hi_task_post_ui(void(*)(void*),void*);
struct hi_compute_scope {cpu_set_t mask;struct sched_param priority;int policy;bool active,priority_changed;};
void hi_task_compute_begin(struct hi_compute_scope*,int priority);
void hi_task_compute_end(struct hi_compute_scope*);
#ifdef __cplusplus
}
#endif
#endif
