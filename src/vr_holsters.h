#ifndef SOF2_VR_HOLSTERS_H
#define SOF2_VR_HOLSTERS_H
enum {VR_HOLSTER_NONE,VR_HOLSTER_PISTOL,VR_HOLSTER_PRIMARY,VR_HOLSTER_GRENADE,VR_HOLSTER_HEAVY,VR_HOLSTER_KNIFE,VR_HOLSTER_AMMO,VR_HOLSTER_SMG,VR_HOLSTER_COUNT};
typedef struct {int armed,held,slot,wheel;unsigned started;int selected[VR_HOLSTER_COUNT];unsigned holdMs;} VRHolsters;
typedef struct {int consume,drawSlot,wheelSlot,opened;} VRHolsterEvent;
/* Body frame: x right, y up relative to eye height, z behind. Metres. */
void VR_HolsterPosition(int slot,float out[3]);
int VR_HolsterAt(const float point[3]);
void VR_HolsterPositionEx(int slot,int leftHanded,float reach,float height,float out[3]);
int VR_HolsterAtEx(const float point[3],int leftHanded,float reach,float height);
void VR_HolsterPositionAccessible(int slot,int leftHanded,float reach,float height,int seated,float out[3]);
int VR_HolsterAtAccessible(const float point[3],int leftHanded,float reach,float height,int seated);
int VR_HolsterAtOccupied(const float point[3],int leftHanded,float reach,float height,int seated,unsigned slots);
int VR_HolsterCategory(int category);
VRHolsterEvent VR_HolsterUpdate(VRHolsters *,int valid,int grip,int nearSlot,unsigned now);
#endif
