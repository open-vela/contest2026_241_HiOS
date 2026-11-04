// Remember only verified SIM PINs, bound to the actual ICCID. No SD storage.
#include "settings.h"
#include <metalio/metalio.h>
#include <mbedtls/gcm.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <mutex>

namespace {
std::mutex pin_store_lock;
std::string Hex(const unsigned char *p,size_t n) {
    static const char h[]="0123456789abcdef"; std::string s;
    for(size_t i=0;i<n;i++){s+=h[p[i]>>4];s+=h[p[i]&15];}return s;
}
bool Unhex(const std::string &s,unsigned char *p,size_t n) {
    if(s.size()!=2*n)return false;
    for(size_t i=0;i<n;i++){unsigned v=0;for(int j=0;j<2;j++){
        char c=s[2*i+j];int x=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;
        if(x<0)return false;v=(v<<4)|x;}p[i]=v;}return true;
}
bool Random(unsigned char *p,size_t n){
    int fd=open("/dev/random",O_RDONLY);if(fd<0)return false;
    size_t at=0;while(at<n){ssize_t z=read(fd,p+at,n-at);if(z<=0){close(fd);return false;}at+=z;}close(fd);return true;
}
bool ValidId(const char *id){size_t n=id?strnlen(id,METALIO_SIM_ID_MAX):0;
    if(n<16||n>=METALIO_SIM_ID_MAX)return false;
    for(size_t i=0;i<n;i++)if(id[i]<'0'||id[i]>'9')return false;return true;}
bool Key(Settings &s,unsigned char key[32],bool create){
    std::string stored=s.GetString("device_key");
    if(!stored.empty())return Unhex(stored,key,32);
    if(!create||!Random(key,32))return false;
    return s.SetStringSaved("device_key",Hex(key,32));
}
void Wipe(void *p,size_t n){volatile unsigned char *v=(volatile unsigned char*)p;while(n--)*v++=0;}
}
extern "C" int metalio_app_sim_pin_read(const char *id,char *pin,size_t size){
    if(!ValidId(id)||!pin||size<9)return -EINVAL;pin[0]=0;
    std::lock_guard<std::mutex> lock(pin_store_lock);Settings s("sim_pin_v1",false);
    unsigned char key[32]{},blob[36]{},plain[8]{};
    if(!Key(s,key,false)||!Unhex(s.GetString(std::string("c")+id),blob,sizeof(blob))){Wipe(key,sizeof(key));return -ENOENT;}
    mbedtls_gcm_context g;mbedtls_gcm_init(&g);
    int ret=mbedtls_gcm_setkey(&g,MBEDTLS_CIPHER_ID_AES,key,256);
    if(!ret)ret=mbedtls_gcm_auth_decrypt(&g,8,blob,12,(const unsigned char*)id,strlen(id),blob+20,16,blob+12,plain);
    if(!ret){size_t n=0;while(n<8&&plain[n]>='0'&&plain[n]<='9')n++;
        if(n<4||(n<8&&plain[n]!=0))ret=-EINVAL;else{memcpy(pin,plain,n);pin[n]=0;}}
    mbedtls_gcm_free(&g);Wipe(key,sizeof(key));Wipe(plain,sizeof(plain));return ret? -EACCES:0;
}
extern "C" int metalio_app_sim_pin_remember(const char *id,const char *pin){
    size_t n=pin?strnlen(pin,9):0;if(!ValidId(id)||n<4||n>8)return -EINVAL;
    for(size_t i=0;i<n;i++)if(pin[i]<'0'||pin[i]>'9')return -EINVAL;
    std::lock_guard<std::mutex> lock(pin_store_lock);Settings s("sim_pin_v1",true);
    unsigned char key[32]{},blob[36]{},plain[8]{};memcpy(plain,pin,n);
    if(!Key(s,key,true)||!Random(blob,12)){Wipe(key,32);Wipe(plain,8);return -EIO;}
    mbedtls_gcm_context g;mbedtls_gcm_init(&g);
    int ret=mbedtls_gcm_setkey(&g,MBEDTLS_CIPHER_ID_AES,key,256);
    if(!ret)ret=mbedtls_gcm_crypt_and_tag(&g,MBEDTLS_GCM_ENCRYPT,8,blob,12,(const unsigned char*)id,strlen(id),plain,blob+12,16,blob+20);
    if(!ret&&!s.SetStringSaved(std::string("c")+id,Hex(blob,sizeof(blob))))ret=-EIO;
    mbedtls_gcm_free(&g);Wipe(key,32);Wipe(plain,8);return ret?-EIO:0;
}
extern "C" void metalio_app_sim_pin_forget(const char *id){
    if(!ValidId(id))return;std::lock_guard<std::mutex> lock(pin_store_lock);
    Settings s("sim_pin_v1",true);s.EraseKey(std::string("c")+id);
}
