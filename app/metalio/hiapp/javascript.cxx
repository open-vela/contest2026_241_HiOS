// SPDX-License-Identifier: Apache-2.0
#include "javascript.h"
#include "duktape.h"
#include <cstdlib>
#include <algorithm>
#include <ctime>
#include <cstring>
namespace {
int64_t Now(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return(int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
struct State {duk_context*ctx=nullptr;std::atomic<bool>*stop;size_t used=0;int64_t deadline=0;};
struct Header{size_t size;double align;};
void*Alloc(void*ud,duk_size_t n){auto*s=(State*)ud;if(!n||n>2*1024*1024||s->used>2*1024*1024-n)return nullptr;auto*p=(Header*)malloc(n+sizeof(Header));if(!p)return nullptr;p->size=n;s->used+=n;return p+1;}
void Free(void*ud,void*p){if(!p)return;auto*s=(State*)ud;auto*h=(Header*)p-1;s->used-=h->size;free(h);}
void*Realloc(void*ud,void*p,duk_size_t n){if(!p)return Alloc(ud,n);if(!n){Free(ud,p);return nullptr;}auto*s=(State*)ud;auto*h=(Header*)p-1;size_t used=s->used-h->size;if(n>2*1024*1024||used>2*1024*1024-n)return nullptr;auto*q=(Header*)realloc(h,n+sizeof(Header));if(!q)return nullptr;q->size=n;s->used=used+n;return q+1;}
std::string Text(duk_context*c,int index,size_t cap){duk_size_t length=0;const char*p=duk_get_lstring(c,index,&length);if(!p)return {};size_t n=std::min((size_t)length,cap);while(n&&n<length&&((unsigned char)p[n]&0xc0)==0x80)--n;return std::string(p,n);}

duk_ret_t Clock(duk_context*c){duk_push_number(c,(double)Now());return 1;}
struct Request {int mode;const std::string *prelude,*html,*url,*id,*value;const std::vector<std::string>*scripts;};
// Every potentially throwing Duktape operation, including argument creation
// and result property getters, stays inside the protected boundary. No C++
// owned temporaries are created here: Duktape errors use longjmp.
duk_ret_t Protected(duk_context*c,void*ud){auto*q=(Request*)ud;
 if(q->mode==0){duk_eval_lstring(c,q->prelude->data(),q->prelude->size());duk_pop(c);duk_get_global_string(c,"__hiStart");duk_push_lstring(c,q->html->data(),q->html->size());duk_push_lstring(c,q->url->data(),q->url->size());duk_push_array(c);for(unsigned i=0;i<q->scripts->size();i++){const auto&v=(*q->scripts)[i];duk_push_lstring(c,v.data(),v.size());duk_put_prop_index(c,-2,i);}duk_call(c,3);}
 else{duk_get_global_string(c,"__hiEvent");duk_push_lstring(c,q->id->data(),q->id->size());duk_push_lstring(c,q->value->data(),q->value->size());duk_call(c,2);}
 if(!duk_is_object(c,-1))return duk_error(c,DUK_ERR_TYPE_ERROR,"Invalid page result");
 // Normalize on the protected stack, then copy primitives after return.
 duk_get_prop_string(c,-1,"html");if(!duk_is_string(c,-1)){duk_pop(c);duk_push_string(c,"");}
 duk_get_prop_string(c,-2,"error");if(!duk_is_string(c,-1)){duk_pop(c);duk_push_string(c,"");}
 duk_get_prop_string(c,-3,"navigate");if(!duk_is_string(c,-1)){duk_pop(c);duk_push_string(c,"");}
 duk_get_prop_string(c,-4,"delay");if(!duk_is_number(c,-1)){duk_pop(c);duk_push_int(c,-1);}duk_get_prop_string(c,-5,"title");if(!duk_is_string(c,-1)){duk_pop(c);duk_push_string(c,"");}return 5;
}
HiApp::JsResult Run(State*s,Request&q){HiApp::JsResult r;auto*c=s->ctx;s->deadline=Now()+(q.mode==0?1500:750);
 if(duk_safe_call(c,Protected,&q,0,5)!=DUK_EXEC_SUCCESS){r.error=duk_safe_to_string(c,-5);r.error.resize(std::min(r.error.size(),(size_t)200));duk_pop_n(c,5);return r;}
 r.html=Text(c,-5,256*1024);r.error=Text(c,-4,200);r.navigate=Text(c,-3,2048);r.delay=duk_get_int(c,-2);r.title=Text(c,-1,256);duk_pop_n(c,5);return r;
}

}
extern "C" int hios_js_timeout(void*ud){auto*s=(State*)ud;return s->stop->load()||Now()>s->deadline;}
namespace HiApp {
JavaScript::JavaScript(std::atomic<bool>*stop){auto*s=new State;s->stop=stop;s->ctx=duk_create_heap(Alloc,Realloc,Free,s,nullptr);state_=s;if(s->ctx){duk_push_c_function(s->ctx,Clock,0);duk_put_global_string(s->ctx,"__hiClock");}}
JavaScript::~JavaScript(){auto*s=(State*)state_;if(s){if(s->ctx)duk_destroy_heap(s->ctx);delete s;}}
JsResult JavaScript::Start(const std::string&prelude,const std::string&html,const std::string&url,const std::vector<std::string>&scripts){auto*s=(State*)state_;if(!s->ctx)return {{},"JavaScript运行内存不足",{},-1};Request q{0,&prelude,&html,&url,nullptr,nullptr,&scripts};auto r=Run(s,q);due=r.delay<0?0:Now()+std::max(100,r.delay);return r;}
JsResult JavaScript::Event(const std::string&id,const std::string&value){auto*s=(State*)state_;if(!s->ctx)return {{},"JavaScript运行内存不足",{},-1};Request q{1,nullptr,nullptr,nullptr,&id,&value,nullptr};auto r=Run(s,q);due=r.delay<0?0:Now()+std::max(100,r.delay);return r;}
JsResult JavaScript::Tick(){return Event("__tick","");}
}
