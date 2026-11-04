// SPDX-License-Identifier: Apache-2.0
// HiOS adapter. No video bytes or decoded pictures enter the Lua heap.
#include "video_decoder.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#ifdef __NuttX__
#include "esp_heap_caps.h"
#endif
#include "h264bsd_decoder.h"
#include "h264bsd_dpb.h"
#include "h264bsd_seq_param_set.h"
#undef malloc
#undef free

#define HV_DECODER_LIMIT (4u*1024*1024)
#define HV_ACCESS_LIMIT (192u*1024)
struct hv_decoder {storage_t *storage;size_t used;const char *error;uint8_t *input;int valid_sps;};
typedef union {struct {size_t size;hv_decoder *owner;} info;long double alignment;} Block;
static pthread_key_t budget_key;
static pthread_once_t budget_once=PTHREAD_ONCE_INIT;
static int budget_ready=0;
static void budget_init(void){budget_ready=pthread_key_create(&budget_key,NULL)==0;}
void *hi_video_malloc(size_t size){
 pthread_once(&budget_once,budget_init);if(!budget_ready)return NULL;hv_decoder *d=pthread_getspecific(budget_key);
 if(!d||size>HV_DECODER_LIMIT-sizeof(Block))return NULL;size_t charge=sizeof(Block)+size;if(d->used>HV_DECODER_LIMIT-charge)return NULL;
#ifdef __NuttX__
 Block*b=heap_caps_malloc(charge,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
 Block*b=malloc(charge);
#endif
 if(!b)return NULL;b->info.size=charge;b->info.owner=d;d->used+=charge;return b+1;
}
void hi_video_free(void*p){if(p){Block*b=(Block*)p-1;b->info.owner->used-=b->info.size;
#ifdef __NuttX__
 heap_caps_free(b);
#else
 free(b);
#endif
}}
typedef struct {uint8_t b[1024];unsigned size,bit;int ok;} Bits;
static unsigned bits(Bits*b,unsigned n){unsigned v=0;if(n>32||b->bit+n>b->size*8){b->ok=0;return 0;}while(n--){v=(v<<1)|((b->b[b->bit/8]>>(7-b->bit%8))&1);b->bit++;}return v;}
static unsigned ue(Bits*b){unsigned z=0;while(b->ok&&!bits(b,1)){if(++z>24){b->ok=0;return 0;}}return ((1u<<z)-1)+bits(b,z);}
static int rbsp(Bits*b,const uint8_t*p,size_t n){memset(b,0,sizeof(*b));b->ok=1;unsigned zero=0;for(size_t i=0;i<n;i++){if(zero>=2&&p[i]==3){zero=0;continue;}if(b->size==sizeof(b->b))break;b->b[b->size++]=p[i];zero=p[i]==0?zero+1:0;}return b->size!=0;}
// Validate dimensions and reference counts BEFORE any decoder frame allocation.
static int check_sps(const uint8_t*p,size_t n){
 Bits b;if(!rbsp(&b,p,n))return 0;
 if(bits(&b,8)!=66)return 0;bits(&b,8);bits(&b,8);if(ue(&b)>31||ue(&b)>12)return 0;
 unsigned poc=ue(&b);if(poc==0){if(ue(&b)>12)return 0;}else if(poc!=2)return 0;
 if(ue(&b)>4)return 0;bits(&b,1);unsigned w=ue(&b),h=ue(&b);
 if(w>=40||h>=30||((w+1)*(h+1)>40*23)||!bits(&b,1))return 0;
 bits(&b,1);if(bits(&b,1)){unsigned l=ue(&b),r=ue(&b),t=ue(&b),bot=ue(&b);if(l+r>=(w+1)*8||t+bot>=(h+1)*8)return 0;}
 return b.ok;
}
static int check_pps(const uint8_t*p,size_t n){Bits b;if(!rbsp(&b,p,n))return 0;if(ue(&b)>255||ue(&b)>31)return 0;if(bits(&b,1))return 0;bits(&b,1);return ue(&b)==0&&b.ok;}
static size_t startcode(const uint8_t*p,size_t n,size_t at,unsigned*len){for(size_t i=at;i+3<=n;i++){if(!p[i]&&!p[i+1]){if(p[i+2]==1){*len=3;return i;}if(i+4<=n&&!p[i+2]&&p[i+3]==1){*len=4;return i;}}}*len=0;return n;}
hv_decoder *hv_decoder_create(void){
 pthread_once(&budget_once,budget_init);if(!budget_ready)return NULL;hv_decoder*d=calloc(1,sizeof(*d));if(!d)return NULL;
 if(pthread_setspecific(budget_key,d)!=0){free(d);return NULL;}d->storage=hi_video_malloc(sizeof(storage_t));
 if(d->storage){memset(d->storage,0,sizeof(storage_t));if(h264bsdInit(d->storage,1)!=0){hv_decoder_destroy(d);return NULL;}}
 if(!d->storage){free(d);return NULL;}return d;
}
extern u32 h264bsdDecodeInternal(storage_t*,u8*,u32,u32*);
int hv_decoder_decode(hv_decoder*d,const uint8_t*p,size_t n,hv_picture*out){
 if(!d||!p||!out||!n||n>HV_ACCESS_LIMIT)return -1;memset(out,0,sizeof(*out));d->error=NULL;
 if(pthread_setspecific(budget_key,d)!=0){d->error="无法绑定解码内存上下文";return -1;}
 unsigned code=0;size_t pos=startcode(p,n,0,&code);if(pos==n){d->error="视频帧不是Annex B格式";return -1;}
 while(pos<n){if(pos+code>=n){d->error="空视频NAL";return -1;}unsigned nextcode;size_t next=startcode(p,n,pos+code,&nextcode);unsigned kind=p[pos+code]&31;
  if(kind==7){if(!check_sps(p+pos+code+1,next-pos-code-1)){d->error="视频参数不在支持范围内。\n支持：H.264 Baseline、逐行扫描、最多4参考帧，最大640×368。\n建议转换为480×272、15帧/秒、1参考帧。\nMP4/FLV是封装格式，扩展名正确不代表视频编码兼容。";return -1;}d->valid_sps=1;}
  if(kind==8&&!check_pps(p+pos+code+1,next-pos-code-1)){d->error="不支持CABAC或多切片组视频";return -1;}
  if((kind==1||kind==5)&&!d->valid_sps){d->error="缺少有效视频参数";return -1;}pos=next;code=nextcode;
 }
 if(!d->input)d->input=hi_video_malloc(HV_ACCESS_LIMIT+64);if(!d->input){d->error="解码内存已达4MiB上限";return -1;}
 memcpy(d->input,p,n);memset(d->input+n,0,64);u32 used=0;size_t at=0;unsigned attempts=0;int picture=0;
 while(at<n&&attempts++<1024){u32 ret=h264bsdDecodeInternal(d->storage,d->input+at,(u32)(n-at),&used);
  if(used>n-at){d->error="解码器返回无效长度";return -1;}at+=used;
  if(ret==H264BSD_PIC_RDY){dpbOutPicture_t*pic=h264bsdDpbOutputPicture(d->storage->dpb);seqParamSet_t*s=d->storage->activeSps;
   if(pic&&s){out->stride=s->picWidthInMbs*16;unsigned ph=s->picHeightInMbs*16;out->width=out->stride;out->height=ph;out->crop_x=out->crop_y=0;
    if(s->frameCroppingFlag){out->crop_x=s->frameCropLeftOffset*2;out->crop_y=s->frameCropTopOffset*2;out->width-=(s->frameCropLeftOffset+s->frameCropRightOffset)*2;out->height-=(s->frameCropTopOffset+s->frameCropBottomOffset)*2;}
    out->y=pic->data;out->u=pic->data+out->stride*ph;out->v=out->u+out->stride*ph/4;picture=1;}
  }else if(ret==H264BSD_ERROR||ret==H264BSD_PARAM_SET_ERROR||ret==H264BSD_MEMALLOC_ERROR){d->error="视频数据损坏、不兼容或内存不足";return -1;}
  if(!used&&ret!=H264BSD_HDRS_RDY&&ret!=H264BSD_PIC_RDY){d->error="视频解码无进展";return -1;}
 }
 if(at<n){d->error="视频NAL数量超过上限";return -1;}return picture;
}
const char *hv_decoder_error(hv_decoder*d){return d&&d->error?d->error:"视频解码失败";}
size_t hv_decoder_bytes(hv_decoder*d){return d?d->used:0;}
void hv_decoder_destroy(hv_decoder*d){if(!d)return;pthread_setspecific(budget_key,d);if(d->storage){h264bsdShutdown(d->storage);hi_video_free(d->storage);}hi_video_free(d->input);pthread_setspecific(budget_key,NULL);free(d);}
