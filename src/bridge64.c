/* x64 OpenXR compositor. The game communicates only through bridge_protocol.h.
 * Intro/menu frames use a persistent LOCAL-space quad; native eye frames use projection
 * layers with the exact poses/FOV that the x86 renderer received. */
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "bridge_protocol.h"
#include "bridge_actions.h"

static FILE *logFile;
#define LOG(...) do { fprintf(logFile,__VA_ARGS__); fputc('\n',logFile); fflush(logFile); } while(0)
static BridgeShared *shared;
static BridgeFrame source;
static BridgeWheel wheel;
static XrPosef panelAnchor;
static int panelAnchored;
static GLuint wheelFont;
static uint32_t eyeWidth, eyeHeight;
static unsigned char *pixels[2];
static GLuint uploadTexture[2];
static int uploadedWidth[2],uploadedHeight[2];
static unsigned freshFrames;
static double sourceCopyMs;
static double CopyClock(void){LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return (double)t.QuadPart*1000/f.QuadPart;}
static char outputDirectory[MAX_PATH];
static PFNGLGENFRAMEBUFFERSPROC genFBO;
static PFNGLBINDFRAMEBUFFERPROC bindFBO;
static PFNGLFRAMEBUFFERTEXTURE2DPROC attachTexture;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC checkFBO;
static PFNGLDELETEFRAMEBUFFERSPROC deleteFBO;
static int sourceGPU;
#include "gpu_transport.h"
typedef struct { XrSwapchain handle; XrSwapchainImageOpenGLKHR *images; uint32_t count; } Eye;

static int Result(XrResult result,const char *operation) {
    if (XR_FAILED(result)) {
        LOG("ERROR %s: %d",operation,result);
        if(shared) InterlockedExchange(&shared->helperStatus,(LONG)result);
        return 0;
    }
    return 1;
}
static LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    return DefWindowProcA(window,message,w,l);
}
static LONG WINAPI ExceptionLog(EXCEPTION_POINTERS *exception) {
    HMODULE module=NULL; char path[MAX_PATH]="(unresolved)";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)exception->ExceptionRecord->ExceptionAddress,&module);
    if(module) GetModuleFileNameA(module,path,sizeof(path));
    LOG("EXCEPTION 0x%08lx module=%s RVA=0x%llx",exception->ExceptionRecord->ExceptionCode,path,
        (unsigned long long)((ULONG_PTR)exception->ExceptionRecord->ExceptionAddress-(ULONG_PTR)module));
    if(shared) InterlockedExchange(&shared->helperStatus,-1000);
    return EXCEPTION_EXECUTE_HANDLER;
}

static int ReadSource(void) {
    double started=CopyClock();
    int gpu=GPURead(shared,&source);
    if(gpu>0){
        sourceGPU=1;++freshFrames;sourceCopyMs+=CopyClock()-started;
        static int logged;if(!logged){logged=1;LOG("TRANSPORT shared-GPU images -> OpenXR swapchain; CPU pixel copies/uploads bypassed");}
        return 1;
    }
    if(gpu<0){sourceGPU=0;source.serial=0;}
    if(!Bridge_TryLock(&shared->frameLock)) return 0;
    BridgeFrame next=shared->frame;
    if(next.serial<=source.serial || !next.serial || !next.width || !next.height ||
       next.width>BRIDGE_MAX_WIDTH || next.height>BRIDGE_MAX_HEIGHT ||
       (next.mode!=BRIDGE_MODE_PANEL && next.mode!=BRIDGE_MODE_STEREO) ||
       (next.mode==BRIDGE_MODE_STEREO && (next.width!=eyeWidth || next.height!=eyeHeight))) {
        Bridge_Unlock(&shared->frameLock); return 0;
    }
    unsigned count=next.mode==BRIDGE_MODE_STEREO?2:1;
    for(unsigned eye=0;eye<count;++eye) memcpy(pixels[eye],shared->pixels[eye],next.width*next.height*4);
    Bridge_Unlock(&shared->frameLock);
    if(next.mode!=source.mode) LOG("SOURCE mode=%s serial=%u size=%ux%u",
        next.mode==BRIDGE_MODE_STEREO?"native-stereo":"intro-menu-panel",next.serial,next.width,next.height);
    source=next;sourceGPU=0;
    for(unsigned eye=0;eye<count;++eye) {
        glBindTexture(GL_TEXTURE_2D,uploadTexture[eye]);
        if(uploadedWidth[eye]!=(int)source.width || uploadedHeight[eye]!=(int)source.height) {
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,source.width,source.height,0,GL_BGRA,GL_UNSIGNED_BYTE,pixels[eye]);
            uploadedWidth[eye]=source.width; uploadedHeight[eye]=source.height;
        } else glTexSubImage2D(GL_TEXTURE_2D,0,0,0,source.width,source.height,GL_BGRA,GL_UNSIGNED_BYTE,pixels[eye]);
    }
    ++freshFrames;sourceCopyMs+=CopyClock()-started;
    return 1;
}

static void SaveImage(int eye) {
    static int savedPanel,savedStereo[2];
    static uint32_t firstSerial[2],lastPanelSerial;
    int *saved=source.mode==BRIDGE_MODE_STEREO?&savedStereo[eye]:&savedPanel;
    char trace[8];int diagnostic=GetEnvironmentVariableA("SOF2_VR_TRACE",trace,sizeof(trace))>0;
    if(!diagnostic)return; /* No implicit synchronous readback during play. */
    if(source.serial<10)return;
    if(source.mode==BRIDGE_MODE_STEREO){
        if(!firstSerial[eye])firstSerial[eye]=source.serial;
        unsigned delay=*saved==0?20:(*saved==1?120:300);
        if(*saved>=(diagnostic?3:1) || source.serial-firstSerial[eye]<delay)return;
    }else if(*saved && (!diagnostic || source.serial-lastPanelSerial<30))return;
    size_t bytes=eyeWidth*eyeHeight*4;
    unsigned char *rgba=malloc(bytes);
    if(!rgba) return;
    glReadPixels(0,0,eyeWidth,eyeHeight,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    unsigned colored=0,bins[4096]={0},distinct=0,largest=0; uint32_t hash=2166136261u;
    for(size_t i=0;i<bytes;i+=4) {
        if((unsigned)rgba[i]+rgba[i+1]+rgba[i+2]>60) ++colored;
        unsigned bin=(rgba[i]>>4)*256+(rgba[i+1]>>4)*16+(rgba[i+2]>>4);
        if(!bins[bin]++)++distinct;if(bins[bin]>largest)largest=bins[bin];
        hash=(hash^rgba[i])*16777619u;
        unsigned char temp=rgba[i];rgba[i]=rgba[i+2];rgba[i+2]=temp;
    }
    if(colored>100 && distinct>=32 && largest<(bytes/4)*.98) {
        char path[MAX_PATH];
        snprintf(path,sizeof(path),"%sbridge-%s.bmp",outputDirectory,
            source.mode==BRIDGE_MODE_PANEL?"panel":(eye?"right":"left"));
        FILE *image=fopen(path,"wb");
        if(image) {
            BITMAPFILEHEADER file={0}; BITMAPINFOHEADER info={0};
            file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+(DWORD)bytes;
            info.biSize=sizeof(info);info.biWidth=eyeWidth;info.biHeight=eyeHeight;
            info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;info.biSizeImage=(DWORD)bytes;
            int ok=fwrite(&file,sizeof(file),1,image)==1 && fwrite(&info,sizeof(info),1,image)==1 && fwrite(rgba,bytes,1,image)==1;
            if(fclose(image))ok=0;
            if(ok){
                LOG("IMAGE %s serial=%u colored=%u bins=%u hash=%08x",path,source.serial,colored,distinct,hash);
                ++*saved;if(source.mode==BRIDGE_MODE_PANEL)lastPanelSerial=source.serial;
            }else{DeleteFileA(path);*saved=3;LOG("WARN diagnostic image write failed; capture disabled");}
        }
    }
    free(rgba);
}

static GLuint wheelTexture;static int bakeWheel;
static void CompositeWheel(int number){
    if(!bakeWheel || !wheelTexture)return;
    float x0=-.62f,x1=.62f,y0=-.62f,y1=.62f;
    if(source.mode==BRIDGE_MODE_STEREO){
        const BridgeView *v=&source.views[number];const float *q=v->orientation;
        float delta[3],right[3]={1-2*(q[1]*q[1]+q[2]*q[2]),2*(q[0]*q[1]+q[3]*q[2]),2*(q[0]*q[2]-q[3]*q[1])};
        float up[3]={2*(q[0]*q[1]-q[3]*q[2]),1-2*(q[0]*q[0]+q[2]*q[2]),2*(q[1]*q[2]+q[3]*q[0])};
        float back[3]={2*(q[0]*q[2]+q[3]*q[1]),2*(q[1]*q[2]-q[3]*q[0]),1-2*(q[0]*q[0]+q[1]*q[1])},ox=0,oy=0,z=1.1f;
        for(int i=0;i<3;++i){delta[i]=v->position[i]-(source.views[0].position[i]+source.views[1].position[i])*.5f;ox+=delta[i]*right[i];oy+=delta[i]*up[i];z+=delta[i]*back[i];}
        float l=tanf(v->fov[0]),r=tanf(v->fov[1]),b=tanf(v->fov[3]),u=tanf(v->fov[2]);
        x0=2*(((-.475f-ox)/z-l)/(r-l))-1;x1=2*(((.475f-ox)/z-l)/(r-l))-1;
        y0=2*(((-.475f-oy)/z-b)/(u-b))-1;y1=2*(((.475f-oy)/z-b)/(u-b))-1;
    }
    glDisable(GL_DEPTH_TEST);glDisable(GL_FOG);glDisable(GL_ALPHA_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_SCISSOR_TEST);
    glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,wheelTexture);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);glColor4f(1,1,1,1);
    glBegin(GL_QUADS);glTexCoord2f(0,0);glVertex2f(x0,y0);glTexCoord2f(1,0);glVertex2f(x1,y0);glTexCoord2f(1,1);glVertex2f(x1,y1);glTexCoord2f(0,1);glVertex2f(x0,y1);glEnd();glDisable(GL_BLEND);
}
static int Paint(Eye *eye,int number,GLuint fbo) {
    XrSwapchainImageAcquireInfo acquire={XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait={XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    XrSwapchainImageReleaseInfo release={XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    uint32_t index=0;
    if(!Result(xrAcquireSwapchainImage(eye->handle,&acquire,&index),"xrAcquireSwapchainImage")) return 0;
    wait.timeout=XR_INFINITE_DURATION;
    if(!Result(xrWaitSwapchainImage(eye->handle,&wait),"xrWaitSwapchainImage") || index>=eye->count) return 0;
    bindFBO(GL_FRAMEBUFFER,fbo);
    attachTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,eye->images[index].image,0);
    if(checkFBO(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) { LOG("ERROR incomplete eye framebuffer");return 0; }
    glViewport(0,0,eyeWidth,eyeHeight);glDisable(GL_SCISSOR_TEST);glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glClearColor(0.015f,0.025f,0.04f,1);glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    if(source.serial) {
        GLuint texture=sourceGPU && gpuConsumerSlot>=0?gpuImages[gpuConsumerSlot].texture[number]:uploadTexture[number];
        glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,texture);glColor4f(1,1,1,1);
        glBegin(GL_QUADS);
        glTexCoord2f(0,0);glVertex2f(-1,-1);glTexCoord2f(1,0);glVertex2f(1,-1);
        glTexCoord2f(1,1);glVertex2f(1,1);glTexCoord2f(0,1);glVertex2f(-1,1);
        glEnd();glDisable(GL_TEXTURE_2D);CompositeWheel(number);SaveImage(number);
    }
    glFlush();bindFBO(GL_FRAMEBUFFER,0);
    return Result(xrReleaseSwapchainImage(eye->handle,&release),"xrReleaseSwapchainImage");
}

#define WHEEL_PIXELS 1024
static GLuint WheelIcon(int id,float *aspect){
    static GLuint icons[65];static float ratios[65];static unsigned char attempted[65];
    if(id<1 || id>64)return 0;
    if(!attempted[id]){
        attempted[id]=1;char path[MAX_PATH];snprintf(path,sizeof(path),"%sicons\\weapon-%02d.rgba",outputDirectory,id);
        FILE *file=fopen(path,"rb");if(!file){LOG("WARN missing wheel icon id=%d",id);return 0;}
        uint32_t header[3];
        if(fread(header,sizeof(header),1,file)!=1 || memcmp(header,"S2IC",4) || !header[1] || !header[2] || header[1]>1024 || header[2]>1024){fclose(file);return 0;}
        size_t bytes=(size_t)header[1]*header[2]*4;unsigned char *pixels=malloc(bytes);
        if(!pixels){fclose(file);return 0;}
        int ok=fread(pixels,bytes,1,file)==1;fclose(file);
        if(ok){
            /* Filter and composite premultiplied icons over the scene. */
            for(size_t i=0;i<bytes;i+=4)
                for(int c=0;c<3;++c)pixels[i+c]=(pixels[i+c]*pixels[i+3]+127)/255;
            glGenTextures(1,&icons[id]);glBindTexture(GL_TEXTURE_2D,icons[id]);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,header[1],header[2],0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);ratios[id]=(float)header[1]/header[2];}
        free(pixels);
    }
    *aspect=ratios[id];return icons[id];
}
static int WheelWeaponIcon(int id,float x,float y,int available){
    float aspect=1;GLuint icon=WheelIcon(id,&aspect);if(!icon)return 0;
    float width=180,height=width/aspect;if(height>58){height=58;width=height*aspect;}
    glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,icon);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    float opacity=available?.85f:.30f;
    glColor4f(opacity,opacity,opacity,opacity);glBegin(GL_QUADS);
    glTexCoord2f(0,1);glVertex2f(x-width/2,y-height/2);glTexCoord2f(1,1);glVertex2f(x+width/2,y-height/2);
    glTexCoord2f(1,0);glVertex2f(x+width/2,y+height/2);glTexCoord2f(0,0);glVertex2f(x-width/2,y+height/2);glEnd();
    glDisable(GL_BLEND);glDisable(GL_TEXTURE_2D);return 1;
}
static void WheelText(float x,float y,const char *text){
    size_t n=strlen(text);glRasterPos2f(x-(float)n*7.5f,y);glListBase(wheelFont-32);glCallLists((GLsizei)n,GL_UNSIGNED_BYTE,text);
}
static void WheelRect(float x,float y,float w,float h){
    glBegin(GL_QUADS);glVertex2f(x,y);glVertex2f(x+w,y);glVertex2f(x+w,y+h);glVertex2f(x,y+h);glEnd();
}
static void SaveWheelImage(void){
    static unsigned saved;char enabled[8];
    if(!GetEnvironmentVariableA("SOF2_VR_TRACE",enabled,sizeof(enabled)) || wheel.page>=12 || !wheel.hover || (saved&(1u<<wheel.page)))return;
    size_t bytes=WHEEL_PIXELS*WHEEL_PIXELS*4;unsigned char *data=malloc(bytes);if(!data)return;
    glReadPixels(0,0,WHEEL_PIXELS,WHEEL_PIXELS,GL_RGBA,GL_UNSIGNED_BYTE,data);
    unsigned clear=0;
    for(size_t i=0;i<bytes;i+=4){if(!data[i+3])++clear;unsigned char r=data[i];data[i]=data[i+2];data[i+2]=r;}
    char path[MAX_PATH];snprintf(path,sizeof(path),"%sbridge-wheel-page%u.bmp",outputDirectory,wheel.page);
    BITMAPFILEHEADER header={0};BITMAPINFOHEADER info={0};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info);header.bfSize=(DWORD)(header.bfOffBits+bytes);
    info.biSize=sizeof(info);info.biWidth=info.biHeight=WHEEL_PIXELS;info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;info.biSizeImage=(DWORD)bytes;
    FILE *file=fopen(path,"wb");if(file){int ok=fwrite(&header,sizeof(header),1,file)==1 && fwrite(&info,sizeof(info),1,file)==1 && fwrite(data,bytes,1,file)==1;int closed=fclose(file)==0;
        if(ok && closed){saved|=1u<<wheel.page;LOG("WHEELIMAGE page=%u hover=%u clear-pixels=%u path=%s",wheel.page,wheel.hover,clear,path);}else DeleteFileA(path);}
    free(data);
}
static int PaintWheel(GLuint fbo){
    if(!wheelTexture){glGenTextures(1,&wheelTexture);glBindTexture(GL_TEXTURE_2D,wheelTexture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,WHEEL_PIXELS,WHEEL_PIXELS,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);}
    bindFBO(GL_FRAMEBUFFER,fbo);attachTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,wheelTexture,0);
    if(checkFBO(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){LOG("ERROR wheel framebuffer incomplete");return 0;}
    glViewport(0,0,WHEEL_PIXELS,WHEEL_PIXELS);glDisable(GL_SCISSOR_TEST);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);glDisable(GL_TEXTURE_2D);glDisable(GL_CULL_FACE);glDisable(GL_LIGHTING);glDisable(GL_ALPHA_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_FOG);
    glMatrixMode(GL_TEXTURE);glLoadIdentity();glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,WHEEL_PIXELS,0,WHEEL_PIXELS,-1,1);glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    /* Only the ring, labels, icons and selection marks touch the scene. */
    glColor4f(.06f,.08f,.10f,.22f);glBegin(GL_LINE_LOOP);
    for(int i=0;i<96;++i){float a=i*6.28318530718f/96;glVertex2f(512+420*cosf(a),512+420*sinf(a));}glEnd();
    int utility=wheel.count && wheel.items[0].id==WHEEL_SAVE;
    glColor4f(.9f,.94f,1,1);WheelText(512,908,utility?"QUICK SAVE / QUICK LOAD":"WEAPONS");
    char text[80];snprintf(text,sizeof(text),"< %u / %u >",wheel.page+1,wheel.pages);WheelText(512,864,text);
    const WheelItem *hover=NULL;
    for(unsigned i=0;i<wheel.count;++i){
        const WheelItem *item=&wheel.items[i];float x,y;Wheel_Slot(i,wheel.count,&x,&y);x=512+x*275;y=512-y*275;
        if((uint32_t)item->id==wheel.hover){
            hover=item;glColor4f(1,.78f,.24f,1);
            for(int side=-1;side<=1;side+=2){
                float edge=x+side*110;
                WheelRect(edge-2,y-43,4,86);
                WheelRect(edge-(side>0?18:0),y-43,18,4);
                WheelRect(edge-(side>0?18:0),y+39,18,4);
            }
        }
        if(item->available)glColor4f(.94f,.96f,1,1);else glColor4f(.42f,.47f,.52f,1);
        if(item->id>=WHEEL_SAVE || !WheelWeaponIcon(item->id,x,y+10,item->available)){
            char label[24];snprintf(label,sizeof(label),"%.20s",item->name);WheelText(x,y+8,label);
        }
        if(item->available)glColor4f(.94f,.96f,1,1);else glColor4f(.42f,.47f,.52f,1);
        if(item->id<WHEEL_SAVE){snprintf(text,sizeof(text),"%d / %d",item->clip,item->reserve);WheelText(x,y-26,text);}
    }
    glColor4f(1,.78f,.24f,1);if(hover)WheelText(512,510,hover->name);
    glColor4f(.8f,.86f,.94f,1);WheelText(512,154,"RIGHT STICK: SELECT / CLICK: PAGE");WheelText(512,112,"TRIGGER: EQUIP / RELEASE GRIP: DROP");
    float x=wheel.point[0],y=wheel.point[1],len=sqrtf(x*x+y*y);if(len>1){x/=len;y/=len;}
    glColor4f(1,.78f,.24f,1);WheelRect(508+x*275,508+y*275,8,8);
    SaveWheelImage();glFlush();bindFBO(GL_FRAMEBUFFER,0);
    return 1;
}

int main(int argc,char **argv) {
    DWORD parentId=0;
    for(int i=1;i+1<argc;++i) if(!strcmp(argv[i],"--pid")) parentId=strtoul(argv[++i],NULL,10);
    if(!parentId) return 2;
    char logPath[MAX_PATH],name[80];
    GetModuleFileNameA(NULL,outputDirectory,sizeof(outputDirectory));
    char *slash=strrchr(outputDirectory,'\\');if(!slash) return 2;slash[1]=0;
    snprintf(logPath,sizeof(logPath),"%ssof2_vr_bridge64.log",outputDirectory);
    logFile=fopen(logPath,"w");if(!logFile) return 2;
    SetUnhandledExceptionFilter(ExceptionLog);
    LOG("SoF2 x64 OpenXR bridge v%u parent=%lu process=%lu ABI=%u",BRIDGE_VERSION,parentId,
        GetCurrentProcessId(),(unsigned)sizeof(BridgeShared));
    int code=1,running=0,quit=0,focused=0;
    BridgeActions actions={0};
    HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,parentId),map=NULL;
    HWND window=NULL;HDC dc=NULL;HGLRC rc=NULL;GLuint fbo=0;
    XrInstance instance=XR_NULL_HANDLE;XrSession session=XR_NULL_HANDLE;
    XrSpace localSpace=XR_NULL_HANDLE,viewSpace=XR_NULL_HANDLE;
    Eye eyes[3]={0};uint32_t frames=0,submitted=0,trackingSerial=0;
    ULONGLONG started=GetTickCount64(),lastReport=started;
    unsigned maxSeconds=0;char seconds[32];
    if(GetEnvironmentVariableA("SOF2_VR_BRIDGE_SECONDS",seconds,sizeof(seconds))) maxSeconds=strtoul(seconds,NULL,10);
    snprintf(name,sizeof(name),"Local\\SoF2VRBridge_%lu",parentId);
    map=OpenFileMappingA(FILE_MAP_ALL_ACCESS,FALSE,name);
    if(!map || !parent) {LOG("ERROR opening parent/mapping: %lu",GetLastError());goto cleanup;}
    shared=MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,sizeof(*shared));
    if(!shared || shared->magic!=BRIDGE_MAGIC || shared->version!=BRIDGE_VERSION ||
        shared->structureBytes!=sizeof(*shared) || shared->producerPid!=parentId) {
        LOG("ERROR bridge protocol/producer mismatch");goto cleanup;
    }
    for(int eye=0;eye<2;++eye) {pixels[eye]=malloc(BRIDGE_IMAGE_BYTES);if(!pixels[eye])goto cleanup;}
    WNDCLASSA wc={0};wc.lpfnWndProc=WindowProc;wc.hInstance=GetModuleHandleA(NULL);wc.lpszClassName="SoF2VRBridge64";
    if(!RegisterClassA(&wc)) goto cleanup;
    window=CreateWindowA(wc.lpszClassName,"SoF2 OpenXR bridge",WS_OVERLAPPEDWINDOW,0,0,320,240,NULL,NULL,wc.hInstance,NULL);
    if(!window)goto cleanup;dc=GetDC(window);
    PIXELFORMATDESCRIPTOR pfd={0};pfd.nSize=sizeof(pfd);pfd.nVersion=1;
    pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;pfd.iPixelType=PFD_TYPE_RGBA;pfd.cColorBits=32;
    int pf=ChoosePixelFormat(dc,&pfd);
    if(!pf || !SetPixelFormat(dc,pf,&pfd) || !(rc=wglCreateContext(dc)) || !wglMakeCurrent(dc,rc)) goto cleanup;
    LOG("GL=%s renderer=%s",glGetString(GL_VERSION),glGetString(GL_RENDERER));
    genFBO=(PFNGLGENFRAMEBUFFERSPROC)wglGetProcAddress("glGenFramebuffers");
    bindFBO=(PFNGLBINDFRAMEBUFFERPROC)wglGetProcAddress("glBindFramebuffer");
    attachTexture=(PFNGLFRAMEBUFFERTEXTURE2DPROC)wglGetProcAddress("glFramebufferTexture2D");
    checkFBO=(PFNGLCHECKFRAMEBUFFERSTATUSPROC)wglGetProcAddress("glCheckFramebufferStatus");
    deleteFBO=(PFNGLDELETEFRAMEBUFFERSPROC)wglGetProcAddress("glDeleteFramebuffers");
    if(!genFBO||!bindFBO||!attachTexture||!checkFBO||!deleteFBO)goto cleanup;
    genFBO(1,&fbo);glGenTextures(2,uploadTexture);
    for(int eye=0;eye<2;++eye) {
        glBindTexture(GL_TEXTURE_2D,uploadTexture[eye]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    wheelFont=glGenLists(96);HFONT font=CreateFontA(-25,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,FIXED_PITCH|FF_MODERN,"Consolas");
    if(!wheelFont || !font){LOG("ERROR wheel font creation failed");goto cleanup;}
    HGDIOBJ oldFont=SelectObject(dc,font);BOOL fontReady=wglUseFontBitmapsA(dc,32,96,wheelFont);SelectObject(dc,oldFont);DeleteObject(font);
    if(!fontReady){LOG("ERROR wheel bitmap font failed");goto cleanup;}
    const char *extensions[]={XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
    /* SDK header version is not the minimum runtime API we require. All used
     * core calls are OpenXR 1.0; OpenGL/input functionality is enabled through
     * explicit extensions. Asking for the SDK's 1.1 version rejects 1.0 runtimes. */
    XrInstanceCreateInfo create={XR_TYPE_INSTANCE_CREATE_INFO};create.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);
    LOG("OPENXR requested API=1.0.0 SDK headers=%u.%u.%u",
        XR_VERSION_MAJOR(XR_CURRENT_API_VERSION),XR_VERSION_MINOR(XR_CURRENT_API_VERSION),XR_VERSION_PATCH(XR_CURRENT_API_VERSION));
    strcpy(create.applicationInfo.applicationName,"SoF2 VR Bridge");create.applicationInfo.applicationVersion=BRIDGE_VERSION;
    strcpy(create.applicationInfo.engineName,"SoF2 x86 bridge");create.applicationInfo.engineVersion=1;
    create.enabledExtensionCount=1;create.enabledExtensionNames=extensions;
    if(!Result(xrCreateInstance(&create,&instance),"xrCreateInstance"))goto cleanup;
    XrInstanceProperties properties={XR_TYPE_INSTANCE_PROPERTIES};
    if(!Result(xrGetInstanceProperties(instance,&properties),"xrGetInstanceProperties"))goto cleanup;
    LOG("RUNTIME=%s version=%u.%u.%u",properties.runtimeName,XR_VERSION_MAJOR(properties.runtimeVersion),
        XR_VERSION_MINOR(properties.runtimeVersion),XR_VERSION_PATCH(properties.runtimeVersion));
    XrSystemGetInfo sysInfo={XR_TYPE_SYSTEM_GET_INFO};sysInfo.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;XrSystemId sys=0;
    XrResult systemResult;
    do {
        systemResult=xrGetSystem(instance,&sysInfo,&sys);
        if(systemResult!=XR_ERROR_FORM_FACTOR_UNAVAILABLE)break;
        if(shared->shutdown || WaitForSingleObject(parent,0)!=WAIT_TIMEOUT || GetTickCount64()-started>30000)break;
        Sleep(250);
    } while(1);
    if(shared->shutdown || WaitForSingleObject(parent,0)!=WAIT_TIMEOUT){code=0;goto cleanup;}
    if(!Result(systemResult,"xrGetSystem"))goto cleanup;
    PFN_xrGetOpenGLGraphicsRequirementsKHR getReq=NULL;
    if(!Result(xrGetInstanceProcAddr(instance,"xrGetOpenGLGraphicsRequirementsKHR",(PFN_xrVoidFunction *)&getReq),"getRequirementsProc")||!getReq)goto cleanup;
    XrGraphicsRequirementsOpenGLKHR req={XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    if(!Result(getReq(instance,sys,&req),"xrGetOpenGLGraphicsRequirementsKHR"))goto cleanup;
    uint32_t viewCount=0;
    if(!Result(xrEnumerateViewConfigurationViews(instance,sys,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&viewCount,NULL),"countViews")||viewCount!=2)goto cleanup;
    XrViewConfigurationView configs[2]={{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if(!Result(xrEnumerateViewConfigurationViews(instance,sys,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&viewCount,configs),"viewRecommendations") || viewCount!=2)goto cleanup;
    /* Common rectangular size for both eyes, bounded by every view's limits.
     * RenderScale affects the actual game FBO, readback and XR swapchain. */
    char configPath[MAX_PATH],scaleText[64],*scaleEnd=NULL;
    snprintf(configPath,sizeof(configPath),"%squality.ini",outputDirectory);
    GetPrivateProfileStringA("Graphics","RenderScale","1.0",scaleText,sizeof(scaleText),configPath);
    double scale=strtod(scaleText,&scaleEnd);
    if(scaleEnd==scaleText || *scaleEnd || !isfinite(scale) || scale<0.5 || scale>1.5){
        LOG("QUALITY invalid RenderScale '%s'; using 1.0",scaleText);scale=1.0;
    }
    int cap=(int)GetPrivateProfileIntA("Graphics","MaxEyeDimension",2560,configPath);
    if(cap<640 || cap>2560){LOG("QUALITY invalid dimension cap %d; using 2560",cap);cap=2560;}
    GLint glLimit=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&glLimit);
    uint32_t recommendedW=0,recommendedH=0;
    uint32_t limitW=cap,limitH=cap;
    if(glLimit<=0){LOG("ERROR invalid GL texture limit");goto cleanup;}
    if(limitW>(uint32_t)glLimit)limitW=glLimit;
    if(limitH>(uint32_t)glLimit)limitH=glLimit;
    for(int eye=0;eye<2;++eye){
        LOG("VIEW eye=%d recommended=%ux%u max=%ux%u",eye,
            configs[eye].recommendedImageRectWidth,configs[eye].recommendedImageRectHeight,
            configs[eye].maxImageRectWidth,configs[eye].maxImageRectHeight);
        if(configs[eye].recommendedImageRectWidth>recommendedW)recommendedW=configs[eye].recommendedImageRectWidth;
        if(configs[eye].recommendedImageRectHeight>recommendedH)recommendedH=configs[eye].recommendedImageRectHeight;
        if(configs[eye].maxImageRectWidth<limitW)limitW=configs[eye].maxImageRectWidth;
        if(configs[eye].maxImageRectHeight<limitH)limitH=configs[eye].maxImageRectHeight;
    }
    if(!recommendedW || !recommendedH || !limitW || !limitH){LOG("ERROR invalid runtime view dimensions");goto cleanup;}
    double effective=fmin(scale,fmin((double)limitW/recommendedW,(double)limitH/recommendedH));
    eyeWidth=(uint32_t)(recommendedW*effective);eyeHeight=(uint32_t)(recommendedH*effective);
    if(!eyeWidth || !eyeHeight){LOG("ERROR empty eye dimensions");goto cleanup;}
    shared->eyeWidth=eyeWidth;shared->eyeHeight=eyeHeight;
    LOG("QUALITY scale=%.3f effective=%.3f cap=%d eye=%ux%u pixelsVs640=%.2f config=%s",
        scale,effective,cap,eyeWidth,eyeHeight,(double)eyeWidth*eyeHeight/(640.0*640.0),configPath);
    XrGraphicsBindingOpenGLWin32KHR binding={XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};binding.hDC=dc;binding.hGLRC=rc;
    XrSessionCreateInfo sessionInfo={XR_TYPE_SESSION_CREATE_INFO};sessionInfo.next=&binding;sessionInfo.systemId=sys;
    if(!Result(xrCreateSession(instance,&sessionInfo,&session),"xrCreateSession"))goto cleanup;
    InterlockedExchange(&shared->helperStatus,1);LOG("SESSION CREATED");
    XrReferenceSpaceCreateInfo spaceInfo={XR_TYPE_REFERENCE_SPACE_CREATE_INFO};spaceInfo.poseInReferenceSpace.orientation.w=1;
    spaceInfo.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;
    if(!Result(xrCreateReferenceSpace(session,&spaceInfo,&localSpace),"createLocalSpace"))goto cleanup;
    spaceInfo.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;
    if(!Result(xrCreateReferenceSpace(session,&spaceInfo,&viewSpace),"createViewSpace"))goto cleanup;
    XrResult actionResult=BridgeActions_Create(&actions,instance,session);
    if(!Result(actionResult,BridgeActions_Failure()))goto cleanup;
    LOG("INPUT Touch actions attached: two grip/aim poses, triggers, grips, sticks, buttons, haptics");
    uint32_t formatCount=0;int64_t formats[128],colorFormat=0;
    if(!Result(xrEnumerateSwapchainFormats(session,128,&formatCount,formats),"xrEnumerateSwapchainFormats"))goto cleanup;
    for(uint32_t i=0;i<formatCount;++i)if(formats[i]==GL_RGBA8){colorFormat=formats[i];break;}
    if(!colorFormat)for(uint32_t i=0;i<formatCount;++i)if(formats[i]==GL_SRGB8_ALPHA8){colorFormat=formats[i];break;}
    if(!colorFormat){LOG("ERROR no RGBA8 format");goto cleanup;}
    for(int eye=0;eye<2;++eye){
        XrSwapchainCreateInfo swap={XR_TYPE_SWAPCHAIN_CREATE_INFO};swap.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        swap.format=colorFormat;swap.sampleCount=1;swap.width=eyeWidth;swap.height=eyeHeight;swap.faceCount=swap.arraySize=swap.mipCount=1;
        if(!Result(xrCreateSwapchain(session,&swap,&eyes[eye].handle),"xrCreateSwapchain"))goto cleanup;
        if(!Result(xrEnumerateSwapchainImages(eyes[eye].handle,0,&eyes[eye].count,NULL),"countImages"))goto cleanup;
        eyes[eye].images=calloc(eyes[eye].count,sizeof(*eyes[eye].images));if(!eyes[eye].images)goto cleanup;
        for(uint32_t i=0;i<eyes[eye].count;++i)eyes[eye].images[i].type=XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        if(!Result(xrEnumerateSwapchainImages(eyes[eye].handle,eyes[eye].count,&eyes[eye].count,(XrSwapchainImageBaseHeader *)eyes[eye].images),"getImages"))goto cleanup;
    }
    while(!quit && !shared->shutdown && WaitForSingleObject(parent,0)==WAIT_TIMEOUT &&
          (!maxSeconds || GetTickCount64()-started<maxSeconds*1000ULL)) {
        MSG message;while(PeekMessageA(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageA(&message);if(message.message==WM_QUIT)quit=1;}
        for(;;){
            XrEventDataBuffer event={XR_TYPE_EVENT_DATA_BUFFER};XrResult r=xrPollEvent(instance,&event);
            if(r==XR_EVENT_UNAVAILABLE)break;if(!Result(r,"xrPollEvent"))goto cleanup;
            if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED){
                XrSessionState state=((XrEventDataSessionStateChanged *)&event)->state;LOG("STATE=%d",state);
                focused=state==XR_SESSION_STATE_FOCUSED;
                if(state==XR_SESSION_STATE_READY && !running){
                    XrSessionBeginInfo begin={XR_TYPE_SESSION_BEGIN_INFO};begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if(!Result(xrBeginSession(session,&begin),"xrBeginSession"))goto cleanup;running=1;InterlockedExchange(&shared->helperStatus,2);
                } else if(state==XR_SESSION_STATE_STOPPING && running){
                    xrEndSession(session);running=0;InterlockedExchange(&shared->helperStatus,1);
                } else if(state==XR_SESSION_STATE_EXITING || state==XR_SESSION_STATE_LOSS_PENDING)quit=1;
            }
        }
        if(quit)break;if(!running){Sleep(10);continue;}
        XrFrameWaitInfo wait={XR_TYPE_FRAME_WAIT_INFO};XrFrameState frame={XR_TYPE_FRAME_STATE};
        if(!Result(xrWaitFrame(session,&wait,&frame),"xrWaitFrame"))goto cleanup;
        XrFrameBeginInfo begin={XR_TYPE_FRAME_BEGIN_INFO};if(!Result(xrBeginFrame(session,&begin),"xrBeginFrame"))goto cleanup;
        BridgeTracking next={0};next.serial=++trackingSerial;
        if(!Result(BridgeActions_Sync(&actions,session,localSpace,frame.predictedDisplayTime,
            focused,&next.input),"syncControllerActions"))goto cleanup;
        next.input.serial=next.serial;
        if(Bridge_TryLock(&shared->hapticLock)){
            BridgeHaptic request=shared->haptic;Bridge_Unlock(&shared->hapticLock);
            int canVibrate=next.input.focused && request.hand<2 &&
                (next.input.hands[request.hand].valid&BRIDGE_HAND_GRIP_VALID);
            if(!Result(BridgeActions_Haptic(&actions,session,&request,canVibrate),"controllerHaptic"))goto cleanup;
        }
        if(frame.shouldRender){
            XrViewLocateInfo locate={XR_TYPE_VIEW_LOCATE_INFO};locate.space=localSpace;locate.displayTime=frame.predictedDisplayTime;
            locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XrViewState state={XR_TYPE_VIEW_STATE};XrView views[2]={{XR_TYPE_VIEW},{XR_TYPE_VIEW}};uint32_t count=0;
            if(!Result(xrLocateViews(session,&locate,&state,2,&count,views),"xrLocateViews"))goto cleanup;
            next.valid=count==2 && (state.viewStateFlags & (XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT))==
                                                   (XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT);
            if(next.valid)for(int eye=0;eye<2;++eye){
                memcpy(next.views[eye].orientation,&views[eye].pose.orientation,sizeof(float)*4);
                memcpy(next.views[eye].position,&views[eye].pose.position,sizeof(float)*3);
                next.views[eye].fov[0]=views[eye].fov.angleLeft;next.views[eye].fov[1]=views[eye].fov.angleRight;
                next.views[eye].fov[2]=views[eye].fov.angleUp;next.views[eye].fov[3]=views[eye].fov.angleDown;
                static unsigned fovLogged;
                if(!(fovLogged&(1u<<eye))){fovLogged|=1u<<eye;LOG("OPENXR FOV eye=%d left=%.4f right=%.4f up=%.4f down=%.4f horizontal-deg=%.2f vertical-deg=%.2f",eye,next.views[eye].fov[0],next.views[eye].fov[1],next.views[eye].fov[2],next.views[eye].fov[3],(next.views[eye].fov[1]-next.views[eye].fov[0])*57.2957795f,(next.views[eye].fov[2]-next.views[eye].fov[3])*57.2957795f);}
            }
        }
        uint32_t incomingMode=source.mode;
        if(Bridge_TryLock(&shared->frameLock)){incomingMode=shared->frame.mode;Bridge_Unlock(&shared->frameLock);}
        if(incomingMode!=BRIDGE_MODE_PANEL)panelAnchored=0;
        if(incomingMode==BRIDGE_MODE_PANEL && next.valid && !panelAnchored){
            /* Capture yaw and centre only on entering the panel. Head motion
             * must never drag the menu or alter its controller-ray plane. */
            const float *q=next.views[0].orientation;
            float fx=-2*(q[0]*q[2]+q[3]*q[1]),fz=-(1-2*(q[0]*q[0]+q[1]*q[1]));
            float yaw=atan2f(-fx,-fz);
            panelAnchor=(XrPosef){{0,sinf(yaw*.5f),0,cosf(yaw*.5f)},{0,0,0}};
            panelAnchor.position.x=(next.views[0].position[0]+next.views[1].position[0])*.5f-2*sinf(yaw);
            panelAnchor.position.y=(next.views[0].position[1]+next.views[1].position[1])*.5f;
            panelAnchor.position.z=(next.views[0].position[2]+next.views[1].position[2])*.5f-2*cosf(yaw);
            panelAnchored=1;LOG("PANEL anchor LOCAL position=%.3f,%.3f,%.3f",panelAnchor.position.x,panelAnchor.position.y,panelAnchor.position.z);
        }
        next.panelValid=panelAnchored;
        if(panelAnchored){memcpy(next.panelPose.orientation,&panelAnchor.orientation,16);memcpy(next.panelPose.position,&panelAnchor.position,12);}
        if(Bridge_TryLock(&shared->trackingLock)){shared->tracking=next;Bridge_Unlock(&shared->trackingLock);}
        /* Publish this prediction before the CPU copy/texture upload, allowing
         * the game to render while the helper uploads the preceding frame. */
        if(frame.shouldRender)ReadSource();
        if(Bridge_TryLock(&shared->wheelLock)){wheel=shared->wheel;Bridge_Unlock(&shared->wheelLock);}
        int showWheel=wheel.open && wheel.count>0 && wheel.count<=WHEEL_PAGE_SIZE && wheel.pages>=1 && wheel.pages<=12 && wheel.page<wheel.pages &&
            (uint32_t)(GetTickCount()-wheel.sampleTimeMs)<250 && focused;
        /* The compositor wheel is the last layer, including the short native
         * menu-close transition. A PANEL source must not hide an open wheel. */
        static int lastWheelVisible=-1;
        if(showWheel!=lastWheelVisible){
            LOG("WHEEL compositor visible=%d open=%u count=%u mode=%u age-ms=%u focused=%d",showWheel,wheel.open,wheel.count,source.mode,(uint32_t)(GetTickCount()-wheel.sampleTimeMs),focused);
            lastWheelVisible=showWheel;
        }
        for(unsigned i=0;i<WHEEL_PAGE_SIZE;++i)wheel.items[i].name[31]=0;
        XrCompositionLayerProjectionView projection[2]={{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection layer={XR_TYPE_COMPOSITION_LAYER_PROJECTION};layer.space=localSpace;layer.viewCount=2;layer.views=projection;
        XrCompositionLayerQuad quad={XR_TYPE_COMPOSITION_LAYER_QUAD};quad.space=localSpace;quad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
        quad.pose=panelAnchor;quad.size.width=2.8f;
        quad.size.height=source.width?2.8f*source.height/source.width:2.0f;
        quad.subImage.swapchain=eyes[0].handle;quad.subImage.imageRect.extent=(XrExtent2Di){eyeWidth,eyeHeight};
        const XrCompositionLayerBaseHeader *layers[2]={NULL,NULL};uint32_t layerCount=0;
        if(frame.shouldRender && next.valid && source.serial){
            /* Bake the wheel into the submitted eyes, after the game/HUD.
             * It no longer depends on a runtime showing a second quad over
             * a native journal/menu layer. Its stereo plane remains 1.1 m. */
            bakeWheel=showWheel;if(showWheel && !PaintWheel(fbo))goto cleanup;
            int count=source.mode==BRIDGE_MODE_STEREO?2:1;
            for(int eye=0;eye<count;++eye)if(!Paint(&eyes[eye],eye,fbo))goto cleanup;
            if(source.mode==BRIDGE_MODE_STEREO){
                for(int eye=0;eye<2;++eye){
                    memcpy(&projection[eye].pose.orientation,source.views[eye].orientation,sizeof(float)*4);
                    memcpy(&projection[eye].pose.position,source.views[eye].position,sizeof(float)*3);
                    projection[eye].fov=(XrFovf){source.views[eye].fov[0],source.views[eye].fov[1],source.views[eye].fov[2],source.views[eye].fov[3]};
                    projection[eye].subImage.swapchain=eyes[eye].handle;
                    projection[eye].subImage.imageRect.extent=(XrExtent2Di){eyeWidth,eyeHeight};
                }
                layers[0]=(XrCompositionLayerBaseHeader *)&layer;
            } else layers[0]=(XrCompositionLayerBaseHeader *)&quad;
            layerCount=1;
        }
        XrFrameEndInfo end={XR_TYPE_FRAME_END_INFO};end.displayTime=frame.predictedDisplayTime;
        end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=layerCount;end.layers=layerCount?layers:NULL;
        if(sourceGPU && layerCount)GPUConsumerSubmitted();
        if(!Result(xrEndFrame(session,&end),"xrEndFrame"))goto cleanup;
        ++frames;if(layerCount)++submitted;
        if(GetTickCount64()-lastReport>=5000){
            static unsigned previousFresh;static double previousCopy;
            unsigned fresh=freshFrames-previousFresh;double elapsed=(double)(GetTickCount64()-lastReport);
            LOG("FRAMES xr=%u submitted=%u gameSerial=%u mode=%u tracking=%u freshFPS=%.1f upload-ms=%.2f display-period-ms=%.2f transport=%s",frames,submitted,source.serial,source.mode,next.valid,fresh*1000.0/elapsed,fresh?(sourceCopyMs-previousCopy)/fresh:0,(double)frame.predictedDisplayPeriod/1000000.0,sourceGPU?"shared-GPU":"CPU-compatibility");
            previousFresh=freshFrames;previousCopy=sourceCopyMs;
            lastReport=GetTickCount64();
        }
    }
    code=0;
cleanup:
    LOG("EXIT code=%d xrFrames=%u submitted=%u gameSerial=%u",code,frames,submitted,source.serial);
    if(shared && code && shared->helperStatus>=0)InterlockedExchange(&shared->helperStatus,-1000);
    if(session && running){
        xrRequestExitSession(session);ULONGLONG deadline=GetTickCount64()+1000;
        while(GetTickCount64()<deadline){
            XrEventDataBuffer event={XR_TYPE_EVENT_DATA_BUFFER};XrResult r=xrPollEvent(instance,&event);
            if(r==XR_EVENT_UNAVAILABLE){Sleep(10);continue;}if(XR_FAILED(r))break;
            if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED && ((XrEventDataSessionStateChanged *)&event)->state==XR_SESSION_STATE_STOPPING){xrEndSession(session);break;}
        }
    }
    if(fbo && deleteFBO)deleteFBO(1,&fbo);if(rc){glDeleteTextures(2,uploadTexture);if(wheelTexture)glDeleteTextures(1,&wheelTexture);if(wheelFont)glDeleteLists(wheelFont,96);}
    for(int eye=0;eye<2;++eye){if(eyes[eye].handle)xrDestroySwapchain(eyes[eye].handle);free(eyes[eye].images);free(pixels[eye]);}
    if(eyes[2].handle)xrDestroySwapchain(eyes[2].handle);free(eyes[2].images);
    BridgeActions_Destroy(&actions);
    if(localSpace)xrDestroySpace(localSpace);if(viewSpace)xrDestroySpace(viewSpace);
    if(session)xrDestroySession(session);if(instance)xrDestroyInstance(instance);
    wglMakeCurrent(NULL,NULL);if(rc)wglDeleteContext(rc);if(dc && window)ReleaseDC(window,dc);if(window)DestroyWindow(window);
    if(shared)UnmapViewOfFile(shared);if(map)CloseHandle(map);if(parent)CloseHandle(parent);
    fclose(logFile);return code;
}
