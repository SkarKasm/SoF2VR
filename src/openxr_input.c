#include "openxr_input.h"
#include "proxy_dll.h"
#include "openxr_core.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static XrActionSet g_actionSet = XR_NULL_HANDLE;
static XrAction g_actionFire = XR_NULL_HANDLE;
static XrAction g_actionReload = XR_NULL_HANDLE;
static XrAction g_actionMoveX = XR_NULL_HANDLE;
static XrAction g_actionMoveY = XR_NULL_HANDLE;
static XrAction g_actionAimPose = XR_NULL_HANDLE;
static XrAction g_actionGripPose = XR_NULL_HANDLE;
static XrAction g_actionSnapTurn = XR_NULL_HANDLE;
static XrAction g_actionMenu = XR_NULL_HANDLE;
static XrSpace g_aimSpace = XR_NULL_HANDLE;
static XrSpace g_gripSpace = XR_NULL_HANDLE;

static XrPath g_pathLeftHand = XR_NULL_PATH;
static XrPath g_pathRightHand = XR_NULL_PATH;
static XrPath g_pathGamepad = XR_NULL_PATH;

static VRInputState g_inputState = {0};
static VRInputState g_prevInputState = {0};
static void OpenXR_Input_SuggestBindings(void);
bool OpenXR_Input_Init(void) {
    if (!OpenXR_IsInitialized()) return false;

    XrInstance instance = OpenXR_GetInstance();
    if (!instance) return false;

    XrActionSetCreateInfo actionSetInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(actionSetInfo.actionSetName, "sof2_gameplay");
    strcpy(actionSetInfo.localizedActionSetName, "SoF2 VR Gameplay");
    actionSetInfo.priority = 0;
    XrResult result = pfn_xrCreateActionSet(instance, &actionSetInfo, &g_actionSet);
    if (XR_FAILED(result)) {
        LOG_ERROR("xrCreateActionSet failed: %d", result);
        return false;
    }

    pfn_xrStringToPath(instance, "/user/hand/left", &g_pathLeftHand);
    pfn_xrStringToPath(instance, "/user/hand/right", &g_pathRightHand);
    pfn_xrStringToPath(instance, "/user/gamepad", &g_pathGamepad);

    XrActionCreateInfo actionInfo = {XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    actionInfo.countSubactionPaths = 2;
    actionInfo.subactionPaths = (XrPath[]){g_pathLeftHand, g_pathRightHand};

    strcpy(actionInfo.actionName, "fire");
    strcpy(actionInfo.localizedActionName, "Fire Weapon");
    pfn_xrCreateAction(g_actionSet, &actionInfo, &g_actionFire);

    strcpy(actionInfo.actionName, "reload");
    strcpy(actionInfo.localizedActionName, "Reload Weapon");
    pfn_xrCreateAction(g_actionSet, &actionInfo, &g_actionReload);

    strcpy(actionInfo.actionName, "snap_turn");
    strcpy(actionInfo.localizedActionName, "Snap Turn");
    pfn_xrCreateAction(g_actionSet, &actionInfo, &g_actionSnapTurn);

    strcpy(actionInfo.actionName, "menu");
    strcpy(actionInfo.localizedActionName, "Menu");
    pfn_xrCreateAction(g_actionSet, &actionInfo, &g_actionMenu);

    actionInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    strcpy(actionInfo.actionName, "move");
    strcpy(actionInfo.localizedActionName, "Move");
    pfn_xrCreateAction(g_actionSet, &actionInfo, &g_actionMoveX);
    g_actionMoveY = g_actionMoveX;

    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    actionInfo.countSubactionPaths = 1;

    actionInfo.subactionPaths = &g_pathRightHand;
    strcpy(actionInfo.actionName, "aim_pose");
    strcpy(actionInfo.localizedActionName, "Aim Pose");
    pfn_xrCreateAction(g_actionSet, &actionInfo, &g_actionAimPose);

    actionInfo.subactionPaths = &g_pathLeftHand;
    strcpy(actionInfo.actionName, "grip_pose");
    strcpy(actionInfo.localizedActionName, "Grip Pose");
    pfn_xrCreateAction(g_actionSet, &actionInfo, &g_actionGripPose);

    XrActionSpaceCreateInfo spaceInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
    spaceInfo.action = g_actionAimPose;
    spaceInfo.subactionPath = g_pathRightHand;
    result = pfn_xrCreateActionSpace(OpenXR_GetSession(), &spaceInfo, &g_aimSpace);
    if (XR_FAILED(result)) return false;
    spaceInfo.action = g_actionGripPose;
    spaceInfo.subactionPath = g_pathLeftHand;
    result = pfn_xrCreateActionSpace(OpenXR_GetSession(), &spaceInfo, &g_gripSpace);
    if (XR_FAILED(result)) return false;

    OpenXR_Input_SuggestBindings();

    XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &g_actionSet;
    result = pfn_xrAttachSessionActionSets(OpenXR_GetSession(), &attachInfo);
    if (XR_FAILED(result)) {
        LOG_ERROR("xrAttachSessionActionSets failed: %d", result);
        return false;
    }

    LOG_INFO("OpenXR input initialized");
    return true;
}

static void OpenXR_Input_SuggestBindings(void) {
    XrInstance instance = OpenXR_GetInstance();
    XrPath metaProfile;
    pfn_xrStringToPath(instance, "/interaction_profiles/meta/quest_touch_controller", &metaProfile);

    XrPath metaPaths[7];
    pfn_xrStringToPath(instance, "/user/hand/right/input/trigger/value", &metaPaths[0]);
    pfn_xrStringToPath(instance, "/user/hand/right/input/squeeze/value", &metaPaths[1]);
    pfn_xrStringToPath(instance, "/user/hand/left/input/thumbstick", &metaPaths[2]);
    pfn_xrStringToPath(instance, "/user/hand/right/input/grip/pose", &metaPaths[3]);
    pfn_xrStringToPath(instance, "/user/hand/left/input/grip/pose", &metaPaths[4]);
    pfn_xrStringToPath(instance, "/user/hand/right/input/thumbstick/click", &metaPaths[5]);
    pfn_xrStringToPath(instance, "/user/hand/left/input/menu/click", &metaPaths[6]);

    XrActionSuggestedBinding metaBindings[] = {
        {g_actionFire, metaPaths[0]},
        {g_actionReload, metaPaths[1]},
        {g_actionMoveX, metaPaths[2]},
        {g_actionAimPose, metaPaths[3]},
        {g_actionGripPose, metaPaths[4]},
        {g_actionSnapTurn, metaPaths[5]},
        {g_actionMenu, metaPaths[6]},
    };

    XrInteractionProfileSuggestedBinding suggestedBinding = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBinding.interactionProfile = metaProfile;
    suggestedBinding.countSuggestedBindings = 7;
    suggestedBinding.suggestedBindings = metaBindings;
    pfn_xrSuggestInteractionProfileBindings(instance, &suggestedBinding);
}
void OpenXR_Input_Update(void) {
    if (!OpenXR_IsInitialized()) return;

    g_prevInputState = g_inputState;

    XrActiveActionSet activeSet = {g_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    pfn_xrSyncActions(OpenXR_GetSession(), &syncInfo);

    XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};

    getInfo.action = g_actionFire;
    getInfo.subactionPath = g_pathRightHand;
    XrActionStateBoolean fireState = {XR_TYPE_ACTION_STATE_BOOLEAN};
    pfn_xrGetActionStateBoolean(OpenXR_GetSession(), &getInfo, &fireState);
    g_inputState.fire = fireState.isActive && fireState.currentState;

    getInfo.action = g_actionReload;
    XrActionStateBoolean reloadState = {XR_TYPE_ACTION_STATE_BOOLEAN};
    pfn_xrGetActionStateBoolean(OpenXR_GetSession(), &getInfo, &reloadState);
    g_inputState.reload = reloadState.isActive && reloadState.currentState;

    getInfo.action = g_actionMoveX;
    getInfo.subactionPath = g_pathLeftHand;
    XrActionStateVector2f moveState = {XR_TYPE_ACTION_STATE_VECTOR2F};
    pfn_xrGetActionStateVector2f(OpenXR_GetSession(), &getInfo, &moveState);
    g_inputState.moveX = moveState.isActive ? moveState.currentState.x : 0.0f;
    g_inputState.moveY = moveState.isActive ? moveState.currentState.y : 0.0f;

    getInfo.action = g_actionSnapTurn;
    getInfo.subactionPath = g_pathRightHand;
    XrActionStateBoolean snapState = {XR_TYPE_ACTION_STATE_BOOLEAN};
    pfn_xrGetActionStateBoolean(OpenXR_GetSession(), &getInfo, &snapState);
    g_inputState.snapTurn = snapState.isActive && snapState.currentState && !(g_prevInputState.snapTurn);

    getInfo.action = g_actionMenu;
    getInfo.subactionPath = g_pathLeftHand;
    XrActionStateBoolean menuState = {XR_TYPE_ACTION_STATE_BOOLEAN};
    pfn_xrGetActionStateBoolean(OpenXR_GetSession(), &getInfo, &menuState);
    g_inputState.menu = menuState.isActive && menuState.currentState && !(g_prevInputState.menu);

    getInfo.action = g_actionAimPose;
    getInfo.subactionPath = g_pathRightHand;
    XrActionStatePose aimPoseState = {XR_TYPE_ACTION_STATE_POSE};
    pfn_xrGetActionStatePose(OpenXR_GetSession(), &getInfo, &aimPoseState);
    XrSpaceLocation aimLocation = {XR_TYPE_SPACE_LOCATION};
    pfn_xrLocateSpace(g_aimSpace, OpenXR_GetStageSpace(), OpenXR_GetPredictedDisplayTime(), &aimLocation);
    if (aimPoseState.isActive &&
        (aimLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (aimLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        g_inputState.aimPose = aimLocation.pose;
        g_inputState.aimPoseValid = true;
    } else {
        g_inputState.aimPoseValid = false;
    }

    getInfo.action = g_actionGripPose;
    getInfo.subactionPath = g_pathLeftHand;
    XrActionStatePose gripPoseState = {XR_TYPE_ACTION_STATE_POSE};
    pfn_xrGetActionStatePose(OpenXR_GetSession(), &getInfo, &gripPoseState);
    XrSpaceLocation gripLocation = {XR_TYPE_SPACE_LOCATION};
    pfn_xrLocateSpace(g_gripSpace, OpenXR_GetStageSpace(), OpenXR_GetPredictedDisplayTime(), &gripLocation);
    if (gripPoseState.isActive &&
        (gripLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (gripLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        g_inputState.gripPose = gripLocation.pose;
        g_inputState.gripPoseValid = true;
    } else {
        g_inputState.gripPoseValid = false;
    }
}

void OpenXR_Input_ApplyToGame(void) {
    if (!OpenXR_IsInitialized()) return;

    if (g_inputState.fire && !g_prevInputState.fire) {
        keybd_event(VK_LCONTROL, 0, 0, 0);
    }
    if (!g_inputState.fire && g_prevInputState.fire) {
        keybd_event(VK_LCONTROL, 0, KEYEVENTF_KEYUP, 0);
    }

    if (g_inputState.reload && !g_prevInputState.reload) {
        keybd_event(0x52, 0, 0, 0);
        keybd_event(0x52, 0, KEYEVENTF_KEYUP, 0);
    }

    if (fabsf(g_inputState.moveX) > 0.1f) {
        int dx = (int)(g_inputState.moveX * 10.0f);
        mouse_event(MOUSEEVENTF_MOVE, dx, 0, 0, 0);
    }
    if (fabsf(g_inputState.moveY) > 0.1f) {
        int dy = (int)(g_inputState.moveY * 10.0f);
        mouse_event(MOUSEEVENTF_MOVE, 0, dy, 0, 0);
    }

    if (g_inputState.snapTurn) {
        mouse_event(MOUSEEVENTF_MOVE, 450, 0, 0, 0);
    }

    if (g_inputState.menu && !g_prevInputState.menu) {
        keybd_event(VK_ESCAPE, 0, 0, 0);
        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    }
}

bool OpenXR_Input_GetAimPose(float* outPos, float* outRot) {
    if (!g_inputState.aimPoseValid) return false;
    if (outPos) {
        outPos[0] = g_inputState.aimPose.position.x;
        outPos[1] = g_inputState.aimPose.position.y;
        outPos[2] = g_inputState.aimPose.position.z;
    }
    if (outRot) {
        outRot[0] = g_inputState.aimPose.orientation.x;
        outRot[1] = g_inputState.aimPose.orientation.y;
        outRot[2] = g_inputState.aimPose.orientation.z;
        outRot[3] = g_inputState.aimPose.orientation.w;
    }
    return true;
}

void OpenXR_Input_Shutdown(void) {
    if (g_aimSpace != XR_NULL_HANDLE) pfn_xrDestroySpace(g_aimSpace);
    if (g_gripSpace != XR_NULL_HANDLE) pfn_xrDestroySpace(g_gripSpace);
    if (g_actionSet != XR_NULL_HANDLE) {
        pfn_xrDestroyActionSet(g_actionSet);
        g_actionSet = XR_NULL_HANDLE;
    }
    LOG_INFO("OpenXR input shutdown");
}
