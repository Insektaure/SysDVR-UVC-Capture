#include <string.h>
#include "../core.h"
#include "nvdrv.h"
#include "vic.h"
#include "vic_engine.h"
#include "nvenc_setups.h"
#include "nvenc_engine.h"

// After Switch Frame Tap's NvfSubmit / NvfWaitDone / TryNvencStream and its
// engine clock handling (applet_mitm_nv.cpp, applet_mitm_clk.cpp; GPL-2.0,
// Tomas Uribe).

// ---- the 720p arena (SFT Layout720: the offsets every 720p run used) --------
#define A_SETUP     0x000000  // IDR setup (4 KB)
#define A_STATUS    0x001000  // nvenc_pic_stat_s (128 B)
#define A_RC        0x002000  // rate-control scratch
#define A_CMD       0x006000  // host1x command buffer
#define A_BITS      0x010000  // bitstream out
#define A_BITS_SIZE 0x160000
#define A_HIST      (A_BITS + A_BITS_SIZE)          // IO history, 0xC0000
#define A_CUR       (A_HIST + 0x0C0000)             // the VIC's NV12, 16-row block-linear
#define A_LUMA      (NVENC_W * NVENC_H)             // 720 rows: whole 16-row blocks
#define A_CHROMA    (NVENC_W * 368)                 // 360 rows rounded up to 16
#define A_CUR_UV    (A_CUR + A_LUMA)
#define A_REF_A     ((A_CUR_UV + A_CHROMA + 0xFFF) & ~0xFFFu)  // reconstruction / reference
#define A_SETUP_P   (A_REF_A + 0x160000)            // P setup (4 KB)
#define A_REF_B     (A_SETUP_P + 0x1000)
#define A_ME_A      (A_REF_B + 0x160000)            // motion prediction (grc: 128 KB apart)
#define A_ME_B      (A_ME_A + 0x40000)
#define A_SIZE      (A_ME_B + 0x40000)

_Static_assert(A_CUR_UV % 0x100 == 0, "host1x addresses are >> 8");
_Static_assert(A_CHROMA == 1280 * 368, "16-row blocks: what NVENC reads");

#define GOP          60           // a keyframe every second at 60 fps (SFT)
#define PIC_INDEX0   0x53445600u  // our picture indices ("SDV" + n)

// status word offsets (nvenc_pic_stat_s, NVIDIA nvenc_drv.h, MIT)
#define ST_PICTURE_INDEX 0
#define ST_ERROR_WORD    4   // error_status:2, ucode_error_status:30
#define ST_TOTAL_BITS    8
#define ST_START_POS     32

// NVENC host1x class and methods (NVIDIA clc5b7.h; stable on this engine, SFT)
#define HOST1X_CLASS_NVENC 0x21
#define NV_SET_APPLICATION_ID   0x200
#define NV_EXECUTE              0x300
#define NV_SET_IN_REF_PIC0      0x400
#define NV_SET_CONTROL_PARAMS   0x700
#define NV_SET_PICTURE_INDEX    0x704
#define NV_SET_IN_DRV_PIC_SETUP 0x710
#define NV_SET_OUT_ENC_STATUS   0x718
#define NV_SET_OUT_BITSTREAM    0x71C
#define NV_SET_IOHISTORY        0x720
#define NV_SET_IO_RC_PROCESS    0x724
#define NV_SET_OUT_REF_PIC      0x730
#define NV_SET_IN_CUR_PIC       0x734
#define NV_SET_IN_MEPRED        0x738
#define NV_SET_OUT_MEPRED       0x73C
#define NV_SET_IN_CUR_PIC_UV    0x740

// pcv module (clkrst) and mm:u request ids. mm:u ids are ambiguous in the
// sources (libnx: 5 NVENC, 6 NVDEC; nvtegra passes 5 for NVDEC): SFT requests
// both, and clocking an idle decoder costs a little power and nothing else.
#define PCV_NVENC 0x40000030u
static const u32 MmIds[2] = { 5, 6 };
#define MM_MAX_HZ ((u32)((1ULL << 32) - 1000))  // official code: INT_MAX kHz; clamped by the service

static u8* g_a = NULL;
static u32 g_ah = 0;                 // arena nvmap handle
static u32 g_encAddr = 0, g_vicAddr = 0;
static u32 g_efd = 0, g_esyncpt = 0;
static bool g_open = false, g_wedged = false;
static Service g_mm;
static bool g_mmOpen = false;
static u32 g_mmReq[2];
static bool g_mmOk[2];
static u32 g_pic = PIC_INDEX0;
static u32 g_pos = 0;                // place in the GOP; 0 = IDR next
static bool g_needIdr = true;
static u32 g_idrs = 0;
static u8* g_frame = NULL;          // SPS/PPS + one access unit: donor memory, never .bss
static u32 g_frameSize = 0;         // (this process lives in the System pool)
static NvencStats g_stats;

u32 NvencArenaSize(void)
{
    return A_SIZE;
}

// ---- clocks (mm:u, checked through clkrst) ----------------------------------

static u32 _clockHz(void)
{
    if (!CoreHasService("clkrst"))
        return 0;
    Service clk, sess;
    if (R_FAILED(smGetService(&clk, "clkrst")))
        return 0;
    u32 hz = 0;
    const struct { u32 module, unk; } in = { PCV_NVENC, 3 };  // 3: what sys-clk passes
    if (R_SUCCEEDED(serviceDispatchIn(&clk, 0, in, .out_num_objects = 1, .out_objects = &sess))) {
        serviceDispatchOut(&sess, 8, hz);
        serviceClose(&sess);
    }
    serviceClose(&clk);
    return hz;
}

static Result _mmInit(u32 module, u32* id)
{
    const struct { u32 module, unk, autoclear; } in = { module, 8, 0 };
    return serviceDispatchInOut(&g_mm, 4, in, *id);
}

static Result _mmSet(u32 id, u32 hz)
{
    const struct { u32 id, hz; s32 timeout; } in = { id, hz, -1 };
    return serviceDispatchIn(&g_mm, 6, in);
}

// SFT M76/M77: an engine that is not clocked accepts a submit and never runs.
// Request the clock, read it back, escalate: re-set, set 0 then max, fresh.
static u32 _ensureClock(void)
{
    if (!g_mmOpen) {
        if (!CoreHasService("mm:u") || R_FAILED(smGetService(&g_mm, "mm:u")))
            return 0;
        g_mmOpen = true;
    }
    for (u32 attempt = 0; attempt < 6; attempt++) {
        const u32 step = attempt < 3 ? attempt : 2;
        for (u32 i = 0; i < 2; i++) {
            if (step == 2 || !g_mmOk[i]) {
                if (g_mmOk[i])
                    serviceDispatchIn(&g_mm, 5, g_mmReq[i]);
                g_mmOk[i] = R_SUCCEEDED(_mmInit(MmIds[i], &g_mmReq[i]));
                if (!g_mmOk[i])
                    continue;
            } else if (step == 1) {
                _mmSet(g_mmReq[i], 0);
            }
            _mmSet(g_mmReq[i], MM_MAX_HZ);
        }
        const u32 hz = _clockHz();
        LOG("nvenc: clock attempt %u -> %u Hz\n", attempt + 1, hz);
        if (hz)
            return hz;
        svcSleepThread(50000000ULL * (attempt + 1));
    }
    return 0;
}

static void _releaseClock(void)
{
    if (!g_mmOpen)
        return;
    for (u32 i = 0; i < 2; i++) {
        if (g_mmOk[i])
            serviceDispatchIn(&g_mm, 5, g_mmReq[i]);
        g_mmOk[i] = false;
    }
    serviceClose(&g_mm);
    g_mmOpen = false;
}

// ---- open / close -----------------------------------------------------------

static bool _mapEnc(u32 handle, u32* addr)
{
    struct { u32 num_handles, reserved; u8 is_compr, pad[3]; u32 handle, phys; } a = { 1, 0, 0, {0}, handle, 0 };
    u32 nverr = 0;
    Result rc = NvdrvIoctl(g_efd, NVHOST_IOC_CHANNEL_MAP_CMD_BUFFER, &a, sizeof(a), &nverr);
    LOG("nvenc: map arena in msenc -> 0x%x (0x%x, nverr %u)\n", a.phys, rc, nverr);
    if (R_FAILED(rc) || nverr)
        return false;
    *addr = a.phys;
    return true;
}

static void _setByte(u32 off, u8 v)
{
    g_a[off] = v;
    armDCacheFlush(g_a + (off & ~0x3Fu), 0x40);
}

u32 NvencFrameBufferSize(void)
{
    return 0x180000;  // SPS/PPS + the largest bitstream the engine may write
}

bool NvencOpen(u8* arena, u32 arenaSize, u8* frameBuf, u32 frameBufSize, u8 qp)
{
    if (g_open)
        return true;
    if (!VicEngineActive() || arenaSize < A_SIZE || frameBufSize < NvencFrameBufferSize()) {
        LOG("nvenc: %s\n", !VicEngineActive() ? "the VIC engine is not open" : "arena or frame buffer too small");
        return false;
    }
    g_frame = frameBuf;
    g_frameSize = frameBufSize;
    g_wedged = false;
    g_needIdr = true;
    g_pos = 0;
    memset(&g_stats, 0, sizeof(g_stats));
    g_a = arena;

    // zeroed and flushed BEFORE any engine writes it (SFT: no dirty line from
    // the memset may be written back over engine output)
    memset(g_a, 0, A_SIZE);
    memcpy(g_a + A_SETUP, NvencIdrSetup720, sizeof(NvencIdrSetup720));
    memcpy(g_a + A_SETUP_P, NvencPSetup720, sizeof(NvencPSetup720));
    g_a[A_SETUP + NVENC_SETUP_QP_I] = qp;
    const u8 qpP = qp + 2 > 51 ? 51 : qp + 2;  // SFT v0.6: P frames at QP+2
    g_a[A_SETUP_P + NVENC_SETUP_QP_P] = qpP;
    g_a[A_SETUP_P + NVENC_SETUP_QP_I] = qpP;
    armDCacheFlush(g_a, A_SIZE);

    g_stats.clockHz = _ensureClock();
    if (!g_stats.clockHz) {
        LOG("nvenc: the encoder clock did not come up - not submitting (it would never run)\n");
        _releaseClock();
        return false;
    }

    u32 nverr = 0;
    if (R_FAILED(NvdrvOpen("/dev/nvhost-msenc", &g_efd, &nverr)) || nverr) {
        LOG("nvenc: /dev/nvhost-msenc nverr %u\n", nverr);
        _releaseClock();
        return false;
    }
    g_open = true;
    struct { u32 module_id, syncpt; } gs = { 0, 0 };
    NvdrvIoctl(g_efd, NVHOST_IOC_CHANNEL_GET_SYNCPOINT, &gs, sizeof(gs), &nverr);
    g_esyncpt = gs.syncpt;
    // 0 is invalid, 12 is the VIC's (the compositor's): never share it
    if (g_esyncpt == 0 || g_esyncpt == 12) {
        LOG("nvenc: msenc syncpoint %u - refusing\n", g_esyncpt);
        NvencClose();
        return false;
    }
    struct { u32 fd; } sn = { VicEngineNvmapFd() };
    NvdrvIoctl(g_efd, NVHOST_IOC_CHANNEL_SET_NVMAP_FD, &sn, sizeof(sn), &nverr);

    // the arena: cached (the CPU writes setups, reads the bitstream), pinned
    // for both engines - the VIC writes the picture NVENC reads
    if (!VicEngineNvmapOwn(g_a, A_SIZE, true, &g_ah) || !VicEnginePin(g_ah, &g_vicAddr) || !_mapEnc(g_ah, &g_encAddr) ||
        !g_vicAddr || !g_encAddr) {
        LOG("nvenc: arena pin failed (vic 0x%x, enc 0x%x)\n", g_vicAddr, g_encAddr);
        NvencClose();
        return false;
    }
    LOG("nvenc: ready - %ux%u, QP %u (P %u), keyframe every %u, clock %u Hz, syncpoint %u, arena %u KB at enc 0x%x / vic 0x%x\n",
        NVENC_W, NVENC_H, qp, qpP, GOP, g_stats.clockHz, g_esyncpt, A_SIZE / 1024, g_encAddr, g_vicAddr);
    return true;
}

void NvencClose(void)
{
    if (!g_open) {
        _releaseClock();
        return;
    }
    if (g_wedged) {
        LOG("nvenc: engine wedged - leaving the msenc channel open (SFT M74)\n");
        return;
    }
    NvdrvClose(g_efd);  // the arena's handle goes with the nvdrv session (VicEngineClose)
    _releaseClock();
    g_open = false;
    g_efd = g_esyncpt = g_ah = g_encAddr = g_vicAddr = 0;
    g_a = NULL;
    g_frame = NULL;
}

bool NvencActive(void) { return g_open && !g_wedged; }
bool NvencWedged(void) { return g_wedged; }

void NvencPictureTarget(u32* lumaAddr, u32* chromaAddr)
{
    *lumaAddr = g_vicAddr ? g_vicAddr + A_CUR : 0;
    *chromaAddr = g_vicAddr ? g_vicAddr + A_CUR_UV : 0;
}

// ---- one picture -------------------------------------------------------------

static u32 _buildCmd(u32* w, u32 pic, bool p)
{
    u32 n = 0;
#define M(method, value) do { w[n++] = Host1xIncr(0x10, 2); w[n++] = (method) >> 2; w[n++] = (value); } while (0)
    w[n++] = Host1xSetClass(0, HOST1X_CLASS_NVENC, 0);
    w[n++] = Host1xIncr(0x0B, 1);  // as grc's job (SFT)
    w[n++] = 0;
    M(NV_SET_CONTROL_PARAMS, 0x00001103u);  // H.264, RCMODE 0 (constant QP)
    M(NV_SET_PICTURE_INDEX, pic);
    M(NV_SET_APPLICATION_ID, 1);            // H.264
    M(NV_SET_IN_DRV_PIC_SETUP, (g_encAddr + (p ? A_SETUP_P : A_SETUP)) >> 8);
    M(NV_SET_OUT_ENC_STATUS, (g_encAddr + A_STATUS) >> 8);
    M(NV_SET_IO_RC_PROCESS, (g_encAddr + A_RC) >> 8);
    M(NV_SET_OUT_BITSTREAM, (g_encAddr + A_BITS) >> 8);
    M(NV_SET_IOHISTORY, (g_encAddr + A_HIST) >> 8);
    M(NV_SET_IN_CUR_PIC, (g_encAddr + A_CUR) >> 8);
    M(NV_SET_IN_CUR_PIC_UV, (g_encAddr + A_CUR_UV) >> 8);
    // references alternate: this frame writes refs[pos & 1], a P frame reads
    // the previous one's; motion buffers likewise
    const u32 refOut = (g_pos & 1) ? A_REF_B : A_REF_A;
    M(NV_SET_OUT_REF_PIC, (g_encAddr + refOut) >> 8);
    if (p) {  // grc's P job, in grc's order
        const u32 refIn = (g_pos & 1) ? A_REF_A : A_REF_B;
        const u32 meIn = (g_pos & 1) ? A_ME_A : A_ME_B;
        const u32 meOut = (g_pos & 1) ? A_ME_B : A_ME_A;
        M(NV_SET_IN_MEPRED, (g_encAddr + meIn) >> 8);
        M(NV_SET_OUT_MEPRED, (g_encAddr + meOut) >> 8);
        M(NV_SET_IN_REF_PIC0, (g_encAddr + refIn) >> 8);
    }
    M(NV_EXECUTE, 0x100);
#undef M
    w[n++] = Host1xNonIncr(HOST1X_UCLASS_INCR_SYNCPT, 1);
    w[n++] = (VIC_INCR_SYNCPT_COND_OP_DONE << VIC_INCR_SYNCPT_COND_SHIFT) | g_esyncpt;
    return n;
}

static bool _syncptReached(u32 id, u32 want)
{
    struct { u32 id, value; } r = { id, 0 };
    u32 nverr = 0;
    if (R_FAILED(NvdrvIoctl(VicEngineCtrlFd(), NVHOST_IOC_CTRL_SYNCPT_READ, &r, sizeof(r), &nverr)) || nverr)
        return false;
    return (s32)(r.value - want) >= 0;
}

bool NvencEncode(bool idr, const u8** data, u32* len, bool* isIdr, u64* ns)
{
    if (!g_open || g_wedged)
        return false;
    if (idr || g_needIdr || g_pos >= GOP)
        g_pos = 0;
    const bool p = g_pos != 0;
    if (p) {
        const u16 fn = (u16)(g_pos % NVENC_MAX_FRAME_NUM);
        const u16 poc = (u16)((2 * g_pos) % NVENC_MAX_POC_LSB);
        memcpy(g_a + A_SETUP_P + NVENC_SETUP_FRAME_NUM, &fn, 2);
        memcpy(g_a + A_SETUP_P + NVENC_SETUP_POC_LSB, &poc, 2);
        armDCacheFlush(g_a + A_SETUP_P + (NVENC_SETUP_FRAME_NUM & ~0x3Fu), 0x40);
    } else {
        _setByte(A_SETUP + NVENC_SETUP_IDR_PIC_ID, (u8)(g_idrs & 1));
    }

    // poison the picture index: it is what says "our job wrote this status"
    const u32 pic = g_pic++;
    const u32 poison = 0xA5A5A5A5u;
    memcpy(g_a + A_STATUS + ST_PICTURE_INDEX, &poison, 4);
    armDCacheFlush(g_a + A_STATUS, 0x80);

    u32* cmd = (u32*)(g_a + A_CMD);
    const u32 words = _buildCmd(cmd, pic, p);
    armDCacheFlush(cmd, 0x1000);

    // CHANNEL_SUBMIT: the command buffer is inside the arena (handle, offset)
    u32 sb[13] = { 1, 0, 1, 1, g_ah, A_CMD, words, g_esyncpt, 1, 0, 0, 0, 0 };
    const u64 t0 = armTicksToNs(armGetSystemTick());
    u32 nverr = 0;
    Result rc = NvdrvIoctl(g_efd, NV_IOWR(0x00, 0x01, sizeof(sb)), sb, sizeof(sb), &nverr);
    if (R_FAILED(rc) || nverr) {
        LOG("nvenc: submit refused (0x%x, nverr %u)\n", rc, nverr);
        g_stats.errors++;
        g_needIdr = true;
        return false;
    }
    const u32 fence = sb[12];

    // done = the syncpoint passed the fence AND the status carries our index
    bool fenceDone = false, ours = false;
    for (;;) {
        if (!fenceDone)
            fenceDone = _syncptReached(g_esyncpt, fence);
        armDCacheFlush(g_a + A_STATUS, 0x80);
        u32 pi = 0;
        memcpy(&pi, g_a + A_STATUS + ST_PICTURE_INDEX, 4);
        ours = pi == pic;
        if (fenceDone && ours)
            break;
        if (armTicksToNs(armGetSystemTick()) - t0 > 1000000000ULL)
            break;
        svcSleepThread(50000);
    }
    const u64 dt = armTicksToNs(armGetSystemTick()) - t0;
    if (!fenceDone || !ours) {
        // SFT v0.7.1: late is not wedged - give it 3 s more before saying so
        for (u32 i = 0; i < 300 && !fenceDone; i++) {
            svcSleepThread(10000000ULL);
            fenceDone = _syncptReached(g_esyncpt, fence);
        }
        g_stats.timeouts++;
        g_needIdr = true;
        if (!fenceDone) {
            g_wedged = true;
            LOG("nvenc: job never completed (syncpoint %u, fence %u) - engine wedged, no more jobs\n", g_esyncpt, fence);
        } else {
            LOG("nvenc: job late (%s) - restarting the chain from a keyframe\n", ours ? "fence" : "no status of ours");
        }
        return false;
    }

    u32 errWord = 0, bits = 0, start = 0;
    memcpy(&errWord, g_a + A_STATUS + ST_ERROR_WORD, 4);
    memcpy(&bits, g_a + A_STATUS + ST_TOTAL_BITS, 4);
    memcpy(&start, g_a + A_STATUS + ST_START_POS, 4);
    const u32 bytes = bits / 8;
    const size_t hdrs = sizeof(NvencSpsPps720);
    // error_status 2 is routine (SFT M81: grc's own frames carry it); the
    // firmware's ucode status is what matters
    if ((errWord >> 2) != 0 || bytes == 0 || start != 0 || bytes > A_BITS_SIZE || hdrs + bytes > g_frameSize) {
        g_stats.errors++;
        g_needIdr = true;  // never predict from a frame we did not keep
        LOG("nvenc: bad frame (ucode 0x%x, %u bytes, start %u) - next is a keyframe\n", errWord >> 2, bytes, start);
        return false;
    }
    armDCacheFlush(g_a + A_BITS, bytes);  // the engine wrote it: drop stale lines
    memcpy(g_frame, NvencSpsPps720, hdrs);
    memcpy(g_frame + hdrs, g_a + A_BITS, bytes);

    if (!p) {
        g_idrs++;
        g_needIdr = false;
        g_stats.idrs++;
        g_stats.bytesI += bytes;
    } else {
        g_stats.bytesP += bytes;
    }
    g_stats.frames++;
    g_stats.nsTotal += dt;
    if (dt > g_stats.nsMax)
        g_stats.nsMax = dt;
    *isIdr = !p;
    g_pos++;
    *data = g_frame;
    *len = (u32)(hdrs + bytes);
    if (ns)
        *ns = dt;
    return true;
}

bool NvencEncodeBlack(u8* out, u32 max, u32* len)
{
    if (!g_open || g_wedged)
        return false;
    // a uniform picture needs no layout: block-linear or not, every byte is
    // the same value per plane
    memset(g_a + A_CUR, 16, A_LUMA);
    memset(g_a + A_CUR_UV, 128, A_CHROMA);
    armDCacheFlush(g_a + A_CUR, A_CUR_UV + A_CHROMA - A_CUR);
    const u8* data = NULL;
    u32 n = 0;
    bool idr = false;
    const bool ok = NvencEncode(true, &data, &n, &idr, NULL) && idr && n <= max;
    if (ok) {
        memcpy(out, data, n);
        *len = n;
    }
    g_needIdr = true;  // the next real frame starts its own chain
    return ok;
}

void NvencGetStats(NvencStats* out, bool reset)
{
    *out = g_stats;
    if (reset) {
        const u32 hz = g_stats.clockHz, wedgedTimeouts = g_stats.timeouts;
        memset(&g_stats, 0, sizeof(g_stats));
        g_stats.clockHz = hz;
        g_stats.timeouts = wedgedTimeouts;
    }
}
