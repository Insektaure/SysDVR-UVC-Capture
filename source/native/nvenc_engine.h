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

// Bytes the 720p arena needs (setups, status, bitstream, history, picture,
// two references, two motion buffers).
u32  NvencArenaSize(void);

// Where NvencEncode assembles a frame (SPS/PPS + bitstream): plain donor
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

// Encodes the picture: an IDR when idr (or when the chain is broken),
// else a P frame. On success *data/*len is one access unit, SPS/PPS
// included, valid until the next call. *isIdr: what it was.
bool NvencEncode(bool idr, const u8** data, u32* len, bool* isIdr, u64* ns);

typedef struct {
    u32 frames, idrs, errors, timeouts;
    u64 bytesI, bytesP, nsTotal, nsMax;
    u32 clockHz;
} NvencStats;
void NvencGetStats(NvencStats* out, bool reset);
