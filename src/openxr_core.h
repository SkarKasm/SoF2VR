/*
===========================================================================
SoF2 VR — OpenXR Core Subsystem
File: openxr_core.h
Description: OpenXR instance, session, swapchain, and frame loop management.
             32-bit compatible for SoF2.exe injection.
===========================================================================
*/

#ifndef __OPENXR_CORE_H__
#define __OPENXR_CORE_H__

#if defined(_WIN32)
#define XR_USE_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>
#endif

#define XR_USE_GRAPHICS_API_OPENGL

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <GL/gl.h>

#include <stdint.h>
#include <stdbool.h>

/* Boolean for compatibility */
typedef int qboolean;
#define qtrue  1
#define qfalse 0

/* Eye indices */
typedef enum {
    XR_EYE_LEFT  = 0,
    XR_EYE_RIGHT = 1,
    XR_EYE_COUNT = 2
} xr_eye_t;

/* OpenXR context — all VR state lives here */
typedef struct {
    XrInstance          instance;
    XrSystemId          systemId;
    XrSession           session;
    XrSpace             stageSpace;
    XrSpace             viewSpace;
    XrViewConfigurationView* viewConfigs;
    uint32_t            viewCount;
    XrView              views[2];
    XrSwapchain         swapchains[2];
    XrSwapchainImageOpenGLKHR* swapchainImages[2];
    uint32_t            swapchainImageCount[2];
    GLuint*             eyeFBOs[2];
    GLuint*             eyeDepthBuffers[2];
    XrTime              predictedDisplayTime;
    bool                shouldRender;
    bool                viewsLocated;
    bool                initialized;
    bool                sessionRunning;
} OpenXRContext;

/* PFN_xr* function pointer types come from the official OpenXR headers. */
extern PFN_xrStringToPath pfn_xrStringToPath;
extern PFN_xrCreateActionSet pfn_xrCreateActionSet;
extern PFN_xrDestroyActionSet pfn_xrDestroyActionSet;
extern PFN_xrCreateAction pfn_xrCreateAction;
extern PFN_xrSuggestInteractionProfileBindings pfn_xrSuggestInteractionProfileBindings;
extern PFN_xrAttachSessionActionSets pfn_xrAttachSessionActionSets;
extern PFN_xrGetActionStateBoolean pfn_xrGetActionStateBoolean;
extern PFN_xrGetActionStateVector2f pfn_xrGetActionStateVector2f;
extern PFN_xrGetActionStatePose pfn_xrGetActionStatePose;
extern PFN_xrSyncActions pfn_xrSyncActions;
extern PFN_xrCreateActionSpace pfn_xrCreateActionSpace;
extern PFN_xrLocateSpace pfn_xrLocateSpace;
extern PFN_xrDestroySpace pfn_xrDestroySpace;
extern PFN_xrAcquireSwapchainImage pfn_xrAcquireSwapchainImage;
extern PFN_xrWaitSwapchainImage pfn_xrWaitSwapchainImage;
extern PFN_xrReleaseSwapchainImage pfn_xrReleaseSwapchainImage;
void OpenXR_BindFramebuffer(GLuint framebuffer);

/* ---- Subsystem API ---- */
bool OpenXR_Init(HWND hwnd, HDC hdc, HGLRC hglrc);
void OpenXR_Shutdown(void);
bool OpenXR_FrameBegin(void);
bool OpenXR_LocateViews(void);
void OpenXR_FrameEnd(void);

/* Getters */
const XrView* OpenXR_GetView(int eye);
XrSwapchain OpenXR_GetSwapchain(int eye);
uint32_t OpenXR_GetSwapchainImageCount(int eye);
int32_t OpenXR_GetEyeWidth(int eye);
int32_t OpenXR_GetEyeHeight(int eye);
GLuint OpenXR_GetEyeFBO(int eye, uint32_t index);
XrSpace OpenXR_GetStageSpace(void);
XrSpace OpenXR_GetViewSpace(void);
XrSession OpenXR_GetSession(void);
XrInstance OpenXR_GetInstance(void);
XrTime OpenXR_GetPredictedDisplayTime(void);
bool OpenXR_ShouldRender(void);
bool OpenXR_IsInitialized(void);

#endif /* __OPENXR_CORE_H__ */
