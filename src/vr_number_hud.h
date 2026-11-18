/* The head and weapon anchors are shared by the two eyes. Each eye projects
 * them with its own origin and frustum, including asymmetric headset lenses. */
static int ProjectHudPoint(const GameRefdef *eye,const BridgeView *view,const float point[3],float *x,float *y){
    float forward=0,left=0,up=0;
    for(int i=0;i<3;++i){
        float delta=point[i]-eye->origin[i];
        forward+=delta*eye->axis[0][i];left+=delta*eye->axis[1][i];up+=delta*eye->axis[2][i];
    }
    if(forward<4)return 0;
    float l=tanf(view->fov[0]),r=tanf(view->fov[1]);
    float b=tanf(view->fov[3]),t=tanf(view->fov[2]);
    *x=2*(-left/forward-l)/(r-l)-1;*y=2*(up/forward-b)/(t-b)-1;
    return isfinite(*x) && isfinite(*y);
}

static GLuint numberFont;
static int numberFontHeight,numberFontWidth;
static int PrepareNumberFont(void){
    static HGLRC context;
    HGLRC current=wglGetCurrentContext();
    if(current!=context){context=current;numberFont=0;numberFontHeight=0;}
    int height=(int)fmaxf(16,fminf(48,eyeHeight*.022f));
    if(numberFont && height==numberFontHeight)return 1;
    if(numberFont){glDeleteLists(numberFont,11);numberFont=0;}
    HDC dc=wglGetCurrentDC();
    if(!dc)return 0;
    HFONT font=CreateFontA(-height,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,ANSI_CHARSET,
                          OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,FIXED_PITCH|FF_MODERN,"Consolas");
    if(!font)return 0;
    HGDIOBJ old=SelectObject(dc,font);
    SIZE width={0};GetTextExtentPoint32A(dc,"0",1,&width);
    GLuint lists=glGenLists(11);
    int ready=lists && wglUseFontBitmapsA(dc,'/',11,lists) && width.cx>0;
    SelectObject(dc,old);DeleteObject(font);
    if(!ready){if(lists)glDeleteLists(lists,11);return 0;}
    numberFont=lists;numberFontHeight=height;numberFontWidth=width.cx;
    return 1;
}

static void DrawHudNumber(const char *text,float x,float y,int alignRight,const float color[3]){
    GLsizei count=(GLsizei)strlen(text);
    float width=2.f*numberFontWidth*count/eyeWidth;
    if(alignRight)x-=width;
    if(x<-.98f || x+width>.98f || y<-.98f || y>.93f)return;
    glListBase(numberFont-'/');
    glColor4f(0,0,0,.85f);
    glRasterPos2f(x+2.f/eyeWidth,y-2.f/eyeHeight);glCallLists(count,GL_UNSIGNED_BYTE,text);
    glColor4f(color[0],color[1],color[2],1);
    glRasterPos2f(x,y);glCallLists(count,GL_UNSIGNED_BYTE,text);
}


typedef struct {
    char text[2][32];
    float x[2],y[2];
    int rows,rightAligned;
} NumberHudBlock;
static NumberHudBlock statusHud,ammoHud;

static float HudBlockWidth(const NumberHudBlock *block){
    size_t length=strlen(block->text[0]);
    if(block->rows==2 && strlen(block->text[1])>length)length=strlen(block->text[1]);
    return 2.f*numberFontWidth*length/eyeWidth;
}

static int FitHudBlock(NumberHudBlock *block,const GameRefdef eyes[2],const float point[3],
                       const NumberHudBlock *avoid){
    float width=HudBlockWidth(block),row=2.5f*numberFontHeight/eyeHeight;
    float ascent=2.f*numberFontHeight/eyeHeight;
    float x[2],y[2];
    for(int eye=0;eye<2;++eye){
        if(!ProjectHudPoint(&eyes[eye],&tracking.views[eye],point,&x[eye],&y[eye]))return 0;
        float left=x[eye]-(block->rightAligned?width:0);
        float bottom=y[eye]-(block->rows-1)*row;
        if(left<-.80f || left+width>.80f || bottom<-.76f || y[eye]+ascent>.76f)return 0;
        if(avoid && avoid->rows && left<avoid->x[eye]+HudBlockWidth(avoid)+.035f &&
           left+width>avoid->x[eye]-.035f && bottom<avoid->y[eye]+ascent+.035f &&
           y[eye]+ascent>avoid->y[eye]-row-.035f)return 0;
    }
    memcpy(block->x,x,sizeof(x));memcpy(block->y,y,sizeof(y));
    return 1;
}

static void HeadHudPoint(const GameRefdef *head,float left,float up,float point[3]){
    for(int i=0;i<3;++i)point[i]=head->origin[i]+78.74f*
        (head->axis[0][i]+left*head->axis[1][i]+up*head->axis[2][i]);
}

static void PrepareNumberHud(const GameRefdef eyes[2], const GameRefdef *head){
    statusHud.rows=ammoHud.rows=0;
    if(!controlsEnabled || !tracking.input.focused || weaponWheel.open || *(unsigned *)(gameBase+0x1476d4))return;
    int health,armor;
    if(!GameVR_GetVitals(&health,&armor) || !PrepareNumberFont())return;

    float horizontal=fminf(-tanf(tracking.views[0].fov[0]),-tanf(tracking.views[1].fov[0]));
    float right=fminf(tanf(tracking.views[0].fov[1]),tanf(tracking.views[1].fov[1]));
    float down=fminf(-tanf(tracking.views[0].fov[3]),-tanf(tracking.views[1].fov[3]));
    float status[3];
    HeadHudPoint(head,.48f*horizontal,-.45f*down,status);
    statusHud.rows=2;
    snprintf(statusHud.text[0],sizeof(statusHud.text[0]),"%d",health);
    snprintf(statusHud.text[1],sizeof(statusHud.text[1]),"%d",armor);
    if(!FitHudBlock(&statusHud,eyes,status,NULL)){
        HeadHudPoint(head,.28f*horizontal,-.29f*down,status);
        if(!FitHudBlock(&statusHud,eyes,status,NULL))statusHud.rows=0;
    }

    VRWeaponState weapon={0};
    if(!GameVR_GetWeaponState(&weapon) || weapon.weapon<=1)return;
    int altClip=0,altReserve=0;
    int secondary=GameVR_GetSecondaryAmmo(&weapon,&altClip,&altReserve);
    ammoHud.rows=1+secondary;
    ammoHud.rightAligned=!leftHanded;
    snprintf(ammoHud.text[0],sizeof(ammoHud.text[0]),"%d/%d",weapon.clip,weapon.reserve);
    if(secondary)snprintf(ammoHud.text[1],sizeof(ammoHud.text[1]),"%d/%d",altClip,altReserve);

    float fallback[3],grip[3],axis[3][3];
    HeadHudPoint(head,leftHanded?.19f*horizontal:-.30f*right,-.22f*down,fallback);
    int fits=FitHudBlock(&ammoHud,eyes,fallback,&statusHud);
    int attached=0;
    if(WeaponPose(&playerBase,&tracking,grip,axis)){
        const VRDropProfile *bounds=DropProfile(weapon.weapon);
        float ahead=bounds?fmaxf(1.8f,fminf(5.5f,bounds->half[0]*.45f)):3;
        float side=(leftHanded?-1.f:1.f)*(bounds?fmaxf(3.6f,fminf(7,bounds->half[1]+2.5f)):4.2f);
        float point[3],depth=0;
        for(int i=0;i<3;++i){
            point[i]=grip[i]+ahead*axis[0][i]+side*head->axis[1][i]+.45f*head->axis[2][i];
            depth+=(point[i]-head->origin[i])*head->axis[0][i];
        }
        attached=depth>=16 && FitHudBlock(&ammoHud,eyes,point,&statusHud);
        if(!attached && fits && depth>=16){
            /* Move one world-space anchor inward until both eyes can see
             * the entire block. Independent eye clamps would split the HUD. */
            float lo=0,hi=1;
            for(int step=0;step<8;++step){
                float t=(lo+hi)*.5f,candidate[3];
                for(int i=0;i<3;++i)candidate[i]=fallback[i]+t*(point[i]-fallback[i]);
                if(FitHudBlock(&ammoHud,eyes,candidate,&statusHud))lo=t;
                else hi=t;
            }
        }
    }
    if(!fits && !attached)ammoHud.rows=0;

    static DWORD last;
    if(tracePoses && GetTickCount()-last>1000){
        last=GetTickCount();
        LOG_INFO("NUMBERHUD hp=%d armor=%d weapon=%d visible=%d side=%s ammo=%d/%d secondary=%d %d/%d attached=%d status=%.2f,%.2f/%.2f,%.2f ammoXY=%.2f,%.2f/%.2f,%.2f",
                 health,armor,weapon.weapon,ammoHud.rows!=0,leftHanded?"right":"left",weapon.clip,weapon.reserve,secondary,altClip,altReserve,attached,
                 statusHud.x[0],statusHud.y[0],statusHud.x[1],statusHud.y[1],
                 ammoHud.x[0],ammoHud.y[0],ammoHud.x[1],ammoHud.y[1]);
    }
}

static void DrawNumberHud(int eye){
    if(!statusHud.rows && !ammoHud.rows)return;
    GLint matrix;glGetIntegerv(GL_MATRIX_MODE,&matrix);glPushAttrib(GL_ALL_ATTRIB_BITS);
    if(!hudActiveTexture)hudActiveTexture=(PFNGLACTIVETEXTUREPROC)wglGetProcAddress("glActiveTexture");
    if(hudActiveTexture){
        GLint units=1;glGetIntegerv(GL_MAX_TEXTURE_UNITS,&units);
        for(int i=units>32?31:units-1;i>=0;--i){hudActiveTexture(GL_TEXTURE0+i);
            glDisable(GL_TEXTURE_1D);glDisable(GL_TEXTURE_2D);glDisable(GL_TEXTURE_3D);glDisable(GL_TEXTURE_CUBE_MAP);}
    }
    glDisable(GL_DEPTH_TEST);glDisable(GL_LIGHTING);glDisable(GL_FOG);glDisable(GL_SCISSOR_TEST);
    glDisable(GL_ALPHA_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_CLIP_PLANE0);
    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glColorMask(1,1,1,1);
    glMatrixMode(GL_PROJECTION);glPushMatrix();glLoadIdentity();glMatrixMode(GL_MODELVIEW);glPushMatrix();glLoadIdentity();
    const float red[]={1.f,.29f,.27f},grey[]={.72f,.74f,.76f},olive[]={.69f,.73f,.37f};
    const float row=2.5f*numberFontHeight/eyeHeight;
    if(statusHud.rows){
        DrawHudNumber(statusHud.text[0],statusHud.x[eye],statusHud.y[eye],0,red);
        DrawHudNumber(statusHud.text[1],statusHud.x[eye],statusHud.y[eye]-row,0,grey);
    }
    for(int line=0;line<ammoHud.rows;++line)
        DrawHudNumber(ammoHud.text[line],ammoHud.x[eye],ammoHud.y[eye]-line*row,ammoHud.rightAligned,olive);
    glPopMatrix();glMatrixMode(GL_PROJECTION);glPopMatrix();glMatrixMode(matrix);glPopAttrib();
}
