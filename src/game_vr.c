/* Verified original SP server hooks. Keep NPC and game-owned state intact. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "game_vr.h"
#include "weapon_profiles.h"
#include "vr_mechanics.h"
#include "vr_log.h"
__attribute__((used)) static void *shotTrampoline,*projectileTrampoline;
__attribute__((used)) static void *weaponUpdateTrampoline;
static void WeaponUpdateHook(void);
static void ShotHook(void);
static void ScatterHook(void),MeleeHook(void);
__attribute__((used)) static void *scatterTrampoline,*meleeTrampoline;
static DWORD scatterUntil;static float scatterStart[3];static int scatterWeapon;
static void ProjectileHook(void);
static VRShotPose shotPose;
static int trackKnife;
static __thread const void *nativeBladeTrace;
static VRFXPose effectPose;
static BYTE *serverBase;static int diagnostics;
static int enemyDamagePercent=50;
__attribute__((used)) static void *damageResume;
void GameVR_SetEnemyDamagePercent(int percent){
    enemyDamagePercent=percent>=25 && percent<=100?percent:50;
}
__attribute__((used,force_align_arg_pointer)) static void ScaleEnemyDamage(uint32_t *saved){
    if(!serverBase)return;
    uint32_t *args=saved+10;
    BYTE **entities=*(BYTE ***)(serverBase+0x308cb4);
    BYTE *target=(BYTE *)(uintptr_t)args[0],*attacker=(BYTE *)(uintptr_t)args[2];
    int damage=(int)args[5];
    if(diagnostics && nativeBladeTrace && entities && target && attacker==entities[0])
        LOG_INFO("VRKNIFE native damage entity=%u amount=%d",*(unsigned *)(target+8),damage);
    if(enemyDamagePercent==100)return;
    if(!entities || !target || target!=entities[0] || !attacker || attacker==target || damage<=0)return;
    unsigned id=*(unsigned *)(attacker+8);
    /* Actor reference and native entity table identify NPC attacks. World
     * hazards, scripted world damage and self-inflicted hits keep their values. */
    if(id==0 || id>=1022 || entities[id]!=attacker)return;
    int sourceRef=*(int *)(attacker+0x1a0),targetRef=*(int *)(target+0x1a0);
    if(sourceRef==-1)return;
    typedef unsigned char (__cdecl *FriendlyFn)(int,int);
    if(((FriendlyFn)(serverBase+0x16d520))(sourceRef,targetRef))return;
    int scaled=(int)(((int64_t)damage*enemyDamagePercent+50)/100);
    args[5]=scaled>0?scaled:1;
    if(diagnostics)LOG_INFO("VR DAMAGE npc=%u incoming=%d scaled=%u",id,damage,args[5]);
}
__attribute__((naked)) static void EnemyDamageHook(void){
    __asm__ volatile("pushfl\n\tpushal\n\tpushl %esp\n\tcall _ScaleEnemyDamage\n\taddl $4,%esp\n\tpopal\n\tpopfl\n\tsubl $0x18,%esp\n\tpushl %ebx\n\tpushl %ebp\n\tpushl %esi\n\tjmp *_damageResume\n\t");
}
/* A thrown gun interrupts attacks without changing actor health, ownership,
 * team, animation or undocumented AI state. Entries expire automatically. */
static struct {BYTE *actor;unsigned until;} impactInterrupts[16];
static int ImpactInterrupted(BYTE *actor){
    if(!actor)return 0;unsigned now=GetTickCount();
    for(unsigned i=0;i<16;++i)if(impactInterrupts[i].actor==actor && (int32_t)(impactInterrupts[i].until-now)>0)return 1;
    return 0;
}
static BYTE *clientBase;
static VRWeaponState weaponState;
static int requestedWeapon,requestSteps;
static DWORD requestStarted;
static int manualWeapon,manualBlocked,fallbackReloading;static DWORD fallbackUntil;
__attribute__((used)) static void *weaponTransitionResume;
void GameVR_SetReloadControl(int weapon,int blocked,int fallback){
    int previous=manualWeapon;manualWeapon=weaponTransitionResume?weapon:0;manualBlocked=blocked;
    if(previous!=manualWeapon){fallbackUntil=0;fallbackReloading=0;}
    if(fallback<0){fallbackUntil=0;fallbackReloading=0;}
    else if(fallback && manualWeapon){fallbackUntil=GetTickCount()+5000;fallbackReloading=0;}
}
__attribute__((used,force_align_arg_pointer)) static void FilterWeaponTransition(uint32_t *saved){
    BYTE *manager=(BYTE *)(uintptr_t)saved[1];int next=(int)saved[7];
    if(!clientBase || !manualWeapon || *(BYTE **)manager!=clientBase+0x1b0a30)return;
    BYTE *owner=*(BYTE **)manager,*item=*(BYTE **)(owner+0x198);
    if(!item || *(unsigned short *)item!=manualWeapon)return;
    int assisted=fallbackUntil && (int32_t)(fallbackUntil-GetTickCount())>0;
    if(((next==5 || next==6) && !assisted) || (manualBlocked && (next==1 || next==2))){
        saved[7]=0;if(diagnostics)LOG_INFO("MANUAL transition weapon=%d requested=%d previous=%u -> idle",manualWeapon,next,saved[4]);
    }
}
/* Replace exactly call [eax]; mov ecx,[esi+74]; mov edi,eax. The native
 * transition/enter code follows unchanged with the filtered next state. */
__attribute__((naked)) static void WeaponTransitionHook(void){
    __asm__ volatile("call *(%eax)\n\tpushfl\n\tpushal\n\tmovl %esp,%eax\n\tpushl %eax\n\tcall _FilterWeaponTransition\n\taddl $4,%esp\n\tpopal\n\tpopfl\n\tmovl 0x74(%esi),%ecx\n\tmovl %eax,%edi\n\tjmp *_weaponTransitionResume\n\t");
}
static struct {int weapon,clip,reserve,rounds,result;DWORD time;} insertion;
static struct {int weapon,clip,reserve,result;DWORD time;} rack;
void GameVR_CancelRoundInsertion(void){if(insertion.weapon){insertion.weapon=0;insertion.result=-1;}rack.weapon=rack.result=0;}
void GameVR_CancelRoundInsertionFor(int weapon){if(insertion.weapon==weapon || rack.weapon==weapon)GameVR_CancelRoundInsertion();}
int GameVR_ReloadResult(void){return insertion.result;}
int GameVR_RackPending(void){return rack.weapon!=0;}
int GameVR_TakeRackResult(void){int r=rack.result;rack.result=0;return r;}
int GameVR_RackRound(int weapon,int clip,int reserve){
    VRWeaponState s;
    if(rack.weapon || insertion.weapon || !GameVR_GetWeaponState(&s) || s.weapon!=weapon || s.clip!=clip || s.reserve!=reserve || clip<1 || (s.state!=0 && s.state!=27))return 0;
    rack.weapon=weapon;rack.clip=clip;rack.reserve=reserve;rack.result=0;rack.time=GetTickCount();return 1;
}
int GameVR_InsertRounds(int weapon,int clip,int reserve,int rounds){
    VRWeaponState state;
    if(insertion.weapon || !GameVR_GetWeaponState(&state) || state.weapon!=weapon || state.clip!=clip || state.reserve!=reserve ||
       rounds<1 || rounds>state.capacity-clip || rounds>reserve || (state.state!=0 && state.state!=27))return 0;
    insertion.weapon=weapon;insertion.clip=clip;insertion.reserve=reserve;insertion.rounds=rounds;insertion.result=0;insertion.time=GetTickCount();return 1;
}
int GameVR_GetWeaponState(VRWeaponState *state){
    if(!state || !clientBase || (BYTE *)GetModuleHandleA("cgamex86.dll")!=clientBase ||
       !weaponState.sampledAt || GetTickCount()-weaponState.sampledAt>=250)return 0;
    *state=weaponState;return 1;
}
int GameVR_GetVitals(int *health,int *armor){
    if(!clientBase || (BYTE *)GetModuleHandleA("cgamex86.dll")!=clientBase)return 0;
    BYTE *owner=clientBase+0x1b0a30,*player=*(BYTE **)(owner+0x160);
    if(!owner[0x124] || !player)return 0;
    /* Same snapshot fields used by the original status page at RVA 693fc. */
    int hp=*(int *)(player+0xd0),ap=*(int *)(player+0xd4);
    if(hp< -9999 || hp>9999 || ap<0 || ap>9999)return 0;
    *health=hp>0?hp:0;*armor=ap;return 1;
}
int GameVR_GetSecondaryAmmo(const VRWeaponState *state,int *clip,int *reserve){
    if(!clientBase || (BYTE *)GetModuleHandleA("cgamex86.dll")!=clientBase)return 0;
    /* Use the native alternate-ammo definition and reserve lookup from
     * the right status page. A fire-mode toggle without its own ammo pool
     * should not duplicate the primary count. */
    typedef unsigned char (__attribute__((thiscall)) *HasAltFn)(void *,int);
    typedef unsigned short (__attribute__((thiscall)) *AmmoFn)(void *,int);
    typedef int (__attribute__((thiscall)) *CountFn)(void *,int);
    void *definitions=clientBase+0x403dfc;
    if(!((HasAltFn)(clientBase+0x52e30))(definitions,state->weapon))return 0;
    int alt=((AmmoFn)(clientBase+0x52850))(definitions,state->weapon);
    if(!alt || alt==((AmmoFn)(clientBase+0x526d0))(definitions,state->weapon))return 0;
    int total=((CountFn)(clientBase+0x5d2d0))(clientBase+0x403e00,alt);
    if(state->alternate<0 || state->alternate>200 || total<0 || total>9999)return 0;
    *clip=state->alternate;*reserve=total;return 1;
}
int GameVR_Inventory(VRInventoryItem *items,int capacity){
    if(!items || capacity<1 || !clientBase || (BYTE *)GetModuleHandleA("cgamex86.dll")!=clientBase)return 0;
    BYTE *owner=clientBase+0x1b0a30,*inventory=owner+0x170;
    if(!owner[0x124])return 0;
    typedef void *(__attribute__((thiscall)) *FindFn)(void *,void **,const unsigned short *);
    typedef const char *(__attribute__((thiscall)) *NameFn)(void *,int);
    typedef unsigned short (__attribute__((thiscall)) *AmmoFn)(void *,int);
    typedef int (__attribute__((thiscall)) *CountFn)(void *,int);
    int count=0;
    for(unsigned short id=1;id<=64 && count<capacity;++id){
        void *node=NULL;((FindFn)(clientBase+0x220a0))(inventory,&node,&id);
        if(!node || node==*(void **)(inventory+4))continue;
        BYTE *item=*(BYTE **)((BYTE *)node+0x10);
        if(!item || *(unsigned short *)item!=id)continue;
        /* A map entry establishes ownership; +0x10 is a category flag. */
        int type=((CountFn)(clientBase+0x53780))(clientBase+0x403dfc,id);
        const VRWeaponProfile *profile=VR_Profile(id);
        if(!profile || profile->category>=10 || type==3 || type==4)continue; /* native equipment toggle paths */
        int clip=*(int *)(item+4),ammo=((AmmoFn)(clientBase+0x526d0))(clientBase+0x403dfc,id);
        int reserve=((CountFn)(clientBase+0x5d2d0))(clientBase+0x403e00,ammo);
        const char *name=((NameFn)(clientBase+0x53000))(clientBase+0x403dfc,id);
        VRInventoryItem candidate={0},*out=&candidate;out->id=id;out->clip=clip<0?0:clip;out->reserve=reserve<0?0:reserve;
        out->available=profile->category!=8 || item[0x19] || clip>0 || reserve>0;
        if(name)for(unsigned k=0;k<sizeof(out->name)-1 && name[k];++k)out->name[k]=(name[k]>=32 && name[k]<127)?name[k]:'?';
        if(!out->name[0])snprintf(out->name,sizeof(out->name),"Weapon %u",id);
        int duplicate=-1;for(int i=0;i<count;++i)if(!_stricmp(items[i].name,out->name)){duplicate=i;break;}
        if(duplicate>=0){if(!items[duplicate].available && out->available)items[duplicate]=*out;}else items[count++]=*out;
    }
    return count;
}
void GameVR_CancelWeaponRequest(void){requestedWeapon=0;}
int GameVR_NativeInventoryOpen(void){
    return clientBase && (BYTE *)GetModuleHandleA("cgamex86.dll")==clientBase &&
        clientBase[0x1b0a30+0x124] && clientBase[0x1b0a30+0x2c9];
}
void GameVR_CloseNativeInventory(void){
    /* 0x6b9f0 is a TOGGLE, not a close operation. Native confirmation calls
     * it with inventory already open. A VR selection starts with it closed. */
    if(!GameVR_NativeInventoryOpen())return;
    typedef void (__attribute__((thiscall)) *ToggleInventoryFn)(void *,int);
    ((ToggleInventoryFn)(clientBase+0x6b9f0))(clientBase+0x1b0a30,-1);
    if(!GameVR_NativeInventoryOpen())LOG_INFO("WHEEL native inventory closed without toggling it open");
}
int GameVR_WeaponRequestPending(void){return requestedWeapon!=0;}
int GameVR_RequestWeapon(int id){
    VRInventoryItem items[64];int count=GameVR_Inventory(items,64),found=0;
    for(int i=0;i<count;++i)if(items[i].id==id && items[i].available)found=1;
    if(!found)return 0;
    requestedWeapon=id;requestSteps=0;requestStarted=GetTickCount();
    LOG_INFO("WHEEL weapon request=%d",id);return 1;
}
int GameVR_AdvanceWeaponRequest(void){
    if(!requestedWeapon)return 0;VRWeaponState current;DWORD now=GetTickCount();
    if(now-requestStarted>5000){LOG_WARN("WHEEL weapon request expired target=%d",requestedWeapon);requestedWeapon=0;return 0;}
    if(!GameVR_GetWeaponState(&current))return 0;
    if(current.weapon==requestedWeapon){LOG_INFO("WHEEL weapon selected=%d",current.weapon);requestedWeapon=0;return 0;}
    /* The original inventory confirmation submits newwpn%d (client RVA
     * 0x70c65 / format 0xb3410). Preserve the native transition and authority. */
    /* State 27 is scoped idle (OICW/MSG90); native ChangeFn still owns
     * acceptance and holster/draw transitions. */
    if((current.state!=0 && current.state!=27) || requestSteps)return 0;
    BYTE *owner=clientBase+0x1b0a30;void *api=*(void **)(owner+0x164);if(!api)return 0;
    void **table=*(void ***)api;uintptr_t target=(uintptr_t)table[1];
    if(!((target>=0x30001000 && target<0x300a4000)||(target>=0x10001000 && target<0x100d0000)||(serverBase && target>=0x20001000 && target<0x201cb000))){requestedWeapon=0;return 0;}
    typedef void (__attribute__((thiscall)) *NativeCommandFn)(void *,const char *);
    typedef int (__attribute__((thiscall)) *ClockFn)(void *);
    /* ChangeFn returns the selected ID even when its action helper declines
     * the holster during the native post-equip delay. Wait on that same clock
     * before submitting, rather than treating the ID as a completed action. */
    const uint32_t clock=(uint32_t)((ClockFn)table[0x140/4])(api);
    uint32_t *equippedAt=(uint32_t *)(owner+0x16c),*secondaryAt=(uint32_t *)(owner+0x28c);
    const int equipDelay=*(int *)(clientBase+0xb2060);
    /* Both fields record past events. Map changes and old saves can leave
     * them ahead of the new clock, blocking selection for the old map age. */
    if((int32_t)(clock-*equippedAt)<0){
        LOG_INFO("WHEEL rebased old equip clock=%u current=%u",*equippedAt,clock);
        *equippedAt=clock-(uint32_t)equipDelay;
    }
    if((int32_t)(clock-*secondaryAt)<0){
        LOG_INFO("WHEEL rebased old secondary clock=%u current=%u",*secondaryAt,clock);
        *secondaryAt=clock-*(uint32_t *)(clientBase+0xb20a0);
    }
    if((int32_t)(clock-*equippedAt)<equipDelay)return 0;
    char command[48];snprintf(command,sizeof(command),"newwpn%d\n",requestedWeapon);
    /* Match native inventory confirmation (0x70a8f,0x70b58,0x70c13):
     * choose a pending primary weapon and ask the original state machine to
     * holster/draw it before forwarding the server command. No ammo edits. */
    *(unsigned short *)(owner+0x2e4)=(unsigned short)requestedWeapon;
    *(unsigned short *)(owner+0x2e6)=0;
    typedef unsigned short (__attribute__((thiscall)) *ChangeFn)(void *,int,int);
    int accepted=((ChangeFn)(clientBase+0x61e50))(owner,-1,0);
    if(accepted!=requestedWeapon)return 0;
    if(diagnostics){BYTE *manager=*(BYTE **)(owner+0x150),*nativeState=*(BYTE **)(manager+0x74);
        LOG_INFO("WHEEL native acceptance=%d flags=%08x state=%u",accepted,*(unsigned *)(manager+0x70),*(unsigned *)(nativeState+4));}
    ((NativeCommandFn)table[1])(api,command);
    GameVR_CloseNativeInventory();
    ++requestSteps;return requestedWeapon;
}
__attribute__((used,force_align_arg_pointer)) static void ObserveWeapon(uint32_t *saved){
    BYTE *manager=(BYTE *)(uintptr_t)saved[6];
    if(!clientBase)return;
    BYTE *owner=*(BYTE **)manager;
    if(owner!=clientBase+0x1b0a30 || *(BYTE **)(owner+0x150)!=manager)return;
    BYTE *item=*(BYTE **)(owner+0x198),*state=*(BYTE **)(manager+0x74);
    if(!item || !state)return;
    typedef unsigned short (__attribute__((thiscall)) *AmmoIdFn)(void *,int);
    typedef int (__attribute__((thiscall)) *LookupFn)(void *,int);
    int weapon=*(unsigned short *)item,clip=*(int *)(item+4),alt=*(int *)(item+8),stateId=*(int *)(state+4);
    int ammo=((AmmoIdFn)(clientBase+0x526d0))(clientBase+0x403dfc,weapon);
    int reserve=((LookupFn)(clientBase+0x5d2d0))(clientBase+0x403e00,ammo);
    int capacity=((LookupFn)(clientBase+0x52bf0))(clientBase+0x403dfc,weapon);
    if(rack.weapon){
        int accepted=0;
        if(weapon==rack.weapon && clip==rack.clip && reserve==rack.reserve && clip>0 && (stateId==0 || stateId==27) &&
           owner[0x124] && !owner[0x3d0] && *(int *)(owner+0x270)!=1 && GetTickCount()-rack.time<250 &&
           *(BYTE **)(owner+0x170+0x28)==item){
            /* Verified native primary clip setter, RVA 4e070. This is the
             * same inventory object used by native reload; reserve untouched. */
            typedef void (__attribute__((thiscall)) *SetClipFn)(void *,int);
            ((SetClipFn)(clientBase+0x4e070))(owner+0x170,clip-1);
            accepted=*(int *)(item+4)==clip-1;clip=*(int *)(item+4);
        }
        LOG_INFO("RACK native weapon=%d clip=%d reserve=%d accepted=%d",weapon,clip,reserve,accepted);
        rack.weapon=0;rack.result=accepted?1:-1;
    }
    if(manualWeapon==weapon){
        if(manualBlocked || clip==0)*(unsigned *)(manager+0x70)&=~3u;
        if(stateId==5 || stateId==6)fallbackReloading=1;
        else if(fallbackReloading && stateId==0){fallbackUntil=0;fallbackReloading=0;}
    }
    if(insertion.weapon){
        int accepted=0;
        if(weapon==insertion.weapon && clip==insertion.clip && reserve==insertion.reserve &&
           (stateId==0 || stateId==27) && owner[0x124] && !owner[0x3d0] && *(int *)(owner+0x270)!=1 &&
           GetTickCount()-insertion.time<250 && insertion.rounds>0 && insertion.rounds<=capacity-clip && insertion.rounds<=reserve){
            /* Native reload-shell operation: owner thiscall(manager, count).
             * 0x73900 checks capacity/reserve, calls 0x68850 to update reserve
             * through the game interface and 0x4e070 for the selected clip. */
            typedef unsigned char (__attribute__((thiscall)) *InsertFn)(void *,void *,int);
            accepted=((InsertFn)(clientBase+0x73900))(owner,manager,insertion.rounds)!=0;
            int afterClip=*(int *)(item+4),afterReserve=((LookupFn)(clientBase+0x5d2d0))(clientBase+0x403e00,ammo);
            accepted=accepted && afterClip-clip==insertion.rounds && reserve-afterReserve==insertion.rounds;
            LOG_INFO("MANUAL native weapon=%d before=%d,%d after=%d,%d rounds=%d accepted=%d",weapon,clip,reserve,afterClip,afterReserve,insertion.rounds,accepted);
            clip=afterClip;reserve=afterReserve;
        }else LOG_WARN("MANUAL native rejected stale context weapon=%d state=%d",weapon,stateId);
        insertion.result=accepted?1:-1;insertion.weapon=0;
    }
    if(weapon<1 || weapon>64 || clip<0 || reserve<0 || reserve>100000 || capacity<1 || capacity>200 || clip>capacity || stateId<0 || stateId>54)return;
    if(diagnostics && (weapon!=weaponState.weapon || clip!=weaponState.clip || alt!=weaponState.alternate || stateId!=weaponState.state || reserve!=weaponState.reserve))
        LOG_INFO("WEAPONSTATE weapon=%d clip=%d reserve=%d capacity=%d alt=%d state=%d",weapon,clip,reserve,capacity,alt,stateId);
    weaponState=(VRWeaponState){weapon,clip,reserve,capacity,alt,stateId,GetTickCount()};
    /* Submit before the native manager consumes its action bits. */
    int selected=GameVR_AdvanceWeaponRequest();
    if(selected)LOG_INFO("WHEEL native inventory request=newwpn%d",selected);
}
__attribute__((naked)) static void WeaponUpdateHook(void){
    __asm__ volatile("pushfl\n\tpushal\n\tmovl %esp,%eax\n\tpushl %eax\n\tcall _ObserveWeapon\n\taddl $4,%esp\n\tpopal\n\tpopfl\n\tjmp *_weaponUpdateTrampoline\n\t");
}
typedef void (__cdecl *TraceFn)(void *,const float *,const float *,const float *,const float *,int,int,int,int,int);
static TraceFn realTrace;
static __thread const float *nativeBladeStart;
static __thread float expectedStart[3];static __thread int expectedHit;
static void __cdecl TraceHook(void *result,const float *start,const float *mins,const float *maxs,const float *end,int pass,int mask,int ghoul2,int flags,int lod){
    /* The native melee frame has room for the base trace (0x44 bytes)
     * plus sixteen 0x40-byte Ghoul2 contacts. Consume the exact hit once. */
    if(nativeBladeTrace && start==nativeBladeStart){
        memcpy(result,nativeBladeTrace,0x444);
        nativeBladeStart=NULL;
        return;
    }
    realTrace(result,start,mins,maxs,end,pass,mask,ghoul2,flags,lod);
    if(diagnostics && GetTickCount()-scatterUntil<100){
        float error=0;for(int i=0;i<3;++i)error+=fabsf(start[i]-scatterStart[i]);
        if(error<.1f){float fraction;memcpy(&fraction,(BYTE *)result+4,4);float *point=(float *)((BYTE *)result+12);
            LOG_INFO("VRPELLET weapon=%d start=%.3f,%.3f,%.3f end=%.3f,%.3f,%.3f fraction=%.6f point=%.3f,%.3f,%.3f",scatterWeapon,start[0],start[1],start[2],end[0],end[1],end[2],fraction,point[0],point[1],point[2]);}
    }
    if(!diagnostics || !expectedHit)return;
    float difference=0;for(int i=0;i<3;++i)difference+=fabsf(start[i]-expectedStart[i]);
    if(difference<.1f){
        float fraction,*point=(float *)((BYTE *)result+12);memcpy(&fraction,(BYTE *)result+4,4);
        LOG_INFO("VRHIT start=%.3f,%.3f,%.3f fraction=%.6f point=%.3f,%.3f,%.3f muzzle=%.3f,%.3f,%.3f",start[0],start[1],start[2],fraction,point[0],point[1],point[2],expectedStart[0],expectedStart[1],expectedStart[2]);expectedHit=0;
    }
}

int GameVR_Ready(void){
    if(!serverBase || (BYTE *)GetModuleHandleA("gamex86.dll")!=serverBase || !shotTrampoline || !projectileTrampoline || !scatterTrampoline || !meleeTrampoline || !realTrace)return 0;
    return serverBase[0xfbe70]==0xe9 && serverBase+0xfbe75+*(int32_t *)(serverBase+0xfbe71)==(BYTE *)ShotHook &&
        serverBase[0xfcd40]==0xe9 && serverBase+0xfcd45+*(int32_t *)(serverBase+0xfcd41)==(BYTE *)ProjectileHook &&
        serverBase[0xfc580]==0xe9 && serverBase+0xfc585+*(int32_t *)(serverBase+0xfc581)==(BYTE *)ScatterHook &&
        serverBase[0xfc390]==0xe9 && serverBase+0xfc395+*(int32_t *)(serverBase+0xfc391)==(BYTE *)MeleeHook;
}
void GameVR_ClampMuzzle(const float head[3],float muzzle[3]){
    if(!serverBase)return;
    TraceFn trace=realTrace;
    if((uintptr_t)trace!=0x1001b260)return;
    /* The original trace includes up to sixteen Ghoul2 collision records.
     * Use an oversized aligned buffer; fraction/endpos are verified +4/+12. */
    uint32_t result[512]={0};trace(result,head,NULL,NULL,muzzle,0,0x1385,0,0,0);
    float fraction;memcpy(&fraction,(BYTE *)result+4,4);
    if(!isfinite(fraction) || fraction<0 || fraction>1){memcpy(muzzle,head,12);return;}
    if(fraction<1){
        float distance=0,delta[3];for(int i=0;i<3;++i){delta[i]=muzzle[i]-head[i];distance+=delta[i]*delta[i];}
        distance=sqrtf(distance);float t=fmaxf(0,fraction-(distance>0?2.0f/distance:0));
        for(int i=0;i<3;++i)muzzle[i]=head[i]+delta[i]*t;
        if(diagnostics)LOG_INFO("MUZZLE obstruction fraction=%.4f clamped=%.3f,%.3f,%.3f",fraction,muzzle[0],muzzle[1],muzzle[2]);
    }
}
int GameVR_TraceAim(const float origin[3],const float forward[3],float target[3]){
    if(!serverBase || !realTrace)return 0;float end[3];for(int i=0;i<3;++i){end[i]=origin[i]+forward[i]*8192;if(!isfinite(end[i]))return 0;}
    uint32_t result[512]={0};realTrace(result,origin,NULL,NULL,end,0,0x1385,0,0,0);float fraction;memcpy(&fraction,(BYTE *)result+4,4);
    if(!isfinite(fraction) || fraction<0 || fraction>1)return 0;memcpy(target,(BYTE *)result+12,12);
    for(int i=0;i<3;++i)if(!isfinite(target[i]))return 0;return 1;
}
int GameVR_TraceMagazine(const float start[3],const float half[3],const float end[3],float stop[3]){
    if(!serverBase || (BYTE *)GetModuleHandleA("gamex86.dll")!=serverBase || !realTrace)return -1;
    float mins[3],maxs[3];for(int i=0;i<3;++i){
        if(!isfinite(start[i]) || !isfinite(end[i]) || !isfinite(half[i]) || half[i]<0 || half[i]>40)return -1;
        mins[i]=-half[i];maxs[i]=half[i];
    }
    uint32_t result[512]={0};realTrace(result,start,mins,maxs,end,0,1,0,0,0);
    float fraction;memcpy(&fraction,(BYTE *)result+4,4);if(!isfinite(fraction) || fraction<0 || fraction>1)return -1;
    for(int i=0;i<3;++i)stop[i]=start[i]+(end[i]-start[i])*fraction;
    return fraction<1;
}
int GameVR_TraceThrownWeapon(const float start[3],const float half[3],const float end[3],float stop[3],int interrupt){
    if(!serverBase || (BYTE *)GetModuleHandleA("gamex86.dll")!=serverBase || !realTrace)return -1;
    float mins[3],maxs[3];for(int i=0;i<3;++i){
        if(!isfinite(start[i]) || !isfinite(end[i]) || !isfinite(half[i]) || half[i]<0 || half[i]>80)return -1;
        mins[i]=-half[i];maxs[i]=half[i];
    }
    uint32_t result[512]={0};realTrace(result,start,mins,maxs,end,0,0x1385,0,0,0);
    float fraction;memcpy(&fraction,(BYTE *)result+4,4);if(!isfinite(fraction) || fraction<0 || fraction>1)return -1;
    for(int i=0;i<3;++i)stop[i]=start[i]+(end[i]-start[i])*fraction;
    /* Trace entity +38, entity table +308cb4, actor id +8 and damageable
     * byte +269 independently observed in native melee at fc46a..fc498.
     * Heap reads remain bounded even when an entity is being destroyed. */
    if(fraction<1 && interrupt){
        unsigned id=result[0x38/4];BYTE **table=*(BYTE ***)(serverBase+0x308cb4),*actor=NULL;SIZE_T got;
        unsigned char prefix[0x26a];
        if(id>0 && id<1022 && table && ReadProcessMemory(GetCurrentProcess(),table+id,&actor,sizeof(actor),&got) && actor &&
           ReadProcessMemory(GetCurrentProcess(),actor,prefix,sizeof(prefix),&got) && *(unsigned *)(prefix+8)==id && prefix[0x269]){
            int slot=0;unsigned now=GetTickCount();
            for(int i=0;i<16;++i){if(impactInterrupts[i].actor==actor || (int32_t)(impactInterrupts[i].until-now)<=0){slot=i;break;}}
            impactInterrupts[slot].actor=actor;impactInterrupts[slot].until=now+1500;
            LOG_INFO("THROW impact actor=%u attack-interrupt-ms=1500 health-change=0",id);
        }
    }
    return fraction<1;
}
int GameVR_GrenadeAim(int weapon,int alternate,const float origin[3],const float aim[3],float direction[3],float target[3]){
    const VRWeaponProfile *profile=VR_Profile(weapon);if(!profile || profile->category!=8 || !GameVR_TraceAim(origin,aim,target))return 0;
    typedef float (__cdecl *GravityFn)(const char *);float gravity=((GravityFn)(uintptr_t)0x10036590)("g_gravity");
    if(!isfinite(gravity) || gravity<0 || gravity>10000)return 0;int alt=alternate?1:0;
    return VR_LobDirection(origin,target,profile->speed[alt],gravity*profile->gravity[alt],direction);
}
float GameVR_RecoilPitch(void){
    if(!serverBase || (BYTE *)GetModuleHandleA("gamex86.dll")!=serverBase)return 0;BYTE *player=*(BYTE **)(serverBase+0x30f638);if(!player)return 0;
    float pitch=*(float *)(player+0xb0);
    return isfinite(pitch) && pitch>=-80 && pitch<=80?pitch:0;
}
static void OriginalAimBasis(float yaw,float pitch,float axis[3][3]){
    float cy=cosf(yaw),sy=sinf(yaw),cp=cosf(pitch),sp=sinf(pitch);
    float basis[3][3]={{cp*cy,cp*sy,-sp},{-sy,cy,0},{sp*cy,sp*sy,cp}};
    memcpy(axis,basis,sizeof(basis));
}
__attribute__((used,force_align_arg_pointer)) static int PatchShotArguments(uint32_t *saved) {
    /* pushfd/pushad: EDI, ESI, EBP, savedESP, EBX, EDX, ECX, EAX,
     * EFLAGS, return address, then the original hitscan arguments. */
    static __thread float vectors[16][2][3];static __thread unsigned slot;
    if(!shotTrampoline || !serverBase || !shotPose)return 1;
    uint32_t *a=saved+10;BYTE *actor=(BYTE *)(uintptr_t)a[1],*fire=(BYTE *)(uintptr_t)a[0];
    if(ImpactInterrupted(actor))return 0;
    if(!actor || *(unsigned *)(actor+8)!=0 || !fire)return 1;
    float muzzle[3],axis[3][3];int status=shotPose(muzzle,axis);
    if(status<0)return 1;if(!status){if(diagnostics)LOG_INFO("VRSHOT suppressed: tracking or gameplay unavailable");return 0;}
    BYTE *player=*(BYTE **)(serverBase+0x30f638);if(!player)return 1;
    float yaw=*(float *)(player+0xb4)*.0174532925199433f;
    float pitch=GameVR_RecoilPitch()*.0174532925199433f,oldAxis[3][3];OriginalAimBasis(yaw,pitch,oldAxis);
    float *direction=(float *)(uintptr_t)a[3],spread[3]={0};
    float (*out)[3]=vectors[slot++%16];
    for(int k=0;k<3;++k)for(int i=0;i<3;++i)spread[k]+=direction[i]*oldAxis[k][i];
    /* Controller barrel aim is authoritative. Shotgun pellet dispersion is
     * still generated by the native scatter function after this centre ray. */
    spread[0]=1;spread[1]=spread[2]=0;
    for(int i=0;i<3;++i){out[0][i]=muzzle[i];out[1][i]=0;for(int k=0;k<3;++k)out[1][i]+=spread[k]*axis[k][i];if(!isfinite(out[1][i]))return 0;}
    a[2]=(uint32_t)(uintptr_t)out[0];a[3]=(uint32_t)(uintptr_t)out[1];
    memcpy(expectedStart,out[0],12);expectedHit=1;
    int range=*(int *)(fire+0x60);if(range<1 || range>131072)range=8192;
    float end[3],distance=0;for(int i=0;i<3;++i){end[i]=out[0][i]+out[1][i]*range;distance+=out[1][i]*out[1][i];}
    if(diagnostics)LOG_INFO("VRSHOT actor=0 start=%.3f,%.3f,%.3f end=%.3f,%.3f,%.3f direction=%.5f,%.5f,%.5f aim=%.5f,%.5f,%.5f spread=%.5f,%.5f,%.5f range=%.2f",
        out[0][0],out[0][1],out[0][2],end[0],end[1],end[2],out[1][0],out[1][1],out[1][2],axis[0][0],axis[0][1],axis[0][2],spread[0],spread[1],spread[2],sqrtf(distance)*range);

    return 1;
}
__attribute__((used,force_align_arg_pointer)) static int PatchScatterArguments(uint32_t *saved){
    uint32_t adapted[16];memcpy(adapted,saved,40);memcpy(adapted+10,saved+11,24);
    if(!PatchShotArguments(adapted))return 0;
    saved[13]=adapted[12];saved[14]=adapted[13];
    BYTE *actor=(BYTE *)(uintptr_t)saved[12],*fire=(BYTE *)(uintptr_t)saved[11];
    if(diagnostics && actor && *(unsigned *)(actor+8)==0 && fire){
        scatterWeapon=*(unsigned short *)(fire+2);memcpy(scatterStart,(void *)(uintptr_t)saved[13],12);scatterUntil=GetTickCount();
        LOG_INFO("VRSCATTER weapon=%d pellets=%u",scatterWeapon,saved[10]);
    }
    return 1;
}
__attribute__((naked)) static void ScatterHook(void){
    __asm__ volatile("pushfl\n\tpushal\n\tmovl %esp,%eax\n\tpushl %eax\n\tcall _PatchScatterArguments\n\taddl $4,%esp\n\ttestl %eax,%eax\n\tjz 1f\n\tpopal\n\tpopfl\n\tjmp *_scatterTrampoline\n\t1: popal\n\tpopfl\n\txorl %eax,%eax\n\tret $28\n\t");
}
__attribute__((used,force_align_arg_pointer)) static int PatchMeleeArguments(uint32_t *saved){
    uint32_t *a=saved+10;BYTE *actor=(BYTE *)(uintptr_t)a[1],*fire=(BYTE *)(uintptr_t)a[0];
    if(ImpactInterrupted(actor))return 0;
    /* Keyboard or native attack animations cannot deal a second, camera-
     * directed hit. Confirmed VR contact calls the original body directly. */
    return !trackKnife || !actor || *(unsigned *)(actor+8)!=0 || !fire || *(unsigned short *)(fire+2)!=1;
}
__attribute__((naked)) static void MeleeHook(void){
    __asm__ volatile("pushfl\n\tpushal\n\tmovl %esp,%eax\n\tpushl %eax\n\tcall _PatchMeleeArguments\n\taddl $4,%esp\n\ttestl %eax,%eax\n\tjz 1f\n\tpopal\n\tpopfl\n\tjmp *_meleeTrampoline\n\t1: popal\n\tpopfl\n\txorl %eax,%eax\n\tret $24\n\t");
}
static int KnifeVisible(const float head[3],const float point[3]){
    uint32_t trace[512]={0};
    realTrace(trace,head,NULL,NULL,point,0,1,0,0,0);
    float fraction;memcpy(&fraction,(BYTE *)trace+4,4);
    return isfinite(fraction) && fraction>=.999f && fraction<=1;
}
void GameVR_BladeContact(const float head[3],const VRBladePose *from,const VRBladePose *to,int steps,float radius,int *lastContact){
    if(!trackKnife || !GameVR_Ready() || steps<1 || steps>100 || !isfinite(radius) || radius<.1f || radius>1.5f)return;
    BYTE **entities=*(BYTE ***)(serverBase+0x308cb4);
    BYTE *actor=entities?entities[0]:NULL;
    if(!actor || *(unsigned *)(actor+8)!=0 || !actor[0x269] || !KnifeVisible(head,to->heel)){*lastContact=-1;return;}
    float mins[3]={-radius,-radius,-radius},maxs[3]={radius,radius,radius};
    int touching=-1;
    for(int n=1;n<=steps;++n){
        float t=(float)n/steps,start[3],end[3],direction[3],squared=0;
        for(int i=0;i<3;++i){
            start[i]=from->heel[i]+t*(to->heel[i]-from->heel[i]);
            end[i]=from->tip[i]+t*(to->tip[i]-from->tip[i]);
            direction[i]=end[i]-start[i];squared+=direction[i]*direction[i];
        }
        if(!isfinite(squared) || squared<.01f || squared>40*40)continue;
        uint32_t trace[512]={0};
        for(int j=0;j<16;++j){trace[(0x44+j*0x40)/4]=0x47c35000u;trace[(0x48+j*0x40)/4]=1023;}
        realTrace(trace,start,mins,maxs,end,0,0x1385,1,1,0);
        float fraction,point[3];memcpy(&fraction,(BYTE *)trace+4,4);
        unsigned entity=trace[0x38/4];
        if(!isfinite(fraction) || fraction<0 || fraction>=1 || !entity || entity>=1023)continue;
        for(int i=0;i<3;++i)point[i]=start[i]+fraction*direction[i];
        /* A blade behind a wall cannot hit a target on the other side. */
        if(!KnifeVisible(head,start) || (entity!=1022 && !KnifeVisible(head,point)))continue;
        touching=(int)entity;
        if(touching==*lastContact)continue;
        float length=sqrtf(squared);
        for(int i=0;i<3;++i)direction[i]/=length;
        /* Native melee fire-info prefix and stdcall signature, verified at
         * gamex86+0xfb6d3. Use the contact's bounded segment and radius. */
        uint32_t fire[0x64/4]={0};
        fire[0]=1u<<16;
        fire[2]=10000;
        fire[0x14/4]=1;
        fire[0x1c/4]=(uintptr_t)actor;
        fire[0x58/4]=1;
        fire[0x60/4]=(uint32_t)ceilf(length);
        nativeBladeStart=start;nativeBladeTrace=trace;
        typedef void (__stdcall *MeleeFn)(uint32_t *,BYTE *,const float *,const float *,float,int);
        ((MeleeFn)meleeTrampoline)(fire,actor,start,direction,1.0f,0);
        nativeBladeStart=NULL;nativeBladeTrace=NULL;
        if(diagnostics)LOG_INFO("VRKNIFE contact entity=%u sample=%d/%d point=%.3f,%.3f,%.3f",entity,n,steps,point[0],point[1],point[2]);
        break;
    }
    *lastContact=touching;
}
__attribute__((used,force_align_arg_pointer)) static int PatchProjectileArguments(uint32_t *saved){
    static __thread float vectors[16][3][3];static __thread unsigned slot;
    if(!projectileTrampoline || !shotPose || !serverBase)return 1;
    uint32_t *a=saved+10;BYTE *actor=(BYTE *)(uintptr_t)a[6];
    if(ImpactInterrupted(actor))return 0;
    if(!actor || *(unsigned *)(actor+8)!=0)return 1;
    float muzzle[3],axis[3][3];int status=shotPose(muzzle,axis);
    if(status<0)return 1;if(!status){if(diagnostics)LOG_INFO("VRSHOT suppressed: tracking or gameplay unavailable");return 0;}
    BYTE *player=*(BYTE **)(serverBase+0x30f638);if(!player)return 1;
    float yaw=*(float *)(player+0xb4)*0.0174532925199433f;
    if(!isfinite(yaw))return 1;
    float pitch=GameVR_RecoilPitch()*0.0174532925199433f,oldAxis[3][3];OriginalAimBasis(yaw,pitch,oldAxis);
    float *angles=(float *)(uintptr_t)a[3],*direction=(float *)(uintptr_t)a[4],local[3]={0};
    float (*out)[3]=vectors[slot++%16];
    for(int k=0;k<3;++k)for(int i=0;i<3;++i)local[k]+=direction[i]*oldAxis[k][i];
    for(int i=0;i<3;++i){out[0][i]=muzzle[i];out[2][i]=axis[0][i];}
    out[1][0]=angles[0]-pitch*57.2957795131f+atan2f(-axis[0][2],sqrtf(axis[0][0]*axis[0][0]+axis[0][1]*axis[0][1]))*57.2957795131f;
    out[1][1]=angles[1]-yaw*57.2957795131f+atan2f(axis[0][1],axis[0][0])*57.2957795131f;
    out[1][2]=angles[2];
    for(int v=0;v<3;++v)for(int i=0;i<3;++i)if(!isfinite(out[v][i]))return 0;
    const VRWeaponProfile *profile=VR_Profile((int)a[0]);
    if(profile && profile->category==8){
        float target[3];int solved=GameVR_GrenadeAim((int)a[0],(int)a[1],out[0],axis[0],out[2],target);if(!solved)return 0;
        out[1][0]=atan2f(-out[2][2],hypotf(out[2][0],out[2][1]))*57.2957795131f;out[1][1]=atan2f(out[2][1],out[2][0])*57.2957795131f;
        if(diagnostics)LOG_INFO("VRGRENADE weapon=%u alt=%u mode=reticle solution=%d target=%.3f,%.3f,%.3f speed=%.1f direction=%.6f,%.6f,%.6f",a[0],a[1],solved,target[0],target[1],target[2],profile->speed[a[1]?1:0],out[2][0],out[2][1],out[2][2]);
    }
    a[2]=(uint32_t)(uintptr_t)out[0];a[3]=(uint32_t)(uintptr_t)out[1];a[4]=(uint32_t)(uintptr_t)out[2];
    if(diagnostics)LOG_INFO("VRPROJECTILE actor=0 weapon=%u alt=%u start=%.3f,%.3f,%.3f direction=%.5f,%.5f,%.5f aim=%.5f,%.5f,%.5f",a[0],a[1],out[0][0],out[0][1],out[0][2],out[2][0],out[2][1],out[2][2],axis[0][0],axis[0][1],axis[0][2]);

    return 1;
}
__attribute__((naked)) static void ProjectileHook(void){
    __asm__ volatile("pushfl\n\tpushal\n\tmovl %esp,%eax\n\tpushl %eax\n\tcall _PatchProjectileArguments\n\taddl $4,%esp\n\ttestl %eax,%eax\n\tjz 1f\n\tpopal\n\tpopfl\n\tjmp *_projectileTrampoline\n\t1: popal\n\tpopfl\n\txorl %eax,%eax\n\tret $36\n\t");
}
__attribute__((naked)) static void ShotHook(void) {
    __asm__ volatile("pushfl\n\tpushal\n\tmovl %esp,%eax\n\tpushl %eax\n\tcall _PatchShotArguments\n\taddl $4,%esp\n\ttestl %eax,%eax\n\tjz 1f\n\tpopal\n\tpopfl\n\tjmp *_shotTrampoline\n\t1: popal\n\tpopfl\n\txorl %eax,%eax\n\tret $24\n\t");
}
static void InstallClientWeaponHook(void){
    BYTE *current=(BYTE *)GetModuleHandleA("cgamex86.dll");
    if(current && current==clientBase && weaponUpdateTrampoline && current[0x82330]==0xe9 && current+0x82335+*(int32_t *)(current+0x82331)==(BYTE *)WeaponUpdateHook)return;
    clientBase=NULL;memset(&weaponState,0,sizeof(weaponState));memset(&insertion,0,sizeof(insertion));memset(&rack,0,sizeof(rack));
    const BYTE updatePrologue[]={0x56,0x8b,0xf1,0x8a,0x46,0x68};
    BYTE *client=(BYTE *)GetModuleHandleA("cgamex86.dll");int clientMatched=0;
    if(client && (uintptr_t)client==0x30000000){
        PIMAGE_DOS_HEADER cdos=(PIMAGE_DOS_HEADER)client;PIMAGE_NT_HEADERS cnt=(PIMAGE_NT_HEADERS)(client+cdos->e_lfanew);
        if(cdos->e_magic==IMAGE_DOS_SIGNATURE && cnt->Signature==IMAGE_NT_SIGNATURE && cnt->FileHeader.TimeDateStamp==0x3db456cf && cnt->OptionalHeader.SizeOfImage==0x412000){
            IMAGE_SECTION_HEADER *cs=IMAGE_FIRST_SECTION(cnt);
            for(unsigned j=0;j<cnt->FileHeader.NumberOfSections;++j)if(!memcmp(cs[j].Name,".text",5) && cs[j].VirtualAddress==0x1000 && cs[j].SizeOfRawData==667648){
                uint32_t hash=2166136261u;for(unsigned k=0;k<cs[j].SizeOfRawData;++k)hash=(hash^client[cs[j].VirtualAddress+k])*16777619u;
                clientMatched=hash==0x9ba87b8b;
            }
        }
    }
    if(clientMatched && !memcmp(client+0x82330,updatePrologue,6)){
        BYTE *t=VirtualAlloc(NULL,16,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(t){memcpy(t,updatePrologue,6);t[6]=0xe9;*(int32_t *)(t+7)=(int32_t)((client+0x82336)-(t+11));
            DWORD protect;
            if(VirtualProtect(t,16,PAGE_EXECUTE_READ,&protect) && VirtualProtect(client+0x82330,6,PAGE_EXECUTE_READWRITE,&protect)){
                clientBase=client;weaponUpdateTrampoline=t;client[0x82330]=0xe9;*(int32_t *)(client+0x82331)=(int32_t)((BYTE *)WeaponUpdateHook-(client+0x82335));client[0x82335]=0x90;
                VirtualProtect(client+0x82330,6,protect,&protect);FlushInstructionCache(GetCurrentProcess(),client+0x82330,6);
                const BYTE transition[]={0xff,0x10,0x8b,0x4e,0x74,0x8b,0xf8};
                if(!memcmp(client+0x82349,transition,7) && VirtualProtect(client+0x82349,7,PAGE_EXECUTE_READWRITE,&protect)){
                    weaponTransitionResume=client+0x82350;client[0x82349]=0xe9;*(int32_t *)(client+0x8234a)=(int32_t)((BYTE *)WeaponTransitionHook-(client+0x8234e));client[0x8234e]=client[0x8234f]=0x90;
                    VirtualProtect(client+0x82349,7,protect,&protect);FlushInstructionCache(GetCurrentProcess(),client+0x82349,7);
                }
                LOG_INFO("Verified local player weapon observer and manual reload transition adapter installed");
            }else VirtualFree(t,0,MEM_RELEASE);
        }
    }
}
typedef void (__attribute__((thiscall)) *NativeEffectFn)(void *,const char *,const float *,const float (*)[3],int,int,int);
typedef void (__attribute__((thiscall)) *NativeBoltedEffectFn)(void *,const char *,void *);
static int WeaponEffectKind(uintptr_t caller,const char *name){
    if(caller==0x8d3c2 || caller==0x8d5d8)return 1;
    return name && strstr(name,"_mm1")?2:0;
}
static void __attribute__((thiscall,force_align_arg_pointer)) WeaponEffectHook(void *manager,const char *name,const float *origin,const float (*axis)[3],int bolt,int entity,int volume){
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)clientBase;
    int ejection=WeaponEffectKind(caller,name);
    float point[3],basis[3][3];int result=effectPose?effectPose(ejection,point,basis):-1;
    if(!result)return;
    if(result>0){origin=point;axis=basis;bolt=entity=-1;}
    static unsigned logged;
    if(result>0 && (diagnostics || logged<12)){++logged;LOG_INFO("VRFX emit site=%lx kind=%d name=%s world=%.3f,%.3f,%.3f",(unsigned long)caller,ejection,name?name:"",point[0],point[1],point[2]);}
    ((NativeEffectFn)(clientBase+0x3e8d0))(manager,name,origin,axis,bolt,entity,volume);
}
static void __attribute__((thiscall,force_align_arg_pointer)) WeaponBoltedEffectHook(void *manager,const char *name,void *attachment){
    /* The inview fire routine normally gives the effect manager a persistent
     * flatscreen bolt object. Supplying a corrected point to other overloads
     * does not affect this path. Resolve this local shot to world space before
     * the old attachment can be scheduled. NPC/world bolt calls are untouched. */
    float point[3],basis[3][3];int kind=WeaponEffectKind(0,name);
    int result=effectPose?effectPose(kind,point,basis):-1;
    if(result<0){((NativeBoltedEffectFn)(clientBase+0x3ea40))(manager,name,attachment);return;}
    if(!result)return;
    static unsigned logged;
    if(diagnostics || logged<6){++logged;LOG_INFO("VRFX emit local-bolt kind=%d name=%s world=%.3f,%.3f,%.3f",kind,name?name:"",point[0],point[1],point[2]);}
    ((NativeEffectFn)(clientBase+0x3e8d0))(manager,name,point,basis,-1,-1,-1);
}
static void InstallClientEffects(void){
    if(!clientBase)return;
    /* Verified local inview fire/flash, smoke and extra effect calls, plus
     * delayed muzzle smoke and both native casing paths. */
    const unsigned sites[]={0x8c508,0x8c520,0x8c547,0x8c58c,0x8d0af,0x8d3bd,0x8d5d3};
    const unsigned count=sizeof(sites)/sizeof(sites[0]);
    for(unsigned i=0;i<count;++i){BYTE *site=clientBase+sites[i];
        if(site[0]!=0xe8)return;
        BYTE *target=site+5+*(int32_t *)(site+1);
        BYTE *original=clientBase+(i?0x3e8d0:0x3ea40);
        BYTE *hook=i?(BYTE *)WeaponEffectHook:(BYTE *)WeaponBoltedEffectHook;
        if(target!=original && target!=hook)return;
    }
    for(unsigned i=0;i<count;++i){BYTE *site=clientBase+sites[i];
        BYTE *hook=i?(BYTE *)WeaponEffectHook:(BYTE *)WeaponBoltedEffectHook;
        if(site+5+*(int32_t *)(site+1)==hook)continue;
        DWORD protect;if(!VirtualProtect(site,5,PAGE_EXECUTE_READWRITE,&protect))return;
        *(int32_t *)(site+1)=(int32_t)(hook-(site+5));
        VirtualProtect(site,5,protect,&protect);FlushInstructionCache(GetCurrentProcess(),site,5);
        LOG_INFO("VRFX local effect call %lx rebound to world weapon locator",(unsigned long)sites[i]);
    }
}
void GameVR_Install(VRShotPose provider,VRFXPose effects,int physicalKnife) {
    effectPose=effects;
    trackKnife=physicalKnife;
    InstallClientWeaponHook();
    InstallClientEffects();
    BYTE *base=(BYTE *)GetModuleHandleA("gamex86.dll");
    if(!base){serverBase=NULL;return;}
    if(serverBase==base && base[0xfbe70]==0xe9 &&
        (BYTE *)ShotHook==base+0xfbe75+*(int32_t *)(base+0xfbe71))return;
    serverBase=NULL;memset(impactInterrupts,0,sizeof(impactInterrupts));
    PIMAGE_DOS_HEADER dos=(PIMAGE_DOS_HEADER)base;
    PIMAGE_NT_HEADERS nt=(PIMAGE_NT_HEADERS)(base+dos->e_lfanew);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE || nt->Signature!=IMAGE_NT_SIGNATURE ||
       (uintptr_t)base!=0x20000000 || nt->FileHeader.TimeDateStamp!=0x3db45786 || nt->OptionalHeader.SizeOfImage!=0x60c000)return;
    IMAGE_SECTION_HEADER *s=IMAGE_FIRST_SECTION(nt);int matched=0;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)if(!memcmp(s[i].Name,".text",5)){
        if(s[i].VirtualAddress!=0x1000 || s[i].SizeOfRawData!=1875968)return;
        uint32_t hash=2166136261u;
        for(unsigned j=0;j<s[i].SizeOfRawData;++j)hash=(hash^base[s[i].VirtualAddress+j])*16777619u;
        if(hash!=0x7b3b8884u)return;matched=1;
    }
    const BYTE prologue[]={0x81,0xec,0x84,0x04,0,0};
    if(!matched || memcmp(base+0xfbe70,prologue,6))return;
    BYTE *trampoline=VirtualAlloc(NULL,16,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!trampoline)return;
    memcpy(trampoline,prologue,6);trampoline[6]=0xe9;
    *(int32_t *)(trampoline+7)=(int32_t)((base+0xfbe76)-(trampoline+11));
    DWORD old;
    if(!VirtualProtect(trampoline,16,PAGE_EXECUTE_READ,&old)){VirtualFree(trampoline,0,MEM_RELEASE);return;}
    if(!VirtualProtect(base+0xfbe70,6,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(trampoline,0,MEM_RELEASE);return;}
    shotPose=provider;shotTrampoline=trampoline;
    base[0xfbe70]=0xe9;*(int32_t *)(base+0xfbe71)=(int32_t)((BYTE *)ShotHook-(base+0xfbe75));base[0xfbe75]=0x90;
    VirtualProtect(base+0xfbe70,6,old,&old);FlushInstructionCache(GetCurrentProcess(),base+0xfbe70,6);
    const BYTE projectilePrologue[]={0x81,0xec,0x5c,0x04,0,0};
    if(!memcmp(base+0xfcd40,projectilePrologue,6)){
        BYTE *t=VirtualAlloc(NULL,16,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(t){memcpy(t,projectilePrologue,6);t[6]=0xe9;*(int32_t *)(t+7)=(int32_t)((base+0xfcd46)-(t+11));
            DWORD protect;
            if(VirtualProtect(t,16,PAGE_EXECUTE_READ,&protect) && VirtualProtect(base+0xfcd40,6,PAGE_EXECUTE_READWRITE,&protect)){
                projectileTrampoline=t;base[0xfcd40]=0xe9;*(int32_t *)(base+0xfcd41)=(int32_t)((BYTE *)ProjectileHook-(base+0xfcd45));base[0xfcd45]=0x90;
                VirtualProtect(base+0xfcd40,6,protect,&protect);FlushInstructionCache(GetCurrentProcess(),base+0xfcd40,6);
            }else VirtualFree(t,0,MEM_RELEASE);
        }
    }
    const unsigned extraRva[2]={0xfc580,0xfc390};const BYTE extraSignatures[2][6]={{0x81,0xec,0xa8,0x04,0,0},{0x81,0xec,0x64,0x04,0,0}};
    void *extraHooks[2]={(void *)ScatterHook,(void *)MeleeHook};void **extraTrampolines[2]={&scatterTrampoline,&meleeTrampoline};
    for(int i=0;i<2;++i){BYTE *site=base+extraRva[i];if(memcmp(site,extraSignatures[i],6))continue;
        BYTE *t=VirtualAlloc(NULL,16,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!t)continue;
        memcpy(t,site,6);t[6]=0xe9;*(int32_t *)(t+7)=(int32_t)((site+6)-(t+11));DWORD protection;
        if(VirtualProtect(t,16,PAGE_EXECUTE_READ,&protection) && VirtualProtect(site,6,PAGE_EXECUTE_READWRITE,&protection)){
            *extraTrampolines[i]=t;site[0]=0xe9;*(int32_t *)(site+1)=(int32_t)((BYTE *)extraHooks[i]-(site+5));site[5]=0x90;
            VirtualProtect(site,6,protection,&protection);FlushInstructionCache(GetCurrentProcess(),site,6);
        }else VirtualFree(t,0,MEM_RELEASE);
    }
    char traceSetting[8];diagnostics=GetEnvironmentVariableA("SOF2_VR_TRACE",traceSetting,sizeof(traceSetting))>0;
    BYTE *damageSite=base+0x52940;
    const BYTE damagePrologue[]={0x83,0xec,0x18,0x53,0x55,0x56};
    damageResume=NULL;
    if(!memcmp(damageSite,damagePrologue,sizeof(damagePrologue)) &&
       VirtualProtect(damageSite,6,PAGE_EXECUTE_READWRITE,&old)){
        damageResume=damageSite+6;
        damageSite[0]=0xe9;*(int32_t *)(damageSite+1)=(int32_t)((BYTE *)EnemyDamageHook-(damageSite+5));damageSite[5]=0x90;
        VirtualProtect(damageSite,6,old,&old);FlushInstructionCache(GetCurrentProcess(),damageSite,6);
        if(damageResume)LOG_INFO("Verified SP enemy damage hook installed scale=%d%%",enemyDamagePercent);
    }
    TraceFn *traceSlot=(TraceFn *)(base+0x3102fc);
    if((uintptr_t)*traceSlot==0x1001b260){realTrace=*traceSlot;*traceSlot=TraceHook;}
    LOG_INFO("SERVER collision trace verified=%d",realTrace!=NULL);
    serverBase=base;LOG_INFO("Verified SP hitscan hook installed: controller barrel-centre rays; native shotgun pellet pattern retained; no added visual recoil");
}
