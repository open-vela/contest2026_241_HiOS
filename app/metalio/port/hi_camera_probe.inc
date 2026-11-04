
#ifdef HI_APP_SMP_PROBE
extern "C" int hi_camera_compute_probe(){
 const int sw=1920,sh=1080;size_t raw_bytes=raw10_frame_bytes(sw,sh);
 auto*raw=(uint8_t*)memalign(64,raw_bytes);auto*a=(uint8_t*)memalign(64,720*600*3);auto*b=(uint8_t*)memalign(64,720*600*3);
 if(!raw||!a||!b){free(raw);free(a);free(b);return -1;}
 for(size_t i=0;i<raw_bytes;i++)raw[i]=(i*47u+(i>>12)*31u+19u)&255;
 // Commit CPU-generated test RAW before the real converter invalidates DMA input.
 for(size_t i=0;i<raw_bytes;i+=4096)esp_cache_msync(raw+i,std::min(size_t(4096),raw_bytes-i),1<<2);
 int dims[][2]={{360,300},{640,360},{720,600}};int result=0;
 for(auto&d:dims){int w=d[0],h=d[1],cw=sw,ch=sw*h/w;if(ch>sh){ch=sh;cw=sh*w/h;}int cx=((sw-cw)/2)&~1,cy=((sh-ch)/2)&~1;
  int xo[1920],yo[1080];for(int x=0;x<w;x++){int sx=(cx+(w-1-x)*cw/w)&~1;if(sx>sw-2)sx=sw-2;xo[x]=(sx/4)*5+sx%4;}
  for(int y=0;y<h;y++){int sy=(cy+(h-1-y)*ch/h)&~1;if(sy>sh-2)sy=sh-2;yo[y]=sy;}
  g_camera_probe_baseline=true;raw10_to_bgr888_rotated(raw,sw,sh,cx,cy,w,h,a,xo,yo);g_camera_probe_baseline=false;
  unsigned old=g_camera_probe_count.load();raw10_to_bgr888_rotated(raw,sw,sh,cx,cy,w,h,b,xo,yo);
  bool equal=memcmp(a,b,w*h*3)==0;bool restored=hi_task_current_mask()==1&&hi_task_current_cpu()==0;
  printf("APP_SMP_CAMERA width=%d height=%d equal=%d restored=%d\n",w,h,equal,restored);
  printf("APP_SMP_CAMERA_HART mask=%u cpu=%u cpus=%u frames=%u\n",hi_task_current_mask(),hi_task_current_cpu(),g_camera_probe_cpus.load(),g_camera_probe_count.load()-old);
  if(!equal||!restored)result=-2;
 }
 free(raw);free(a);free(b);return result;
}
extern "C" void hi_camera_live_probe(){printf("APP_SMP_CAMERA_LIVE frames=%u cpus=%u\n",g_camera_probe_count.load(),g_camera_probe_cpus.load());}
#endif
