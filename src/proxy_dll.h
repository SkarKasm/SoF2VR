/*
===========================================================================
SoF2 VR — OpenGL Proxy DLL
File: proxy_dll.h
Description: Shared declarations for the OpenGL proxy DLL that injects
             OpenXR VR into Soldier of Fortune II (2002) via wglSwapBuffers hook.
===========================================================================
*/

#ifndef __PROXY_DLL_H__
#define __PROXY_DLL_H__

#include <windows.h>
#include <GL/gl.h>
#include "vr_log.h"

/* ---- OpenXR integration ---- */
#include "openxr_core.h"
#include "openxr_input.h"
#include "stereo_render.h"
#include "vr_particles.h"

/* ---- Proxy DLL state ---- */
typedef struct {
    /* Original OpenGL DLL */
    HMODULE                   hOrigOpenGL;
    
    /* Original function pointers */
    BOOL (APIENTRY *pfnSwapBuffers)(HDC hdc);
    
    /* VR state */
    qboolean                  vrInitialized;
    qboolean                  vrActive;
    HDC                       currentHDC;
    HGLRC                     currentHGLRC;
    
    /* Frame timing */
    LARGE_INTEGER             frameStartTime;
    LARGE_INTEGER             freq;
    float                     frameTime;
    
    /* CVars */
    int                       vr_enable;
    float                     vr_worldScale;
    float                     vr_ipd;
    int                       vr_comfortVignette;
    int                       vr_snapTurn;
    int                       vr_weaponDecouple;
    
} proxy_state_t;

extern proxy_state_t g_proxy;

/* ---- Function declarations ---- */
/* DLL entry */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);

/* OpenGL hooks */
BOOL APIENTRY Hooked_wglSwapBuffers(HDC hdc);

/* Initialization */
void Proxy_Init(HDC hdc, HGLRC hglrc);
void Proxy_Shutdown(void);

/* Per-frame */
void Proxy_BeginFrame(void);
void Proxy_EndFrame(void);

/* Input */
void Proxy_UpdateInput(void);

/* CVar system */
void Proxy_RegisterCvars(void);
void Proxy_UpdateCvars(void);

/* Utility */
const char* Proxy_GetSoF2WindowTitle(void);
HWND Proxy_FindSoF2Window(void);

#endif /* __PROXY_DLL_H__ */
