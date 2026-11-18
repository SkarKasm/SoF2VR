#include "vr_body_ik.h"
#include <math.h>
#include <string.h>

static float dot(const float a[3],const float b[3]){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
static void cross(const float a[3],const float b[3],float c[3]){c[0]=a[1]*b[2]-a[2]*b[1];c[1]=a[2]*b[0]-a[0]*b[2];c[2]=a[0]*b[1]-a[1]*b[0];}
static float normal(float a[3]){float n=sqrtf(dot(a,a));if(n>1e-8f)for(int i=0;i<3;++i)a[i]/=n;return n;}
static int finite3(const float a[3]){return isfinite(a[0])&&isfinite(a[1])&&isfinite(a[2]);}
static void perpendicular(const float axis[3],float p[3]){
    int least=0;for(int i=1;i<3;++i)if(fabsf(axis[i])<fabsf(axis[least]))least=i;
    float seed[3]={0};seed[least]=1;cross(axis,seed,p);normal(p);
}
int VR_IKSolveArm(VRArmIK *out,const float shoulder[3],const float wrist[3],const float hint[3],float upper,float forearm,const VRArmIK *previous){
    /* Permit previous==out without erasing continuity state. */
    VRArmIK saved={0};if(previous)saved=*previous;memset(out,0,sizeof(*out));
    if(!finite3(shoulder)||!finite3(wrist)||!finite3(hint)||!isfinite(upper)||!isfinite(forearm)||upper<=.01f||forearm<=.01f)return 0;
    float direction[3],pole[3];for(int i=0;i<3;++i)direction[i]=wrist[i]-shoulder[i];
    float requested=normal(direction);
    if(requested<1e-7f){direction[0]=0;direction[1]=-1;direction[2]=0;}
    float minimum=fabsf(upper-forearm)+.0001f,maximum=upper+forearm-.0001f;
    float reach=fmaxf(minimum,fminf(maximum,requested));
    for(int i=0;i<3;++i)pole[i]=hint[i]-shoulder[i];
    float projection=dot(pole,direction);for(int i=0;i<3;++i)pole[i]-=projection*direction[i];
    if(normal(pole)<1e-5f){
        if(saved.valid){memcpy(pole,saved.pole,sizeof(pole));projection=dot(pole,direction);for(int i=0;i<3;++i)pole[i]-=projection*direction[i];}
        if(normal(pole)<1e-5f)perpendicular(direction,pole);
    }
    float along=(upper*upper-forearm*forearm+reach*reach)/(2*reach);
    float bend=sqrtf(fmaxf(0,upper*upper-along*along));
    for(int i=0;i<3;++i){out->shoulder[i]=shoulder[i];out->target[i]=wrist[i];out->end[i]=shoulder[i]+direction[i]*reach;out->elbow[i]=shoulder[i]+direction[i]*along+pole[i]*bend;out->pole[i]=pole[i];}
    out->reachError=fabsf(requested-reach);out->limited=out->reachError>.0002f;out->valid=1;return 1;
}
int VR_IKSegmentMatrix(const float a[3],const float b[3],const float c[3],const float d[3],float m[3][4]){
    if(!finite3(a)||!finite3(b)||!finite3(c)||!finite3(d))return 0;
    float u[3],v[3],axis[3];for(int i=0;i<3;++i){u[i]=b[i]-a[i];v[i]=d[i]-c[i];}
    if(normal(u)<1e-7f||normal(v)<1e-7f)return 0;
    float cosine=fmaxf(-1,fminf(1,dot(u,v)));cross(u,v,axis);float sine=normal(axis);
    if(sine<1e-6f){if(cosine<0)perpendicular(u,axis);else{axis[0]=1;axis[1]=axis[2]=0;}}
    float skew[3][3]={{0,-axis[2],axis[1]},{axis[2],0,-axis[0]},{-axis[1],axis[0],0}};
    for(int i=0;i<3;++i){for(int j=0;j<3;++j)m[i][j]=cosine*(i==j)+(1-cosine)*axis[i]*axis[j]+sine*skew[i][j];m[i][3]=c[i]-dot(m[i],a);}
    return 1;
}
int VR_IKSegmentBasis(const float direction[3],const float width[3],const float fallback[3],float axis[3][3]){
    if(!finite3(direction)||!finite3(width)||!finite3(fallback))return 0;
    float forward[3],lateral[3],up[3];memcpy(forward,direction,sizeof(forward));if(normal(forward)<1e-7f)return 0;
    float projection=dot(width,forward);for(int i=0;i<3;++i)lateral[i]=width[i]-projection*forward[i];
    if(normal(lateral)<1e-5f){projection=dot(fallback,forward);for(int i=0;i<3;++i)lateral[i]=fallback[i]-projection*forward[i];if(normal(lateral)<1e-5f)perpendicular(forward,lateral);}
    cross(forward,lateral,up);normal(up);cross(up,forward,lateral);normal(lateral);
    memcpy(axis[0],forward,12);memcpy(axis[1],lateral,12);memcpy(axis[2],up,12);return 1;
}
