// Phase 2a (docs/NATIVE-CAPTURE.md §11): attach to the running game, find
// its swapchain buffers, read frames, dump a few to the SD card. No GPU, no
// USB - it runs beside the grc:d camera and changes nothing there.
//
// Per game session:
//   vi-tap's surface (geometry) + presents -> attach (direct, else through
//   dmnt:cht) -> resume + debug event pump -> find the slot buffers in the
//   game's memory -> match them to slots (slotmatch) -> every second read a
//   whole slot (throughput, and the kill test while attached) -> when the
//   game stops presenting (HOME pressed: suspended, its buffers still), dump
//   the slot on screen -> detach when the game goes or changes.
//
// Default build (-DNATIVE_CAPTURE, Makefile CAPTURE=native); grc:d is not
// used at all in this build.
#include <stdio.h>
#include <string.h>
#include "../core.h"
#include "../tap_client.h"
#include "game_dbg.h"
#include "slotmatch.h"
#include "donor.h"
#include "vic.h"
#include "vic_engine.h"
#include "nvenc_engine.h"
#include "cpu_convert.h"
#include "../uvc/uvc_device.h"
#include "native_capture.h"

#ifdef NATIVE_CAPTURE

// Debug features (logging builds only): a whole-slot read every second
// (throughput), and on HOME a dump of the frame on screen to the SD card.
#define NATIVE_DEBUG LOGGING_ENABLED

#define SIG_POINTS        32
#define SAMPLE_DELAY_NS   8000000ULL   // without the engine (no nvdrv): no fence wait, let the GPU finish
#define FENCE_TIMEOUT_NS  250000000ULL // SFT v0.7.1: some games queue ~20 ms (up to 68) before the GPU is done
#define PIPE_FENCE_NS     20000000ULL  // with a job in flight: how long the next present may keep it waiting
#define READ_EVERY_NS     1000000000ULL
#define DUMPS_PER_SESSION 3
#define PAUSED_NS         1000000000ULL  // no present for this long: the game is suspended
#define MAX_BUF_BYTES     (16u * 1024 * 1024)
#define CHUNK             0x20000      // reads go through this; dumps are written from it piece by piece
#define DUMP_DIR          "/config/sysdvr-uvc/dump"
#define DONOR_MB          34           // per game session: VIC buffers + slot copy (12.5 MB), encoder arena (8.4: two pictures), frame (1.5), second slot copy (8.5), send copy (1.5)
#define ENCODE_QP         20           // SFT: constant QP 20 (P frames 22)
// How a frame becomes the encoder's picture (one of):
//  - VIC: copy, then a VIC job, collected at once;
//  - VIC pipelined: the next frame is copied while the VIC converts this one;
//  - CPU: each piece read and converted at once with NEON on our own core
//    (cpu_convert.c), no VIC at all. In GPU-heavy games every VIC job waits
//    behind the compositor's, itself waiting on the game's GPU (17-41 ms for a
//    0.6 ms job, 01003ec027cc8000). 1280x720 rects only (CpuCanConvert).
// The loop does one frame after the other on one core, so a frame costs the
// sum of its steps: every step is measured on every frame of the current
// mode, and the others' costs are probed beside it, without switching: in CPU
// mode a VIC job into the VIC's own output, not waited on (its cost is the
// queue in front of it, not the picture), polled between the pieces of the
// CPU frames; on the VIC a CPU conversion of the last frame into the bounce.
// Probes only while the capture falls behind a game presenting steadily (2 s
// windows: the share of the game's presents encoded; loading screens and
// pauses do not count). A mode predicted MODE_GAIN_PCT faster twice in a row
// is switched to, then checked: 2 windows later its share must not be worse
// than before, or it is left again and not tried for MODE_BLOCK_NS. Pipelined
// is a candidate only where the CPU cannot convert (its overlap cannot be
// predicted well; the check guards it).
enum { MODE_VIC, MODE_PIPE, MODE_CPU, MODES };
static const char* const kModeName[MODES] = { "VIC", "VIC pipelined", "CPU" };
#define MODE_WINDOW_NS    2000000000ULL
#define MODE_MIN_FPS      20           // a window counts when the game presented at least this fast
#define MODE_RATE_PCT     20           // ... at a rate within this much of the previous window's
#define MODE_BEHIND_PML   950          // behind: under this share (per mille) of the game's presents
#define MODE_GAIN_PCT     15           // predicted this much faster, twice in a row: switch
#define MODE_CHECK_PCT    3            // after a switch: a share this much (relative) worse goes back
#define MODE_BLOCK_NS     20000000000ULL   // a mode that failed its check is not tried for this long, doubled at each failure in a row
#define MODE_BLOCK_MAX_NS 160000000000ULL
#define PROBE_EVERY_NS    3000000000ULL
#define PROBE_STALE_NS    30000000000ULL  // a probe older than this no longer counts
#define PROBE_KEEP        5            // a probed cost: the median of the last ones still fresh
#define H264_DUMP_FRAMES  0            // 3a wrote the first N frames to <program>.h264; off while streaming (SD writes cost latency)

extern volatile bool g_nativeCaptureRunning;


static u8 alignas(0x1000) g_chunk[CHUNK];

typedef struct {
    // what vi-tap says
    UvcTapSurface surf;
    bool   separate;        // one nvmap object per slot
    u64    span;            // one object: the whole set

    GameDbg dbg;
    bool   attached;
    u64    attachTick;

    // slot buffers
    u64    cand[SLOTMATCH_MAX_CAND];
    u32    devc[SLOTMATCH_MAX_CAND];
    u32    ncand;
    u8     slotCand[SLOTMATCH_MAX_SLOTS];
    bool   locked;          // slot -> buffer known
    const char* how;
    SlotMatcher matcher;
    u32    lastCount;

    // reads
    u64    lastSeq;
    u64    nextRead;
    u32    reads, readFails, dumps;
    u32    ivReads;          // since the last periodic line
    u64    ivNsSum, ivNsMax;
    u64    dumpedSeq;        // the present last dumped: one dump per pause
    u64    sampleNsMax;

    // memory from the donor (phase 2b)
    void*  mem;
    u64    memSize;
    bool   memYielded;       // given back to an applet: not taken again this session
    u64    nextYieldCheck;

    // VIC conversions (phase 2b-3): every present once the slots are known
    u32    convs, convFails, fenceTimeouts;
    u32    ivConvs;
    u64    ivReadNs, ivVicNs, ivFenceNs, ivFenceMax, ivReadBytes;
    u32    ivFences, ivCopies;
    u64    ivVicWaitNs;      // of ivVicNs, what the loop spent waiting for the VIC
    u32    ivOverlapped, ivAlone;  // pipelined jobs collected after the next copy / with nothing to overlap
    VicSource jobSrc, lastSrc;   // the job's source, and the last one encoded
    bool   haveLast;
    bool   jobCpu;           // the job's picture was made by the CPU: nothing to collect
    bool   encPending;       // its encode in flight (NvencSubmit): collected as soon as done, then sent
    UvcTapPresent encJob;    // the present it encodes
    bool   pictBad;          // the encoder's picture holds a torn or partial frame: not to be re-sent
    u32    ivVicJobs, ivCpuN, ivCpuUnsup;
    u64    ivCpuNs, ivCpuMax;
    // where a CPU frame's time goes (logging builds): the conversion (wall
    // clock, and what the thread actually ran), the flush; and once a second
    // the last piece converted again while it is still in the cache, tiled
    // (NVENC's layout) and in row order, and only read
    u64    ivConvNs, ivConvRunNs, ivFlushNs;
    u64    nextHot, hotOff, hotBl, hotPitch, hotRead;
    u32    hotFirst, hotEnd, hotN, hotLines;
    VicSource hotSrc;
    bool   hotOk;
    bool   jobTimed;         // collected at once: its time is the job's

    // the mode (MODE_*)
    u32    mode;
    bool   pipe;             // mode == MODE_PIPE: copy the next frame while the VIC converts this one
    bool   pending;          // a VIC job in flight
    bool   again;            // a newer present was left for after the job: look again at once
    UvcTapPresent job;       // its present
    u32    jobCopy;          // the slot copy it reads
    // the steps' costs (moving averages, ns; 0 unknown): the read through the
    // slot copy (VIC routes) or piece by piece (CPU), the VIC job, the CPU
    // conversion, and a pipelined frame as measured (the encode runs beside
    // the next frame: not a cost of the loop)
    u64    eReadCopy, eReadFused, eVic, eCpu, ePipe;
    // the last probes of each (ns, and when): one probe taken in a bad moment
    // (01003ec027cc8000: 50 ms at the end of a loading screen, 11-15 after)
    // must not decide alone
    u64    vicP[PROBE_KEEP], vicPt[PROBE_KEEP], cpuP[PROBE_KEEP], cpuPt[PROBE_KEEP];
    u32    vicPn, cpuPn;
    u64    jobPipeNs;        // the pipelined job's read and VIC wait
    bool   probing;          // a VIC probe in flight
    u64    nextProbe;
    u32    ivProbes;
    u64    ivProbeNs;
    // the 2 s windows
    u64    winStartNs;       // the window being watched (0: none)
    u64    winSeq;
    u32    winConvs;
    u32    lastRate;         // the game's presents a second in the last window
    u32    pml[2];           // the last two steady windows' shares (per mille), newest first; 0 none
    u32    want, wantMode;   // windows in a row a faster mode was predicted, and which
    u32    checkWin, prevMode, prevPml;  // after a switch: windows left before the check, what it replaced
    u64    blockUntil[MODES], blockNs[MODES];
    // H.264 (phase 3a) and the camera (3b)
    u32    encFails;
    u64    ivEncNs;
    bool   wasStreaming;     // a new stream starts on a keyframe
    u32    commits;          // UvcDeviceCommitCount at the last frame: a new consumer wants a keyframe
    u64    lastEncSeq;       // the present encoded last
    u32    ivPickReady, ivPickWaited, ivSkipped;
    u32    torn, rereads;    // copies the game changed under us
    u64    nextSurfaceCheck; // the game may make new frame buffers mid-session
    bool   needKey;          // a send failed: the host lost a frame, restart the chain
    u32    sent, sendFails, ivSent;
    u64    ivSentBytes;
    // a frame's whole work from its read to its hand-over to the send
    u64    ivWorkNs, jobStartNs;
    u32    ivSendPieces, ivWorkN;
    FsFile h264;
    bool   h264Open;
    u64    h264Off;
    u32    h264Frames;
} Session;

static Session g_s;

// Back-off for sessions that end right after starting (a bug, or a game we
// cannot handle): never cycle the donor and the engines in a tight loop
static u64 g_retryAtNs = 0, g_retryDelayNs = 0;

// Keep-alive: an app with the camera open never sees the stream go silent.
// Frames that stop for seconds (between games, HOME, a loading screen) and
// come back with a timestamp seconds later make players hold the last frame
// as if it lasted that long and stop reading (ffplay had to be killed between
// games). After FILLER_NS without a frame: during a game, the last picture
// encoded again (a tiny P frame); between games, a black keyframe made once.
#define FILLER_NS 200000000ULL
static u8 g_black[0x8000];
static u32 g_blackLen = 0;
static u64 _nowNs(void) { return armTicksToNs(armGetSystemTick()); }

static u64 g_lastSentNs = 0;
static u32 g_fillers = 0;

// Background send: the capture loop hands an encoded frame to this thread and
// goes on with the next one while USB carries it (2.2-4.3 ms a frame measured
// in 01003ec027cc8000, the loop waiting on every 16 KB). One frame at a time,
// copied into donor memory (set per session, _sendSetBuffer): the encoder
// reuses its own buffer at the next encode. Without that buffer, or for a
// frame larger than it, the frame is sent in place, as before. A failure is
// only known at the next hand-over: that frame is already encoded, so the
// keyframe the host needs comes one frame later than with a send in place.
#define SEND_PRIORITY 0x3A  // above the capture loop's 0x3B: it mostly waits for USB, posting needs little
static struct {
    Mutex lock;
    CondVar go, idle;
    Thread thread;
    bool up, quit, busy;
    u8* buf;
    u32 cap, len;
    u64 tsMs;
    bool idr;
    u64 presentNs;            // when the game presented the frame going out (0: a keep-alive filler)
    u32 failed;               // failures not yet told to the capture loop
    u64 ns, nsMax, waitNs;    // since the last periodic line: send time, and the loop's wait for the thread
    u32 n;
    // the Switch's whole delay: the game's present to the frame's last byte
    // out over USB (game frames only)
    u64 latNs, latMax;
    u32 latN;
} g_send;
static u8 alignas(0x1000) g_sendStack[0x4000];

// (lock held)
static void _sendDone(u64 dt, u64 presentNs)
{
    g_send.ns += dt;
    if (dt > g_send.nsMax)
        g_send.nsMax = dt;
    g_send.n++;
    if (presentNs) {
        const u64 lat = _nowNs() - presentNs;
        g_send.latNs += lat;
        if (lat > g_send.latMax)
            g_send.latMax = lat;
        g_send.latN++;
    }
}

static void _sendThread(void* arg)
{
    (void)arg;
    mutexLock(&g_send.lock);
    for (;;) {
        while (!g_send.busy && !g_send.quit)
            condvarWait(&g_send.go, &g_send.lock);
        if (!g_send.busy)
            break;  // quit, nothing left to send
        const u8* d = g_send.buf;
        const u32 len = g_send.len;
        const u64 ts = g_send.tsMs, presentNs = g_send.presentNs;
        const bool idr = g_send.idr;
        mutexUnlock(&g_send.lock);
        const u64 t0 = _nowNs();
        const bool ok = UvcDeviceSendFrame(d, len, ts, idr);
        const u64 dt = _nowNs() - t0;
        mutexLock(&g_send.lock);
        _sendDone(dt, presentNs);
        if (!ok)
            g_send.failed++;
        g_send.busy = false;
        condvarWakeAll(&g_send.idle);
    }
    mutexUnlock(&g_send.lock);
}

static void _sendStart(void)
{
    mutexInit(&g_send.lock);
    condvarInit(&g_send.go);
    condvarInit(&g_send.idle);
    if (R_SUCCEEDED(threadCreate(&g_send.thread, _sendThread, NULL, g_sendStack, sizeof(g_sendStack), SEND_PRIORITY, -2)) &&
        R_SUCCEEDED(threadStart(&g_send.thread)))
        g_send.up = true;
    else
        LOG("native: no send thread - frames are sent in the capture loop\n");
}

static void _sendStop(void)
{
    if (!g_send.up)
        return;
    mutexLock(&g_send.lock);
    g_send.quit = true;
    condvarWakeAll(&g_send.go);
    mutexUnlock(&g_send.lock);
    threadWaitForExit(&g_send.thread);
    threadClose(&g_send.thread);
    g_send.up = false;
}

// Waits for the frame going out, if any (lock held)
static void _sendWaitIdle(void)
{
    const u64 w0 = _nowNs();
    while (g_send.busy)
        condvarWait(&g_send.idle, &g_send.lock);
    g_send.waitNs += _nowNs() - w0;
}

// The donor memory the frames are copied to (NULL: none - before it goes)
static void _sendSetBuffer(u8* buf, u32 cap)
{
    mutexLock(&g_send.lock);
    _sendWaitIdle();
    g_send.buf = buf;
    g_send.cap = cap;
    mutexUnlock(&g_send.lock);
}

// A frame to the camera. False: it failed (sent in place), or an earlier one
// did (sent in the background) - the host lost a frame, the chain restarts.
// presentNs: when the game presented it (0 for a keep-alive filler)
static bool _send(const u8* data, u32 len, u64 tsMs, bool idr, u64 presentNs)
{
    mutexLock(&g_send.lock);
    _sendWaitIdle();  // in order: the one going out first
    const bool earlierFailed = g_send.failed != 0;
    g_send.failed = 0;
    if (!g_send.up || !g_send.buf || len > g_send.cap) {
        mutexUnlock(&g_send.lock);
        const u64 t0 = _nowNs();
        const bool ok = UvcDeviceSendFrame(data, len, tsMs, idr);
        const u64 dt = _nowNs() - t0;
        mutexLock(&g_send.lock);
        _sendDone(dt, presentNs);
        mutexUnlock(&g_send.lock);
        return ok && !earlierFailed;
    }
    memcpy(g_send.buf, data, len);
    g_send.len = len;
    g_send.tsMs = tsMs;
    g_send.presentNs = presentNs;
    g_send.idr = idr;
    g_send.busy = true;
    condvarWakeOne(&g_send.go);
    mutexUnlock(&g_send.lock);
    return !earlierFailed;
}

// When the running application last changed: how long a game switch keeps
// the camera without frames (logged with the first frame of a session)
static u64 g_appPid = 0, g_appSinceNs = 0;


// ============================================================================
// SD card
// ============================================================================

static FsFileSystem g_sd;
static bool g_sdOpen = false;

static bool _sdOpen(void)
{
    if (g_sdOpen)
        return true;
    if (R_FAILED(fsInitialize()))  // refcounted; FILE_LOGGING opened it too
        return false;
    if (R_FAILED(fsOpenSdCardFileSystem(&g_sd)))
        return false;
    fsFsCreateDirectory(&g_sd, "/config");
    fsFsCreateDirectory(&g_sd, "/config/sysdvr-uvc");
    fsFsCreateDirectory(&g_sd, DUMP_DIR);
    g_sdOpen = true;
    return true;
}

#if NATIVE_DEBUG
static bool _sdWrite(const char* path, const void* data, size_t size)
{
    if (!_sdOpen())
        return false;
    fsFsDeleteFile(&g_sd, path);
    if (R_FAILED(fsFsCreateFile(&g_sd, path, size, 0)))
        return false;
    FsFile f;
    if (R_FAILED(fsFsOpenFile(&g_sd, path, FsOpenMode_Write, &f)))
        return false;
    Result rc = fsFileWrite(&f, 0, data, size, FsWriteOption_Flush);
    fsFileClose(&f);
    return R_SUCCEEDED(rc);
}
#endif

// ============================================================================
// Finding the slot buffers
// ============================================================================

static bool _deviceMapped(const MemoryInfo* mi)
{
    return (mi->attr & MemAttr_IsDeviceMapped) || mi->device_refcount > 0;
}

// One object holding every slot (MK8): a device-mapped region of exactly the
// set's size is the swapchain, slots at their offsets - nothing to match.
static bool _findExact(Session* s, u64* base)
{
    u64 addr = 0;
    for (u32 steps = 0; steps < 4000; steps++) {
        MemoryInfo mi = {0};
        if (!GameDbgQuery(&s->dbg, &mi, addr) || mi.size == 0)
            return false;
        if (_deviceMapped(&mi) && mi.size == s->span) {
            *base = mi.addr;
            return true;
        }
        u64 next = mi.addr + mi.size;
        if (next <= addr)
            return false;
        addr = next;
    }
    return false;
}

// Otherwise every device-mapped region that could hold the buffers gives
// candidates (SFT FindSlotCandidates): one per buffer-sized step for one
// object per slot (rounded up to a power-of-two alignment: Moonlight keeps a
// 3.75 MB buffer in a 4 MB block), or one per slot offset for one object.
static u32 _findCandidates(Session* s)
{
    static const u64 aligns[] = { 0x1000, 0x10000, 0x20000, 0x40000, 0x100000, 0x200000, 0x400000 };
    const u64 buf = s->surf.buf_size;
    u64 addr = 0;
    u32 n = 0;
    for (u32 steps = 0; steps < 4000 && n < SLOTMATCH_MAX_CAND; steps++) {
        MemoryInfo mi = {0};
        if (!GameDbgQuery(&s->dbg, &mi, addr) || mi.size == 0)
            break;
        if (_deviceMapped(&mi) && s->separate) {
            for (u32 a = 0; a < sizeof(aligns) / sizeof(aligns[0]); a++) {
                const u64 step = (buf + aligns[a] - 1) & ~(aligns[a] - 1);
                if (mi.size % step == 0 && mi.size / step <= 8) {
                    for (u64 o = 0; o < mi.size && n < SLOTMATCH_MAX_CAND; o += step) {
                        s->devc[n] = mi.device_refcount;
                        s->cand[n++] = mi.addr + o;
                    }
                    break;
                }
            }
        } else if (_deviceMapped(&mi) && mi.size >= s->span) {
            for (u32 k = 0; k < s->surf.num_slots && n < SLOTMATCH_MAX_CAND; k++) {
                s->devc[n] = mi.device_refcount;
                s->cand[n++] = mi.addr + s->surf.slot_offset[k];
            }
        }
        u64 next = mi.addr + mi.size;
        if (next <= addr)
            break;
        addr = next;
    }
    // one object in several regions that could hold it (Sonic: two pools
    // laid out alike): the swapchain is also mapped for the display, so keep
    // the regions mapped for 2+ devices when enough of them remain
    if (!s->separate && n > s->surf.num_slots) {
        u32 kept = 0;
        for (u32 c = 0; c < n; c++)
            kept += s->devc[c] >= 2;
        if (kept >= s->surf.num_slots && kept < n) {
            u32 k2 = 0;
            for (u32 c = 0; c < n; c++) {
                if (s->devc[c] >= 2) {
                    s->cand[k2] = s->cand[c];
                    s->devc[k2++] = s->devc[c];
                }
            }
            n = kept;
        }
    }
    return n;
}

// FNV-1a over SIG_POINTS 64-byte samples spread over the buffer. *varied
// false when every sample is one repeated 4-byte value (a cleared buffer).
static u32 _sampleSig(Session* s, u64 base, bool* varied)
{
    const u64 step = (s->surf.buf_size / SIG_POINTS) & ~63ULL;
    u32 h = 2166136261u, first = 0;
    bool var = false;
    for (u32 j = 0; j < SIG_POINTS; j++) {
        u8 alignas(64) b[64];
        if (!GameDbgRead(&s->dbg, b, base + j * step + ((step / 2) & ~63ULL), sizeof(b))) {
            *varied = false;
            return 0;
        }
        if (j == 0)
            memcpy(&first, b, sizeof(first));
        for (u32 i = 0; i < sizeof(b); i += 4)
            var |= memcmp(b + i, &first, 4) != 0;
        for (u32 i = 0; i < sizeof(b); i++)
            h = (h ^ b[i]) * 16777619u;
    }
    *varied = var;
    return h;
}

static u64 _slotAddr(Session* s, s32 slot)
{
    if (slot < 0 || (u32)slot >= s->surf.num_slots)
        slot = 0;
    return s->cand[s->slotCand[slot]];
}

static void _logMatch(Session* s, const char* what)
{
    char line[256];
    int len = snprintf(line, sizeof(line), "native: slots %s:", what);
    for (u32 k = 0; k < s->surf.num_slots && len > 0 && len < (int)sizeof(line); k++)
        len += snprintf(line + len, sizeof(line) - len, " %u->0x%010lx", k, s->cand[s->slotCand[k]]);
    LOG("%s\n", line);
}

// ============================================================================
// Session
// ============================================================================

// Borrow the donor's memory and prove it usable: a pattern on every 64 KB,
// read back. 2b-3 puts the VIC buffers here.
static void _takeMemory(Session* s)
{
    if (s->memYielded || !DonorObtain(DONOR_MB, &s->mem, &s->memSize)) {
        s->mem = NULL;
        return;
    }
    u8* m = (u8*)s->mem;
    u32 bad = 0;
    for (u64 off = 0; off + 8 <= s->memSize; off += 0x10000)
        *(volatile u64*)(m + off) = off ^ 0x5A5A5A5A5A5A5A5AULL;
    for (u64 off = 0; off + 8 <= s->memSize; off += 0x10000)
        bad += *(volatile u64*)(m + off) != (off ^ 0x5A5A5A5A5A5A5A5AULL);
    LOG("native: donor memory %lu MB %s\n", s->memSize >> 20, bad ? "CORRUPT" : "checked (read/write)");
    s->nextYieldCheck = _nowNs() + 1000000000ULL;
    if (bad || !VicEngineOpen(s->mem, s->memSize, s->surf.buf_size))
        return;
    GameDbgSetBounce(VicEngineBounce(), VicEngineBounceSize());

    // the encoder's arena and frame buffer after the VIC's buffers
    const u64 arenaOff = (VicEngineBytesUsed() + 0xFFFFF) & ~0xFFFFFULL;
    const u64 frameOff = (arenaOff + NvencArenaSize() + 0xFFFF) & ~0xFFFFULL;
    if (frameOff + NvencFrameBufferSize() > s->memSize) {
        LOG("native: %lu MB of donor memory: not enough for the encoder (needs %lu MB) - VIC only\n",
            s->memSize >> 20, (frameOff + NvencFrameBufferSize() + 0xFFFFF) >> 20);
        return;
    }
    u8* m8 = (u8*)s->mem;
    if (!NvencOpen(m8 + arenaOff, NvencArenaSize(), m8 + frameOff, NvencFrameBufferSize(), ENCODE_QP))
        return;
    if (!g_blackLen && NvencEncodeBlack(g_black, sizeof(g_black), &g_blackLen))
        LOG("native: black keyframe for the gaps between games: %u B\n", g_blackLen);

    // the second slot copy, for pipelining, after the encoder's frame buffer
    const u64 copyOff = (frameOff + NvencFrameBufferSize() + 0xFFFF) & ~0xFFFFULL;
    const u64 copySize = ((u64)s->surf.buf_size + 0xFFF) & ~0xFFFULL;
    const bool copy2 = copyOff + copySize <= s->memSize && VicEngineAddSlotCopy(m8 + copyOff, copySize);
    if (copy2)
        LOG("native: second slot copy (%lu KB): pipelining possible, tried once streaming\n", copySize / 1024);
    else
        LOG("native: no second slot copy (%lu MB of donor memory, needs %lu) - copy and conversion one after the other\n",
            s->memSize >> 20, (copyOff + copySize + 0xFFFFF) >> 20);

    // the background send's copy of a frame, after the rest; frames larger
    // than what is left are sent in place
    const u64 sendOff = ((copy2 ? copyOff + copySize : copyOff) + 0xFFFF) & ~0xFFFFULL;
    if (sendOff < s->memSize && s->memSize - sendOff >= 0x40000) {
        const u64 cap = s->memSize - sendOff < NvencFrameBufferSize() ? s->memSize - sendOff : NvencFrameBufferSize();
        _sendSetBuffer(m8 + sendOff, (u32)cap);
        LOG("native: frames sent in the background (copy buffer %lu KB)\n", cap / 1024);
    } else {
        LOG("native: no room for the background send's buffer - frames sent in the capture loop\n");
    }
}

// The first H264_DUMP_FRAMES encoded frames of a session, as an Annex-B
// elementary stream (each frame carries its SPS/PPS): ffplay plays it as is.
// Written through g_chunk: the frame is in donor memory.
static void _h264Append(Session* s, const u8* data, u32 len)
{
    if (H264_DUMP_FRAMES == 0 || s->h264Frames >= H264_DUMP_FRAMES)
        return;
    if (!s->h264Open) {
        if (!_sdOpen())
            return;
        char path[96];
        UvcTapPresent p = {0};
        TapClientGetLatestPresent(&p);
        snprintf(path, sizeof(path), DUMP_DIR "/%016lx.h264", p.program_id);
        fsFsDeleteFile(&g_sd, path);
        if (R_FAILED(fsFsCreateFile(&g_sd, path, 0, 0)) || R_FAILED(fsFsOpenFile(&g_sd, path, FsOpenMode_Write | FsOpenMode_Append, &s->h264))) {
            s->h264Frames = H264_DUMP_FRAMES;  // do not retry every frame
            return;
        }
        s->h264Open = true;
        s->h264Off = 0;
        LOG("native: writing the first %u frames to %s\n", H264_DUMP_FRAMES, path);
    }
    for (u32 off = 0; off < len; off += CHUNK) {
        const u32 n = len - off < CHUNK ? len - off : CHUNK;
        memcpy(g_chunk, data + off, n);
        if (R_FAILED(fsFileWrite(&s->h264, s->h264Off, g_chunk, n, FsWriteOption_None))) {
            s->h264Frames = H264_DUMP_FRAMES;
            break;
        }
        s->h264Off += n;
    }
    if (++s->h264Frames >= H264_DUMP_FRAMES) {
        fsFileFlush(&s->h264);
        fsFileClose(&s->h264);
        s->h264Open = false;
        LOG("native: H.264 dump complete: %u frames, %lu KB\n", H264_DUMP_FRAMES, s->h264Off / 1024);
    }
}

// The order matters: no dmnt read may use the bounce once the memory goes;
// the nvdrv session must be closed (freeing every pin) before the unmap.
static void _finish(Session* s);
static void _probeDrain(Session* s);
static void _encCollect(Session* s, bool wait);

static void _releaseMemory(Session* s)
{
    if (!s->mem)
        return;
    _finish(s);  // never a job in flight on memory that goes
    _encCollect(s, true);
    _probeDrain(s);
    _sendSetBuffer(NULL, 0);  // the frame going out from it first
    GameDbgSetBounce(NULL, 0);
    NvencClose();
    VicEngineClose();
    DonorRelease();
    s->mem = NULL;
}

// The source as the VIC must read it: the game presents a rect from (0,0)
// of its surface (handheld: 1280x720 of 1920x1080). Only a 16:9 crop that
// fits is trusted (SFT PresentRect); otherwise the whole surface, scaled.
static void _source(const Session* s, const UvcTapPresent* p, VicSource* src)
{
    src->w = s->surf.width;
    src->h = s->surf.height;
    src->stride_px = s->surf.stride_px;
    src->blk_h_log2 = s->surf.block_h_log2;
    // NvColorFormat A8R8G8B8 (bytes B,G,R,A) is the VIC's A8B8G8R8, and the
    // usual A8B8G8R8 (bytes R,G,B,A) its A8R8G8B8 - the matrix is measured so
    src->pixfmt = s->surf.color_format == 0x100d12120ULL ? VIC_PIXFMT_A8B8G8R8 : VIC_PIXFMT_A8R8G8B8;
    src->flip = p->transform & 3;
    src->copy = 0;
    const u32 cw = p->crop[2] - p->crop[0], ch = p->crop[3] - p->crop[1];
    const bool ok = p->crop[0] == 0 && p->crop[1] == 0 && p->crop[2] > 0 && p->crop[3] > 0 &&
                    cw >= 320 && ch >= 180 && cw <= src->w && ch <= src->h &&
                    (cw * 9 > ch * 16 ? cw * 9 - ch * 16 : ch * 16 - cw * 9) <= cw * 9 / 50;
    src->rect_w = ok ? cw : src->w;
    src->rect_h = ok ? ch : src->h;
}

// Copy what the VIC will sample into the slot copy, at the same offsets: the
// block rows covering rect_h, and in each only the GOB columns covering
// rect_w (blocks lie left to right, so they are contiguous). Handheld 720p
// of a 1080p surface: 6 x 640 KB = 3.9 MB instead of 8.4 (SFT CornerFast).
typedef struct { u64 rowBytes, readPerRow, rows; } Region;

static Region _region(const VicSource* src)
{
    const u64 blockRows = 8u << src->blk_h_log2;
    const u64 blockBytes = 512u << src->blk_h_log2;
    const u64 gobsX = ((u64)src->stride_px * 4 + 63) / 64;
    Region r = { gobsX * blockBytes, (((u64)src->rect_w * 4 + 63) / 64) * blockBytes,
                 (src->rect_h + blockRows - 1) / blockRows };
    return r;
}

// Did the game draw into the slot while we copied it? Eight 64-byte samples,
// one per block row, against the copy (SFT ChangedSinceRead, WillMidia's PR
// #4). A slow copy (through dmnt:cht ~20 ms) can outlast the game's next
// frame: a torn picture, half old half new.
static bool _changedSinceRead(Session* s, u64 addr, const VicSource* src)
{
    const Region r = _region(src);
    const u8* copy = VicEngineSlotCopy(src->copy);
    for (u32 k = 0; k < 8 && r.rows; k++) {
        const u64 off = (k % r.rows) * r.rowBytes + (((u64)k * 0x2340u % r.readPerRow) & ~63ULL);
        if (off + 64 > s->surf.buf_size)
            continue;
        u8 alignas(64) b[64];
        if (!GameDbgRead(&s->dbg, b, addr + off, sizeof(b)))
            return false;
        armDCacheFlush((void*)(copy + off), 64);  // our copy's view, as the engine reads it
        if (memcmp(b, copy + off, sizeof(b)) != 0)
            return true;
    }
    return false;
}

// The encode in flight: done? (wait: until it is.) Polled between the
// pieces of the next frame and once a pass, so a frame goes to the send
// thread as soon as NVENC is done with it - the encode no longer holds the
// loop, and no frame waits for the next one to be read.
static void _encCollect(Session* s, bool wait)
{
    if (!s->encPending)
        return;
    const u8* frame = NULL;
    u32 len = 0;
    bool idr = false;
    u64 encNs = 0;
    const int r = wait ? NvencCollect(&frame, &len, &idr, &encNs) : NvencPoll(&frame, &len, &idr, &encNs);
    if (r == 0)
        return;
    s->encPending = false;
    if (r < 0) {
        s->encFails++;
        s->needKey = true;
        return;
    }
    s->ivEncNs += encNs;
    _h264Append(s, frame, len);
    const UvcTapPresent* p = &s->encJob;
    const bool sentOk = _send(frame, len, armTicksToNs(p->tick) / 1000000, idr, armTicksToNs(p->tick));
    s->ivSendPieces += (len + 0x4000 - 12 - 1) / (0x4000 - 12);  // UvcDeviceSendFrame's payload size
    if (sentOk) {
        g_lastSentNs = _nowNs();
        if (s->sent == 0) {
            const u64 now = _nowNs();
            LOG("native: first frame sent (%s, %u KB): %lu ms after the game appeared, %lu ms after attaching\n",
                idr ? "keyframe" : "P frame", len / 1024, g_appSinceNs ? (now - g_appSinceNs) / 1000000 : 0,
                (now - s->attachTick) / 1000000);
        }
        s->sent++;
        s->ivSent++;
        s->ivSentBytes += len;
    } else {
        s->sendFails++;
        s->needKey = true;  // the host lost a frame: P frames after it would decode wrong
    }
}

static bool _readRegion(Session* s, u64 addr, const VicSource* src, u64* bytes)
{
    u8* copy = VicEngineSlotCopy(src->copy);
    const Region reg = _region(src);
    const u64 rowBytes = reg.rowBytes, readPerRow = reg.readPerRow, rows = reg.rows;
    u64 total = 0;
    for (u64 br = 0; br < rows; br++) {
        const u64 off = br * rowBytes;
        if (off + readPerRow > s->surf.buf_size)
            break;
        if (!GameDbgRead(&s->dbg, copy + off, addr + off, readPerRow))
            return false;
        armDCacheFlush(copy + off, readPerRow);  // the engine reads it from memory
        _encCollect(s, false);
        total += readPerRow;
    }
    *bytes = total;
    return true;
}

// Which present to encode (SFT M96, "capture the newest FINISHED present").
// A game that queues frames ahead of its GPU has several in flight: waiting
// on the newest cost ~39 ms a frame in a heavy game (12 fps). Among the
// presents since the last one encoded: the newest whose fence is reached; if
// none is, the oldest - the GPU finishes them in order, it is next. Its slot
// stays intact until the present after the next one is drawn; the copy is
// checked anyway (_changedSinceRead).
// With a job in flight the fence wait is short (PIPE_FENCE_NS): the VIC
// works meanwhile, but the job's frame waits on us. A GPU-heavy game is
// usually still drawing its newest present when we look (01003ec027cc8000:
// without this wait, the copy almost never overlapped the job). Not done by
// then: the present is left for after the job (s->again).
// The present's fence, waited on - while an encode is in flight, polling it
// too: its frame would otherwise wait for the next one's GPU (the Switch's
// delay rose from ~19 to ~28 ms at worst in Tetris before this)
static bool _waitFence(Session* s, const UvcTapPresent* p, u64 timeoutNs, u64* waited)
{
    if (!s->encPending)
        return VicEngineWaitFence(p->fence_count, p->fence, timeoutNs, waited);
    const u64 t0 = _nowNs();
    bool ok = true;
    while (!VicEngineFenceReached(p->fence_count, p->fence)) {
        _encCollect(s, false);
        if (_nowNs() - t0 > timeoutNs) {
            ok = false;
            break;
        }
        svcSleepThread(250000);
    }
    *waited = _nowNs() - t0;
    return ok;
}

static bool _pickPresent(Session* s, const UvcTapPresent* latest, UvcTapPresent* out, bool inFlight)
{
    UvcTapPresent r[UVCTAP_RECENT_MAX];
    u32 n = 0;
    if (TapClientVersion() < 3 || R_FAILED(TapClientGetRecentPresents(r, UVCTAP_RECENT_MAX, &n)) || n == 0) {
        r[0] = *latest;  // vi-tap v2: only the latest
        n = 1;
    }
    s32 pick = -1, oldest = -1;
    for (u32 i = 0; i < n; i++) {  // newest first
        if (r[i].seq <= s->lastEncSeq || r[i].process_id != s->dbg.pid)
            break;
        if (VicEngineFenceReached(r[i].fence_count, r[i].fence)) {
            pick = (s32)i;
            break;
        }
        oldest = (s32)i;
    }
    if (pick >= 0) {
        s->ivPickReady++;
    } else if (oldest >= 0) {
        u64 waited = 0;
        if (!_waitFence(s, &r[oldest], inFlight ? PIPE_FENCE_NS : FENCE_TIMEOUT_NS, &waited)) {
            if (inFlight) {
                s->again = true;
                return false;
            }
            s->fenceTimeouts++;
        }
        s->ivFenceNs += waited;
        s->ivFences++;
        if (waited > s->ivFenceMax)
            s->ivFenceMax = waited;
        s->ivPickWaited++;
        pick = oldest;
    } else {
        return false;  // nothing new
    }
    if (s->lastEncSeq && r[pick].seq > s->lastEncSeq + 1)
        s->ivSkipped += (u32)(r[pick].seq - s->lastEncSeq - 1);
    *out = r[pick];
    return true;
}

// A moving average over ~2^shift samples (0: none yet)
static void _ema(u64* e, u64 x, u32 shift)
{
    *e = *e ? (u64)((s64)*e + (((s64)x - (s64)*e) >> shift)) : x;
}

// A probe's result: kept with the last ones; the cost is their median (of
// those still fresh)
static u64 _probeAdd(u64* v, u64* t, u32* n, u64 ns)
{
    const u64 now = _nowNs();
    v[*n % PROBE_KEEP] = ns;
    t[*n % PROBE_KEEP] = now;
    (*n)++;
    u64 f[PROBE_KEEP];
    u32 k = 0;
    for (u32 i = 0; i < PROBE_KEEP && i < *n; i++) {
        if (now - t[i] < PROBE_STALE_NS)
            f[k++] = v[i];
    }
    for (u32 i = 1; i < k; i++) {  // sort the (at most PROBE_KEEP)
        for (u32 j = i; j > 0 && f[j - 1] > f[j]; j--) {
            const u64 x = f[j]; f[j] = f[j - 1]; f[j - 1] = x;
        }
    }
    return k % 2 ? f[k / 2] : (f[k / 2 - 1] + f[k / 2]) / 2;
}

static u32 _probesFresh(const u64* t, u32 n)
{
    const u64 now = _nowNs();
    u32 k = 0;
    for (u32 i = 0; i < PROBE_KEEP && i < n; i++)
        k += now - t[i] < PROBE_STALE_NS;
    return k;
}

// The VIC probe in flight: done yet? (Between the pieces of a CPU frame and
// once a pass: a few ms of precision.)
static void _probePoll(Session* s)
{
    if (!s->probing)
        return;
    u64 ns = 0;
    const int r = VicEngineJobPoll(&ns);
    if (r == 0)
        return;
    s->probing = false;
    s->nextProbe = _nowNs() + PROBE_EVERY_NS;
    if (r > 0) {
        s->eVic = _probeAdd(s->vicP, s->vicPt, &s->vicPn, ns);
        s->ivProbes++;
        s->ivProbeNs += ns;
    }
}

// Before the VIC is used for real (or its memory goes): the probe collected
static void _probeDrain(Session* s)
{
    if (!s->probing)
        return;
    u64 ns = 0;
    s->probing = false;
    s->nextProbe = _nowNs() + PROBE_EVERY_NS;
    if (VicEngineJobWait(&ns, NULL))
        s->eVic = _probeAdd(s->vicP, s->vicPt, &s->vicPn, ns);
}

// The CPU route (MODE_CPU): each block row of the frame read into the bounce
// buffer (cached, plain memory - dmnt:cht writes it there with no copy of
// ours, a direct read too) and converted at once into the encoder's picture,
// while it is still in the cache: no slot copy, no second pass over memory.
// The torn check's 8 samples are kept from the pieces as they pass (the
// same places as _changedSinceRead).
typedef struct { u8 b[8][64]; bool valid[8]; } TornSamples;

static u64 _sampleOff(const Region* r, u32 k)
{
    return (k % r->rows) * r->rowBytes + (((u64)k * 0x2340u % r->readPerRow) & ~63ULL);
}

#if NATIVE_DEBUG
// The time this thread has run, all cores (ns): against the wall clock, what
// other threads on our core took
static u64 _runNs(void)
{
    u64 t = 0;
    svcGetInfo(&t, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total);
    return armTicksToNs(t);
}
#endif

typedef struct { u64 conv, run, flush; } CpuSplit;

static bool _readConvert(Session* s, u64 addr, const VicSource* src, TornSamples* ts, u64* bytes, u64* readNs, u64* cpuNs,
                         CpuSplit* sp)
{
    const Region r = _region(src);
    u8* chunk = VicEngineBounce();
    u8 *luma = NULL, *chroma = NULL;
    u32 lumaBytes = 0, chromaBytes = 0;
    NvencPictureCpu(&luma, &chroma, &lumaBytes, &chromaBytes);
    if (!chunk || !luma || r.readPerRow > VicEngineBounceSize())
        return false;
    const u32 lines = CpuBlockRowLines(src);
    memset(ts->valid, 0, sizeof(ts->valid));
    for (u64 br = 0; br < r.rows; br++) {
        const u64 off = br * r.rowBytes;
        if (off + r.readPerRow > s->surf.buf_size)
            break;
        const u64 t0 = _nowNs();
        if (!GameDbgReadTo(&s->dbg, chunk, addr + off, r.readPerRow))
            return false;
        const u64 t1 = _nowNs();
        *readNs += t1 - t0;
        *bytes += r.readPerRow;
        for (u32 k = 0; k < 8; k++) {
            const u64 so = _sampleOff(&r, k);
            if (k % r.rows == br && so + 64 <= s->surf.buf_size) {
                memcpy(ts->b[k], chunk + (so - off), 64);
                ts->valid[k] = true;
            }
        }
        const u32 first = (u32)br * lines, end = first + lines < VIC_OUT_H ? first + lines : VIC_OUT_H;
#if NATIVE_DEBUG
        const u64 r0 = _runNs();
#endif
        if (first < VIC_OUT_H && !CpuConvertRows(chunk, off, src, first, end, luma, chroma, true))
            return false;
        const u64 t2 = _nowNs();
        *cpuNs += t2 - t1;
        sp->conv += t2 - t1;
        _encCollect(s, false);
#if NATIVE_DEBUG
        sp->run += _runNs() - r0;
#endif
        if (first < VIC_OUT_H && end > first) {  // the piece the diagnostic converts again
            s->hotOff = off;
            s->hotFirst = first;
            s->hotEnd = end;
            s->hotSrc = *src;
            s->hotOk = true;
        }
        _probePoll(s);
    }
    const u64 t2 = _nowNs();
    armDCacheFlush(luma, lumaBytes);  // NVENC reads it from memory
    armDCacheFlush(chroma, chromaBytes);
    *cpuNs += _nowNs() - t2;
    sp->flush += _nowNs() - t2;
    return true;
}

static bool _changedSinceSamples(Session* s, u64 addr, const VicSource* src, const TornSamples* ts)
{
    const Region r = _region(src);
    for (u32 k = 0; k < 8 && r.rows; k++) {
        if (!ts->valid[k])
            continue;
        u8 alignas(64) b[64];
        if (!GameDbgRead(&s->dbg, b, addr + _sampleOff(&r, k), sizeof(b)))
            return false;
        if (memcmp(b, ts->b[k], sizeof(b)) != 0)
            return true;
    }
    return false;
}

// The job in flight: collect it (a VIC job), encode its picture, send it.
static void _finish(Session* s)
{
    if (!s->pending)
        return;
    s->pending = false;
    u64 vicNs = 0, waitNs = 0;
    if (!s->jobCpu && !VicEngineJobWait(&vicNs, &waitNs)) {
        s->convFails++;
        return;
    }
    s->convs++;
    s->ivConvs++;
    s->pictBad = false;
    s->lastSrc = s->jobSrc;
    s->haveLast = true;
    if (!s->jobCpu) {
        s->ivVicJobs++;
        s->ivVicNs += vicNs;
        s->ivVicWaitNs += waitNs;
    }
    if (!s->jobCpu && s->jobTimed)
        _ema(&s->eVic, vicNs, 3);
    if (!s->jobCpu && s->pipe) {  // what a pipelined frame costs the loop: read, the wait for the VIC
        s->jobPipeNs += waitNs;
        _ema(&s->ePipe, s->jobPipeNs, 3);
    }
    _encCollect(s, true);  // the previous frame's encode (normally done long ago): its picture is free again
    if (!NvencSubmit(s->needKey)) {
        s->encFails++;
        s->needKey = true;
        return;
    }
    s->needKey = false;  // set again if the encode fails
    s->encPending = true;
    s->encJob = s->job;
    if (!s->pipe && s->jobStartNs) {  // pipelined frames overlap: not one frame's work
        s->ivWorkNs += _nowNs() - s->jobStartNs;
        s->ivWorkN++;
    }
}

// One present: copy it into the slot copy the VIC is not reading, then make
// the encoder's picture (16-row block-linear) from it - a VIC job, or the CPU
// (MODE_CPU). The job in flight, if any, is collected (encoded, sent) after
// this copy - the copy overlapped its conversion. Pipelined, this job is left
// in flight; otherwise collected at once. The fence was waited on. Returns
// whether a job is left in flight.
static bool _convert(Session* s, const UvcTapPresent* p)
{
    if (!NvencActive())
        return false;
    s->jobStartNs = s->pending ? 0 : _nowNs();  // (pipelined: the job in flight is older)
    if (s->pending && !VicEngineSlotCopy(s->jobCopy ^ 1))
        _finish(s);
    if (s->pending)
        s->ivOverlapped++;
    VicSource src;
    _source(s, p, &src);
    src.copy = s->pending ? s->jobCopy ^ 1 : 0;
    const bool cpu = s->mode == MODE_CPU && CpuCanConvert(&src) && _region(&src).readPerRow <= VicEngineBounceSize();
    if (s->mode == MODE_CPU && !cpu)
        s->ivCpuUnsup++;
    const u64 addr = _slotAddr(s, p->slot);
    if (cpu) {
        _finish(s);  // a VIC job still writing the picture (a mode change)
        s->pictBad = true;  // written from here on
        TornSamples ts;
        CpuSplit sp = { 0, 0, 0 };
        u64 bytes = 0, readNs = 0, cpuNs = 0;
        bool ok = _readConvert(s, addr, &src, &ts, &bytes, &readNs, &cpuNs, &sp);
        if (!ok) {
            s->convFails++;
            return false;
        }
        if (_changedSinceSamples(s, addr, &src, &ts)) {
            s->rereads++;
            bytes = readNs = cpuNs = 0;
            sp.conv = sp.run = sp.flush = 0;
            if (!_readConvert(s, addr, &src, &ts, &bytes, &readNs, &cpuNs, &sp) || _changedSinceSamples(s, addr, &src, &ts)) {
                s->torn++;  // skip it: the P chain goes on from the last frame we encoded
                return false;
            }
        }
        s->ivCopies++;
        s->ivReadNs += readNs;
        s->ivReadBytes += bytes;
        s->ivCpuN++;
        s->ivCpuNs += cpuNs;
        if (cpuNs > s->ivCpuMax)
            s->ivCpuMax = cpuNs;
        _ema(&s->eReadFused, readNs, 3);
        _ema(&s->eCpu, cpuNs, 3);
        s->ivConvNs += sp.conv;
        s->ivConvRunNs += sp.run;
        s->ivFlushNs += sp.flush;
        s->job = *p;
        s->jobSrc = src;
        s->pending = true;
        s->jobCpu = true;
        s->jobTimed = false;
        _finish(s);
        return false;
    }
    _probeDrain(s);  // the VIC is ours again
    u64 bytes = 0;
    const u64 t0 = _nowNs();
    bool ok = _readRegion(s, addr, &src, &bytes);
    if (!ok) {
        s->convFails++;
    } else if (_changedSinceRead(s, addr, &src)) {
        s->rereads++;
        if (!_readRegion(s, addr, &src, &bytes) || _changedSinceRead(s, addr, &src)) {
            s->torn++;  // skip it: the P chain goes on from the last frame we encoded
            ok = false;
        }
    }
    const u64 t1 = _nowNs();
    _finish(s);  // the encoder's picture is free again
    if (!ok)
        return false;
    s->ivCopies++;
    s->ivReadNs += t1 - t0;
    s->ivReadBytes += bytes;
    _ema(&s->eReadCopy, t1 - t0, 3);
    s->job = *p;
    s->jobCopy = src.copy;
    s->jobSrc = src;
    VicTarget t = { 0, 0, true };
    NvencPictureTarget(&t.lumaAddr, &t.chromaAddr);
    if (!VicEngineSubmitTo(&src, &t)) {
        s->convFails++;
        return false;
    }
    s->pending = true;
    s->jobCpu = false;
    s->jobPipeNs = t1 - t0;
    s->jobTimed = !s->pipe;
    if (!s->pipe)
        _finish(s);
    return s->pending;
}

static void _setMode(Session* s, u32 mode)
{
    s->mode = mode;
    s->pipe = mode == MODE_PIPE;
}

static bool _cpuPossible(const Session* s)
{
    return s->haveLast && CpuCanConvert(&s->lastSrc) && _region(&s->lastSrc).readPerRow <= VicEngineBounceSize();
}

// Which modes this session can use: the CPU a frame it can convert (the last
// one encoded), pipelining the second slot copy - and only where the CPU
// cannot (MODE_* above)
static bool _modeAvailable(const Session* s, u32 mode)
{
    return mode == MODE_VIC || (mode == MODE_CPU && _cpuPossible(s)) ||
           (mode == MODE_PIPE && VicEngineSlotCopy(1) && !_cpuPossible(s));
}

// What a frame would cost in a mode (ns), from the steps' costs; 0 unknown.
// Through dmnt:cht the slot copy costs an extra copy of ours: the piecewise
// read was measured at ~2/3 of it (6 vs 9.5 ms).
static u64 _predict(const Session* s, u32 mode)
{
    u64 copy = s->eReadCopy, fused = s->eReadFused;
    if (!copy)
        copy = s->dbg.dmnt ? fused * 3 / 2 : fused;
    if (!fused)
        fused = s->dbg.dmnt ? copy * 2 / 3 : copy;
    if (!copy)
        return 0;
    switch (mode) {
    case MODE_VIC:
        return s->eVic ? copy + s->eVic : 0;
    case MODE_PIPE:
        if (s->mode == MODE_PIPE && s->ePipe)
            return s->ePipe;
        return s->eVic ? (copy > s->eVic ? copy : s->eVic) : 0;
    default:
        return s->eCpu ? fused + s->eCpu : 0;
    }
}

// The probes (MODE_*), while behind: a VIC job beside the CPU frames, or one
// CPU conversion of the last frame on the VIC
static void _probe(Session* s)
{
    const u64 now = _nowNs();
    if (!s->pml[0] || s->pml[0] >= MODE_BEHIND_PML || now < s->nextProbe || s->pending || s->probing)
        return;
    if (s->mode == MODE_CPU) {
        VicSource src = s->lastSrc;
        src.copy = 0;  // whatever slot copy 0 holds: the probe measures the queue, not the picture
        if (VicEngineActive() && VicEngineSubmitOwn(&src))
            s->probing = true;
        else
            s->nextProbe = now + PROBE_EVERY_NS;
        return;
    }
    s->nextProbe = now + PROBE_EVERY_NS;
    // until PROBE_KEEP fresh ones; then one as each turns stale
    if (!_cpuPossible(s) || _probesFresh(s->cpuPt, s->cpuPn) >= PROBE_KEEP)
        return;
    u8* out = VicEngineBounce();
    const u8* slot = VicEngineSlotCopy(s->lastSrc.copy);
    if (!out || !slot || VicEngineBounceSize() < VIC_OUT_BYTES)
        return;
    const u64 t0 = _nowNs();
    if (CpuConvertNv12(slot, &s->lastSrc, out, out + VIC_OUT_LUMA, false)) {
        s->eCpu = _probeAdd(s->cpuP, s->cpuPt, &s->cpuPn, _nowNs() - t0);
        s->ivProbes++;
        s->ivProbeNs += _nowNs() - t0;
    }
}

// Every 2 s while frames go to the camera: the window's share of the game's
// presents, then the check after a switch, or a switch
static void _modeControl(Session* s, const UvcTapPresent* p, bool encodeable)
{
    if (!_modeAvailable(s, s->mode))
        _setMode(s, MODE_VIC);
    const u64 now = _nowNs();
    if (!encodeable || s->sent == 0) {
        s->winStartNs = 0;
        s->pml[0] = s->pml[1] = 0;
        s->want = s->checkWin = 0;
        return;
    }
    if (s->winStartNs && now - s->winStartNs < MODE_WINDOW_NS)
        return;

    // a window ends (none at the first call)
    const bool had = s->winStartNs != 0;
    const u64 span = now - s->winStartNs, pres = p->seq - s->winSeq, conv = s->convs - s->winConvs;
    s->winStartNs = now;
    s->winSeq = p->seq;
    s->winConvs = s->convs;
    if (!had)
        return;
    // steady: the game presents fast enough, at a stable rate. A loading
    // screen or a scene change moves the share whatever the mode
    // (01003ec027cc8000: a trial during loading had chosen wrongly)
    const u32 rate = span ? (u32)(pres * 1000000000ULL / span) : 0;
    const bool steady = rate >= MODE_MIN_FPS && s->lastRate &&
                        rate * 100 >= s->lastRate * (100 - MODE_RATE_PCT) && rate * 100 <= s->lastRate * (100 + MODE_RATE_PCT);
    s->lastRate = rate;
    if (!steady) {
        s->want = 0;
        return;
    }
    const u32 pml = pres ? (u32)(conv * 1000 / pres) : 1000;
    s->pml[1] = s->pml[0];
    s->pml[0] = pml ? pml : 1;

    if (s->checkWin) {
        if (--s->checkWin)
            return;
        const u32 now2 = s->pml[1] ? (s->pml[0] + s->pml[1]) / 2 : s->pml[0];
        if (now2 * 100 < s->prevPml * (100 - MODE_CHECK_PCT)) {
            // a load spike can fail a good mode (01003ec027cc8000 with cheats:
            // 11% in a loading moment): a short block first, longer if it fails again
            u64* b = &s->blockNs[s->mode];
            *b = !*b ? MODE_BLOCK_NS : *b * 2 > MODE_BLOCK_MAX_NS ? MODE_BLOCK_MAX_NS : *b * 2;
            LOG("native: mode %s: %u%% of the game's frames, %u%% before with %s - back to it, %s not again for %lu s\n",
                kModeName[s->mode], now2 / 10, s->prevPml / 10, kModeName[s->prevMode], kModeName[s->mode],
                *b / 1000000000ULL);
            s->blockUntil[s->mode] = now + *b;
            _setMode(s, s->prevMode);
        } else {
            s->blockNs[s->mode] = 0;
            LOG("native: mode %s kept: %u%% of the game's frames (%u%% before with %s)\n",
                kModeName[s->mode], now2 / 10, s->prevPml / 10, kModeName[s->prevMode]);
        }
        s->pml[0] = s->pml[1] = 0;  // the next windows are the new mode's
        return;
    }
    if (s->pml[0] >= MODE_BEHIND_PML) {
        s->want = 0;
        return;
    }
    const u64 cur = _predict(s, s->mode);
    u32 best = s->mode;
    u64 bestNs = cur;
    for (u32 m = 0; m < MODES; m++) {
        const u64 ns = _predict(s, m);
        if (m != s->mode && ns && _modeAvailable(s, m) && now >= s->blockUntil[m] && (!bestNs || ns < bestNs)) {
            best = m;
            bestNs = ns;
        }
    }
    if (best == s->mode || !cur || bestNs * (100 + MODE_GAIN_PCT) > cur * 100) {
        s->want = 0;
        return;
    }
    s->want = s->wantMode == best ? s->want + 1 : 1;
    s->wantMode = best;
    if (s->want < 2)
        return;
    LOG("native: mode %s -> %s: predicted %lu.%lu -> %lu.%lu ms a frame (%u%% of the game's frames now)\n",
        kModeName[s->mode], kModeName[best], cur / 1000000, cur / 100000 % 10, bestNs / 1000000, bestNs / 100000 % 10,
        s->pml[1] ? (s->pml[0] + s->pml[1]) / 20 : s->pml[0] / 10);
    s->prevMode = s->mode;
    s->prevPml = s->pml[1] ? (s->pml[0] + s->pml[1]) / 2 : s->pml[0];
    s->want = 0;
    s->checkWin = 3;  // the first window is the switch's own: then two of the new mode
    s->pml[0] = s->pml[1] = 0;
    _setMode(s, best);
}

static void _endSession(Session* s, const char* why)
{
    if (!s->attached)
        return;
    DebugPumpStats ps;
    DebugPumpGetStats(&ps);
    LOG("native: session end (%s) after %lus: %u reads (%u failed), %u dumps; "
        "pump: %u events (+%u/-%u threads, %u faults left to the game), %u continues "
        "(cores %u/%u/%u/%u), busy %u, failed %u (0x%x), longest hold %lu us\n",
        why, (_nowNs() - s->attachTick) / 1000000000ULL, s->reads, s->readFails, s->dumps,
        ps.events, ps.created, ps.exited, ps.faults, ps.continues,
        ps.per_core[0], ps.per_core[1], ps.per_core[2], ps.per_core[3],
        ps.busy, ps.failed, ps.last_fail_rc, ps.max_hold_ns / 1000);
    VicEngineStats vs;
    VicEngineGetStats(&vs, false);
    LOG("native: VIC: %u frames converted (%u failed), %u timeouts, %u refused; fence timeouts %u%s\n",
        s->convs, s->convFails, vs.timeouts, vs.refused, s->fenceTimeouts, VicEngineWedged() ? " - ENGINE WEDGED" : "");
    if (s->h264Open) {
        fsFileFlush(&s->h264);
        fsFileClose(&s->h264);
        LOG("native: H.264 dump closed early: %u frames, %lu KB\n", s->h264Frames, s->h264Off / 1024);
    }
    if (NvencActive() || NvencWedged()) {
        NvencStats es;
        NvencGetStats(&es, false);
        LOG("native: NVENC: %u encode failures, %u timeouts%s\n", s->encFails, es.timeouts, NvencWedged() ? " - ENGINE WEDGED" : "");
    }
    _releaseMemory(s);
    GameDbgDetach(&s->dbg);
    memset(s, 0, sizeof(*s));
}

static bool _startSession(Session* s, const UvcTapPresent* p, const UvcTapSurface* sf)
{
    memset(s, 0, sizeof(*s));
    s->surf = *sf;
    for (u32 k = 1; k < sf->num_slots && k < 8; k++)
        s->separate |= sf->slot_nvmap[k] != sf->slot_nvmap[0];
    s->span = sf->buf_size;
    for (u32 k = 0; k < sf->num_slots && k < 8; k++) {
        if ((u64)sf->slot_offset[k] + sf->buf_size > s->span)
            s->span = (u64)sf->slot_offset[k] + sf->buf_size;
    }

    LOG("native: game %016lx pid %lu: %ux%u stride %u px, block 2^%u, layout %u, kind 0x%x, format %#lx, "
        "%u slot(s) of %u B, %s\n",
        p->program_id, p->process_id, sf->width, sf->height, sf->stride_px, sf->block_h_log2, sf->layout,
        sf->kind, sf->color_format, sf->num_slots, sf->buf_size,
        s->separate ? "one object per slot" : "one object for all slots");
    if (sf->layout != 3 || sf->num_slots < 1 || sf->num_slots > 8 || sf->buf_size == 0 ||
        sf->buf_size > MAX_BUF_BYTES) {
        LOG("native: swapchain not supported (needs block-linear, 1-8 slots, <= 16 MB a buffer)\n");
        return false;
    }

    Result directRc = 0;
    if (!GameDbgAttach(p->process_id, &s->dbg, &directRc)) {
        LOG("native: no attach (direct 0x%x, dmnt does not hold the game)\n", directRc);
        return false;
    }
    s->attached = true;
    s->attachTick = _nowNs();
    u32 nev = 0;
    Result rrc = GameDbgResume(&s->dbg, &nev);
    if (!s->dbg.dmnt)
        DebugPumpStart(s->dbg.handle);
    LOG("native: attached %s (direct 0x%x%s), resume: %u events, continue 0x%x\n",
        s->dbg.dmnt ? "through dmnt:cht - the cheat engine holds the game" : "directly", directRc,
        directRc == MAKERESULT(Module_Kernel, 122) ? ": busy, another debugger" : "", nev, rrc);
    if (s->dbg.dmnt) {
        const s64 cheats = GameDbgDmntCheatCount(&s->dbg);
        LOG("native: dmnt has %ld cheat(s) loaded for this game%s\n", cheats,
            cheats == 0 ? " - it was force-opened (a cheat overlay or tool), not attached for a cheat file" : "");
    }

    u64 base = 0;
    if (!s->separate && _findExact(s, &base)) {
        for (u32 k = 0; k < sf->num_slots; k++) {
            s->cand[k] = base + sf->slot_offset[k];
            s->slotCand[k] = (u8)k;
        }
        s->ncand = sf->num_slots;
        s->locked = true;
        s->how = "exact region";
        _logMatch(s, "from the region of the swapchain's exact size");
    } else {
        s->ncand = _findCandidates(s);
        if (s->ncand < sf->num_slots) {
            LOG("native: only %u candidate buffer(s) for %u slot(s) - giving up on this game\n",
                s->ncand, sf->num_slots);
            return true;  // stay attached: still useful for the kill test
        }
        for (u32 k = 0; k < sf->num_slots; k++)
            s->slotCand[k] = (u8)k;
        SlotMatcherReset(&s->matcher, sf->num_slots, s->ncand);
        s->how = "matched";
        // One object for every slot, and one region that can hold it: its
        // candidates ARE the slots, at the offsets the game declared - nothing
        // to match. (0100ba4026e94000: the region is larger than the set, and
        // matching through dmnt:cht never converged.)
        if (!s->separate && s->ncand == sf->num_slots) {
            s->locked = true;
            s->how = "single region";
            _logMatch(s, "from the only region that holds the set");
        }
        LOG("native: %u candidate buffer(s) from 0x%010lx - matching them to slots\n", s->ncand, s->cand[0]);
        if (sf->num_slots == 1) {
            s->locked = s->ncand == 1;
            if (s->locked)
                _logMatch(s, "single buffer");
        }
    }
    s->nextRead = _nowNs() + READ_EVERY_NS;
    _takeMemory(s);
    return true;
}

static void _calibrate(Session* s, const UvcTapPresent* p)
{
    if (s->locked || s->ncand < s->surf.num_slots || s->surf.num_slots < 2)
        return;
    u32 sig[SLOTMATCH_MAX_CAND];
    bool varied[SLOTMATCH_MAX_CAND];
    const u64 t0 = _nowNs();
    for (u32 c = 0; c < s->ncand; c++)
        sig[c] = _sampleSig(s, s->cand[c], &varied[c]);
    const u64 dt = _nowNs() - t0;
    if (dt > s->sampleNsMax)
        s->sampleNsMax = dt;

    SlotMatcherAdd(&s->matcher, (u32)p->seq, (u32)p->slot, sig);
    uint8_t m[SLOTMATCH_MAX_SLOTS];
    int32_t weakest = 0;
    if (SlotMatcherMatch(&s->matcher, m, &weakest)) {
        memcpy(s->slotCand, m, s->surf.num_slots);
        s->locked = true;
        char what[64];
        snprintf(what, sizeof(what), "matched after %u intervals (weakest %d/1000)", s->matcher.intervals, weakest);
        _logMatch(s, what);
    } else if (SlotMatcherGaveUp(&s->matcher)) {
        s->locked = true;
        s->how = "first guess";
        LOG("native: slots not told apart in %u samples (%u with a change) - keeping the first guess\n",
            s->matcher.samples, s->matcher.intervals);
    }
}

#if NATIVE_DEBUG
// Whole slot in CHUNK pieces, discarded: throughput, and reads in flight
// for the kill test.
static void _throughputRead(Session* s, s32 slot)
{
    const u64 addr = _slotAddr(s, slot);
    const u64 t0 = _nowNs();
    bool ok = true;
    for (u64 off = 0; off < s->surf.buf_size && ok; off += CHUNK) {
        const u64 n = s->surf.buf_size - off < CHUNK ? s->surf.buf_size - off : CHUNK;
        ok = GameDbgRead(&s->dbg, g_chunk, addr + off, n);
    }
    const u64 dt = _nowNs() - t0;
    if (!ok) {
        s->readFails++;
        return;
    }
    s->reads++;
    s->ivReads++;
    s->ivNsSum += dt;
    if (dt > s->ivNsMax)
        s->ivNsMax = dt;
}

// The slot on screen, read and written to the SD card piece by piece: no
// buffer of a frame's size (a 1080p buffer is 8.4 MB, about what the whole
// System group has free). Taken while the game is suspended (HOME), so the
// buffer holds still; the .txt says whether a present came during the dump
// anyway (then the picture may mix two frames).
static void _dump(Session* s, const UvcTapPresent* p)
{
    const u32 n = s->dumps++;
    const s32 slot = p->slot;
    const u64 addr = _slotAddr(s, slot);
    char path[128], info[512];

    if (!_sdOpen()) {
        LOG("native: dump %u: SD card not available\n", n);
        return;
    }
    snprintf(path, sizeof(path), DUMP_DIR "/%016lx-%u.bin", p->program_id, n);
    fsFsDeleteFile(&g_sd, path);
    FsFile f;
    if (R_FAILED(fsFsCreateFile(&g_sd, path, s->surf.buf_size, 0)) ||
        R_FAILED(fsFsOpenFile(&g_sd, path, FsOpenMode_Write, &f))) {
        LOG("native: dump %u: cannot create %s\n", n, path);
        return;
    }
    const u64 t0 = _nowNs();
    bool ok = true;
    for (u64 off = 0; off < s->surf.buf_size && ok; off += CHUNK) {
        const u64 len = s->surf.buf_size - off < CHUNK ? s->surf.buf_size - off : CHUNK;
        ok = GameDbgRead(&s->dbg, g_chunk, addr + off, len) &&
             R_SUCCEEDED(fsFileWrite(&f, off, g_chunk, len, FsWriteOption_None));
    }
    fsFileFlush(&f);
    fsFileClose(&f);
    const u64 dt = _nowNs() - t0;

    UvcTapPresent after;
    const bool still = R_SUCCEEDED(TapClientGetLatestPresent(&after)) && after.seq == p->seq;
    const int len = snprintf(info, sizeof(info),
        "program=%016lx\nwidth=%u\nheight=%u\nstride_px=%u\nblock_h_log2=%u\nkind=0x%x\nformat=%#lx\n"
        "layout=%u\nbuf_size=%u\nslot=%d\naddress=0x%lx\nfound=%s\nvia=%s\ndump_ms=%lu\n"
        "presents_during_dump=%lu\ncrop=%d,%d,%d,%d\ntransform=0x%x\n",
        p->program_id, s->surf.width, s->surf.height, s->surf.stride_px, s->surf.block_h_log2,
        s->surf.kind, s->surf.color_format, s->surf.layout, s->surf.buf_size, slot, addr, s->how,
        s->dbg.dmnt ? "dmnt:cht" : "direct", dt / 1000000, still ? 0 : after.seq - p->seq,
        p->crop[0], p->crop[1], p->crop[2], p->crop[3], p->transform);
    snprintf(path, sizeof(path), DUMP_DIR "/%016lx-%u.txt", p->program_id, n);
    const bool saved = ok && _sdWrite(path, info, len);
    LOG("native: dump %u: slot %d at 0x%lx, %u B in %lu ms%s -> %s\n", n, slot, addr, s->surf.buf_size,
        dt / 1000000, still ? "" : " (the game presented during it: may mix frames)", saved ? "saved" : "FAILED");
}

// The frame on screen through the VIC, saved as NV12 1280x720 (+ .txt). The
// output is pinned for the engine, so it goes to the SD through g_chunk.
static void _dumpNv12(Session* s, const UvcTapPresent* p)
{
    const u32 n = s->dumps++;
    if (!_sdOpen()) {
        LOG("native: dump %u: SD card not available\n", n);
        return;
    }
    _finish(s);  // the VIC is ours, and so is slot copy 0
    _probeDrain(s);
    VicSource src;
    _source(s, p, &src);
    u64 bytes = 0, vicNs = 0;
    const u64 t0 = _nowNs();
    const bool ok = _readRegion(s, _slotAddr(s, p->slot), &src, &bytes) && VicEngineConvert(&src, &vicNs);
    const u64 readNs = _nowNs() - t0 - vicNs;
    if (!ok) {
        LOG("native: dump %u: conversion failed\n", n);
        return;
    }
    char path[128], info[512];
    snprintf(path, sizeof(path), DUMP_DIR "/%016lx-%u.nv12", p->program_id, n);
    fsFsDeleteFile(&g_sd, path);
    FsFile f;
    bool saved = R_SUCCEEDED(fsFsCreateFile(&g_sd, path, VIC_OUT_BYTES, 0)) &&
                 R_SUCCEEDED(fsFsOpenFile(&g_sd, path, FsOpenMode_Write, &f));
    if (saved) {
        const u8* out = VicEngineOutput();
        for (u32 off = 0; off < VIC_OUT_BYTES && saved; off += CHUNK) {
            const u32 len = VIC_OUT_BYTES - off < CHUNK ? VIC_OUT_BYTES - off : CHUNK;
            memcpy(g_chunk, out + off, len);
            saved = R_SUCCEEDED(fsFileWrite(&f, off, g_chunk, len, FsWriteOption_None));
        }
        fsFileFlush(&f);
        fsFileClose(&f);
    }
    UvcTapPresent after;
    const bool still = R_SUCCEEDED(TapClientGetLatestPresent(&after)) && after.seq == p->seq;
    const int len = snprintf(info, sizeof(info),
        "program=%016lx\nformat=nv12\nwidth=%u\nheight=%u\nsource=%ux%u stride %u px, block 2^%u, vic pixfmt %u\n"
        "rect=%ux%u\nflip=%u\nslot=%d\nvia=%s\nread_bytes=%lu\nread_us=%lu\nvic_us=%lu\npresents_during_dump=%lu\n",
        p->program_id, VIC_OUT_W, VIC_OUT_H, src.w, src.h, src.stride_px, src.blk_h_log2, src.pixfmt,
        src.rect_w, src.rect_h, src.flip, p->slot, s->dbg.dmnt ? "dmnt:cht" : "direct", bytes, readNs / 1000,
        vicNs / 1000, still ? 0 : after.seq - p->seq);
    snprintf(path, sizeof(path), DUMP_DIR "/%016lx-%u.txt", p->program_id, n);
    saved = saved && _sdWrite(path, info, len);
    LOG("native: dump %u: NV12 %ux%u from a %ux%u rect (flip %u), read %lu KB in %lu us, VIC %lu us -> %s\n",
        n, VIC_OUT_W, VIC_OUT_H, src.rect_w, src.rect_h, src.flip, bytes / 1024, readNs / 1000, vicNs / 1000,
        saved ? "saved" : "FAILED");
}
#endif

#if NATIVE_DEBUG
// The last piece of the last CPU frame, still in the bounce buffer, converted
// again: tiled and in row order into the slot copy (unused in CPU mode), and
// only read. Every output is written cold, as the encoder's picture is (it is
// flushed every frame).
static void _hotRerun(Session* s)
{
    const u8* chunk = VicEngineBounce();
    u8* scratch = VicEngineSlotCopy(0);
    const Region r = _region(&s->hotSrc);
    if (!s->hotOk || !chunk || !scratch || r.readPerRow > VicEngineBounceSize() ||
        (u64)s->surf.buf_size < 0x400000 + VIC_OUT_W * 368)
        return;
    u8* y = scratch;
    u8* uv = scratch + 0x400000;  // past any luma offset, tiled or not
    u64 t0 = _nowNs();
    u64 sum = 0;
    const u64* w = (const u64*)chunk;
    for (u64 i = 0; i < r.readPerRow / 8; i += 8)  // one load a cache line... and the rest of the line
        sum += w[i] ^ w[i + 1] ^ w[i + 2] ^ w[i + 3] ^ w[i + 4] ^ w[i + 5] ^ w[i + 6] ^ w[i + 7];
    const u64 t1 = _nowNs();
    const bool ok = CpuConvertRows(chunk, s->hotOff, &s->hotSrc, s->hotFirst, s->hotEnd, y, uv, true);
    const u64 t2 = _nowNs();
    const bool ok2 = CpuConvertRows(chunk, s->hotOff, &s->hotSrc, s->hotFirst, s->hotEnd, y, uv, false);
    const u64 t3 = _nowNs();
    armDCacheFlush(scratch, 0x400000 + VIC_OUT_W * 368);  // what the VIC probe may read: no dirty line left
    if (!ok || !ok2 || sum == 0x5A5A5A5A5A5A5A5AULL)  // (sum: keep the loop)
        return;
    s->hotRead += t1 - t0;
    s->hotBl += t2 - t1;
    s->hotPitch += t3 - t2;
    s->hotLines = s->hotEnd - s->hotFirst;
    s->hotN++;
}
#endif

static void _logPeriodic(Session* s, const UvcTapPresent* p)
{
    DebugPumpStats ps;
    DebugPumpGetStats(&ps);
    const u64 avg = s->ivReads ? s->ivNsSum / s->ivReads : 0;
    const u64 mbps = avg ? (u64)s->surf.buf_size * 1000 / avg : 0;  // bytes/ns * 1000 = MB/s
    LOG("native: [%lus] seq %lu slot %d | %s%s via %s | last 10 s: %u reads, avg %lu us, max %lu us, ~%lu MB/s | "
        "total %u reads, %u failed, %u dumps | sampling max %lu us | pump %u events (+%u/-%u threads), "
        "continues %u, longest hold %lu us%s\n",
        _nowNs() / 1000000000ULL, p->seq, p->slot,
        s->locked ? "slots known: " : "matching", s->locked ? s->how : "", s->dbg.dmnt ? "dmnt:cht" : "direct",
        s->ivReads, avg / 1000, s->ivNsMax / 1000, mbps, s->reads, s->readFails, s->dumps,
        s->sampleNsMax / 1000, ps.events, ps.created, ps.exited, ps.continues, ps.max_hold_ns / 1000,
        ps.gone ? ", GAME GONE" : "");
    s->ivReads = 0;
    s->ivNsSum = s->ivNsMax = 0;
    if (VicEngineActive() || VicEngineWedged()) {
        VicEngineStats vs;
        VicEngineGetStats(&vs, true);
        const u64 n = s->ivVicJobs ? s->ivVicJobs : 1, nc = s->ivCopies ? s->ivCopies : 1;
        LOG("native: presents last 10 s: %u already finished, %u waited for, %u newer skipped (vi-tap protocol %u); "
            "pipelined jobs: %u overlapped by the next copy, %u not\n",
            s->ivPickReady, s->ivPickWaited, s->ivSkipped, TapClientVersion(), s->ivOverlapped, s->ivAlone);
        LOG("native: VIC last 10 s: %u frames (%lu.%lu fps), per frame avg: fence wait %lu us (max %lu), read %lu KB in %lu us, "
            "VIC %u jobs avg %lu us (max %lu, waited for %lu), CPU %u frames avg %lu us (max %lu)%s | mode %s%s | "
            "total %u, failed %u, fence timeouts %u%s\n",
            s->ivConvs, (u64)s->ivConvs / 10, (u64)s->ivConvs % 10, s->ivFenceNs / (s->ivFences ? s->ivFences : 1) / 1000, s->ivFenceMax / 1000,
            s->ivReadBytes / nc / 1024, s->ivReadNs / nc / 1000, s->ivVicJobs, s->ivVicNs / n / 1000, vs.nsMax / 1000,
            s->ivVicWaitNs / n / 1000, s->ivCpuN, s->ivCpuN ? s->ivCpuNs / s->ivCpuN / 1000 : 0, s->ivCpuMax / 1000,
            s->ivCpuUnsup ? " (some frames not convertible: on the VIC)" : "",
            kModeName[s->mode], s->checkWin ? " (just switched: checking)" : "",
            s->convs, s->convFails, s->fenceTimeouts, VicEngineWedged() ? " - ENGINE WEDGED" : "");
#if NATIVE_DEBUG
        if (s->ivCpuN || s->hotN) {
            const u64 n = s->ivCpuN ? s->ivCpuN : 1, h = s->hotN ? s->hotN : 1, l = s->hotLines ? s->hotLines : 1;
            LOG("native: CPU route per frame: convert %lu us (the thread ran %lu us of it), flush %lu us | "
                "last piece again, in the cache (%u lines, %u times): tiled %lu us, row order %lu us, read only %lu us "
                "-> a whole frame at that speed: tiled %lu us, row order %lu us | CPU clock %u MHz, tiled lines zeroed first: %s\n",
                s->ivConvNs / n / 1000, s->ivConvRunNs / n / 1000, s->ivFlushNs / n / 1000,
                s->hotLines, s->hotN, s->hotBl / h / 1000, s->hotPitch / h / 1000, s->hotRead / h / 1000,
                s->hotBl / h * VIC_OUT_H / l / 1000, s->hotPitch / h * VIC_OUT_H / l / 1000,
                NvencPcvClockHz(0x40000001u) / 1000000, CpuUsesZva() ? "yes" : "no");
        }
        s->ivConvNs = s->ivConvRunNs = s->ivFlushNs = 0;
        s->hotBl = s->hotPitch = s->hotRead = 0;
        s->hotN = 0;
#endif
        s->ivVicJobs = s->ivCpuN = s->ivCpuUnsup = 0;
        s->ivCpuNs = s->ivCpuMax = 0;
        if (NvencActive() || NvencWedged()) {
            NvencStats es;
            NvencGetStats(&es, true);
            const u64 ne = es.frames ? es.frames : 1;
            const u64 bits = (es.bytesI + es.bytesP) * 8;
            const u64 ns = s->ivSent ? s->ivSent : 1, nw = s->ivWorkN ? s->ivWorkN : 1;
            LOG("native: USB last 10 s: %u frames sent (%lu KB), %u send failures in total, %u sent in total; "
                "copies re-read %u, torn and skipped %u; keep-alive frames %u | send avg %lu us (max %lu), "
                "%lu.%lu pieces of 16 KB a frame | a frame's work, read to handed over: avg %lu us, waited for the send thread avg %lu us | "
                "game present to last byte out: avg %lu us (max %lu)\n",
                s->ivSent, s->ivSentBytes / 1024, s->sendFails, s->sent, s->rereads, s->torn, g_fillers,
                g_send.n ? g_send.ns / g_send.n / 1000 : 0, g_send.nsMax / 1000, (u64)s->ivSendPieces / ns,
                (u64)s->ivSendPieces * 10 / ns % 10, s->ivWorkNs / nw / 1000, g_send.waitNs / ns / 1000,
                g_send.latN ? g_send.latNs / g_send.latN / 1000 : 0, g_send.latMax / 1000);
            mutexLock(&g_send.lock);
            g_send.ns = g_send.nsMax = g_send.waitNs = 0;
            g_send.n = 0;
            g_send.latNs = g_send.latMax = 0;
            g_send.latN = 0;
            mutexUnlock(&g_send.lock);
            s->ivSent = 0;
            s->ivSentBytes = 0;
            s->ivWorkNs = 0;
            s->ivSendPieces = s->ivWorkN = 0;
            LOG("native: NVENC last 10 s: %u frames (%u.%u fps), %u keyframes, encode avg %lu us (max %lu), "
                "%lu Mbps (I avg %lu KB, P avg %lu KB), errors %u, clock %u MHz%s\n",
                es.frames, es.frames / 10, es.frames % 10, es.idrs, es.nsTotal / ne / 1000, es.nsMax / 1000,
                bits / 10 / 1000000, es.idrs ? es.bytesI / es.idrs / 1024 : 0,
                es.frames > es.idrs ? es.bytesP / (es.frames - es.idrs) / 1024 : 0, es.errors,
                es.clockHz / 1000000, NvencWedged() ? " - ENGINE WEDGED" : "");
        }
        char pred[160];
        u32 pl = 0;
        for (u32 m = 0; m < MODES; m++) {
            const u64 ns = _modeAvailable(s, m) ? _predict(s, m) : 0;
            if (ns)
                pl += snprintf(pred + pl, sizeof(pred) - pl, "%s%s %lu.%lu", pl ? ", " : "", kModeName[m],
                               ns / 1000000, ns / 100000 % 10);
        }
        if (!pl)
            snprintf(pred, sizeof(pred), "not known yet");
        LOG("native: mode %s, predicted ms a frame: %s | last share of the game's frames %u%% | probes last 10 s: %u, avg %lu us\n",
            kModeName[s->mode], pred, s->pml[0] / 10, s->ivProbes, s->ivProbes ? s->ivProbeNs / s->ivProbes / 1000 : 0);
        s->ivProbes = 0;
        s->ivProbeNs = 0;
        s->ivConvs = 0;
        s->ivReadNs = s->ivVicNs = s->ivFenceNs = s->ivFenceMax = s->ivReadBytes = 0;
        s->ivFences = s->ivCopies = 0;
        s->ivVicWaitNs = 0;
        s->ivOverlapped = s->ivAlone = 0;
        s->ivPickReady = s->ivPickWaited = s->ivSkipped = 0;
        s->ivEncNs = 0;
    }
}

static void _filler(Session* s)
{
    if (!UvcDeviceIsStreaming() || _nowNs() - g_lastSentNs < FILLER_NS || s->pending || s->encPending)
        return;  // (a job or an encode in flight: a frame is coming)
    const u8* frame = NULL;
    u32 len = 0;
    bool idr = false;
    if (s->pictBad)
        return;  // a frame the game drew over: the next one comes, do not show this one
    if (s->attached && s->locked && NvencActive() && s->convs > 0 && !s->needKey) {
        // the encoder's picture still holds the last frame: the same again
        if (!NvencEncodeAgain(&frame, &len, &idr, NULL))
            return;
    } else if (g_blackLen) {
        frame = g_black;
        len = g_blackLen;
        idr = true;
        // the decoder's reference is black now: real frames restart from a
        // keyframe
        s->needKey = true;
    } else {
        return;
    }
    g_lastSentNs = _nowNs();  // even if it fails: a stalled host is not retried at once
    if (_send(frame, len, g_lastSentNs / 1000000, idr, 0))
        g_fillers++;
    else
        s->needKey = true;
}

void NativeCaptureThread(void* arg)
{
    (void)arg;
    LOG("native: capture started (native H.264 720p to the camera while an app streams)\n");
    _sendStart();
    DonorCleanupStale();
    Session* s = &g_s;
    memset(s, 0, sizeof(*s));
    u64 lastLog = _nowNs();
    u64 failedPid = 0, failedGen = 0;

    while (g_nativeCaptureRunning) {
        if (!TapClientIsConnected()) {
            if (R_FAILED(TapClientConnect())) {
                svcSleepThread(2000000000ULL);
                continue;
            }
            LOG("native: connected to uvctap\n");
        }

        Event* ev = TapClientPresentEvent();
        // while an app streams, wake at least as often as the keep-alive
        // needs; a job in flight or a present left for after it: no waiting
        const bool again = s->again;
        s->again = false;
        // (an encode in flight: back within 1 ms to hand its frame over)
        bool woke = R_SUCCEEDED(eventWait(ev, s->pending || again ? 0 : s->encPending ? 1000000ULL :
                                              UvcDeviceIsStreaming() ? FILLER_NS / 2 : 1000000000ULL));
        if (woke)
            eventClear(ev);

        UvcTapPresent p;
        UvcTapStats st;
        if (R_FAILED(TapClientGetLatestPresent(&p)) || R_FAILED(TapClientGetStats(&st))) {
            LOG("native: uvctap gone - reconnecting\n");
            _endSession(s, "uvctap gone");
            TapClientClose();
            continue;
        }

        if (st.app_pid != g_appPid) {
            LOG("native: [%lus] application pid %lu -> %lu\n", _nowNs() / 1000000000ULL, g_appPid, st.app_pid);
            g_appPid = st.app_pid;
            g_appSinceNs = st.app_pid ? _nowNs() : 0;
            g_retryAtNs = g_retryDelayNs = 0;  // a new game: no back-off
        }

        // the game went away or another one runs
        if (s->attached && (st.app_pid != s->dbg.pid || (!s->dbg.dmnt && DebugPumpGone()))) {
            const bool quick = _nowNs() - s->attachTick < 2000000000ULL;
            _endSession(s, st.app_pid != s->dbg.pid ? "application changed" : "game exited");
            if (quick && st.app_pid != 0) {
                g_retryDelayNs = g_retryDelayNs ? (g_retryDelayNs * 2 > 30000000000ULL ? 30000000000ULL : g_retryDelayNs * 2)
                                                : 2000000000ULL;
                g_retryAtNs = _nowNs() + g_retryDelayNs;
                LOG("native: that session lasted under 2 s - next attempt in %lu s\n", g_retryDelayNs / 1000000000ULL);
            }
        }


        // The game made new frame buffers (a boot or loading set replaced,
        // Minecraft loading a world - SFT v0.7.1): the addresses we read are
        // gone, and reading them gives garbage. Start over on the new set:
        // the next present starts a new session (and a keyframe).
        if (s->attached && _nowNs() >= s->nextSurfaceCheck) {
            s->nextSurfaceCheck = _nowNs() + 250000000ULL;
            UvcTapSurface sf;
            if (R_SUCCEEDED(TapClientGetSurface(&sf)) && sf.process_id == s->dbg.pid &&
                (sf.generation != s->surf.generation || sf.num_slots != s->surf.num_slots)) {
                LOG("native: the game made new frame buffers (set %u -> %u, %u -> %u slots)\n",
                    s->surf.generation, sf.generation, s->surf.num_slots, sf.num_slots);
                _endSession(s, "new frame buffers");
            }
        }

        const bool presenting = p.seq != 0 && p.process_id == st.app_pid && st.app_pid != 0 &&
                                armTicksToNs(armGetSystemTick() - p.tick) < 500000000ULL;
        if (!s->attached && presenting && _nowNs() >= g_retryAtNs) {
            UvcTapSurface sf;
            if (R_FAILED(TapClientGetSurface(&sf)) || sf.process_id != p.process_id) {
                if (failedPid != p.process_id) {
                    LOG("native: no swapchain geometry for pid %lu - was the game started before vi-tap v2? relaunch it\n",
                        p.process_id);
                    failedPid = p.process_id;
                    failedGen = ~0ULL;
                }
            } else if (!(failedPid == p.process_id && failedGen == sf.generation)) {
                if (!_startSession(s, &p, &sf)) {
                    _endSession(s, "setup failed");
                    failedPid = p.process_id;
                    failedGen = sf.generation;
                }
            }
        }

        if (s->attached) {
            _probePoll(s);
            _encCollect(s, false);
            _modeControl(s, &p, s->locked && NvencActive() && UvcDeviceIsStreaming());
        }

        bool inFlight = false;  // a job submitted in this pass, its collection overlapping the next copy
        if (s->attached && ((woke && p.seq != s->lastSeq) || again)) {
            const bool streaming = UvcDeviceIsStreaming();
            const bool encodeable = s->locked && NvencActive() && streaming;
            if (!s->locked) {
                // matching the buffers: sample the latest present, finished
                if (VicEngineActive()) {
                    u64 waited = 0;
                    if (!VicEngineWaitFence(p.fence_count, p.fence, FENCE_TIMEOUT_NS, &waited))
                        s->fenceTimeouts++;
                } else {
                    const u64 due = armTicksToNs(p.tick) + SAMPLE_DELAY_NS;
                    const u64 now = _nowNs();
                    if (due > now)
                        svcSleepThread(due - now);
                }
                _calibrate(s, &p);
            }
            s->lastSeq = p.seq;

            if (s->locked && NvencActive()) {
                if (streaming && !s->wasStreaming) {
                    s->needKey = true;  // a new stream starts on a keyframe
                    LOG("native: a camera app is streaming - native H.264 720p from now on%s\n",
                        s->dbg.dmnt ? " (read through dmnt:cht: slower copies)" : "");
                } else if (!streaming && s->wasStreaming) {
                    LOG("native: the camera app stopped streaming\n");
                }
                s->wasStreaming = streaming;
                // an app reopened the camera while it was already streaming
                // (ffplay relaunched): it decodes nothing until a keyframe
                const u32 commits = UvcDeviceCommitCount();
                if (commits != s->commits) {
                    s->commits = commits;
                    s->needKey = true;
                }
            }
            // As fast as the copies allow, on either route: through dmnt:cht
            // a copy is slower (7-27 ms for 3.75 MB, direct 2.2 ms), and one
            // the game drew over is re-read or skipped (_changedSinceRead)
            UvcTapPresent pick;
            if (encodeable && _pickPresent(s, &p, &pick, s->pending)) {
                s->lastEncSeq = pick.seq;
                inFlight = _convert(s, &pick);
            }
#if NATIVE_DEBUG
            else if (s->locked && !NvencActive() && _nowNs() >= s->nextRead) {
                _throughputRead(s, p.slot);
                s->nextRead = _nowNs() + READ_EVERY_NS;
            }
#endif
        }

        // nothing newer to copy meanwhile: the frame in flight goes out now
        if (s->pending && !inFlight) {
            s->ivAlone++;
            _finish(s);
        }
        if (s->attached && s->locked && NvencActive() && UvcDeviceIsStreaming())
            _probe(s);
#if NATIVE_DEBUG
        if (s->attached && s->mode == MODE_CPU && s->hotOk && !s->pending && UvcDeviceIsStreaming() &&
            _nowNs() >= s->nextHot) {
            s->nextHot = _nowNs() + 1000000000ULL;
            _hotRerun(s);
        }
#endif

#if NATIVE_DEBUG
        // suspended (HOME): its buffers hold still - dump the one on screen
        if (s->attached && s->locked && !woke && p.seq != 0 && p.seq != s->dumpedSeq &&
            s->dumps < DUMPS_PER_SESSION && p.process_id == s->dbg.pid &&
            armTicksToNs(armGetSystemTick() - p.tick) >= PAUSED_NS) {
            s->dumpedSeq = p.seq;
            if (VicEngineActive())
                _dumpNv12(s, &p);
            else
                _dump(s, &p);
        }
#endif

        _filler(s);

        if (s->mem && _nowNs() >= s->nextYieldCheck) {
            s->nextYieldCheck = _nowNs() + 1000000000ULL;
            if (DonorShouldYield()) {
                LOG("native: an applet needs the Applet pool - giving the donor memory back\n");
                _releaseMemory(s);
                s->memYielded = true;
            }
        }

        if (s->attached && _nowNs() - lastLog >= 10000000000ULL) {
            _logPeriodic(s, &p);
            lastLog = _nowNs();
        }
    }

    _endSession(s, "probe stopping");
    _sendStop();
    TapClientClose();
    LOG("native: probe exiting\n");
}

#endif
