/* Ballistic aiming adapted from G_VR_LobAt in Brutally RealRTCW VR g_vr.c.
 * Copyright (C) 2026 RealRTCW-VR contributors. GPL-3.0-or-later.
 * Local SoF2 adaptation: bounded/finite inputs and complete back-forward pump.
 * See licenses/RealRTCW-VR-GPL-3.0.txt. No physical-throw velocity is used. */
#include "vr_mechanics.h"
#include <math.h>
int VR_BladeMotion(VRBladeMotion *motion,unsigned now,const VRBladePose *blade,float radius,VRBladePose *previous){
    if(!blade || !isfinite(radius) || radius<.1f){motion->sampled=0;return 0;}
    float heel=0,tip=0;
    for(int i=0;i<3;++i){
        if(!isfinite(blade->heel[i]) || !isfinite(blade->tip[i])){motion->sampled=0;return 0;}
        float a=blade->heel[i]-motion->previous.heel[i],b=blade->tip[i]-motion->previous.tip[i];
        heel+=a*a;tip+=b*b;
    }
    float travel=sqrtf(fmaxf(heel,tip));
    unsigned elapsed=now-motion->time;
    if(motion->sampled && !elapsed)return 0;
    motion->time=now;
    if(!motion->sampled || elapsed>100 || travel>10.0f){
        motion->previous=*blade;motion->sampled=1;return 0;
    }
    /* Accumulate sub-millimetre movement across frames. A slow stab or draw
     * cut only needs contact, with no speed or gesture threshold. */
    if(travel<.04f)return 0;
    *previous=motion->previous;motion->previous=*blade;
    return (int)ceilf(travel/radius);
}
int VR_MagazineEject(VRMagazine *m,int valid){
    if(!valid || m->stage!=VR_MAG_SEATED)return 0;
    *m=(VRMagazine){0};m->stage=VR_MAG_EMPTY;m->gripBefore=1;
    return VR_MAG_OUT;
}
int VR_MagazineUpdate(VRMagazine *m,const VRMagazineInput *in){
    int event=VR_MAG_NONE;
    if(!in->valid || !isfinite(in->extraction) || !isfinite(in->along)){
        if(m->stage==VR_MAG_EXTRACT)m->stage=VR_MAG_SEATED;
        else if(m->stage==VR_MAG_OLD || m->stage==VR_MAG_FRESH)m->stage=VR_MAG_EMPTY;
        m->gripBefore=m->actionBefore=1;m->actionArmed=m->armed=m->boltHeld=m->boltBack=0;m->boltPull=0;return 0;
    }
    if(!in->grip)m->armed=1;
    int press=in->grip && !m->gripBefore && m->armed,release=!in->grip && m->gripBefore;m->gripBefore=in->grip;
    if(!in->action)m->actionArmed=1;
    int actionPress=in->action && !m->actionBefore && m->actionArmed;
    int actionRelease=!in->action && m->actionBefore;m->actionBefore=in->action;
    switch(m->stage){
    case VR_MAG_SEATED:
        /* Magazine removal belongs exclusively to the eject button. */
        if(actionPress && in->nearBolt){m->boltHeld=1;m->boltBack=0;m->boltStart=in->along;m->actionArmed=0;event=VR_MAG_GRAB;}
        break;
    case VR_MAG_EXTRACT:
        if(release){m->stage=VR_MAG_SEATED;event=VR_MAG_RETURN;}
        else if(in->extraction>=.09f){m->stage=VR_MAG_OLD;event=VR_MAG_OUT;}break;
    case VR_MAG_OLD:
        if(release){if(in->nearSocket && in->aligned){m->stage=VR_MAG_SEATED;event=VR_MAG_RETURN;}
            else{m->stage=VR_MAG_EMPTY;event=in->nearPouch?VR_MAG_RETAIN:VR_MAG_DROP;}}break;
    case VR_MAG_EMPTY:
        if(press && in->nearPouch && in->spare){m->stage=VR_MAG_FRESH;m->away=0;m->armed=0;event=VR_MAG_SPARE;}break;
    case VR_MAG_FRESH:
        if(!in->nearSocket)m->away=1;
        if(release){if(m->away && in->nearSocket && in->aligned){m->stage=in->empty?VR_MAG_BOLT:VR_MAG_PENDING;event=VR_MAG_INSERT;}
            else{m->stage=VR_MAG_EMPTY;event=VR_MAG_DROP;}}break;
    case VR_MAG_BOLT:
        if(actionPress && in->nearBolt){m->boltHeld=1;m->boltBack=0;m->boltStart=in->along;m->actionArmed=0;event=VR_MAG_GRAB;}
        break;
    default:break;
    }
    if(m->boltHeld){
            float travel=isfinite(in->actionTravel) && in->actionTravel>=.025f && in->actionTravel<=.15f?in->actionTravel:.07f;
            m->boltPull=fmaxf(0,fminf(1,(m->boltStart-in->along)/travel));if(m->boltPull>.85f)m->boltBack=1;
            if(m->boltBack && (actionRelease || m->boltPull<.15f)){
                event=m->stage==VR_MAG_BOLT?VR_MAG_CHARGE:VR_MAG_RACK;
                if(m->stage==VR_MAG_BOLT)m->stage=VR_MAG_PENDING;
                /* A loaded action may stay in the hand across repeated
                 * back/forward strokes; release the trigger to let go. */
                m->boltHeld=event==VR_MAG_RACK && !actionRelease;m->boltBack=0;m->boltPull=0;
            }else if(actionRelease){m->boltHeld=m->boltBack=0;m->boltPull=0;}
    }
    return event;
}
int VR_LobDirection(const float start[3],const float target[3],float speed,float gravity,float out[3]){
    if(!start || !target || !out || !isfinite(speed) || !isfinite(gravity) || speed<1 || speed>10000 || gravity<0 || gravity>10000)return 0;
    double d[3];for(int i=0;i<3;++i){d[i]=target[i]-start[i];if(!isfinite(d[i]) || fabs(d[i])>65536)return 0;}
    double x=hypot(d[0],d[1]),length=sqrt(x*x+d[2]*d[2]);if(length<.001)return 0;
    if(x<.001 || gravity<.001){for(int i=0;i<3;++i)out[i]=(float)(d[i]/length);return 1;}
    double v2=(double)speed*speed,disc=v2*v2-gravity*(gravity*x*x+2*d[2]*v2);
    double angle=disc<0?.7853981633974483:atan2(v2-sqrt(disc),gravity*x);
    out[0]=(float)(d[0]/x*cos(angle));out[1]=(float)(d[1]/x*cos(angle));out[2]=(float)sin(angle);return disc<0?2:1;
}
void VR_PumpShot(VRPump *p){p->needed=1;p->held=p->back=0;p->pull=0;}
int VR_PumpUpdate(VRPump *p,int valid,int grip,int trigger,int inReach,float along,float travel){
    if(!valid || !isfinite(along) || !isfinite(travel) || travel<=0){p->held=p->back=p->armed=0;p->pull=0;return 0;}
    if(!trigger || !grip){p->held=p->back=0;p->pull=0;p->armed=1;return 0;}
    if(!p->held && p->armed && inReach){p->held=1;p->armed=0;p->start=along;p->back=0;}
    if(!p->held)return 0;
    p->pull=fmaxf(0,fminf(1,(p->start-along)/travel));
    if(p->pull>=.85f)p->back=1;
    if(p->back && p->pull<=.05f){p->needed=p->held=p->back=0;p->pull=0;return 1;}
    return 0;
}

int VR_ArmSolve(const float shoulder[3],const float wrist[3],const float bend[3],float upper,float lower,float elbow[3]){
    if(!isfinite(upper) || !isfinite(lower) || upper<=0 || lower<=0)return 0;
    float direction[3],hint[3],distance=0;
    for(int i=0;i<3;++i){direction[i]=wrist[i]-shoulder[i];if(!isfinite(direction[i]) || !isfinite(bend[i]))return 0;distance+=direction[i]*direction[i];}
    distance=sqrtf(distance);if(distance<fmaxf(.01f,fabsf(upper-lower)+.01f) || distance>(upper+lower)*1.3f)return 0;
    for(int i=0;i<3;++i)direction[i]/=distance;
    int stretched=distance>=upper+lower;float factor=stretched?distance/(upper+lower)*1.001f:1;upper*=factor;lower*=factor;
    float along=(upper*upper-lower*lower+distance*distance)/(2*distance),height=sqrtf(fmaxf(0,upper*upper-along*along));
    float projection=0,length=0;for(int i=0;i<3;++i)projection+=bend[i]*direction[i];
    for(int i=0;i<3;++i){hint[i]=bend[i]-projection*direction[i];length+=hint[i]*hint[i];}
    if(length<.0001f){float fallback[3]={direction[1],-direction[0],0};if(fabsf(direction[2])>.95f){fallback[0]=1;fallback[1]=fallback[2]=0;}for(int i=0;i<3;++i)hint[i]=fallback[i];length=hint[0]*hint[0]+hint[1]*hint[1]+hint[2]*hint[2];}
    length=sqrtf(length);for(int i=0;i<3;++i)elbow[i]=shoulder[i]+direction[i]*along+hint[i]/length*height;
    return stretched?2:1;
}
