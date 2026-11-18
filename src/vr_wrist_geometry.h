#ifndef SOF2_VR_WRIST_GEOMETRY_H
#define SOF2_VR_WRIST_GEOMETRY_H
#include <math.h>
typedef struct {float xyz[3],st[2];unsigned char rgba[4];} VRWristVertex;
typedef struct {float p[3],uv[2],endUV[2],angle;} VRWristPoint;
static int WristAngleOrder(const void *a,const void *b){float d=((const VRWristPoint *)a)->angle-((const VRWristPoint *)b)->angle;return (d>0)-(d<0);}
static void WristSample(const VRWristPoint *ring,int count,float angle,VRWristVertex *out){
    while(angle<ring[0].angle)angle+=6.28318530718f;
    while(angle>=ring[0].angle+6.28318530718f)angle-=6.28318530718f;
    int a=count-1,b=0;float lo=ring[a].angle,hi=ring[0].angle+6.28318530718f;
    for(int i=0;i<count;++i){int j=(i+1)%count;float end=ring[j].angle+(j?0:6.28318530718f);
        if(angle>=ring[i].angle && angle<end){a=i;b=j;lo=ring[i].angle;hi=end;break;}}
    float t=fmaxf(0,fminf(1,(angle-lo)/fmaxf(.000001f,hi-lo)));
    for(int k=0;k<3;++k)out->xyz[k]=ring[a].p[k]+t*(ring[b].p[k]-ring[a].p[k]);
    for(int k=0;k<2;++k)out->st[k]=ring[a].uv[k]+t*(ring[a].endUV[k]-ring[a].uv[k]);
}
#endif
