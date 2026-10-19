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
// Built only with -DNATIVE_PROBE.
#include <stdio.h>
#include <string.h>
#include "../core.h"
#include "../tap_client.h"
#include "game_dbg.h"
#include "slotmatch.h"
#include "donor.h"
#include "vic.h"
#include "vic_engine.h"
#include "native_probe.h"

#ifdef NATIVE_PROBE

#if !LOGGING_ENABLED
#error "NATIVE_PROBE reports only through the log: build with -DFILE_LOGGING (or -DUDP_LOGGING)"
#endif

#define SIG_POINTS        32
#define SAMPLE_DELAY_NS   8000000ULL   // without the engine (no nvdrv): no fence wait, let the GPU finish
#define FENCE_TIMEOUT_NS  250000000ULL // SFT v0.7.1: some games queue ~20 ms (up to 68) before the GPU is done
#define READ_EVERY_NS     1000000000ULL
#define DUMPS_PER_SESSION 3
#define PAUSED_NS         1000000000ULL  // no present for this long: the game is suspended
#define MAX_BUF_BYTES     (16u * 1024 * 1024)
#define CHUNK             0x20000      // reads go through this; dumps are written from it piece by piece
#define DUMP_DIR          "/config/sysdvr-uvc/dump"
#define DONOR_MB          16           // per game session: VIC buffers + a copy of the game's buffer (vic_engine.c)

extern volatile bool g_nativeProbeRunning;

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
} Session;

static Session g_s;

static u64 _nowNs(void) { return armTicksToNs(armGetSystemTick()); }

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
    if (!bad && VicEngineOpen(s->mem, s->memSize, s->surf.buf_size))
        GameDbgSetBounce(VicEngineBounce(), VicEngineBounceSize());
}

// The order matters: no dmnt read may use the bounce once the memory goes;
// the nvdrv session must be closed (freeing every pin) before the unmap.
static void _releaseMemory(Session* s)
{
    if (!s->mem)
        return;
    GameDbgSetBounce(NULL, 0);
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
static bool _readRegion(Session* s, u64 addr, const VicSource* src, u64* bytes)
{
    u8* copy = VicEngineSlotCopy();
    const u64 blockRows = 8u << src->blk_h_log2;
    const u64 blockBytes = 512u << src->blk_h_log2;
    const u64 gobsX = ((u64)src->stride_px * 4 + 63) / 64;
    const u64 rowBytes = gobsX * blockBytes;
    const u64 readPerRow = (((u64)src->rect_w * 4 + 63) / 64) * blockBytes;
    const u64 rows = (src->rect_h + blockRows - 1) / blockRows;
    u64 total = 0;
    for (u64 br = 0; br < rows; br++) {
        const u64 off = br * rowBytes;
        if (off + readPerRow > s->surf.buf_size)
            break;
        if (!GameDbgRead(&s->dbg, copy + off, addr + off, readPerRow))
            return false;
        armDCacheFlush(copy + off, readPerRow);  // the engine reads it from memory
        total += readPerRow;
    }
    *bytes = total;
    return true;
}

// One present through the VIC: copy, convert. The fence was waited on.
static bool _convert(Session* s, const UvcTapPresent* p)
{
    VicSource src;
    _source(s, p, &src);
    u64 bytes = 0, vicNs = 0;
    const u64 t0 = _nowNs();
    if (!_readRegion(s, _slotAddr(s, p->slot), &src, &bytes)) {
        s->convFails++;
        return false;
    }
    const u64 t1 = _nowNs();
    if (!VicEngineConvert(&src, &vicNs)) {
        s->convFails++;
        return false;
    }
    s->convs++;
    s->ivConvs++;
    s->ivReadNs += t1 - t0;
    s->ivVicNs += vicNs;
    s->ivReadBytes += bytes;
    return true;
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
        const u64 n = s->ivConvs ? s->ivConvs : 1;
        LOG("native: VIC last 10 s: %u frames (%lu.%lu fps), per frame avg: fence wait %lu us (max %lu), read %lu KB in %lu us, "
            "VIC %lu us (max %lu) | total %u, failed %u, fence timeouts %u%s\n",
            s->ivConvs, (u64)s->ivConvs / 10, (u64)s->ivConvs % 10, s->ivFenceNs / n / 1000, s->ivFenceMax / 1000,
            s->ivReadBytes / n / 1024, s->ivReadNs / n / 1000, s->ivVicNs / n / 1000, vs.nsMax / 1000,
            s->convs, s->convFails, s->fenceTimeouts, VicEngineWedged() ? " - ENGINE WEDGED" : "");
        s->ivConvs = 0;
        s->ivReadNs = s->ivVicNs = s->ivFenceNs = s->ivFenceMax = s->ivReadBytes = 0;
    }
}

void NativeProbeThread(void* arg)
{
    (void)arg;
    LOG("native: probe started (phase 2b-3: attach, find slots, fence, VIC to NV12 every frame, dumps)\n");
    DonorCleanupStale();
    Session* s = &g_s;
    memset(s, 0, sizeof(*s));
    u64 lastLog = _nowNs();
    u64 failedPid = 0, failedGen = 0;

    while (g_nativeProbeRunning) {
        if (!TapClientIsConnected()) {
            if (R_FAILED(TapClientConnect())) {
                svcSleepThread(2000000000ULL);
                continue;
            }
            LOG("native: connected to uvctap\n");
        }

        Event* ev = TapClientPresentEvent();
        bool woke = R_SUCCEEDED(eventWait(ev, 1000000000ULL));
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

        // the game went away or another one runs
        if (s->attached && (st.app_pid != s->dbg.pid || DebugPumpGone()))
            _endSession(s, DebugPumpGone() ? "game exited" : "application changed");

        const bool presenting = p.seq != 0 && p.process_id == st.app_pid && st.app_pid != 0 &&
                                armTicksToNs(armGetSystemTick() - p.tick) < 500000000ULL;
        if (!s->attached && presenting) {
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

        if (s->attached && woke && p.seq != s->lastSeq) {
            if (VicEngineActive()) {
                // the present's acquire fence: the GPU may still be drawing
                // the slot when the game queues it
                u64 waited = 0;
                if (!VicEngineWaitFence(p.fence_count, p.fence, FENCE_TIMEOUT_NS, &waited))
                    s->fenceTimeouts++;
                s->ivFenceNs += waited;
                if (waited > s->ivFenceMax)
                    s->ivFenceMax = waited;
            } else {
                const u64 due = armTicksToNs(p.tick) + SAMPLE_DELAY_NS;
                const u64 now = _nowNs();
                if (due > now)
                    svcSleepThread(due - now);
            }
            _calibrate(s, &p);
            s->lastSeq = p.seq;

            if (s->locked && VicEngineActive()) {
                _convert(s, &p);
            } else if (s->locked && _nowNs() >= s->nextRead) {
                _throughputRead(s, p.slot);
                s->nextRead = _nowNs() + READ_EVERY_NS;
            }
        }

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
    TapClientClose();
    LOG("native: probe exiting\n");
}

#endif
