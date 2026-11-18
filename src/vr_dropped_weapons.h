/* Standalone mesh objects: no transient native/Ghoul2 pointers are retained. */
#define VR_DROP_COUNT 12
#include "vr_drop_profiles.h"
typedef struct {int active,id,model,settled,reflected;unsigned epoch,born,last;float origin[3],axis[3][3],velocity[3],angular[3];} VRDroppedWeapon;
static VRDroppedWeapon droppedWeapons[VR_DROP_COUNT];
static struct {unsigned time,epoch;float point[3],velocity[3],axis[3][3],angular[3];} weaponMotion;
static void TrackWeaponMotion(const BridgeTracking *sample,int valid){
    float point[3],axis[3][3];unsigned now=sample->input.sampleTimeMs;
    if(!valid || !WeaponPose(&playerBase,sample,point,axis)){weaponMotion.time=0;memset(weaponMotion.velocity,0,12);return;}
    unsigned dt=now-weaponMotion.time;
    if(weaponMotion.time && dt>0 && dt<=100 && weaponMotion.epoch==reloadEpoch){
        float v[3],length=0;for(int i=0;i<3;++i){v[i]=(point[i]-weaponMotion.point[i])*1000/dt;length+=v[i]*v[i];}
        if(length<472*472){for(int i=0;i<3;++i)weaponMotion.velocity[i]=weaponMotion.velocity[i]*.4f+v[i]*.6f;
            float w[3]={0};for(int j=0;j<3;++j)for(int k=0;k<3;++k){int a=(k+1)%3,b=(k+2)%3;w[k]+=(weaponMotion.axis[j][a]*axis[j][b]-weaponMotion.axis[j][b]*axis[j][a])*500/dt;}
            float speed=hypotf(hypotf(w[0],w[1]),w[2]);for(int k=0;k<3;++k)weaponMotion.angular[k]=w[k]*(speed>20?20/speed:1);
        }else{memset(weaponMotion.velocity,0,12);memset(weaponMotion.angular,0,12);}
    }else{memset(weaponMotion.velocity,0,12);memset(weaponMotion.angular,0,12);}
    memcpy(weaponMotion.axis,axis,36);
    memcpy(weaponMotion.point,point,12);weaponMotion.time=now;weaponMotion.epoch=reloadEpoch;
}
static int WeaponOnGround(int id){
    for(int i=0;i<VR_DROP_COUNT;++i)if(droppedWeapons[i].active && droppedWeapons[i].epoch==reloadEpoch && droppedWeapons[i].id==id)return 1;
    return 0;
}
static void ClearDroppedWeapon(int id){
    for(int i=0;i<VR_DROP_COUNT;++i)
        if(droppedWeapons[i].active && droppedWeapons[i].id==id)droppedWeapons[i].active=0;
}
static void DropHeldWeapon(const BridgeTracking *sample){
    VRWeaponState state;float grip[3],axis[3][3],origin[3];
    if(!weaponHeld || !GameVR_GetWeaponState(&state) || !WeaponPose(&playerBase,sample,grip,axis))return;
    const VRWeaponProfile *p=VR_Profile(state.weapon);if(!p || p->category<1 || p->category>8)return;
    int slot=0;for(int i=0;i<VR_DROP_COUNT;++i){if(!droppedWeapons[i].active){slot=i;break;}
        if(droppedWeapons[i].born<droppedWeapons[slot].born)slot=i;}
    char path[96];snprintf(path,sizeof(path),"models/sof2vr_dropped/w%d.md3",state.weapon);
    int model=((ModelFn)(gameBase+0x96d60))(path);
    if(!model){weaponHeld=weaponSupported=0;GameVR_CancelWeaponRequest();GameVR_CancelRoundInsertion();ResetKnifeMotion();return;}
    if(state.weapon==1)KnifeRenderAxes(axis);
    WeaponEntityOrigin(state.weapon,grip,axis,origin);
    VRDroppedWeapon *d=&droppedWeapons[slot];memset(d,0,sizeof(*d));
    d->active=1;d->id=state.weapon;d->model=model;d->reflected=leftHanded;d->epoch=reloadEpoch;d->born=d->last=GetTickCount();
    memcpy(d->origin,origin,12);memcpy(d->axis,axis,36);
    if(d->last-weaponMotion.time<100){memcpy(d->velocity,weaponMotion.velocity,12);memcpy(d->angular,weaponMotion.angular,12);}
    weaponHeld=0;weaponSupported=0;GameVR_CancelWeaponRequest();GameVR_CancelRoundInsertion();ResetKnifeMotion();
    LOG_INFO("DROP weapon=%d velocity=%.2f,%.2f,%.2f",d->id,d->velocity[0],d->velocity[1],d->velocity[2]);
}
static int GrabDroppedWeapon(const BridgeTracking *sample){
    const BridgeHand *hand=&sample->input.hands[leftHanded?0:1];float point[3],unused[3][3];
    PoseInGame(&playerBase,&hand->grip,point,unused);
    int closest=-1;float best=8;
    for(int i=0;i<VR_DROP_COUNT;++i){VRDroppedWeapon *d=&droppedWeapons[i];
        if(!d->active || d->epoch!=reloadEpoch)continue;
        const VRWeaponGrip *rig=WeaponGrip(d->id);float handle[3];
        for(int k=0;k<3;++k){handle[k]=d->origin[k];if(rig)for(int j=0;j<3;++j)handle[k]+=d->axis[j][k]*rig->point[j];}
        float distance=Distance3(handle,point);
        if(distance<best || (closest>=0 && fabsf(distance-best)<.001f && d->born>droppedWeapons[closest].born)){closest=i;best=distance;}
    }
    if(closest>=0){VRDroppedWeapon *d=&droppedWeapons[closest];
        if(GameVR_RequestWeapon(d->id)){d->active=0;weaponHeld=1;LOG_INFO("DROP pickup weapon=%d distance=%.3f",d->id,best);return 1;}}
    return 0;
}
static void UpdateDroppedWeapons(void){
    unsigned now=GetTickCount();int paused=cvarFloat("cl_paused") || *(unsigned *)(gameBase+0x1476d4);
    for(int i=0;i<VR_DROP_COUNT;++i){VRDroppedWeapon *d=&droppedWeapons[i];if(!d->active)continue;
        if(d->epoch!=reloadEpoch || now-d->born>45000){d->active=0;continue;}
        float elapsed=fminf(.05f,(now-d->last)*.001f);d->last=now;
        if(paused){d->born+=(unsigned)(elapsed*1000);continue;}
        if(d->settled)continue;
        int steps=(int)ceilf(elapsed/.008f);if(steps<1)continue;float dt=elapsed/steps;
        const VRDropProfile *profile=DropProfile(d->id);
        for(int s=0;s<steps;++s){
            float next[3],stop[3],start[3],end[3],half[3]={.5f,.5f,.5f},offset[3]={0};
            for(int k=0;k<3;++k){if(profile)for(int j=0;j<3;++j){offset[k]+=d->axis[j][k]*profile->centre[j];half[k]+=fabsf(d->axis[j][k])*profile->half[j];}
                start[k]=d->origin[k]+offset[k];next[k]=d->origin[k]+d->velocity[k]*dt;end[k]=next[k]+offset[k];d->velocity[k]+=(k==2?-800:0)*dt;}
            float speed=hypotf(hypotf(d->velocity[0],d->velocity[1]),d->velocity[2]);
            int hit=GameVR_TraceThrownWeapon(start,half,end,stop,speed>=31.5f);
            if(hit<0)break;for(int k=0;k<3;++k)d->origin[k]=stop[k]-offset[k];
            if(hit){d->settled=1;break;}
            float angularSpeed=hypotf(hypotf(d->angular[0],d->angular[1]),d->angular[2]);
            if(angularSpeed>.01f){float q[4],sinHalf=sinf(angularSpeed*dt*.5f);for(int k=0;k<3;++k)q[k]=d->angular[k]/angularSpeed*sinHalf;q[3]=cosf(angularSpeed*dt*.5f);
                for(int j=0;j<3;++j){float rotated[3];Rotate(q,d->axis[j],rotated);memcpy(d->axis[j],rotated,12);}
                if(profile)for(int k=0;k<3;++k){float rotatedOffset=0;for(int j=0;j<3;++j)rotatedOffset+=d->axis[j][k]*profile->centre[j];d->origin[k]=stop[k]-rotatedOffset;}}
        }
    }
}
static void AddDroppedWeapons(void){
    EntityFn add=(EntityFn)(gameBase+0x999f0);
    for(int i=0;i<VR_DROP_COUNT;++i){VRDroppedWeapon *d=&droppedWeapons[i];if(!d->active || d->epoch!=reloadEpoch)continue;
        GameEntity e={0};e.model=d->model;e.nonNormalized=d->reflected;memcpy(e.origin,d->origin,12);memcpy(e.axis,d->axis,36);memset(e.shaderRGBA,255,4);add(&e);
    }
}
