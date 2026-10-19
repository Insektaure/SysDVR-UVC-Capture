#pragma once
#include <switch.h>
#include "uvctap.h"

// Client side of the uvctap protocol (include/uvctap.h), served by vi-tap.
// Nothing here holds vi-tap up: if this process is killed the kernel closes
// the session and the event handle, and vi-tap does not notice.

// Connects if vi-tap is running. Never blocks on sm (CoreHasService first).
Result TapClientConnect(void);
void   TapClientClose(void);
bool   TapClientIsConnected(void);

// The readable "a frame was presented" event (manual clear).
Event* TapClientPresentEvent(void);

Result TapClientGetLatestPresent(UvcTapPresent* out);
Result TapClientGetStats(UvcTapStats* out);
Result TapClientGetSurface(UvcTapSurface* out);

// The protocol vi-tap speaks (0 = not connected). v3 keeps a present history.
u32    TapClientVersion(void);
// v3: the last presents, newest first (max <= UVCTAP_RECENT_MAX).
Result TapClientGetRecentPresents(UvcTapPresent* out, u32 max, u32* count);

// Phase 1 spike (docs/NATIVE-CAPTURE.md §11): reads present records and logs
// them, nothing else. Built only with -DTAP_PROBE.
void TapProbeThread(void* arg);
