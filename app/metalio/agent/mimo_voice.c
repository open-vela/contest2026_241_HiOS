// SPDX-License-Identifier: Apache-2.0
// MiMo voice backend for the upstream ASR/TTS provider interfaces.
#include "mimo_voice.h"
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static unsigned le16(const unsigned char *p){return p[0]|((unsigned)p[1]<<8);}
static uint32_t le32(const unsigned char *p){return le16(p)|((uint32_t)le16(p+2)<<16);}
static void put32(unsigned char *p,uint32_t value){for(int i=0;i<4;i++)p[i]=(unsigned char)(value>>(8*i));}
static cJSON *request(cJSON *body,size_t cap) {
 char key[256]={0},host[128]={0},port[8]={0};
 if(claw_config_get("voice_api_key",key,sizeof(key))!=0)claw_config_get("api_key",key,sizeof(key));
 if(!key[0])return NULL;
 if(claw_config_get("voice_host",host,sizeof(host))!=0)strcpy(host,"api.xiaomimimo.com");
 if(claw_config_get("voice_port",port,sizeof(port))!=0)strcpy(port,"443");
 vela_header_t headers[]={{"api-key",key},{NULL,NULL}};
 char *payload=cJSON_PrintUnformatted(body),*response=calloc(1,cap);
 cJSON *json=NULL;
 if(payload && response){int status=vela_https_request(host,port,"POST","/v1/chat/completions",headers,payload,strlen(payload),response,cap,NULL);if(status==200)json=cJSON_Parse(response);else syslog(LOG_ERR,"[mimo_voice] HTTP status %d\n",status);}
 free(response);free(payload);memset(key,0,sizeof(key));return json;
}
static cJSON *message(cJSON *response){return cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(response,"choices"),0),"message");}
static int recognize(const unsigned char *pcm,size_t len,char *out,size_t cap) {
 if(!pcm || !len || len>16000*2*30 || !out || cap<2)return -EINVAL;
 unsigned char *wav=malloc(len+44);if(!wav)return -ENOMEM;
 memcpy(wav,"RIFF",4);put32(wav+4,(uint32_t)len+36);memcpy(wav+8,"WAVEfmt ",8);put32(wav+16,16);wav[20]=1;wav[21]=0;wav[22]=1;wav[23]=0;put32(wav+24,16000);put32(wav+28,32000);wav[32]=2;wav[33]=0;wav[34]=16;wav[35]=0;memcpy(wav+36,"data",4);put32(wav+40,(uint32_t)len);memcpy(wav+44,pcm,len);
 size_t bcap=((len+44+2)/3)*4+1,encoded=0;char *data=malloc(bcap+22);if(!data){free(wav);return -ENOMEM;}
 strcpy(data,"data:audio/wav;base64,");int ret=mbedtls_base64_encode((unsigned char*)data+22,bcap,&encoded,wav,len+44);free(wav);if(ret){free(data);return -EINVAL;}data[22+encoded]=0;
 cJSON *root=cJSON_CreateObject(),*messages=cJSON_AddArrayToObject(root,"messages"),*msg=cJSON_CreateObject(),*content=cJSON_CreateArray(),*part=cJSON_CreateObject(),*audio=cJSON_CreateObject();
 cJSON_AddStringToObject(root,"model","mimo-v2.5-asr");cJSON_AddBoolToObject(root,"stream",false);cJSON_AddStringToObject(msg,"role","user");cJSON_AddStringToObject(part,"type","input_audio");cJSON_AddStringToObject(audio,"data",data);free(data);cJSON_AddItemToObject(part,"input_audio",audio);cJSON_AddItemToArray(content,part);cJSON_AddItemToObject(msg,"content",content);cJSON_AddItemToArray(messages,msg);
 cJSON *response=request(root,32768);cJSON_Delete(root);cJSON *text=cJSON_GetObjectItem(message(response),"content");ret=-EIO;
 if(cJSON_IsString(text) && strlen(text->valuestring)<cap){strcpy(out,text->valuestring);ret=0;}cJSON_Delete(response);return ret;
}
// Parse WAV chunks instead of assuming a 44-byte header. Resample provider
// PCM to the 16 kHz mono format consumed by HiOS AudioService.
int hios_mimo_decode_wav(const unsigned char *wav,size_t len,unsigned char *out,size_t cap,size_t *written) {
 if(!wav || len<12 || memcmp(wav,"RIFF",4) || memcmp(wav+8,"WAVE",4))return -EINVAL;
 unsigned channels=0,rate=0,bits=0,format=0;const unsigned char *pcm=NULL;size_t bytes=0;
 for(size_t off=12;off+8<=len;){uint32_t chunk=le32(wav+off+4);if(chunk>len-off-8)return -EINVAL;const unsigned char *p=wav+off+8;if(!memcmp(wav+off,"fmt ",4)&&chunk>=16){format=le16(p);channels=le16(p+2);rate=le32(p+4);bits=le16(p+14);}if(!memcmp(wav+off,"data",4)){pcm=p;bytes=chunk;}off+=8+(size_t)chunk+(chunk&1);}
 if(format!=1 || bits!=16 || (channels!=1 && channels!=2) || !rate || rate>96000 || !pcm)return -EINVAL;
 size_t frames=bytes/(channels*2),samples=(uint64_t)frames*16000/rate;if(samples>cap/2 || frames==0)return -ENOSPC;
 for(size_t i=0;i<samples;i++){uint64_t source=(uint64_t)i*rate;size_t frame=source/16000;unsigned fraction=source%16000;int a=0,b=0;size_t next=frame+1<frames?frame+1:frame;for(unsigned c=0;c<channels;c++){a+=(int16_t)le16(pcm+(frame*channels+c)*2);b+=(int16_t)le16(pcm+(next*channels+c)*2);}int value=((int64_t)a*(16000-fraction)+(int64_t)b*fraction)/(16000*channels);out[i*2]=(unsigned char)value;out[i*2+1]=(unsigned char)(value>>8);}
 *written=samples*2;return 0;
}
static int synthesize(const char *text,unsigned char *out,size_t cap,size_t *written){
 *written=0;if(!text || !*text || !out)return -EINVAL;
 cJSON *root=cJSON_CreateObject(),*messages=cJSON_AddArrayToObject(root,"messages"),*msg=cJSON_CreateObject(),*audio=cJSON_CreateObject();char voice[64]={0};if(claw_config_get("mimo_voice",voice,sizeof(voice))!=0)strcpy(voice,"mimo_default");
 cJSON_AddStringToObject(root,"model","mimo-v2.5-tts");cJSON_AddBoolToObject(root,"stream",false);cJSON_AddStringToObject(msg,"role","assistant");cJSON_AddStringToObject(msg,"content",text);cJSON_AddItemToArray(messages,msg);cJSON_AddStringToObject(audio,"format","wav");cJSON_AddStringToObject(audio,"voice",voice);cJSON_AddItemToObject(root,"audio",audio);
 cJSON *response=request(root,2*1024*1024);cJSON_Delete(root);cJSON *data=cJSON_GetObjectItem(cJSON_GetObjectItem(message(response),"audio"),"data");int ret=-EIO;
 if(cJSON_IsString(data)){size_t b64=strlen(data->valuestring),n=0,rawcap=b64/4*3+3;unsigned char *raw=malloc(rawcap);if(raw){if(mbedtls_base64_decode(raw,rawcap,&n,(const unsigned char*)data->valuestring,b64)==0)ret=hios_mimo_decode_wav(raw,n,out,cap,written);free(raw);}}
 cJSON_Delete(response);return ret;
}
static const voice_asr_ops_t asr={.name="mimo",.recognize=recognize};
static const voice_tts_ops_t tts={.name="mimo",.synthesize=synthesize};
int hios_mimo_voice_register(void){int ret=voice_asr_register(&asr);return ret?ret:voice_tts_register(&tts);}
