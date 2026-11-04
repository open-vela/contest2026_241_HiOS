#pragma once
#include "compass_math.h"
#include <cstring>

namespace HiCompass {
struct FieldTransform {
    Vec offset{0,0,0};
    float matrix[3][3]{{1,0,0},{0,1,0},{0,0,1}};
    float strength=0;
    bool Valid() const {
        if(!__builtin_isfinite(Norm(offset)) || Norm(offset)>60000 ||
           !__builtin_isfinite(strength) || strength<0 || strength>60000)return false;
        for(int i=0;i<3;++i)for(int j=0;j<3;++j){
            if(!__builtin_isfinite(matrix[i][j]) || std::abs(matrix[i][j])>5 ||
               std::abs(matrix[i][j]-matrix[j][i])>.002f)return false;
        }
        const float minor=matrix[0][0]*matrix[1][1]-matrix[0][1]*matrix[1][0];
        const float determinant=matrix[0][0]*(matrix[1][1]*matrix[2][2]-matrix[1][2]*matrix[2][1])
            -matrix[0][1]*(matrix[1][0]*matrix[2][2]-matrix[1][2]*matrix[2][0])
            +matrix[0][2]*(matrix[1][0]*matrix[2][1]-matrix[1][1]*matrix[2][0]);
        return matrix[0][0]>=.2f && matrix[1][1]>=.2f && matrix[2][2]>=.2f && minor>.01f && determinant>.008f;
    }
    Vec Apply(Vec raw) const {
        const float v[]={raw.x-offset.x,raw.y-offset.y,raw.z-offset.z};
        Vec out{};float* p[]={&out.x,&out.y,&out.z};
        for(int i=0;i<3;++i)for(int j=0;j<3;++j)*p[i]+=matrix[i][j]*v[j];
        return out;
    }
};

// Fit a full ellipsoid, including off-diagonal soft-iron terms. A bounded
// reservoir and residual checks prevent a brief planar turn or a magnetic
// spike from being accepted as a complete three-dimensional calibration.
struct MatrixCalibration : Calibration {
    Vec points[192]{};
    unsigned kept=0;
    void Add(Vec magnetic,Vec gravity) {
        if(!__builtin_isfinite(Norm(magnetic))||!__builtin_isfinite(Norm(gravity)))return;
        Calibration::Add(magnetic,gravity);
        unsigned slot=kept<192?kept++:((samples*2654435761u)^(samples>>3))%samples;
        if(slot<192)points[slot]=magnetic;
    }
    bool FinishMatrix(FieldTransform& transform) const {
        float offset[3],scale[3];
        if(samples<120||kept<80||!Finish(offset,scale))return false;
        double a[9][10]{};
        for(unsigned n=0;n<kept;++n){
            double x=points[n].x/1000.,y=points[n].y/1000.,z=points[n].z/1000.;
            const double f[]={x*x,y*y,z*z,2*x*y,2*x*z,2*y*z,2*x,2*y,2*z};
            for(int i=0;i<9;++i){for(int j=0;j<9;++j)a[i][j]+=f[i]*f[j];a[i][9]+=f[i];}
        }
        for(int col=0;col<9;++col){
            int pivot=col;for(int row=col+1;row<9;++row)if(std::abs(a[row][col])>std::abs(a[pivot][col]))pivot=row;
            if(std::abs(a[pivot][col])<1e-9)return false;
            if(pivot!=col)for(int j=col;j<10;++j)std::swap(a[col][j],a[pivot][j]);
            double d=a[col][col];for(int j=col;j<10;++j)a[col][j]/=d;
            for(int row=0;row<9;++row)if(row!=col){d=a[row][col];for(int j=col;j<10;++j)a[row][j]-=d*a[col][j];}
        }
        double q[3][3]={{a[0][9],a[3][9],a[4][9]},{a[3][9],a[1][9],a[5][9]},{a[4][9],a[5][9],a[2][9]}};
        double solve[3][4];for(int i=0;i<3;++i){for(int j=0;j<3;++j)solve[i][j]=q[i][j];solve[i][3]=-a[i+6][9];}
        for(int col=0;col<3;++col){
            int pivot=col;for(int row=col+1;row<3;++row)if(std::abs(solve[row][col])>std::abs(solve[pivot][col]))pivot=row;
            if(std::abs(solve[pivot][col])<1e-9)return false;
            for(int j=col;j<4;++j)std::swap(solve[col][j],solve[pivot][j]);
            double d=solve[col][col];for(int j=col;j<4;++j)solve[col][j]/=d;
            for(int row=0;row<3;++row)if(row!=col){d=solve[row][col];for(int j=col;j<4;++j)solve[row][j]-=d*solve[col][j];}
        }
        double center[3]={solve[0][3],solve[1][3],solve[2][3]},factor=1;
        for(int i=0;i<3;++i)for(int j=0;j<3;++j)factor+=center[i]*q[i][j]*center[j];
        if(!__builtin_isfinite(factor)||std::abs(factor)<1e-9)return false;
        const double radius=((high[0]-low[0])+(high[1]-low[1])+(high[2]-low[2]))/6000.;
        for(int i=0;i<3;++i)for(int j=0;j<3;++j)q[i][j]*=radius*radius/factor;
        double vectors[3][3]={{1,0,0},{0,1,0},{0,0,1}};
        for(int sweep=0;sweep<24;++sweep){
            int p=0,r=1;for(int i=0;i<3;++i)for(int j=i+1;j<3;++j)if(std::abs(q[i][j])>std::abs(q[p][r])){p=i;r=j;}
            if(std::abs(q[p][r])<1e-10)break;
            double angle=.5*std::atan2(2*q[p][r],q[r][r]-q[p][p]),c=std::cos(angle),s=std::sin(angle);
            double pp=q[p][p],rr=q[r][r],pr=q[p][r];
            q[p][p]=c*c*pp-2*s*c*pr+s*s*rr;q[r][r]=s*s*pp+2*s*c*pr+c*c*rr;q[p][r]=q[r][p]=0;
            for(int k=0;k<3;++k)if(k!=p&&k!=r){double kp=q[k][p],kr=q[k][r];q[k][p]=q[p][k]=c*kp-s*kr;q[k][r]=q[r][k]=s*kp+c*kr;}
            for(int k=0;k<3;++k){double kp=vectors[k][p],kr=vectors[k][r];vectors[k][p]=c*kp-s*kr;vectors[k][r]=s*kp+c*kr;}
        }
        double min_e=1e9,max_e=0;for(int i=0;i<3;++i){min_e=std::min(min_e,q[i][i]);max_e=std::max(max_e,q[i][i]);}
        if(min_e<.04||max_e>25||max_e>min_e*25)return false;
        FieldTransform candidate;candidate.offset={(float)(center[0]*1000),(float)(center[1]*1000),(float)(center[2]*1000)};candidate.strength=radius*1000;
        for(int i=0;i<3;++i)for(int j=0;j<3;++j){candidate.matrix[i][j]=0;for(int k=0;k<3;++k)candidate.matrix[i][j]+=vectors[i][k]*std::sqrt(q[k][k])*vectors[j][k];}
        double squared=0;float worst=0;
        for(unsigned i=0;i<kept;++i){float e=std::abs(Norm(candidate.Apply(points[i]))/candidate.strength-1);squared+=e*e;worst=std::max(worst,e);}
        if(std::sqrt(squared/kept)>.06||worst>.18||!candidate.Valid())return false;
        transform=candidate;return true;
    }
};

struct HeadingFilter {
    bool ready=false;float angle=0;
    float Update(float measured) {
        if(!ready){ready=true;angle=Wrap(measured);return angle;}
        float delta=Delta(angle,measured);
        if(std::abs(delta)>.35f)angle=Wrap(angle+delta*(std::abs(delta)>8?.7f:.22f));
        return angle;
    }
};
}
