#ifndef SOF2_GAME_VR_H
#define SOF2_GAME_VR_H
#include "vr_mechanics.h"
typedef int (*VRShotPose)(float origin[3],float axis[3][3]);
typedef struct {
    int weapon,clip,reserve,capacity,alternate,state;
    unsigned sampledAt;
} VRWeaponState;
/* Read-only sample from the separately verified local player weapon manager. */
int GameVR_GetWeaponState(VRWeaponState *state);
int GameVR_GetVitals(int *health,int *armor);
int GameVR_GetSecondaryAmmo(const VRWeaponState *state,int *clip,int *reserve);
/* Guarded native reserve -> primary magazine transfer, processed by the
 * verified local weapon update. No animation or command-based reload. */
int GameVR_InsertRounds(int weapon,int clip,int reserve,int rounds);
int GameVR_ReloadResult(void); /* 0 pending/none, 1 committed, -1 rejected */
/* Eject exactly one loaded round through the selected native clip setter.
 * Reserve ammunition is never credited for a round discarded by cycling. */
int GameVR_RackRound(int weapon,int clip,int reserve);
int GameVR_RackPending(void);
int GameVR_TakeRackResult(void);
void GameVR_SetReloadControl(int weapon,int blocked,int fallback);
void GameVR_CancelRoundInsertion(void);
void GameVR_CancelRoundInsertionFor(int weapon);
typedef struct {int id,clip,reserve,available;char name[32];} VRInventoryItem;
int GameVR_Inventory(VRInventoryItem *items,int capacity);
int GameVR_RequestWeapon(int id);
int GameVR_AdvanceWeaponRequest(void); /* ID: issue native newwpn<ID>, 0: no step */
int GameVR_WeaponRequestPending(void);
void GameVR_CancelWeaponRequest(void);
int GameVR_NativeInventoryOpen(void);
void GameVR_CloseNativeInventory(void);
int GameVR_Ready(void);
void GameVR_BladeContact(const float head[3],const VRBladePose *from,const VRBladePose *to,int steps,float radius,int *lastContact);
void GameVR_SetEnemyDamagePercent(int percent);
int GameVR_TraceAim(const float origin[3],const float forward[3],float target[3]);
int GameVR_GrenadeAim(int weapon,int alternate,const float origin[3],const float aim[3],float direction[3],float target[3]);
void GameVR_ClampMuzzle(const float head[3],float muzzle[3]);
/* Cosmetic magazine AABB against world solids: 1 contact, 0 clear, -1 unavailable. */
int GameVR_TraceMagazine(const float start[3],const float half[3],const float end[3],float stop[3]);
int GameVR_TraceThrownWeapon(const float start[3],const float half[3],const float end[3],float stop[3],int interrupt);
float GameVR_RecoilPitch(void);
typedef int (*VRFXPose)(int ejection,float origin[3],float axis[3][3]);
void GameVR_Install(VRShotPose provider,VRFXPose effects,int physicalKnife);
#endif
