// SPDX-License-Identifier: Apache-2.0
#include "video_convert.h"
#include <string.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#if defined(CONFIG_METALIO_HPWORKER) || defined(CONFIG_METALIO_SMP_IMAGEWORKER)
#include <stdbool.h>
extern bool esp_hpworker_submit(const uint8_t*,const uint8_t*,const uint8_t*,const uint8_t*,unsigned);
extern bool esp_hpworker_finish(uint16_t*,uint16_t*,unsigned);
#endif
static inline unsigned clip(int n){return (unsigned)n<=255u?(unsigned)n:n<0?0u:255u;}
static inline uint16_t pixel(unsigned y,int r,int g,int b){int c=298*(y>16?y-16:0);return (uint16_t)((clip((c+r)>>8)>>3)<<11|(clip((c+g)>>8)>>2)<<5|(clip((c+b)>>8)>>3));}
void hv_yuv420_rgb565(const uint8_t*y,const uint8_t*u,const uint8_t*v,unsigned stride,unsigned cx,unsigned cy,unsigned sw,unsigned sh,uint16_t*out,unsigned w,unsigned h){
 if(!y||!u||!v||!out||!w||!h||w>480||h>360)return;
 if(w==sw&&h==sh&&!(w&1)&&!(h&1)&&!(cx&1)&&!(cy&1)){
  uint8_t ya[480],yb[480],ur[240],vr[240];uint16_t upper[480],lower[480];
  for(unsigned row=0;row<h;row+=2){
#if defined(CONFIG_METALIO_HPWORKER) || defined(CONFIG_METALIO_SMP_IMAGEWORKER)
   bool pending=row+3<h && esp_hpworker_submit(y+(row+cy+2)*stride+cx,y+(row+cy+3)*stride+cx,u+((row+cy+2)/2)*(stride/2)+cx/2,v+((row+cy+2)/2)*(stride/2)+cx/2,w);
#endif
   memcpy(ya,y+(row+cy)*stride+cx,w);memcpy(yb,y+(row+cy+1)*stride+cx,w);
   memcpy(ur,u+((row+cy)/2)*(stride/2)+cx/2,w/2);memcpy(vr,v+((row+cy)/2)*(stride/2)+cx/2,w/2);
   for(unsigned x=0;x<w;x+=2){int du=ur[x/2]-128,dv=vr[x/2]-128;int r=409*dv+128,g=-100*du-208*dv+128,bl=516*du+128;
    upper[x]=pixel(ya[x],r,g,bl);upper[x+1]=pixel(ya[x+1],r,g,bl);lower[x]=pixel(yb[x],r,g,bl);lower[x+1]=pixel(yb[x+1],r,g,bl);}
   memcpy(out+row*w,upper,w*2);memcpy(out+(row+1)*w,lower,w*2);
#if defined(CONFIG_METALIO_HPWORKER) || defined(CONFIG_METALIO_SMP_IMAGEWORKER)
   if(pending && esp_hpworker_finish(out+(row+2)*w,out+(row+3)*w,w)) row+=2;
#endif
  }return;
 }
 unsigned map[480];for(unsigned x=0;x<w;x++)map[x]=x*sw/w+cx;
 for(unsigned row=0;row<h;row++){unsigned sy=row*sh/h+cy;const uint8_t*py=y+sy*stride,*pu=u+(sy/2)*(stride/2),*pv=v+(sy/2)*(stride/2);
  for(unsigned x=0;x<w;x++){unsigned sx=map[x];int du=pu[sx/2]-128,dv=pv[sx/2]-128;out[row*w+x]=pixel(py[sx],409*dv+128,-100*du-208*dv+128,516*du+128);}
 }
}

#ifdef HI_VIDEO_TARGET_TEST
// SPDX-License-Identifier: Apache-2.0

static inline unsigned old_clip(int n){return (unsigned)n<=255u?(unsigned)n:n<0?0u:255u;}
static inline uint16_t old_pixel(unsigned y,int r,int g,int b){int c=298*(y>16?y-16:0);return (uint16_t)((old_clip((c+r)>>8)>>3)<<11|(old_clip((c+g)>>8)>>2)<<5|(old_clip((c+b)>>8)>>3));}
void hv_yuv420_rgb565_reference(const uint8_t*y,const uint8_t*u,const uint8_t*v,unsigned stride,unsigned cx,unsigned cy,unsigned sw,unsigned sh,uint16_t*out,unsigned w,unsigned h){
 if(!y||!u||!v||!out||!w||!h||w>480||h>360)return;
 if(w==sw&&h==sh&&!(w&1)&&!(h&1)&&!(cx&1)&&!(cy&1)){
  for(unsigned row=0;row<h;row+=2){const uint8_t*a=y+(row+cy)*stride+cx,*b=a+stride;const uint8_t*pu=u+((row+cy)/2)*(stride/2)+cx/2,*pv=v+((row+cy)/2)*(stride/2)+cx/2;uint16_t*dst=out+row*w;
   for(unsigned x=0;x<w;x+=2){int du=pu[x/2]-128,dv=pv[x/2]-128;int r=409*dv+128,g=-100*du-208*dv+128,bl=516*du+128;
    dst[x]=old_pixel(a[x],r,g,bl);dst[x+1]=old_pixel(a[x+1],r,g,bl);dst[w+x]=old_pixel(b[x],r,g,bl);dst[w+x+1]=old_pixel(b[x+1],r,g,bl);}
  }return;
 }
 unsigned map[480];for(unsigned x=0;x<w;x++)map[x]=x*sw/w+cx;
 for(unsigned row=0;row<h;row++){unsigned sy=row*sh/h+cy;const uint8_t*py=y+sy*stride,*pu=u+(sy/2)*(stride/2),*pv=v+(sy/2)*(stride/2);
  for(unsigned x=0;x<w;x++){unsigned sx=map[x];int du=pu[sx/2]-128,dv=pv[sx/2]-128;out[row*w+x]=old_pixel(py[sx],409*dv+128,-100*du-208*dv+128,516*du+128);}
 }
}

#endif
