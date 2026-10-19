// uvctap - IPC protocol between vi-tap (the always-on vi mitm) and the
// SysDVR-UVC sysmodule. Shared by both sides: plain C, no libnx types beyond
// the fixed-width ones. See docs/NATIVE-CAPTURE.md §5.1.
//
// vi-tap only publishes what the game's binder traffic says about each
// present. It never holds state about its clients: a client that disappears
// (SysDVR-UVC killed by ovlSysmodules) just closes its session.
#pragma once

#include <stdint.h>

#define UVCTAP_SERVICE_NAME "uvctap"
#define UVCTAP_VERSION      1

// Program id of vi-tap (atmosphere/contents/<id>/)
#define UVCTAP_PROGRAM_ID   0x00FF0000A53BB667ull

enum {
    UvcTapCmd_GetVersion       = 0,  // () -> u32 version
    UvcTapCmd_GetPresentEvent  = 1,  // () -> copy handle: readable event, signalled on each present
    UvcTapCmd_GetLatestPresent = 2,  // () -> UvcTapPresent
    UvcTapCmd_GetStats         = 3,  // () -> UvcTapStats
};

// The application's last queueBuffer (IGraphicBufferProducer code 7).
// The parcels carry no buffer addresses: the reader finds each slot's memory
// itself (SFT slotmatch).
typedef struct {
    uint64_t seq;           // presents recorded so far; 0 = none yet
    uint64_t process_id;    // the presenting process (the running application)
    uint64_t program_id;
    uint64_t tick;          // armGetSystemTick() when recorded
    int32_t  slot;          // swapchain slot presented, -1 if unparsed
    uint32_t fence_count;   // 0..4; 0 = no fence parsed
    uint64_t fence[4];      // (syncpoint id << 32) | threshold
    uint32_t transform;     // NATIVE_WINDOW_TRANSFORM_* (1 flip-H, 2 flip-V, 4 rot-90)
    int32_t  crop[4];       // left, top, right, bottom
    uint32_t slot_mask;     // slots seen in setPreallocatedBuffer / queueBuffer
} UvcTapPresent;

typedef struct {
    uint64_t app_pid;           // running application per pm:dmnt, 0 = none
    uint64_t displays_wrapped;  // IApplicationDisplayService objects wrapped
    uint64_t binders_wrapped;   // IHOSBinderDriver objects wrapped
    uint64_t presents;          // queueBuffer calls of the application
    uint64_t parse_failures;    // queueBuffer parcels we could not read
    uint64_t other_presents;    // queueBuffer from a wrapped non-application process
} UvcTapStats;

#ifdef __cplusplus
static_assert(sizeof(UvcTapPresent) == 96, "UvcTapPresent layout is part of the protocol");
static_assert(sizeof(UvcTapStats) == 48, "UvcTapStats layout is part of the protocol");
#else
_Static_assert(sizeof(UvcTapPresent) == 96, "UvcTapPresent layout is part of the protocol");
_Static_assert(sizeof(UvcTapStats) == 48, "UvcTapStats layout is part of the protocol");
#endif
