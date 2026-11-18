/*
===========================================================================
SoF2 VR — Stereo Rendering Subsystem
File: stereo_render.h
Description: Double-pass stereo rendering with OpenXR projection/view matrices.
===========================================================================
*/

#ifndef __STEREO_RENDER_H__
#define __STEREO_RENDER_H__

#include <openxr/openxr.h>
#include <stdbool.h>

typedef struct {
    float pos[3];
    float rot[4];  // quaternion
    XrFovf fov;
} VRViewParams;

/* ---- Subsystem API ---- */
void StereoRender_Init(int width, int height);
void StereoRender_Shutdown(void);
void StereoRender_BeginFrame(void);
void StereoRender_RenderEye(int eye, void (*renderCallback)(int eye, const float* proj, const float* view));
void StereoRender_EndFrame(void);

const float* StereoRender_GetProjectionMatrix(int eye);
const float* StereoRender_GetViewMatrix(int eye);
const VRViewParams* StereoRender_GetViewParams(int eye);

#endif /* __STEREO_RENDER_H__ */
