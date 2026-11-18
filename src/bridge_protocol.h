#ifndef SOF2_BRIDGE_PROTOCOL_H
#define SOF2_BRIDGE_PROTOCOL_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include "weapon_wheel.h"

#define BRIDGE_MAGIC 0x32565253u
#define BRIDGE_VERSION 6u
#define BRIDGE_GPU_SLOTS 3
#define BRIDGE_MAX_WIDTH 2560u
#define BRIDGE_MAX_HEIGHT 2560u
#define BRIDGE_IMAGE_BYTES (BRIDGE_MAX_WIDTH * BRIDGE_MAX_HEIGHT * 4u)
#define BRIDGE_MODE_PANEL 1u
#define BRIDGE_MODE_STEREO 2u
#define BRIDGE_HAND_GRIP_VALID 1u
#define BRIDGE_HAND_AIM_VALID 2u
#define BRIDGE_BUTTON_PRIMARY 1u
#define BRIDGE_BUTTON_SECONDARY 2u
#define BRIDGE_BUTTON_STICK 4u
#define BRIDGE_BUTTON_MENU 8u

/* No native pointers or OpenXR handles cross the x86/x64 boundary. Locks are
 * aligned 32-bit interlocked words. Frame and tracking locks are independent;
 * neither process waits for a lock held by the other process. */
#pragma pack(push, 4)
typedef struct {
    float orientation[4]; /* x, y, z, w */
    float position[3];
    float fov[4];         /* left, right, up, down */
} BridgeView;

typedef struct {
    float orientation[4];
    float position[3];
} BridgePose;

typedef struct {
    uint32_t valid;
    BridgePose grip, aim;
    float trigger, squeeze, stick[2];
    uint32_t buttons;
} BridgeHand;

typedef struct {
    uint32_t serial, sampleTimeMs, focused, reserved;
    BridgeHand hands[2]; /* left, right; LOCAL space, metres */
} BridgeInput;

typedef struct {
    uint32_t serial, hand;
    float amplitude;
    uint32_t durationMs;
    float frequency;
} BridgeHaptic;

typedef struct {
    uint32_t serial;
    uint32_t valid;
    BridgeView views[2];
    BridgeInput input;
    BridgePose panelPose; /* persistent LOCAL-space menu anchor, shared with ray input */
    uint32_t panelValid;
} BridgeTracking;

typedef struct {
    uint32_t serial;
    uint32_t mode;
    uint32_t width;
    uint32_t height;
    BridgeView views[2]; /* poses used to render these pixels */
} BridgeFrame;

typedef struct {
    uint32_t serial,open,page,pages,hover,count,sampleTimeMs;
    float point[2];
    WheelItem items[WHEEL_PAGE_SIZE];
} BridgeWheel;

/* Legacy DXGI shared-resource handles are GPU resource identifiers, not
 * process pointers. Slot ownership: 0 free, 1 producer, 2 ready, 3 consumer. */
typedef struct {
    volatile LONG state;
    uint32_t reserved;
    uint64_t handles[2];
    BridgeFrame frame;
} BridgeGPUFrame;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t structureBytes;
    uint32_t producerPid;
    volatile LONG shutdown;
    volatile LONG helperStatus; /* 0 starting, 1 created, 2 running, <0 XR error */
    volatile LONG frameLock;
    volatile LONG trackingLock;
    /* Written once by the helper before helperStatus becomes running. */
    uint32_t eyeWidth;
    uint32_t eyeHeight;
    volatile LONG hapticLock;
    BridgeHaptic haptic; /* bounded latest-request mailbox; serial acknowledges ownership */
    BridgeTracking tracking;
    BridgeFrame frame;
    volatile LONG wheelLock;
    BridgeWheel wheel;
    unsigned char pixels[2][BRIDGE_IMAGE_BYTES]; /* tightly packed bottom-up BGRA8 */
    volatile LONG gpuStatus; /* 0 not initialized, 1 shared GPU images, -1 fallback */
    uint32_t gpuReserved;
    BridgeGPUFrame gpu[BRIDGE_GPU_SLOTS];
} BridgeShared;
#pragma pack(pop)

typedef char BridgeViewSizeCheck[sizeof(BridgeView) == 44 ? 1 : -1];
typedef char BridgeInputSizeCheck[sizeof(BridgeInput) == 176 ? 1 : -1];
typedef char BridgeWheelSizeCheck[sizeof(BridgeWheel) == 324 ? 1 : -1];
typedef char BridgePixelsOffsetCheck[offsetof(BridgeShared, pixels) == 800 ? 1 : -1];
typedef char BridgeGPUFrameSizeCheck[sizeof(BridgeGPUFrame) == 128 ? 1 : -1];
typedef char BridgeSharedSizeCheck[sizeof(BridgeShared) == 52429992 ? 1 : -1];

static inline int Bridge_TryLock(volatile LONG *lock) {
    return InterlockedCompareExchange(lock, 1, 0) == 0;
}
static inline void Bridge_Unlock(volatile LONG *lock) {
    InterlockedExchange(lock, 0);
}

#endif
