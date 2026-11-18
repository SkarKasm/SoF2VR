/*
===========================================================================
SoF2 VR — OpenXR Input Subsystem
File: openxr_input.h
Description: VR controller input handling, action mapping to game input.
===========================================================================
*/

#ifndef __OPENXR_INPUT_H__
#define __OPENXR_INPUT_H__

#include <openxr/openxr.h>
#include <stdbool.h>

typedef struct {
    bool fire;
    bool reload;
    float moveX;
    float moveY;
    bool snapTurn;
    bool menu;
    bool aimPoseValid;
    bool gripPoseValid;
    XrPosef aimPose;
    XrPosef gripPose;
} VRInputState;

/* ---- Subsystem API ---- */
bool OpenXR_Input_Init(void);
void OpenXR_Input_Update(void);
void OpenXR_Input_ApplyToGame(void);
bool OpenXR_Input_GetAimPose(float* outPos, float* outRot);
void OpenXR_Input_Shutdown(void);

#endif /* __OPENXR_INPUT_H__ */
