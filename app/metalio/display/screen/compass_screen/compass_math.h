#pragma once

#include <algorithm>
#include <cmath>

// Both inputs must be expressed in the same right-handed device frame:
// X screen-right, Y screen-top, Z out of the screen. Keep board mounting
// transforms separate from calibration; a level-app zero is not sensor bias.
namespace HiCompass {
constexpr float kPi = 3.14159265358979323846f;
struct Vec { float x, y, z; };
inline float Dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec Cross(Vec a, Vec b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
inline float Norm(Vec v) { return std::sqrt(Dot(v,v)); }
inline Vec Unit(Vec v) {
    float n=Norm(v);
    return n>0.00001f ? Vec{v.x/n,v.y/n,v.z/n} : Vec{0,0,0};
}
inline float Wrap(float angle) {
    angle=std::fmod(angle,360.f);
    return angle<0 ? angle+360.f : angle;
}
inline float Delta(float from, float to) {
    float d=Wrap(to)-Wrap(from);
    if(d>180) d-=360;
    if(d< -180) d+=360;
    return d;
}
enum class Result { Ok, FieldInvalid, PointingVertical };
inline Result Heading(Vec magnetic, Vec up, float& degrees) {
    float magnitude=Norm(magnetic);
    if(!__builtin_isfinite(magnitude) || magnitude<10 || Norm(up)<0.5f)
        return Result::FieldInvalid;
    up=Unit(up);
    Vec east=Cross(magnetic,up);
    if(Norm(east)<magnitude*0.08f) return Result::FieldInvalid;
    east=Unit(east);
    Vec north=Unit(Cross(up,east));
    if(std::sqrt(east.y*east.y+north.y*north.y)<0.20f)
        return Result::PointingVertical;
    degrees=Wrap(std::atan2(east.y,north.y)*180/kPi);
    return Result::Ok;
}

// Full three-dimensional min/max calibration: hard-iron offset and per-axis
// gain. It deliberately does not claim to correct general soft-iron coupling.
struct Calibration {
    float low[3]{}, high[3]{}, gravity_low[3]{}, gravity_high[3]{};
    unsigned samples=0, octants=0;
    void Add(Vec magnetic, Vec gravity) {
        const float m[]={magnetic.x,magnetic.y,magnetic.z};
        const float g[]={gravity.x,gravity.y,gravity.z};
        for(int i=0;i<3;++i) {
            if(!samples) { low[i]=high[i]=m[i]; gravity_low[i]=gravity_high[i]=g[i]; }
            low[i]=std::min(low[i],m[i]); high[i]=std::max(high[i],m[i]);
            gravity_low[i]=std::min(gravity_low[i],g[i]);
            gravity_high[i]=std::max(gravity_high[i],g[i]);
        }
        // Fixed gravity octants do not move with the running magnetic bounds.
        octants |= 1u<<((g[0]>=0?1:0)|(g[1]>=0?2:0)|(g[2]>=0?4:0));
        ++samples;
    }
    const char* NextMove() const {
        if(gravity_high[0]-gravity_low[0]<1.2f)return "左右翻转设备";
        if(gravity_high[1]-gravity_low[1]<1.2f)return "前后翻转设备";
        if(gravity_high[2]-gravity_low[2]<1.2f)return "屏幕朝上再朝下";
        return "再缓慢转一圈";
    }
    unsigned Progress() const {
        float p=std::min(1.f,samples/80.f);
        unsigned occupied=0;
        for(int i=0;i<8;++i) if(octants&(1u<<i)) ++occupied;
        p=std::min(p,occupied/4.f);
        for(int i=0;i<3;++i) {
            p=std::min(p,(high[i]-low[i])/150.f);
            p=std::min(p,(gravity_high[i]-gravity_low[i])/1.2f);
        }
        return static_cast<unsigned>(std::max(0.f,std::min(1.f,p))*100);
    }
    bool Finish(float offset[3], float scale[3]) const {
        if(Progress()<100) return false;
        float smallest=1e9f, largest=0, mean=0;
        for(int i=0;i<3;++i) {
            float span=high[i]-low[i];
            smallest=std::min(smallest,span); largest=std::max(largest,span);
            mean+=span/6;
        }
        if(largest>smallest*4) return false;
        for(int i=0;i<3;++i) {
            offset[i]=(high[i]+low[i])*0.5f;
            scale[i]=mean/((high[i]-low[i])*0.5f);
        }
        return true;
    }
};
} // namespace HiCompass
