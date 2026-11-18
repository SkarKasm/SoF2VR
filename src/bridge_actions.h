#ifndef SOF2_BRIDGE_ACTIONS_H
#define SOF2_BRIDGE_ACTIONS_H
#include <openxr/openxr.h>
#include "bridge_protocol.h"
typedef struct {
    XrActionSet set;
    XrPath hands[2];
    XrAction grip, aim, trigger, squeeze, stick, primary, secondary, click, menu, haptic;
    XrSpace gripSpace[2], aimSpace[2];
    uint32_t hapticSerial;
} BridgeActions;
XrResult BridgeActions_Create(BridgeActions *, XrInstance, XrSession);
XrResult BridgeActions_Sync(BridgeActions *, XrSession, XrSpace, XrTime, int, BridgeInput *);
XrResult BridgeActions_Haptic(BridgeActions *, XrSession, const BridgeHaptic *, int);
void BridgeActions_Destroy(BridgeActions *);
const char *BridgeActions_Failure(void);
#endif
