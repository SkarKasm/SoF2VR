/* SPDX-License-Identifier: GPL-3.0-or-later
 * Wheel behavior adapted from Brutally RealRTCW VR / RealRTCW-VR:
 * Original VR wheel implementation: RealRTCW-VR contributors.
 * RTCW SP source portions: Copyright (C) 1999-2010 id Software LLC.
 * SoF2 adaptation: hold/release handling, bounded data, inventory integration.
 * GPL v3 or later with the retained RTCW additional terms; provided without
 * warranty. See licenses/RealRTCW-VR-GPL-3.0.txt.
 * No hand models, textures or other RealRTCW media are included.
 */
#include "weapon_wheel.h"
#include <string.h>
#include <math.h>
void Wheel_Slot(int i,int n,float *x,float *y){
    static const float three[3][2]={{0,-1},{.85f,.55f},{-.85f,.55f}};
    static const float four[4][2]={{0,-1},{1,0},{0,1},{-1,0}};
    if(n==1){*x=0;*y=-1;}else if(n==2){*x=i?1:-1;*y=0;}
    else if(n==3){*x=three[i][0];*y=three[i][1];}
    else if(n==4){*x=four[i][0];*y=four[i][1];}
    else{float a=(float)i/n*6.28318530718f+3.14159265359f/n-1.57079632679f;*x=cosf(a);*y=sinf(a);}
}
void Wheel_SetInventory(WeaponWheel *w,const WheelItem *items,int n){
    if(n<0)n=0;if(n>WHEEL_MAX_ITEMS)n=WHEEL_MAX_ITEMS;
    w->inventoryCount=n;if(n)memcpy(w->items,items,n*sizeof(*items));
    w->pages=(n+WHEEL_PAGE_SIZE-1)/WHEEL_PAGE_SIZE+!w->weaponsOnly;if(!w->pages)w->pages=1;
    if(w->page<0 || w->page>=w->pages){w->page=0;w->hover=0;}
}
int Wheel_PageItems(const WeaponWheel *w,WheelItem out[WHEEL_PAGE_SIZE]){
    if(!w->weaponsOnly && w->page==w->pages-1){
        out[0]=(WheelItem){WHEEL_SAVE,0,0,1,"Quick Save"};out[1]=(WheelItem){WHEEL_LOAD,0,0,w->loadAvailable,"Quick Load"};return 2;
    }
    int first=w->page*WHEEL_PAGE_SIZE,n=w->inventoryCount-first;if(n>WHEEL_PAGE_SIZE)n=WHEEL_PAGE_SIZE;if(n<0)n=0;
    if(n)memcpy(out,w->items+first,n*sizeof(*out));return n;
}
void Wheel_Cancel(WeaponWheel *w){w->open=0;w->hover=0;w->quiet=1;w->gripArmed=0;w->gripStart=0;w->used=1;w->triggerArmed=0;}
WheelEvent Wheel_Update(WeaponWheel *w,const WheelInput *in){
    WheelEvent e={0};
    if(!in->valid || !isfinite(in->hand[0]) || !isfinite(in->hand[1]) || !isfinite(in->stick) || !isfinite(in->stickY)){e.closed=w->open;e.consume=w->open;Wheel_Cancel(w);w->buttons[0]=in->buttons[0];w->buttons[1]=in->buttons[1];return e;}
    unsigned pressed=(in->buttons[0]&~w->buttons[0])|(in->buttons[1]&~w->buttons[1]);w->buttons[0]=in->buttons[0];w->buttons[1]=in->buttons[1];
    if(hypotf(in->stick,in->stickY)<.3f)w->quiet=0;
    if(!in->trigger)w->triggerArmed=1;
    if(!in->grip){
        if(w->open){e.action=in->holdConfirm?0:w->hover;e.closed=e.consume=1;w->open=0;w->quiet=1;}
        /* Grip release never reloads or ejects a magazine. */
        w->gripStart=0;w->gripArmed=1;w->used=0;return e;
    }
    /* Face buttons cannot cancel a simultaneous grip-open request. They
     * remain reserved for gameplay once the grip/wheel gesture finishes. */
    (void)pressed;
    if(!w->open){
        if(!w->gripArmed || w->used || in->trigger)return e;
        if(!w->pages)return e;
        w->gripStart=in->now;
        w->open=w->used=1;w->point[0]=w->point[1]=0;memcpy(w->start,in->hand,sizeof(w->start));w->paged=0;w->hover=0;e.opened=1;
    }
    e.consume=1;
    if(in->trigger && w->triggerArmed){e.action=w->hover;e.closed=1;w->open=0;w->quiet=1;w->triggerArmed=0;return e;}
    int pagePress=in->stickSelect?(pressed&4u)!=0:fabsf(in->stick)>.7f;
    if(!w->paged && pagePress){w->page=(w->page+(in->stickSelect || in->stick>0?1:-1)+w->pages)%w->pages;w->paged=1;w->hover=0;memcpy(w->start,in->hand,sizeof(w->start));w->point[0]=w->point[1]=0;e.pageChanged=1;}
    else if(in->stickSelect?!pagePress:fabsf(in->stick)<.3f)w->paged=0;
    if(in->stickSelect){w->point[0]=in->stick;w->point[1]=in->stickY;}
    else for(int i=0;i<2;++i)w->point[i]+=((in->hand[i]-w->start[i])/.10f-w->point[i])*.35f;
    float x=w->point[0],y=-w->point[1],len=sqrtf(x*x+y*y);
    if(len>1){x/=len;y/=len;}
    WheelItem items[WHEEL_PAGE_SIZE];int n=Wheel_PageItems(w,items);w->count=n;
    if(len>=.3f && n){
        float angle=atan2f(y,x),best=100,cur=100;int pick=0;
        for(int i=0;i<n;++i){float sx,sy;Wheel_Slot(i,n,&sx,&sy);float d=fabsf(atan2f(sinf(atan2f(sy,sx)-angle),cosf(atan2f(sy,sx)-angle)))*57.2957795f;
            if(d<best){best=d;pick=items[i].available?items[i].id:0;}if(items[i].id==w->hover)cur=d;
        }
        if(cur-best<12)pick=w->hover;
        e.changed=pick!=w->hover;w->hover=pick;
    }
    return e;
}
