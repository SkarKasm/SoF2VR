/* Native RMF menu controls use temporary non-archived CVars. The shared INI
 * remains authoritative for both the launcher and the in-game options. */
static void NativeOptionValue(const char *name,int value){char text[24];snprintf(text,sizeof(text),"%d",value);nativeCvarSet(name,text,0);}
static void SeedNativeVROptions(const VRAccessibility *settings){
    NativeOptionValue("vr_uiReload",0);NativeOptionValue("vr_uiApply",0);NativeOptionValue("vr_uiStatus",0);
    NativeOptionValue("vr_uiEdited",0);nativeCvarSet("vr_uiMessage","Changes are not saved until Save and Apply.",0);
    for(unsigned i=0;i<sizeof(vrAccessibilityOptions)/sizeof(vrAccessibilityOptions[0]);++i){
        const VRAccessibilityOption *option=&vrAccessibilityOptions[i];int value=*(const int *)((const char *)settings+option->offset);
        char name[80];snprintf(name,sizeof(name),"vr_ui_%s",option->key);NativeOptionValue(name,value);
    }
}
static void UpdateNativeVROptions(void){
    static int initialized;
    if(flatMode || !addCommand || !cvarFloat || !nativeCvarSet || !qualityPath[0])return;
    if(!initialized || cvarFloat("vr_uiReload")!=0){
        VRAccessibility saved=VR_ReadAccessibility(qualityPath);SeedNativeVROptions(&saved);initialized=1;return;
    }
    if(cvarFloat("vr_uiEdited")!=0){NativeOptionValue("vr_uiEdited",0);NativeOptionValue("vr_uiStatus",0);nativeCvarSet("vr_uiMessage","Unsaved changes.",0);}
    if(cvarFloat("vr_uiApply")==0)return;
    NativeOptionValue("vr_uiApply",0);
    VRAccessibility next=accessibility;int valid=1;
    for(unsigned i=0;i<sizeof(vrAccessibilityOptions)/sizeof(vrAccessibilityOptions[0]);++i){
        const VRAccessibilityOption *option=&vrAccessibilityOptions[i];char name[80];snprintf(name,sizeof(name),"vr_ui_%s",option->key);
        float raw=cvarFloat(name);
        if(!isfinite(raw) || raw<option->low || raw>option->high || fabsf(raw-roundf(raw))>.001f){valid=0;break;}
        int value=(int)roundf(raw);
        if(!strcmp(option->key,"SnapAngle") && value>0 && value<15){valid=0;break;}
        *(int *)((char *)&next+option->offset)=value;
    }
    if(!valid || !VR_WriteAccessibility(qualityPath,&next)){
        NativeOptionValue("vr_uiStatus",2);nativeCvarSet("vr_uiMessage","Could not save. Check values or folder write access.",0);addCommand("\nrefresh\n");
        LOG_WARN("VR OPTIONS save rejected valid=%d error=%lu",valid,GetLastError());return;
    }
    if(next.leftHanded!=accessibility.leftHanded || next.roomScale!=accessibility.roomScale || next.seated!=accessibility.seated)recentered=0;
    accessibility=next;leftHanded=next.leftHanded;manualReloadEnabled=next.manualReload;manualPump=next.manualPump;snapAngle=next.snapAngle;holsters.holdMs=next.holdMs;
    GameVR_SetEnemyDamagePercent(next.enemyDamage);
    /* Applying happens in a menu. Cancel stale gestures before rearming at
     * controller release; do not interpret the menu trigger as a gunshot. */
    GameVR_CancelRoundInsertion();ResetKnifeMotion();crouchLatched=crouchBefore=0;controlsTime=0;
    NativeOptionValue("vr_uiStatus",1);nativeCvarSet("vr_uiMessage","Settings saved and applied.",0);addCommand("\nrefresh\n");
    LOG_INFO("VR OPTIONS saved/applied from native menu; handed=%d snap=%d seated=%d",leftHanded,next.snapAngle,next.seated);
}
