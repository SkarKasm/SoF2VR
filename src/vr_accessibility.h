/* Shared, bounded VR-only settings. Native game configuration is untouched. */
#ifndef SOF2_VR_ACCESSIBILITY_H
#define SOF2_VR_ACCESSIBILITY_H
#include <windows.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    int leftHanded,switchSticks,snapAngle,turnSpeed,walkDirection,deadzone;
    int toggleGrip,gripThreshold,triggerThreshold,holsterReach,holsterHeight,holdMs;
    int seated,crouchToggle,physicalCrouch,manualReload,manualPump;
    int haptics,reticle,hands,vignette,roomScale,enemyDamage;
} VRAccessibility;
typedef struct {const char *key;size_t offset;int low,high;} VRAccessibilityOption;
#define VR_ACCESS_OPTION(key,field,low,high) {key,offsetof(VRAccessibility,field),low,high}
static const VRAccessibilityOption vrAccessibilityOptions[]={
    VR_ACCESS_OPTION("LeftHanded",leftHanded,0,1),
    VR_ACCESS_OPTION("ToggleGrip",toggleGrip,0,1),
    VR_ACCESS_OPTION("SnapAngle",snapAngle,0,90),
    VR_ACCESS_OPTION("TurnSpeed",turnSpeed,60,360),
    VR_ACCESS_OPTION("WalkDirection",walkDirection,0,1),
    VR_ACCESS_OPTION("SwitchSticks",switchSticks,0,1),
    VR_ACCESS_OPTION("DeadzonePercent",deadzone,10,70),
    VR_ACCESS_OPTION("GripThresholdPercent",gripThreshold,20,90),
    VR_ACCESS_OPTION("TriggerThresholdPercent",triggerThreshold,20,90),
    VR_ACCESS_OPTION("HolsterReachPercent",holsterReach,50,150),
    VR_ACCESS_OPTION("HolsterHeightCm",holsterHeight,-30,30),
    VR_ACCESS_OPTION("HolsterHoldMs",holdMs,0,1500),
    VR_ACCESS_OPTION("Seated",seated,0,1),
    VR_ACCESS_OPTION("CrouchToggle",crouchToggle,0,1),
    VR_ACCESS_OPTION("PhysicalCrouch",physicalCrouch,0,1),
    VR_ACCESS_OPTION("ManualReload",manualReload,0,1),
    VR_ACCESS_OPTION("ManualPump",manualPump,0,1),
    VR_ACCESS_OPTION("HapticsPercent",haptics,0,100),
    VR_ACCESS_OPTION("Reticle",reticle,0,1),
    VR_ACCESS_OPTION("ShowHands",hands,0,1),
    VR_ACCESS_OPTION("VignettePercent",vignette,0,100),
    VR_ACCESS_OPTION("EnemyDamagePercent",enemyDamage,25,100),
    VR_ACCESS_OPTION("RoomScale",roomScale,0,1)
};
#undef VR_ACCESS_OPTION
/* Stage beside the existing INI, preserving Graphics/HUD/outfit settings.
 * Only publish the complete settings file after every key has been written. */
static int VR_WriteAccessibility(const char *path,const VRAccessibility *a){
    char pending[MAX_PATH];int n=snprintf(pending,sizeof(pending),"%s.%lu.options-tmp",path,GetCurrentProcessId());
    if(n<0 || n>=(int)sizeof(pending)){SetLastError(ERROR_BUFFER_OVERFLOW);return 0;}
    if(!CopyFileA(path,pending,FALSE)){
        if(GetLastError()!=ERROR_FILE_NOT_FOUND)return 0;
        HANDLE f=CreateFileA(pending,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if(f==INVALID_HANDLE_VALUE)return 0;CloseHandle(f);
    }
    for(unsigned i=0;i<sizeof(vrAccessibilityOptions)/sizeof(vrAccessibilityOptions[0]);++i){
        const VRAccessibilityOption *option=&vrAccessibilityOptions[i];char value[24];int v=*(const int *)((const char *)a+option->offset);
        if(v<option->low || v>option->high || (!strcmp(option->key,"SnapAngle") && v>0 && v<15)){DeleteFileA(pending);SetLastError(ERROR_INVALID_DATA);return 0;}
        snprintf(value,sizeof(value),"%d",v);
        if(!WritePrivateProfileStringA("Accessibility",option->key,value,pending)){DWORD error=GetLastError();DeleteFileA(pending);SetLastError(error);return 0;}
    }
    WritePrivateProfileStringA(NULL,NULL,NULL,pending);
    if(!MoveFileExA(pending,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DWORD error=GetLastError();DeleteFileA(pending);SetLastError(error);return 0;}
    WritePrivateProfileStringA(NULL,NULL,NULL,path);return 1;
}
static int VR_Option(const char *path,const char *key,int fallback,int low,int high){
    char text[40],def[20],*end;wsprintfA(def,"%d",fallback);
    GetPrivateProfileStringA("Accessibility",key,def,text,sizeof(text),path);
    long value=strtol(text,&end,10);
    return end==text || *end || value<low || value>high?fallback:(int)value;
}
static VRAccessibility VR_ReadAccessibility(const char *path){
    VRAccessibility a;
    a.leftHanded=VR_Option(path,"LeftHanded",GetPrivateProfileIntA("Controls","LeftHanded",0,path)!=0,0,1);
    a.switchSticks=VR_Option(path,"SwitchSticks",0,0,1);
    a.snapAngle=VR_Option(path,"SnapAngle",GetPrivateProfileIntA("Controls","SnapAngle",45,path),0,90);
    if(a.snapAngle && a.snapAngle<15)a.snapAngle=45;
    a.turnSpeed=VR_Option(path,"TurnSpeed",180,60,360);
    a.walkDirection=VR_Option(path,"WalkDirection",0,0,1);
    a.deadzone=VR_Option(path,"DeadzonePercent",35,10,70);
    a.toggleGrip=VR_Option(path,"ToggleGrip",0,0,1);
    a.gripThreshold=VR_Option(path,"GripThresholdPercent",65,20,90);
    a.triggerThreshold=VR_Option(path,"TriggerThresholdPercent",65,20,90);
    a.holsterReach=VR_Option(path,"HolsterReachPercent",100,50,150);
    a.holsterHeight=VR_Option(path,"HolsterHeightCm",0,-30,30);
    a.holdMs=VR_Option(path,"HolsterHoldMs",250,0,1500);
    a.seated=VR_Option(path,"Seated",0,0,1);
    a.crouchToggle=VR_Option(path,"CrouchToggle",0,0,1);
    a.physicalCrouch=VR_Option(path,"PhysicalCrouch",0,0,1);
    a.manualReload=VR_Option(path,"ManualReload",GetPrivateProfileIntA("Controls","ManualReload",1,path)!=0,0,1);
    a.manualPump=VR_Option(path,"ManualPump",GetPrivateProfileIntA("Controls","ManualPump",1,path)!=0,0,1);
    a.haptics=VR_Option(path,"HapticsPercent",100,0,100);
    a.reticle=VR_Option(path,"Reticle",1,0,1);
    a.hands=VR_Option(path,"ShowHands",1,0,1);
    a.vignette=VR_Option(path,"VignettePercent",0,0,100);
    a.enemyDamage=VR_Option(path,"EnemyDamagePercent",50,25,100);
    a.roomScale=VR_Option(path,"RoomScale",1,0,1);
    return a;
}
#endif
