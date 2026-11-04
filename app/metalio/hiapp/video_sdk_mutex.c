// SPDX-License-Identifier: Apache-2.0
// ESP-IDF prebuilt extractor objects embed a four-byte pthread mutex slot.
// NuttX pthread_mutex_t is larger: never let it write into the vendor pool.
// Only the private extractor archive references these ABI adapters.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SDK_MUTEX_SLOTS 8
typedef struct {pthread_mutex_t mutex;uint32_t token;} NativeMutex;
static pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;
static NativeMutex *registry[SDK_MUTEX_SLOTS];
static uint32_t generation;

static uint32_t read_token(const void *slot){uint32_t token=0;if(slot)memcpy(&token,slot,sizeof(token));return token;}
static NativeMutex *lookup(const void *slot){
 uint32_t token=read_token(slot);unsigned index=(token&15u)-1u;NativeMutex *native=NULL;
 pthread_mutex_lock(&registry_lock);
 if(index<SDK_MUTEX_SLOTS&&registry[index]&&registry[index]->token==token)native=registry[index];
 pthread_mutex_unlock(&registry_lock);return native;
}
int hivideo_sdk_mutex_init(void *slot,const void *foreign_attr){
 if(!slot)return EINVAL;if(foreign_attr)return ENOTSUP;
 NativeMutex *native=malloc(sizeof(*native));if(!native)return ENOMEM;
 int rc=pthread_mutex_init(&native->mutex,NULL);if(rc){free(native);return rc;}
 pthread_mutex_lock(&registry_lock);unsigned index=0;while(index<SDK_MUTEX_SLOTS&&registry[index])++index;
 if(index==SDK_MUTEX_SLOTS){pthread_mutex_unlock(&registry_lock);pthread_mutex_destroy(&native->mutex);free(native);return EAGAIN;}
 generation=(generation+1u)&0x0fffffffu;if(!generation)generation=1;
 native->token=(generation<<4)|(index+1u);registry[index]=native;
 memcpy(slot,&native->token,sizeof(native->token));pthread_mutex_unlock(&registry_lock);return 0;
}
int hivideo_sdk_mutex_lock(void *slot){NativeMutex *native=lookup(slot);return native?pthread_mutex_lock(&native->mutex):EINVAL;}
int hivideo_sdk_mutex_unlock(void *slot){NativeMutex *native=lookup(slot);return native?pthread_mutex_unlock(&native->mutex):EINVAL;}
int hivideo_sdk_mutex_destroy(void *slot){
 uint32_t token=read_token(slot);unsigned index=(token&15u)-1u;int rc=EINVAL;NativeMutex *native=NULL;
 pthread_mutex_lock(&registry_lock);
 if(index<SDK_MUTEX_SLOTS&&registry[index]&&registry[index]->token==token){
  native=registry[index];rc=pthread_mutex_destroy(&native->mutex);
  if(!rc){registry[index]=NULL;uint32_t empty=0;memcpy(slot,&empty,sizeof(empty));}else native=NULL;
 }
 pthread_mutex_unlock(&registry_lock);if(native)free(native);return rc;
}
