/* Continuous native-outfit wrist skin between the exact submitted arm and
 * hand boundaries. Every corner of both boundaries participates in the strip. */
#include "vr_wrist_geometry.h"
typedef char WristVertexSize[sizeof(VRWristVertex)==24?1:-1];
static struct {int valid;const VRDonorWrist *rig;GameEntity hand;} wristJoin[2];
static void AddWristJoin(const GameRefdef *base,const GameEntity *arm,int physical,int outfit){
    if(!wristJoin[physical].valid)return;
    const GameEntity *hand=&wristJoin[physical].hand;const VRDonorWrist *w=wristJoin[physical].rig;const VRArmRim *r=&vrArmRims[outfit][physical];
    if(w->count<3 || w->count>64 || r->count<3 || r->count>64)return;
    VRWristPoint rings[2][64];int counts[2]={r->count,w->count};float centres[2][3]={{0}};
    for(int side=0;side<2;++side)for(int n=0;n<counts[side];++n){
        const GameEntity *e=side?hand:arm;const float *p=side?w->ring[n]:r->point[n];
        for(int k=0;k<3;++k){rings[side][n].p[k]=e->origin[k];for(int j=0;j<3;++j)rings[side][n].p[k]+=e->axis[j][k]*p[j];centres[side][k]+=rings[side][n].p[k]/counts[side];}
        memcpy(rings[side][n].uv,side?w->uv[outfit][n]:r->uv[n],8);
        memcpy(rings[side][n].endUV,side?w->uv[outfit][(n+1)%w->count]:r->endUV[n],8);
    }
    VRWristPoint angles[128];int total=0;
    float localCentre[3]={0},normal[3],depth[3],normalLength=0;
    for(int n=0;n<w->count;++n)for(int k=0;k<3;++k)localCentre[k]+=w->ring[n][k]/w->count;
    for(int k=0;k<3;++k){normal[k]=w->point[k]-localCentre[k];normalLength+=normal[k]*normal[k];}normalLength=sqrtf(fmaxf(normalLength,.00001f));
    for(int k=0;k<3;++k)normal[k]/=normalLength;
    for(int k=0;k<3;++k)depth[k]=-normal[(k+1)%3]*w->width[(k+2)%3]+normal[(k+2)%3]*w->width[(k+1)%3];
    for(int side=0;side<2;++side){for(int n=0;n<counts[side];++n){float x=0,y=0;
            if(side){for(int k=0;k<3;++k){float d=w->ring[n][k]-localCentre[k];x+=d*w->width[k];y+=d*depth[k];}if(hand->nonNormalized)y=-y;}
            else{x=r->point[n][1]-r->centre[1];y=r->point[n][2]-r->centre[2];}
            rings[side][n].angle=atan2f(y,x);angles[total++]=rings[side][n];}
        qsort(rings[side],counts[side],sizeof(VRWristPoint),WristAngleOrder);}
    qsort(angles,total,sizeof(VRWristPoint),WristAngleOrder);
    float ambient[3]={96,96,96},directed[3]={64,64,64},lightDir[3]={0,0,1};
    typedef int (__cdecl *LightFn)(const float *,float *,float *,float *);
    ((LightFn)(gameBase+0x92f10))(base->origin,ambient,directed,lightDir);
    /* Raven's API registers shaders with an explicit lightmap index. The
     * texture-cache lookup is not a shader registration function. */
    typedef int (__cdecl *ShaderFn)(const char *,int);
    char shader[64];snprintf(shader,sizeof(shader),"sof2vr/wrist_join_%d",outfit);int material=((ShaderFn)(gameBase+0xa4cc0))(shader,-1);if(!material)return;
    typedef void (__cdecl *PolyFn)(int,int,const VRWristVertex *);PolyFn add=(PolyFn)(gameBase+0x99830);
    unsigned before=*(unsigned *)(gameBase+0x7b1df4);
    for(int n=0;n<total;++n){float a=angles[n].angle,b=angles[(n+1)%total].angle;if(n==total-1)b+=6.28318530718f;if(b-a<.00001f)continue;
        VRWristVertex v[4];WristSample(rings[0],counts[0],a,&v[0]);WristSample(rings[0],counts[0],b,&v[1]);WristSample(rings[1],counts[1],b,&v[2]);WristSample(rings[1],counts[1],a,&v[3]);
        VRWristVertex edgeEnd;WristSample(rings[0],counts[0],b-.000001f,&edgeEnd);memcpy(v[1].st,edgeEnd.st,8);
        /* Continue the native forearm skin to the glove edge. Interpolating
         * across unrelated glove/sleeve UV islands creates dark triangles. */
        memcpy(v[2].st,v[1].st,8);memcpy(v[3].st,v[0].st,8);
        float u[3],d[3],normal[3],length=0,dot=0;for(int k=0;k<3;++k){u[k]=v[1].xyz[k]-v[0].xyz[k];d[k]=v[3].xyz[k]-v[0].xyz[k];}
        for(int k=0;k<3;++k){normal[k]=u[(k+1)%3]*d[(k+2)%3]-u[(k+2)%3]*d[(k+1)%3];length+=normal[k]*normal[k];}
        if(length<1e-8f)continue;length=sqrtf(length);for(int k=0;k<3;++k)dot+=normal[k]/length*lightDir[k];dot=fmaxf(0,dot);
        for(int j=0;j<4;++j){float radius[3],norm=0;dot=0;int ring=j>=2;
            for(int k=0;k<3;++k){radius[k]=v[j].xyz[k]-centres[ring][k];norm+=radius[k]*radius[k];}norm=sqrtf(fmaxf(norm,.00001f));
            for(int k=0;k<3;++k)dot+=radius[k]/norm*lightDir[k];dot=fmaxf(0,dot);
            for(int k=0;k<3;++k)v[j].rgba[k]=(unsigned char)fminf(255,fmaxf(24,ambient[k]+directed[k]*dot));v[j].rgba[3]=255;}add(material,4,v);
    }
    static unsigned logged;
    if(tracePoses && !(logged&(1u<<physical))){logged|=1u<<physical;LOG_INFO("WRISTJOIN hand=%d outfit=%d shader=%d boundary=%d,%d polygons=%u->%u arm=%.2f,%.2f,%.2f glove=%.2f,%.2f,%.2f",physical,outfit,material,counts[0],counts[1],before,*(unsigned *)(gameBase+0x7b1df4),centres[0][0],centres[0][1],centres[0][2],centres[1][0],centres[1][1],centres[1][2]);}
}
