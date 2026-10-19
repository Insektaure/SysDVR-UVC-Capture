#include <string.h>
#include "../core.h"
#include "game_dbg.h"

// svcContinueDebugEvent flags (Atmosphère svc_types_common.hpp; not in libnx)
#define CONTINUE_EXCEPTION_HANDLED (1u << 0)
#define CONTINUE_ALL               (1u << 2)

#define KERNEL_RESULT(desc) MAKERESULT(Module_Kernel, desc)
#define RESULT_TIMED_OUT          KERNEL_RESULT(117)
#define RESULT_BUSY               KERNEL_RESULT(122)
#define RESULT_PROCESS_TERMINATED KERNEL_RESULT(520)

// dmnt:cht command ids (Atmosphère dmntcht.os.horizon.c)
#define DMNT_CHT_HAS_CHEAT_PROCESS 65000
#define DMNT_CHT_READ_MEMORY       65102
#define DMNT_CHT_QUERY_MEMORY      65104
#define DMNT_CHT_GET_CHEAT_COUNT   65200

// ============================================================================
// dmnt:cht
// ============================================================================

static Service g_cht;
static bool g_chtOpen = false;
static Mutex g_chtLock;
static u8* g_bounce = NULL;
static size_t g_bounceSize = 0;

void GameDbgSetBounce(void* buf, size_t size)
{
    mutexLock(&g_chtLock);
    g_bounce = (u8*)buf;
    g_bounceSize = size;
    mutexUnlock(&g_chtLock);
}

static bool _chtService(void)
{
    if (g_chtOpen)
        return true;
    // dmnt:cht allows only a few sessions; never block on a missing one
    if (!CoreHasService("dmnt:cht"))
        return false;
    g_chtOpen = R_SUCCEEDED(smGetService(&g_cht, "dmnt:cht"));
    return g_chtOpen;
}

static bool _chtHasProcess(void)
{
    u8 has = 0;
    return R_SUCCEEDED(serviceDispatchOut(&g_cht, DMNT_CHT_HAS_CHEAT_PROCESS, has)) && (has & 1);
}

// ============================================================================
// Attach / read / detach
// ============================================================================

bool GameDbgAttach(u64 pid, GameDbg* out, Result* direct_rc)
{
    memset(out, 0, sizeof(*out));
    out->handle = INVALID_HANDLE;
    out->pid = pid;

    Result rc = svcDebugActiveProcess(&out->handle, pid);
    if (direct_rc)
        *direct_rc = rc;
    if (R_SUCCEEDED(rc))
        return true;

    // HasCheatProcess revalidates inside dmnt (handle alive, same
    // application), so "true" means dmnt is the debugger that beat us
    out->handle = INVALID_HANDLE;
    mutexLock(&g_chtLock);
    bool viaDmnt = _chtService() && _chtHasProcess();
    mutexUnlock(&g_chtLock);
    out->dmnt = viaDmnt;
    return viaDmnt;
}

s64 GameDbgDmntCheatCount(GameDbg* d)
{
    if (!d->dmnt)
        return -1;
    u64 count = 0;
    mutexLock(&g_chtLock);
    Result rc = serviceDispatchOut(&g_cht, DMNT_CHT_GET_CHEAT_COUNT, count);
    mutexUnlock(&g_chtLock);
    return R_SUCCEEDED(rc) ? (s64)count : -1;
}

Result GameDbgResume(GameDbg* d, u32* nev)
{
    if (nev)
        *nev = 0;
    if (d->dmnt)
        return 0;
    DebugEventInfo ev;
    u32 n = 0;
    while (n < 256 && R_SUCCEEDED(svcGetDebugEvent(&ev, d->handle)))
        n++;
    if (nev)
        *nev = n;
    return svcContinueDebugEvent(d->handle, CONTINUE_EXCEPTION_HANDLED | CONTINUE_ALL, NULL, 0);
}

bool GameDbgRead(GameDbg* d, void* dst, u64 va, size_t len)
{
    if (!d->dmnt)
        return R_SUCCEEDED(svcReadDebugProcessMemory(dst, d->handle, va, len));

    // 2 MB pieces, as SFT reads through dmnt (smaller cost too many IPCs)
    Result rc = 0;
    mutexLock(&g_chtLock);
    const size_t piece = g_bounce ? (g_bounceSize < 0x200000 ? g_bounceSize : 0x200000) : 0x200000;
    for (size_t off = 0; off < len && R_SUCCEEDED(rc); off += piece) {
        const size_t n = len - off < piece ? len - off : piece;
        const struct { u64 address; u64 size; } in = { va + off, n };
        u8* to = g_bounce ? g_bounce : (u8*)dst + off;
        rc = serviceDispatchIn(&g_cht, DMNT_CHT_READ_MEMORY, in,
            .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
            .buffers = { { to, n } },
        );
        if (R_SUCCEEDED(rc) && g_bounce)
            memcpy((u8*)dst + off, g_bounce, n);
    }
    mutexUnlock(&g_chtLock);
    return R_SUCCEEDED(rc);
}

bool GameDbgQuery(GameDbg* d, MemoryInfo* mi, u64 addr)
{
    if (!d->dmnt) {
        u32 pageinfo = 0;
        return R_SUCCEEDED(svcQueryDebugProcessMemory(mi, &pageinfo, d->handle, addr));
    }
    mutexLock(&g_chtLock);
    Result rc = serviceDispatchInOut(&g_cht, DMNT_CHT_QUERY_MEMORY, addr, *mi);
    mutexUnlock(&g_chtLock);
    return R_SUCCEEDED(rc);
}

void GameDbgDetach(GameDbg* d)
{
    // dmnt's handle is dmnt's: closing it would detach the cheat engine
    if (!d->dmnt && d->handle != INVALID_HANDLE) {
        DebugPumpStop();
        svcCloseHandle(d->handle);
    }
    d->handle = INVALID_HANDLE;
    d->dmnt = false;
    d->pid = 0;
}

// ============================================================================
// Debug event pump
// ============================================================================

// svc priorities. The pump sits above every other thread of ours on core 3
// and never spins (every path blocks or returns within microseconds). A
// helper continues on a game core: 24, the npdm's best, is above every
// application priority (28 and up) - at a lower one a busy game thread kept
// a helper off its core for 10-26 s while the new thread's event held the
// whole game (SFT, DuckStation).
#define PUMP_PRIORITY    0x2B
#define HELPER_PRIORITY  24
#define PUMP_CORE        3
#define NUM_HELPERS      3     // cores 0, 1, 2
#define WAIT_TIMEOUT_NS  50000000ULL  // only how fast a stop is noticed

typedef struct {
    Thread   thread;
    bool     ok;
    Mutex    lock;
    CondVar  cv;
    bool     pending, done, fault;
    Handle   handle;
    Result   rc;
} PumpHelper;

static u8 alignas(0x1000) g_pumpStack[0x2000];
static u8 alignas(0x1000) g_helperStacks[NUM_HELPERS][0x1000];
static PumpHelper g_helpers[NUM_HELPERS];
static bool g_helpersStarted = false;

static Thread g_pumpThread;
static bool g_pumpRunning = false;
static Handle g_pumpHandle = INVALID_HANDLE;
static volatile bool g_pumpStop = false;
static volatile bool g_pumpGone = false;

static Mutex g_statsLock;
static DebugPumpStats g_stats;

static Result _continue(Handle h, bool fault)
{
    // a real fault is left to the game, as if no debugger were attached
    return svcContinueDebugEvent(h, (fault ? 0 : CONTINUE_EXCEPTION_HANDLED) | CONTINUE_ALL, NULL, 0);
}

static void _helperThread(void* arg)
{
    PumpHelper* hp = (PumpHelper*)arg;
    mutexLock(&hp->lock);
    for (;;) {
        while (!hp->pending)
            condvarWait(&hp->cv, &hp->lock);
        hp->pending = false;
        Handle h = hp->handle;
        bool fault = hp->fault;
        mutexUnlock(&hp->lock);
        Result rc = _continue(h, fault);
        mutexLock(&hp->lock);
        hp->rc = rc;
        hp->done = true;
        condvarWakeAll(&hp->cv);
    }
}

static Result _continueOnCore(Handle h, bool fault, u32 core, u32* ranOn)
{
    if (core >= NUM_HELPERS || !g_helpers[core].ok) {
        *ranOn = PUMP_CORE;
        return _continue(h, fault);
    }
    PumpHelper* hp = &g_helpers[core];
    mutexLock(&hp->lock);
    hp->handle = h;
    hp->fault = fault;
    hp->done = false;
    hp->pending = true;
    condvarWakeAll(&hp->cv);
    while (!hp->done)
        condvarWait(&hp->cv, &hp->lock);
    Result rc = hp->rc;
    mutexUnlock(&hp->lock);
    *ranOn = core;
    return rc;
}

// One wake: drain, continue. False when the game is gone.
static bool _service(Handle h)
{
    const u64 t0 = armTicksToNs(armGetSystemTick());
    u32 n = 0, created = 0, exited = 0, other = 0, faults = 0, core = PUMP_CORE;
    bool processExited = false;
    DebugEventInfo ev;
    while (n < 256 && R_SUCCEEDED(svcGetDebugEvent(&ev, h))) {
        n++;
        switch (ev.type) {
            case DebugEventType_CreateThread: {
                created++;
                u64 o64 = 0;
                u32 o32 = 0;
                if (R_SUCCEEDED(svcGetDebugThreadParam(&o64, &o32, h, ev.info.create_thread.thread_id,
                                                       DebugThreadParam_CurrentCore)))
                    core = o32;
                break;
            }
            case DebugEventType_ExitThread:
                exited++;
                break;
            case DebugEventType_ExitProcess:
                other++;
                processExited = true;
                break;
            case DebugEventType_Exception:
                other++;
                if (ev.info.exception.type != DebugException_DebuggerAttached &&
                    ev.info.exception.type != DebugException_DebuggerBreak)
                    faults++;
                break;
            default:
                other++;
                break;
        }
    }

    // signalled with nothing queued: detached or terminated
    if (n == 0) {
        mutexLock(&g_statsLock);
        g_stats.wakes++;
        mutexUnlock(&g_statsLock);
        return false;
    }

    u32 ranOn = PUMP_CORE;
    Result rc = _continueOnCore(h, faults != 0, core, &ranOn);
    const u64 held = armTicksToNs(armGetSystemTick()) - t0;

    mutexLock(&g_statsLock);
    g_stats.wakes++;
    g_stats.events += n;
    g_stats.created += created;
    g_stats.exited += exited;
    g_stats.other += other;
    g_stats.faults += faults;
    if (held > g_stats.max_hold_ns)
        g_stats.max_hold_ns = held;
    bool keep = !processExited;
    if (R_SUCCEEDED(rc)) {
        g_stats.continues++;
        g_stats.per_core[ranOn & 3]++;
    } else if (rc == RESULT_BUSY) {
        g_stats.busy++;  // an event came in between: the next wake handles it
    } else {
        g_stats.failed++;
        g_stats.last_fail_rc = rc;
        if (rc == RESULT_PROCESS_TERMINATED)
            keep = false;
    }
    mutexUnlock(&g_statsLock);
    return keep;
}

static void _pumpThread(void* arg)
{
    (void)arg;
    const Handle h = g_pumpHandle;
    u32 busyWakes = 0;
    while (!g_pumpStop) {
        Result rc = svcWaitSynchronizationSingle(h, WAIT_TIMEOUT_NS);
        if (g_pumpStop)
            break;
        if (rc == RESULT_TIMED_OUT) {
            busyWakes = 0;
            continue;
        }
        if (R_FAILED(rc) || !_service(h)) {
            g_pumpGone = true;
            break;
        }
        // a thousand back-to-back wakes without one timeout is not a game
        // making threads: never spin at this priority
        if (++busyWakes > 1000) {
            svcSleepThread(1000000);
            busyWakes = 0;
        }
    }
}

static void _startHelpers(void)
{
    if (g_helpersStarted)
        return;
    g_helpersStarted = true;
    for (u32 i = 0; i < NUM_HELPERS; i++) {
        PumpHelper* hp = &g_helpers[i];
        mutexInit(&hp->lock);
        condvarInit(&hp->cv);
        // a core the npdm does not allow is not fatal: its continues then
        // run on core 3
        hp->ok = R_SUCCEEDED(threadCreate(&hp->thread, _helperThread, hp, g_helperStacks[i],
                                          sizeof(g_helperStacks[i]), HELPER_PRIORITY, (int)i)) &&
                 R_SUCCEEDED(threadStart(&hp->thread));
        if (!hp->ok)
            LOG("debug pump: no helper on core %u - its continues run on core 3\n", i);
    }
}

void DebugPumpStart(Handle dbg)
{
    if (g_pumpRunning)
        return;
    _startHelpers();
    mutexLock(&g_statsLock);
    memset(&g_stats, 0, sizeof(g_stats));
    mutexUnlock(&g_statsLock);
    g_pumpHandle = dbg;
    g_pumpStop = false;
    g_pumpGone = false;
    if (R_FAILED(threadCreate(&g_pumpThread, _pumpThread, NULL, g_pumpStack, sizeof(g_pumpStack),
                              PUMP_PRIORITY, PUMP_CORE)) ||
        R_FAILED(threadStart(&g_pumpThread))) {
        LOG("debug pump: NOT started - the game will stop at its next thread start/exit\n");
        return;
    }
    g_pumpRunning = true;
}

void DebugPumpStop(void)
{
    if (!g_pumpRunning)
        return;
    g_pumpStop = true;
    threadWaitForExit(&g_pumpThread);  // at most one wait timeout
    threadClose(&g_pumpThread);
    g_pumpRunning = false;
    g_pumpHandle = INVALID_HANDLE;
    // the next session starts clean: a stale "gone" from the last game ended
    // every session through dmnt:cht at once (no pump there), 884 restarts
    g_pumpGone = false;
}

bool DebugPumpGone(void)
{
    return g_pumpRunning && g_pumpGone;
}

void DebugPumpGetStats(DebugPumpStats* out)
{
    mutexLock(&g_statsLock);
    *out = g_stats;
    mutexUnlock(&g_statsLock);
    out->gone = g_pumpGone;
}
