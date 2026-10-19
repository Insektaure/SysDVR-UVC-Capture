#pragma once
#include <switch.h>
#include "uvctap.h"

// Client side of the uvctap protocol (include/uvctap.h), served by vi-tap.
// Nothing here holds vi-tap up: if this process is killed the kernel closes
// the session and the event handle, and vi-tap does not notice.

// Connects if vi-tap is running. Never blocks on sm: an unregistered name
// makes smGetService wait forever under Atmosphère, so registration is
// checked first (Atmosphère sm extension AtmosphereHasService).
Result TapClientConnect(void);
void   TapClientClose(void);
bool   TapClientIsConnected(void);

// The readable "a frame was presented" event (manual clear).
Event* TapClientPresentEvent(void);

Result TapClientGetLatestPresent(UvcTapPresent* out);
Result TapClientGetStats(UvcTapStats* out);

// Phase 1 spike (docs/NATIVE-CAPTURE.md §11): reads present records and logs
// them, nothing else. Built only with -DTAP_PROBE.
void TapProbeThread(void* arg);
