/* SPDX-License-Identifier: GPL-3.0-or-later
 * Adapted from Brutally RealRTCW VR; see docs/WEAPON_WHEEL.md. */
#ifndef SOF2_WEAPON_WHEEL_H
#define SOF2_WEAPON_WHEEL_H
#define WHEEL_PAGE_SIZE 6
#define WHEEL_MAX_ITEMS 64
#define WHEEL_SAVE 1001
#define WHEEL_LOAD 1002
typedef struct {int id,clip,reserve,available;char name[32];} WheelItem;
typedef struct {
    unsigned now;int valid,grip,trigger;unsigned buttons[2];
    float stick,stickY,hand[2];int stickSelect,holdConfirm;
} WheelInput;
typedef struct {
    int open,quiet,paged,gripArmed,triggerArmed,used,page,pages,hover,count,loadAvailable;
    unsigned gripStart,buttons[2];float start[2],point[2];
    WheelItem items[WHEEL_MAX_ITEMS];int inventoryCount,weaponsOnly;
} WeaponWheel;
typedef struct {int action,changed,opened,closed,pageChanged,tapReload,consume;} WheelEvent;
void Wheel_SetInventory(WeaponWheel *,const WheelItem *,int);
void Wheel_Cancel(WeaponWheel *);
WheelEvent Wheel_Update(WeaponWheel *,const WheelInput *);
int Wheel_PageItems(const WeaponWheel *,WheelItem out[WHEEL_PAGE_SIZE]);
void Wheel_Slot(int index,int count,float *x,float *y); /* screen y down */
#endif
