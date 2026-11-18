/* GPU-only x86/x64 transport using D3D11 shared textures and NV interop2.
 * Included once per process. No OpenXR handles cross the process boundary.
 * Render/copy completion is polled with zero-timeout GL fences. */
#ifndef SOF2_GPU_TRANSPORT_H
#define SOF2_GPU_TRANSPORT_H
#define COBJMACROS
#include <d3d11.h>
#include <dxgi.h>
#include <GL/wglext.h>
typedef struct {
    ID3D11Texture2D *dx[2];GLuint texture[2];HANDLE object[2];GLsync fence;
    int locked;
} GPUImages;
static ID3D11Device *gpuDevice;
static ID3D11DeviceContext *gpuDXContext;
static HANDLE gpuInterop;
static HGLRC gpuGLContext;
static GPUImages gpuImages[BRIDGE_GPU_SLOTS];
static PFNWGLDXOPENDEVICENVPROC gpuOpen;
static PFNWGLDXREGISTEROBJECTNVPROC gpuRegister;
static PFNWGLDXLOCKOBJECTSNVPROC gpuLock;
static PFNWGLDXUNLOCKOBJECTSNVPROC gpuUnlock;
static PFNGLFENCESYNCPROC gpuFence;
static PFNGLCLIENTWAITSYNCPROC gpuWait;
static PFNGLDELETESYNCPROC gpuDeleteFence;
static int gpuConsumerSlot=-1;
static int GPUFunctions(void){
    if(gpuInterop)return gpuGLContext==wglGetCurrentContext();
    gpuOpen=(void *)wglGetProcAddress("wglDXOpenDeviceNV");
    gpuRegister=(void *)wglGetProcAddress("wglDXRegisterObjectNV");
    gpuLock=(void *)wglGetProcAddress("wglDXLockObjectsNV");
    gpuUnlock=(void *)wglGetProcAddress("wglDXUnlockObjectsNV");
    gpuFence=(void *)wglGetProcAddress("glFenceSync");
    gpuWait=(void *)wglGetProcAddress("glClientWaitSync");
    gpuDeleteFence=(void *)wglGetProcAddress("glDeleteSync");
    if(!gpuOpen || !gpuRegister || !gpuLock || !gpuUnlock || !gpuFence || !gpuWait || !gpuDeleteFence)return 0;
    const char *extensions=NULL;
    PFNWGLGETEXTENSIONSSTRINGARBPROC getExtensions=(void *)wglGetProcAddress("wglGetExtensionsStringARB");
    if(getExtensions)extensions=getExtensions(wglGetCurrentDC());
    if(!extensions || !strstr(extensions,"WGL_NV_DX_interop2"))return 0;
    if(FAILED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
       NULL,0,D3D11_SDK_VERSION,&gpuDevice,NULL,&gpuDXContext)))return 0;
    gpuInterop=gpuOpen(gpuDevice);gpuGLContext=wglGetCurrentContext();return gpuInterop!=NULL;
}
static int GPUFenceReady(GPUImages *images){
    if(!images->fence)return 1;
    GLenum result=gpuWait(images->fence,0,0);
    if(result!=GL_ALREADY_SIGNALED && result!=GL_CONDITION_SATISFIED)return result==GL_WAIT_FAILED?-1:0;
    gpuDeleteFence(images->fence);images->fence=NULL;return 1;
}
#ifdef SOF2_GPU_PRODUCER
static GLuint gpuCopyFBO;
static int GPUCreateProducer(BridgeShared *mail,unsigned width,unsigned height){
    if(!blitFBO || !GPUFunctions())return 0;
    GLint bound;glGetIntegerv(GL_TEXTURE_BINDING_2D,&bound);
    for(int slot=0;slot<BRIDGE_GPU_SLOTS;++slot){GPUImages *images=&gpuImages[slot];glGenTextures(2,images->texture);
        for(int eye=0;eye<2;++eye){
            D3D11_TEXTURE2D_DESC desc={0};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
            desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
            desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
            if(FAILED(ID3D11Device_CreateTexture2D(gpuDevice,&desc,NULL,&images->dx[eye])))goto fail;
            IDXGIResource *resource=NULL;HANDLE handle=NULL;
            if(FAILED(ID3D11Texture2D_QueryInterface(images->dx[eye],&IID_IDXGIResource,(void **)&resource)))goto fail;
            HRESULT result=IDXGIResource_GetSharedHandle(resource,&handle);IDXGIResource_Release(resource);
            if(FAILED(result) || !handle)goto fail;
            images->object[eye]=gpuRegister(gpuInterop,images->dx[eye],images->texture[eye],GL_TEXTURE_2D,WGL_ACCESS_WRITE_DISCARD_NV);
            if(!images->object[eye])goto fail;
            mail->gpu[slot].handles[eye]=(uint64_t)(uintptr_t)handle;
        }
    }
    genFBO(1,&gpuCopyFBO);glBindTexture(GL_TEXTURE_2D,bound);
    ID3D11DeviceContext_Flush(gpuDXContext);InterlockedExchange(&mail->gpuStatus,1);return gpuCopyFBO!=0;
fail:
    glBindTexture(GL_TEXTURE_2D,bound);return 0;
}
static int GPUPublish(BridgeShared *mail,GLuint eyes[2],unsigned width,unsigned height,const BridgeView views[2],unsigned prediction,DWORD *serial){
    static int attempted;static unsigned queuedPrediction;
    if(!attempted){attempted=1;
        char disabled[8];
        if(GetEnvironmentVariableA("SOF2_VR_CPU_TRANSPORT",disabled,sizeof(disabled)) || !GPUCreateProducer(mail,width,height)){
            InterlockedExchange(&mail->gpuStatus,-1);LOG_INFO("GPU transport unavailable; using compatibility readback");return 0;
        }
        LOG_INFO("GPU transport initialized: D3D11 shared eye images, three slots; no CPU readback/upload");
    }
    if(mail->gpuStatus!=1)return 0;
    if(gpuGLContext!=wglGetCurrentContext())goto fail;
    for(int slot=0;slot<BRIDGE_GPU_SLOTS;++slot)if(mail->gpu[slot].state==1){
        int ready=GPUFenceReady(&gpuImages[slot]);if(ready<0)goto fail;
        if(ready){
            if(!gpuUnlock(gpuInterop,2,gpuImages[slot].object))goto fail;
            gpuImages[slot].locked=0;ID3D11DeviceContext_Flush(gpuDXContext);
            InterlockedExchange(&mail->gpu[slot].state,2);
        }
    }
    if(queuedPrediction==prediction)return 1;
    int slot=-1;
    for(int i=0;i<BRIDGE_GPU_SLOTS;++i)if(InterlockedCompareExchange(&mail->gpu[i].state,1,0)==0){slot=i;break;}
    if(slot<0){
        /* Drop one unconsumed old frame, never a consumer-owned image. */
        unsigned oldest=~0u;int candidate=-1;
        for(int i=0;i<BRIDGE_GPU_SLOTS;++i)if(mail->gpu[i].state==2 && mail->gpu[i].frame.serial<oldest){oldest=mail->gpu[i].frame.serial;candidate=i;}
        if(candidate>=0 && InterlockedCompareExchange(&mail->gpu[candidate].state,1,2)==2)slot=candidate;
    }
    if(slot<0)return 1; /* GPU is busy: keep latest frame rather than wait. */
    GPUImages *images=&gpuImages[slot];
    if(!gpuLock(gpuInterop,2,images->object))goto fail;images->locked=1;
    GLint read,draw;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);
    glPushAttrib(GL_ALL_ATTRIB_BITS);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
    for(int eye=0;eye<2;++eye){
        bindFBO(GL_DRAW_FRAMEBUFFER,gpuCopyFBO);attachTexture(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,images->texture[eye],0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        if(checkFBO(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){bindFBO(GL_READ_FRAMEBUFFER,read);bindFBO(GL_DRAW_FRAMEBUFFER,draw);glPopAttrib();goto fail;}
        bindFBO(GL_READ_FRAMEBUFFER,eyes[eye]);glReadBuffer(GL_COLOR_ATTACHMENT0);
        blitFBO(0,0,width,height,0,0,width,height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    }
    bindFBO(GL_READ_FRAMEBUFFER,read);bindFBO(GL_DRAW_FRAMEBUFFER,draw);glPopAttrib();
    BridgeFrame *frame=&mail->gpu[slot].frame;frame->mode=BRIDGE_MODE_STEREO;frame->width=width;frame->height=height;
    memcpy(frame->views,views,sizeof(frame->views));frame->serial=++*serial;
    images->fence=gpuFence(GL_SYNC_GPU_COMMANDS_COMPLETE,0);glFlush();
    if(!images->fence)goto fail;queuedPrediction=prediction;return 1;
fail:
    InterlockedExchange(&mail->gpuStatus,-1);LOG_WARN("GPU transport failed; compatibility transport retained");return 0;
}
#else
static int GPUOpenConsumer(BridgeShared *mail,int slot){
    if(!GPUFunctions())return 0;
    GPUImages *images=&gpuImages[slot];if(images->object[0] && images->object[1])return 1;
    GLint previous;glGetIntegerv(GL_TEXTURE_BINDING_2D,&previous);glGenTextures(2,images->texture);
    for(int eye=0;eye<2;++eye){
        HANDLE handle=(HANDLE)(uintptr_t)mail->gpu[slot].handles[eye];
        if(FAILED(ID3D11Device_OpenSharedResource(gpuDevice,handle,&IID_ID3D11Texture2D,(void **)&images->dx[eye])))goto fail;
        images->object[eye]=gpuRegister(gpuInterop,images->dx[eye],images->texture[eye],GL_TEXTURE_2D,WGL_ACCESS_READ_ONLY_NV);
        if(!images->object[eye])goto fail;
    }
    glBindTexture(GL_TEXTURE_2D,previous);return 1;
fail:
    glBindTexture(GL_TEXTURE_2D,previous);return 0;
}
static int GPURead(BridgeShared *mail,BridgeFrame *out){
    if(mail->gpuStatus!=1)return 0;
    unsigned newest=out->serial;int candidate=-1;
    for(int i=0;i<BRIDGE_GPU_SLOTS;++i)if(mail->gpu[i].state==2 && mail->gpu[i].frame.serial>newest){newest=mail->gpu[i].frame.serial;candidate=i;}
    if(candidate<0)return 0;
    if(gpuConsumerSlot>=0){int ready=GPUFenceReady(&gpuImages[gpuConsumerSlot]);if(ready<0)goto fail;if(!ready)return 0;}
    if(InterlockedCompareExchange(&mail->gpu[candidate].state,3,2)!=2)return 0;
    if(!GPUOpenConsumer(mail,candidate))goto fail;
    if(gpuConsumerSlot>=0){GPUImages *old=&gpuImages[gpuConsumerSlot];
        if(!gpuUnlock(gpuInterop,2,old->object))goto fail;
        old->locked=0;InterlockedExchange(&mail->gpu[gpuConsumerSlot].state,0);gpuConsumerSlot=-1;
    }
    if(!gpuLock(gpuInterop,2,gpuImages[candidate].object))goto fail;
    gpuImages[candidate].locked=1;gpuConsumerSlot=candidate;*out=mail->gpu[candidate].frame;
    /* Sampling must be complete for a non-mipmapped shared texture. */
    GLint previous;glGetIntegerv(GL_TEXTURE_BINDING_2D,&previous);
    for(int eye=0;eye<2;++eye){glBindTexture(GL_TEXTURE_2D,gpuImages[candidate].texture[eye]);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);}
    glBindTexture(GL_TEXTURE_2D,previous);return 1;
fail:
    InterlockedExchange(&mail->gpuStatus,-1);LOG("GPU shared-image import failed; reverting to compatibility transport");return -1;
}
static void GPUConsumerSubmitted(void){
    if(gpuConsumerSlot<0)return;GPUImages *images=&gpuImages[gpuConsumerSlot];
    if(images->fence)gpuDeleteFence(images->fence);
    images->fence=gpuFence(GL_SYNC_GPU_COMMANDS_COMPLETE,0);glFlush();
    if(!images->fence){
        /* Never release this slot for reuse without a completion fence. */
        InterlockedExchange(&shared->gpuStatus,-1);sourceGPU=0;source.serial=0;
        LOG("GPU completion fence failed; reverting to compatibility transport");
    }
}
#endif
#endif
