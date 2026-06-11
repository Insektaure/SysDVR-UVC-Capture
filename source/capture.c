#include <string.h>
#include "core.h"
#include "grcd.h"
#include "capture.h"

// ============================================================================
// Static Data
// ============================================================================

// Video buffer (aligned for DMA)
static u8 alignas(0x1000) g_videoBuffer[VbufSz];

// Audio buffer (aligned for DMA)
static u8 alignas(0x1000) g_audioBuffer[AbufSz];

static Service g_grcdVideo;
static Service g_grcdAudio;

// grcdServiceBegin cannot be called at boot: it only works once the capture
// pipeline reports GRCD_NOT_INITIALIZED on a Transfer, so it is called
// lazily from the first failing read. It must run EXACTLY once - calling
// Begin twice crashes the console.
static bool g_grcdBeginCalled = false;
static Mutex g_grcdBeginMutex;

// Error code when grc:d is not initialized
#define GRCD_NOT_INITIALIZED 0x3E8D4

// SPS/PPS for Nintendo Switch H.264 stream.
//
// The SPS is Nintendo's original with its VUI extended to add:
//  - timing_info: 30 fps (num_units_in_tick=1, time_scale=60, fixed rate)
//  - bitstream_restriction: max_num_reorder_frames=0, max_dec_frame_buffering=1
// Without the reorder bound, spec-respecting decoders (FFmpeg inside OBS,
// Windows DirectShow consumers) reserve a frame-reorder buffer for B-frames
// this stream can never contain (pic_order_cnt_type=2 forbids reordering),
// adding 1-2s of constant capture latency. All other fields, including
// Nintendo's colour metadata, are bit-identical to the original.
// Generated and field-verified by tools/patch_sps_vui.py.
const u8 SPS[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20,
    0xAC, 0x2B, 0x40, 0x28, 0x02, 0xDD, 0x35, 0x01,
    0x0D, 0x01, 0xF0, 0x00, 0x00, 0x03, 0x00, 0x10,
    0x00, 0x00, 0x03, 0x03, 0xC8, 0xF0, 0x88, 0x46,
    0xA0
};

const u8 PPS[] = {
    0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0
};

const size_t SPS_SIZE = sizeof(SPS);
const size_t PPS_SIZE = sizeof(PPS);

static bool g_injectSpsPps = true;
static int g_idrCount = 0;

// ============================================================================
// Internal Helpers
// ============================================================================

static bool _ensureGrcdInit(Result* rc)
{
    if (g_grcdBeginCalled)
        return false;

    mutexLock(&g_grcdBeginMutex);

    if (g_grcdBeginCalled) {
        mutexUnlock(&g_grcdBeginMutex);
        return false;
    }

    g_grcdBeginCalled = true;
    *rc = grcdServiceBegin(&g_grcdVideo);
    LOG("grcdServiceBegin: 0x%x\n", *rc);

    mutexUnlock(&g_grcdBeginMutex);
    return true;
}

// ============================================================================
// Public API
// ============================================================================

Result CaptureInitialize(void)
{
    mutexInit(&g_grcdBeginMutex);

    R_RET_ON_FAIL(grcdServiceOpen(&g_grcdVideo));
    R_RET_ON_FAIL(grcdServiceOpen(&g_grcdAudio));

    LOG("Capture initialized\n");
    return 0;
}

void CaptureFinalize(void)
{
    grcdServiceClose(&g_grcdVideo);
    grcdServiceClose(&g_grcdAudio);
    LOG("Capture finalized\n");
}

bool CaptureReadVideoFrame(VideoFrame* frame)
{
    u32 dataSize = 0;
    u64 timestamp = 0;

    Result res = grcdServiceTransfer(
        &g_grcdVideo, GrcStream_Video,
        g_videoBuffer, VbufSz,
        NULL, &dataSize, &timestamp);

    // Handle lazy initialization
    if (res == GRCD_NOT_INITIALIZED && _ensureGrcdInit(&res)) {
        if (R_FAILED(res)) {
            LOG("grcdServiceBegin failed: 0x%x\n", res);
            return false;
        }
        return CaptureReadVideoFrame(frame);
    }

    if (R_FAILED(res) || dataSize <= 4) {
        LOG("Video capture failed: 0x%x size: %u\n", res, dataSize);
        return false;
    }

    // NAL unit type 5 = IDR (keyframe). The grc:d stream carries no
    // parameter sets at all, so hosts can only start decoding from an IDR
    // that we prefixed with SPS/PPS.
    bool isKeyframe = (g_videoBuffer[4] & 0x1F) == 5;

    if (g_injectSpsPps && isKeyframe) {
        g_idrCount++;

        // Re-inject on the first IDR and then every 5th: enough for hosts
        // joining mid-stream, without paying the prefix on every keyframe.
        if (g_idrCount >= 5 || g_idrCount == 1) {
            g_idrCount = 0;

            // Skip injection rather than overflow if the frame leaves no
            // headroom (guard sized by SPS_SIZE so it tracks the constant).
            if ((VbufSz - dataSize) >= (SPS_SIZE + PPS_SIZE)) {
                memmove(g_videoBuffer + SPS_SIZE + PPS_SIZE, g_videoBuffer, dataSize);
                memcpy(g_videoBuffer, SPS, SPS_SIZE);
                memcpy(g_videoBuffer + SPS_SIZE, PPS, PPS_SIZE);
                dataSize += SPS_SIZE + PPS_SIZE;
            }
        }
    }

    frame->Data = g_videoBuffer;
    frame->DataSize = dataSize;
    frame->Timestamp = timestamp;
    frame->IsKeyframe = isKeyframe;

    return true;
}

u8* CaptureGetVideoBuffer(void)
{
    return g_videoBuffer;
}

void CaptureSetInjectSPSPPS(bool inject)
{
    g_injectSpsPps = inject;
}

// ============================================================================
// Audio Capture
// ============================================================================

bool CaptureReadAudioFrame(AudioFrame* frame)
{
    u32 dataSize = 0;
    u64 timestamp = 0;

    Result res = grcdServiceTransfer(
        &g_grcdAudio, GrcStream_Audio,
        g_audioBuffer, AbufSz,
        NULL, &dataSize, &timestamp);

    // Handle lazy initialization
    if (res == GRCD_NOT_INITIALIZED && _ensureGrcdInit(&res)) {
        if (R_FAILED(res)) {
            LOG("grcdServiceBegin failed for audio: 0x%x\n", res);
            return false;
        }
        return CaptureReadAudioFrame(frame);
    }

    if (R_FAILED(res)) {
        LOG("Audio capture failed: 0x%x\n", res);
        return false;
    }

    frame->Data = g_audioBuffer;
    frame->DataSize = dataSize;
    frame->Timestamp = timestamp;

    return true;
}

u8* CaptureGetAudioBuffer(void)
{
    return g_audioBuffer;
}
