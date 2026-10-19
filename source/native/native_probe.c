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
#include "native_probe.h"

#ifdef NATIVE_PROBE

#if !LOGGING_ENABLED
#error "NATIVE_PROBE reports only through the log: build with -DFILE_LOGGING (or -DUDP_LOGGING)"
#endif

#define SIG_POINTS        32
#define SAMPLE_DELAY_NS   8000000ULL   // after the present: no fence wait yet (phase 2b), let the GPU finish
#define READ_EVERY_NS     1000000000ULL
#define DUMPS_PER_SESSION 3
#define PAUSED_NS         1000000000ULL  // no present for this long: the game is suspended
#define MAX_BUF_BYTES     (16u * 1024 * 1024)
#define CHUNK             0x20000      // reads go through this; dumps are written from it piece by piece
#define DUMP_DIR          "/config/sysdvr-uvc/dump"

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
}

void NativeProbeThread(void* arg)
{
    (void)arg;
    LOG("native: probe started (phase 2a: attach, find slots, read, dump)\n");
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
            // phase 2a has no fence wait: give the GPU time to finish the slot
            const u64 due = armTicksToNs(p.tick) + SAMPLE_DELAY_NS;
            const u64 now = _nowNs();
            if (due > now)
                svcSleepThread(due - now);
            _calibrate(s, &p);
            s->lastSeq = p.seq;

            if (s->locked && _nowNs() >= s->nextRead) {
                _throughputRead(s, p.slot);
                s->nextRead = _nowNs() + READ_EVERY_NS;
            }
        }

        // suspended (HOME): its buffers hold still - dump the one on screen
        if (s->attached && s->locked && !woke && p.seq != 0 && p.seq != s->dumpedSeq &&
            s->dumps < DUMPS_PER_SESSION && p.process_id == s->dbg.pid &&
            armTicksToNs(armGetSystemTick() - p.tick) >= PAUSED_NS) {
            s->dumpedSeq = p.seq;
            _dump(s, &p);
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
