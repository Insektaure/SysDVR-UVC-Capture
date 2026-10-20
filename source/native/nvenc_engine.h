#pragma once
// NVENC: the VIC's NV12 picture to H.264 (docs/NATIVE-CAPTURE.md phase 3).
//
// The encoder runs grc's own job (the console's video recorder, read out by
// Switch Frame Tap, GPL-2.0, Tomas Uribe - see nvenc_setups.h): a 4 KB setup
// per picture type, every buffer bound by a method, constant QP (RCMODE 0).
// Keyframes are IDRs; P frames reference the previous picture and chain two
// reference and two motion-prediction buffers. NVENC writes slices only: the
// SPS/PPS go in front of every frame, so a decoder can start anywhere.
//
// It shares the VIC engine's nvdrv session and nvmap fd; its arena (all its
// buffers, the VIC's output picture included) is in the donor's memory.
// Same rules as the VIC: the job increments its syncpoint, a job that never
// completes leaves the engine wedged and its channel open.
#include <switch.h>

#define NVENC_W 1280
#define NVENC_H 720

// Bytes the 720p arena needs (setups, status, bitstream, history, two
// pictures, two references, two motion buffers).
u32  NvencArenaSize(void);

// Where a collected frame is assembled (SPS/PPS + bitstream): plain donor
// memory, not pinned.
u32  NvencFrameBufferSize(void);

// Opens /dev/nvhost-msenc, raises the encoder clock (mm:u), pins the arena in
// the msenc and VIC channels and loads the setups. qp: I frames; P frames +2.
bool NvencOpen(u8* arena, u32 arenaSize, u8* frameBuf, u32 frameBufSize, u8 qp);
// Closes the msenc channel and the clock requests, unless wedged. Before
// VicEngineClose (which ends the session).
void NvencClose(void);
bool NvencActive(void);
bool NvencWedged(void);

// Where the VIC writes the picture to encode (VIC device addresses).
void NvencPictureTarget(u32* lumaAddr, u32* chromaAddr);
// The same picture for the CPU (cached: flush what you write), in the same
// 16-row block-linear layout.
void NvencPictureCpu(u8** luma, u8** chroma, u32* lumaBytes, u32* chromaBytes);

// Encodes the picture just written (NvencPictureTarget / NvencPictureCpu),
// without waiting: an IDR when idr (or when the chain is broken), else a P
// frame. The next frame then goes to the other picture, so it can be written
// while NVENC reads this one. One job at a time: collect it before the next
// submit (which is also what makes the other picture free again).
bool NvencSubmit(bool idr);
bool NvencJobPending(void);
// The job's frame: 1 done (*data/*len one access unit, SPS/PPS included,
// valid until the next collect; *isIdr what it was; *ns submit to done, as
// precise as the polls are frequent), 0 not yet (Poll only), -1 failed (the
// next submit starts from a keyframe; a job never done wedges the engine).
int  NvencPoll(const u8** data, u32* len, bool* isIdr, u64* ns);
int  NvencCollect(const u8** data, u32* len, bool* isIdr, u64* ns);
// The picture submitted last, encoded again (a P frame), waited on: the
// keep-alive's filler. No job may be in flight.
bool NvencEncodeAgain(const u8** data, u32* len, bool* isIdr, u64* ns);

// Encodes a black keyframe (Y 16, U/V 128 - BT.709 limited black) into out,
// SPS/PPS included: what the camera sends between games, so the stream never
// goes silent. The next submit starts a new chain from a keyframe.
bool NvencEncodeBlack(u8* out, u32 max, u32* len);

// A clock's current rate, read through clkrst (pcv module id, e.g.
// 0x4000002F VIC, 0x40000039 EMC, 0x40000002 GPU). 0 when unreadable.
u32  NvencPcvClockHz(u32 module);

typedef struct {
    u32 frames, idrs, errors, timeouts;
    u64 bytesI, bytesP, nsTotal, nsMax;
    u32 clockHz;
} NvencStats;
void NvencGetStats(NvencStats* out, bool reset);
