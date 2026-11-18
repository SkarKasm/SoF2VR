#include "bridge_actions.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
static const char *failure="controller actions";
const char *BridgeActions_Failure(void){return failure;}
#define CHECK(call) do { XrResult bridgeResult_=(call); if(XR_FAILED(bridgeResult_)){failure=#call;return bridgeResult_;} } while(0)

static XrResult Action(BridgeActions *a,const char *name,XrActionType type,XrAction *out) {
    XrActionCreateInfo c={XR_TYPE_ACTION_CREATE_INFO};
    c.actionType=type;c.countSubactionPaths=2;c.subactionPaths=a->hands;
    strcpy(c.actionName,name);strcpy(c.localizedActionName,name);
    CHECK(xrCreateAction(a->set,&c,out));return XR_SUCCESS;
}
XrResult BridgeActions_Create(BridgeActions *a,XrInstance instance,XrSession session) {
    XrActionSetCreateInfo set={XR_TYPE_ACTION_SET_CREATE_INFO};
    /* A new action identity prevents obsolete SteamVR grip/reload/journal
     * overrides from silently replacing the current suggested bindings. */
    strcpy(set.actionSetName,"sof2_controls_v2");strcpy(set.localizedActionSetName,"SoF2 VR controls");
    CHECK(xrStringToPath(instance,"/user/hand/left",&a->hands[0]));
    CHECK(xrStringToPath(instance,"/user/hand/right",&a->hands[1]));
    CHECK(xrCreateActionSet(instance,&set,&a->set));
    CHECK(Action(a,"grip_pose",XR_ACTION_TYPE_POSE_INPUT,&a->grip));
    CHECK(Action(a,"aim_pose",XR_ACTION_TYPE_POSE_INPUT,&a->aim));
    CHECK(Action(a,"trigger",XR_ACTION_TYPE_FLOAT_INPUT,&a->trigger));
    CHECK(Action(a,"grip_wheel_hold",XR_ACTION_TYPE_FLOAT_INPUT,&a->squeeze));
    CHECK(Action(a,"stick",XR_ACTION_TYPE_VECTOR2F_INPUT,&a->stick));
    CHECK(Action(a,"face_a_x",XR_ACTION_TYPE_BOOLEAN_INPUT,&a->primary));
    CHECK(Action(a,"face_b_y",XR_ACTION_TYPE_BOOLEAN_INPUT,&a->secondary));
    CHECK(Action(a,"stick_click",XR_ACTION_TYPE_BOOLEAN_INPUT,&a->click));
    CHECK(Action(a,"menu",XR_ACTION_TYPE_BOOLEAN_INPUT,&a->menu));
    CHECK(Action(a,"haptic",XR_ACTION_TYPE_VIBRATION_OUTPUT,&a->haptic));
    XrActionSuggestedBinding bindings[24];uint32_t count=0;
    for(int h=0;h<2;++h){
        const char *side=h?"right":"left";char path[128];XrPath p;
#define BIND(action,suffix) do{snprintf(path,sizeof(path),"/user/hand/%s/%s",side,suffix);CHECK(xrStringToPath(instance,path,&p));bindings[count++]=(XrActionSuggestedBinding){a->action,p};}while(0)
        BIND(grip,"input/grip/pose");BIND(aim,"input/aim/pose");
        BIND(trigger,"input/trigger/value");BIND(squeeze,"input/squeeze/value");
        BIND(stick,"input/thumbstick");BIND(click,"input/thumbstick/click");
        BIND(primary,h?"input/a/click":"input/x/click");
        BIND(secondary,h?"input/b/click":"input/y/click");
        if(!h){BIND(menu,"input/menu/click");}
        BIND(haptic,"output/haptic");
#undef BIND
        XrActionSpaceCreateInfo space={XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space.poseInActionSpace.orientation.w=1;space.subactionPath=a->hands[h];space.action=a->grip;
        CHECK(xrCreateActionSpace(session,&space,&a->gripSpace[h]));
        space.action=a->aim;CHECK(xrCreateActionSpace(session,&space,&a->aimSpace[h]));
    }
    XrInteractionProfileSuggestedBinding suggest={XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    CHECK(xrStringToPath(instance,"/interaction_profiles/oculus/touch_controller",&suggest.interactionProfile));
    suggest.countSuggestedBindings=count;suggest.suggestedBindings=bindings;
    CHECK(xrSuggestInteractionProfileBindings(instance,&suggest));
    XrSessionActionSetsAttachInfo attach={XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets=1;attach.actionSets=&a->set;CHECK(xrAttachSessionActionSets(session,&attach));
    return XR_SUCCESS;
}
static XrResult Pose(XrSession session,XrAction action,XrPath path,XrSpace space,
    XrSpace local,XrTime time,BridgePose *pose,uint32_t *valid,uint32_t flag) {
    XrActionStateGetInfo get={XR_TYPE_ACTION_STATE_GET_INFO};get.action=action;get.subactionPath=path;
    XrActionStatePose active={XR_TYPE_ACTION_STATE_POSE};CHECK(xrGetActionStatePose(session,&get,&active));
    if(!active.isActive)return XR_SUCCESS;
    XrSpaceLocation loc={XR_TYPE_SPACE_LOCATION};CHECK(xrLocateSpace(space,local,time,&loc));
    XrSpaceLocationFlags required=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if((loc.locationFlags&required)==required){
        memcpy(pose,&loc.pose,sizeof(*pose));*valid|=flag;
    }
    return XR_SUCCESS;
}
static XrResult Float(XrSession session,XrAction action,XrPath path,float *out) {
    XrActionStateGetInfo get={XR_TYPE_ACTION_STATE_GET_INFO};get.action=action;get.subactionPath=path;
    XrActionStateFloat state={XR_TYPE_ACTION_STATE_FLOAT};CHECK(xrGetActionStateFloat(session,&get,&state));
    if(state.isActive && isfinite(state.currentState))*out=fmaxf(0,fminf(1,state.currentState));
    return XR_SUCCESS;
}
static XrResult Button(XrSession session,XrAction action,XrPath path,uint32_t *buttons,uint32_t flag) {
    XrActionStateGetInfo get={XR_TYPE_ACTION_STATE_GET_INFO};get.action=action;get.subactionPath=path;
    XrActionStateBoolean state={XR_TYPE_ACTION_STATE_BOOLEAN};CHECK(xrGetActionStateBoolean(session,&get,&state));
    if(state.isActive && state.currentState)*buttons|=flag;
    return XR_SUCCESS;
}
XrResult BridgeActions_Sync(BridgeActions *a,XrSession session,XrSpace local,XrTime time,int focused,BridgeInput *out) {
    memset(out,0,sizeof(*out));out->sampleTimeMs=GetTickCount();
    XrActiveActionSet active={a->set,XR_NULL_PATH};
    XrActionsSyncInfo sync={XR_TYPE_ACTIONS_SYNC_INFO};sync.countActiveActionSets=1;sync.activeActionSets=&active;
    XrResult r=xrSyncActions(session,&sync);
    if(r==XR_SESSION_NOT_FOCUSED || !focused)return XR_SUCCESS;
    CHECK(r);out->focused=1;
    for(int h=0;h<2;++h){
        BridgeHand *hand=&out->hands[h];XrPath path=a->hands[h];
        CHECK(Pose(session,a->grip,path,a->gripSpace[h],local,time,&hand->grip,&hand->valid,BRIDGE_HAND_GRIP_VALID));
        CHECK(Pose(session,a->aim,path,a->aimSpace[h],local,time,&hand->aim,&hand->valid,BRIDGE_HAND_AIM_VALID));
        CHECK(Float(session,a->trigger,path,&hand->trigger));CHECK(Float(session,a->squeeze,path,&hand->squeeze));
        XrActionStateGetInfo get={XR_TYPE_ACTION_STATE_GET_INFO};get.action=a->stick;get.subactionPath=path;
        XrActionStateVector2f stick={XR_TYPE_ACTION_STATE_VECTOR2F};CHECK(xrGetActionStateVector2f(session,&get,&stick));
        if(stick.isActive && isfinite(stick.currentState.x) && isfinite(stick.currentState.y)){
            hand->stick[0]=fmaxf(-1,fminf(1,stick.currentState.x));hand->stick[1]=fmaxf(-1,fminf(1,stick.currentState.y));
        }
        CHECK(Button(session,a->primary,path,&hand->buttons,BRIDGE_BUTTON_PRIMARY));
        CHECK(Button(session,a->secondary,path,&hand->buttons,BRIDGE_BUTTON_SECONDARY));
        CHECK(Button(session,a->click,path,&hand->buttons,BRIDGE_BUTTON_STICK));
        CHECK(Button(session,a->menu,path,&hand->buttons,BRIDGE_BUTTON_MENU));
    }
    return XR_SUCCESS;
}
XrResult BridgeActions_Haptic(BridgeActions *a,XrSession session,const BridgeHaptic *request,int focused) {
    if(!request->serial || request->serial==a->hapticSerial)return XR_SUCCESS;
    a->hapticSerial=request->serial;
    if(!focused || request->hand>1 || !isfinite(request->amplitude) || !isfinite(request->frequency))return XR_SUCCESS;
    XrHapticActionInfo info={XR_TYPE_HAPTIC_ACTION_INFO};info.action=a->haptic;info.subactionPath=a->hands[request->hand];
    XrHapticVibration vibration={XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude=fmaxf(0,fminf(1,request->amplitude));vibration.frequency=fmaxf(0,request->frequency);
    vibration.duration=(XrDuration)(request->durationMs>1000?1000:request->durationMs)*1000000;
    if(!vibration.duration)vibration.duration=XR_MIN_HAPTIC_DURATION;
    return xrApplyHapticFeedback(session,&info,(const XrHapticBaseHeader *)&vibration);
}
void BridgeActions_Destroy(BridgeActions *a) {
    for(int h=0;h<2;++h){if(a->gripSpace[h])xrDestroySpace(a->gripSpace[h]);if(a->aimSpace[h])xrDestroySpace(a->aimSpace[h]);}
    if(a->set)xrDestroyActionSet(a->set);
    memset(a,0,sizeof(*a));
}
