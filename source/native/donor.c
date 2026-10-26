#include <string.h>
#include "../core.h"
#include "uvcdonor.h"
#include "donor.h"

// After Switch Frame Tap's applet_mitm_donor.cpp (GPL-2.0, Tomas Uribe).

#define APPLET_MARGIN_MB 48  // Applet pool left free beside us (SFT TightMb)
#define WEB_ROOM_MB 96       // more room needed beside a web applet (some games keep OfflineWeb open)
#define WEB_GROWTH_MB 24     // a web applet growing this much past our start needs the memory

static Mutex g_lock;
static bool g_mapped = false;
static TransferMemory g_tmem;
static Handle g_nvTmem = INVALID_HANDLE;
static u64 g_nvSize = 0;
static u64 g_donorPid = 0;
static u64 g_baseMb = 0;     // the Applet group's lowest since our memory came

// ============================================================================
// pm
// ============================================================================

bool DonorAppletGroupMb(u64* used, u64* limit)
{
    if (R_FAILED(pmdmntInitialize()))
        return false;
    // AtmosphereGetCurrentLimitInfo(group, resource 0 = PhysicalMemoryMax).
    // Groups (pm_types.hpp): 0 System, 1 Application (the game's), 2 Applet.
    const struct { u32 group, resource; } in = { 2, 0 };
    struct { s64 cur, lim; } out = { 0, 0 };
    Result rc = serviceDispatchInOut(pmdmntGetServiceSession(), 65001, in, out);
    pmdmntExit();
    if (R_FAILED(rc))
        return false;
    *used = (u64)out.cur >> 20;
    *limit = (u64)out.lim >> 20;
    return true;
}

static bool _running(u64 programId, u64* pid)
{
    if (R_FAILED(pmdmntInitialize()))
        return false;
    // command 2 (5.0.0+) directly: libnx's pmdmntGetProcessId picks the id
    // from the firmware version and, if that were unknown, would send
    // HookToCreateProcess instead - see donor/source/main.c
    u64 p = 0;
    Result rc = serviceDispatchInOut(pmdmntGetServiceSession(), 2, programId, p);
    pmdmntExit();
    if (pid)
        *pid = p;
    return R_SUCCEEDED(rc) && p != 0;
}

// The browser applets (offline web: manuals and news, the eShop, the web
// browser, login pages) can take nearly the whole Applet pool. Firmware 22+:
// systemWeb (0x1042) and openWeb (0x1043).
static bool _webAppletRunning(void)
{
    static const u64 web[] = { 0x010000000000100Aull, 0x010000000000100Bull, 0x010000000000100Full,
                               0x0100000000001010ull, 0x0100000000001011ull,
                               0x0100000000001042ull, 0x0100000000001043ull };
    for (u32 i = 0; i < sizeof(web) / sizeof(web[0]); i++) {
        if (_running(web[i], NULL))
            return true;
    }
    return false;
}

static void _terminateDonor(void)
{
    if (g_donorPid == 0)
        return;
    if (R_SUCCEEDED(pmshellInitialize())) {
        Result rc = pmshellTerminateProcess(g_donorPid);
        LOG("donor: pid %lu terminated (0x%x)\n", g_donorPid, rc);
        (void)rc;
        pmshellExit();
    }
    g_donorPid = 0;
}

void DonorCleanupStale(void)
{
    u64 pid = 0, used = 0, limit = 0;
    if (_running(UVCDONOR_PROGRAM_ID, &pid)) {
        DonorAppletGroupMb(&used, &limit);
        LOG("donor: one is still running (pid %lu, Applet group %lu of %lu MB) - an earlier instance's - terminating it\n",
            pid, used, limit);
        g_donorPid = pid;
        _terminateDonor();
    }
    DonorAppletGroupMb(&used, &limit);
    LOG("donor: none running at start; Applet group %lu of %lu MB\n", used, limit);
}

// ============================================================================
// The one-shot server the donor talks to
// ============================================================================

typedef struct {
    u32 cmd;
    const u8* args;
    u32 argsSize;
    u32 numCopy;
    Handle copy[4];
    bool closed;
} DonorRequest;

static bool _receive(Handle session, DonorRequest* r, u64 timeoutNs)
{
    s32 idx = -1;
    Result rc = svcReplyAndReceive(&idx, &session, 1, INVALID_HANDLE, timeoutNs);
    if (R_FAILED(rc))
        return false;  // timeout, or the donor closed the session
    memset(r, 0, sizeof(*r));
    HipcParsedRequest req = hipcParseRequest(armGetTls());
    if (req.meta.type == CmifCommandType_Close) {
        r->closed = true;
        return true;
    }
    r->numCopy = req.meta.num_copy_handles < 4 ? req.meta.num_copy_handles : 4;
    for (u32 i = 0; i < r->numCopy; i++)
        r->copy[i] = req.data.copy_handles[i];
    const uintptr_t words = (uintptr_t)req.data.data_words;
    const uintptr_t raw = (words + 0xF) & ~(uintptr_t)0xF;
    const CmifInHeader* hdr = (const CmifInHeader*)raw;
    if (req.meta.type != CmifCommandType_Request || hdr->magic != CMIF_IN_HEADER_MAGIC) {
        r->cmd = ~0u;
        return true;
    }
    r->cmd = hdr->command_id;
    r->args = (const u8*)(raw + sizeof(CmifInHeader));
    const u32 total = req.meta.num_data_words * 4;
    const u32 used = (u32)(raw - words) + sizeof(CmifInHeader);
    r->argsSize = total > used ? total - used : 0;
    return true;
}

static void _reply(Handle session, Result result, const void* out, u32 outSize)
{
    HipcRequest resp = hipcMakeRequestInline(armGetTls(),
        .type = CmifCommandType_Invalid,
        .num_data_words = (sizeof(CmifOutHeader) + outSize + 0x10 + 3) / 4,
    );
    CmifOutHeader* hdr = (CmifOutHeader*)(((uintptr_t)resp.data_words + 0xF) & ~(uintptr_t)0xF);
    hdr->magic = CMIF_OUT_HEADER_MAGIC;
    hdr->version = 0;
    hdr->result = result;
    hdr->token = 0;
    if (outSize)
        memcpy(hdr + 1, out, outSize);
    s32 idx = -1;
    // sends the reply, then times out waiting for nothing: expected
    svcReplyAndReceive(&idx, &session, 0, session, 0);
}

// ============================================================================
// Obtain / release
// ============================================================================

bool DonorObtain(u32 mb, void** addr, u64* size)
{
    mutexLock(&g_lock);
    if (g_mapped) {
        *addr = g_tmem.map_addr;
        *size = g_tmem.size;
        mutexUnlock(&g_lock);
        return true;
    }

    u64 used = 0, limit = 0;
    const bool known = DonorAppletGroupMb(&used, &limit);
    const bool tight = known && limit - used < mb + APPLET_MARGIN_MB;
    const bool web = !tight && (!known || limit - used < mb + APPLET_MARGIN_MB + WEB_ROOM_MB) && _webAppletRunning();
    if (tight || web) {
        LOG("donor: not now - %s (Applet group %lu of %lu MB)\n",
            tight ? "the Applet pool is nearly full" : "a web applet is open, without room beside it",
            used, limit);
        mutexUnlock(&g_lock);
        return false;
    }

    Handle port = INVALID_HANDLE;
    Result rc = smRegisterService(&port, smEncodeName(UVCDONOR_SERVICE_NAME), false, 1);
    if (R_FAILED(rc)) {
        LOG("donor: register %s 0x%x\n", UVCDONOR_SERVICE_NAME, rc);
        mutexUnlock(&g_lock);
        return false;
    }

    bool ok = false;
    Handle session = INVALID_HANDLE;
    if (R_SUCCEEDED(rc = pmshellInitialize())) {
        const NcmProgramLocation loc = { .program_id = UVCDONOR_PROGRAM_ID, .storageID = NcmStorageId_None };
        rc = pmshellLaunchProgram(0, &loc, &g_donorPid);
        pmshellExit();
    }
    const u64 t0 = armTicksToNs(armGetSystemTick());
    (void)t0;  // only logged
    LOG("donor: launch 0x%x pid %lu, asking %u MB (Applet group %lu of %lu MB)\n", rc, g_donorPid, mb, used, limit);

    if (R_SUCCEEDED(rc) && R_SUCCEEDED(rc = svcWaitSynchronizationSingle(port, 5000000000ULL)) &&
        R_SUCCEEDED(rc = svcAcceptSession(&session, port))) {
        DonorRequest r;
        u64 tmemSize = 0, nvSize = 0;
        Handle tmem = INVALID_HANDLE, nv = INVALID_HANDLE;
        bool done = false;
        while (!done && _receive(session, &r, 5000000000ULL)) {
            if (r.closed)
                break;
            switch (r.cmd) {
                case UvcDonorCmd_GetSizeMb:
                    _reply(session, 0, &mb, sizeof(mb));
                    break;
                case UvcDonorCmd_Give:
                    if (r.numCopy == 2 && r.argsSize >= 16) {
                        memcpy(&tmemSize, r.args, 8);
                        memcpy(&nvSize, r.args + 8, 8);
                        tmem = r.copy[0];
                        nv = r.copy[1];
                    } else {
                        for (u32 i = 0; i < r.numCopy; i++)
                            svcCloseHandle(r.copy[i]);
                    }
                    _reply(session, 0, NULL, 0);
                    done = true;
                    break;
                case UvcDonorCmd_Failed: {
                    u32 why = 0;
                    if (r.argsSize >= 4)
                        memcpy(&why, r.args, 4);
                    LOG("donor: the Applet pool refused every size (0x%x)\n", why);
                    _reply(session, 0, NULL, 0);
                    done = true;
                    break;
                }
                default:
                    for (u32 i = 0; i < r.numCopy; i++)
                        svcCloseHandle(r.copy[i]);
                    _reply(session, MAKERESULT(Module_Libnx, LibnxError_NotFound), NULL, 0);
                    break;
            }
        }
        if (tmem != INVALID_HANDLE) {
            tmemLoadRemote(&g_tmem, tmem, tmemSize, Perm_None);
            rc = tmemMap(&g_tmem);
            if (R_SUCCEEDED(rc)) {
                g_mapped = ok = true;
                g_baseMb = 0;
                g_nvTmem = nv;
                g_nvSize = nvSize;
                nv = INVALID_HANDLE;
            } else {
                LOG("donor: map %lu MB 0x%x\n", tmemSize >> 20, rc);
                tmemClose(&g_tmem);
            }
        }
        if (nv != INVALID_HANDLE)
            svcCloseHandle(nv);
    } else {
        LOG("donor: no connection (0x%x)\n", rc);
    }
    if (session != INVALID_HANDLE)
        svcCloseHandle(session);
    smUnregisterService(smEncodeName(UVCDONOR_SERVICE_NAME));
    svcCloseHandle(port);

    if (ok) {
        DonorAppletGroupMb(&used, &limit);
        LOG("donor: %lu MB mapped at %p (+%lu KB for nvdrv) in %lu ms; Applet group now %lu of %lu MB\n",
            g_tmem.size >> 20, g_tmem.map_addr, g_nvSize / 1024,
            (armTicksToNs(armGetSystemTick()) - t0) / 1000000, used, limit);
        *addr = g_tmem.map_addr;
        *size = g_tmem.size;
    } else {
        _terminateDonor();
    }
    mutexUnlock(&g_lock);
    return ok;
}

bool DonorTakeNvTmem(Handle* out, u64* size)
{
    mutexLock(&g_lock);
    bool ok = g_nvTmem != INVALID_HANDLE;
    if (ok) {
        *out = g_nvTmem;
        *size = g_nvSize;
        g_nvTmem = INVALID_HANDLE;
    }
    mutexUnlock(&g_lock);
    return ok;
}

void DonorRelease(void)
{
    mutexLock(&g_lock);
    if (g_nvTmem != INVALID_HANDLE) {  // nvdrv never took it
        svcCloseHandle(g_nvTmem);
        g_nvTmem = INVALID_HANDLE;
    }
    if (g_mapped) {
        // The kernel unmaps only memory with no attribute left: engine
        // buffers are made Uncached (SFT test14: 0xd401, attribute 0x8)
        svcSetMemoryAttribute(g_tmem.map_addr, g_tmem.size, MemAttr_IsUncached, 0);
        // and refuses while a device still maps the pages: nvservices drops
        // its pins only once it sees our nvdrv session closed (SFT test13)
        Result rc = 0;
        int tries = 0;
        for (; tries < 100; tries++) {
            if (R_SUCCEEDED(rc = tmemUnmap(&g_tmem)))
                break;
            svcSleepThread(50000000ULL);
        }
        if (R_FAILED(rc)) {
            MemoryInfo mi = {0};
            u32 pi = 0;
            svcQueryMemory(&mi, &pi, (u64)g_tmem.map_addr);
            LOG("donor: unmap refused for 5 s (0x%x; attr 0x%x, device maps %u) - keeping the memory\n",
                rc, mi.attr, mi.device_refcount);
            mutexUnlock(&g_lock);
            return;
        }
        tmemCloseHandle(&g_tmem);
        g_mapped = false;
        g_baseMb = 0;
        LOG("donor: memory unmapped%s\n", tries ? " (after waiting for nvservices)" : "");
    }

    // the donor frees its heap ~5 ms after the unmap and exits; terminate it
    // only if it is still there after a second
    u64 pid = 0;
    for (int i = 0; i < 20 && g_donorPid != 0 && _running(UVCDONOR_PROGRAM_ID, &pid); i++)
        svcSleepThread(50000000ULL);
    if (g_donorPid != 0 && _running(UVCDONOR_PROGRAM_ID, &pid)) {
        LOG("donor: still running 1 s after the release\n");
        _terminateDonor();
    } else if (g_donorPid != 0) {
        u64 used = 0, limit = 0;
        DonorAppletGroupMb(&used, &limit);
        LOG("donor: exited by itself; Applet group now %lu of %lu MB\n", used, limit);
        g_donorPid = 0;
    }
    mutexUnlock(&g_lock);
}

bool DonorActive(void)
{
    return g_mapped;
}

// The applets are only asked when the pool looks tight for one
bool DonorShouldYield(void)
{
    u64 used = 0, limit = 0;
    if (!DonorAppletGroupMb(&used, &limit))
        return _webAppletRunning();
    if (limit - used < APPLET_MARGIN_MB)
        return true;
    if (!g_baseMb || used < g_baseMb)
        g_baseMb = used;
    return (limit - used < WEB_ROOM_MB || used >= g_baseMb + WEB_GROWTH_MB) && _webAppletRunning();
}
