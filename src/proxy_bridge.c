/* x86-only game integration. OpenXR is intentionally owned by bridge64.c. */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <dsound.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "bridge_protocol.h"
#include "opengl_forward.h"
#include "vr_log.h"
#include "game_vr.h"
#include "weapon_profiles.h"
#include "vr_mechanics.h"
#include "reload_parts.h"
#include "weapon_sides.h"
#include "weapon_muzzles.h"
#include "knife_collision.h"
#include "weapon_effects.h"
#include "weapon_grips.h"
#include "magazine_parts.h"
#include "weapon_actions.h"
#include "magazine_bounds.h"
#include "magazine_interactions.h"
#include "loading_parts.h"
#include "replacement_models.h"
#include "vr_holsters.h"
#include "vr_body_ik.h"
#include "body_profiles.h"
#include "outfit_profiles.h"
#include "native_wrists.h"
#include "vr_accessibility.h"

typedef BOOL (WINAPI *SwapFn)(HDC);
typedef void (APIENTRY *MatrixFn)(const GLfloat *);
typedef void (APIENTRY *RectFn)(GLint, GLint, GLsizei, GLsizei);
typedef struct {
    int x, y, width, height;
    float fovX, fovY, origin[3], axis[3][3];
    int time, flags;
    unsigned char areaMask[32], text[256];
} GameRefdef;
typedef char RefdefSizeCheck[sizeof(GameRefdef) == 0x170 ? 1 : -1];
typedef void (__cdecl *SceneFn)(const GameRefdef *);
typedef void (__cdecl *FlushFn)(void);
typedef void (__cdecl *CommandFn)(const char *);
typedef float (__cdecl *CvarFloatFn)(const char *);
typedef void (__cdecl *NativeCvarSetFn)(const char *,const char *,int);

static HMODULE selfModule,systemGL;
static int flatMode; /* Set by the launcher in this process only. */
static INIT_ONCE forwardingOnce=INIT_ONCE_STATIC_INIT;
static SwapFn realSwap, realWglSwap;
static PROC (WINAPI *realWglGetProcAddress)(LPCSTR);
static MatrixFn realMatrix;
static RectFn realViewport, realScissor;
static HANDLE mapping, helperProcess;
static BridgeShared *shared;
static int bridgeAttempted, helperFailureLogged, eyeActive, nativeSinceSwap,nativeRendered;
static void (APIENTRY *realDrawElements)(GLenum,GLsizei,GLenum,const void *);
static unsigned hudDraws;
static void (APIENTRY *realDepthRange)(GLclampd,GLclampd);
static int skyProjectionActive;static GLfloat skyProjection[16];static GLboolean skyFogEnabled;
static DWORD imageSerial,testQuitDeadline;
static SceneFn originalScene;
static DWORD *sceneSlot;
static FlushFn flushCommands;
static CommandFn addCommand;
static CvarFloatFn cvarFloat;
static NativeCvarSetFn nativeCvarSet;
static DWORD lastNativeTime;
enum { CONTROL_SPEED=10, CONTROL_COUNT };
static int heldControls[CONTROL_COUNT],turnLatched,menuHeld,controlsEnabled=1,leftHanded;
static uint32_t inputSerial,hapticSerial;
static float snapAngle=45.0f;
static VRAccessibility accessibility;
static char qualityPath[MAX_PATH];
static int weaponHeld,gripPhysical,gripRearm=1,gripSampled,crouchLatched,crouchBefore;
static float standingEyeHeight=1.7f;static unsigned controlsTime;
static float GripOn(void){return accessibility.gripThreshold*.01f;}
static float GripOff(void){return fmaxf(.1f,GripOn()-.30f);}
static float TriggerOn(void){return accessibility.triggerThreshold*.01f;}
static float hudWidth=.84f,hudHeight=.75f,hudOffset=-.04f,hudOpacity=.65f;
static int manualPump=1,pumpFireBlocked;static VRPump pump;
static VRMagazine magazine;
static struct {unsigned *vector;unsigned saved[3],records[128][6];} reloadSurfaceBackup[32];static unsigned reloadSurfaceCount;
static int reloadModels[3],reloadWeapon,reloadClip,reloadReserve,reloadCapacity;
static int reloadAssetWeapon,reloadEjectRequest,reloadSlideRequest;
static int reloadHandModels[2];
static struct {int active;DWORD seatStarted,lastSample;float depth,radial,seatOrigin[3],seatAxis[3][3];} magazineGuide;
static int pistolSlideLocked;
static int openBoltReady;
static int reloadRenderReady;
static struct {float origin[3],axis[3][3],socket[3],bolt[3],pouch[3];} reloadRender;
#define MAX_DROPPED_MAGAZINES 8
static struct {int active,model,weapon,reflected,settled;unsigned born,last,epoch;float origin[3],axis[3][3],velocity[3],spinAxis[3],spin,half[3];} droppedMagazines[MAX_DROPPED_MAGAZINES];
static int loadingModels[3],loadingLive,loadingBlocked;
static struct {int stage,armed,gripBefore,weapon,clip,reserve;unsigned epoch;DWORD seatingSince;} shellLoad;
static int manualReloadEnabled=1,reloadLive,reloadHeldOld;static DWORD reloadLastValid;
static float reloadOffset[3],reloadBasis[3][3],reloadStartHand[3],reloadStartRelative[3];
static float reloadSocket[3],reloadPouch[3],reloadBolt[3];
static BridgeHand testReloadHands[2];static int testReloadActive,testGripAuditActive,testGripAuditWeapon;static unsigned reloadEpoch;static DWORD testManualPauseAt;static int testManualPauseResumed,testManualPauseSampled;
/* Diagnostic view index only; never enabled by ordinary gameplay. */
static int testArsenalView=-1;
static BYTE *gameBase;
static void (__cdecl *originalSkyClip)(void *);
static const BYTE skyClipSignature[8]={0x83,0xec,0x3c,0xe8,0xa8,0x49,0xff,0xff};
static void __attribute__((force_align_arg_pointer)) __cdecl StereoSkyClip(void *tess){
    /* Native clipping rejects a sky triangle when all vertices lie inside
     * the terrain/fog cull radius. Sky coverage must remain independent of
     * that finite world radius in the eye/background passes. */
    uint32_t *radius=(uint32_t *)(gameBase+0x7a2614),saved=*radius;
    int active=shared && shared->helperStatus>=2;
    if(active)*radius=0;
    originalSkyClip(tess);
    if(active){*radius=saved;static unsigned logged;if(logged<2){float value;memcpy(&value,&saved,4);LOG_INFO("SKY native triangle coverage: terrain/fog radius %.2f excluded only during sky clipping",value);++logged;}}
}
static void InstallSkyClip(BYTE *base){
    if(originalSkyClip || memcmp(base+0xa66e0,skyClipSignature,8))return;
    BYTE *t=VirtualAlloc(NULL,32,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!t)return;
    memcpy(t,skyClipSignature,8);*(int32_t *)(t+4)=(int32_t)((base+0x9b090)-(t+8));
    t[8]=0xe9;*(int32_t *)(t+9)=(int32_t)((base+0xa66e8)-(t+13));DWORD protect;
    if(!VirtualProtect(t,32,PAGE_EXECUTE_READ,&protect)){VirtualFree(t,0,MEM_RELEASE);return;}
    if(!VirtualProtect(base+0xa66e0,8,PAGE_EXECUTE_READWRITE,&protect)){VirtualFree(t,0,MEM_RELEASE);return;}
    originalSkyClip=(void *)t;base[0xa66e0]=0xe9;*(int32_t *)(base+0xa66e1)=(int32_t)((BYTE *)StereoSkyClip-(base+0xa66e5));memset(base+0xa66e5,0x90,3);
    VirtualProtect(base+0xa66e0,8,protect,&protect);FlushInstructionCache(GetCurrentProcess(),base+0xa66e0,8);
    LOG_INFO("Verified native sky triangle-coverage adapter installed");
}
static volatile LONG sceneGuard;
static DWORD lastHookCheck;
static BridgeTracking tracking;
static uint32_t renderedTrackingSerial;
static BridgeView activeView;
static int activeEye,eyeProjectionChecked;
static int recentered;
static int tracePoses;
static DWORD lastPoseTrace;
static float referencePosition[3], referenceInverse[4];
static void Rotate(const float q[4],const float v[3],float out[3]);
static HGLRC eyeContext;
static uint32_t eyeWidth, eyeHeight;
static GLuint eyeFBO[2], eyeColor[2], eyeDepth[2];
static unsigned char *eyePixels[2], *desktopPixels;
static size_t desktopCapacity;
static PFNGLGENFRAMEBUFFERSPROC genFBO;
static PFNGLBINDFRAMEBUFFERPROC bindFBO;
static PFNGLFRAMEBUFFERTEXTURE2DPROC attachTexture;
static PFNGLGENRENDERBUFFERSPROC genRB;
static PFNGLBINDRENDERBUFFERPROC bindRB;
static PFNGLRENDERBUFFERSTORAGEPROC storeRB;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC attachRB;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC checkFBO;
static PFNGLBLITFRAMEBUFFERPROC blitFBO;
static PFNGLGENBUFFERSPROC readGenBuffers;
static PFNGLBINDBUFFERPROC readBindBuffer;
static PFNGLBUFFERDATAPROC readBufferData;
static PFNGLGETBUFFERPARAMETERIVPROC readBufferSize;
static PFNGLMAPBUFFERRANGEPROC readMapBuffer;
static PFNGLUNMAPBUFFERPROC readUnmapBuffer;
static PFNGLFENCESYNCPROC readFence;
static PFNGLCLIENTWAITSYNCPROC readWait;
static PFNGLDELETESYNCPROC readDeleteSync;
static struct {GLuint buffers[2];GLsync fence;BridgeView views[2];unsigned sequence;} readSlots[2];
static int readbackInitialized,readbackAvailable;
static unsigned readSequence;
static int desktopMirror=1;
static double perfEyes,perfRead,perfDesktop;static unsigned perfSamples;
static double PerfNow(void){LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return (double)t.QuadPart*1000/f.QuadPart;}
typedef struct {
    int type,flags,model;
    float axis[3][3];int nonNormalized;
    float origin[3];unsigned char reserved40[12];
    int customShader;unsigned char shaderRGBA[4];
    unsigned char reserved54[36];
    /* R_SetupEntityLighting, verified RVA 92440, uses +78 with renderfx 80.
     * +40 is unrelated animation data, not the lighting sample position. */
    float lightingOrigin[3];
    unsigned char tail[76];
} GameEntity;
typedef char EntitySizeCheck[sizeof(GameEntity)==208?1:-1];
typedef char EntityOriginCheck[offsetof(GameEntity,origin)==52?1:-1];
typedef char EntityShaderCheck[offsetof(GameEntity,shaderRGBA)==80?1:-1];
typedef char EntityLightingCheck[offsetof(GameEntity,lightingOrigin)==0x78?1:-1];
typedef void (__cdecl *EntityFn)(const GameEntity *);
typedef int (__cdecl *ModelFn)(const char *);
static GameRefdef playerBase;
static DWORD playerBaseTime;
static int playerScene;
static void MenuKey(int key){
    typedef void (__cdecl *KeyFn)(int,int,unsigned);
    KeyFn keyEvent=(KeyFn)(gameBase+0xbab0);
    keyEvent(key,1,GetTickCount());keyEvent(key,0,GetTickCount());LOG_INFO("MENU key=%d",key);
}
static int NativeGameplayPageOpen(void){
    /* Identify the actual top page, so closing objectives on grip also works
     * when it pauses gameplay, without dismissing pause/settings dialogs. */
    BYTE *base=(BYTE *)GetModuleHandleA("Menusx86.dll");static BYTE *verified;
    if(!base)return 0;
    if(base!=verified){
        PIMAGE_DOS_HEADER dos=(void *)base;if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return 0;
        PIMAGE_NT_HEADERS nt=(void *)(base+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.ImageBase!=0x40000000 ||
           nt->OptionalHeader.SizeOfImage!=0x3c000 || nt->FileHeader.TimeDateStamp!=0x3db45796)return 0;
        IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);int matched=0;
        for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)if(!memcmp(sections[i].Name,".text",5)){
            if(sections[i].VirtualAddress!=0x1000 || sections[i].SizeOfRawData!=188416)return 0;
            uint32_t hash=2166136261u;
            for(unsigned j=0;j<188416;++j)hash=(hash^base[0x1000+j])*16777619u;
            if(hash!=0x49b3a404u)return 0;matched=1;
        }
        if(!matched)return 0;verified=base;
    }
    /* Verified menu manager +4 = top page, page +8 = authored page name.
     * ReadProcessMemory bounds each heap read during menu destruction. */
    BYTE *manager=*(BYTE **)(base+0x37534),*page=NULL;char *name=NULL,text[64]={0};SIZE_T read;
    if(!manager || !ReadProcessMemory(GetCurrentProcess(),manager+4,&page,sizeof(page),&read) || !page ||
       !ReadProcessMemory(GetCurrentProcess(),page+8,&name,sizeof(name),&read) || !name ||
       !ReadProcessMemory(GetCurrentProcess(),name,text,sizeof(text)-1,&read))return 0;
    return !_stricmp(text,"objective") || !_stricmp(text,"inventory") || !_stricmp(text,"weapons");
}

static int trackWeapon=1,weaponSupported;
static VRBladeMotion knifeMotion;
static int knifeContact=-1;
static unsigned knifeEpoch;
static int knifeReverse,offhandKnife;
static void ResetKnifeMotion(void){
    knifeMotion.sampled=0;knifeContact=-1;
}
static unsigned attachedHandsMask;
static int nativeHandModels[2];
static int nativeGripModels[2][17];
static int attachedGripModels[37][2];
static struct {int active,weapon;unsigned epoch;float basis[3][3];} actionHand;
static WeaponWheel weaponWheel;
static VRHolsters holsters;
static int bodyIKEnabled,bodyOutfitOverride=-1;
static float bodyWrists[2][3],bodyWristWidths[2][3];static unsigned bodyWristMask;
static VRArmIK bodyArms[2];
static uint32_t wheelSerial;
static unsigned *hiddenArmFlags[32],savedArmFlags[32],hiddenArmCount;
static unsigned char entityBackup[1024][256];
static int ControllerShotPose(float origin[3],float axis[3][3]);

static void __cdecl BridgeScene(const GameRefdef *fd);
static BOOL WINAPI BridgeSwap(HDC dc);
static void UpdateControls(void);
void OpenGL_EnsureForwarding(void);

static BOOL CALLBACK InitializeForwarding(PINIT_ONCE once,PVOID parameter,PVOID *context) {
    (void)once;(void)parameter;(void)context;
    char path[MAX_PATH];UINT length=GetSystemDirectoryA(path,sizeof(path));
    if(!length || length>=sizeof(path)-sizeof("\\opengl32.dll"))return FALSE;
    strcat(path,"\\opengl32.dll");systemGL=LoadLibraryA(path);
    if(!systemGL)return FALSE;
    OpenGL_ForwardInit(systemGL);
    realWglSwap=(SwapFn)GetProcAddress(systemGL,"wglSwapBuffers");
    realWglGetProcAddress=(void *)GetProcAddress(systemGL,"wglGetProcAddress");
    realMatrix=(MatrixFn)GetProcAddress(systemGL,"glLoadMatrixf");
    realDepthRange=(void *)GetProcAddress(systemGL,"glDepthRange");
    realViewport=(RectFn)GetProcAddress(systemGL,"glViewport");
    realScissor=(RectFn)GetProcAddress(systemGL,"glScissor");
    realDrawElements=(void *)GetProcAddress(systemGL,"glDrawElements");
    LOG_INFO("System OpenGL forwarding initialized outside DllMain");
    return realWglSwap && realMatrix && realViewport && realScissor;
}
/* Called by the forwarding assembly before it tail-jumps to a real export.
 * Realign the stack because this is reached from an assembly save-registers
 * stub rather than a compiler-generated call site. */
__attribute__((force_align_arg_pointer)) void OpenGL_EnsureForwarding(void) {
    if(!InitOnceExecuteOnce(&forwardingOnce,InitializeForwarding,NULL,NULL)){
        LOG_ERROR("Unable to initialize system OpenGL: %lu",GetLastError());
        ExitProcess(ERROR_DLL_INIT_FAILED);
    }
}

/* These three exports intercept the renderer's final backend GL calls. Its
 * own projection setup would otherwise overwrite the OpenXR asymmetric FOV.
 * Preserve the game's inReach/far terms and replace only the four FOV terms. */
void APIENTRY Proxy_glLoadMatrixf(const GLfloat *matrix) {
    if(!realMatrix)OpenGL_EnsureForwarding();
    GLint mode = 0;
    if (eyeActive) glGetIntegerv(GL_MATRIX_MODE, &mode);
    /* Fog, lights and screen effects also load orthographic projections.
     * Replacing their scale/translation with an eye FOV corrupts the pass. */
    if (eyeActive && mode == GL_PROJECTION && fabsf(matrix[11]+1)<.001f && fabsf(matrix[15])<.001f) {
        float p[16];
        float l = tanf(activeView.fov[0]), r = tanf(activeView.fov[1]);
        float u = tanf(activeView.fov[2]), d = tanf(activeView.fov[3]);
        memcpy(p, matrix, sizeof(p));
        p[0] = 2.0f / (r - l); p[5] = 2.0f / (u - d);
        p[8] = (r + l) / (r - l); p[9] = (u + d) / (u - d);
        static unsigned fovLogged;
        if(!fovLogged){fovLogged=1;LOG_INFO("EYE FOV native asymmetric projection left=%.4f right=%.4f up=%.4f down=%.4f scale=%.4f,%.4f offset=%.4f,%.4f",activeView.fov[0],activeView.fov[1],activeView.fov[2],activeView.fov[3],p[0],p[5],p[8],p[9]);}
        if(fabsf(matrix[11]+1)<.001f && fabsf(matrix[15])<.001f){
            float originalNear=matrix[14]/(matrix[10]-1);
            if(isfinite(originalNear) && originalNear>.5f){
                const float nearDistance=.5f; /* 1.27 cm in native game units. */
                float farDistance=matrix[14]/(matrix[10]+1);
                if(isfinite(farDistance) && farDistance>nearDistance){
                    p[10]=-(farDistance+nearDistance)/(farDistance-nearDistance);
                    p[14]=-2*farDistance*nearDistance/(farDistance-nearDistance);
                }else{p[10]=-1;p[14]=-2*nearDistance;}
                static int logged;if(!logged){logged=1;LOG_INFO("VR near clip %.3f -> %.3f game units (1.27 cm); original far plane retained",originalNear,nearDistance);}
            }
        }
        realMatrix(p);
    } else realMatrix(matrix);
}
static void EnsureEyeProjection(void){
    if(!eyeActive || eyeProjectionChecked)return;
    GLfloat p[16];glGetFloatv(GL_PROJECTION_MATRIX,p);
    if(fabsf(p[11]+1)>.001f || fabsf(p[15])>.001f)return;
    eyeProjectionChecked=1;float l=tanf(activeView.fov[0]),r=tanf(activeView.fov[1]),u=tanf(activeView.fov[2]),d=tanf(activeView.fov[3]);
    float expected[4]={2/(r-l),2/(u-d),(r+l)/(r-l),(u+d)/(u-d)};
    int changed=fabsf(p[0]-expected[0])>.0001f || fabsf(p[5]-expected[1])>.0001f || fabsf(p[8]-expected[2])>.0001f || fabsf(p[9]-expected[3])>.0001f;
    static unsigned logged;
    if(!(logged&(1u<<activeEye))){logged|=1u<<activeEye;LOG_INFO("FOV DRAW eye=%d corrected=%d observed=%.5f,%.5f,%.5f,%.5f expected=%.5f,%.5f,%.5f,%.5f",activeEye,changed,p[0],p[5],p[8],p[9],expected[0],expected[1],expected[2],expected[3]);}
    if(changed){GLint mode;glGetIntegerv(GL_MATRIX_MODE,&mode);glMatrixMode(GL_PROJECTION);p[0]=expected[0];p[5]=expected[1];p[8]=expected[2];p[9]=expected[3];realMatrix(p);glMatrixMode(mode);}
}
#include "sky_capture.h"

/* The verified native sky iterator brackets its draw with glDepthRange.
 * Its cube must not be clipped by the world scene's finite far plane. */
void APIENTRY Proxy_glDepthRange(GLclampd nearValue,GLclampd farValue){
    if(!realDepthRange)OpenGL_EnsureForwarding();
    if(flatMode){realDepthRange(nearValue,farValue);return;}
    uintptr_t caller=(uintptr_t)__builtin_return_address(0);
    if(skyProjectionActive && (!eyeActive || nearValue!=1 || farValue!=1)){
        GLint mode;glGetIntegerv(GL_MATRIX_MODE,&mode);glMatrixMode(GL_PROJECTION);realMatrix(skyProjection);glMatrixMode(mode);skyProjectionActive=0;
        if(skyFogEnabled)glEnable(GL_FOG);else glDisable(GL_FOG);
    }
    /* The game can route qgl calls through a logging thunk. Identify the
     * far-depth sky pass by GL state rather than that thunk's return address. */
    if(eyeActive && nearValue==1 && farValue==1 && !skyProjectionActive){
        glGetFloatv(GL_PROJECTION_MATRIX,skyProjection);
        if(fabsf(skyProjection[11]+1)<.001f && fabsf(skyProjection[15])<.001f){
            GLfloat infinite[16];memcpy(infinite,skyProjection,sizeof(infinite));
            float n=skyProjection[14]/(skyProjection[10]-1);
            if(isfinite(n) && n>0){
                infinite[10]=-1;infinite[14]=-2*n;
                GLint mode;glGetIntegerv(GL_MATRIX_MODE,&mode);glMatrixMode(GL_PROJECTION);realMatrix(infinite);glMatrixMode(mode);skyProjectionActive=1;
                skyFogEnabled=glIsEnabled(GL_FOG);glDisable(GL_FOG);
                static unsigned logged;if(logged<4){GLint depth,fbo;glGetIntegerv(GL_DEPTH_FUNC,&depth);glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);LOG_INFO("SKY far-depth pass caller=%p eyeFBO=%d depthFunc=%x near=%.3f finiteFar=%.3f far-clip=disabled",(void *)caller,fbo,depth,n,skyProjection[14]/(skyProjection[10]+1));++logged;}
            }
        }
    }
    if(nearValue==1 && farValue==1)BeginNativeSkyCapture();
    realDepthRange(nearValue,farValue);
    if(nearValue!=farValue)EndNativeSkyCapture();
}
void APIENTRY Proxy_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if(!realViewport)OpenGL_EnsureForwarding();
    realViewport(eyeActive ? 0 : x, eyeActive ? 0 : y,
                 eyeActive ? eyeWidth : w, eyeActive ? eyeHeight : h);
}
void APIENTRY Proxy_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if(!realScissor)OpenGL_EnsureForwarding();
    realScissor(eyeActive ? 0 : x, eyeActive ? 0 : y,
                eyeActive ? eyeWidth : w, eyeActive ? eyeHeight : h);
}
void APIENTRY Proxy_glDrawElements(GLenum mode,GLsizei count,GLenum type,const void *indices);
void APIENTRY Proxy_glBegin(GLenum);void APIENTRY Proxy_glEnd(void);
void APIENTRY Proxy_glVertex2f(GLfloat,GLfloat);void APIENTRY Proxy_glVertex3f(GLfloat,GLfloat,GLfloat);
void APIENTRY Proxy_glVertex2fv(const GLfloat *);void APIENTRY Proxy_glVertex3fv(const GLfloat *);
void APIENTRY Proxy_glDrawArrays(GLenum,GLint,GLsizei);void APIENTRY Proxy_glRectf(GLfloat,GLfloat,GLfloat,GLfloat);
PROC WINAPI Proxy_wglGetProcAddress(LPCSTR name){
    if(!realWglGetProcAddress)OpenGL_EnsureForwarding();
    if(flatMode)return realWglGetProcAddress?realWglGetProcAddress(name):NULL;
    /* Core functions obtained dynamically must retain the same eye hooks as
     * exported functions; forwarding these queries bypasses stereo sky state. */
    if(name){
        if(!strcmp(name,"glDepthRange"))return (PROC)(void *)Proxy_glDepthRange;
        if(!strcmp(name,"glLoadMatrixf"))return (PROC)(void *)Proxy_glLoadMatrixf;
        if(!strcmp(name,"glViewport"))return (PROC)(void *)Proxy_glViewport;
        if(!strcmp(name,"glScissor"))return (PROC)(void *)Proxy_glScissor;
        if(!strcmp(name,"glDrawElements"))return (PROC)(void *)Proxy_glDrawElements;
    }
    if(name){
        if(!strcmp(name,"glBegin"))return (PROC)(void *)Proxy_glBegin;
        if(!strcmp(name,"glEnd"))return (PROC)(void *)Proxy_glEnd;
        if(!strcmp(name,"glVertex2f"))return (PROC)(void *)Proxy_glVertex2f;
        if(!strcmp(name,"glVertex3f"))return (PROC)(void *)Proxy_glVertex3f;
        if(!strcmp(name,"glVertex2fv"))return (PROC)(void *)Proxy_glVertex2fv;
        if(!strcmp(name,"glVertex3fv"))return (PROC)(void *)Proxy_glVertex3fv;
        if(!strcmp(name,"glDrawArrays"))return (PROC)(void *)Proxy_glDrawArrays;
        if(!strcmp(name,"glRectf"))return (PROC)(void *)Proxy_glRectf;
    }
    return realWglGetProcAddress?realWglGetProcAddress(name):NULL;
}
__declspec(dllexport) BOOL APIENTRY wglSwapBuffers(HDC dc) {
    if(!realWglSwap)OpenGL_EnsureForwarding();
    return realWglSwap ? realWglSwap(dc) : FALSE;
}

static int HudFullBounds(const GLfloat *bounds);
static int HudProjectedBounds(const GLfloat *bounds,GLfloat out[4]);
static int FullScreenBatch(GLsizei count,GLenum type,const void *indices,const GLint viewport[4]){
    if(count<4 || count>12 || !indices)return 0;
    GLint elementBuffer=0,vertexBuffer=0,stride=0,size=0,format=0;void *vertices=NULL;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&elementBuffer);
    glGetIntegerv(GL_VERTEX_ARRAY_BUFFER_BINDING,&vertexBuffer);
    if(elementBuffer || vertexBuffer)return 0;
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE,&stride);glGetIntegerv(GL_VERTEX_ARRAY_SIZE,&size);
    glGetIntegerv(GL_VERTEX_ARRAY_TYPE,&format);glGetPointerv(GL_VERTEX_ARRAY_POINTER,&vertices);
    if(!vertices || format!=GL_FLOAT || size<2 || size>4)return 0;if(!stride)stride=size*4;
    if(stride<8 || stride>256)return 0;
    float minX=1e9f,minY=1e9f,maxX=-1e9f,maxY=-1e9f;
    for(int n=0;n<count;++n){
        unsigned index;if(type==GL_UNSIGNED_INT)index=((const unsigned *)indices)[n];
        else if(type==GL_UNSIGNED_SHORT)index=((const unsigned short *)indices)[n];
        else if(type==GL_UNSIGNED_BYTE)index=((const unsigned char *)indices)[n];else return 0;
        if(index>=16384)return 0;const float *v=(const float *)((const BYTE *)vertices+(size_t)index*stride);
        minX=fminf(minX,v[0]);maxX=fmaxf(maxX,v[0]);minY=fminf(minY,v[1]);maxY=fmaxf(maxY,v[1]);
    }
    (void)viewport;GLfloat bounds[4]={minX,minY,maxX,maxY};return HudFullBounds(bounds);
}

static int CinematicBars(GLsizei count,GLenum type,const void *indices,const GLint viewport[4]){
    if(playerScene || (count!=6 && count!=12) || !indices || !glIsEnabled(GL_COLOR_ARRAY))return 0;
    GLint vb=0,eb=0,stride=0,size=0,format=0,colorStride=0,colorType=0,colorSize=0;void *vertices=NULL,*colors=NULL;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&eb);glGetIntegerv(GL_VERTEX_ARRAY_BUFFER_BINDING,&vb);if(vb || eb)return 0;
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE,&stride);glGetIntegerv(GL_VERTEX_ARRAY_SIZE,&size);glGetIntegerv(GL_VERTEX_ARRAY_TYPE,&format);glGetPointerv(GL_VERTEX_ARRAY_POINTER,&vertices);
    glGetIntegerv(GL_COLOR_ARRAY_STRIDE,&colorStride);glGetIntegerv(GL_COLOR_ARRAY_TYPE,&colorType);glGetIntegerv(GL_COLOR_ARRAY_SIZE,&colorSize);glGetPointerv(GL_COLOR_ARRAY_POINTER,&colors);
    if(!vertices || !colors || format!=GL_FLOAT || size<2 || size>4 || colorType!=GL_UNSIGNED_BYTE || colorSize!=4)return 0;
    if(!stride)stride=size*4;if(!colorStride)colorStride=4;if(stride<8 || stride>256 || colorStride<4 || colorStride>256)return 0;
    if(glIsEnabled(GL_TEXTURE_2D)){GLint width=0,height=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);if(width>16 || height>16 || width<1 || height<1)return 0;}
    for(int group=0;group<count;group+=6){float minX=1e9f,maxX=-1e9f,minY=1e9f,maxY=-1e9f;
        for(int j=0;j<6;++j){unsigned i;if(type==GL_UNSIGNED_INT)i=((const unsigned *)indices)[group+j];else if(type==GL_UNSIGNED_SHORT)i=((const unsigned short *)indices)[group+j];else if(type==GL_UNSIGNED_BYTE)i=((const unsigned char *)indices)[group+j];else return 0;
            if(i>=16384)return 0;const float *v=(const float *)((const BYTE *)vertices+i*stride);const BYTE *c=(const BYTE *)colors+i*colorStride;
            if(c[0]>2 || c[1]>2 || c[2]>2 || !c[3])return 0;
            minX=fminf(minX,v[0]);maxX=fmaxf(maxX,v[0]);minY=fminf(minY,v[1]);maxY=fmaxf(maxY,v[1]);
        }
        /* HUD vertices use the authored 640x480 coordinate system even
         * when the physical viewport is 1920x1080 or a 1x1 mirror. */
        GLfloat bounds[4]={minX,minY,maxX,maxY},q[4];
        if(!HudProjectedBounds(bounds,q) || q[2]-q[0]<.9f || q[3]-q[1]>.65f || q[3]-q[1]<.003f)return 0;
    }
    (void)viewport;static int logged;if(!logged){logged=1;LOG_INFO("CINEMATIC solid letterbox/subtitle backdrops omitted using projected bounds");}return 1;
}

#include "weapon_material.h"
#include "hud_capture.h"

/* The backend's orthographic batches contain the authored HUD/text. Replay
 * those batches into each eye using the same texture, vertices and blending.
 * Push/pop all GL attributes so the engine's cached state remains valid. */
void APIENTRY Proxy_glDrawElements(GLenum mode,GLsizei count,GLenum type,const void *indices){
    EnsureEyeProjection();
    if(!realDrawElements)OpenGL_EnsureForwarding();
    if(flatMode){realDrawElements(mode,count,type,indices);return;}
    GLint savedCull=GL_BACK;int reflected=0;
    if(eyeActive && (leftHanded || offhandKnife) && glIsEnabled(GL_CULL_FACE)){
        GLfloat m[16];glGetFloatv(GL_MODELVIEW_MATRIX,m);
        float det=m[0]*(m[5]*m[10]-m[6]*m[9])-m[4]*(m[1]*m[10]-m[2]*m[9])+m[8]*(m[1]*m[6]-m[2]*m[5]);
        if(det<0){glGetIntegerv(GL_CULL_FACE_MODE,&savedCull);if(savedCull==GL_FRONT || savedCull==GL_BACK){glCullFace(savedCull==GL_FRONT?GL_BACK:GL_FRONT);reflected=1;}}
    }
    WeaponMaterialDrawState material;
    int customMaterial=BeginWeaponMaterial(mode,&material);
    realDrawElements(mode,count,type,indices);
    if(customMaterial)EndWeaponMaterial(&material);
    if(reflected)glCullFace(savedCull);
    CaptureHud(0,mode,count,type,indices,0,0,NULL,NULL);
}

static int HelperAlive(void) {
    if (!shared || !helperProcess) return 0;
    if (WaitForSingleObject(helperProcess, 0) != WAIT_TIMEOUT) {
        if (!helperFailureLogged) {
            DWORD code = 0;
            GetExitCodeProcess(helperProcess, &code);
            LOG_ERROR("64-bit helper exited: code=%lu status=%ld", code, shared->helperStatus);
            helperFailureLogged = 1;
        }
        return 0;
    }
    return 1;
}

static void StartBridge(void) {
    char name[80], executable[MAX_PATH], command[MAX_PATH + 100];
    STARTUPINFOA startup = {0}; PROCESS_INFORMATION process = {0};
    if (bridgeAttempted) return;
    bridgeAttempted = 1;
    LOG_INFO("BUILD native-vr-options-20261101; user hand poses preserved; owner testing only; gameplay evaluation pending");
    char trace[8];
    trackWeapon=!GetEnvironmentVariableA("SOF2_VR_NO_WEAPON",trace,sizeof(trace));
    tracePoses=GetEnvironmentVariableA("SOF2_VR_TRACE",trace,sizeof(trace))>0;
    if(tracePoses && GetEnvironmentVariableA("SOF2_VR_TEST_QUIT_SECONDS",trace,sizeof(trace))){
        unsigned seconds=(unsigned)atoi(trace);if(seconds>=2 && seconds<=60)testQuitDeadline=GetTickCount()+seconds*1000;
    }
    if (!GetModuleFileNameA(selfModule, executable, sizeof(executable))) return;
    char *slash = strrchr(executable, '\\');
    if (!slash || (size_t)(slash - executable) + 48 >= sizeof(executable)) return;
    /* A separate executable directory prevents Windows from trying to load
     * the game's x86 opengl32.dll into this x64 process. */
    strcpy(slash + 1, "sof2-vr-x64\\sof2_vr_bridge64.exe");
    char quality[MAX_PATH];strcpy(quality,executable);
    char *qualityName=strrchr(quality,'\\');strcpy(qualityName+1,"quality.ini");
    strcpy(qualityPath,quality);
    desktopMirror=GetPrivateProfileIntA("Graphics","DesktopMirror",1,quality)!=0;
    controlsEnabled=GetPrivateProfileIntA("Controls","Enabled",1,quality)!=0;
    leftHanded=GetPrivateProfileIntA("Controls","LeftHanded",0,quality)!=0;
    manualPump=GetPrivateProfileIntA("Controls","ManualPump",1,quality)!=0;
    manualReloadEnabled=GetPrivateProfileIntA("Controls","ManualReload",1,quality)!=0;
    accessibility=VR_ReadAccessibility(quality);
    GameVR_SetEnemyDamagePercent(accessibility.enemyDamage);
    leftHanded=accessibility.leftHanded;manualReloadEnabled=accessibility.manualReload;manualPump=accessibility.manualPump;
    holsters.holdMs=accessibility.holdMs;
    bodyIKEnabled=GetPrivateProfileIntA("Body","Enabled",0,quality)!=0;
    char bodyOutfit[40];GetPrivateProfileStringA("Body","Outfit","auto",bodyOutfit,sizeof(bodyOutfit),quality);
    for(unsigned i=0;i<sizeof(vrBodyProfiles)/sizeof(vrBodyProfiles[0]);++i)if(!_stricmp(bodyOutfit,vrBodyProfiles[i].name))bodyOutfitOverride=(int)i;
    int hw=GetPrivateProfileIntA("HUD","WidthPercent",84,quality),hh=GetPrivateProfileIntA("HUD","HeightPercent",75,quality),ho=(int)GetPrivateProfileIntA("HUD","VerticalOffsetPercent",-4,quality);
    int opacity=GetPrivateProfileIntA("HUD","OpacityPercent",65,quality);
    hudWidth=(hw>=50 && hw<=100?hw:84)*.01f;hudHeight=(hh>=40 && hh<=100?hh:75)*.01f;hudOffset=(ho>=-20 && ho<=20?ho:-4)*.01f;
    hudOpacity=(opacity>=25 && opacity<=100?opacity:65)*.01f;
    LOG_INFO("HUD screen width=%.2f height=%.2f offset=%.2f opacity=%.2f wrist=disabled",hudWidth,hudHeight,hudOffset,hudOpacity);
    int angle=(int)GetPrivateProfileIntA("Controls","SnapAngle",45,quality);
    snapAngle=accessibility.snapAngle;(void)angle;
    snprintf(name, sizeof(name), "Local\\SoF2VRBridge_%lu", GetCurrentProcessId());
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                sizeof(BridgeShared), name);
    if (!mapping) { LOG_ERROR("Bridge mapping creation failed: %lu", GetLastError()); return; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG_ERROR("Bridge mapping already exists for this process"); return;
    }
    shared = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*shared));
    if (!shared) { LOG_ERROR("Bridge mapping view failed: %lu", GetLastError()); return; }
    memset(shared, 0, sizeof(*shared));
    shared->magic = BRIDGE_MAGIC; shared->version = BRIDGE_VERSION;
    shared->structureBytes = sizeof(*shared); shared->producerPid = GetCurrentProcessId();
    snprintf(command, sizeof(command), "\"%s\" --pid %lu", executable, GetCurrentProcessId());
    startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (!CreateProcessA(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process)) {
        LOG_ERROR("Launching 64-bit helper failed: %lu (%s)", GetLastError(), executable);
        return;
    }
    helperProcess = process.hProcess; CloseHandle(process.hThread);
    LOG_INFO("Bridge v%u: x86 producer pid=%lu -> x64 helper pid=%lu; bytes=%u",
             BRIDGE_VERSION, GetCurrentProcessId(), process.dwProcessId, (unsigned)sizeof(*shared));
}

/* All addresses below are for the verified 2002 GOG SP executable. Fail closed
 * on other builds. The refdef prefix and queue/counter addresses were checked
 * against that executable's disassembly, not inferred from RTCW layouts. */
static void EnsureNativeGLDispatch(void){
    if(!gameBase || !wglGetCurrentContext())return;
    /* Renderer initialization/context recreation fills this after the scene
     * hook is installed, and GL logging can replace it again later. */
    void **slot=(void **)(gameBase+0x841c44);DWORD protect;
    if(!*slot || *slot==(void *)Proxy_glDepthRange)return;
    if(VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&protect)){
        LOG_INFO("SKY native depth dispatch intercepted (previous=%p)",*slot);
        *slot=(void *)Proxy_glDepthRange;
        VirtualProtect(slot,sizeof(*slot),protect,&protect);
    }
}
static int InstallSceneHook(void) {
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)GetModuleHandleA(NULL);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE *)dos + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.ImageBase != 0x10000000 || nt->OptionalHeader.SizeOfImage != 0x848000 ||
        nt->FileHeader.TimeDateStamp != 0x3db457db) return 0;
    BYTE *base = (BYTE *)dos;
    /* Fingerprint all of .text before using renderer globals, not just the
     * API slot or a short entry signature. This executable is not relocatable
     * at a different image base and was checked above. */
    IMAGE_SECTION_HEADER *section=IMAGE_FIRST_SECTION(nt);
    int matchedText=0;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){
        if(memcmp(section[i].Name,".text",5))continue;
        if(section[i].VirtualAddress!=0x1000 || section[i].SizeOfRawData!=888832)return 0;
        uint32_t hash=2166136261u;
        for(uint32_t offset=0;offset<section[i].SizeOfRawData;++offset){
            uint32_t rva=section[i].VirtualAddress+offset;BYTE value=base[rva];
            if(originalSkyClip && base[0xa66e0]==0xe9 && base+0xa66e5+*(int32_t *)(base+0xa66e1)==(BYTE *)StereoSkyClip && rva>=0xa66e0 && rva<0xa66e8)value=skyClipSignature[rva-0xa66e0];
            if(originalSkyIterator && base[0xa6f10]==0xe9 && base+0xa6f15+*(int32_t *)(base+0xa6f11)==(BYTE *)StereoSkyIterator && rva>=0xa6f10 && rva<0xa6f15)value=skyIteratorSignature[rva-0xa6f10];
            hash=(hash^value)*16777619u;
        }
        if(hash!=0xffffe04eu){LOG_WARN("SoF2 code fingerprint mismatch: %08x",hash);return 0;}
        matchedText=1;
    }
    if(!matchedText)return 0;
    const BYTE sceneSignature[] = {0xa0,0xe8,0x1f,0x7a,0x10,0x81,0xec,0x2c,0x02,0,0};
    const BYTE flushSignature[] = {0xa0,0xe8,0x1f,0x7a,0x10,0x84,0xc0,0x74,0x35};
    if (memcmp(base + 0x99e30, sceneSignature, sizeof(sceneSignature)) ||
        memcmp(base + 0x83020, flushSignature, sizeof(flushSignature))) return 0;
    DWORD *slot = (DWORD *)(base + 0x799710), protect;
    if (*slot != (DWORD)(ULONG_PTR)(base + 0x99e30)) return 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protect)) return 0;
    originalScene = (SceneFn)(ULONG_PTR)*slot;
    sceneSlot=slot;
    flushCommands = (FlushFn)(base + 0x83020);
    gameBase = base;
    const BYTE commandSignature[]={0x8b,0x54,0x24,0x04,0x83,0xc9,0xff,0x57,0x33,0xc0};
    const BYTE yawSignature[]={0xd8,0x05,0x34,0x2c,0x1f,0x10,0xd9,0x1d,0x34,0x2c,0x1f,0x10};
    const BYTE cvarSignature[]={0xd9,0x47,0x5c,0x5f,0x5e,0x5d,0x5b,0xc3};
    if(!memcmp(base+0x33090,commandSignature,sizeof(commandSignature)) &&
       !memcmp(base+0x97b1,yawSignature,sizeof(yawSignature)) &&
       !memcmp(base+0x36653,cvarSignature,sizeof(cvarSignature))){
        addCommand=(CommandFn)(base+0x33090);cvarFloat=(CvarFloatFn)(base+0x36590);
        /* Native three-argument setter thunk: name/value/flags, forced set.
         * Updates are immediate and cannot accumulate behind a queued wait. */
        const BYTE setSignature[]={0x8b,0x44,0x24,0x0c,0x8b,0x4c,0x24,0x08,0x8b,0x54,0x24,0x04,0x50,0x6a,0x01};
        if(!memcmp(base+0x36f90,setSignature,sizeof(setSignature)))nativeCvarSet=(NativeCvarSetFn)(base+0x36f90);
    }
    *slot = (DWORD)(ULONG_PTR)BridgeScene;
    VirtualProtect(slot, sizeof(*slot), protect, &protect);
    InstallSkyClip(base);
    InstallSkyIterator(base);
    /* This build's sky iterator calls qglDepthRange through this verified
     * native dispatch slot. Keep it intercepted even if the engine obtained
     * a system-GL pointer directly instead of using our exported function. */
    EnsureNativeGLDispatch();
    LOG_INFO("Verified SoF2 scene and queue-flush hooks installed");
    return 1;
}

static int MakeEyeTargets(void) {
    if (!shared || !shared->eyeWidth || !shared->eyeHeight ||
        shared->eyeWidth > BRIDGE_MAX_WIDTH || shared->eyeHeight > BRIDGE_MAX_HEIGHT) return 0;
    eyeWidth = shared->eyeWidth; eyeHeight = shared->eyeHeight;
    HGLRC current = wglGetCurrentContext();
    if (eyeContext == current && eyeFBO[0]) return 1;
    if (!current) return 0;
    genFBO = (PFNGLGENFRAMEBUFFERSPROC)wglGetProcAddress("glGenFramebuffers");
    bindFBO = (PFNGLBINDFRAMEBUFFERPROC)wglGetProcAddress("glBindFramebuffer");
    attachTexture = (PFNGLFRAMEBUFFERTEXTURE2DPROC)wglGetProcAddress("glFramebufferTexture2D");
    genRB = (PFNGLGENRENDERBUFFERSPROC)wglGetProcAddress("glGenRenderbuffers");
    bindRB = (PFNGLBINDRENDERBUFFERPROC)wglGetProcAddress("glBindRenderbuffer");
    storeRB = (PFNGLRENDERBUFFERSTORAGEPROC)wglGetProcAddress("glRenderbufferStorage");
    attachRB = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)wglGetProcAddress("glFramebufferRenderbuffer");
    checkFBO = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)wglGetProcAddress("glCheckFramebufferStatus");
    blitFBO = (PFNGLBLITFRAMEBUFFERPROC)wglGetProcAddress("glBlitFramebuffer");
    if (!genFBO || !bindFBO || !attachTexture || !genRB || !bindRB || !storeRB || !attachRB || !checkFBO) return 0;
    GLint previousFBO, previousRB;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFBO);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRB);
    glPushAttrib(GL_TEXTURE_BIT);
    genFBO(2, eyeFBO); glGenTextures(2, eyeColor); genRB(2, eyeDepth);
    int complete = 1;
    for (int eye = 0; eye < 2; ++eye) {
        if (!eyePixels[eye]) eyePixels[eye] = malloc(eyeWidth * eyeHeight * 4);
        if (!eyePixels[eye]) { complete = 0; break; }
        glBindTexture(GL_TEXTURE_2D, eyeColor[eye]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, eyeWidth, eyeHeight,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        bindFBO(GL_FRAMEBUFFER, eyeFBO[eye]);
        attachTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eyeColor[eye], 0);
        bindRB(GL_RENDERBUFFER, eyeDepth[eye]);
        storeRB(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, eyeWidth, eyeHeight);
        attachRB(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, eyeDepth[eye]);
        if (checkFBO(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { complete = 0; break; }
    }
    bindRB(GL_RENDERBUFFER, previousRB); bindFBO(GL_FRAMEBUFFER, previousFBO);
    glPopAttrib();
    if (!complete) { LOG_ERROR("Eye target creation failed"); return 0; }
    eyeContext = current; recentered = 0;
    /* Buffer and fence names belong to this GL context. */
    memset(readSlots,0,sizeof(readSlots));readbackInitialized=readbackAvailable=0;readSequence=0;
    LOG_INFO("Native stereo eye targets created: %ux%u", eyeWidth, eyeHeight);
    return 1;
}

static void Rotate(const float q[4], const float v[3], float out[3]) {
    float t[3] = {2*(q[1]*v[2]-q[2]*v[1]), 2*(q[2]*v[0]-q[0]*v[2]), 2*(q[0]*v[1]-q[1]*v[0])};
    out[0] = v[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1];
    out[1] = v[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2];
    out[2] = v[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0];
}
static void Multiply(const float a[4], const float b[4], float out[4]) {
    out[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    out[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    out[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    out[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
static float TrackingReference(int i){return !accessibility.roomScale && tracking.valid?(tracking.views[0].position[i]+tracking.views[1].position[i])*.5f:referencePosition[i];}
static void PoseInGame(const GameRefdef *base,const BridgePose *pose,float origin[3],float axis[3][3]) {
    const float basis[3][3]={{0,0,-1},{-1,0,0},{0,1,0}};
    float q[4],delta[3],position[3];Multiply(referenceInverse,pose->orientation,q);
    for(int i=0;i<3;++i)delta[i]=pose->position[i]-TrackingReference(i);
    Rotate(referenceInverse,delta,position);
    for(int i=0;i<3;++i)origin[i]=base->origin[i]+39.37f*(-position[2]*base->axis[0][i]-position[0]*base->axis[1][i]+position[1]*base->axis[2][i]);
    for(int k=0;k<3;++k){float v[3];Rotate(q,basis[k],v);
        for(int i=0;i<3;++i)axis[k][i]=-v[2]*base->axis[0][i]-v[0]*base->axis[1][i]+v[1]*base->axis[2][i];}
}
static int PointMenu(const BridgeTracking *sample){
    static HMODULE checked;static int verified;HMODULE module=GetModuleHandleA("Menusx86.dll");
    if(!module)return 0;
    if(module!=checked){
        checked=module;verified=0;BYTE *m=(BYTE *)module;
        PIMAGE_NT_HEADERS nt=(PIMAGE_NT_HEADERS)(m+((PIMAGE_DOS_HEADER)m)->e_lfanew);
        if((uintptr_t)m!=0x40000000 || nt->FileHeader.TimeDateStamp!=0x3db45796 || nt->OptionalHeader.SizeOfImage!=0x3c000)return 0;
        IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
        for(unsigned n=0;n<nt->FileHeader.NumberOfSections;++n)if(!memcmp(sections[n].Name,".text",5)){
            if(sections[n].VirtualAddress!=0x1000 || sections[n].SizeOfRawData!=188416)return 0;
            uint32_t hash=2166136261u;for(unsigned j=0;j<188416;++j)hash=(hash^m[0x1000+j])*16777619u;
            verified=hash==0x49b3a404u;
        }
    }
    if(!verified)return 0;
    BYTE *m=(BYTE *)module,*menu=*(BYTE **)(m+0x37534);
    DWORD *api=*(DWORD **)(gameBase+0x1477e4);
    if(!menu || !api || api[16]!=(DWORD)(uintptr_t)(m+0x74f0))return 0;
    float *xvar=*(float **)(menu+0xc),*yvar=*(float **)(menu+0x10);
    int width=*(int *)(m+0x37674),height=*(int *)(m+0x37678);
    if(!xvar || !yvar || width<100 || width>8192 || height<100 || height>8192)return 0;
    const BridgeHand *hand=&sample->input.hands[leftHanded?0:1];if(!(hand->valid&BRIDGE_HAND_AIM_VALID))return 0;
    float inverse[4],origin[3],delta[3],direction[3],local[3],forward[3]={0,0,-1};
    if(!sample->panelValid)return 0;
    for(int i=0;i<3;++i){inverse[i]=-sample->panelPose.orientation[i];delta[i]=hand->aim.position[i]-sample->panelPose.position[i];}
    inverse[3]=sample->panelPose.orientation[3];Rotate(inverse,delta,origin);
    Rotate(hand->aim.orientation,forward,direction);Rotate(inverse,direction,local);
    if(local[2]>-.05f)return 0;float t=-origin[2]/local[2];if(t<0)return 0;
    static int stickMode;static float stickReference[4];static DWORD lastMove;
    DWORD now=GetTickCount();float dt=fminf(.05f,(now-lastMove)*.001f);lastMove=now;
    const BridgeHand *stick=&sample->input.hands[0];
    int moving=(stick->valid&BRIDGE_HAND_GRIP_VALID) && (fabsf(stick->stick[0])>.25f || fabsf(stick->stick[1])>.25f);
    if(moving && !stickMode){stickMode=1;memcpy(stickReference,hand->aim.orientation,16);LOG_INFO("MENU cursor: stick");}
    float dot=0;for(int i=0;i<4;++i)dot+=stickReference[i]*hand->aim.orientation[i];
    if(stickMode && !moving && fabsf(dot)<.999f){stickMode=0;LOG_INFO("MENU cursor: controller ray");}
    float nx=.5f+(origin[0]+t*local[0])/2.8f;
    float ny=.5f-(origin[1]+t*local[1])/(2.8f*height/width);
    if(stickMode){
        nx=xvar[0x5c/4]/width;ny=yvar[0x5c/4]/height;
        if(moving){nx+=stick->stick[0]*.6f*dt;ny-=stick->stick[1]*.6f*dt;}
    }
    int inside=nx>=0 && nx<=1 && ny>=0 && ny<=1;
    nx=fminf(1,fmaxf(0,nx));ny=fminf(1,fmaxf(0,ny));
    int dx=(int)lroundf(nx*width-xvar[0x5c/4]),dy=(int)lroundf(ny*height-yvar[0x5c/4]);
    typedef void (__cdecl *MouseFn)(int,int);if(dx || dy)((MouseFn)(uintptr_t)api[16])(dx,dy);
    static DWORD lastTrace;if(tracePoses && GetTickCount()-lastTrace>500){LOG_INFO("MENU pointer=%d,%d size=%d,%d",(int)(nx*width),(int)(ny*height),width,height);lastTrace=GetTickCount();}
    return inside;
}
static void WeaponEntityOrigin(int weapon,const float grip[3],const float axis[3][3],float origin[3]){
    const VRWeaponGrip *profile=WeaponGrip(weapon);const float *anchor=profile?profile->point:NULL;
    for(int i=0;i<3;++i){origin[i]=grip[i];for(int k=0;k<3;++k)origin[i]+=axis[k][i]*(anchor?-anchor[k]:(k==0?-8:k==1?3:5));}
}
static int WeaponPose(const GameRefdef *base,const BridgeTracking *sample,float grip[3],float axis[3][3]) {
    static int supported;
    static int offClosed;static unsigned offSerial;
    const BridgeHand *hand=&sample->input.hands[leftHanded?0:1];
    if(!weaponHeld || !sample->valid || !sample->input.focused || GetTickCount()-sample->input.sampleTimeMs>250 ||
       (hand->valid&3)!=3){supported=weaponSupported=0;return 0;}
    float gripAxis[3][3];PoseInGame(base,&hand->grip,grip,gripAxis);
    float aim[3];PoseInGame(base,&hand->aim,aim,axis);
    int support=0;
    const BridgeHand *off=&sample->input.hands[leftHanded?1:0];
    int supportPress=0;
    if(offSerial!=sample->input.serial){
        int closed=off->squeeze>(offClosed?GripOff():GripOn());
        supportPress=closed && !offClosed;offClosed=closed;offSerial=sample->input.serial;
    }
    VRWeaponState current;const VRWeaponProfile *profile=GameVR_GetWeaponState(&current)?VR_Profile(current.weapon):NULL;
    const VRWeaponGrip *rig=profile?WeaponGrip(current.weapon):NULL;
    if(rig){
        float controllerAxis[3][3];memcpy(controllerAxis,axis,sizeof(controllerAxis));
        for(int j=0;j<3;++j)for(int i=0;i<3;++i){axis[j][i]=0;
            for(int k=0;k<3;++k)axis[j][i]+=controllerAxis[k][i]*rig->basis[j][k]*(leftHanded && k==1?-1:1);
        }
    }else if(leftHanded)for(int i=0;i<3;++i)axis[1][i]=-axis[1][i];
    int pistol=profile && profile->category==2;
    int compact=pistol || (profile && profile->id==7);
    int twoHanded=profile && profile->category>=2 && profile->category<=7;
    static int supportWeapon;static unsigned supportEpoch;
    if(!profile || supportWeapon!=current.weapon || supportEpoch!=reloadEpoch){supported=0;supportWeapon=profile?current.weapon:0;supportEpoch=reloadEpoch;}
    int trackingOff=(off->valid&BRIDGE_HAND_GRIP_VALID)!=0;
    if(offhandKnife || shellLoad.stage || magazine.stage || magazine.boltHeld || weaponWheel.open || !controlsEnabled ||
       !playerScene || !recentered || *(unsigned *)(gameBase+0x1476d4) || cvarFloat("cl_paused"))twoHanded=0;
    if(twoHanded && trackingOff && (supportPress || (accessibility.toggleGrip?supported:off->squeeze>GripOff()))){
        float position[3],unusedAxis[3][3],direction[3],length=0,distance=0;
        PoseInGame(base,&off->grip,position,unusedAxis);
        float authored[3]={0};
        if(rig && rig->supportAuthored)for(int i=0;i<3;++i)for(int k=0;k<3;++k)authored[i]+=axis[k][i]*(rig->support[k]-rig->point[k]);
        else for(int i=0;i<3;++i)authored[i]=axis[0][i]*(compact?2:11);
        for(int i=0;i<3;++i){direction[i]=position[i]-grip[i];length+=direction[i]*direction[i];float d=direction[i]-authored[i];distance+=d*d;}
        length=sqrtf(length);
        /* Distance gates acquisition only. Once held, releasing the squeeze
         * (or losing gameplay/tracking) detaches; arm reach does not. */
        support=accessibility.toggleGrip && supportPress && supported?0:supported || (supportPress && distance<30.25f);
        if(support && !compact && length>1){
            if(profile->pump && loadingLive)
                for(int i=0;i<3;++i)authored[i]-=axis[0][i]*pump.pull*M590_PUMP_TRAVEL;
            float n=sqrtf(authored[0]*authored[0]+authored[1]*authored[1]+authored[2]*authored[2]);
            if(n>.1f){
                float a[3],b[3],q[4];for(int i=0;i<3;++i){a[i]=authored[i]/n;b[i]=direction[i]/length;}
                q[0]=a[1]*b[2]-a[2]*b[1];q[1]=a[2]*b[0]-a[0]*b[2];q[2]=a[0]*b[1]-a[1]*b[0];q[3]=1+a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
                float norm=sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
                if(norm>.001f){for(int i=0;i<4;++i)q[i]/=norm;for(int j=0;j<3;++j){float rotated[3];Rotate(q,axis[j],rotated);memcpy(axis[j],rotated,12);}}
            }
        }
    }
    weaponSupported=support; /* Presentation and ballistic aim share the un-kicked controller basis. */
    /* Reflection was applied in controller space before the native palm
     * correction, so the complete assembly remains centred on that hand. */
    if(support!=supported){LOG_INFO("SUPPORT active=%d",support);supported=support;}
    return 1;
}
static void KnifeRenderAxes(float axis[3][3]){
    if(!knifeReverse)return;
    const VRWeaponGrip *rig=WeaponGrip(1);if(!rig)return;
    float blade[3],cross[3],length=0,old[3][3];memcpy(old,axis,36);
    for(int k=0;k<3;++k)blade[k]=vrKnifeBladeTip[k]-vrKnifeBladeBase[k];
    for(int k=0;k<3;++k){cross[k]=blade[(k+1)%3]*rig->basis[(k+2)%3][0]-blade[(k+2)%3]*rig->basis[(k+1)%3][0];length+=cross[k]*cross[k];}
    if(length<1e-6f)return;length=sqrtf(length);for(int k=0;k<3;++k)cross[k]/=length;
    for(int j=0;j<3;++j)for(int i=0;i<3;++i){axis[j][i]=0;for(int k=0;k<3;++k)axis[j][i]+=(2*cross[j]*cross[k]-(j==k))*old[k][i];}
}
static int KnifeHandPose(const GameRefdef *base,const BridgeTracking *sample,int offhand,float grip[3],float axis[3][3]){
    if(!offhand)return WeaponPose(base,sample,grip,axis);
    const VRWeaponGrip *rig=WeaponGrip(1);int physical=leftHanded?1:0;
    const BridgeHand *hand=&sample->input.hands[physical];
    if(!rig || !sample->valid || !sample->input.focused || GetTickCount()-sample->input.sampleTimeMs>250 || (hand->valid&3)!=3)return 0;
    float controller[3][3],unused[3],aim[3][3];
    PoseInGame(base,&hand->grip,grip,controller);
    PoseInGame(base,&hand->aim,unused,aim);
    for(int j=0;j<3;++j)for(int i=0;i<3;++i){
        axis[j][i]=0;
        for(int k=0;k<3;++k)axis[j][i]+=aim[k][i]*rig->basis[j][k]*(physical==0 && k==1?-1:1);
    }
    return 1;
}
static int KnifeBladePose(const GameRefdef *base,const BridgeTracking *sample,float heel[3],float tip[3]){
    float grip[3],axis[3][3],origin[3];if(!KnifeHandPose(base,sample,offhandKnife,grip,axis))return 0;
    KnifeRenderAxes(axis);
    WeaponEntityOrigin(1,grip,axis,origin);
    for(int i=0;i<3;++i){
        heel[i]=tip[i]=origin[i];
        for(int k=0;k<3;++k){heel[i]+=axis[k][i]*vrKnifeBladeBase[k];tip[i]+=axis[k][i]*vrKnifeBladeTip[k];}
    }
    return 1;
}
static void GamePointToTracking(const GameRefdef *base,const float point[3],float out[3]){
    float local[3]={0},inverse[4];
    for(int i=0;i<3;++i){float d=(point[i]-base->origin[i])/39.37f;local[0]-=d*base->axis[1][i];local[1]+=d*base->axis[2][i];local[2]-=d*base->axis[0][i];}
    for(int i=0;i<3;++i)inverse[i]=-referenceInverse[i];inverse[3]=referenceInverse[3];
    Rotate(inverse,local,out);for(int i=0;i<3;++i)out[i]+=TrackingReference(i);
}
static void TrackingPointToGame(const float point[3],float out[3]){
    BridgePose pose={{0,0,0,1},{0,0,0}};float unused[3][3];memcpy(pose.position,point,12);PoseInGame(&playerBase,&pose,out,unused);
}
static void UpdateKnifeContact(const BridgeTracking *sample,int valid){
    VRBladePose blade,previous;
    if(!valid || !recentered || GetTickCount()-playerBaseTime>250 ||
       !KnifeBladePose(&playerBase,sample,blade.heel,blade.tip)){ResetKnifeMotion();return;}
    if(knifeEpoch!=reloadEpoch){ResetKnifeMotion();knifeEpoch=reloadEpoch;}
    int steps=VR_BladeMotion(&knifeMotion,sample->input.sampleTimeMs,&blade,VR_KNIFE_CONTACT_RADIUS,&previous);
    if(!steps)return;
    float trackedHead[3],head[3];
    for(int i=0;i<3;++i)trackedHead[i]=(sample->views[0].position[i]+sample->views[1].position[i])*.5f;
    TrackingPointToGame(trackedHead,head);
    GameVR_BladeContact(head,&previous,&blade,steps,VR_KNIFE_CONTACT_RADIUS,&knifeContact);
}
/* One model-space bore transform for both the reticle and actual shots.
 * The rendered barrel need not be parallel to the controller's local X axis. */
static int WeaponVisualBorePose(const GameRefdef *base,const BridgeTracking *sample,int weapon,float origin[3],float axis[3][3]){
    float grip[3],render[3][3];if(!WeaponPose(base,sample,grip,render))return 0;
    const VRWeaponMuzzle *bore=NULL;
    for(unsigned i=0;i<sizeof(vrWeaponMuzzles)/sizeof(vrWeaponMuzzles[0]);++i)if(vrWeaponMuzzles[i].id==weapon){bore=&vrWeaponMuzzles[i];break;}
    memcpy(axis,render,sizeof(render));
    if(bore){
        WeaponEntityOrigin(weapon,grip,render,origin);
        for(int i=0;i<3;++i){axis[0][i]=0;for(int k=0;k<3;++k){origin[i]+=render[k][i]*bore->point[k];axis[0][i]+=render[k][i]*bore->direction[k];}}
        float n=0;for(int i=0;i<3;++i)n+=axis[0][i]*axis[0][i];n=sqrtf(n);if(n<.01f)return 0;for(int i=0;i<3;++i)axis[0][i]/=n;
        for(int k=1;k<3;++k){
            for(int j=0;j<k;++j){float dot=0;for(int i=0;i<3;++i)dot+=axis[k][i]*axis[j][i];for(int i=0;i<3;++i)axis[k][i]-=dot*axis[j][i];}
            n=0;for(int i=0;i<3;++i)n+=axis[k][i]*axis[k][i];n=sqrtf(n);if(n<.01f)return 0;for(int i=0;i<3;++i)axis[k][i]/=n;
        }
    }else{
        const VRWeaponProfile *profile=VR_Profile(weapon);float distance=profile && (profile->category==1 || profile->category==8)?8:18;
        for(int i=0;i<3;++i)origin[i]=grip[i]+render[0][i]*distance;
    }
    return 1;
}
static int WeaponBorePose(const GameRefdef *base,const BridgeTracking *sample,int weapon,float origin[3],float axis[3][3]){
    if(!WeaponVisualBorePose(base,sample,weapon,origin,axis))return 0;
    GameVR_ClampMuzzle(base->origin,origin);return 1;
}
static int ControllerShotPose(float origin[3],float axis[3][3]) {
    if(!controlsEnabled)return -1;
    if((reloadWeapon && (magazine.stage || magazine.boltHeld || GameVR_RackPending())) || loadingBlocked || pumpFireBlocked)return 0;
    if(!playerScene || !recentered || GetTickCount()-playerBaseTime>250 || !controlsEnabled || *(unsigned *)(gameBase+0x1476d4) || cvarFloat("cl_paused"))return 0;
    BridgeTracking sample;if(!Bridge_TryLock(&shared->trackingLock))return 0;
    sample=shared->tracking;Bridge_Unlock(&shared->trackingLock);
    /* Keep fixture shots on the same injected hands used by its interaction
     * and eye rendering. Ordinary physical input never enables this branch. */
    if(testReloadActive || testGripAuditActive)memcpy(sample.input.hands,testReloadHands,sizeof(testReloadHands));
    VRWeaponState weapon;if(!GameVR_GetWeaponState(&weapon) || !WeaponBorePose(&playerBase,&sample,weapon.weapon,origin,axis))return 0;
    if(tracePoses)LOG_INFO("SHOTPOSE weapon=%d origin=%.3f,%.3f,%.3f direction=%.6f,%.6f,%.6f",weapon.weapon,origin[0],origin[1],origin[2],axis[0][0],axis[0][1],axis[0][2]);
    if(Bridge_TryLock(&shared->hapticLock)){
        shared->haptic=(BridgeHaptic){++hapticSerial,leftHanded?0u:1u,.35f*accessibility.haptics*.01f,30,0};Bridge_Unlock(&shared->hapticLock);
    }
    return 1;
}
static float Distance3(const float a[3],const float b[3]);
static int ControllerEffectPose(int ejection,float origin[3],float axis[3][3]){
    if(!controlsEnabled || !trackWeapon)return -1;
    if(!shared || shared->helperStatus<2 || !playerScene || !recentered || GetTickCount()-playerBaseTime>250)return 0;
    BridgeTracking sample;if(!Bridge_TryLock(&shared->trackingLock))return 0;
    sample=shared->tracking;Bridge_Unlock(&shared->trackingLock);
    if(testReloadActive || testGripAuditActive)memcpy(sample.input.hands,testReloadHands,sizeof(testReloadHands));
    VRWeaponState state;if(!GameVR_GetWeaponState(&state))return 0;
    if(ejection==2 && state.weapon!=9)ejection=0;
    /* Cosmetic emission remains at the rendered barrel even when the
     * authoritative bullet origin is pulled back by a nearby wall. */
    if(!ejection)return WeaponVisualBorePose(&playerBase,&sample,state.weapon,origin,axis);
    const VRWeaponEjection *locator=NULL;
    if(ejection==2){
        for(unsigned i=0;i<sizeof(vrWeaponAlternateFlashes)/sizeof(vrWeaponAlternateFlashes[0]);++i)if(vrWeaponAlternateFlashes[i].id==state.weapon){locator=&vrWeaponAlternateFlashes[i];break;}
    }else for(unsigned i=0;i<sizeof(vrWeaponEjections)/sizeof(vrWeaponEjections[0]);++i)if(vrWeaponEjections[i].id==state.weapon){locator=&vrWeaponEjections[i];break;}
    if(!locator)return 0;
    float grip[3],render[3][3];if(!WeaponPose(&playerBase,&sample,grip,render))return 0;
    WeaponEntityOrigin(state.weapon,grip,render,origin);
    for(int i=0;i<3;++i){axis[0][i]=0;for(int k=0;k<3;++k){origin[i]+=render[k][i]*locator->point[k];axis[0][i]+=render[k][i]*locator->direction[k];}}
    float n=Distance3(axis[0],(float[3]){0,0,0});if(n<.001f)return 0;
    for(int i=0;i<3;++i)axis[0][i]/=n;
    /* Emitter X follows the real port's outward direction. Preserve local
     * up for the native casing lift, then gravity remains world-relative. */
    float up[3]={0};for(int i=0;i<3;++i)for(int k=0;k<3;++k)up[i]+=render[k][i]*locator->up[k];
    axis[1][0]=up[1]*axis[0][2]-up[2]*axis[0][1];axis[1][1]=up[2]*axis[0][0]-up[0]*axis[0][2];axis[1][2]=up[0]*axis[0][1]-up[1]*axis[0][0];
    n=Distance3(axis[1],(float[3]){0,0,0});if(n<.001f)return 0;for(int i=0;i<3;++i)axis[1][i]/=n;
    axis[2][0]=axis[0][1]*axis[1][2]-axis[0][2]*axis[1][1];axis[2][1]=axis[0][2]*axis[1][0]-axis[0][0]*axis[1][2];axis[2][2]=axis[0][0]*axis[1][1]-axis[0][1]*axis[1][0];
    return 1;
}
static int PrepareNativeHands(void){
    ModelFn model=(ModelFn)(gameBase+0x96d60);
    /* Renderer model handles belong to its current registration, not the
     * proxy DLL lifetime. Quickload/respawn/vid_restart can reuse an old
     * integer for another model. Native registration is a cached name lookup. */
    for(int h=0;h<2;++h)nativeHandModels[h]=model(h?"models/sof2vr_native/rhand.md3":"models/sof2vr_native/lhand.md3");
    for(int h=0;h<2;++h){int step=(int)lroundf(fmaxf(0,fminf(1,tracking.input.hands[h].squeeze))*16);
        char path[100];snprintf(path,sizeof(path),"models/sof2vr_native/%chand_grip%02d.md3",h?'r':'l',step);nativeGripModels[h][step]=model(path);
    }
    VRWeaponState state;
    if(GameVR_GetWeaponState(&state)){
        const VRWeaponGrip *rig=WeaponGrip(state.weapon);
        if(rig && state.weapon>0 && state.weapon<37){
            attachedGripModels[state.weapon][0]=model(rig->rightModel);
            if(rig->supportAuthored)attachedGripModels[state.weapon][1]=model(rig->leftModel);
        }
    }
    static int logged;if(!logged){logged=1;LOG_INFO("NATIVEHANDS source=SoF2-original mesh-and-UV material=models/hands/hands");}
    return nativeHandModels[0] && nativeHandModels[1];
}
static int CloneWeaponSurfaces(BYTE *m);
/* The flat renderer may leave its normally hidden right hand in an unrelated
 * pose. Hide both native instances for the eye passes and draw explicit original
 * grip meshes in the exact same model transform as the weapon. */
static void HideStockArms(const GameEntity *entity){
    unsigned handle=*(const unsigned *)((const BYTE *)entity+204);if(!handle)return;
    unsigned *v=((unsigned *(__cdecl *)(unsigned))(gameBase+0x490a0))(handle);
    if(!v || v[2]<v[1] || (v[2]-v[1])%208 || v[2]-v[1]>16*208)return;
    for(BYTE *m=(BYTE *)(uintptr_t)v[1];m<(BYTE *)(uintptr_t)v[2];m+=208){
        const char *name=(const char *)(m+0x5a);
        if(!strstr(name,"/lhand/") && !strstr(name,"/rhand/"))continue;
        unsigned *flags=(unsigned *)(m+0xa8);int duplicate=0;
        for(unsigned j=0;j<hiddenArmCount;++j)if(hiddenArmFlags[j]==flags)duplicate=1;
        if(!duplicate && hiddenArmCount<32){hiddenArmFlags[hiddenArmCount]=flags;savedArmFlags[hiddenArmCount++]=*flags;*flags|=2;}
    }
}
/* Manual M4 uses authored idle geometry only during interaction. The native
 * Ghoul2 instance and desktop rendering are restored after both eye passes. */
static int PrepareReloadParts(void){
    VRWeaponState state;if(!manualReloadEnabled || !GameVR_GetWeaponState(&state))return 0;
    const VRReloadParts *parts=ReloadParts(state.weapon);if(!parts)return 0;
    ModelFn model=(ModelFn)(gameBase+0x96d60);const char *names[]={"body","magazine","bolt"};
    for(int i=0;i<3;++i){char path[100];snprintf(path,sizeof(path),"%s_%s.md3",parts->prefix,names[i]);reloadModels[i]=model(path);}
    const VRMagazineInteraction *interaction=MagazineInteraction(state.weapon);
    for(int h=0;h<2;++h)reloadHandModels[h]=interaction?model(interaction->handModels[h]):0;
    reloadAssetWeapon=state.weapon;
    return reloadModels[0] && reloadModels[1] && reloadModels[2] && (!interaction || (reloadHandModels[0] && reloadHandModels[1]));
}
static int PrepareLoadingParts(void){
    VRWeaponState state;if(!(manualPump || manualReloadEnabled) || !GameVR_GetWeaponState(&state) || state.weapon!=6)return 0;
    ModelFn model=(ModelFn)(gameBase+0x96d60);
    const char *paths[]={"models/sof2vr_loading/m590_body.md3","models/sof2vr_loading/m590_pump.md3","models/sof2vr_loading/shell.md3"};
    for(int i=0;i<3;++i)loadingModels[i]=model(paths[i]);
    return loadingModels[0] && loadingModels[1] && loadingModels[2];
}
/* Clone authored surface overrides only for the eye passes. Reveal complementary
 * side meshes without resetting moving-part visibility or changing native instances. */
static int CloneWeaponSurfaces(BYTE *m){
    unsigned *v=(unsigned *)(m+24),begin=v[0],end=v[1];
    for(unsigned j=0;j<reloadSurfaceCount;++j)if(reloadSurfaceBackup[j].vector==v)return (int)j;
    if(end<begin || (end-begin)%24 || end-begin>64*24 || reloadSurfaceCount>=32)return -1;
    unsigned index=reloadSurfaceCount++;reloadSurfaceBackup[index].vector=v;memcpy(reloadSurfaceBackup[index].saved,v,12);
    if(end>begin)memcpy(reloadSurfaceBackup[index].records,(void *)(uintptr_t)begin,end-begin);
    v[0]=(unsigned)(uintptr_t)reloadSurfaceBackup[index].records;v[1]=v[2]=v[0]+end-begin;return (int)index;
}
static void CompleteWeaponSides(const GameEntity *entity){
    unsigned handle=*(const unsigned *)((const BYTE *)entity+204);if(!handle)return;
    unsigned *v=((unsigned *(__cdecl *)(unsigned))(gameBase+0x490a0))(handle);
    if(!v || v[2]<v[1] || (v[2]-v[1])%208 || v[2]-v[1]>16*208)return;
    for(BYTE *m=(BYTE *)(uintptr_t)v[1];m<(BYTE *)(uintptr_t)v[2];m+=208){
        const VRWeaponSides *sides=NULL;
        for(unsigned i=0;i<sizeof(vrWeaponSides)/sizeof(vrWeaponSides[0]);++i)if(!_stricmp((char *)m+0x5a,vrWeaponSides[i].path)){sides=&vrWeaponSides[i];break;}
        if(!sides)continue;int index=CloneWeaponSurfaces(m);if(index<0)continue;
        unsigned *vector=(unsigned *)(m+24),n=(vector[1]-vector[0])/24;
        for(int k=0;k<sides->count;++k){unsigned side=sides->surfaces[k],j=0;
            for(;j<n;++j)if(reloadSurfaceBackup[index].records[j][1]==side)break;
            if(j==n){if(n>=128)break;memset(reloadSurfaceBackup[index].records[n],0,24);reloadSurfaceBackup[index].records[n++][1]=side;}
            unsigned hidden=sides->defaults[k];int base=sides->bases[k];
            if(base>=0)for(unsigned b=0;b<n;++b)if(reloadSurfaceBackup[index].records[b][1]==(unsigned)base)hidden=reloadSurfaceBackup[index].records[b][0]&2u;
            reloadSurfaceBackup[index].records[j][0]=(reloadSurfaceBackup[index].records[j][0]&~(2u|0x100u))|hidden;
        }
        vector[1]=vector[2]=vector[0]+n*24;
        static unsigned logged;if(tracePoses && logged<24){LOG_INFO("WEAPONSIDES path=%s restored=%d",sides->path,sides->count);++logged;}
    }
}
static void HideWeaponModel(const GameEntity *entity,const char *model){
    unsigned handle=*(const unsigned *)((const BYTE *)entity+204);if(!handle)return;
    unsigned *v=((unsigned *(__cdecl *)(unsigned))(gameBase+0x490a0))(handle);
    if(!v || v[2]<v[1] || (v[2]-v[1])%208 || v[2]-v[1]>16*208)return;
    for(BYTE *m=(BYTE *)(uintptr_t)v[1];m<(BYTE *)(uintptr_t)v[2];m+=208){
        if(_stricmp((char *)m+0x5a,model))continue;
        unsigned *flags=(unsigned *)(m+0xa8);int duplicate=0;
        for(unsigned j=0;j<hiddenArmCount;++j)if(hiddenArmFlags[j]==flags)duplicate=1;
        if(!duplicate && hiddenArmCount<32){hiddenArmFlags[hiddenArmCount]=flags;savedArmFlags[hiddenArmCount++]=*flags;*flags|=2;}
    }
}
static void HideReloadWeapon(const GameEntity *entity){
    const VRReloadParts *parts=ReloadParts(reloadAssetWeapon);if(parts)HideWeaponModel(entity,parts->model);
}
/* A yaw-only torso approximation. Looking down must not swing the pouch or
 * shoulder slots up into the face. Positive body z points behind the player. */
static void BodyFrame(const BridgeTracking *sample,float centre[3],float q[4],float inverse[4]){
    float forward[3],ahead[3]={0,0,-1};Rotate(sample->views[0].orientation,ahead,forward);
    static float heading;if(hypotf(forward[0],forward[2])>.2f)heading=atan2f(-forward[0],-forward[2]);
    q[0]=q[2]=0;q[1]=sinf(heading*.5f);q[3]=cosf(heading*.5f);
    inverse[0]=inverse[2]=0;inverse[1]=-q[1];inverse[3]=q[3];
    for(int i=0;i<3;++i)centre[i]=(sample->views[0].position[i]+sample->views[1].position[i])*.5f;
}
static void BodySlotInGame(const GameRefdef *base,const BridgeTracking *sample,int slot,float point[3]){
    float centre[3],q[4],inv[4],local[3],delta[3],unused[3][3];BridgePose p={0};
    BodyFrame(sample,centre,q,inv);VR_HolsterPositionAccessible(slot,leftHanded,accessibility.holsterReach*.01f,accessibility.holsterHeight*.01f,accessibility.seated,local);Rotate(q,local,delta);
    for(int i=0;i<3;++i)p.position[i]=centre[i]+delta[i];memcpy(p.orientation,q,16);PoseInGame(base,&p,point,unused);
}
static int ReloadGeometry(const GameRefdef *base,const BridgeTracking *sample,float origin[3],float axis[3][3],float socket[3],float bolt[3],float pouch[3]){
    VRWeaponState state;if(!GameVR_GetWeaponState(&state))return 0;
    const VRReloadParts *parts=ReloadParts(state.weapon);if(!parts)return 0;
    float grip[3];if(!WeaponPose(base,sample,grip,axis))return 0;
    WeaponEntityOrigin(state.weapon,grip,axis,origin);
    for(int i=0;i<3;++i){socket[i]=origin[i];
        for(int k=0;k<3;++k)socket[i]+=axis[k][i]*parts->socket[k];
        bolt[i]=origin[i];for(int k=0;k<3;++k)bolt[i]+=axis[k][i]*parts->bolt[k];
    }
    BodySlotInGame(base,sample,VR_HOLSTER_AMMO,pouch);
    return 1;
}
static float ReloadActionTravel(int weapon){return 39.37f*VR_ActionStroke(weapon);}
static float Distance3(const float a[3],const float b[3]){float d=0;for(int i=0;i<3;++i)d+=(a[i]-b[i])*(a[i]-b[i]);return sqrtf(d);}
static int MagazineInHand(const GameRefdef *base,const BridgeHand *hand,float origin[3],float axis[3][3]){
    if(!(hand->valid&BRIDGE_HAND_GRIP_VALID))return 0;
    float point[3],handAxis[3][3];PoseInGame(base,&hand->grip,point,handAxis);
    const VRMagazineInteraction *interaction=MagazineInteraction(reloadWeapon);int side=leftHanded?1:0;
    const float *offset=interaction?interaction->holdOrigin[side]:reloadOffset;
    const float (*basis)[3]=interaction?interaction->holdBasis[side]:reloadBasis;
    for(int i=0;i<3;++i){
        origin[i]=point[i];for(int k=0;k<3;++k)origin[i]+=handAxis[k][i]*offset[k];
        for(int j=0;j<3;++j){axis[j][i]=0;for(int k=0;k<3;++k)axis[j][i]+=handAxis[k][i]*basis[j][k];}
    }
    return 1;
}
static void PouchMagazineAxes(const GameRefdef *base,int weapon,float axis[3][3]){
    BridgePose torso={0};float inverse[4],origin[3],body[3][3];
    BodyFrame(&tracking,torso.position,torso.orientation,inverse);
    PoseInGame(base,&torso,origin,body);
    const VRMagazineInteraction *interaction=MagazineInteraction(weapon);
    if(!interaction){memcpy(axis,body,sizeof(body));return;}
    const float *up=interaction->up,*front=interaction->front;
    for(int j=0;j<3;++j){
        float side=up[(j+1)%3]*front[(j+2)%3]-up[(j+2)%3]*front[(j+1)%3];
        if(leftHanded)side=-side;
        for(int k=0;k<3;++k)axis[j][k]=front[j]*body[0][k]+side*body[1][k]+up[j]*body[2][k];
    }
}
static int SpawnDroppedMagazine(const float origin[3],const float axis[3][3]){
    if(!reloadModels[1] || reloadWeapon!=reloadAssetWeapon)return -1;
    int slot=-1;unsigned age=0,time=(unsigned)playerBase.time;
    for(int i=0;i<MAX_DROPPED_MAGAZINES;++i){
        if(!droppedMagazines[i].active){slot=i;break;}
        if(slot<0 || time-droppedMagazines[i].born>age){slot=i;age=time-droppedMagazines[i].born;}
    }
    memset(&droppedMagazines[slot],0,sizeof(droppedMagazines[slot]));
    droppedMagazines[slot].active=1;droppedMagazines[slot].model=reloadModels[1];droppedMagazines[slot].weapon=reloadWeapon;
    droppedMagazines[slot].reflected=leftHanded;droppedMagazines[slot].born=droppedMagazines[slot].last=time;droppedMagazines[slot].epoch=reloadEpoch;
    memcpy(droppedMagazines[slot].origin,origin,12);memcpy(droppedMagazines[slot].axis,axis,36);
    droppedMagazines[slot].velocity[2]=-12;droppedMagazines[slot].spin=5.0f+(slot%3);memcpy(droppedMagazines[slot].spinAxis,axis[0],12);
    for(unsigned i=0;i<sizeof(vrMagazineBounds)/sizeof(vrMagazineBounds[0]);++i)if(vrMagazineBounds[i].id==reloadWeapon)memcpy(droppedMagazines[slot].half,vrMagazineBounds[i].half,12);
    LOG_INFO("MAGDROP spawned weapon=%d slot=%d position=%.3f,%.3f,%.3f",reloadWeapon,slot,origin[0],origin[1],origin[2]);
    return slot;
}
static void SpawnRackedCartridge(const float axis[3][3]){
    float point[3],portBasis[3][3];int port=ControllerEffectPose(1,point,portBasis)>0;
    int slot=SpawnDroppedMagazine(port?point:reloadBolt,port?(const float (*)[3])portBasis:axis);if(slot<0)return;
    const VRWeaponProfile *p=VR_Profile(reloadWeapon);ModelFn model=(ModelFn)(gameBase+0x96d60);
    droppedMagazines[slot].model=model(p && p->category==2?"models/weapons/shells/shell_brass_small.md3":"models/weapons/shells/shell_brass.md3");
    droppedMagazines[slot].half[0]=.6f;droppedMagazines[slot].half[1]=droppedMagazines[slot].half[2]=.2f;
    for(int i=0;i<3;++i)droppedMagazines[slot].velocity[i]=port?portBasis[0][i]*45+portBasis[2][i]*30:-axis[1][i]*45+axis[2][i]*30;
    LOG_INFO("RACK discarded cartridge prop weapon=%d slot=%d",reloadWeapon,slot);
}
static void UpdateDroppedMagazines(const GameRefdef *base){
    unsigned now=(unsigned)base->time;int paused=cvarFloat("cl_paused")!=0;
    float gravity=cvarFloat("g_gravity");if(!isfinite(gravity) || gravity<0 || gravity>4000)gravity=800;
    for(int i=0;i<MAX_DROPPED_MAGAZINES;++i){
        if(!droppedMagazines[i].active)continue;
        if(droppedMagazines[i].epoch!=reloadEpoch || now-droppedMagazines[i].born>12000){droppedMagazines[i].active=0;continue;}
        float elapsed=fminf(.08f,(now-droppedMagazines[i].last)*.001f);droppedMagazines[i].last=now;
        if(paused || droppedMagazines[i].settled)continue;
        while(elapsed>0){float dt=fminf(elapsed,1/90.0f),next[3],half[3]={0},stop[3],axis[3][3];elapsed-=dt;
            float angle=droppedMagazines[i].spin*dt*.5f,q[4];for(int k=0;k<3;++k)q[k]=droppedMagazines[i].spinAxis[k]*sinf(angle);q[3]=cosf(angle);
            for(int k=0;k<3;++k)Rotate(q,droppedMagazines[i].axis[k],axis[k]);
            for(int k=0;k<3;++k){
                for(int j=0;j<3;++j)half[k]+=fabsf(axis[j][k])*droppedMagazines[i].half[j];
                float acceleration=k==2?-gravity:0;
                next[k]=droppedMagazines[i].origin[k]+droppedMagazines[i].velocity[k]*dt+.5f*acceleration*dt*dt;
                droppedMagazines[i].velocity[k]+=acceleration*dt;
            }
            int hit=GameVR_TraceMagazine(droppedMagazines[i].origin,half,next,stop);
            if(hit<0)break;
            memcpy(droppedMagazines[i].origin,stop,12);memcpy(droppedMagazines[i].axis,axis,36);
            if(hit){droppedMagazines[i].settled=1;break;}
        }
    }
}
static void AddDroppedMagazines(void){
    EntityFn add=(EntityFn)(gameBase+0x999f0);
    for(int i=0;i<MAX_DROPPED_MAGAZINES;++i)if(droppedMagazines[i].active){
        GameEntity entity={0};entity.model=droppedMagazines[i].model;entity.nonNormalized=droppedMagazines[i].reflected;
        memcpy(entity.origin,droppedMagazines[i].origin,12);memcpy(entity.axis,droppedMagazines[i].axis,36);memset(entity.shaderRGBA,255,4);add(&entity);
    }
}
static void ReloadHaptic(float amplitude,int duration){if(shared && Bridge_TryLock(&shared->hapticLock)){shared->haptic=(BridgeHaptic){++hapticSerial,leftHanded?1u:0u,amplitude*accessibility.haptics*.01f,(uint32_t)duration,0};Bridge_Unlock(&shared->hapticLock);}}
#include "vr_dropped_weapons.h"
#include "vr_stowed_weapons.h"
static int LoadingGeometry(const GameRefdef *base,const BridgeTracking *sample,float origin[3],float axis[3][3],float port[3],float foreend[3],float chest[3]){
    float grip[3];if(!WeaponPose(base,sample,grip,axis))return 0;WeaponEntityOrigin(6,grip,axis,origin);
    for(int i=0;i<3;++i){port[i]=foreend[i]=origin[i];for(int k=0;k<3;++k){port[i]+=axis[k][i]*m590LoadPort[k];foreend[i]+=axis[k][i]*m590PumpSocket[k];}}
    BodySlotInGame(base,sample,VR_HOLSTER_AMMO,chest);return 1;
}
static void AddLoadingParts(const GameRefdef *base){
    if(!loadingLive || (!manualPump && !shellLoad.stage) || !tracking.input.focused)return;
    float origin[3],axis[3][3],port[3],foreend[3],chest[3];if(!LoadingGeometry(base,&tracking,origin,axis,port,foreend,chest))return;
    EntityFn add=(EntityFn)(gameBase+0x999f0);GameEntity e={0};e.flags=4|0x80|0x40;e.nonNormalized=leftHanded?1:0;memcpy(e.lightingOrigin,base->origin,12);memset(e.shaderRGBA,255,4);
    memcpy(e.origin,origin,12);memcpy(e.axis,axis,sizeof(axis));e.model=loadingModels[0];add(&e);
    for(int i=0;i<3;++i)e.origin[i]=origin[i]-axis[0][i]*pump.pull*M590_PUMP_TRAVEL;
    e.model=loadingModels[1];add(&e);
    if(shellLoad.stage==1){
        const BridgeHand *hand=&tracking.input.hands[leftHanded?1:0];if((hand->valid&3)!=3)return;
        PoseInGame(base,&hand->grip,e.origin,e.axis);e.nonNormalized=0;e.model=loadingModels[2];add(&e);
    }
}
static void AddReloadParts(const GameRefdef *base){
    if(!reloadRenderReady)return;
    const float *origin=reloadRender.origin,*socket=reloadRender.socket,*pouch=reloadRender.pouch;
    const float (*axis)[3]=reloadRender.axis;
    EntityFn add=(EntityFn)(gameBase+0x999f0);GameEntity e={0};e.flags=4|0x80|0x40;e.nonNormalized=leftHanded?1:0;memcpy(e.lightingOrigin,base->origin,12);memset(e.shaderRGBA,255,4);memcpy(e.axis,axis,sizeof(e.axis));memcpy(e.origin,origin,12);
    { /* Split the complete body even when a clip shares a stock surface. */
        e.model=reloadModels[0];add(&e);e.model=reloadModels[2];
        /* The M3A1 is cocked directly by its bolt. The normal Uzi's top
         * charging knob returns forward independently of the open bolt. */
        float actionPull=(pistolSlideLocked || (reloadWeapon==8 && openBoltReady))?1.0f:magazine.boltPull;
        for(int i=0;i<3;++i)e.origin[i]=origin[i]-axis[0][i]*actionPull*ReloadActionTravel(reloadWeapon);add(&e);
    }
    int held=magazine.stage==VR_MAG_EXTRACT || magazine.stage==VR_MAG_OLD || magazine.stage==VR_MAG_FRESH;
    int seated=magazine.stage==VR_MAG_SEATED || magazine.stage==VR_MAG_BOLT || magazine.stage==VR_MAG_PENDING;
    int heldVisible=held && MagazineInHand(base,&tracking.input.hands[leftHanded?1:0],e.origin,e.axis);
    if(!held){
        memcpy(e.origin,socket,12);memcpy(e.axis,axis,sizeof(e.axis));
        if(seated && MagazineInteraction(reloadWeapon) && magazineGuide.seatStarted){
            float t=(GetTickCount()-magazineGuide.seatStarted)/140.0f;
            if(t<1){t=t*t*(3-2*t);
                for(int i=0;i<3;++i)e.origin[i]=magazineGuide.seatOrigin[i]+t*(socket[i]-magazineGuide.seatOrigin[i]);
                for(int j=0;j<3;++j){float length=0;for(int i=0;i<3;++i){e.axis[j][i]=magazineGuide.seatAxis[j][i]+t*(axis[j][i]-magazineGuide.seatAxis[j][i]);length+=e.axis[j][i]*e.axis[j][i];}
                    if(length>.001f)for(int i=0;i<3;++i)e.axis[j][i]/=sqrtf(length);
                }
                /* Keep the magazine rigid while aligning to the well. */
                float dot=0,length=0;for(int i=0;i<3;++i)dot+=e.axis[0][i]*e.axis[1][i];
                for(int i=0;i<3;++i){e.axis[1][i]-=dot*e.axis[0][i];length+=e.axis[1][i]*e.axis[1][i];}
                if(length>.001f)for(int i=0;i<3;++i)e.axis[1][i]/=sqrtf(length);
                float sign=leftHanded?-1:1;
                for(int i=0;i<3;++i)e.axis[2][i]=sign*(e.axis[0][(i+1)%3]*e.axis[1][(i+2)%3]-e.axis[0][(i+2)%3]*e.axis[1][(i+1)%3]);
            }
        }
    }
    if(heldVisible || seated){e.model=reloadModels[1];add(&e);}
    /* One visible spare at the waist. It represents reserve ammunition, not
     * a spawned inventory item; acquiring/dropping it changes no native ammo. */
    if(reloadReserve>0 || (reloadClip>0 && magazine.stage==VR_MAG_EMPTY)){
        e.model=reloadModels[1];memcpy(e.origin,pouch,12);PouchMagazineAxes(base,reloadWeapon,e.axis);add(&e);
    }
}
static int UpdateMagazine(const BridgeTracking *sample,int valid,int fallback){
    VRWeaponState state;int current=GameVR_GetWeaponState(&state);
    static int observedWeapon;int eject=reloadEjectRequest,slide=reloadSlideRequest;reloadEjectRequest=reloadSlideRequest=0;
    if(current)observedWeapon=ReloadParts(state.weapon)?state.weapon:0;
    int supported=manualReloadEnabled && current && ReloadParts(state.weapon) && reloadLive && state.weapon==reloadAssetWeapon;
    static char mapBefore[64];static unsigned seenEpoch;int changedEpoch=seenEpoch!=reloadEpoch;seenEpoch=reloadEpoch;const char *map=((const char *(__cdecl *)(const char *))(gameBase+0x36730))("mapname");
    int changedMap=_stricmp(mapBefore,map)!=0;if(changedMap){strncpy(mapBefore,map,sizeof(mapBefore)-1);mapBefore[sizeof(mapBefore)-1]=0;}
    if(changedMap || changedEpoch)memset(droppedMagazines,0,sizeof(droppedMagazines));
    if(!supported || reloadWeapon!=state.weapon || changedMap || changedEpoch || fallback || (magazine.stage && GetTickCount()-reloadLastValid>3000)){
        if(magazine.stage)LOG_INFO("MANUAL reset stage=%d reason=context",magazine.stage);if(reloadWeapon || magazine.stage)GameVR_CancelRoundInsertion();
        memset(&magazine,0,sizeof(magazine));memset(&magazineGuide,0,sizeof(magazineGuide));pistolSlideLocked=0;openBoltReady=supported && VR_OpenBolt(state.weapon) && state.clip>0;reloadHeldOld=0;reloadWeapon=supported?state.weapon:0;
    }
    /* Keep automatic M4 reload disabled while the headset is asleep or the
     * cached native sample expires during pause. Other weapons are filtered
     * by the adapter's actual inventory ID check. */
    if(!(manualReloadEnabled && controlsEnabled && ((current && state.weapon==6) || shellLoad.weapon==6)))
        GameVR_SetReloadControl(manualReloadEnabled && controlsEnabled?observedWeapon:0,magazine.stage!=0,supported?fallback:-1);
    if(!supported)return 0;
    static int actionWeapon,actionClip;static unsigned actionEpoch;
    if(actionWeapon!=state.weapon || actionEpoch!=reloadEpoch){actionWeapon=state.weapon;actionEpoch=reloadEpoch;actionClip=state.clip;openBoltReady=VR_OpenBolt(state.weapon) && state.clip>0;}
    if(VR_OpenBolt(state.weapon) && state.clip<actionClip)openBoltReady=state.clip>0;
    actionClip=state.clip;
    if(magazine.stage==VR_MAG_PENDING){
        int result=GameVR_ReloadResult();
        if(result){LOG_INFO("MANUAL committed=%d weapon=%d",result,state.weapon);magazine.stage=result>0?VR_MAG_SEATED:VR_MAG_EMPTY;if(result>0)pistolSlideLocked=0;ReloadHaptic(result>0?.7f:.2f,result>0?60:110);}
        else return 1;
    }
    if(magazine.stage && (state.clip!=reloadClip || state.reserve!=reloadReserve)){
        LOG_INFO("MANUAL reset stage=%d reason=native-ammo-changed",magazine.stage);memset(&magazine,0,sizeof(magazine));memset(&magazineGuide,0,sizeof(magazineGuide));
    }
    if(!magazine.stage){reloadClip=state.clip;reloadReserve=state.reserve;reloadCapacity=state.capacity;}
    const VRWeaponProfile *slideProfile=VR_Profile(state.weapon);
    if(slideProfile && slideProfile->category==2 && !magazine.stage){
        int locked=state.clip==0;
        if(locked!=pistolSlideLocked)LOG_INFO("MANUAL pistol slide weapon=%d locked=%d clip=%d",state.weapon,locked,state.clip);
        pistolSlideLocked=locked;
    }
    if(eject && valid && magazine.stage==VR_MAG_SEATED){
        float body[3],basis[3][3],socket[3],bolt[3],pouch[3];
        if(ReloadGeometry(&playerBase,sample,body,basis,socket,bolt,pouch) && VR_MagazineEject(&magazine,1)){
            SpawnDroppedMagazine(socket,basis);reloadHeldOld=0;ReloadHaptic(.45f,35);
            LOG_INFO("MANUAL eject weapon=%d button=A clip=%d reserve=%d",state.weapon,state.clip,state.reserve);
        }
    }
    const BridgeHand *hand=&sample->input.hands[leftHanded?1:0];float origin[3]={0},axis[3][3]={{0}},point[3],handAxis[3][3];
    int tracked=(hand->valid&3)==3 && ReloadGeometry(&playerBase,sample,origin,axis,reloadSocket,reloadBolt,reloadPouch);
    if(tracked)PoseInGame(&playerBase,&hand->grip,point,handAxis);else{memset(point,0,12);memset(handAxis,0,sizeof(handAxis));}
    int playable=valid && tracked && (state.state==0 || state.state==27);
    int rackResult=GameVR_TakeRackResult();
    if(rackResult>0 && tracked)SpawnRackedCartridge(axis);
    if(playable)reloadLastValid=GetTickCount();
    float along=0;for(int i=0;i<3;++i)along+=(point[i]-origin[i])*axis[0][i];
    float alignment=0;for(int i=0;i<3;++i)for(int k=0;k<3;++k){float a=0;for(int j=0;j<3;++j)a+=handAxis[j][i]*reloadBasis[k][j];alignment+=a*axis[k][i];}
    VRMagazineInput in={0};in.valid=playable;
    /* Grip handles ammunition; trigger works the slide/charging action. */
    in.grip=hand->squeeze>(magazine.gripBefore?GripOff():GripOn());
    in.action=hand->trigger>(magazine.actionBefore?.25f:.55f);
    in.actionTravel=ReloadActionTravel(state.weapon)/39.37f;
    float magazineCentre[3];memcpy(magazineCentre,point,12);
    if(magazine.stage==VR_MAG_FRESH || magazine.stage==VR_MAG_OLD || magazine.stage==VR_MAG_EXTRACT)
        for(int i=0;i<3;++i)for(int k=0;k<3;++k)magazineCentre[i]+=handAxis[k][i]*reloadOffset[k];
    float upDot=0;for(int i=0;i<3;++i){float up=0;for(int j=0;j<3;++j)up+=handAxis[j][i]*reloadBasis[2][j];upDot+=up*axis[2][i];}
    if(magazine.stage==VR_MAG_FRESH || magazine.stage==VR_MAG_OLD)alignment=upDot>.45f?3:0;
    const VRWeaponProfile *reloadProfile=VR_Profile(state.weapon);
    in.nearSocket=tracked && Distance3(magazineCentre,reloadSocket)<(magazine.stage==VR_MAG_FRESH?4.25f:3.55f);
    in.nearPouch=tracked && Distance3(point,reloadPouch)<(reloadProfile && reloadProfile->category==2?17.0f:14.0f);
    static int pouchWasNear;
    if(playable && magazine.stage==VR_MAG_EMPTY && in.grip && in.nearPouch && !pouchWasNear){
        magazine.armed=1;magazine.gripBefore=0;
    }
    pouchWasNear=playable && in.nearPouch;
    if(magazine.stage==VR_MAG_SEATED)in.nearSocket=0; /* Only A ejects; squeezing a seated magazine never removes it. */
    in.nearBolt=tracked && Distance3(point,reloadBolt)<5.5f;in.aligned=alignment>2.1f;
    if(state.weapon==8 && openBoltReady)in.nearBolt=0; /* Direct bolt already on the sear. */
    /* Seated acquisition uses the current hand pose as the initial basis. */
    if(magazine.stage==VR_MAG_SEATED)in.aligned=1;
    const VRMagazineInteraction *interaction=MagazineInteraction(state.weapon);
    float heldOrigin[3]={0},heldAxis[3][3]={{0}};
    if(interaction){
        int holding=magazine.stage==VR_MAG_FRESH || magazine.stage==VR_MAG_OLD || magazine.stage==VR_MAG_EXTRACT;
        int hasPose=tracked && holding && MagazineInHand(&playerBase,hand,heldOrigin,heldAxis);
        in.nearSocket=0;in.aligned=0;
        if(hasPose){
            float tip[3],mouth[3],up[3]={0},heldUp[3]={0},front[3]={0},heldFront[3]={0},delta[3],depth=0,radial=0,upDot=0,frontDot=0;
            for(int i=0;i<3;++i){
                tip[i]=heldOrigin[i];mouth[i]=reloadSocket[i];
                for(int k=0;k<3;++k){
                    tip[i]+=heldAxis[k][i]*interaction->feed[k];mouth[i]+=axis[k][i]*interaction->mouth[k];
                    up[i]+=axis[k][i]*interaction->up[k];heldUp[i]+=heldAxis[k][i]*interaction->up[k];
                    front[i]+=axis[k][i]*interaction->front[k];heldFront[i]+=heldAxis[k][i]*interaction->front[k];
                }
                delta[i]=tip[i]-mouth[i];depth+=delta[i]*up[i];upDot+=up[i]*heldUp[i];frontDot+=front[i]*heldFront[i];
            }
            for(int i=0;i<3;++i){float d=delta[i]-depth*up[i];radial+=d*d;}radial=sqrtf(radial);
            int aligned=upDot>.55f && frontDot>-.1f;
            if(!playable || !aligned || radial>interaction->radial*2 || depth< -3.5f)magazineGuide.active=0;
            /* The feed end acquires a generous magnetic mouth. Once it
             * reaches the entrance, seat the remaining travel cosmetically;
             * do not require a narrow 100-ms hold deep inside the receiver. */
            int crossed=magazineGuide.lastSample && sample->input.sampleTimeMs-magazineGuide.lastSample<100 &&
                magazineGuide.depth<0 && depth>=0 && depth-magazineGuide.depth<6 && magazineGuide.radial<interaction->radial*1.5f && depth<interaction->seatDepth+3;
            if(!magazineGuide.active && playable && magazine.stage==VR_MAG_FRESH && magazine.away && aligned &&
               radial<interaction->radial && ((depth>=-2.4f && depth<=interaction->seatDepth+2) || crossed)){
                magazineGuide.active=1;LOG_INFO("MAGWELL entered weapon=%d feed-end at opening",state.weapon);
            }
            magazineGuide.depth=depth;magazineGuide.radial=radial;magazineGuide.lastSample=sample->input.sampleTimeMs;
            in.nearSocket=magazineGuide.active && aligned && radial<interaction->radial*1.35f && depth>=-.3f && depth<interaction->seatDepth+4;
            in.aligned=aligned;
        }else{magazineGuide.active=0;magazineGuide.lastSample=0;}
    }
    in.spare=state.reserve>0 || reloadClip>0;in.empty=reloadClip==0;float relative[3]={0};for(int k=0;k<3;++k)for(int i=0;i<3;++i)relative[k]+=(point[i]-reloadSocket[i])*axis[k][i];
    in.extraction=-(relative[2]-reloadStartRelative[2])/39.37f;if(hypotf(relative[0]-reloadStartRelative[0],relative[1]-reloadStartRelative[1])>6.3f)in.extraction=0;in.along=along/39.37f;
    static DWORD seatingSince;
    if(magazine.stage==VR_MAG_FRESH && playable && magazine.away && in.grip && in.nearSocket && in.aligned){
        if(!seatingSince)seatingSince=GetTickCount();
        if(GetTickCount()-seatingSince>=45)in.grip=0;
    }else seatingSince=0;
    int before=magazine.stage,heldBolt=magazine.boltHeld,event=VR_MagazineUpdate(&magazine,&in);
    if(VR_OpenBolt(state.weapon) && (event==VR_MAG_RACK || event==VR_MAG_CHARGE)){
        openBoltReady=1;magazine.boltHeld=magazine.boltBack=0;magazine.boltPull=0;
        LOG_INFO("MANUAL open bolt caught weapon=%d chambered-round-ejected=0",state.weapon);
    }
    if(VR_OpenBolt(state.weapon) && openBoltReady && event==VR_MAG_INSERT && magazine.stage==VR_MAG_BOLT)magazine.stage=VR_MAG_PENDING;
    if(event==VR_MAG_RACK){
        if(!VR_OpenBolt(state.weapon) && state.clip>0 && GameVR_RackRound(state.weapon,state.clip,state.reserve))LOG_INFO("RACK cycle requested weapon=%d clip=%d",state.weapon,state.clip);
        ReloadHaptic(.4f,30);
    }
    if(event==VR_MAG_INSERT && interaction){
        magazineGuide.seatStarted=GetTickCount();memcpy(magazineGuide.seatOrigin,heldOrigin,12);memcpy(magazineGuide.seatAxis,heldAxis,36);magazineGuide.active=0;
        LOG_INFO("MAGWELL seated weapon=%d insertion-cm=%.2f lateral-cm=%.2f",state.weapon,magazineGuide.depth*2.54f,magazineGuide.radial*2.54f);
    }
    if(event==VR_MAG_DROP){float droppedOrigin[3],droppedAxis[3][3];
        if(MagazineInHand(&playerBase,hand,droppedOrigin,droppedAxis))SpawnDroppedMagazine(droppedOrigin,droppedAxis);
    }
    if(slide && valid && (state.state==0 || state.state==27) && reloadProfile && reloadProfile->category==2 && magazine.stage==VR_MAG_BOLT){
        magazine.stage=VR_MAG_PENDING;magazine.boltHeld=magazine.boltBack=0;magazine.boltPull=0;event=VR_MAG_CHARGE;
        LOG_INFO("MANUAL slide release weapon=%d button=B offhandTracked=%d",state.weapon,tracked);
    }
    if(!tracked && (before!=magazine.stage || heldBolt))LOG_INFO("MANUAL tracking lost stage=%d boltHeld=%d",before,heldBolt);
    if(event==VR_MAG_SPARE)reloadHeldOld=reloadReserve==0 && reloadClip>0;
    if(event==VR_MAG_INSERT && (reloadHeldOld || reloadClip==reloadCapacity)){magazine.stage=VR_MAG_SEATED;LOG_INFO("MANUAL reseated weapon=%d clip=%d reserve=%d transfer=0",state.weapon,state.clip,state.reserve);}
    if(!interaction && ((event==VR_MAG_GRAB && before==VR_MAG_SEATED) || event==VR_MAG_SPARE)){
        memcpy(reloadStartHand,point,12);memcpy(reloadStartRelative,relative,12);
        for(int k=0;k<3;++k){reloadOffset[k]=0;for(int i=0;i<3;++i)reloadOffset[k]+=(event==VR_MAG_SPARE?0:reloadSocket[i]-point[i])*handAxis[k][i];
            for(int j=0;j<3;++j){reloadBasis[j][k]=0;for(int i=0;i<3;++i)reloadBasis[j][k]+=axis[j][i]*handAxis[k][i];}}
    }
    if(event){LOG_INFO("MANUAL gesture=%d stage=%d clip=%d reserve=%d socketDistance=%.3f pouchDistance=%.3f",event,magazine.stage,state.clip,state.reserve,Distance3(point,reloadSocket)/39.37f,Distance3(point,reloadPouch)/39.37f);ReloadHaptic(event==VR_MAG_INSERT?.65f:.32f,event==VR_MAG_INSERT?45:25);}
    if(magazine.stage==VR_MAG_PENDING && before!=VR_MAG_PENDING){
        int rounds=reloadCapacity-reloadClip;if(rounds>reloadReserve)rounds=reloadReserve;
        if(!GameVR_InsertRounds(reloadWeapon,reloadClip,reloadReserve,rounds)){magazine.stage=VR_MAG_EMPTY;LOG_WARN("MANUAL insertion rejected");}
    }
    GameVR_SetReloadControl(state.weapon,magazine.stage!=0 || magazine.boltHeld || GameVR_RackPending(),fallback);
    return magazine.stage!=VR_MAG_SEATED || magazine.boltHeld || GameVR_RackPending();
}
static void RestoreStockArms(void){for(unsigned j=0;j<reloadSurfaceCount;++j)memcpy(reloadSurfaceBackup[j].vector,reloadSurfaceBackup[j].saved,12);reloadSurfaceCount=0;for(unsigned j=0;j<hiddenArmCount;++j)*hiddenArmFlags[j]=savedArmFlags[j];hiddenArmCount=0;}
static int BodyOutfit(void){
    int outfit=bodyOutfitOverride;
    if(outfit<0){outfit=0;const char *map=((const char *(__cdecl *)(const char *))(gameBase+0x36730))("mapname");
        if(map)for(unsigned i=0;i<sizeof(vrOutfitMaps)/sizeof(vrOutfitMaps[0]);++i)if(!_stricmp(map,vrOutfitMaps[i].map)){outfit=vrOutfitMaps[i].outfit;break;}}
    return outfit>=0 && outfit<7?outfit:0;
}
static void AttachForearm(const GameEntity *hand,const char *source,int physical){
    if(!bodyIKEnabled || !source)return;
    for(unsigned j=0;j<sizeof(vrNativeWrists)/sizeof(vrNativeWrists[0]);++j){const VRNativeWrist *w=&vrNativeWrists[j];
        if(strcmp(w->model,source))continue;
        for(int i=0;i<3;++i){bodyWrists[physical][i]=hand->origin[i];bodyWristWidths[physical][i]=0;
            for(int k=0;k<3;++k){bodyWrists[physical][i]+=hand->axis[k][i]*w->point[k];bodyWristWidths[physical][i]+=hand->axis[k][i]*w->width[k];}}
        bodyWristMask|=1u<<physical;
        return;
    }
}
static void AddAttachedHand(const GameRefdef *base,int weapon,int side,int physical,int reflected,
                            int model,const float origin[3],const float axis[3][3]){
    const VRWeaponGrip *rig=WeaponGrip(weapon);
    if(!model || !rig)return;
    EntityFn add=(EntityFn)(gameBase+0x999f0);
    GameEntity e={0};e.model=model;e.flags=4|0x80|0x40;e.nonNormalized=reflected;
    memcpy(e.origin,origin,12);memcpy(e.axis,axis,sizeof(e.axis));memcpy(e.lightingOrigin,base->origin,12);memset(e.shaderRGBA,255,4);add(&e);
    attachedHandsMask|=1u<<physical;
    AttachForearm(&e,side?rig->leftModel:rig->rightModel,physical);
}
static void AddWeaponHands(const GameRefdef *base){
    VRWeaponState state;if(!GameVR_GetWeaponState(&state) || state.weapon<1 || state.weapon>=37)return;
    const VRWeaponGrip *rig=WeaponGrip(state.weapon);if(!rig)return;
    float grip[3],axis[3][3],origin[3];if(!WeaponPose(base,&tracking,grip,axis))return;
    WeaponEntityOrigin(state.weapon,grip,axis,origin);
    for(int side=0;side<2;++side){
        if(side && (!weaponSupported || !rig->supportAuthored || magazine.stage || magazine.boltHeld))continue;
        float handOrigin[3];memcpy(handOrigin,origin,sizeof(handOrigin));
        if(side && state.weapon==6 && loadingLive)
            for(int i=0;i<3;++i)handOrigin[i]-=axis[0][i]*pump.pull*M590_PUMP_TRAVEL;
        AddAttachedHand(base,state.weapon,side,leftHanded?side:1-side,leftHanded,attachedGripModels[state.weapon][side],handOrigin,axis);
    }
}
static void AddOffhandKnife(const GameRefdef *base,int showHand){
    if(!offhandKnife)return;
    float grip[3],axis[3][3],origin[3];if(!KnifeHandPose(base,&tracking,1,grip,axis))return;
    ModelFn model=(ModelFn)(gameBase+0x96d60);
    if(showHand){
        WeaponEntityOrigin(1,grip,axis,origin);
        AddAttachedHand(base,1,0,leftHanded?1:0,!leftHanded,model(WeaponGrip(1)->rightModel),origin,axis);
    }
    GameEntity blade={0};blade.model=model("models/sof2vr_dropped/w1.md3");if(!blade.model)return;
    blade.flags=0x80|0x40;blade.nonNormalized=!leftHanded;
    KnifeRenderAxes(axis);WeaponEntityOrigin(1,grip,axis,blade.origin);memcpy(blade.axis,axis,sizeof(axis));
    memcpy(blade.lightingOrigin,base->origin,12);memset(blade.shaderRGBA,255,4);
    ((EntityFn)(gameBase+0x999f0))(&blade);
}
static void AddNativeHands(const GameRefdef *base){
    if(!tracking.input.focused || GetTickCount()-tracking.input.sampleTimeMs>=250)return;
    if(!magazine.boltHeld)actionHand.active=0;
    EntityFn add=(EntityFn)(gameBase+0x999f0);
    for(int h=0;h<2;++h){const BridgeHand *hand=&tracking.input.hands[h];
        if(!(hand->valid&BRIDGE_HAND_GRIP_VALID) || (attachedHandsMask&(1u<<h)))continue;
        int step=(int)lroundf(fmaxf(0,fminf(1,hand->squeeze))*16);
        int magazineHand=h==(leftHanded?1:0) && MagazineInteraction(reloadWeapon) && reloadHandModels[h] &&
            (magazine.stage==VR_MAG_EXTRACT || magazine.stage==VR_MAG_OLD || magazine.stage==VR_MAG_FRESH);
        GameEntity e={0};e.flags=4|0x80|0x40;e.model=magazineHand?reloadHandModels[h]:(nativeGripModels[h][step]?nativeGripModels[h][step]:nativeHandModels[h]);PoseInGame(base,&hand->grip,e.origin,e.axis);
        if(h==(leftHanded?1:0) && magazine.boltHeld){
            float gun[3],basis[3][3],contact[3],socket[3],pouch[3];
            if(ReloadGeometry(base,&tracking,gun,basis,socket,contact,pouch)){
                if(!actionHand.active || actionHand.weapon!=reloadWeapon || actionHand.epoch!=reloadEpoch){
                    for(int j=0;j<3;++j)for(int k=0;k<3;++k){actionHand.basis[j][k]=0;for(int i=0;i<3;++i)actionHand.basis[j][k]+=e.axis[j][i]*basis[k][i];}
                    actionHand.active=1;actionHand.weapon=reloadWeapon;actionHand.epoch=reloadEpoch;
                }
                float pull=fmaxf(magazine.boltPull,pistolSlideLocked?1:0),travel=ReloadActionTravel(reloadWeapon);
                for(int i=0;i<3;++i){e.origin[i]=contact[i]-basis[0][i]*pull*travel;for(int j=0;j<3;++j){e.axis[j][i]=0;for(int k=0;k<3;++k)e.axis[j][i]+=actionHand.basis[j][k]*basis[k][i];}}
                /* Place the closed native palm on the rendered moving part.
                 * Tracking still measures stroke distance, but its sideways
                 * drift cannot peel the visual fingers off the handle. */
                char path[100];snprintf(path,sizeof(path),"models/sof2vr_native/%chand_grip16.md3",h?'r':'l');e.model=((ModelFn)(gameBase+0x96d60))(path);step=16;
            }
        }
        /* Use the same head-area light-grid sample as the held assembly, not
         * a wrist that can enter solid geometry. Normals stay at native scale. */
        memcpy(e.lightingOrigin,base->origin,12);
        /* Native SoF2 bind mesh, native UVs and wrist-relative controller pose. */
        if(!magazineHand)for(int i=0;i<3;++i)for(int k=0;k<3;++k)e.origin[i]-=e.axis[k][i]*vrFreeHandPalm[h][k];
        e.nonNormalized=0;memset(e.shaderRGBA,255,4);add(&e);
        char wristSource[128];
        if(magazineHand)snprintf(wristSource,sizeof(wristSource),"%s",MagazineInteraction(reloadWeapon)->handModels[h]);
        else snprintf(wristSource,sizeof(wristSource),"models/sof2vr_native/%chand_grip%02d.md3",h?'r':'l',step);
        AttachForearm(&e,wristSource,h);
        static unsigned logged;if(logged<4){LOG_INFO("NATIVEGLOVE hand=%d model=%d origin=%.3f,%.3f,%.3f",h,e.model,e.origin[0],e.origin[1],e.origin[2]);++logged;}
    }
}
/* Experimental local body only. All wrists come from the exact submitted hand
 * meshes above, including weapon-latched support hands. Never alter weapon
 * pose, shots, controller input, locomotion or the desktop scene. */
static void AddBodyIK(const GameRefdef *base){
    if(!bodyIKEnabled)return;
    if(!tracking.valid||!tracking.input.focused||GetTickCount()-tracking.input.sampleTimeMs>=250){memset(bodyArms,0,sizeof(bodyArms));return;}
    int outfit=BodyOutfit();
    if(outfit<0||outfit>=7)return;
    const VRBodyProfile *profile=&vrBodyProfiles[outfit];
    static int models[7][5];static unsigned epoch;
    if(epoch!=reloadEpoch){memset(models,0,sizeof(models));memset(bodyArms,0,sizeof(bodyArms));epoch=reloadEpoch;}
    const char *parts[]={"torso","l_upper","l_lower","r_upper","r_lower"};ModelFn model=(ModelFn)(gameBase+0x96d60);
    for(int i=0;i<5;++i)if(!models[outfit][i]){char path[128];snprintf(path,sizeof(path),"%s/%s.md3",profile->prefix,parts[i]);models[outfit][i]=model(path);}
    float centre[3],q[4],inverse[4];BodyFrame(&tracking,centre,q,inverse);BridgePose pose={0};memcpy(pose.position,centre,12);memcpy(pose.orientation,q,16);
    GameEntity torso={0};torso.model=models[outfit][0];torso.flags=0x80|0x40;PoseInGame(base,&pose,torso.origin,torso.axis);
    memcpy(torso.lightingOrigin,base->origin,12);memset(torso.shaderRGBA,255,4);EntityFn add=(EntityFn)(gameBase+0x999f0);if(torso.model)add(&torso);
    for(int hand=0;hand<2;++hand){
        if(!(bodyWristMask&(1u<<hand))){memset(&bodyArms[hand],0,sizeof(bodyArms[hand]));continue;}
        float shoulder[3],wrist[3],hint[3];
        for(int i=0;i<3;++i){
            shoulder[i]=torso.origin[i];for(int k=0;k<3;++k)shoulder[i]+=torso.axis[k][i]*profile->shoulder[hand][k];
            shoulder[i]/=39.37f;wrist[i]=bodyWrists[hand][i]/39.37f;
            hint[i]=shoulder[i]+(hand?-1:1)*.22f*torso.axis[1][i]-.28f*torso.axis[2][i]-.06f*torso.axis[0][i];
        }
        const float *lengths=profile->length[hand];
        float reach=Distance3(shoulder,wrist);
        /* Controller tracking stops at the wrist, not the fingertips.
         * Start with a shorter wrist reach and extend the sleeves along
         * their axes as needed to keep a straight arm joined to the hand. */
        float fit=fmaxf(.89f,(reach+.0001f)*39.37f/(lengths[0]+lengths[1]));
        if(!VR_IKSolveArm(&bodyArms[hand],shoulder,wrist,hint,fit*lengths[0]/39.37f,fit*lengths[1]/39.37f,&bodyArms[hand]))continue;
        const VRArmIK *arm=&bodyArms[hand];
        for(int segment=0;segment<2;++segment){
            const float *start=segment?arm->elbow:arm->shoulder,*end=segment?arm->end:arm->elbow;GameEntity limb={0};
            limb.flags=0x80|0x40;limb.model=models[outfit][1+2*hand+segment];memcpy(limb.lightingOrigin,base->origin,12);memset(limb.shaderRGBA,255,4);
            float direction[3],width[3];
            for(int i=0;i<3;++i){limb.origin[i]=start[i]*39.37f;direction[i]=end[i]-start[i];
                width[i]=-torso.axis[0][(i+1)%3]*direction[(i+2)%3]+torso.axis[0][(i+2)%3]*direction[(i+1)%3];}
            if(!VR_IKSegmentBasis(direction,segment?bodyWristWidths[hand]:width,torso.axis[1],limb.axis))continue;
            limb.nonNormalized=1;
            for(int i=0;i<3;++i)limb.axis[0][i]*=fit;
            if(limb.model)add(&limb);
        }
        static DWORD logged[2];DWORD now=GetTickCount();
        if(tracePoses&&now-logged[hand]>1000){logged[hand]=now;LOG_INFO("BODYIK outfit=%s hand=%d mask=%u reachError=%.5f shoulder=%.3f,%.3f,%.3f wrist=%.3f,%.3f,%.3f",profile->name,hand,bodyWristMask,arm->reachError,shoulder[0],shoulder[1],shoulder[2],wrist[0],wrist[1],wrist[2]);}
    }
}
static void MakeRefdef(GameRefdef *eye, const GameRefdef *base, const BridgeView *view) {
    const float basis[3][3] = {{0,0,-1}, {-1,0,0}, {0,1,0}};
    float q[4], delta[3], position[3];
    *eye = *base;
    Multiply(referenceInverse, view->orientation, q);
    for (int i=0;i<3;++i) delta[i]=view->position[i]-TrackingReference(i);
    Rotate(referenceInverse, delta, position);
    for (int i=0;i<3;++i) {
        eye->origin[i] = base->origin[i] + 39.37f *
            (-position[2]*base->axis[0][i]-position[0]*base->axis[1][i]+position[1]*base->axis[2][i]);
    }
    for (int axis=0;axis<3;++axis) {
        float rotated[3]; Rotate(q, basis[axis], rotated);
        for (int i=0;i<3;++i) eye->axis[axis][i] =
            -rotated[2]*base->axis[0][i]-rotated[0]*base->axis[1][i]+rotated[1]*base->axis[2][i];
    }
    eye->x = eye->y = 0; eye->width = eyeWidth; eye->height = eyeHeight;
    eye->fovX = 2.0f*fmaxf(fabsf(view->fov[0]),fabsf(view->fov[1]))*57.2957795f;
    eye->fovY = 2.0f*fmaxf(fabsf(view->fov[2]),fabsf(view->fov[3]))*57.2957795f;
    char inspect[16];
    if(tracePoses && GetEnvironmentVariableA("SOF2_VR_TEST_STOWED_VIEW",inspect,sizeof(inspect))){
        /* Isolated asset inspection only: render the same submitted body and
         * holsters from outside. Controller poses/ownership are unchanged. */
        float front=!strcmp(inspect,"front")?1:!strcmp(inspect,"back")?-1:0;
        float side=!strcmp(inspect,"left")?1:!strcmp(inspect,"right")?-1:0;
        float length=0;
        for(int k=0;k<3;++k){eye->origin[k]=base->origin[k]+(front*base->axis[0][k]+side*base->axis[1][k])*64-base->axis[2][k]*17;
            eye->axis[0][k]=base->origin[k]-base->axis[2][k]*24-eye->origin[k];length+=eye->axis[0][k]*eye->axis[0][k];}
        length=sqrtf(length);for(int k=0;k<3;++k)eye->axis[0][k]/=length;
        length=0;for(int k=0;k<3;++k){eye->axis[1][k]=base->axis[2][(k+1)%3]*eye->axis[0][(k+2)%3]-base->axis[2][(k+2)%3]*eye->axis[0][(k+1)%3];length+=eye->axis[1][k]*eye->axis[1][k];}
        length=sqrtf(length);for(int k=0;k<3;++k)eye->axis[1][k]/=length;
        for(int k=0;k<3;++k)eye->axis[2][k]=eye->axis[0][(k+1)%3]*eye->axis[1][(k+2)%3]-eye->axis[0][(k+2)%3]*eye->axis[1][(k+1)%3];
    }
}

static void DrawComfortVignette(void){
    if(!accessibility.vignette || !tracking.input.focused)return;
    int moving=heldControls[1] || heldControls[2] || heldControls[3] || heldControls[4];
    int turnHand=accessibility.switchSticks?0:1;
    moving|=!snapAngle && fabsf(tracking.input.hands[turnHand].stick[0])>accessibility.deadzone*.01f;
    if(!moving)return;
    glPushAttrib(GL_ALL_ATTRIB_BITS);glDisable(GL_DEPTH_TEST);glDisable(GL_ALPHA_TEST);glDisable(GL_CULL_FACE);glDisable(GL_FOG);glDisable(GL_TEXTURE_2D);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION);glPushMatrix();glLoadIdentity();glMatrixMode(GL_MODELVIEW);glPushMatrix();glLoadIdentity();
    glBegin(GL_QUAD_STRIP);for(int n=0;n<=64;++n){float a=n*6.28318530718f/64,x=cosf(a),y=sinf(a);
        glColor4f(0,0,0,0);glVertex2f(x*.62f,y*.62f);glColor4f(0,0,0,accessibility.vignette*.01f);glVertex2f(x*1.45f,y*1.45f);}
    glEnd();glPopMatrix();glMatrixMode(GL_PROJECTION);glPopMatrix();glMatrixMode(GL_MODELVIEW);glPopAttrib();
}
static void DrawWeaponReticle(const GameRefdef *eye){
    VRWeaponState state;if(!accessibility.reticle || !weaponHeld || !controlsEnabled || !GameVR_GetWeaponState(&state))return;
    const VRWeaponProfile *profile=VR_Profile(state.weapon);if(!profile || profile->category==1 || profile->category>=9)return;
    float origin[3],axis[3][3],target[3],direction[3];if(!WeaponBorePose(&playerBase,&tracking,state.weapon,origin,axis))return;
    int solution=profile->category==8?GameVR_GrenadeAim(state.weapon,0,origin,axis[0],direction,target):GameVR_TraceAim(origin,axis[0],target);if(!solution)return;
    float d[3],forward=0,left=0,up=0;for(int i=0;i<3;++i){d[i]=target[i]-eye->origin[i];forward+=d[i]*eye->axis[0][i];left+=d[i]*eye->axis[1][i];up+=d[i]*eye->axis[2][i];}
    if(forward<1)return;float l=tanf(activeView.fov[0]),r=tanf(activeView.fov[1]),u=tanf(activeView.fov[2]),b=tanf(activeView.fov[3]);
    float x=2*((-left/forward-l)/(r-l))-1,y=2*((up/forward-b)/(u-b))-1;if(fabsf(x)>1 || fabsf(y)>1)return;
    glPushAttrib(GL_ALL_ATTRIB_BITS);glDisable(GL_TEXTURE_2D);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glDisable(GL_LIGHTING);glDisable(GL_CULL_FACE);glDisable(GL_FOG);glLineWidth(2);
    glMatrixMode(GL_PROJECTION);glPushMatrix();glLoadIdentity();glMatrixMode(GL_MODELVIEW);glPushMatrix();glLoadIdentity();
    float sx=9.0f/eyeWidth,sy=9.0f/eyeHeight;
    for(int outline=1;outline>=0;--outline){
        if(outline){glColor4f(0,0,0,1);glLineWidth(4);}else{glColor4f(1,solution==1?1:.65f,solution==1?1:.1f,1);glLineWidth(2);}
        glBegin(GL_LINES);glVertex2f(x-sx*2,y);glVertex2f(x-sx,y);glVertex2f(x+sx,y);glVertex2f(x+sx*2,y);glVertex2f(x,y-sy*2);glVertex2f(x,y-sy);glVertex2f(x,y+sy);glVertex2f(x,y+sy*2);glEnd();
    }
    glPopMatrix();glMatrixMode(GL_PROJECTION);glPopMatrix();glPopAttrib();
    static DWORD last;if(tracePoses && GetTickCount()-last>1000){last=GetTickCount();LOG_INFO("RETICLE weapon=%d target=%.3f,%.3f,%.3f solution=%d",state.weapon,target[0],target[1],target[2],solution);}
}
static void DrawReloadPrompt(void){
    if(!controlsEnabled || !tracking.input.focused)return;
    static GLuint textures[12];static HGLRC context;HGLRC current=wglGetCurrentContext();if(current!=context){memset(textures,0,sizeof(textures));context=current;}
    int stage=reloadLive?magazine.stage:0;
    if(loadingLive){if(shellLoad.stage==1)stage=8;else if(shellLoad.stage==2)stage=10;else if(manualPump && pump.needed)stage=9;}
    const VRWeaponProfile *promptProfile=VR_Profile(reloadWeapon);
    if(stage==VR_MAG_BOLT && promptProfile && promptProfile->category==2)stage=11;
    if(stage<=0 || stage>11)return;
    const char *labels[]={"","PULL THE MAGAZINE DOWN","RELEASE THE OLD MAGAZINE","GRIP A SPARE AT YOUR CHEST","INSERT MAGAZINE INTO WELL","OFF-HAND TRIGGER: RACK SLIDE / BOLT","MAGAZINE SEATED","","INSERT SHELL INTO RECEIVER LOADING PORT","OFF-HAND GRIP + TRIGGER: PUMP BACK THEN FORWARD","SHELL SEATED","B: RELEASE SLIDE / OFF-HAND TRIGGER: RACK SLIDE"};
    glPushAttrib(GL_ALL_ATTRIB_BITS);glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);glPixelStorei(GL_UNPACK_SKIP_ROWS,0);
    glEnable(GL_TEXTURE_2D);glDisable(GL_DEPTH_TEST);glDisable(GL_LIGHTING);glDisable(GL_SCISSOR_TEST);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDisable(GL_CULL_FACE);glDisable(GL_FOG);
    if(!textures[stage]){
        BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=768;info.bmiHeader.biHeight=-48;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void *bits=NULL;HDC dc=CreateCompatibleDC(NULL);HBITMAP bmp=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,NULL,0);
        if(dc && bmp && bits){HGDIOBJ old=SelectObject(dc,bmp);memset(bits,0,768*48*4);HFONT font=CreateFontA(24,0,0,0,FW_SEMIBOLD,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,"Segoe UI");HGDIOBJ oldFont=SelectObject(dc,font);
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));RECT rect={0,0,768,48};DrawTextA(dc,labels[stage],-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);GdiFlush();
            unsigned char *pixels=bits;for(int i=0;i<768*48;++i){unsigned char alpha=pixels[i*4+1];pixels[i*4]=220;pixels[i*4+1]=240;pixels[i*4+2]=255;pixels[i*4+3]=alpha;}
            glGenTextures(1,&textures[stage]);glBindTexture(GL_TEXTURE_2D,textures[stage]);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,768,48,0,GL_RGBA,GL_UNSIGNED_BYTE,bits);
            SelectObject(dc,oldFont);DeleteObject(font);SelectObject(dc,old);
        }if(bmp)DeleteObject(bmp);if(dc)DeleteDC(dc);
    }
    glMatrixMode(GL_PROJECTION);glPushMatrix();glLoadIdentity();glMatrixMode(GL_MODELVIEW);glPushMatrix();glLoadIdentity();glBindTexture(GL_TEXTURE_2D,textures[stage]);glColor4f(1,1,1,stage?1:.7f);
    float inv[4],delta[3],offset[3];for(int i=0;i<3;++i){inv[i]=-tracking.views[0].orientation[i];delta[i]=activeView.position[i]-(tracking.views[0].position[i]+tracking.views[1].position[i])*.5f;}inv[3]=tracking.views[0].orientation[3];Rotate(inv,delta,offset);
    float z=2+offset[2],l=tanf(activeView.fov[0]),r=tanf(activeView.fov[1]),u=tanf(activeView.fov[2]),b=tanf(activeView.fov[3]);
    float x0=2*(((-1.30f-offset[0])/z-l)/(r-l))-1,x1=2*(((1.30f-offset[0])/z-l)/(r-l))-1;
    float y0=2*(((-1.20f-offset[1])/z-b)/(u-b))-1,y1=2*(((-1.04f-offset[1])/z-b)/(u-b))-1;
    glBegin(GL_QUADS);glTexCoord2f(0,1);glVertex2f(x0,y0);glTexCoord2f(1,1);glVertex2f(x1,y0);glTexCoord2f(1,0);glVertex2f(x1,y1);glTexCoord2f(0,0);glVertex2f(x0,y1);glEnd();
    glPopMatrix();glMatrixMode(GL_PROJECTION);glPopMatrix();glPopClientAttrib();glPopAttrib();
}
#include "vr_number_hud.h"
static void ReadRGBA(int width, int height, void *pixels) {
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPopClientAttrib();
}

static void ReadTransport(int width,int height,void *pixels){
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);glPixelStorei(GL_PACK_ALIGNMENT,1);
    glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);
    glReadPixels(0,0,width,height,GL_BGRA,GL_UNSIGNED_BYTE,pixels);glPopClientAttrib();
}
static void CaptureHandMode(int eye){
    static unsigned saved[37],last=~0u;static int lastWeapon;static DWORD entered;char flag[8];VRWeaponState held;
    static unsigned sideSaved[37][6];static int lastView=-2;
    if(eye || !tracePoses)return;
    int audit=GetEnvironmentVariableA("SOF2_VR_TEST_GRIP_AUDIT",flag,sizeof(flag))>0;
    if(eye || !tracePoses || (!audit && !GetEnvironmentVariableA("SOF2_VR_TEST_HANDS",flag,sizeof(flag))) || !GameVR_GetWeaponState(&held) || held.weapon<1 || held.weapon>=37 || (!audit && held.weapon!=9))return;
    if(audit && (held.weapon!=testGripAuditWeapon || (held.state!=0 && held.state!=27)))return;
    unsigned mode=attachedHandsMask;if(mode!=last || held.weapon!=lastWeapon || testArsenalView!=lastView){last=mode;lastWeapon=held.weapon;lastView=testArsenalView;entered=GetTickCount();}
    unsigned *record=testArsenalView>=0 && testArsenalView<6?&sideSaved[held.weapon][testArsenalView]:&saved[held.weapon];
    if((mode!=(leftHanded?1u:2u) && mode!=3) || GetTickCount()-entered<150 || (*record&(1u<<mode)))return;
    size_t bytes=(size_t)eyeWidth*eyeHeight*4;unsigned char *pixels=malloc(bytes);if(!pixels)return;ReadRGBA(eyeWidth,eyeHeight,pixels);
    for(size_t i=0;i<bytes;i+=4){unsigned char r=pixels[i];pixels[i]=pixels[i+2];pixels[i+2]=r;}
    char path[MAX_PATH];GetModuleFileNameA(selfModule,path,sizeof(path));char *slash=strrchr(path,'\\');
    if(slash){if(audit && testArsenalView>=0)snprintf(slash+1,sizeof(path)-(size_t)(slash+1-path),"weapon-%02d-view-%d-mode-%u.bmp",held.weapon,testArsenalView,mode);else if(audit)snprintf(slash+1,sizeof(path)-(size_t)(slash+1-path),"weapon-%02d-hand-mode-%u.bmp",held.weapon,mode);else snprintf(slash+1,sizeof(path)-(size_t)(slash+1-path),"hand-mode-%u.bmp",mode);FILE *f=fopen(path,"wb");
        if(f){BITMAPFILEHEADER fh={0};BITMAPINFOHEADER ih={0};fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(ih);fh.bfSize=(DWORD)(fh.bfOffBits+bytes);ih.biSize=sizeof(ih);ih.biWidth=eyeWidth;ih.biHeight=eyeHeight;ih.biPlanes=1;ih.biBitCount=32;fwrite(&fh,sizeof(fh),1,f);fwrite(&ih,sizeof(ih),1,f);fwrite(pixels,bytes,1,f);fclose(f);*record|=1u<<mode;LOG_INFO("HANDIMAGE weapon=%d mode=%u view=%d path=%s",held.weapon,mode,testArsenalView,path);}}
    free(pixels);
}
static void CaptureManualStage(int eye){
    static unsigned saved;static int last=-1;static DWORD entered;
    if(!tracePoses || !testReloadActive || eye || !reloadLive)return;int stage=magazine.stage;if(stage==VR_MAG_BOLT && magazine.boltHeld && magazine.boltPull>.85f)stage=7;
    char targetText[12];int target=GetEnvironmentVariableA("SOF2_VR_TEST_RELOAD_WEAPON",targetText,sizeof(targetText))?atoi(targetText):9;
    VRWeaponState captured;if(!GameVR_GetWeaponState(&captured) || captured.weapon!=target)return;
    if(stage!=last){last=stage;entered=GetTickCount();}if(GetTickCount()-entered<120 || saved&(1u<<stage))return;
    size_t bytes=(size_t)eyeWidth*eyeHeight*4;unsigned char *pixels=malloc(bytes);if(!pixels)return;ReadRGBA(eyeWidth,eyeHeight,pixels);
    for(size_t i=0;i<bytes;i+=4){unsigned char r=pixels[i];pixels[i]=pixels[i+2];pixels[i+2]=r;}
    char path[MAX_PATH];GetModuleFileNameA(selfModule,path,sizeof(path));char *slash=strrchr(path,'\\');if(slash){snprintf(slash+1,sizeof(path)-(size_t)(slash+1-path),"manual-stage%d.bmp",stage);FILE *f=fopen(path,"wb");if(f){BITMAPFILEHEADER fh={0};BITMAPINFOHEADER ih={0};fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(ih);fh.bfSize=(DWORD)(fh.bfOffBits+bytes);ih.biSize=sizeof(ih);ih.biWidth=eyeWidth;ih.biHeight=eyeHeight;ih.biPlanes=1;ih.biBitCount=32;fwrite(&fh,sizeof(fh),1,f);fwrite(&ih,sizeof(ih),1,f);fwrite(pixels,bytes,1,f);fclose(f);saved|=1u<<stage;LOG_INFO("MANUALIMAGE stage=%d path=%s",stage,path);}}free(pixels);
}
static void TraceSceneEntities(const GameRefdef *fd) {
    static int saved;
    if(!tracePoses || saved)return;
    unsigned count=*(unsigned *)(gameBase+0x7b1e04);
    BYTE *backend=*(BYTE **)(gameBase+0x782068);
    if(!backend || !count || count>1024)return;
    char path[MAX_PATH];GetModuleFileNameA(selfModule,path,sizeof(path));
    char *slash=strrchr(path,'\\');if(!slash)return;strcpy(slash+1,"scene-entities.bin");
    FILE *file=fopen(path,"wb");if(!file)return;
    fwrite(&count,4,1,file);fwrite(fd,sizeof(*fd),1,file);
    for(unsigned i=0;i<count;++i)fwrite(backend+0x101200+i*0x100,0xd0,1,file);
    fclose(file);saved=1;LOG_INFO("ENTITY snapshot count=%u",count);
    for(unsigned i=0;i<count;++i){GameEntity *e=(GameEntity *)(backend+0x101200+i*0x100);
        if((e->flags&12)!=12)continue;
        int handle=*(int *)((BYTE *)e+204);
        uint32_t *v=((uint32_t *(__cdecl *)(int))(gameBase+0x490a0))(handle);
        if(v && v[1] && v[2]>=v[1] && (v[2]-v[1])%208==0 && (v[2]-v[1])/208<=16){
            strcpy(slash+1,"view-g2.bin");file=fopen(path,"wb");if(file){fwrite((void *)(uintptr_t)v[1],v[2]-v[1],1,file);fclose(file);}
        }
    }
}

static void __cdecl BridgeScene(const GameRefdef *fd) {
    if (!originalScene || !fd) return;
    EnsureNativeGLDispatch();
    if (InterlockedCompareExchange(&sceneGuard, 1, 0)) return;
    if (nativeSinceSwap || (fd->flags & 1) || !HelperAlive() || shared->helperStatus < 2 ||
        !Bridge_TryLock(&shared->trackingLock)) goto desktop;
    tracking = shared->tracking; Bridge_Unlock(&shared->trackingLock);
    if (!tracking.valid) goto desktop;
    lastNativeTime=GetTickCount();
    /* A faster desktop loop must not repeatedly read back both large eyes
     * for the same XR prediction. Keep the last native frame in that case. */
    if(testReloadActive || testGripAuditActive)memcpy(tracking.input.hands,testReloadHands,sizeof(testReloadHands));
    if (tracking.serial == renderedTrackingSerial) { nativeSinceSwap=1; goto desktop; }
    /* Empty the desktop command queue before selecting a private eye target. */
    flushCommands();
    TraceSceneEntities(fd);
    if (!MakeEyeTargets()) goto desktop;
    if (!recentered) {
        for (int i=0;i<3;++i) referencePosition[i]=(tracking.views[0].position[i]+tracking.views[1].position[i])*0.5f;
        /* Recenter heading only: never bake head pitch/roll into world gravity. */
        float ahead[3]={0,0,-1},forward[3];Rotate(tracking.views[0].orientation,ahead,forward);
        float heading=atan2f(-forward[0],-forward[2]);
        referenceInverse[0]=referenceInverse[2]=0;
        referenceInverse[1]=-sinf(heading*.5f);referenceInverse[3]=cosf(heading*.5f);recentered=1;
        standingEyeHeight=referencePosition[1];
        LOG_INFO("Head tracking recentered; native stereo rendering started");
    }
    BYTE *backend=*(BYTE **)(gameBase+0x782068);
    unsigned count=*(unsigned *)(gameBase+0x7b1e04),first=*(unsigned *)(gameBase+0x7b1e00);
    if(!backend || count>1022 || first>count)goto desktop;
    memcpy(entityBackup,backend+0x101200,count*256);
    GameRefdef base=*fd;playerScene=0;
    for(unsigned i=first;i<count;++i){GameEntity *e=(GameEntity *)entityBackup[i];if((e->flags&12)==12)playerScene=1;}
    if(playerScene && controlsEnabled){
        float yaw=atan2f(fd->axis[0][1],fd->axis[0][0]);
        base.axis[0][0]=cosf(yaw);base.axis[0][1]=sinf(yaw);base.axis[0][2]=0;
        base.axis[1][0]=-sinf(yaw);base.axis[1][1]=cosf(yaw);base.axis[1][2]=0;
        base.axis[2][0]=base.axis[2][1]=0;base.axis[2][2]=1;
        static int lastGameTime;
        if(fd->time+100<lastGameTime){++reloadEpoch;LOG_INFO("MANUAL context time reset old=%d new=%d",lastGameTime,fd->time);GameVR_CancelRoundInsertion();}
        lastGameTime=fd->time;playerBase=base;playerBaseTime=GetTickCount();
    }
    /* RE_RenderScene advances the first-scene entity/light/poly/surface
     * counters. Restore them between passes so both eyes and the desktop use
     * the same submitted scene. Flush each eye while its FBO is still bound. */
    attachedHandsMask=0;
    if(playerScene && controlsEnabled){float grip[3],axis[3][3];WeaponPose(&base,&tracking,grip,axis);}
    int nativeHands=playerScene && controlsEnabled && accessibility.hands && PrepareNativeHands();
    if(playerScene && controlsEnabled)for(unsigned i=first;i<count;++i){GameEntity *e=(GameEntity *)entityBackup[i];if((e->flags&12)==12)HideStockArms(e);}
    if(playerScene && controlsEnabled)for(unsigned i=first;i<count;++i){GameEntity *e=(GameEntity *)entityBackup[i];if((e->flags&12)==12)CompleteWeaponSides(e);}
    reloadLive=weaponHeld && playerScene && controlsEnabled && PrepareReloadParts();
    int seating=MagazineInteraction(reloadWeapon) && magazineGuide.seatStarted && GetTickCount()-magazineGuide.seatStarted<140;
    reloadRenderReady=reloadLive && reloadWeapon==reloadAssetWeapon && (magazine.stage || magazine.boltHeld || pistolSlideLocked || (reloadWeapon==8 && openBoltReady) || seating) && tracking.input.focused &&
        ReloadGeometry(&base,&tracking,reloadRender.origin,reloadRender.axis,reloadRender.socket,reloadRender.bolt,reloadRender.pouch);
    UpdateDroppedMagazines(&base);
    UpdateDroppedWeapons();
    loadingLive=weaponHeld && playerScene && controlsEnabled && PrepareLoadingParts();
    /* The licensed M4 assembly includes its complete M203. Hide the separate
     * native launcher bolton for these eye passes to avoid drawing it twice. */
    if(playerScene && controlsEnabled){VRWeaponState held;
        if(GameVR_GetWeaponState(&held) && held.weapon==9 && VRReplacementWeapon(9))
            for(unsigned i=first;i<count;++i){GameEntity *e=(GameEntity *)entityBackup[i];if((e->flags&12)==12)HideWeaponModel(e,"models/weapons/m203/m203.glm");}
    }
    static unsigned lastHandMask=~0u;
    /* Hide the intact Ghoul2 model only when its complete replacement body
     * and exact render pose are available for both eye passes. */
    if(reloadRenderReady)for(unsigned i=first;i<count;++i){GameEntity *e=(GameEntity *)entityBackup[i];if((e->flags&12)==12)HideReloadWeapon(e);}
    if(loadingLive && (manualPump || shellLoad.stage))for(unsigned i=first;i<count;++i){GameEntity *e=(GameEntity *)entityBackup[i];if((e->flags&12)==12)HideWeaponModel(e,"models/weapons/m590/m590.glm");}
    unsigned char sceneCounters[0x3c];
    memcpy(sceneCounters, gameBase + 0x7b1dec, sizeof(sceneCounters));
    GLint previousFBO, previousMode;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFBO);
    glGetIntegerv(GL_MATRIX_MODE, &previousMode);
    glMatrixMode(GL_PROJECTION); glPushMatrix();
    glMatrixMode(GL_MODELVIEW); glPushMatrix();
    double eyesStarted=PerfNow();
    VRWeaponState visibleWeapon={0};
    GameVR_GetWeaponState(&visibleWeapon);
    BridgeView centreView=tracking.views[0];
    for(int i=0;i<3;++i)centreView.position[i]=(tracking.views[0].position[i]+tracking.views[1].position[i])*.5f;
    GameRefdef hudHead;MakeRefdef(&hudHead,&base,&centreView);
    GameRefdef stereoEyes[2];
    for(int eye=0;eye<2;++eye)MakeRefdef(&stereoEyes[eye],&base,&tracking.views[eye]);
    if(playerScene)PrepareNumberHud(stereoEyes,&hudHead);
    int equipModel=0;
    if(playerScene && controlsEnabled && trackWeapon && weaponHeld &&
       (visibleWeapon.state==10 || visibleWeapon.state==11) && DropProfile(visibleWeapon.weapon)){
        char path[80];snprintf(path,sizeof(path),"models/sof2vr_dropped/w%d.md3",visibleWeapon.weapon);
        equipModel=((ModelFn)(gameBase+0x96d60))(path);
    }
    for (int eye=0;eye<2;++eye) {
        GameRefdef eyeDef;
        memcpy(gameBase + 0x7b1dec, sceneCounters, sizeof(sceneCounters));
        memcpy(backend+0x101200,entityBackup,count*256);
        if(playerScene && controlsEnabled){
            float grip[3],axis[3][3];
            int poseValid=WeaponPose(&base,&tracking,grip,axis);
            if(trackWeapon && poseValid)for(unsigned i=first;i<count;++i){
                GameEntity *e=(GameEntity *)(backend+0x101200+i*256);
                /* Classify the original submitted entity. RF_DEPTHHACK is
                 * removed for VR rendering, so the modified eye entity is
                 * no longer a reliable first-person classification source. */
                if((((const GameEntity *)entityBackup[i])->flags&12)!=12)continue;
                if(equipModel){e->flags|=2;continue;}
                int id=visibleWeapon.weapon;float render[3][3];memcpy(render,axis,36);if(id==1)KnifeRenderAxes(render);
                memcpy(e->axis,render,sizeof(e->axis));e->nonNormalized=leftHanded?1:0;e->flags|=0x80;memcpy(e->lightingOrigin,base.origin,12);
                WeaponEntityOrigin(id,grip,render,e->origin);
                if(!eye && tracePoses && GetTickCount()-lastPoseTrace>=1000)LOG_INFO("WEAPON grip=%.3f,%.3f,%.3f forward=%.4f,%.4f,%.4f",grip[0],grip[1],grip[2],axis[0][0],axis[0][1],axis[0][2]);
            }
            if(!poseValid)for(unsigned i=first;i<count;++i){
                GameEntity *e=(GameEntity *)(backend+0x101200+i*256);if((((const GameEntity *)entityBackup[i])->flags&12)==12)e->flags|=2;
            }
            if(equipModel && poseValid){
                GameEntity weapon={0};weapon.model=equipModel;weapon.flags=4|0x80|0x40;weapon.nonNormalized=leftHanded;
                if(visibleWeapon.weapon==1)KnifeRenderAxes(axis);
                WeaponEntityOrigin(visibleWeapon.weapon,grip,axis,weapon.origin);memcpy(weapon.axis,axis,sizeof(axis));
                memcpy(weapon.lightingOrigin,base.origin,12);memset(weapon.shaderRGBA,255,4);
                ((EntityFn)(gameBase+0x999f0))(&weapon);
                static int lastEquipWeapon,lastEquipState;
                if(!eye && tracePoses && (lastEquipWeapon!=visibleWeapon.weapon || lastEquipState!=visibleWeapon.state)){
                    lastEquipWeapon=visibleWeapon.weapon;lastEquipState=visibleWeapon.state;
                    LOG_INFO("EQUIP static weapon=%d state=%d",lastEquipWeapon,lastEquipState);
                }
            }
        }
        /* Remove native in-view depth compression only AFTER identifying
         * and transforming the weapon. Effects still use ordinary world
         * depth, while the gun retains its tracked position in every state. */
        for(unsigned i=first;i<count;++i){GameEntity *e=(GameEntity *)(backend+0x101200+i*256);e->flags&=~8;}
        if(nativeHands){bodyWristMask=0;AddWeaponHands(&base);}
        if(playerScene && controlsEnabled)AddOffhandKnife(&base,nativeHands);
        if(nativeHands){AddNativeHands(&base);AddBodyIK(&base);}
        if(!eye && tracePoses && attachedHandsMask!=lastHandMask){lastHandMask=attachedHandsMask;LOG_INFO("HANDMODE explicit-native-poses=%u supported=%d",attachedHandsMask,weaponSupported);}
        if(reloadLive && !equipModel)AddReloadParts(&base);
        AddDroppedMagazines();
        AddDroppedWeapons();
        AddStowedWeapons(&base,&tracking);
        if(loadingLive && !equipModel)AddLoadingParts(&base);
        eyeDef=stereoEyes[eye];
        if(!eye && tracePoses){char cameraTest[8];
            if(GetEnvironmentVariableA("SOF2_VR_TEST_CAMERA",cameraTest,sizeof(cameraTest))){
                VRWeaponState state={0};GameVR_GetWeaponState(&state);
                LOG_INFO("CAMERATEST time=%d weapon=%d state=%d origin=%.5f,%.5f,%.5f forward=%.7f,%.7f,%.7f",fd->time,state.weapon,state.state,eyeDef.origin[0],eyeDef.origin[1],eyeDef.origin[2],eyeDef.axis[0][0],eyeDef.axis[0][1],eyeDef.axis[0][2]);
            }
        }
        if(!eye && tracePoses && GetTickCount()-lastPoseTrace>=1000){
            LOG_INFO("POSE serial=%u origin=%.5f,%.5f,%.5f forward=%.5f,%.5f,%.5f",
                tracking.serial,eyeDef.origin[0],eyeDef.origin[1],eyeDef.origin[2],
                eyeDef.axis[0][0],eyeDef.axis[0][1],eyeDef.axis[0][2]);
            lastPoseTrace=GetTickCount();
        }
        bindFBO(GL_FRAMEBUFFER, eyeFBO[eye]);
        glDrawBuffer(GL_COLOR_ATTACHMENT0); glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPushAttrib(GL_SCISSOR_BIT|GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        glDepthMask(GL_TRUE); glStencilMask(~0u);
        /* RB_DrawBuffer (verified RVA 7dd20) selects the map's native fog /
         * background clear colour before RE_RenderScene. RMG skies can be
         * deliberately textureless: clearing their eye targets to black
         * discards the only background they have. Preserve that GL colour. */
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
        glPopAttrib();
        if(!eye)PrepareWeaponMaterials();
        activeView=tracking.views[eye];activeEye=eye;eyeProjectionChecked=0;eyeActive=1;activeSkyEye=&eyeDef;eyeSkyDrawn=0;
        originalScene(&eyeDef); flushCommands();if(!eyeSkyDrawn)ReplayNativeSky(&eyeDef);activeSkyEye=NULL;if(playerScene){DrawComfortVignette();DrawWeaponReticle(&eyeDef);DrawReloadPrompt();DrawNumberHud(eye);CaptureManualStage(eye);CaptureHandMode(eye);}
        eyeActive=0;

    }
    memcpy(gameBase + 0x7b1dec, sceneCounters, sizeof(sceneCounters));
    memcpy(backend+0x101200,entityBackup,count*256);
    RestoreStockArms();
    bindFBO(GL_FRAMEBUFFER, previousFBO);
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(previousMode);
    perfEyes+=PerfNow()-eyesStarted;
    nativeRendered=1;hudDraws=0;++hudEpoch;
    renderedTrackingSerial=tracking.serial;
    nativeSinceSwap=1;
desktop:
    {
        double started=PerfNow();
        /* Keep the native scene bookkeeping, but avoid a third full-size
         * raster pass. The desktop receives an eye-texture mirror at swap. */
        GameRefdef mirror=*fd;
        if(nativeSinceSwap && eyeFBO[0] && desktopMirror && blitFBO && !(fd->flags&1)){mirror.width=mirror.height=1;}
        originalScene(&mirror);perfDesktop+=PerfNow()-started;
    }
    InterlockedExchange(&sceneGuard, 0);
}

/* Two bounded GPU readback slots. Never wait for an unfinished fence; keep
 * each completed image paired with the exact poses that rendered it. */
#define SOF2_GPU_PRODUCER
#include "gpu_transport.h"
static int PublishAsyncStereo(void){
    if(!readbackInitialized){
        readbackInitialized=1;
        readGenBuffers=(PFNGLGENBUFFERSPROC)wglGetProcAddress("glGenBuffers");
        readBindBuffer=(PFNGLBINDBUFFERPROC)wglGetProcAddress("glBindBuffer");
        readBufferData=(PFNGLBUFFERDATAPROC)wglGetProcAddress("glBufferData");
        readBufferSize=(PFNGLGETBUFFERPARAMETERIVPROC)wglGetProcAddress("glGetBufferParameteriv");
        readMapBuffer=(PFNGLMAPBUFFERRANGEPROC)wglGetProcAddress("glMapBufferRange");
        readUnmapBuffer=(PFNGLUNMAPBUFFERPROC)wglGetProcAddress("glUnmapBuffer");
        readFence=(PFNGLFENCESYNCPROC)wglGetProcAddress("glFenceSync");
        readWait=(PFNGLCLIENTWAITSYNCPROC)wglGetProcAddress("glClientWaitSync");
        readDeleteSync=(PFNGLDELETESYNCPROC)wglGetProcAddress("glDeleteSync");
        readbackAvailable=readGenBuffers && readBindBuffer && readBufferData && readBufferSize && readMapBuffer && readUnmapBuffer && readFence && readWait && readDeleteSync;
        if(readbackAvailable){
            GLint previous;glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&previous);
            for(int slot=0;slot<2;++slot){readGenBuffers(2,readSlots[slot].buffers);
                for(int eye=0;eye<2;++eye){readBindBuffer(GL_PIXEL_PACK_BUFFER,readSlots[slot].buffers[eye]);readBufferData(GL_PIXEL_PACK_BUFFER,(GLsizeiptr)eyeWidth*eyeHeight*4,NULL,GL_STREAM_READ);
                    GLint size=0;readBufferSize(GL_PIXEL_PACK_BUFFER,GL_BUFFER_SIZE,&size);
                    if(size!=(GLint)(eyeWidth*eyeHeight*4))readbackAvailable=0;
                }
            }
            readBindBuffer(GL_PIXEL_PACK_BUFFER,previous);
        }
        LOG_INFO("STEREO readback=%s resolution=%ux%u",readbackAvailable?"asynchronous PBO, nonblocking fences":"synchronous compatibility",eyeWidth,eyeHeight);
    }
    if(!readbackAvailable)return 0;
    GLint previous,fbo,readBuffer;glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&previous);glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);glGetIntegerv(GL_READ_BUFFER,&readBuffer);
    int ready=-1;
    for(int slot=0;slot<2;++slot)if(readSlots[slot].fence){
        GLenum result=readWait(readSlots[slot].fence,0,0);
        if(result==GL_ALREADY_SIGNALED || result==GL_CONDITION_SATISFIED){
            if(ready<0 || readSlots[slot].sequence>readSlots[ready].sequence)ready=slot;
        }else if(result==GL_WAIT_FAILED){readbackAvailable=0;LOG_WARN("Async readback fence failed; restoring synchronous transport");}
    }
    if(!readbackAvailable){readBindBuffer(GL_PIXEL_PACK_BUFFER,previous);return 0;}
    if(ready>=0){
        /* Completed GPU buffers need only one CPU copy into the mailbox.
         * Map both first; never lock the helper out while waiting on the GPU. */
        int valid=1;size_t bytes=(size_t)eyeWidth*eyeHeight*4;const void *mapped[2]={NULL,NULL};
        for(int eye=0;eye<2;++eye){readBindBuffer(GL_PIXEL_PACK_BUFFER,readSlots[ready].buffers[eye]);
            mapped[eye]=readMapBuffer(GL_PIXEL_PACK_BUFFER,0,bytes,GL_MAP_READ_BIT);
            if(!mapped[eye]){valid=0;break;}
        }
        if(valid && Bridge_TryLock(&shared->frameLock)){
            for(int eye=0;eye<2;++eye)memcpy(shared->pixels[eye],mapped[eye],bytes);
            for(int eye=0;eye<2;++eye){readBindBuffer(GL_PIXEL_PACK_BUFFER,readSlots[ready].buffers[eye]);if(!readUnmapBuffer(GL_PIXEL_PACK_BUFFER))valid=0;mapped[eye]=NULL;}
            if(valid){
            shared->frame.mode=BRIDGE_MODE_STEREO;shared->frame.width=eyeWidth;shared->frame.height=eyeHeight;
            memcpy(shared->frame.views,readSlots[ready].views,sizeof(shared->frame.views));
            shared->frame.serial=++imageSerial;
            }else shared->frame.serial=0; /* Never expose corrupted pixels under an old frame ID. */
            Bridge_Unlock(&shared->frameLock);
        }
        for(int eye=0;eye<2;++eye)if(mapped[eye]){readBindBuffer(GL_PIXEL_PACK_BUFFER,readSlots[ready].buffers[eye]);if(!readUnmapBuffer(GL_PIXEL_PACK_BUFFER))valid=0;}
        unsigned completed=readSlots[ready].sequence;
        for(int slot=0;slot<2;++slot)if(readSlots[slot].fence && readSlots[slot].sequence<=completed){readDeleteSync(readSlots[slot].fence);readSlots[slot].fence=NULL;}
        if(!valid){readbackAvailable=0;LOG_WARN("Async readback map failed; restoring synchronous transport");}
    }
    if(readbackAvailable)for(int slot=0;slot<2;++slot)if(!readSlots[slot].fence){
        for(int eye=0;eye<2;++eye){bindFBO(GL_FRAMEBUFFER,eyeFBO[eye]);glReadBuffer(GL_COLOR_ATTACHMENT0);
            readBindBuffer(GL_PIXEL_PACK_BUFFER,readSlots[slot].buffers[eye]);ReadTransport(eyeWidth,eyeHeight,NULL);}
        memcpy(readSlots[slot].views,tracking.views,sizeof(tracking.views));readSlots[slot].sequence=++readSequence;
        readSlots[slot].fence=readFence(GL_SYNC_GPU_COMMANDS_COMPLETE,0);glFlush();
        if(!readSlots[slot].fence){readbackAvailable=0;LOG_WARN("Async readback fence creation failed");}break;
    }
    readBindBuffer(GL_PIXEL_PACK_BUFFER,previous);bindFBO(GL_FRAMEBUFFER,fbo);glReadBuffer(readBuffer);
    return readbackAvailable;
}

static void PublishStereo(void){
    double started=PerfNow();
    if(!GPUPublish(shared,eyeFBO,eyeWidth,eyeHeight,tracking.views,renderedTrackingSerial,&imageSerial) && !PublishAsyncStereo()){
    GLint fbo,readBuffer;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);glGetIntegerv(GL_READ_BUFFER,&readBuffer);
    for(int eye=0;eye<2;++eye){bindFBO(GL_FRAMEBUFFER,eyeFBO[eye]);glReadBuffer(GL_COLOR_ATTACHMENT0);ReadTransport(eyeWidth,eyeHeight,eyePixels[eye]);}
    bindFBO(GL_FRAMEBUFFER,fbo);glReadBuffer(readBuffer);
    if(Bridge_TryLock(&shared->frameLock)){
        shared->frame.mode=BRIDGE_MODE_STEREO;shared->frame.width=eyeWidth;shared->frame.height=eyeHeight;
        memcpy(shared->frame.views,tracking.views,sizeof(tracking.views));
        for(int eye=0;eye<2;++eye)memcpy(shared->pixels[eye],eyePixels[eye],eyeWidth*eyeHeight*4);
        shared->frame.serial=++imageSerial;Bridge_Unlock(&shared->frameLock);
    }
    }
    perfRead+=PerfNow()-started;
    if(++perfSamples>=180){LOG_INFO("PERF average-ms eyes=%.2f readback-copy=%.2f desktop-submit=%.2f samples=%u",perfEyes/perfSamples,perfRead/perfSamples,perfDesktop/perfSamples,perfSamples);perfSamples=0;perfEyes=perfRead=perfDesktop=0;}
    static DWORD lastHUD;if(tracePoses && GetTickCount()-lastHUD>1000){LOG_INFO("HUD replay batches=%u",hudDraws);lastHUD=GetTickCount();}
}

static void CapturePanel(HDC dc) {
    RECT rect; HWND window=WindowFromDC(dc);
    if (!HelperAlive() || !window || !GetClientRect(window,&rect)) return;
    int width=rect.right-rect.left, height=rect.bottom-rect.top;
    if (width<1 || height<1 || width>8192 || height>8192) return;
    size_t bytes=(size_t)width*height*4;
    if (bytes>desktopCapacity) {
        unsigned char *resized=realloc(desktopPixels,bytes);
        if (!resized) return;
        desktopPixels=resized; desktopCapacity=bytes;
    }
    GLint previousBuffer; glGetIntegerv(GL_READ_BUFFER,&previousBuffer);
    glReadBuffer(GL_BACK); ReadTransport(width,height,desktopPixels); glReadBuffer(previousBuffer);
    unsigned w=width,h=height;
    if (w>BRIDGE_MAX_WIDTH || h>BRIDGE_MAX_HEIGHT) {
        float ratio=fminf((float)BRIDGE_MAX_WIDTH/w,(float)BRIDGE_MAX_HEIGHT/h);
        w=(unsigned)(w*ratio); h=(unsigned)(h*ratio);
    }
    if (!Bridge_TryLock(&shared->frameLock)) return;
    if (w==(unsigned)width && h==(unsigned)height) memcpy(shared->pixels[0],desktopPixels,bytes);
    else for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x)
        memcpy(shared->pixels[0]+(y*w+x)*4,desktopPixels+((size_t)(y*height/h)*width+x*width/w)*4,4);
    shared->frame.mode=BRIDGE_MODE_PANEL; shared->frame.width=w; shared->frame.height=h;
    shared->frame.serial=++imageSerial;
    Bridge_Unlock(&shared->frameLock);
    if (imageSerial==1) LOG_INFO("First game frame sent to x64 helper: %ux%u",w,h);
}

/* Runs on the same game thread as SwapBuffers. No process-global keyboard or
 * mouse injection. Synthetic key IDs preserve the player's physical keys. */
static void SetControl(int index,int down) {
    static const char *names[]={"attack","forward","back","moveleft","moveright","moveup","movedown","use","reload","altattack","speed"};
    down=!!down;if(heldControls[index]==down)return;
    char command[80];snprintf(command,sizeof(command),"\n%c%s %d 0\n",down?'+':'-',names[index],1000+index);
    addCommand(command);heldControls[index]=down;
    LOG_INFO("CONTROL %c%s",down?'+':'-',names[index]);

}
#include "vr_locomotion.h"
static void PublishWheel(void){
    static int slowed;
    static float normalTime;
    if(nativeCvarSet && cvarFloat && slowed!=weaponWheel.open){
        char value[32];
        if(weaponWheel.open)normalTime=cvarFloat("timescale");
        float scale=isfinite(normalTime) && normalTime>0?normalTime:1;
        snprintf(value,sizeof(value),"%.8g",weaponWheel.open?scale*.35f:scale);
        nativeCvarSet("timescale",value,0);
        slowed=weaponWheel.open;
        LOG_INFO("WHEEL timescale %s",value);
    }
    if(!shared || !Bridge_TryLock(&shared->wheelLock))return;
    BridgeWheel next={0};next.serial=++wheelSerial;next.open=weaponWheel.open;next.page=weaponWheel.page;
    next.pages=weaponWheel.pages;next.hover=weaponWheel.hover;next.sampleTimeMs=GetTickCount();
    next.point[0]=weaponWheel.point[0];next.point[1]=weaponWheel.point[1];
    next.count=Wheel_PageItems(&weaponWheel,next.items);shared->wheel=next;Bridge_Unlock(&shared->wheelLock);
}
static void WheelHaptic(float amplitude,unsigned duration){
    if(shared && Bridge_TryLock(&shared->hapticLock)){
        shared->haptic=(BridgeHaptic){++hapticSerial,leftHanded?0u:1u,amplitude*accessibility.haptics*.01f,duration,0};Bridge_Unlock(&shared->hapticLock);
    }
}
static int WheelHasSave(void){
    char path[MAX_PATH];DWORD n=GetModuleFileNameA(NULL,path,sizeof(path));if(!n || n>=sizeof(path))return 0;
    char *slash=strrchr(path,'\\');if(!slash || (size_t)(slash-path)+25>=sizeof(path))return 0;
    strcpy(slash+1,"base\\save\\*.sof2");WIN32_FIND_DATAA data;HANDLE find=FindFirstFileA(path,&data);if(find==INVALID_HANDLE_VALUE)return 0;
    int exists=0;do{if(!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && (data.nFileSizeHigh || data.nFileSizeLow)){exists=1;break;}}while(FindNextFileA(find,&data));FindClose(find);return exists;
}
static void HolsterPreferences(int saveSlot){
    static int loaded;char path[MAX_PATH];GetModuleFileNameA(selfModule,path,sizeof(path));char *slash=strrchr(path,'\\');if(!slash)return;
    strcpy(slash+1,"sof2-vr-holsters.ini");
    for(int slot=1;slot<VR_HOLSTER_COUNT;++slot){if(slot==VR_HOLSTER_AMMO)continue;char key[16];snprintf(key,sizeof(key),"Slot%d",slot);
        if(!loaded)holsters.selected[slot]=GetPrivateProfileIntA("Weapons",key,0,path);
        if(saveSlot==slot){char value[16];snprintf(value,sizeof(value),"%d",holsters.selected[slot]);WritePrivateProfileStringA("Weapons",key,value,path);}
    }loaded=1;
}
#include "vr_holster_controls.h"
static int UpdatePump(const BridgeTracking *sample,int valid){
    static int lastWeapon,lastClip;VRWeaponState state;int current=GameVR_GetWeaponState(&state);
    const VRWeaponProfile *profile=current?VR_Profile(state.weapon):NULL;
    if(current && state.weapon!=lastWeapon){memset(&pump,0,sizeof(pump));lastWeapon=state.weapon;lastClip=state.clip;}
    if(current && profile && profile->pump && state.clip<lastClip){VR_PumpShot(&pump);LOG_INFO("PUMP required weapon=%d clip=%d",state.weapon,state.clip);}
    if(current)lastClip=state.clip;
    int enabled=manualPump && valid && current && profile && profile->pump && loadingLive && !shellLoad.stage;
    const BridgeHand *gun=&sample->input.hands[leftHanded?0:1],*hand=&sample->input.hands[leftHanded?1:0];
    int tracked=(hand->valid&3)==3 && (gun->valid&3)==3,grip=hand->squeeze>(pump.held?GripOff():GripOn()),trigger=hand->trigger>TriggerOn();
    float origin[3],axis[3][3],port[3],foreend[3],chest[3],point[3],dominant[3],unused[3][3],along=0;int inReach=0;
    if(enabled && tracked && LoadingGeometry(&playerBase,sample,origin,axis,port,foreend,chest)){
        PoseInGame(&playerBase,&hand->grip,point,unused);
        PoseInGame(&playerBase,&gun->grip,dominant,unused);
        const VRWeaponGrip *rig=WeaponGrip(state.weapon);
        inReach=weaponSupported && Distance3(point,foreend)<5.5f;
        tracked=rig && rig->supportAuthored;
        if(tracked){
            /* Remove the fitted transverse offset from the inter-hand
             * distance. Rotating the shotgun then cannot drive the pump. */
            float axialSquared=0;
            for(int i=0;i<3;++i){float d=point[i]-dominant[i];axialSquared+=d*d;}
            for(int i=1;i<3;++i){float d=rig->support[i]-rig->point[i];axialSquared-=d*d;}
            along=sqrtf(fmaxf(0,axialSquared));
        }
    }else tracked=0;
    int held=pump.held;int completed=VR_PumpUpdate(&pump,enabled && tracked,grip,trigger,inReach,along,M590_PUMP_TRAVEL);
    if(!held && pump.held)LOG_INFO("PUMP grabbed weapon=%d",state.weapon);
    if(completed){LOG_INFO("PUMP complete weapon=%d",state.weapon);ReloadHaptic(.6f,45);}
    pumpFireBlocked=enabled && (pump.needed || pump.held);return pumpFireBlocked;
}
static int UpdateLoading(const BridgeTracking *sample,int valid,int fallback){
    VRWeaponState state;int current=GameVR_GetWeaponState(&state);
    int supported=manualReloadEnabled && current && state.weapon==6 && loadingLive;
    loadingBlocked=0;
    if(!supported || shellLoad.weapon!=state.weapon || shellLoad.epoch!=reloadEpoch || fallback){
        if(shellLoad.stage==2)GameVR_CancelRoundInsertionFor(6);
        memset(&shellLoad,0,sizeof(shellLoad));shellLoad.weapon=supported?6:0;shellLoad.epoch=reloadEpoch;
        if(fallback && supported)memset(&pump,0,sizeof(pump));
    }
    if(!supported)return 0;
    const BridgeHand *hand=&sample->input.hands[leftHanded?1:0];
    float origin[3],axis[3][3],port[3],foreend[3],chest[3],point[3],unused[3][3];
    int tracked=valid && playerScene && GetTickCount()-playerBaseTime<250 && (hand->valid&3)==3 && (state.state==0 || state.state==27) && LoadingGeometry(&playerBase,sample,origin,axis,port,foreend,chest);
    if(!tracked || fallback){
        if(shellLoad.stage==2)GameVR_CancelRoundInsertionFor(6);shellLoad.stage=shellLoad.armed=0;shellLoad.gripBefore=1;shellLoad.seatingSince=0;
        GameVR_SetReloadControl(6,manualPump && pump.needed,fallback);return 0;
    }
    PoseInGame(&playerBase,&hand->grip,point,unused);
    if(shellLoad.stage==2){
        int result=GameVR_ReloadResult();
        if(!result){GameVR_SetReloadControl(6,1,0);loadingBlocked=1;return 1;}
        if(result>0 && !shellLoad.clip && manualPump)VR_PumpShot(&pump);
        LOG_INFO("SHELL committed=%d clip=%d reserve=%d",result,state.clip,state.reserve);
        ReloadHaptic(result>0?.65f:.2f,result>0?45:90);shellLoad.stage=0;shellLoad.armed=0;shellLoad.gripBefore=1;
    }
    int grip=hand->squeeze>(shellLoad.gripBefore?GripOff():GripOn());
    if(!grip)shellLoad.armed=1;
    int press=grip && !shellLoad.gripBefore && shellLoad.armed;shellLoad.gripBefore=grip;
    if(!shellLoad.stage && press && state.clip<state.capacity && state.reserve>0 && Distance3(point,chest)<14 && Distance3(point,foreend)>6 && Distance3(point,port)>5){
        shellLoad.stage=1;shellLoad.armed=0;shellLoad.clip=state.clip;shellLoad.reserve=state.reserve;ReloadHaptic(.3f,25);LOG_INFO("SHELL acquired at chest clip=%d reserve=%d",state.clip,state.reserve);
    }
    if(shellLoad.stage==1){
        if(!grip || state.clip!=shellLoad.clip || state.reserve!=shellLoad.reserve){shellLoad.stage=0;shellLoad.seatingSince=0;}
        else if(Distance3(point,port)<4.0f){
            if(!shellLoad.seatingSince)shellLoad.seatingSince=GetTickCount();
            if(GetTickCount()-shellLoad.seatingSince>=100){
                if(GameVR_InsertRounds(6,state.clip,state.reserve,1)){shellLoad.stage=2;ReloadHaptic(.45f,30);LOG_INFO("SHELL native insertion requested");}
                else shellLoad.stage=0;
                shellLoad.seatingSince=0;
            }
        }else shellLoad.seatingSince=0;
    }
    loadingBlocked=shellLoad.stage!=0 || (manualPump && (pump.needed || pump.held));
    GameVR_SetReloadControl(6,loadingBlocked,0);return loadingBlocked;
}
static void TestWorldToTracking(const float point[3],float out[3]);
static void TestHolsters(BridgeTracking *sample,int playable,DWORD now){
    char flag[8];static DWORD begun;static int done,last=-1,observed;
    static const int slots[]={VR_HOLSTER_PISTOL,VR_HOLSTER_SMG,VR_HOLSTER_PRIMARY,VR_HOLSTER_HEAVY,VR_HOLSTER_GRENADE,VR_HOLSTER_KNIFE};
    static const int expected[]={3,8,9,13,18,1};
    if(!tracePoses || !GetEnvironmentVariableA("SOF2_VR_TEST_HOLSTERS",flag,sizeof(flag)))return;
    if(!playable || done)return;
    if(!begun){addCommand("\ngod\ngive all\n");begun=now;return;}
    if(now-begun<4500)return;
    unsigned elapsed=now-begun-4500,step=elapsed/7000,phase=elapsed%7000;
    if(step>=6){done=1;LOG_INFO("HOLSTERTEST complete");addCommand("\nquit\n");return;}
    int slot=slots[step],id=expected[step],dom=leftHanded?0:1,off=!dom;
    if(last!=(int)step){last=(int)step;observed=0;LOG_INFO("HOLSTERTEST begin slot=%d expected=%d left=%d",slot,id,leftHanded);}
    for(int h=0;h<2;++h){sample->input.hands[h].buttons=0;sample->input.hands[h].trigger=sample->input.hands[h].squeeze=0;sample->input.hands[h].stick[0]=sample->input.hands[h].stick[1]=0;}
    BridgeHand *hand=&sample->input.hands[dom];float centre[3],q[4],inv[4],local[3],delta[3];BodyFrame(sample,centre,q,inv);
    if((phase>=600 && phase<2200) || (phase>=5700 && phase<5860))
        VR_HolsterPositionAccessible(slot,leftHanded,accessibility.holsterReach*.01f,accessibility.holsterHeight*.01f,accessibility.seated,local);
    else if(phase>=2850 && phase<3250)
        VR_HolsterPositionAccessible(VR_HOLSTER_GRENADE,leftHanded,accessibility.holsterReach*.01f,accessibility.holsterHeight*.01f,accessibility.seated,local);
    else {local[0]=leftHanded?-.23f:.23f;local[1]=-.35f;local[2]=-.45f;}
    Rotate(q,local,delta);for(int i=0;i<3;++i)hand->grip.position[i]=centre[i]+delta[i];
    hand->valid=3;hand->squeeze=(phase>=600 && phase<5000) || (phase>=5700 && phase<6800)?.9f:0;
    if(phase>=1050 && phase<2200 && weaponWheel.open){
        WheelItem items[WHEEL_PAGE_SIZE];int count=Wheel_PageItems(&weaponWheel,items);
        for(int i=0;i<count;++i)if(items[i].id==id){float x,y;Wheel_Slot(i,count,&x,&y);sample->input.hands[1].stick[0]=x;sample->input.hands[1].stick[1]=-y;}
        if(phase>=1400 && phase<1650)hand->trigger=.9f;
    }
    memcpy(hand->aim.position,hand->grip.position,12);memcpy(hand->aim.orientation,hand->grip.orientation,16);
    if(phase>=2300 && phase<4800 && weaponHeld){
        const VRWeaponGrip *rig=WeaponGrip(id);float point[3],controller[3][3],axis[3][3],support[3];PoseInGame(&playerBase,&hand->aim,point,controller);
        if(rig && rig->supportAuthored && id!=1 && id!=18){
            for(int j=0;j<3;++j)for(int i=0;i<3;++i){axis[j][i]=0;for(int k=0;k<3;++k)axis[j][i]+=controller[k][i]*rig->basis[j][k]*(leftHanded && k==1?-1:1);}
            for(int i=0;i<3;++i){support[i]=point[i];for(int k=0;k<3;++k)support[i]+=axis[k][i]*(rig->support[k]-rig->point[k]);}
            GamePointToTracking(&playerBase,support,sample->input.hands[off].grip.position);sample->input.hands[off].valid=3;sample->input.hands[off].squeeze=.9f;
        }
    }
    if(phase>=3200 && !(observed&8)){
        observed|=8;LOG_INFO("HOLSTERTEST carried past slot=%d wheel=%d",slot,weaponWheel.open);
    }
    if(phase>=4000 && !(observed&1)){VRWeaponState state={0};GameVR_GetWeaponState(&state);observed|=1;
        LOG_INFO("HOLSTERTEST held slot=%d expected=%d weapon=%d held=%d supported=%d pending=%d",slot,id,state.weapon,weaponHeld,weaponSupported,GameVR_WeaponRequestPending());addCommand("\nr_screenshot\n");}
    if(phase>=5350 && !(observed&2)){observed|=2;LOG_INFO("HOLSTERTEST dropped slot=%d weapon=%d held=%d ground=%d",slot,id,weaponHeld,WeaponOnGround(id));}
    if(phase>=6500 && !(observed&4)){VRWeaponState state={0};GameVR_GetWeaponState(&state);observed|=4;
        LOG_INFO("HOLSTERTEST recovered slot=%d expected=%d weapon=%d held=%d ground=%d",slot,id,state.weapon,weaponHeld,WeaponOnGround(id));}
    memcpy(testReloadHands,sample->input.hands,sizeof(testReloadHands));testGripAuditActive=1;testGripAuditWeapon=id;
}
static void TestKnifeReach(BridgeTracking *sample,int playable,DWORD now){
    char flag[8];static DWORD began;static unsigned noted;static int done;
    if(!tracePoses || !GetEnvironmentVariableA("SOF2_VR_TEST_KNIFE_REACH",flag,sizeof(flag)) || !playable || done)return;
    if(!began){began=now;addCommand("\ngod\ngive all\n");return;}if(now-began<4500)return;
    unsigned phase=now-began-4500;int dom=leftHanded?0:1,off=!dom;
    for(int h=0;h<2;++h){sample->input.hands[h].valid=3;sample->input.hands[h].squeeze=sample->input.hands[h].trigger=0;sample->input.hands[h].buttons=0;sample->input.hands[h].stick[0]=sample->input.hands[h].stick[1]=0;}
    BridgeHand *hand=&sample->input.hands[dom];float centre[3],q[4],inv[4],local[3],delta[3];BodyFrame(sample,centre,q,inv);
    if(phase>=600 && phase<2200)VR_HolsterPositionAccessible(VR_HOLSTER_PRIMARY,leftHanded,accessibility.holsterReach*.01f,accessibility.holsterHeight*.01f,accessibility.seated,local);
    else if(phase>=4000 && phase<4700)VR_HolsterPositionAccessible(VR_HOLSTER_KNIFE,leftHanded,accessibility.holsterReach*.01f,accessibility.holsterHeight*.01f,accessibility.seated,local);
    else{local[0]=leftHanded?-.23f:.23f;local[1]=-.35f;local[2]=-.45f;}
    Rotate(q,local,delta);for(int k=0;k<3;++k)hand->grip.position[k]=centre[k]+delta[k];hand->squeeze=phase>=600?.9f:0;
    if(phase>=1050 && phase<2200 && weaponWheel.open){WheelItem items[WHEEL_PAGE_SIZE];int count=Wheel_PageItems(&weaponWheel,items);
        for(int i=0;i<count;++i)if(items[i].id==9){float x,y;Wheel_Slot(i,count,&x,&y);sample->input.hands[1].stick[0]=x;sample->input.hands[1].stick[1]=-y;}
        if(phase>=1400 && phase<1650)hand->trigger=.9f;}
    if((phase>=6000 && phase<6250) || (phase>=7000 && phase<7250))hand->trigger=.9f;
    memcpy(hand->aim.position,hand->grip.position,12);memcpy(hand->aim.orientation,hand->grip.orientation,16);
    VRWeaponState state={0};GameVR_GetWeaponState(&state);
    unsigned marks[]={3700,5800,6500,7500};
    for(int i=0;i<4;++i)if(phase>=marks[i] && !(noted&(1u<<i))){noted|=1u<<i;LOG_INFO("KNIFEREACH step=%d weapon=%d held=%d reverse=%d dominant=%d continuous-grip=1",i,state.weapon,weaponHeld,knifeReverse,dom);
        if(i>=2)addCommand("\nr_screenshot\n");}
    memcpy(testReloadHands,sample->input.hands,sizeof(testReloadHands));testGripAuditActive=1;testGripAuditWeapon=state.weapon;
    if(phase>=7900){done=1;LOG_INFO("KNIFEREACH complete");addCommand("\nquit\n");}(void)off;
}
static void TestPumpStroke(BridgeTracking *sample,unsigned phase,int dom,int off){
    const VRWeaponGrip *rig=WeaponGrip(6);
    if(!rig)return;
    BridgeHand *main=&sample->input.hands[dom],*support=&sample->input.hands[off];
    main->squeeze=.9f;
    main->trigger=((phase>=350 && phase<1200) || (phase>=3550 && phase<4300))?.9f:0;
    support->squeeze=phase>=100?.9f:0;
    support->trigger=phase>=1600 && phase<3250?.9f:0;
    float back=fmaxf(0,fminf(1,fminf((phase-1750.f)/450,(2950.f-phase)/500)));
    float local[3],grip[3],axis[3][3],point[3];
    PoseInGame(&playerBase,&main->grip,grip,axis);
    for(int k=0;k<3;++k){
        local[k]=0;
        for(int j=0;j<3;++j){
            float offset=rig->support[j]-rig->point[j]-(j==0?back*M590_PUMP_TRAVEL:0);
            local[k]+=offset*rig->basis[j][k];
        }
    }
    if(leftHanded)local[1]=-local[1];
    float steer=back*.18f;
    float forward=local[0];local[0]=forward*cosf(steer)-local[1]*sinf(steer);
    local[1]=forward*sinf(steer)+local[1]*cosf(steer);
    for(int i=0;i<3;++i){point[i]=grip[i];for(int k=0;k<3;++k)point[i]+=axis[k][i]*local[k];}
    GamePointToTracking(&playerBase,point,support->grip.position);support->aim=support->grip;
}
static void TestHandling(BridgeTracking *sample,int playable,DWORD now){
    char flag[8];static DWORD start,pumpStart;static unsigned seen;
    if(!tracePoses || !GetEnvironmentVariableA("SOF2_VR_TEST_HANDLING",flag,sizeof(flag)) || !playable || (seen&8))return;
    if(!start){start=now;addCommand("\ngod\ngive all\n");return;}
    if(now-start<2600)return;
    unsigned phase=now-start-2600;int dom=leftHanded?0:1,off=1-dom;
    int testPump=GetEnvironmentVariableA("SOF2_VR_TEST_PUMP_STROKE",flag,sizeof(flag))!=0;
    for(int h=0;h<2;++h){
        BridgeHand *hand=&sample->input.hands[h];
        memset(hand,0,sizeof(*hand));hand->valid=3;hand->grip.orientation[3]=1;
    }
    if(!seen){
        seen=1;
        holsters.selected[VR_HOLSTER_PRIMARY]=6;
        holsters.selected[VR_HOLSTER_PISTOL]=3;
    }
    float centre[3],q[4],inv[4];BodyFrame(sample,centre,q,inv);
    float poses[2][3];
    for(int h=0;h<2;++h){
        poses[h][0]=h?.24f:-.24f;poses[h][1]=-.24f;poses[h][2]=-.53f;
    }
    if(!testPump && phase>=540 && phase<690){
        VR_HolsterPositionAccessible(VR_HOLSTER_PRIMARY,leftHanded,1,0,0,poses[dom]);
        poses[dom][1]+=.22f;
    }else if(!testPump && phase>=2850 && phase<3000){
        VR_HolsterPositionAccessible(VR_HOLSTER_PISTOL,leftHanded,1,0,0,poses[dom]);
    }
    sample->input.hands[dom].squeeze=(testPump || (phase>=390 && phase<2350) || phase>=2850)?.9f:0;
    if(phase>=4770 && phase<5010)VR_HolsterPositionAccessible(VR_HOLSTER_KNIFE,leftHanded,1,0,0,poses[off]);
    if(phase>=4800 && phase<7700)sample->input.hands[off].squeeze=.9f;
    if(phase>=5650 && phase<6100)poses[off][0]+=(phase-5650)*.00026f;
    if(phase>=6300 && phase<6970)sample->input.hands[dom].trigger=.9f;
    if(phase>=7170 && phase<7470)sample->input.hands[off].trigger=.9f;
    if(phase>=7860)poses[off][2]=-.86f;
    for(int h=0;h<2;++h){
        BridgeHand *hand=&sample->input.hands[h];float delta[3];Rotate(q,poses[h],delta);
        for(int k=0;k<3;++k)hand->grip.position[k]=centre[k]+delta[k];
        hand->aim=hand->grip;
    }
    VRWeaponState state={0};GameVR_GetWeaponState(&state);
    if(testPump){
        if(!(seen&16) && GameVR_RequestWeapon(6))seen|=16;
        if(!pumpStart && state.weapon==6 && state.state==0 && !GameVR_WeaponRequestPending()){
            pumpStart=now;weaponHeld=1;
        }
        unsigned stroke=pumpStart?now-pumpStart:0;
        TestPumpStroke(sample,stroke,dom,off);
        if(stroke>=2300 && !(seen&4)){
            seen|=4;LOG_INFO("PUMPTEST rear weapon=%d supported=%d held=%d pull=%.3f",state.weapon,weaponSupported,pump.held,pump.pull);
        }
        memcpy(testReloadHands,sample->input.hands,sizeof(testReloadHands));testGripAuditActive=1;testGripAuditWeapon=state.weapon;
        if(stroke>=4700){seen|=8;LOG_INFO("PUMPTEST complete");addCommand("\nquit\n");}
        return;
    }
    if(phase>=1950 && !(seen&2)){seen|=2;LOG_INFO("HANDLING shotgun weapon=%d held=%d",state.weapon,weaponHeld);}
    if(phase>=7530 && !(seen&4)){
        seen|=4;LOG_INFO("HANDLING dual weapon=%d held=%d knife=%d reverse=%d",state.weapon,weaponHeld,offhandKnife,knifeReverse);
    }
    memcpy(testReloadHands,sample->input.hands,sizeof(testReloadHands));testGripAuditActive=1;testGripAuditWeapon=state.weapon;
    if(phase>=9700){seen|=8;LOG_INFO("HANDLING complete");addCommand("\nquit\n");}
}
static void TestArsenal(BridgeTracking *sample,int playable,DWORD now){
    static int ids[8],count,index,requested,initialized,done;static DWORD grantAt,readyAt;
    char text[96];if(!tracePoses || !GetEnvironmentVariableA("SOF2_VR_TEST_ARSENAL",text,sizeof(text)))return;
    for(int h=0;h<2;++h){sample->input.hands[h].trigger=sample->input.hands[h].squeeze=0;sample->input.hands[h].buttons=0;sample->input.hands[h].stick[0]=sample->input.hands[h].stick[1]=0;}
    char audit[8];int allSides=GetEnvironmentVariableA("SOF2_VR_TEST_ALL_SIDES",audit,sizeof(audit))>0;
    if(allSides){
        /* Keep the diagnostic holder closed between native selections: a
         * synthetic release would drop the old gun and cancel the request.
         * Repositioning for screenshots must not become a knife attack. */
        sample->input.hands[leftHanded?0:1].squeeze=.9f;
        ResetKnifeMotion();holsters.held=holsters.slot=holsters.wheel=holsters.armed=0;
    }
    if(!playable || done)return;
    if(!initialized){char *p=text;while(*p && count<8){char *end;int id=(int)strtol(p,&end,10);if(end==p)break;const VRWeaponProfile *profile=VR_Profile(id);if(profile && profile->category>=1 && profile->category<=8)ids[count++]=id;p=end;if(*p==',')++p;else break;}initialized=1;}
    if(!count)return;
    if(!grantAt){addCommand("\ngod\ngive all\nwpn_list\n");grantAt=now;return;}
    if(now-grantAt<2500)return;
    if(!requested){requested=GameVR_RequestWeapon(ids[index]);readyAt=0;if(requested)LOG_INFO("ARSENAL request id=%d",ids[index]);return;}
    VRWeaponState state;if(!GameVR_GetWeaponState(&state) || state.weapon!=ids[index])return;
    if(!readyAt){if(state.state!=0 && state.state!=27)return;readyAt=now;LOG_INFO("ARSENAL begin id=%d clip=%d reserve=%d",state.weapon,state.clip,state.reserve);}
    DWORD elapsed=now-readyAt;int dom=leftHanded?0:1,off=!dom;
    if(GetEnvironmentVariableA("SOF2_VR_TEST_GRIP_AUDIT",audit,sizeof(audit))){
        const VRWeaponGrip *rig=WeaponGrip(state.weapon);if(!rig)return;
        for(int h=0;h<2;++h){BridgeHand *hand=&sample->input.hands[h];hand->valid=3;hand->buttons=0;hand->trigger=hand->squeeze=0;memset(hand->grip.orientation,0,16);hand->grip.orientation[3]=1;memcpy(hand->aim.orientation,hand->grip.orientation,16);}
        BridgeHand *main=&sample->input.hands[dom];main->grip.position[0]=leftHanded?-.2f:.2f;main->grip.position[1]=1.46f;main->grip.position[2]=-.4f;memcpy(main->aim.position,main->grip.position,12);
        if(allSides){
            /* Presentation fixture, not an equip/input test: native requests
             * choose the inventory item; pin possession for orbit pictures.
             * Holster input behavior is covered by TestHolsters separately. */
            weaponHeld=1;main->squeeze=.9f;
            int stage=(int)(elapsed/750);testArsenalView=stage?((stage-1)%6):-1;
            static const float rotations[6][4]={
                {0,.70710678f,0,.70710678f},{0,-.70710678f,0,.70710678f},
                {.70710678f,0,0,.70710678f},{-.70710678f,0,0,.70710678f},
                {0,1,0,0},{0,0,0,1}};
            if(testArsenalView>=0){memcpy(main->grip.orientation,rotations[testArsenalView],16);memcpy(main->aim.orientation,main->grip.orientation,16);}
        }
        float grip[3],axis[3][3],controllerAxis[3][3],origin[3],point[3];
        /* Fixture geometry must not consume/reset the real support-grip
         * edge before the authored squeeze is published to gameplay. */
        PoseInGame(&playerBase,&main->grip,grip,controllerAxis);
        for(int j=0;j<3;++j)for(int i=0;i<3;++i){axis[j][i]=0;for(int k=0;k<3;++k)axis[j][i]+=controllerAxis[k][i]*rig->basis[j][k]*(leftHanded && k==1?-1:1);}
        WeaponEntityOrigin(state.weapon,grip,axis,origin);
        if(allSides){
            const VRDropProfile *bounds=DropProfile(state.weapon);
            if(bounds){
                float centre[3],trackCentre[3];
                for(int i=0;i<3;++i){centre[i]=origin[i];for(int k=0;k<3;++k)centre[i]+=axis[k][i]*bounds->centre[k];}
                TestWorldToTracking(centre,trackCentre);
                float radius=hypotf(hypotf(bounds->half[0],bounds->half[1]),bounds->half[2])/39.37f;
                char close[8];int closeView=GetEnvironmentVariableA("SOF2_VR_TEST_CLOSE",close,sizeof(close))>0;
                float target[3]={0,1.57f,closeView?-fmaxf(.42f,radius*1.22f):-fmaxf(.70f,radius*1.65f)};
                for(int i=0;i<3;++i)main->grip.position[i]+=target[i]-trackCentre[i];
                memcpy(main->aim.position,main->grip.position,12);
                PoseInGame(&playerBase,&main->grip,grip,controllerAxis);
                WeaponEntityOrigin(state.weapon,grip,axis,origin);
            }
        }
        for(int i=0;i<3;++i){point[i]=origin[i];for(int k=0;k<3;++k)point[i]+=axis[k][i]*rig->support[k];}
        TestWorldToTracking(point,sample->input.hands[off].grip.position);memcpy(sample->input.hands[off].aim.position,sample->input.hands[off].grip.position,12);
        sample->input.hands[off].squeeze=elapsed>=(allSides?650:1700)?.9f:0;
        memcpy(testReloadHands,sample->input.hands,sizeof(testReloadHands));testGripAuditActive=1;testGripAuditWeapon=state.weapon;
    }else{
    sample->input.hands[dom].trigger=elapsed>=600 && elapsed<1000?.9f:0;
    sample->input.hands[off].trigger=elapsed>=1600 && elapsed<1800?.9f:0;
    sample->input.hands[off].buttons=elapsed>=2600 && elapsed<2750?BRIDGE_BUTTON_PRIMARY:0;
    char nativeReload[8];static int insertedIndex=-1;
    if(GetEnvironmentVariableA("SOF2_VR_TEST_NATIVE_RELOAD",nativeReload,sizeof(nativeReload))){
        sample->input.hands[off].buttons=0;sample->input.hands[off].trigger=0;
        if(elapsed>=2600 && insertedIndex!=index){int rounds=state.capacity-state.clip;if(rounds>state.reserve)rounds=state.reserve;
            if(rounds>0 && GameVR_InsertRounds(state.weapon,state.clip,state.reserve,rounds)){insertedIndex=index;LOG_INFO("TEST manual native transfer requested id=%d",state.weapon);}}
        sample->input.hands[dom].trigger=(elapsed>=600 && elapsed<1000) || (elapsed>=3200 && elapsed<3400)?.9f:0;
    }
    }
    if(elapsed>=(allSides?5250u:3800u)){LOG_INFO("ARSENAL end id=%d clip=%d reserve=%d",state.weapon,state.clip,state.reserve);requested=0;readyAt=0;if(++index>=count){done=1;LOG_INFO("ARSENAL complete count=%d",count);if(testGripAuditActive)addCommand("\nquit\n");}}
}
static void TestWorldToTracking(const float point[3],float out[3]){
    float v[3]={0},q[4],delta[3];for(int i=0;i<3;++i)delta[i]=(point[i]-playerBase.origin[i])/39.37f;
    for(int i=0;i<3;++i){v[0]-=delta[i]*playerBase.axis[1][i];v[1]+=delta[i]*playerBase.axis[2][i];v[2]-=delta[i]*playerBase.axis[0][i];q[i]=-referenceInverse[i];}q[3]=referenceInverse[3];
    Rotate(q,v,out);for(int i=0;i<3;++i)out[i]+=referencePosition[i];
}
/* Explicit fixture-only controller stimulus. Same state machine, hit regions,
 * rendering and native transaction as physical input; no direct gesture calls. */
static void TestHandAxesToTracking(const float world[3][3],float q[4]){
    float a[3][3],inverse[4];for(int k=0;k<3;++k)inverse[k]=-referenceInverse[k];inverse[3]=referenceInverse[3];
    for(int j=0;j<3;++j){float local[3]={0};for(int k=0;k<3;++k){local[0]-=world[j][k]*playerBase.axis[1][k];local[1]+=world[j][k]*playerBase.axis[2][k];local[2]-=world[j][k]*playerBase.axis[0][k];}Rotate(inverse,local,a[j]);}
    float m[3][3];for(int k=0;k<3;++k){m[k][0]=-a[1][k];m[k][1]=a[2][k];m[k][2]=-a[0][k];}
    float trace=m[0][0]+m[1][1]+m[2][2];
    if(trace>0){float s=sqrtf(trace+1)*2;q[3]=s*.25f;q[0]=(m[2][1]-m[1][2])/s;q[1]=(m[0][2]-m[2][0])/s;q[2]=(m[1][0]-m[0][1])/s;}
    else{int i=m[1][1]>m[0][0]?1:0;if(m[2][2]>m[i][i])i=2;int j=(i+1)%3,k=(i+2)%3;float s=sqrtf(fmaxf(.00001f,1+m[i][i]-m[j][j]-m[k][k]))*2;q[i]=s*.25f;q[j]=(m[j][i]+m[i][j])/s;q[k]=(m[k][i]+m[i][k])/s;q[3]=(m[k][j]-m[j][k])/s;}
}
static void TestManualReload(BridgeTracking *sample,int playable,DWORD now){
    char mode[32];if(!tracePoses || !GetEnvironmentVariableA("SOF2_VR_TEST_MANUAL_RELOAD",mode,sizeof(mode)))return;
    static DWORD grantAt,readyAt,emptyAt,shotAt;static int requested,done;
    char weaponText[12];int target=GetEnvironmentVariableA("SOF2_VR_TEST_RELOAD_WEAPON",weaponText,sizeof(weaponText))?atoi(weaponText):9;
    if(!ReloadParts(target))return;
    int dom=leftHanded?0:1,off=!dom;
    for(int h=0;h<2;++h){BridgeHand *hand=&sample->input.hands[h];hand->valid=3;hand->trigger=hand->squeeze=0;hand->buttons=0;hand->stick[0]=hand->stick[1]=0;
        memset(hand->grip.orientation,0,16);hand->grip.orientation[3]=1;memcpy(hand->aim.orientation,hand->grip.orientation,16);}
    sample->input.hands[dom].grip.position[0]=leftHanded?-.2f:.2f;sample->input.hands[dom].grip.position[1]=1.46f;sample->input.hands[dom].grip.position[2]=-.4f;
    memcpy(sample->input.hands[dom].aim.position,sample->input.hands[dom].grip.position,12);
    if(!playable || done)goto publish;
    if(!grantAt){addCommand("\ngod\ngive all\n");grantAt=now;goto publish;}if(now-grantAt<2500)goto publish;
    if(!requested){requested=GameVR_RequestWeapon(target);goto publish;}
    VRWeaponState state;if(!GameVR_GetWeaponState(&state))goto publish;
    if(state.weapon!=target){if(!_stricmp(mode,"switch") && state.weapon==2 && !magazine.stage){LOG_INFO("MANUALTEST end mode=switch stage=0 clip=%d reserve=%d weapon=%d",state.clip,state.reserve,target);done=1;}goto publish;}
    /* This fixture isolates magazine/action inputs after selection. Like the
     * arsenal inspector it pins possession explicitly; holster acquisition
     * itself is exercised by the separate physical-input holster fixture. */
    weaponHeld=1;sample->input.hands[dom].squeeze=.9f;
    if(!readyAt){if(state.state!=0 && state.state!=27)return;readyAt=now;LOG_INFO("MANUALTEST begin mode=%s clip=%d reserve=%d weapon=%d",mode,state.clip,state.reserve,target);LOG_INFO("MANUALTEST poseSource=interaction-render-shot");}
    if(!_stricmp(mode,"rack")){
        float origin[3],axis[3][3],socket[3],bolt[3],pouch[3],point[3];
        if(!ReloadGeometry(&playerBase,sample,origin,axis,socket,bolt,pouch))goto publish;
        unsigned elapsed=now-readyAt,phase=elapsed%800;int action=phase>=100 && phase<650;
        float pull=phase>=250 && phase<450?1.05f:0;
        for(int i=0;i<3;++i)point[i]=bolt[i]-axis[0][i]*ReloadActionTravel(target)*pull;
        sample->input.hands[off].trigger=action?.9f:0;TestWorldToTracking(point,sample->input.hands[off].grip.position);memcpy(sample->input.hands[off].aim.position,sample->input.hands[off].grip.position,12);
        if((!state.clip && phase>650) || elapsed>32000){LOG_INFO("MANUALTEST end mode=rack stage=%d clip=%d reserve=%d weapon=%d",magazine.stage,state.clip,state.reserve,target);done=1;addCommand("\nquit\n");}
        goto publish;
    }
    const VRWeaponProfile *testProfile=VR_Profile(target);int pistol=testProfile && testProfile->category==2;
    DWORD rawElapsed=now-readyAt,elapsed=rawElapsed;int emptyTest=!_stricmp(mode,"empty") || !_stricmp(mode,"bolt_loss") || !_stricmp(mode,"slide_loss") || !_stricmp(mode,"pause_empty");
    int depleting=emptyTest && !emptyAt;
    if(depleting && !state.clip){emptyAt=now;depleting=0;}
    if(emptyTest)elapsed=emptyAt?1000+now-emptyAt:1000;
    /* Asset registration can stall a frame past a timed trigger window.
     * Begin the reload only after a native shot actually consumed a round. */
    int tacticalStart=!emptyTest && _stricmp(mode,"full");
    if(tacticalStart && !shotAt && state.clip<state.capacity)shotAt=now;
    if(tacticalStart)elapsed=shotAt?600+now-shotAt:300;
    float origin[3],axis[3][3],socket[3],bolt[3],pouch[3],point[3];if(!ReloadGeometry(&playerBase,sample,origin,axis,socket,bolt,pouch))goto publish;
    memcpy(point,socket,12);sample->input.hands[dom].trigger=(depleting || (tacticalStart && !shotAt)) && rawElapsed%360<260?.9f:0;
    if(!depleting && _stricmp(mode,"pause_empty") && elapsed>=1400 && elapsed<1600)sample->input.hands[dom].buttons=BRIDGE_BUTTON_PRIMARY;
    int grip=0;
    if(!_stricmp(mode,"pause_empty") && emptyAt && now-emptyAt>=400 && !testManualPauseAt){testManualPauseAt=now;MenuKey(27);LOG_INFO("MANUALTEST pause queued clip=%d reserve=%d",state.clip,state.reserve);}
    static int saved,loaded,switched;
    if(!_stricmp(mode,"quickload") && elapsed>=1000 && !saved){saved=1;addCommand("\nquicksave\n");LOG_INFO("MANUALTEST save queued");}
    if(!_stricmp(mode,"quickload") && elapsed>=2000 && !loaded){loaded=1;addCommand("\nquickload\n");LOG_INFO("MANUALTEST load queued");}
    if(!_stricmp(mode,"switch") && elapsed>=2000 && !switched){switched=1;GameVR_RequestWeapon(2);}
    if(elapsed>=1400 && elapsed<2500){grip=1;if(elapsed>=1800)for(int i=0;i<3;++i)point[i]-=axis[2][i]*6;}
    if(elapsed>=2200 && elapsed<3100){memcpy(point,pouch,12);grip=elapsed<2500 || elapsed>=2800;}
    if(elapsed>=3100 && elapsed<3800){grip=1;if(elapsed<3400)for(int i=0;i<3;++i)point[i]-=axis[2][i]*6;}
    const VRMagazineInteraction *testInteraction=MagazineInteraction(target);
    if(testInteraction && elapsed>=3100 && elapsed<3800){float handAxes[3][3],desired[3];
        for(int k=0;k<3;++k)for(int i=0;i<3;++i){handAxes[k][i]=0;for(int j=0;j<3;++j)handAxes[k][i]+=testInteraction->holdBasis[off][j][k]*axis[j][i];}
        TestHandAxesToTracking(handAxes,sample->input.hands[off].grip.orientation);memcpy(sample->input.hands[off].aim.orientation,sample->input.hands[off].grip.orientation,16);
        for(int i=0;i<3;++i){desired[i]=socket[i];for(int k=0;k<3;++k)desired[i]+=axis[k][i]*(testInteraction->mouth[k]-testInteraction->feed[k]+testInteraction->up[k]*(elapsed<3400?-5:.8f));
            point[i]=desired[i];for(int k=0;k<3;++k)point[i]-=handAxes[k][i]*testInteraction->holdOrigin[off][k];}
    }
    int action=emptyTest && elapsed>=4100 && elapsed<4800;
    if(action){memcpy(point,bolt,12);grip=elapsed<4700;if(elapsed>=4350)for(int i=0;i<3;++i)point[i]-=axis[0][i]*ReloadActionTravel(target)*1.05f;
        if(pistol && _stricmp(mode,"bolt_loss")){grip=0;if(elapsed>=4200 && elapsed<4400)sample->input.hands[dom].buttons=BRIDGE_BUTTON_SECONDARY;}}
    if(elapsed>=(emptyTest?4900:4300) && elapsed<(emptyTest?5100:4500))sample->input.hands[dom].trigger=.9f;
    if(!_stricmp(mode,"quickload") && elapsed>=2000)grip=0;
    if(!_stricmp(mode,"fallback")){grip=0;if(elapsed>=1400 && elapsed<1550)sample->input.hands[off].buttons=BRIDGE_BUTTON_PRIMARY;}
    if(!_stricmp(mode,"earlyfire") && elapsed>=1900 && elapsed<3300)sample->input.hands[dom].trigger=.9f;
    if(!_stricmp(mode,"wrongangle") && elapsed>=3400 && elapsed<4200){sample->input.hands[off].grip.orientation[0]=1;sample->input.hands[off].grip.orientation[3]=0;}
    if(!_stricmp(mode,"pause_empty")){grip=0;sample->input.hands[dom].trigger=depleting && rawElapsed%360<260?.9f:0;memcpy(point,pouch,12);}
    if(!_stricmp(mode,"quickload") && loaded){grip=0;sample->input.hands[dom].trigger=0;memcpy(point,pouch,12);}
    if(elapsed>=5500){LOG_INFO("MANUALTEST end mode=%s stage=%d clip=%d reserve=%d weapon=%d",mode,magazine.stage,state.clip,state.reserve,target);done=1;
        char finite[8];if(GetEnvironmentVariableA("SOF2_VR_TEST_MANUAL_FINITE",finite,sizeof(finite)))addCommand("\nquit\n");}
    sample->input.hands[off].squeeze=action?0:(grip?.9f:0);sample->input.hands[off].trigger=action && grip?.9f:0;TestWorldToTracking(point,sample->input.hands[off].grip.position);memcpy(sample->input.hands[off].aim.position,sample->input.hands[off].grip.position,12);
    if(!_stricmp(mode,"loss") && elapsed>=1950 && elapsed<2150)sample->input.hands[off].valid=0;
    if(!_stricmp(mode,"bolt_loss") && elapsed>=4400 && elapsed<4600)sample->input.hands[off].valid=0;
    if(!_stricmp(mode,"slide_loss") && elapsed>=4100 && elapsed<4800)sample->input.hands[off].valid=0;
    if(!_stricmp(mode,"cancel") && elapsed>=1800 && elapsed<2200){sample->input.hands[off].squeeze=0;TestWorldToTracking(socket,sample->input.hands[off].grip.position);}
 publish:
    memcpy(testReloadHands,sample->input.hands,sizeof(testReloadHands));testReloadActive=1;
}
static int RestoreDirectSoundFocus(void){
    BYTE *sound=(BYTE *)GetModuleHandleW(L"snd_dsx86.dll");
    void **api=*(void ***)(gameBase+0x1477f4);
    if(!sound || !api || api!=(void **)(sound+0x3d550))return 0;
    /* Verified GetSndAPI v8 slots: restore the existing DirectSound
     * cooperative level and unmute the mixer. Keep sound/model registries. */
    if(api[2]!=sound+0x6c30 || api[10]!=sound+0x1b10 || !sound[0x3d5d8] || !*(void **)(sound+0x57439c))return 0;
    typedef void (__cdecl *SoundFn)(void);
    ((SoundFn)api[2])();
    ((SoundFn)api[10])();
    return 1;
}
static void UpdateVRAudio(int focused,DWORD now){
    static DWORD readyAt,checkedAt,attemptedAt;
    static GUID playback;
    static int known,started,pending=1;
    HWND window=WindowFromDC(wglGetCurrentDC());
    if(!focused || !window || GetForegroundWindow()!=GetAncestor(window,GA_ROOT)){
        readyAt=0;
        return;
    }
    if(!readyAt)readyAt=now;
    if(!checkedAt || now-checkedAt>=1000){
        checkedAt=now;
        HMODULE sound=GetModuleHandleW(L"dsound.dll");
        typedef HRESULT (WINAPI *DeviceIDFn)(LPCGUID,LPGUID);
        DeviceIDFn deviceId=sound?(DeviceIDFn)GetProcAddress(sound,"GetDeviceID"):NULL;
        GUID current;
        if(deviceId && SUCCEEDED(deviceId(&DSDEVID_DefaultPlayback,&current))){
            if(known && memcmp(&playback,&current,sizeof(current))){
                readyAt=now;
                pending=1;
            }
            playback=current;known=1;
        }
    }
    if(pending && now-readyAt>=900 && (!attemptedAt || now-attemptedAt>=1000)){
        attemptedAt=now;
        if(RestoreDirectSoundFocus()){
            LOG_INFO("VR audio refreshed native playback reason=%s",started?"default-device-changed":"startup");
            started=1;pending=0;
        }
    }
}
static void UpdateControls(void) {
    static int altArmed,centerLatched;
    static unsigned domButtonsBefore=~0u,offButtonsBefore=~0u;
    static DWORD centerHold;
    static int activeBefore,triggerArmed,lastPaused=-1,menuTriggerArmed,menuBackHeld,wheelMenuPending;
    static DWORD lastTrace;
    static BridgeTracking sample;DWORD now=GetTickCount();
    if(!addCommand)return;
    if(testManualPauseAt && !testManualPauseResumed){
        if(!testManualPauseSampled && now-testManualPauseAt>700){VRWeaponState expired;testManualPauseSampled=1;LOG_INFO("MANUALTEST pause sampleExpired=%d",!GameVR_GetWeaponState(&expired));}
        if(now-testManualPauseAt>1300){testManualPauseResumed=1;MenuKey(27);LOG_INFO("MANUALTEST pause resumed");}
    }
    if(shared && Bridge_TryLock(&shared->trackingLock)){
        sample=shared->tracking;Bridge_Unlock(&shared->trackingLock);
    }
    BridgeInput *input=&sample.input;
    int paused=cvarFloat("cl_paused")!=0;
    if(paused!=lastPaused){LOG_INFO("INPUT game paused=%d",paused);lastPaused=paused;}
    int nativeReady=now-lastNativeTime<250;
    unsigned catcher=*(unsigned *)(gameBase+0x1476d4);
    int inMenu=(catcher&2)!=0;
    /* Verified native Key_Event routes state 9 to CIN_StopCinematic.
     * Cinematics do not render the player scene, so nativeReady is false. */
    int cinematic=*(unsigned *)(gameBase+0x1476d0)==9;
    int valid=controlsEnabled && HelperAlive() && shared->helperStatus==2 && input->focused && sample.valid &&
        input->serial && now-input->sampleTimeMs<250 && (nativeReady || paused || inMenu || cinematic);
    UpdateVRAudio(valid && !cinematic,now);
    if(!valid){
        wheelMenuPending=0;
        UpdateOffhandKnife(&sample,0);
        ResetKnifeMotion();
        int unused;UpdateWheel(&sample,0,&unused);UpdatePump(&sample,0);UpdateMagazine(&sample,0,0);UpdateLoading(&sample,0,0);
        for(int i=0;i<CONTROL_COUNT;++i)SetControl(i,0);
        triggerArmed=0;turnLatched=1;menuHeld=1;menuTriggerArmed=0;menuBackHeld=0;altArmed=0;offButtonsBefore=domButtonsBefore=~0u;
        if(activeBefore)LOG_INFO("INPUT inactive: all controller commands released");
        activeBefore=0;return;
    }
    if(inputSerial==input->serial)return;
    inputSerial=input->serial;
    if(!activeBefore){LOG_INFO("INPUT active: controller commands enabled; dominant=%s",leftHanded?"left":"right");addCommand("\nset cg_drawCrosshair 0\nset cg_bobup 0\nset cg_bobpitch 0\nset cg_bobroll 0\nset cg_runpitch 0\nset cg_runroll 0\n");}
    activeBefore=1;
    /* Test stimuli must enter before button-edge detection, just like the
     * actual OpenXR samples. Otherwise A/B/X/Y fixture presses disappear. */
    TestHolsters(&sample,nativeReady && !paused && !catcher && playerScene,now);
    TestKnifeReach(&sample,nativeReady && !paused && !catcher && playerScene,now);
    TestHandling(&sample,nativeReady && !paused && !catcher && playerScene,now);
    TestArsenal(&sample,nativeReady && !paused && !catcher && playerScene,now);
    TestManualReload(&sample,nativeReady && !paused && !catcher && playerScene,now);
    BridgeHand *dom=&input->hands[leftHanded?0:1];
    BridgeHand *off=&input->hands[leftHanded?1:0];
    unsigned domPressed=dom->buttons&~domButtonsBefore,offPressed=off->buttons&~offButtonsBefore;
    static int loggedGrip;
    int gripNow=dom->squeeze>(loggedGrip?.35f:.65f);
    if(gripNow!=loggedGrip || (offPressed&BRIDGE_BUTTON_SECONDARY)){
        LOG_INFO("INPUT controls-v2 dominant-grip=%.2f off-grip=%.2f buttons=%x,%x wheel=%d",dom->squeeze,off->squeeze,dom->buttons,off->buttons,weaponWheel.open);
        loggedGrip=gripNow;
    }
    domButtonsBefore=dom->buttons;offButtonsBefore=off->buttons;
    if(cinematic){
        UpdateOffhandKnife(&sample,0);
        for(int i=0;i<CONTROL_COUNT;++i)SetControl(i,0);
        int unused;UpdateWheel(&sample,0,&unused);UpdatePump(&sample,0);UpdateMagazine(&sample,0,0);UpdateLoading(&sample,0,0);
        ResetKnifeMotion();
        if(dom->trigger<.2f)menuTriggerArmed=1;
        int menu=(input->hands[0].buttons&BRIDGE_BUTTON_MENU)!=0;
        if((menu && !menuHeld) || ((domPressed|offPressed)&(BRIDGE_BUTTON_PRIMARY|BRIDGE_BUTTON_SECONDARY)) ||
           (dom->trigger>TriggerOn() && menuTriggerArmed)){
            MenuKey(27);menuTriggerArmed=0;LOG_INFO("CINEMATIC controller skip request");
        }
        menuHeld=menu;triggerArmed=altArmed=0;return;
    }
    int menu=(input->hands[0].buttons&BRIDGE_BUTTON_MENU)!=0;
    int menuEdge=menu && !menuHeld;
    if(menuEdge){wheelMenuPending=0;MenuKey(27);LOG_INFO("CONTROL pause request");}
    menuHeld=menu;
    /* Grip owns the VR wheel even if a native gameplay page is still open.
     * menuoff is the native unconditional close command; Escape can instead
     * open the pause page when two overlapping pages close on different ticks.
     * Do not dismiss the main menu or an explicit pause with a grip. */
    static DWORD wheelMenuCloseAt;
    int wheelGrip=!weaponHeld && (dom->valid&BRIDGE_HAND_GRIP_VALID) && dom->squeeze>GripOn() &&
        (weaponWheel.open || (holsters.held && holsters.slot!=VR_HOLSTER_KNIFE && !weaponWheel.used) || (!weaponHeld && NearHolster(&sample)));
    if(dom->squeeze<GripOff())wheelMenuPending=0;
    if(!menuEdge && wheelGrip && (GameVR_NativeInventoryOpen() ||
       NativeGameplayPageOpen() || (inMenu && nativeReady && playerScene) || (inMenu && weaponWheel.open))){
        GameVR_CloseNativeInventory();
        ResetKnifeMotion();
        wheelMenuPending=1;
        if(!wheelMenuCloseAt || now-wheelMenuCloseAt>250){
            addCommand("\nmenuoff\n");wheelMenuCloseAt=now;
            LOG_INFO("WHEEL closing native gameplay pages before grip selection");
        }
        if(!weaponWheel.open){weaponWheel.gripArmed=1;weaponWheel.used=0;}
        for(int i=0;i<CONTROL_COUNT;++i)SetControl(i,0);
        triggerArmed=altArmed=0;return;
    }
    if(!inMenu)wheelMenuCloseAt=0;
    int center=(input->hands[0].valid&BRIDGE_HAND_GRIP_VALID) && (input->hands[1].valid&BRIDGE_HAND_GRIP_VALID) &&
        (input->hands[0].buttons&BRIDGE_BUTTON_STICK) && (input->hands[1].buttons&BRIDGE_BUTTON_STICK);
    if(!center){centerHold=0;centerLatched=0;}
    else if(!centerHold)centerHold=now;
    else if(!centerLatched && now-centerHold>1000){recentered=0;centerLatched=1;LOG_INFO("CONTROL recenter: both sticks held");}
    if(inMenu && !menuEdge){
        UpdateOffhandKnife(&sample,0);
        ResetKnifeMotion();
        int unused;UpdateWheel(&sample,0,&unused);UpdatePump(&sample,0);UpdateMagazine(&sample,0,0);UpdateLoading(&sample,0,0);
        for(int i=0;i<CONTROL_COUNT;++i)SetControl(i,0);
        triggerArmed=0;turnLatched=1;altArmed=0;
        int pointerValid=PointMenu(&sample);
        int tracked=(dom->valid&BRIDGE_HAND_AIM_VALID)!=0;
        if(tracked && dom->trigger<0.2f)menuTriggerArmed=1;
        if(tracked && pointerValid && dom->trigger>TriggerOn() && menuTriggerArmed){MenuKey(10);menuTriggerArmed=0;}
        int back=(tracked && (dom->buttons&BRIDGE_BUTTON_SECONDARY)) || ((off->valid&BRIDGE_HAND_GRIP_VALID) && (off->buttons&BRIDGE_BUTTON_SECONDARY));
        if(back && !menuBackHeld)MenuKey(27);menuBackHeld=back;
        return;
    }
    menuTriggerArmed=0;menuBackHeld=0;
    int play=nativeReady && !paused && !menuEdge && !catcher;
    /* A grip gesture owns the wheel. Never interpret a simultaneous face
     * action (including a stale runtime binding) as an objectives request. */
    if(play && !weaponHeld && (!holsters.held || weaponWheel.used || holsters.slot==VR_HOLSTER_KNIFE) && !weaponWheel.open &&
       (off->valid&BRIDGE_HAND_GRIP_VALID) && (offPressed&BRIDGE_BUTTON_SECONDARY)){
        LOG_INFO("CONTROL journal: offhand Y edge; dominant grip=%.2f offhand grip=%.2f",dom->squeeze,off->squeeze);
        addCommand("\nmenu objective\n");menuBackHeld=1;for(int i=0;i<CONTROL_COUNT;++i)SetControl(i,0);
        int unused;UpdateWheel(&sample,0,&unused);UpdatePump(&sample,0);UpdateMagazine(&sample,0,0);UpdateLoading(&sample,0,0);return;
    }
    static int testInventory;
    char testWheel[8];if(play && playerScene && tracePoses && !testInventory && GetEnvironmentVariableA("SOF2_VR_SIM_WHEEL",testWheel,sizeof(testWheel))){testInventory=1;addCommand("\ngive all\nwpn_list\n");LOG_INFO("TEST wheel inventory grant queued");}
    static int testWeapon;static DWORD testWeaponAt;
    char weaponTest[12];
    if(tracePoses && play && playerScene && GetEnvironmentVariableA("SOF2_VR_TEST_WEAPON",weaponTest,sizeof(weaponTest))){
        if(!testWeaponAt){addCommand("\ngod\ngive all\n");testWeaponAt=now;}
        else if(!testWeapon && now-testWeaponAt>2500){testWeapon=atoi(weaponTest);GameVR_RequestWeapon(testWeapon);LOG_INFO("TEST selected weapon=%d",testWeapon);}
    }
    int weaponsReady=GameVR_Ready();
    int domValid=(dom->valid&3)==3;
    int tapReload=0;
    if(wheelMenuPending && play && domValid && playerScene && now-playerBaseTime<250){
        weaponWheel.gripArmed=1;weaponWheel.used=0;wheelMenuPending=0;
    }
    UpdateOffhandKnife(&sample,play && playerScene && now-playerBaseTime<250);
    if(UpdateWheel(&sample,play && domValid && playerScene && now-playerBaseTime<250,&tapReload)){
        ResetKnifeMotion();
        UpdatePump(&sample,0);UpdateMagazine(&sample,0,0);UpdateLoading(&sample,0,0);
        SetControl(0,0);SetControl(8,0);SetControl(9,0);
        SetControl(7,play && (off->valid&3)==3 && (off->buttons&BRIDGE_BUTTON_PRIMARY));
        UpdateLocomotion(&sample,play,now);triggerArmed=altArmed=0;return;
    }
    int weaponPending=GameVR_WeaponRequestPending();
    int pumpBlocked=UpdatePump(&sample,play && domValid && weaponHeld);
    VRWeaponState reloadChoice={0};int physicalMagazine=manualReloadEnabled && GameVR_GetWeaponState(&reloadChoice) && ReloadParts(reloadChoice.weapon);
    int reloadFallback=0; /* X is use; no face button silently runs stock reload. */
    reloadEjectRequest=physicalMagazine && (domPressed&BRIDGE_BUTTON_PRIMARY);
    reloadSlideRequest=(domPressed&BRIDGE_BUTTON_SECONDARY)!=0;
    int reloadBlocked=UpdateMagazine(&sample,play && domValid && weaponHeld && !weaponPending && !offhandKnife,reloadFallback);
    int shellBlocked=UpdateLoading(&sample,play && domValid && weaponHeld && !weaponPending,reloadFallback);reloadBlocked|=shellBlocked;
    if(!domValid || !play || reloadBlocked){
        if(triggerArmed && !domValid)LOG_INFO("INPUT dominant tracking lost: trigger disarmed");
        triggerArmed=0;
    } else if(dom->trigger<0.2f && !triggerArmed){
        triggerArmed=1;LOG_INFO("INPUT trigger rearmed after release");
    }
    VRWeaponState heldState;int knife=GameVR_GetWeaponState(&heldState) && heldState.weapon==1;
    static int knifeFlipArmed,knifeHandBefore=-1;
    int knifeHand=offhandKnife?(leftHanded?1:0):(knife && weaponHeld?(leftHanded?0:1):-1);
    if(knifeHand!=knifeHandBefore){knifeFlipArmed=0;ResetKnifeMotion();knifeHandBefore=knifeHand;}
    const BridgeHand *bladeHand=knifeHand>=0?&input->hands[knifeHand]:NULL;
    if(!bladeHand || !play || (bladeHand->valid&3)!=3)knifeFlipArmed=0;
    else if(bladeHand->trigger<.2f)knifeFlipArmed=1;
    else if(bladeHand->trigger>TriggerOn() && knifeFlipArmed){knifeFlipArmed=0;knifeReverse=!knifeReverse;ResetKnifeMotion();LOG_INFO("KNIFE grip reverse=%d hand=%d",knifeReverse,knifeHand);}
    UpdateKnifeContact(&sample,bladeHand && play && playerScene && (offhandKnife || (!weaponPending && !reloadBlocked)));
    SetControl(0,play && weaponHeld && !knife && !weaponPending && !pumpBlocked && !reloadBlocked && weaponsReady && domValid && triggerArmed && dom->trigger>TriggerOn());
    /* Face buttons are reserved for magazine and slide actions. */
    int offValid=play && domValid && (off->valid&3)==3;
    int offButtons=offValid?(int)off->buttons:0;
    SetControl(7,offValid && (offButtons&BRIDGE_BUTTON_PRIMARY));
    SetControl(8,play && weaponHeld && domValid && !physicalMagazine && !(manualReloadEnabled && reloadChoice.weapon==6) && (dom->buttons&BRIDGE_BUTTON_PRIMARY));
    VRWeaponState activeWeapon={0};const VRWeaponProfile *activeProfile=GameVR_GetWeaponState(&activeWeapon)?VR_Profile(activeWeapon.weapon):NULL;
    int charging=magazine.stage==VR_MAG_BOLT || magazine.boltHeld;
    if(offValid && ReloadParts(activeWeapon.weapon)){
        float gun[3],basis[3][3],socket[3],bolt[3],pouch[3],point[3],unused[3][3];
        if(ReloadGeometry(&playerBase,&sample,gun,basis,socket,bolt,pouch)){
            PoseInGame(&playerBase,&off->grip,point,unused);
            charging|=Distance3(point,bolt)<5.5f;
        }
    }
    /* The trigger grabs the charging action before it can fire a launcher.
     * It must be released after leaving the action before alt fire rearms. */
    if(!offValid || reloadBlocked || charging)altArmed=0;
    else if(off->trigger<.2f)altArmed=1;
    int launcherSupport=!activeProfile || (activeWeapon.weapon!=9 && activeWeapon.weapon!=11) || weaponSupported;
    int pumpChord=manualPump && activeProfile && activeProfile->pump && off->squeeze>.35f;
    SetControl(9,offValid && !offhandKnife && weaponHeld && !weaponPending && !reloadBlocked && !pumpChord && !charging && launcherSupport && weaponsReady && altArmed && off->trigger>TriggerOn());
    UpdateLocomotion(&sample,play,now);
    if(tracePoses && now-lastTrace>=1000){
        LOG_INFO("HANDS serial=%u valid=%u,%u left=%.3f,%.3f,%.3f right=%.3f,%.3f,%.3f leftQ=%.4f,%.4f,%.4f,%.4f rightQ=%.4f,%.4f,%.4f,%.4f",
            input->serial,input->hands[0].valid,input->hands[1].valid,
            input->hands[0].grip.position[0],input->hands[0].grip.position[1],input->hands[0].grip.position[2],
            input->hands[1].grip.position[0],input->hands[1].grip.position[1],input->hands[1].grip.position[2],
            input->hands[0].grip.orientation[0],input->hands[0].grip.orientation[1],input->hands[0].grip.orientation[2],input->hands[0].grip.orientation[3],
            input->hands[1].grip.orientation[0],input->hands[1].grip.orientation[1],input->hands[1].grip.orientation[2],input->hands[1].grip.orientation[3]);
        lastTrace=now;
    }
}

#include "vr_native_options.h"
static BOOL WINAPI BridgeSwap(HDC dc) {
    static int recenterDown;
    static int hookWarningLogged;
    static DWORD gameHookCheck;
    if(originalScene && GetTickCount()-gameHookCheck>1000){gameHookCheck=GetTickCount();GameVR_Install(ControllerShotPose,ControllerEffectPose,controlsEnabled);}
    if (!bridgeAttempted && wglGetCurrentContext()) StartBridge();
    if(sceneSlot && *sceneSlot!=(DWORD)(ULONG_PTR)BridgeScene)originalScene=NULL;
    if(!originalScene && GetTickCount()-lastHookCheck>=1000){
        lastHookCheck=GetTickCount();
        if(!InstallSceneHook() && !hookWarningLogged){LOG_WARN("No verified scene hook; headset panel capture only");hookWarningLogged=1;}
    }
    int down=(GetAsyncKeyState(VK_F12)&0x8000)!=0;
    if(down && !recenterDown && GetForegroundWindow()==WindowFromDC(dc))recentered=0;
    recenterDown=down;
    static int pacingSet;static HGLRC pacingContext;
    if(addCommand && !pacingSet){addCommand("\nset com_maxfps 0\nset r_swapInterval 0\nset r_finish 0\nset r_fastsky 0\nset cg_drawCrosshair 0\n");pacingSet=1;LOG_INFO("PACING engine FPS cap removed; desktop VSync disabled");}
    if(wglGetCurrentContext()!=pacingContext){
        typedef BOOL (WINAPI *IntervalFn)(int);IntervalFn interval=(IntervalFn)wglGetProcAddress("wglSwapIntervalEXT");
        if(interval){interval(0);pacingContext=wglGetCurrentContext();}
    }
    EnsureNativeGLDispatch();
    UpdateNativeVROptions();
    UpdateControls();
    if(testQuitDeadline && addCommand && (LONG)(GetTickCount()-testQuitDeadline)>=0){
        testQuitDeadline=0;addCommand("\nquit\n");LOG_INFO("TEST queued normal engine quit");
    }
    if(nativeSinceSwap && nativeRendered)CompositeHud();
    if(nativeSinceSwap && eyeFBO[0] && desktopMirror && blitFBO && gameBase && !*(unsigned *)(gameBase+0x1476d4)){
        RECT rect;HWND window=WindowFromDC(dc);
        if(window && GetClientRect(window,&rect)){
            GLint read,draw;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);
            glPushAttrib(GL_SCISSOR_BIT|GL_COLOR_BUFFER_BIT);glDisable(GL_SCISSOR_TEST);
            bindFBO(GL_READ_FRAMEBUFFER,eyeFBO[0]);bindFBO(GL_DRAW_FRAMEBUFFER,0);glReadBuffer(GL_COLOR_ATTACHMENT0);glDrawBuffer(GL_BACK);
            blitFBO(0,0,eyeWidth,eyeHeight,0,0,rect.right,rect.bottom,GL_COLOR_BUFFER_BIT,GL_LINEAR);
            bindFBO(GL_READ_FRAMEBUFFER,read);bindFBO(GL_DRAW_FRAMEBUFFER,draw);glPopAttrib();
        }
    }
    if (!nativeSinceSwap || (gameBase && (*(unsigned *)(gameBase+0x1476d4)&2)))CapturePanel(dc);
    else if(nativeRendered)PublishStereo();
    nativeSinceSwap=0;nativeRendered=0;nativeSkyCount=0;
    return realSwap ? realSwap(dc) : FALSE;
}

static void InstallSwapHook(void) {
    BYTE *base=(BYTE *)GetModuleHandleA(NULL);
    PIMAGE_DOS_HEADER dos=(PIMAGE_DOS_HEADER)base;
    PIMAGE_NT_HEADERS nt=(PIMAGE_NT_HEADERS)(base+dos->e_lfanew);
    IMAGE_DATA_DIRECTORY directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    PIMAGE_IMPORT_DESCRIPTOR imports=(PIMAGE_IMPORT_DESCRIPTOR)(base+directory.VirtualAddress);
    if (!directory.VirtualAddress) return;
    for (;imports->Name;++imports) {
        if (_stricmp((char *)(base+imports->Name),"gdi32.dll") || !imports->OriginalFirstThunk) continue;
        PIMAGE_THUNK_DATA name=(PIMAGE_THUNK_DATA)(base+imports->OriginalFirstThunk);
        PIMAGE_THUNK_DATA target=(PIMAGE_THUNK_DATA)(base+imports->FirstThunk);
        for (;name->u1.AddressOfData;++name,++target) {
            int swap=IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal)
                ? IMAGE_ORDINAL(name->u1.Ordinal)==516
                : !strcmp((char *)((PIMAGE_IMPORT_BY_NAME)(base+name->u1.AddressOfData))->Name,"SwapBuffers");
            if (!swap) continue;
            DWORD protect;
            if (!VirtualProtect(&target->u1.Function,sizeof(target->u1.Function),PAGE_READWRITE,&protect)) return;
            realSwap=(SwapFn)(ULONG_PTR)target->u1.Function;
            target->u1.Function=(ULONG_PTR)BridgeSwap;
            VirtualProtect(&target->u1.Function,sizeof(target->u1.Function),protect,&protect);
            return;
        }
    }
}

BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID reserved) {
    (void)reserved;
    if (reason==DLL_PROCESS_ATTACH) {
        selfModule=module; DisableThreadLibraryCalls(module);
        char mode[16];
        flatMode=GetEnvironmentVariableA("SOF2_VR_MODE",mode,sizeof(mode))==4 && !_stricmp(mode,"flat");
        if(!flatMode)InstallSwapHook();
        LOG_INFO("Launcher mode: %s",flatMode?"flatscreen (system OpenGL only)":"VR");
        if(!flatMode)LOG_INFO("SoF2 x86 bridge proxy loaded; OpenXR belongs to separate x64 helper");
    } else if (reason==DLL_PROCESS_DETACH) {
        /* No GL/OpenXR teardown or process waits under the loader lock. */
        if (shared) { InterlockedExchange(&shared->shutdown,1); UnmapViewOfFile(shared); }
        if (mapping) CloseHandle(mapping);
        if (helperProcess) CloseHandle(helperProcess);
    }
    return TRUE;
}


