/* Eye-pass-only world entities. Interaction and visuals use the same body
 * slots/accessibility transform; no native inventory ownership is changed. */
#include "holster_fits.h"
static void AddStowedWeapons(const GameRefdef *base,const BridgeTracking *sample){
    if(!playerScene || !controlsEnabled || !sample->input.focused)return;
    VRWeaponState held;if(!GameVR_GetWeaponState(&held))return;
    static VRInventoryItem inventory[64];static int count;static DWORD checked;static unsigned epoch;
    DWORD now=GetTickCount();
    if(!checked || now-checked>=100 || epoch!=reloadEpoch){count=GameVR_Inventory(inventory,64);checked=now;epoch=reloadEpoch;}
    float centre[3],q[4],inverse[4],unused[3],body[3][3];BridgePose pose={0};
    BodyFrame(sample,centre,q,inverse);memcpy(pose.position,centre,12);memcpy(pose.orientation,q,16);PoseInGame(base,&pose,unused,body);
    EntityFn add=(EntityFn)(gameBase+0x999f0);ModelFn model=(ModelFn)(gameBase+0x96d60);
    for(int slot=1;slot<VR_HOLSTER_COUNT;++slot){
        if(slot==VR_HOLSTER_AMMO)continue;
        int selected=0,available=0;
        for(int i=0;i<count;++i){const VRWeaponProfile *p=VR_Profile(inventory[i].id);
            if(!p || VR_HolsterCategory(p->category)!=slot)continue;
            if(!selected || inventory[i].id==holsters.selected[slot]){selected=inventory[i].id;available=inventory[i].available;}
            if(selected==holsters.selected[slot])break;
        }
        if(!selected)continue;
        GameEntity e={0};e.flags=0x80|0x40;memcpy(e.lightingOrigin,base->origin,12);memset(e.shaderRGBA,255,4);
        BodySlotInGame(base,sample,slot,e.origin);
        float side=leftHanded?-1:1;
        /* Canonical weapon X is the bore/blade, Y left, Z up. */
        for(int k=0;k<3;++k){
            e.axis[0][k]=-body[2][k];e.axis[1][k]=side*body[1][k];e.axis[2][k]=body[0][k];
        }
        e.nonNormalized=leftHanded;
        if(slot==VR_HOLSTER_PISTOL){
            float gripPoint[3];memcpy(gripPoint,e.origin,12);
            const VRHolsterFit *fit=NULL;for(unsigned j=0;j<sizeof(vrHolsterFits)/sizeof(vrHolsterFits[0]);++j)if(vrHolsterFits[j].id==selected)fit=&vrHolsterFits[j];
            /* The exported pouch's original vertical axis is local X.
             * Place its top at the barrel entry below the exposed grip. */
            for(int k=0;k<3;++k){
                e.axis[0][k]=body[2][k]*(fit?fit->scale[2]:1);
                e.axis[1][k]=side*body[1][k]*(fit?fit->scale[1]:1);
                e.axis[2][k]=-body[0][k]*(fit?fit->scale[0]:1);
                e.origin[k]=gripPoint[k]-2.35f*body[2][k]-2.70f*e.axis[0][k]+2.35f*e.axis[2][k];
            }
            e.nonNormalized=1;
            e.model=model("models/sof2vr_holsters/pistol_holster.md3");if(e.model)add(&e);
            e.nonNormalized=leftHanded;
            for(int k=0;k<3;++k){e.origin[k]=gripPoint[k]-(fit?side*body[1][k]*fit->side+body[0][k]*fit->front:0);e.axis[0][k]=-body[2][k];e.axis[1][k]=side*body[1][k];e.axis[2][k]=body[0][k];}
        }else if(slot==VR_HOLSTER_KNIFE){
            /* Both assets are authored along +X from blade base/throat.
             * Align them vertically and put the exposed grip at the slot. */
            for(int k=0;k<3;++k){
                e.origin[k]+=4.0f*body[2][k];
                e.axis[0][k]=body[2][k];
                e.axis[1][k]=body[0][k];
                e.axis[2][k]=side*body[1][k];
            }
            GameEntity sheath=e;
            /* Seat the mouth at the guard and centre the pocket on the blade.
             * The pocket is offset across its thickness and width. */
            for(int k=0;k<3;++k)sheath.origin[k]-=.65f*e.axis[0][k]+.18f*e.axis[1][k]+.20f*e.axis[2][k];
            sheath.model=model("models/sof2vr_holsters/knife_sheath.md3");if(sheath.model)add(&sheath);
        }else if(slot==VR_HOLSTER_PRIMARY){
            for(int k=0;k<3;++k){e.origin[k]-=body[2][k]*6;
                e.axis[0][k]=body[2][k];e.axis[1][k]=side*body[1][k];e.axis[2][k]=-body[0][k];}
        }else if(slot==VR_HOLSTER_HEAVY){
            /* Long heavy guns hang down/across the back. A barrel-up M60
             * would put its deployed bipod directly above the headset. */
            for(int k=0;k<3;++k){
                e.axis[0][k]=-.94f*body[2][k]-.341174f*side*body[1][k];
                e.axis[1][k]=.94f*side*body[1][k]-.341174f*body[2][k];
                e.axis[2][k]=body[0][k];
            }
        }else if(slot==VR_HOLSTER_GRENADE){
            /* Pin up, body hanging below the front shoulder. */
            for(int k=0;k<3;++k){e.axis[0][k]=body[0][k];e.axis[1][k]=side*body[1][k];e.axis[2][k]=body[2][k];}
        }
        if((slot==VR_HOLSTER_GRENADE && !available) || (slot==VR_HOLSTER_KNIFE && offhandKnife) || (weaponHeld && held.weapon==selected))continue;
        char path[96];snprintf(path,sizeof(path),"models/sof2vr_holsters/weapon_%d.md3",selected);
        e.model=model(path);if(e.model)add(&e);
        static unsigned logged;unsigned bit=1u<<slot;
        if(!(logged&bit)){logged|=bit;LOG_INFO("STOWED slot=%d weapon=%d mesh=%d",slot,selected,e.model);}
    }
}
