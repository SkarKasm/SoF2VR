/* Holster selection and possession share one grip edge stream. Tracking loss
 * cancels gestures, and never fabricates a drop or a throw velocity. */
static void HandInBody(const BridgeTracking *sample,int hand,float point[3]){
    float centre[3],q[4],inverse[4],delta[3];BodyFrame(sample,centre,q,inverse);
    for(int i=0;i<3;++i)delta[i]=sample->input.hands[hand].grip.position[i]-centre[i];
    Rotate(inverse,delta,point);
}
static VRInventoryItem holsterItems[WHEEL_MAX_ITEMS];
static int holsterItemCount;
static unsigned HolsterInventory(int force){
    static DWORD checked;static unsigned epoch,slots;DWORD now=GetTickCount();
    if(force || !checked || now-checked>=100 || epoch!=reloadEpoch){
        checked=now;epoch=reloadEpoch;slots=0;
        holsterItemCount=GameVR_Inventory(holsterItems,WHEEL_MAX_ITEMS);
        for(int i=0;i<holsterItemCount;++i){
            const VRWeaponProfile *profile=VR_Profile(holsterItems[i].id);
            if(profile && holsterItems[i].available)slots|=1u<<VR_HolsterCategory(profile->category);
        }
    }
    return slots;
}
static int HolsterAt(const float point[3],unsigned slots){
    return VR_HolsterAtOccupied(point,leftHanded,accessibility.holsterReach*.01f,accessibility.holsterHeight*.01f,accessibility.seated,slots);
}
static int NearHolster(const BridgeTracking *sample){
    float point[3];HandInBody(sample,leftHanded?0:1,point);
    return HolsterAt(point,HolsterInventory(0));
}
static void UpdateOffhandKnife(const BridgeTracking *sample,int playable){
    static int grip,armed;static unsigned epoch;
    int hand=leftHanded?1:0;const BridgeHand *off=&sample->input.hands[hand];
    VRWeaponState state;const VRWeaponProfile *profile=GameVR_GetWeaponState(&state)?VR_Profile(state.weapon):NULL;
    int canHold=(!weaponHeld || (profile && profile->category==2)) && epoch==reloadEpoch;
    if(!playable || !canHold || (off->valid&3)!=3){
        if(offhandKnife){offhandKnife=knifeReverse=0;ResetKnifeMotion();}
        grip=armed=0;epoch=reloadEpoch;return;
    }
    int closed=off->squeeze>(grip?GripOff():GripOn()),pressed=closed && !grip;
    grip=closed;if(!closed)armed=1;
    int held=offhandKnife;
    if(held){
        if(accessibility.toggleGrip?pressed:!closed)held=0;
    }else if(armed && pressed && !weaponWheel.open && !magazine.stage && !magazine.boltHeld &&
             (HolsterInventory(1)&(1u<<VR_HOLSTER_KNIFE))){
        float point[3];HandInBody(sample,hand,point);
        held=HolsterAt(point,1u<<VR_HOLSTER_KNIFE)==VR_HOLSTER_KNIFE;
    }
    if(held!=offhandKnife){
        offhandKnife=held;knifeReverse=0;ResetKnifeMotion();ClearDroppedWeapon(1);
        LOG_INFO("KNIFE offhand=%d hand=%d main=%d",held,hand,profile?profile->id:0);
    }
}
static int UpdateWheel(const BridgeTracking *sample,int playable,int *tapReload){
    static int previousSlot;
    const BridgeHand *dom=&sample->input.hands[leftHanded?0:1];WheelInput in={0};
    VRWeaponState weapon={0};in.now=GetTickCount();in.valid=playable;
    int haveWeapon=in.valid && GameVR_GetWeaponState(&weapon);
    *tapReload=0;HolsterPreferences(0);
    in.stick=sample->input.hands[1].stick[0];in.stickY=sample->input.hands[1].stick[1];
    in.buttons[1]=sample->input.hands[1].buttons;in.stickSelect=in.holdConfirm=1;
    in.trigger=dom->trigger>TriggerOn();
    int physical=in.valid && dom->squeeze>(gripPhysical?GripOff():GripOn());
    int press=0,release=0;
    if(!in.valid){gripSampled=gripPhysical=gripRearm=0;weaponMotion.time=0;}
    else{
        if(!gripSampled){gripSampled=1;gripPhysical=physical;gripRearm=!physical;}
        else{press=physical && !gripPhysical && gripRearm;release=!physical && gripPhysical && gripRearm;gripPhysical=physical;}
        if(!physical)gripRearm=1;
    }
    TrackWeaponMotion(sample,in.valid);
    float point[3];HandInBody(sample,leftHanded?0:1,point);
    unsigned occupied=in.valid?HolsterInventory(press):0;
    int nearSlot=HolsterAt(point,occupied);
    in.hand[0]=point[0];in.hand[1]=point[1];
    if(in.valid && weaponHeld && ((release && !accessibility.toggleGrip) ||
       (press && accessibility.toggleGrip && nearSlot!=VR_HOLSTER_KNIFE))){
        const VRWeaponProfile *profile=VR_Profile(weapon.weapon);
        if(profile && nearSlot && VR_HolsterCategory(profile->category)==nearSlot){
            holsters.selected[nearSlot]=weapon.weapon;HolsterPreferences(nearSlot);
            ClearDroppedWeapon(weapon.weapon);weaponHeld=weaponSupported=0;
            GameVR_CancelWeaponRequest();GameVR_CancelRoundInsertion();ResetKnifeMotion();WheelHaptic(.25f,30);
        }else if(haveWeapon)DropHeldWeapon(sample);
        else{weaponHeld=weaponSupported=0;GameVR_CancelWeaponRequest();}
        press=0;
    }
    if(press && !weaponHeld && !nearSlot && GrabDroppedWeapon(sample)){
        holsters.armed=0;Wheel_Cancel(&weaponWheel);PublishWheel();return 1;
    }
    VRHolsterEvent h=VR_HolsterUpdate(&holsters,in.valid && !weaponHeld,physical && gripRearm,nearSlot,in.now);
    /* A carried weapon can only be replaced by the direct knife draw. */
    const VRWeaponProfile *heldProfile=haveWeapon?VR_Profile(weapon.weapon):NULL;
    if(!offhandKnife && heldProfile && weaponHeld && physical && nearSlot==VR_HOLSTER_KNIFE && (press || previousSlot!=nearSlot) &&
       heldProfile->category!=1 && heldProfile->category!=2)
        h.drawSlot=VR_HOLSTER_KNIFE;
    previousSlot=nearSlot;
    static int context;static DWORD inventoryTime;
    int next=h.wheelSlot;
    if(next!=context){
        Wheel_Cancel(&weaponWheel);weaponWheel.gripArmed=1;weaponWheel.used=0;weaponWheel.page=0;
        context=next;weaponWheel.weaponsOnly=0;inventoryTime=0;
    }
    int draw=0;
    if(in.valid && (!weaponWheel.pages || !inventoryTime || in.now-inventoryTime>=250 || h.drawSlot || press)){
        WheelItem items[WHEEL_MAX_ITEMS];int n=0;
        for(int i=0;i<holsterItemCount;++i){
            const VRInventoryItem *native=&holsterItems[i];
            const VRWeaponProfile *p=VR_Profile(native->id);if(!p)continue;
            int slot=VR_HolsterCategory(p->category);
            if(h.drawSlot==slot && native->available && (!draw || native->id==holsters.selected[slot]))draw=native->id;
            if(!context || slot!=context)continue;
            items[n]=(WheelItem){native->id,native->clip,native->reserve,native->available,""};
            memcpy(items[n++].name,native->name,sizeof(items[0].name));
        }
        Wheel_SetInventory(&weaponWheel,items,n);weaponWheel.loadAvailable=WheelHasSave();inventoryTime=in.now;
    }
    if(h.drawSlot){
        if(draw && GameVR_RequestWeapon(draw)){
            ClearDroppedWeapon(draw);weaponHeld=1;holsters.selected[h.drawSlot]=draw;
            HolsterPreferences(h.drawSlot);WheelHaptic(.4f,35);
            LOG_INFO("HOLSTER draw slot=%d weapon=%d",h.drawSlot,draw);
        }
        holsters.held=holsters.slot=holsters.wheel=holsters.armed=0;
        Wheel_Cancel(&weaponWheel);PublishWheel();return 1;
    }
    in.grip=!weaponHeld && context && physical && gripRearm && weaponWheel.inventoryCount>0;
    WheelEvent event=Wheel_Update(&weaponWheel,&in);
    if(event.opened){
        GameVR_CloseNativeInventory();GameVR_CancelWeaponRequest();WheelHaptic(.3f,30);
        for(int i=0;i<weaponWheel.inventoryCount;++i)if(weaponWheel.items[i].id==holsters.selected[context] && weaponWheel.items[i].available){
            weaponWheel.hover=weaponWheel.items[i].id;weaponWheel.page=i/WHEEL_PAGE_SIZE;
        }
        LOG_INFO("HOLSTER wheel slot=%d count=%d",context,weaponWheel.inventoryCount);
    }
    if(event.pageChanged)WheelHaptic(.4f,25);
    if(event.changed)WheelHaptic(.2f,15);
    if(event.action==WHEEL_SAVE)addCommand("\nquicksave\n");
    else if(event.action==WHEEL_LOAD && WheelHasSave()){++reloadEpoch;GameVR_CancelRoundInsertion();addCommand("\nquickload\n");}
    else if(event.action>0){
        const VRWeaponProfile *p=VR_Profile(event.action);
        if(context && p && VR_HolsterCategory(p->category)==context && GameVR_RequestWeapon(event.action)){
            ClearDroppedWeapon(event.action);
            holsters.selected[context]=event.action;HolsterPreferences(context);weaponHeld=1;
        }
    }
    if(event.closed && physical)holsters.held=holsters.slot=holsters.wheel=holsters.armed=0;
    if(!in.valid)GameVR_CancelWeaponRequest();
    PublishWheel();return event.consume || weaponWheel.open || (h.consume && !h.wheelSlot && holsters.slot!=VR_HOLSTER_KNIFE);
}
