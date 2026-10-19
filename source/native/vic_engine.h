#pragma once
// The VIC: a game frame (block-linear RGBA, copied from the game's memory)
// to NV12 1280x720 pitch-linear, BT.709 limited range (docs/NATIVE-CAPTURE.md
// phase 2b-3).
//
// Every buffer lives in the donor's memory (layout in vic_engine.c), our own
// nvdrv session uses the donor's nvdrv transfer memory. Rules carried over
// from Switch Frame Tap's hardware runs (GPL-2.0, Tomas Uribe):
//  - a zero device address hangs the VIC, and a hung VIC takes the
//    compositor with it: a guard page is pinned first to hold address 0, and
//    a job with any zero address is refused;
//  - every job increments its syncpoint in the command stream (the
//    compositor composites on the same VIC syncpoint);
//  - a job that never completes leaves the engine "wedged": no more jobs,
//    and the channel is left open (closing a stuck channel took the
//    compositor down, SFT M74).
#include <switch.h>

#define VIC_OUT_W 1280
#define VIC_OUT_H 720
#define VIC_OUT_LUMA   (VIC_OUT_W * VIC_OUT_H)
#define VIC_OUT_BYTES  (VIC_OUT_LUMA + VIC_OUT_LUMA / 2)  // NV12: Y plane, then interleaved UV

typedef struct {
    u32 w, h;           // the surface
    u32 stride_px;      // pitch / 4
    u32 blk_h_log2;     // GOBs per block, log2
    u32 pixfmt;         // VIC_PIXFMT_*: A8R8G8B8 for the game's A8B8G8R8 (bytes R,G,B,A)
    u32 rect_w, rect_h; // what the game presents, from (0,0): converted to 1280x720
    u32 flip;           // present transform: bit 0 flip-H, bit 1 flip-V
    u32 copy;           // the slot copy holding it (VicEngineSlotCopy)
} VicSource;

// mem/size: the donor's capture region. slotBytes: the largest buffer the
// slot copy must hold. Takes the donor's nvdrv transfer memory.
bool VicEngineOpen(void* mem, u64 size, u32 slotBytes);
// Closes the nvdrv session (frees every fd and pin) unless the engine is
// wedged. Must run before the donor memory is released.
void VicEngineClose(void);
bool VicEngineActive(void);
// How much of the donor memory the engine's own buffers use (from its start).
u64  VicEngineBytesUsed(void);
bool VicEngineWedged(void);

// Where the caller copies the game's buffer (same offsets as in the game):
// copy 0, and copy 1 once added. NULL for a copy that does not exist. And a
// plain (unpinned) buffer for reads through dmnt:cht: pinned pages cannot be
// an IPC buffer.
u8*    VicEngineSlotCopy(u32 i);
// A second slot copy (pipelining: the next frame is copied while the VIC
// reads the other), page-aligned, at least the slot's size. Pinned until
// VicEngineClose.
bool   VicEngineAddSlotCopy(void* cpu, u64 size);
u8*    VicEngineBounce(void);
size_t VicEngineBounceSize(void);
const u8* VicEngineOutput(void);

// One job into the engine's own NV12 1280x720 pitch buffer (VicEngineOutput).
// The slot copy must hold (and be flushed for) the source's rows.
// *ns: submit to done.
bool VicEngineConvert(const VicSource* src, u64* ns);

// One job into another pinned buffer: NV12 1280x720, pitch-linear or NVENC's
// input layout (16-row block-linear, chroma rows rounded up to 16). The
// caller invalidates its cache if the CPU reads it.
typedef struct {
    u32 lumaAddr, chromaAddr;  // VIC device addresses (VicEnginePin), 256-aligned
    bool blockLinear;
} VicTarget;
bool VicEngineConvertTo(const VicSource* src, const VicTarget* dst, u64* ns);

// The same in two halves: the job goes in, the caller does something else
// (copies the next frame), then collects it. One job at a time; nothing may
// touch its slot copy or target until collected. *ns: submit to collected,
// *blockedNs: how long JobWait itself waited. A job not done 0.5 s after its
// submit wedges the engine (JobWait returns false).
bool VicEngineSubmitTo(const VicSource* src, const VicTarget* dst);
bool VicEngineJobPending(void);
bool VicEngineJobWait(u64* ns, u64* blockedNs);

// For the encoder: the same nvdrv session's nvmap and ctrl fds, and a pin of
// one more buffer (cached) in the VIC channel.
u32  VicEngineNvmapFd(void);
u32  VicEngineCtrlFd(void);
bool VicEngineNvmapOwn(void* cpu, u32 size, bool cacheable, u32* handle);
bool VicEnginePin(u32 handle, u32* vicAddr);

// The present's acquire fence, from vi-tap: (syncpoint id << 32) | value.
// Polls nvhost-ctrl until every syncpoint reached it, at most timeoutNs.
// Returns false on timeout. Needs the engine open (ctrl fd).
bool VicEngineWaitFence(u32 count, const u64* fence, u64 timeoutNs, u64* waitedNs);
// The same, without waiting: has the GPU finished that present? (No fence
// known counts as finished.)
bool VicEngineFenceReached(u32 count, const u64* fence);

typedef struct { u32 jobs, timeouts, refused; u64 nsTotal, nsMax; } VicEngineStats;
void VicEngineGetStats(VicEngineStats* out, bool reset);
