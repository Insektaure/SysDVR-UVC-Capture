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
#define UVCTAP_VERSION      2

// Program id of vi-tap (atmosphere/contents/<id>/)
#define UVCTAP_PROGRAM_ID   0x00FF0000A53BB667ull

enum {
    UvcTapCmd_GetVersion       = 0,  // () -> u32 version
    UvcTapCmd_GetPresentEvent  = 1,  // () -> copy handle: readable event, signalled on each present
    UvcTapCmd_GetLatestPresent = 2,  // () -> UvcTapPresent
    UvcTapCmd_GetStats         = 3,  // () -> UvcTapStats
    UvcTapCmd_GetSurface       = 4,  // () -> UvcTapSurface (v2)
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

// The application's swapchain, from setPreallocatedBuffer (code 14): one
// flattened NvGraphicBuffer per slot. Slot 0 starts a new set.
typedef struct {
    uint64_t process_id;    // 0 = nothing recorded yet
    uint64_t color_format;  // plane 0 NvColorFormat (A8B8G8R8 = 0x100532120)
    uint32_t generation;    // bumped when a new set starts
    uint32_t num_slots;
    uint32_t width;         // plane 0, pixels
    uint32_t height;
    uint32_t stride_px;     // plane 0 pitch / 4
    uint32_t block_h_log2;  // GOBs per block, log2 (block-linear)
    uint32_t layout;        // NvLayout: 3 = block-linear
    uint32_t kind;          // NvKind (0xFE = Generic_16BX2)
    uint32_t buf_size;      // one buffer's total size, bytes
    uint32_t reserved;
    uint32_t slot_offset[8];// plane 0 offset in its nvmap object, per slot
    uint32_t slot_nvmap[8]; // nvmap object id, per slot (differs: one object per slot)
} UvcTapSurface;

#ifdef __cplusplus
static_assert(sizeof(UvcTapSurface) == 120, "UvcTapSurface layout is part of the protocol");
static_assert(sizeof(UvcTapPresent) == 96, "UvcTapPresent layout is part of the protocol");
static_assert(sizeof(UvcTapStats) == 48, "UvcTapStats layout is part of the protocol");
#else
_Static_assert(sizeof(UvcTapSurface) == 120, "UvcTapSurface layout is part of the protocol");
_Static_assert(sizeof(UvcTapPresent) == 96, "UvcTapPresent layout is part of the protocol");
_Static_assert(sizeof(UvcTapStats) == 48, "UvcTapStats layout is part of the protocol");
#endif
