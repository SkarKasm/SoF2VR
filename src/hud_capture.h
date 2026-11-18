/* Keep the native HUD in one transparent canvas, then project it once per eye.
 * Included after the verified renderer globals and HUD batch predicates. */
static GLuint hudTargets[3],hudTextures[3];
static unsigned hudCanvasWidth,hudCanvasHeight,hudEpoch,hudCapturedEpoch;
static HGLRC hudContext;
static int hudHasLayer[3],hudRecording;
static GLuint hudImmediateList;
static GLfloat hudImmediateColor[4],hudImmediateBounds[4];
static void (APIENTRY *hudBegin)(GLenum), (APIENTRY *hudEnd)(void);
static void (APIENTRY *hudVertex2f)(GLfloat,GLfloat), (APIENTRY *hudVertex3f)(GLfloat,GLfloat,GLfloat);
static void (APIENTRY *hudVertex2fv)(const GLfloat *), (APIENTRY *hudVertex3fv)(const GLfloat *);
static void (APIENTRY *hudDrawArrays)(GLenum,GLint,GLsizei);
static void (APIENTRY *hudRectf)(GLfloat,GLfloat,GLfloat,GLfloat);
static PFNGLBLENDFUNCSEPARATEPROC hudBlendSeparate;
static PFNGLACTIVETEXTUREPROC hudActiveTexture;
static GLubyte hudOpaqueColors[16384][4];

static void HudFunctions(void){
    if(hudBegin)return;OpenGL_EnsureForwarding();
    hudBegin=(void *)GetProcAddress(systemGL,"glBegin");hudEnd=(void *)GetProcAddress(systemGL,"glEnd");
    hudVertex2f=(void *)GetProcAddress(systemGL,"glVertex2f");hudVertex3f=(void *)GetProcAddress(systemGL,"glVertex3f");
    hudVertex2fv=(void *)GetProcAddress(systemGL,"glVertex2fv");hudVertex3fv=(void *)GetProcAddress(systemGL,"glVertex3fv");
    hudDrawArrays=(void *)GetProcAddress(systemGL,"glDrawArrays");hudRectf=(void *)GetProcAddress(systemGL,"glRectf");
}
static int HudEligible(void){
    if(!nativeRendered || eyeActive || !gameBase || *(unsigned *)(gameBase+0x1476d4))return 0;
    GLfloat p[16];glGetFloatv(GL_PROJECTION_MATRIX,p);
    return fabsf(p[15]-1)<.001f && fabsf(p[11])<.001f;
}
static int HudTargets(void){
    RECT rect;HWND window=WindowFromDC(wglGetCurrentDC());
    if(!window || !GetClientRect(window,&rect) || rect.right<1 || rect.bottom<1 || rect.right>8192 || rect.bottom>8192)return 0;
    HGLRC context=wglGetCurrentContext();
    if(hudContext!=context){memset(hudTargets,0,sizeof(hudTargets));memset(hudTextures,0,sizeof(hudTextures));hudImmediateList=0;hudActiveTexture=NULL;hudCanvasWidth=hudCanvasHeight=0;hudContext=context;}
    GLint previous;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&previous);glPushAttrib(GL_ALL_ATTRIB_BITS);
    int resize=hudCanvasWidth!=(unsigned)rect.right || hudCanvasHeight!=(unsigned)rect.bottom;
    if(!hudTargets[0])genFBO(3,hudTargets);
    if(!hudTextures[0])glGenTextures(3,hudTextures);
    if(resize){
        hudCanvasWidth=rect.right;hudCanvasHeight=rect.bottom;
        for(int i=0;i<3;++i){
            glBindTexture(GL_TEXTURE_2D,hudTextures[i]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,rect.right,rect.bottom,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            bindFBO(GL_FRAMEBUFFER,hudTargets[i]);attachTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,hudTextures[i],0);
            if(checkFBO(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){bindFBO(GL_FRAMEBUFFER,previous);glPopAttrib();return 0;}
        }
        hudBlendSeparate=(PFNGLBLENDFUNCSEPARATEPROC)wglGetProcAddress("glBlendFuncSeparate");
        LOG_INFO("HUD native canvas=%ux%u; complete textured/immediate batches",hudCanvasWidth,hudCanvasHeight);
    }
    if(resize || hudCapturedEpoch!=hudEpoch){
        for(int i=0;i<3;++i){bindFBO(GL_FRAMEBUFFER,hudTargets[i]);glDrawBuffer(GL_COLOR_ATTACHMENT0);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);hudHasLayer[i]=0;}
        hudCapturedEpoch=hudEpoch;
    }
    bindFBO(GL_FRAMEBUFFER,previous);glPopAttrib();return 1;
}
static int HudProjectedBounds(const GLfloat *bounds,GLfloat out[4]){
    if(!bounds || bounds[0]>bounds[2])return 0;
    GLfloat p[16],m[16];glGetFloatv(GL_PROJECTION_MATRIX,p);glGetFloatv(GL_MODELVIEW_MATRIX,m);
    float minX=1e9f,minY=1e9f,maxX=-1e9f,maxY=-1e9f;
    for(int k=0;k<4;++k){float x=bounds[k&1?2:0],y=bounds[k&2?3:1],v[4],q[4];
        for(int i=0;i<4;++i)v[i]=m[i]*x+m[4+i]*y+m[12+i];
        for(int i=0;i<4;++i){q[i]=0;for(int j=0;j<4;++j)q[i]+=p[j*4+i]*v[j];}
        if(fabsf(q[3])<.001f)return 0;minX=fminf(minX,q[0]/q[3]);maxX=fmaxf(maxX,q[0]/q[3]);minY=fminf(minY,q[1]/q[3]);maxY=fmaxf(maxY,q[1]/q[3]);
    }
    out[0]=minX;out[1]=minY;out[2]=maxX;out[3]=maxY;return 1;
}
static int HudFullBounds(const GLfloat *bounds){
    GLfloat q[4];return HudProjectedBounds(bounds,q) && q[0]<=-.995f && q[2]>=.995f && q[1]<=-.995f && q[3]>=.995f;
}
static void HudOpaqueAlpha(int kind,GLsizei count,GLenum type,const void *indices,GLint first){
    /* The desktop backbuffer ignores alpha for an unblended solid bar. The
     * transparent canvas cannot: keep its RGB while making that draw opaque. */
    if(glIsEnabled(GL_BLEND) || glIsEnabled(GL_ALPHA_TEST))return;
    GLfloat color[4];glGetFloatv(GL_CURRENT_COLOR,color);glColor4f(color[0],color[1],color[2],1);
    if((kind!=0 && kind!=2) || !glIsEnabled(GL_COLOR_ARRAY) || count<1 || count>65536)return;
    GLint format,size,stride,buffer,arrayBuffer,elementBuffer;void *colors=NULL;
    glGetIntegerv(GL_COLOR_ARRAY_BUFFER_BINDING,&buffer);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&arrayBuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&elementBuffer);if(kind==0 && elementBuffer)return;
    glGetIntegerv(GL_COLOR_ARRAY_TYPE,&format);glGetIntegerv(GL_COLOR_ARRAY_SIZE,&size);glGetIntegerv(GL_COLOR_ARRAY_STRIDE,&stride);glGetPointerv(GL_COLOR_ARRAY_POINTER,&colors);
    if(buffer || arrayBuffer || !colors || format!=GL_UNSIGNED_BYTE || size!=4)return;
    if(!stride)stride=4;if(stride<4 || stride>256)return;
    if(kind==0 && (!indices || (type!=GL_UNSIGNED_INT && type!=GL_UNSIGNED_SHORT && type!=GL_UNSIGNED_BYTE)))return;
    if(kind==2 && (first<0 || first>16384 || count>16384-first))return;
    int changed=0;
    for(GLsizei n=0;n<count;++n){
        unsigned index=kind==2?(unsigned)(first+n):type==GL_UNSIGNED_INT?((const GLuint *)indices)[n]:type==GL_UNSIGNED_SHORT?((const GLushort *)indices)[n]:((const GLubyte *)indices)[n];
        if(index>=16384)return;const GLubyte *source=(const GLubyte *)colors+(size_t)index*stride;
        memcpy(hudOpaqueColors[index],source,3);hudOpaqueColors[index][3]=255;changed|=source[3]!=255;
    }
    glColorPointer(4,GL_UNSIGNED_BYTE,4,hudOpaqueColors);
    static int logged;if(changed && !logged){logged=1;LOG_INFO("HUD solid native bar alpha made opaque in transparent canvas; RGB and fill geometry preserved");}
}
/* kind: indexed=0, display list=1, arrays=2, rectangle=3. */
static int CinematicSolidBounds(const GLfloat *bounds,const GLfloat *color){
    if(playerScene || !bounds || !color || color[0]>.01f || color[1]>.01f || color[2]>.01f)return 0;
    if(glIsEnabled(GL_TEXTURE_2D)){GLint w=0,h=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&w);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&h);if(w>16 || h>16)return 0;}
    GLfloat q[4];return HudProjectedBounds(bounds,q) && q[2]-q[0]>.9f && q[3]-q[1]<.65f && q[3]-q[1]>.003f;
}
static void CaptureHud(int kind,GLenum mode,GLsizei count,GLenum type,const void *indices,GLint first,GLuint list,const GLfloat *color,const GLfloat *bounds){
    if(!HudEligible())return;HudFunctions();GLfloat current[4];
    GLint viewport[4],previous;glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_FRAMEBUFFER_BINDING,&previous);
    if(kind==0 && CinematicBars(count,type,indices,viewport))return;
    if(kind==2 && first>=0 && (count==6 || count==12)){GLuint ids[12];for(int i=0;i<count;++i)ids[i]=first+i;if(CinematicBars(count,GL_UNSIGNED_INT,ids,viewport))return;}
    if(bounds){if(!color){glGetFloatv(GL_CURRENT_COLOR,current);color=current;}if(CinematicSolidBounds(bounds,color))return;}
    int layer=bounds?HudFullBounds(bounds):(kind==0 && FullScreenBatch(count,type,indices,viewport));
    GLint blendDestination=0;glGetIntegerv(GL_BLEND_DST,&blendDestination);
    /* Searchlight glare is additive. Accumulating it into an alpha-over
     * canvas gives it an opaque alpha and replaces the world with grey. */
    if(glIsEnabled(GL_BLEND) && blendDestination==GL_ONE)layer=2;
    if(!HudTargets())return;
    glPushAttrib(GL_ALL_ATTRIB_BITS);glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);bindFBO(GL_FRAMEBUFFER,hudTargets[layer]);glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    /* Mirror scenes rasterize at 1x1; that must not shrink native HUD pages. */
    if(viewport[2]<=1 || viewport[3]<=1){realViewport(0,0,hudCanvasWidth,hudCanvasHeight);glDisable(GL_SCISSOR_TEST);}
    else if(!layer && glIsEnabled(GL_SCISSOR_TEST)){
        /* Native sub-pages clip the lower health row before the completed
         * canvas reaches VR. Retain X/width for horizontal bar-fill masks,
         * but let the whole vertical status page reach the HUD canvas. */
        GLint clip[4];glGetIntegerv(GL_SCISSOR_BOX,clip);
        realScissor(clip[0],0,clip[2],hudCanvasHeight);
        static int logged;if(!logged){logged=1;LOG_INFO("HUD canvas vertical page clipping removed; native horizontal fill masks retained (%d,%d %dx%d)",clip[0],clip[1],clip[2],clip[3]);}
    }
    glDisable(GL_DEPTH_TEST);glDisable(GL_FOG);
    if(hudBlendSeparate && glIsEnabled(GL_BLEND)){GLint src,dst;glGetIntegerv(GL_BLEND_SRC,&src);glGetIntegerv(GL_BLEND_DST,&dst);hudBlendSeparate(src,dst,layer==2?GL_ZERO:GL_ONE,layer==2?GL_ONE:GL_ONE_MINUS_SRC_ALPHA);}
    if(color)glColor4fv(color);
    /* Full-screen fades/fog retain their authored alpha. Promoting their
     * alpha to opaque hides the entire world when entering a searchlight. */
    if(!layer)HudOpaqueAlpha(kind,count,type,indices,first);
    if(kind==0)realDrawElements(mode,count,type,indices);
    else if(kind==1)glCallList(list);
    else if(kind==2)hudDrawArrays(mode,first,count);
    else hudRectf(bounds[0],bounds[1],bounds[2],bounds[3]);
    hudHasLayer[layer]=1;++hudDraws;bindFBO(GL_FRAMEBUFFER,previous);glPopClientAttrib();glPopAttrib();
}
static void HudVertex(float x,float y){if(hudRecording){hudImmediateBounds[0]=fminf(hudImmediateBounds[0],x);hudImmediateBounds[1]=fminf(hudImmediateBounds[1],y);hudImmediateBounds[2]=fmaxf(hudImmediateBounds[2],x);hudImmediateBounds[3]=fmaxf(hudImmediateBounds[3],y);}}
void APIENTRY Proxy_glBegin(GLenum mode){
    EnsureEyeProjection();
    HudFunctions();
    if(!hudRecording && HudEligible()){
        GLint list;glGetIntegerv(GL_LIST_INDEX,&list);
        if(!list){if(!hudImmediateList)hudImmediateList=glGenLists(1);
            if(hudImmediateList){glGetFloatv(GL_CURRENT_COLOR,hudImmediateColor);hudImmediateBounds[0]=hudImmediateBounds[1]=1e9f;hudImmediateBounds[2]=hudImmediateBounds[3]=-1e9f;hudRecording=1;glNewList(hudImmediateList,GL_COMPILE_AND_EXECUTE);}}
    }
    hudBegin(mode);
}
void APIENTRY Proxy_glEnd(void){HudFunctions();hudEnd();if(hudRecording){glEndList();hudRecording=0;CaptureHud(1,0,0,0,NULL,0,hudImmediateList,hudImmediateColor,hudImmediateBounds);}}
void APIENTRY Proxy_glVertex2f(GLfloat x,GLfloat y){HudFunctions();HudVertex(x,y);hudVertex2f(x,y);}
void APIENTRY Proxy_glVertex3f(GLfloat x,GLfloat y,GLfloat z){HudFunctions();HudVertex(x,y);hudVertex3f(x,y,z);}
void APIENTRY Proxy_glVertex2fv(const GLfloat *v){HudFunctions();HudVertex(v[0],v[1]);hudVertex2fv(v);}
void APIENTRY Proxy_glVertex3fv(const GLfloat *v){HudFunctions();HudVertex(v[0],v[1]);hudVertex3fv(v);}
void APIENTRY Proxy_glDrawArrays(GLenum mode,GLint first,GLsizei count){
    EnsureEyeProjection();HudFunctions();
    WeaponMaterialDrawState material;
    int customMaterial=BeginWeaponMaterial(mode,&material);
    hudDrawArrays(mode,first,count);
    if(customMaterial)EndWeaponMaterial(&material);
    CaptureHud(2,mode,count,0,NULL,first,0,NULL,NULL);
}
void APIENTRY Proxy_glRectf(GLfloat x0,GLfloat y0,GLfloat x1,GLfloat y1){HudFunctions();hudRectf(x0,y0,x1,y1);GLfloat bounds[4]={fminf(x0,x1),fminf(y0,y1),fmaxf(x0,x1),fmaxf(y0,y1)};CaptureHud(3,0,0,0,NULL,0,0,NULL,bounds);}

static void CompositeHud(void){
    if(!nativeRendered || weaponWheel.open || hudCapturedEpoch!=hudEpoch || (!hudHasLayer[0] && !hudHasLayer[1] && !hudHasLayer[2]))return;
    GLint previous,mode;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&previous);glGetIntegerv(GL_MATRIX_MODE,&mode);glPushAttrib(GL_ALL_ATTRIB_BITS);
    /* The engine's fog/light passes leave secondary texture stages and
     * generated coordinates enabled. A HUD quad has only unit-zero UVs. */
    if(!hudActiveTexture)hudActiveTexture=(PFNGLACTIVETEXTUREPROC)wglGetProcAddress("glActiveTexture");
    if(hudActiveTexture){
        GLint units=1;glGetIntegerv(GL_MAX_TEXTURE_UNITS,&units);if(units>32)units=32;
        for(GLint unit=units-1;unit>=0;--unit){
            hudActiveTexture(GL_TEXTURE0+unit);glDisable(GL_TEXTURE_1D);glDisable(GL_TEXTURE_2D);glDisable(GL_TEXTURE_3D);glDisable(GL_TEXTURE_CUBE_MAP);
            glDisable(GL_TEXTURE_GEN_S);glDisable(GL_TEXTURE_GEN_T);glDisable(GL_TEXTURE_GEN_R);glDisable(GL_TEXTURE_GEN_Q);
        }
    }
    glMatrixMode(GL_TEXTURE);glPushMatrix();glLoadIdentity();
    glMatrixMode(GL_PROJECTION);glPushMatrix();glLoadIdentity();glMatrixMode(GL_MODELVIEW);glPushMatrix();glLoadIdentity();
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_LIGHTING);glDisable(GL_SCISSOR_TEST);glDisable(GL_CLIP_PLANE0);
    /* Searchlight fog belongs to the world pass. At HUD identity depth it
     * turns the entire textured overlay into an opaque fog-colour rectangle. */
    glDisable(GL_FOG);glDisable(GL_ALPHA_TEST);glDisable(GL_STENCIL_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glEnable(GL_TEXTURE_2D);glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);glColor4f(1,1,1,1);
    float inv[4],mid[3],offsets[2][3],horizontal=1e6f,vertical=1e6f;const float depth=2,cy=hudOffset*2;
    for(int i=0;i<3;++i){inv[i]=-tracking.views[0].orientation[i];mid[i]=(tracking.views[0].position[i]+tracking.views[1].position[i])*.5f;}inv[3]=tracking.views[0].orientation[3];
    for(int eye=0;eye<2;++eye){float d[3];for(int i=0;i<3;++i)d[i]=tracking.views[eye].position[i]-mid[i];Rotate(inv,d,offsets[eye]);
        const BridgeView *v=&tracking.views[eye];float z=depth+offsets[eye][2];horizontal=fminf(horizontal,fminf(-tanf(v->fov[0])*z-offsets[eye][0],tanf(v->fov[1])*z+offsets[eye][0]));vertical=fminf(vertical,fminf(tanf(v->fov[2])*z+offsets[eye][1]-cy,cy-tanf(v->fov[3])*z-offsets[eye][1]));}
    float hx=fmaxf(.1f,horizontal)*hudWidth,hy=hx*hudCanvasHeight/hudCanvasWidth,maxY=fmaxf(.1f,vertical)*hudHeight;if(hy>maxY){hx*=maxY/hy;hy=maxY;}
    for(int eye=0;eye<2;++eye){bindFBO(GL_FRAMEBUFFER,eyeFBO[eye]);glDrawBuffer(GL_COLOR_ATTACHMENT0);realViewport(0,0,eyeWidth,eyeHeight);
        for(int layer=0;layer<3;++layer)if(hudHasLayer[layer]){float x0=-1,x1=1,y0=-1,y1=1;glBlendFunc(GL_ONE,layer==2?GL_ONE:GL_ONE_MINUS_SRC_ALPHA);
            if(!layer){const BridgeView *v=&tracking.views[eye];float z=depth+offsets[eye][2],l=tanf(v->fov[0]),r=tanf(v->fov[1]),b=tanf(v->fov[3]),u=tanf(v->fov[2]);x0=2*(((-hx-offsets[eye][0])/z-l)/(r-l))-1;x1=2*(((hx-offsets[eye][0])/z-l)/(r-l))-1;y0=2*(((-hy+cy-offsets[eye][1])/z-b)/(u-b))-1;y1=2*(((hy+cy-offsets[eye][1])/z-b)/(u-b))-1;}
            float opacity=layer?1: hudOpacity;glColor4f(opacity,opacity,opacity,opacity);
            glBindTexture(GL_TEXTURE_2D,hudTextures[layer]);hudBegin(GL_QUADS);glTexCoord2f(0,0);hudVertex2f(x0,y0);glTexCoord2f(1,0);hudVertex2f(x1,y0);glTexCoord2f(1,1);hudVertex2f(x1,y1);glTexCoord2f(0,1);hudVertex2f(x0,y1);hudEnd();
        }
    }
    bindFBO(GL_FRAMEBUFFER,previous);glMatrixMode(GL_MODELVIEW);glPopMatrix();glMatrixMode(GL_PROJECTION);glPopMatrix();glMatrixMode(GL_TEXTURE);glPopMatrix();glMatrixMode(mode);glPopAttrib();
}
