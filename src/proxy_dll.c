#include "proxy_dll.h"
#include "openxr_core.h"
#include "openxr_input.h"
#include "stereo_render.h"
#include "opengl_forward.h"
#include "cvars.h"
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

// Original OpenGL function pointers
typedef BOOL (WINAPI *PFN_WGL_SWAP_BUFFERS)(HDC);

static PFN_WGL_SWAP_BUFFERS g_pfnSwapBuffers = NULL;
static PFN_WGL_SWAP_BUFFERS g_pfnRealWglSwapBuffers = NULL;

// Global state
static bool g_vrInitialized = false;
static bool g_vrActive = false;
static bool g_vrInitAttempted = false;
static bool g_cvarsInitialized = false;
static bool g_noRendererHookWarningLogged = false;
static bool g_rendererHookWarningLogged = false;
static HWND g_gameWindow = NULL;
static HDC g_lastGameHDC = NULL;
static HGLRC g_lastGameHGLRC = NULL;
static DWORD g_lastSwapThreadId = 0;
static int g_windowWidth = 0;
static int g_windowHeight = 0;

// VR frame callback - called to render the game scene for each eye

/* SoF2's refdef_t prefix matches the Q3-family layout in the workspace.
 * The opaque tail carries areamask, text strings, and renderer fog data. */
typedef struct {
    int x, y, width, height;
    float fov_x, fov_y;
    float vieworg[3];
    float viewaxis[3][3];
    int time;
    int rdflags;
    unsigned char rendererTail[0x160];
} SoF2Refdef;

typedef void (__cdecl *PFN_RENDERSCENE)(const SoF2Refdef*);
static PFN_RENDERSCENE g_originalRenderScene = NULL;
static SoF2Refdef g_eyeRefdef;
static bool g_renderHookEntryLogged = false;
/* Avoid nested scene callbacks if the renderer re-enters while drawing an
 * eye. The latest startup dump instead traced the fatal loop to swap dispatch. */
static volatile LONG g_renderSceneHookActive = 0;

static BOOL APIENTRY Proxy_wglSwapBuffers(HDC hdc);
static BOOL WINAPI HookedSwapBuffers(HDC hdc);
static void __cdecl HookedRenderScene(const SoF2Refdef* refdef);
static void __cdecl RenderSceneForEye(int eye, const float* proj, const float* view);
static void InitializeVR(HDC hdc, HGLRC hglrc);

/* The original SoF2 executable has a fixed image base and its static renderer
 * API table places RenderScene at RVA 0x799710. Verify the exact target before
 * patching; unknown executables keep the normal desktop path. */
static BOOL InstallRenderSceneHook(void) {
    const DWORD tableRva = 0x799710;
    const DWORD renderSceneRva = 0x99e30;
    HMODULE module = GetModuleHandleA(NULL);
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)module;
    PIMAGE_NT_HEADERS nt;
    volatile DWORD* slot;
    DWORD oldProtect;
    DWORD expected;

    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    nt = (PIMAGE_NT_HEADERS)((BYTE*)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.ImageBase != 0x10000000 ||
        nt->OptionalHeader.SizeOfImage <= tableRva + sizeof(DWORD)) {
        return FALSE;
    }

    slot = (volatile DWORD*)((BYTE*)module + tableRva);
    expected = (DWORD)(ULONG_PTR)((BYTE*)module + renderSceneRva);
    if (*slot != expected) return FALSE;
    if (!VirtualProtect((LPVOID)slot, sizeof(*slot), PAGE_READWRITE, &oldProtect)) return FALSE;
    g_originalRenderScene = (PFN_RENDERSCENE)(ULONG_PTR)*slot;
    *slot = (DWORD)(ULONG_PTR)HookedRenderScene;
    VirtualProtect((LPVOID)slot, sizeof(*slot), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)slot, sizeof(*slot));
    LOG_INFO("Hooked SoF2 renderer RenderScene API at %p", (void*)expected);
    return TRUE;
}

static void __cdecl RenderSceneForEye(int eye, const float* proj, const float* view) {
    const XrView* xrView = OpenXR_GetView(eye);
    const XrView* left = OpenXR_GetView(0);
    const XrView* right = OpenXR_GetView(1);
    float worldScale = Cvar_VariableValue("vr_worldscale");
    float midX;
    float eyeOffset;
    (void)proj;
    (void)view;
    if (!g_originalRenderScene || !xrView || !left || !right) return;

    g_eyeRefdef.x = 0;
    g_eyeRefdef.y = 0;
    g_eyeRefdef.width = OpenXR_GetEyeWidth(eye);
    g_eyeRefdef.height = OpenXR_GetEyeHeight(eye);
    g_eyeRefdef.fov_x = (xrView->fov.angleRight - xrView->fov.angleLeft) * (180.0f / 3.14159265358979323846f);
    g_eyeRefdef.fov_y = (xrView->fov.angleUp - xrView->fov.angleDown) * (180.0f / 3.14159265358979323846f);

    /* Apply the runtime's per-eye horizontal offset in the game's right axis.
     * Head rotation and translation are added in a later tracking pass. */
    midX = (left->pose.position.x + right->pose.position.x) * 0.5f;
    eyeOffset = (xrView->pose.position.x - midX) * worldScale;
    for (int axis = 0; axis < 3; ++axis) {
        g_eyeRefdef.vieworg[axis] += g_eyeRefdef.viewaxis[1][axis] * eyeOffset;
    }
    g_originalRenderScene(&g_eyeRefdef);
}

static void __cdecl HookedRenderScene(const SoF2Refdef* refdef) {
    if (InterlockedCompareExchange(&g_renderSceneHookActive, 1, 0) != 0) {
        return;
    }
    if (!g_renderHookEntryLogged) {
        LOG_INFO("SoF2 RenderScene hook entered (refdef=%p)", (const void*)refdef);
        g_renderHookEntryLogged = true;
    }
    if (!refdef || !g_originalRenderScene) goto done;

    /* A size/layout mismatch is detected through API target verification; the
     * known SoF2 refdef size is copied only after the renderer hook is verified. */
    if (!g_vrActive || !OpenXR_IsInitialized() || !OpenXR_FrameBegin()) {
        g_originalRenderScene(refdef);
        goto done;
    }
    if (!OpenXR_ShouldRender() || !OpenXR_LocateViews()) {
        OpenXR_FrameEnd();
        g_originalRenderScene(refdef);
        goto done;
    }

    StereoRender_BeginFrame();
    OpenXR_Input_Update();
    OpenXR_Input_ApplyToGame();
    for (int eye = 0; eye < 2; ++eye) {
        memcpy(&g_eyeRefdef, refdef, sizeof(g_eyeRefdef));
        StereoRender_RenderEye(eye, RenderSceneForEye);
    }
    StereoRender_EndFrame();
done:
    InterlockedExchange(&g_renderSceneHookActive, 0);
}

static BOOL InstallSwapBuffersHook(HMODULE module) {
    const WORD swapBuffersOrdinal = 516;
    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)module;
    PIMAGE_NT_HEADERS ntHeaders;
    IMAGE_DATA_DIRECTORY importDirectory;
    PIMAGE_IMPORT_DESCRIPTOR descriptor;

    if (!module || dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    ntHeaders = (PIMAGE_NT_HEADERS)((BYTE*)module + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return FALSE;
    importDirectory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDirectory.VirtualAddress) return FALSE;

    descriptor = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)module + importDirectory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* dllName = (const char*)((BYTE*)module + descriptor->Name);
        PIMAGE_THUNK_DATA names;
        PIMAGE_THUNK_DATA functions;
        if (_stricmp(dllName, "gdi32.dll") != 0 || !descriptor->FirstThunk) continue;

        names = descriptor->OriginalFirstThunk
            ? (PIMAGE_THUNK_DATA)((BYTE*)module + descriptor->OriginalFirstThunk)
            : (PIMAGE_THUNK_DATA)((BYTE*)module + descriptor->FirstThunk);
        functions = (PIMAGE_THUNK_DATA)((BYTE*)module + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++functions) {
            BOOL isSwapBuffers = FALSE;
            DWORD oldProtect;
            PROC original;
            if (descriptor->OriginalFirstThunk && IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                isSwapBuffers = IMAGE_ORDINAL(names->u1.Ordinal) == swapBuffersOrdinal;
            } else if (descriptor->OriginalFirstThunk) {
                PIMAGE_IMPORT_BY_NAME importName =
                    (PIMAGE_IMPORT_BY_NAME)((BYTE*)module + names->u1.AddressOfData);
                isSwapBuffers = strcmp((const char*)importName->Name, "SwapBuffers") == 0;
            } else {
                HMODULE gdi = GetModuleHandleA("gdi32.dll");
                PROC swapBuffers = gdi
                    ? GetProcAddress(gdi, MAKEINTRESOURCEA(swapBuffersOrdinal))
                    : NULL;
                isSwapBuffers = swapBuffers &&
                    (PROC)(ULONG_PTR)functions->u1.Function == swapBuffers;
            }
            if (!isSwapBuffers) continue;

            original = (PROC)(ULONG_PTR)functions->u1.Function;
            if (!VirtualProtect(&functions->u1.Function, sizeof(functions->u1.Function),
                                PAGE_READWRITE, &oldProtect)) return FALSE;
            functions->u1.Function = (ULONG_PTR)HookedSwapBuffers;
            VirtualProtect(&functions->u1.Function, sizeof(functions->u1.Function),
                           oldProtect, &oldProtect);
            g_pfnSwapBuffers = (PFN_WGL_SWAP_BUFFERS)original;
            return TRUE;
        }
    }
    return FALSE;
}

static void InitializeVR(HDC hdc, HGLRC hglrc) {
    if (g_vrInitialized || g_vrInitAttempted) return;
    g_vrInitAttempted = true;
    LOG_INFO("VR initialization handles: HDC=%p HGLRC=%p swapThread=%lu initThread=%lu currentDC=%p currentRC=%p",
             (void*)hdc, (void*)hglrc, (unsigned long)g_lastSwapThreadId,
             (unsigned long)GetCurrentThreadId(), (void*)wglGetCurrentDC(),
             (void*)wglGetCurrentContext());
    if (!hdc || !hglrc || hdc != wglGetCurrentDC() || hglrc != wglGetCurrentContext()) {
        LOG_ERROR("Skipping OpenXR: saved graphics handles are not current on the render thread");
        return;
    }
    {
        const GLubyte* vendor = glGetString(GL_VENDOR);
        const GLubyte* renderer = glGetString(GL_RENDERER);
        const GLubyte* version = glGetString(GL_VERSION);
        LOG_INFO("OpenGL context: pixelFormat=%d vendor=%s renderer=%s version=%s",
                 GetPixelFormat(hdc), vendor ? (const char*)vendor : "(null)",
                 renderer ? (const char*)renderer : "(null)",
                 version ? (const char*)version : "(null)");
    }
    if (!g_cvarsInitialized) {
        Cvar_Init();
        g_cvarsInitialized = true;
    }
    
    // Get the game window from the device context
    g_gameWindow = WindowFromDC(hdc);
    if (!g_gameWindow) {
        g_gameWindow = GetActiveWindow();
    }
    
    RECT rect;
    if (g_gameWindow) {
        GetClientRect(g_gameWindow, &rect);
        g_windowWidth = rect.right - rect.left;
        g_windowHeight = rect.bottom - rect.top;
    }
    
    // Initialize OpenXR
    if (OpenXR_Init(g_gameWindow, hdc, hglrc)) {
        // Initialize input system
        OpenXR_Input_Init();
        
        // Initialize stereo rendering
        StereoRender_Init(g_windowWidth, g_windowHeight);
        
        g_vrInitialized = true;
        g_vrActive = true;
        LOG_INFO("SoF2 VR Proxy initialized successfully");
    } else {
        LOG_ERROR("Failed to initialize OpenXR");
        OpenXR_Shutdown();
        g_vrActive = false;
    }
}

static void ShutdownVR(void) {
    if (!g_vrInitialized) return;
    
    OpenXR_Input_Shutdown();
    StereoRender_Shutdown();
    OpenXR_Shutdown();
    
    g_vrInitialized = false;
    g_vrActive = false;
    LOG_INFO("SoF2 VR Proxy shutdown");
}

// Our hooked wglSwapBuffers
BOOL APIENTRY Proxy_wglSwapBuffers(HDC hdc) {
    BOOL result;
    if (!g_originalRenderScene && !InstallRenderSceneHook() && !g_rendererHookWarningLogged) {
        LOG_WARN("SoF2 renderer API hook not found; using desktop rendering");
        g_rendererHookWarningLogged = true;
    }
    // If the renderer API hook was not verified, leave the desktop path untouched.
    if (!g_originalRenderScene) {
        if (!g_noRendererHookWarningLogged) {
            LOG_WARN("No compatible SoF2 renderer hook is active; continuing with desktop rendering");
            g_noRendererHookWarningLogged = true;
        }
        return g_pfnSwapBuffers ? g_pfnSwapBuffers(hdc) : FALSE;
    }

    result = g_pfnSwapBuffers ? g_pfnSwapBuffers(hdc) : FALSE;
    /* Bring up OpenXR after the game's first complete desktop frame. This
     * avoids creating the runtime session from inside the renderer callback. */
    if (!g_vrInitAttempted && g_lastGameHDC && g_lastGameHGLRC) {
        InitializeVR(g_lastGameHDC, g_lastGameHGLRC);
    }
    return result;
}

// Load original OpenGL functions
static void LoadOriginalGLFunctions(void) {
    char systemDirectory[MAX_PATH];
    UINT directoryLength = GetSystemDirectoryA(systemDirectory, MAX_PATH);
    HMODULE hOpenGL = NULL;
    if (directoryLength && directoryLength < MAX_PATH - sizeof("\\opengl32.dll")) {
        strcat(systemDirectory, "\\opengl32.dll");
        hOpenGL = LoadLibraryA(systemDirectory);
    }
    
    if (hOpenGL) {
        OpenGL_ForwardInit(hOpenGL);
        /* GDI's SwapBuffers implementation dispatches through the OpenGL
         * DLL's wglSwapBuffers export. That export must reach the system
         * OpenGL implementation directly; routing it back through our GDI
         * hook creates an infinite SwapBuffers -> wglSwapBuffers loop. */
        g_pfnRealWglSwapBuffers =
            (PFN_WGL_SWAP_BUFFERS)GetProcAddress(hOpenGL, "wglSwapBuffers");
        LOG_INFO("Original OpenGL functions loaded");
    }
    if (!g_pfnSwapBuffers) {
        HMODULE hGdi = GetModuleHandleA("gdi32.dll");
        if (hGdi) g_pfnSwapBuffers = (PFN_WGL_SWAP_BUFFERS)GetProcAddress(hGdi, "SwapBuffers");
    }
}

static BOOL WINAPI HookedSwapBuffers(HDC hdc) {
    g_lastGameHDC = hdc;
    g_lastGameHGLRC = wglGetCurrentContext();
    g_lastSwapThreadId = GetCurrentThreadId();
    return Proxy_wglSwapBuffers(hdc);
}

// DllMain
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            LoadOriginalGLFunctions();
            InstallSwapBuffersHook(GetModuleHandleA(NULL));
            LOG_INFO("SoF2 VR Proxy DLL loaded");
            break;
            
        case DLL_PROCESS_DETACH:
            ShutdownVR();
            if (g_cvarsInitialized) Cvar_Shutdown();
            LOG_INFO("SoF2 VR Proxy DLL unloaded");
            break;
    }
    return TRUE;
}

// GDI dispatches OpenGL swaps through this export. Forward to the real
// system OpenGL implementation; the IAT hook above observes the app's GDI call.
__declspec(dllexport) BOOL APIENTRY wglSwapBuffers(HDC hdc) {
    return g_pfnRealWglSwapBuffers ? g_pfnRealWglSwapBuffers(hdc) : FALSE;
}
