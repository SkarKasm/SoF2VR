#ifndef SOF2_VR_MECHANICS_H
#define SOF2_VR_MECHANICS_H
/* 1 reachable, 2 bounded maximum-range fallback, 0 invalid. Unit direction. */
int VR_LobDirection(const float start[3],const float target[3],float speed,float gravity,float direction[3]);
typedef struct {int needed,held,back,armed;float start,pull;} VRPump;
/* Validity loss cancels a stroke; it cannot chamber a round. */
int VR_PumpUpdate(VRPump *pump,int valid,int grip,int trigger,int inReach,float along,float travel);
void VR_PumpShot(VRPump *pump);
int VR_ArmSolve(const float shoulder[3],const float wrist[3],const float bend[3],float upper,float lower,float elbow[3]);

typedef struct {float heel[3],tip[3];} VRBladePose;
typedef struct {VRBladePose previous;unsigned time;int sampled;} VRBladeMotion;
/* Returns the number of overlapping blade samples needed for this motion.
 * Coordinates and radius are in game units. Tracking jumps seed a new pose. */
int VR_BladeMotion(VRBladeMotion *motion,unsigned time,const VRBladePose *blade,float radius,VRBladePose *previous);
enum {VR_MAG_SEATED,VR_MAG_EXTRACT,VR_MAG_OLD,VR_MAG_EMPTY,VR_MAG_FRESH,VR_MAG_BOLT,VR_MAG_PENDING};
enum {VR_MAG_NONE,VR_MAG_GRAB,VR_MAG_OUT,VR_MAG_RETAIN,VR_MAG_SPARE,VR_MAG_INSERT,VR_MAG_CHARGE,VR_MAG_RETURN,VR_MAG_DROP,VR_MAG_RACK};
typedef struct {int stage,gripBefore,armed,away,boltHeld,boltBack,actionBefore,actionArmed;float boltStart,boltPull;} VRMagazine;
typedef struct {int valid,grip,nearSocket,nearPouch,nearBolt,aligned,spare,empty;float extraction,along;int action;float actionTravel;} VRMagazineInput;
/* Emits one insertion/charging edge. Caller owns native transaction and ammo. */
int VR_MagazineUpdate(VRMagazine *mag,const VRMagazineInput *input);
/* A-button edge from the caller; gripping a seated magazine never ejects it. */
int VR_MagazineEject(VRMagazine *mag,int valid);
#endif
