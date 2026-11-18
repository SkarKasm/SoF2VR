/* Some native sky/background commands execute before the scene hook. Preserve
 * their authored geometry/textures and replay them with each eye's orientation. */
typedef struct {GLuint list,texture,cube[6];float origin[3],color[4];GLint cull,depth,src,dst,env;GLboolean textured,culled,blended,depthTest,fog,depthWrite;} NativeSkyPass;
static NativeSkyPass nativeSky[8];
static HGLRC nativeSkyContext;
static int nativeSkyRecording,nativeSkyCount,nativeSkyCached;
static PFNGLACTIVETEXTUREPROC skyActiveTexture;
static const GameRefdef *activeSkyEye;
static int eyeSkyDrawn;
static void (__cdecl *originalSkyIterator)(void);
static const BYTE skyIteratorSignature[5]={0xa1,0x74,0x99,0x79,0x10};
static void RefreshNativeSkyMap(void){
    static char previous[64];
    const char *map=((const char *(__cdecl *)(const char *))(gameBase+0x36730))("mapname");
    if(map && strncmp(previous,map,sizeof(previous)-1)){
        snprintf(previous,sizeof(previous),"%s",map);
        /* Never replay the last campaign cube into a newly loaded map
         * whose authored background may intentionally be textureless. */
        nativeSkyCount=nativeSkyCached=0;
    }
}
static void CaptureNativeCube(NativeSkyPass *s){
    memset(s->cube,0,sizeof(s->cube));BYTE *shader=NULL;BYTE *images[6];SIZE_T got;
    /* Verified sky iterator reads shader->sky.outerbox at +b0; GL_Bind
     * (RVA 7bdf0) reads image->texnum at +58. No asset names are guessed. */
    if(!ReadProcessMemory(GetCurrentProcess(),gameBase+0x7d3f40,&shader,sizeof(shader),&got) || got!=sizeof(shader) || !shader)return;
    if(!ReadProcessMemory(GetCurrentProcess(),shader+0xb0,images,sizeof(images),&got) || got!=sizeof(images))return;
    GLuint textures[6];
    for(int i=0;i<6;++i){
        if(!images[i] || images[i]==*(BYTE **)(gameBase+0x79a3c0) || !ReadProcessMemory(GetCurrentProcess(),images[i]+0x58,&textures[i],4,&got) || got!=4 || !glIsTexture(textures[i]))return;
    }
    memcpy(s->cube,textures,sizeof(textures));
    static unsigned logged;if(tracePoses && logged<4){++logged;char name[65]={0};ReadProcessMemory(GetCurrentProcess(),shader,name,64,&got);LOG_INFO("SKY native cube shader=%s textures=%u,%u,%u,%u,%u,%u",name,textures[0],textures[1],textures[2],textures[3],textures[4],textures[5]);}
}
static void BeginNativeSkyCapture(void){
    if(eyeActive || !gameBase || nativeSkyRecording || nativeSkyCount>=8)return;
    GLfloat p[16],m[16];GLint list;glGetFloatv(GL_PROJECTION_MATRIX,p);glGetIntegerv(GL_LIST_INDEX,&list);
    if(list || fabsf(p[11]+1)>.001f || fabsf(p[15])>.001f)return;
    HGLRC context=wglGetCurrentContext();
    if(nativeSkyContext!=context){memset(nativeSky,0,sizeof(nativeSky));nativeSkyRecording=nativeSkyCount=nativeSkyCached=0;nativeSkyContext=context;}
    RefreshNativeSkyMap();
    NativeSkyPass *s=&nativeSky[nativeSkyCount];if(!s->list)s->list=glGenLists(1);if(!s->list)return;
    CaptureNativeCube(s);
    glGetFloatv(GL_MODELVIEW_MATRIX,m);for(int i=0;i<3;++i)s->origin[i]=-(m[i*4]*m[12]+m[i*4+1]*m[13]+m[i*4+2]*m[14]);
    glGetFloatv(GL_CURRENT_COLOR,s->color);glGetIntegerv(GL_TEXTURE_BINDING_2D,(GLint *)&s->texture);glGetTexEnviv(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,&s->env);
    glGetIntegerv(GL_CULL_FACE_MODE,&s->cull);glGetIntegerv(GL_DEPTH_FUNC,&s->depth);glGetIntegerv(GL_BLEND_SRC,&s->src);glGetIntegerv(GL_BLEND_DST,&s->dst);
    s->textured=glIsEnabled(GL_TEXTURE_2D);s->culled=glIsEnabled(GL_CULL_FACE);s->blended=glIsEnabled(GL_BLEND);s->depthTest=glIsEnabled(GL_DEPTH_TEST);s->fog=glIsEnabled(GL_FOG);glGetBooleanv(GL_DEPTH_WRITEMASK,&s->depthWrite);
    nativeSkyRecording=1;glNewList(s->list,GL_COMPILE_AND_EXECUTE);
}
static void EndNativeSkyCapture(void){
    if(!nativeSkyRecording)return;glEndList();nativeSkyRecording=0;++nativeSkyCount;nativeSkyCached=nativeSkyCount;
    static unsigned logged;if(logged<4){LOG_INFO("SKY captured native background pass=%d outside scene eye hook",nativeSkyCount);++logged;}
}
static void RenderSkyPasses(const GameRefdef *eye,const NativeSkyPass *passes,int count){
    if(!count)return;
    GLint mode;glGetIntegerv(GL_MATRIX_MODE,&mode);glPushAttrib(GL_ALL_ATTRIB_BITS);
    if(!skyActiveTexture)skyActiveTexture=(PFNGLACTIVETEXTUREPROC)wglGetProcAddress("glActiveTexture");
    if(skyActiveTexture){
        GLint units=1;glGetIntegerv(GL_MAX_TEXTURE_UNITS,&units);if(units>32)units=32;
        for(int unit=units-1;unit>=0;--unit){
            skyActiveTexture(GL_TEXTURE0+unit);
            glDisable(GL_TEXTURE_1D);glDisable(GL_TEXTURE_2D);glDisable(GL_TEXTURE_3D);glDisable(GL_TEXTURE_CUBE_MAP);
            glDisable(GL_TEXTURE_GEN_S);glDisable(GL_TEXTURE_GEN_T);glDisable(GL_TEXTURE_GEN_R);glDisable(GL_TEXTURE_GEN_Q);
        }
    }
    glMatrixMode(GL_TEXTURE);glPushMatrix();glLoadIdentity();
    glMatrixMode(GL_PROJECTION);glPushMatrix();
    float l=tanf(activeView.fov[0]),r=tanf(activeView.fov[1]),u=tanf(activeView.fov[2]),b=tanf(activeView.fov[3]);
    GLfloat p[16]={0};p[0]=2/(r-l);p[5]=2/(u-b);p[8]=(r+l)/(r-l);p[9]=(u+b)/(u-b);p[10]=p[11]=p[14]=-1;realMatrix(p);
    glMatrixMode(GL_MODELVIEW);glPushMatrix();glDisable(GL_SCISSOR_TEST);glDisable(GL_CLIP_PLANE0);glDisable(GL_LIGHTING);glDisable(GL_STENCIL_TEST);glDisable(GL_ALPHA_TEST);glColorMask(1,1,1,1);realViewport(0,0,eyeWidth,eyeHeight);
    for(int n=0;n<count;++n){const NativeSkyPass *s=&passes[n];GLfloat m[16]={0};m[15]=1;
        for(int i=0;i<3;++i){m[i*4]=-eye->axis[1][i];m[i*4+1]=eye->axis[2][i];m[i*4+2]=-eye->axis[0][i];m[12]+=eye->axis[1][i]*s->origin[i];m[13]-=eye->axis[2][i]*s->origin[i];m[14]+=eye->axis[0][i]*s->origin[i];}
        if(s->cube[0]){
            /* Sky faces are directions at infinity. Use the native six
             * loaded textures with the engine's face order and UV mapping;
             * desktop visibility bounds/fog draws must not limit an eye. */
            for(int i=0;i<3;++i){m[i*4]=-eye->axis[1][i];m[i*4+1]=eye->axis[2][i];m[i*4+2]=-eye->axis[0][i];}m[12]=m[13]=m[14]=0;
            realMatrix(m);glEnable(GL_TEXTURE_2D);glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);glColor4f(1,1,1,1);
            glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_ALPHA_TEST);glDisable(GL_FOG);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDepthMask(GL_FALSE);realDepthRange(1,1);
            static const int order[6]={0,2,1,3,4,5};
            for(int face=0;face<6;++face){glBindTexture(GL_TEXTURE_2D,s->cube[order[face]]);glBegin(GL_QUADS);
                for(int corner=0;corner<4;++corner){float a=(corner==1 || corner==2)?1:-1,b=corner>=2?1:-1,x,y,z;
                    if(face==0){x=16;y=-a*16;z=b*16;}else if(face==1){x=-16;y=a*16;z=b*16;}
                    else if(face==2){x=a*16;y=16;z=b*16;}else if(face==3){x=-a*16;y=-16;z=b*16;}
                    else if(face==4){x=-b*16;y=-a*16;z=16;}else{x=b*16;y=-a*16;z=-16;}
                    glTexCoord2f(a>0?.9995f:.0005f,b>0?.0005f:.9995f);glVertex3f(x,y,z);
                }glEnd();
            }
            continue;
        }
        realMatrix(m);glBindTexture(GL_TEXTURE_2D,s->texture);glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,s->env);glColor4fv(s->color);
        if(s->textured)glEnable(GL_TEXTURE_2D);else glDisable(GL_TEXTURE_2D);
        if(s->culled)glEnable(GL_CULL_FACE);else glDisable(GL_CULL_FACE);glCullFace(s->cull);
        if(s->blended)glEnable(GL_BLEND);else glDisable(GL_BLEND);glBlendFunc(s->src,s->dst);
        if(s->depthTest)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);glDepthFunc(s->depth);glDepthMask(s->depthWrite);
        /* The world/searchlight fog state at replay time is unrelated to
         * the distant background. Do not fog a sky cube at its artificial
         * render distance, or it becomes black/grey rather than a sky. */
        /* Capture starts inside the native glDepthRange(1,1) wrapper, so
         * that initial range is not part of the display list. The native
         * sky uses GL_EQUAL against clear-depth 1: replaying at the ordinary
         * 0..1 range rejects every distant sky fragment. */
        realDepthRange(1,1);glDisable(GL_FOG);glCallList(s->list);
        static unsigned debug;if(tracePoses && debug<4){++debug;GLint width=0,height=0,texture=0;GLfloat matrix[16];glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);glGetFloatv(GL_MODELVIEW_MATRIX,matrix);LOG_INFO("SKY replay list=%u texture=%d size=%dx%d origin=%.1f,%.1f,%.1f color=%.2f,%.2f,%.2f textured=%d depth=%x glError=%x matrixZ=%.2f",s->list,texture,width,height,s->origin[0],s->origin[1],s->origin[2],s->color[0],s->color[1],s->color[2],s->textured,s->depth,glGetError(),matrix[14]);}
    }
    glPopMatrix();glMatrixMode(GL_PROJECTION);glPopMatrix();if(skyActiveTexture)skyActiveTexture(GL_TEXTURE0);glMatrixMode(GL_TEXTURE);glPopMatrix();glMatrixMode(mode);glPopAttrib();
    static int logged;if(!logged){logged=1;LOG_INFO("SKY replaying captured native passes into headset eyes");}
}
static void ReplayNativeSky(const GameRefdef *eye){RefreshNativeSkyMap();RenderSkyPasses(eye,nativeSky,nativeSkyCached);}
static void __attribute__((force_align_arg_pointer)) __cdecl StereoSkyIterator(void){
    if(eyeActive && activeSkyEye){
        NativeSkyPass sky={0};CaptureNativeCube(&sky);
        if(sky.cube[0]){
            RenderSkyPasses(activeSkyEye,&sky,1);eyeSkyDrawn=1;
            static unsigned logged;if(logged<4){++logged;GLint fbo;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);LOG_INFO("SKY native eye stage rendered complete cube eyeFBO=%d",fbo);}
            /* Native skyRendered flag, used later by the sun/effects pass. */
            *(BYTE *)(gameBase+0x781f65)=1;
            return;
        }
    }
    originalSkyIterator();
}
static void InstallSkyIterator(BYTE *base){
    if(originalSkyIterator || memcmp(base+0xa6f10,skyIteratorSignature,5))return;
    BYTE *t=VirtualAlloc(NULL,16,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!t)return;
    memcpy(t,skyIteratorSignature,5);t[5]=0xe9;*(int32_t *)(t+6)=(int32_t)((base+0xa6f15)-(t+10));DWORD protect;
    if(!VirtualProtect(t,16,PAGE_EXECUTE_READ,&protect)){VirtualFree(t,0,MEM_RELEASE);return;}
    if(!VirtualProtect(base+0xa6f10,5,PAGE_EXECUTE_READWRITE,&protect)){VirtualFree(t,0,MEM_RELEASE);return;}
    originalSkyIterator=(void *)t;base[0xa6f10]=0xe9;*(int32_t *)(base+0xa6f11)=(int32_t)((BYTE *)StereoSkyIterator-(base+0xa6f15));
    VirtualProtect(base+0xa6f10,5,protect,&protect);FlushInstructionCache(GetCurrentProcess(),base+0xa6f10,5);
    LOG_INFO("Verified native sky stage: complete native cube drawn per eye before transparent effects");
}
