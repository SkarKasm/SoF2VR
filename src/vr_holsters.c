#include "vr_holsters.h"
#include <math.h>
#include <string.h>
void VR_HolsterPosition(int slot,float out[3]){
    /* The knife slot is centred on the exposed grip, below the shoulder. */
    static const float p[VR_HOLSTER_COUNT][3]={{0,0,0},{.24f,-.64f,.02f},{.22f,-.18f,.22f},{.20f,-.22f,-.15f},{-.22f,-.18f,.22f},{-.24f,-.44f,-.18f},{0,-.50f,-.18f},{-.24f,-.64f,.02f}};
    if(slot<1 || slot>=VR_HOLSTER_COUNT)slot=0;memcpy(out,p[slot],12);
}
void VR_HolsterPositionEx(int slot,int leftHanded,float reach,float height,float out[3]){
    VR_HolsterPosition(slot,out);
    if(slot!=VR_HOLSTER_AMMO){out[0]*=(leftHanded?-1:1)*reach;out[1]*=reach;out[2]*=reach;}
    out[1]+=height;
}
void VR_HolsterPositionAccessible(int slot,int leftHanded,float reach,float height,int seated,float out[3]){
    VR_HolsterPositionEx(slot,leftHanded,reach,height,out);
    if(seated && (slot==VR_HOLSTER_PISTOL || slot==VR_HOLSTER_SMG || slot==VR_HOLSTER_AMMO))out[1]+=.20f;
}
int VR_HolsterAtOccupied(const float point[3],int leftHanded,float reach,float height,int seated,unsigned slots){
    int best=0;float distance=1;
    for(int k=0;k<3;++k)if(!isfinite(point[k]))return 0;
    for(int s=1;s<VR_HOLSTER_COUNT;++s){
        if(s==VR_HOLSTER_AMMO || !(slots&(1u<<s)))continue;
        float p[3],d=0;VR_HolsterPositionAccessible(s,leftHanded,reach,height,seated,p);
        int back=s==VR_HOLSTER_PRIMARY || s==VR_HOLSTER_HEAVY;
        for(int k=0;k<3;++k){
            float radius=back?(k==0?.23f:k==1?.30f:.25f):s==VR_HOLSTER_KNIFE?.20f:.18f;
            float offset=(point[k]-p[k])/radius;d+=offset*offset;
        }
        if(d<distance){distance=d;best=s;}
    }return best;
}
int VR_HolsterAtAccessible(const float point[3],int leftHanded,float reach,float height,int seated){
    return VR_HolsterAtOccupied(point,leftHanded,reach,height,seated,~0u);
}
int VR_HolsterAtEx(const float point[3],int leftHanded,float reach,float height){return VR_HolsterAtAccessible(point,leftHanded,reach,height,0);}
int VR_HolsterAt(const float point[3]){return VR_HolsterAtEx(point,0,1,0);}
int VR_HolsterCategory(int c){
    if(c==1)return VR_HOLSTER_KNIFE;if(c==2)return VR_HOLSTER_PISTOL;
    if(c==4)return VR_HOLSTER_SMG;if(c==3 || c==5 || c==6)return VR_HOLSTER_PRIMARY;if(c==7)return VR_HOLSTER_HEAVY;
    if(c==8)return VR_HOLSTER_GRENADE;return 0;
}
VRHolsterEvent VR_HolsterUpdate(VRHolsters *h,int valid,int grip,int nearSlot,unsigned now){
    VRHolsterEvent e={0};
    if(!valid){h->armed=h->held=h->slot=h->wheel=0;return e;}
    if(!grip){
        if(h->held){e.consume=1;if(h->wheel)e.wheelSlot=h->slot;}
        h->held=h->slot=h->wheel=0;h->armed=1;return e;
    }
    if(!h->held){
        if(!h->armed)return e;
        if(nearSlot<1 || nearSlot>=VR_HOLSTER_COUNT || nearSlot==VR_HOLSTER_AMMO)return e;
        h->armed=0;h->held=1;h->slot=nearSlot;h->started=now;
        if(nearSlot==VR_HOLSTER_KNIFE)e.drawSlot=nearSlot;
    }
    e.consume=1;
    if(!h->wheel && nearSlot!=h->slot){
        if(h->slot!=VR_HOLSTER_KNIFE)e.drawSlot=h->slot;
        h->held=h->slot=h->armed=0;
        return e;
    }
    if(h->slot!=VR_HOLSTER_KNIFE && (unsigned)(now-h->started)>=h->holdMs){
        e.opened=!h->wheel;h->wheel=1;e.wheelSlot=h->slot;
    }
    return e;
}
