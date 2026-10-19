#include "core.h"
#include "tap_client.h"

static Service g_tapService;
static Event g_presentEvent;
static bool g_connected = false;
static u32 g_version = 0;

Result TapClientConnect(void)
{
    if (g_connected)
        return 0;
    if (!CoreHasService(UVCTAP_SERVICE_NAME))
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    R_RET_ON_FAIL(smGetService(&g_tapService, UVCTAP_SERVICE_NAME));

    u32 version = 0;
    Result rc = serviceDispatchOut(&g_tapService, UvcTapCmd_GetVersion, version);
    // v2 (no present history) still works: an older vi-tap until the next reboot
    if (R_SUCCEEDED(rc) && (version < 2 || version > UVCTAP_VERSION)) {
        LOG("uvctap: protocol %u, expected 2..%u\n", version, UVCTAP_VERSION);
        rc = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    }
    g_version = version;

    Handle h = INVALID_HANDLE;
    if (R_SUCCEEDED(rc)) {
        rc = serviceDispatch(&g_tapService, UvcTapCmd_GetPresentEvent,
            .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
            .out_handles = &h,
        );
    }
    if (R_FAILED(rc)) {
        serviceClose(&g_tapService);
        return rc;
    }

    eventLoadRemote(&g_presentEvent, h, false);
    g_connected = true;
    return 0;
}

void TapClientClose(void)
{
    if (!g_connected)
        return;
    eventClose(&g_presentEvent);
    serviceClose(&g_tapService);
    g_connected = false;
}

bool TapClientIsConnected(void)
{
    return g_connected;
}

Event* TapClientPresentEvent(void)
{
    return &g_presentEvent;
}

Result TapClientGetLatestPresent(UvcTapPresent* out)
{
    return serviceDispatchOut(&g_tapService, UvcTapCmd_GetLatestPresent, *out);
}

Result TapClientGetStats(UvcTapStats* out)
{
    return serviceDispatchOut(&g_tapService, UvcTapCmd_GetStats, *out);
}

u32 TapClientVersion(void)
{
    return g_connected ? g_version : 0;
}

Result TapClientGetRecentPresents(UvcTapPresent* out, u32 max, u32* count)
{
    *count = 0;
    if (g_version < 3)
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    return serviceDispatchOut(&g_tapService, UvcTapCmd_GetRecentPresents, *count,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
        .buffers = { { out, max * sizeof(UvcTapPresent) } },
    );
}

Result TapClientGetSurface(UvcTapSurface* out)
{
    return serviceDispatchOut(&g_tapService, UvcTapCmd_GetSurface, *out);
}

// ============================================================================
// Phase 1 probe
// ============================================================================

#ifdef TAP_PROBE

extern volatile bool g_tapProbeRunning;

void TapProbeThread(void* arg)
{
    (void)arg;
    LOG("tap probe: started\n");

    u64 lastSeq = 0, wakes = 0, gaps = 0, records = 0;
    u64 lastLogTick = armGetSystemTick();

    while (g_tapProbeRunning) {
        if (!TapClientIsConnected()) {
            Result rc = TapClientConnect();
            if (R_FAILED(rc)) {
                svcSleepThread(2E+9);  // vi-tap not running (yet)
                continue;
            }
            LOG("tap probe: [%lus] connected to uvctap\n", armTicksToNs(armGetSystemTick()) / 1000000000);
            lastSeq = 0;
            // log the first record at once instead of 10 s later, so a short
            // run still shows whether records come through
            lastLogTick = armGetSystemTick() - armNsToTicks(10000000000ULL);
        }

        Event* ev = TapClientPresentEvent();
        if (R_SUCCEEDED(eventWait(ev, 1E+9))) {
            eventClear(ev);
            wakes++;
        }

        UvcTapPresent p;
        Result rc = TapClientGetLatestPresent(&p);
        if (R_FAILED(rc)) {
            // vi-tap gone: should never happen (it is not toggleable), but
            // drop the session and retry rather than spin on a dead one
            LOG("tap probe: GetLatestPresent 0x%x - reconnecting\n", rc);
            TapClientClose();
            continue;
        }
        if (p.seq != lastSeq) {
            if (lastSeq != 0 && p.seq > lastSeq + 1)
                gaps += p.seq - lastSeq - 1;  // presents we slept through
            records++;
            lastSeq = p.seq;
        }

        u64 now = armGetSystemTick();
        if (armTicksToNs(now - lastLogTick) >= 10E+9) {
            UvcTapStats s = {0};
            TapClientGetStats(&s);
            u64 ageMs = p.seq ? armTicksToNs(now - p.tick) / 1000000 : 0;
            LOG("tap probe: [%lus] seq %lu (+%lu read, %lu skipped, %lu wakes) pid %lu prog %016lx "
                "slot %d mask 0x%x fences %u xf 0x%x crop %d,%d-%d,%d age %lums | "
                "app %lu disp %lu binder %lu parse-fail %lu other %lu\n",
                armTicksToNs(now) / 1000000000, p.seq, records, gaps, wakes, p.process_id, p.program_id,
                p.slot, p.slot_mask, p.fence_count, p.transform,
                p.crop[0], p.crop[1], p.crop[2], p.crop[3], ageMs,
                s.app_pid, s.displays_wrapped, s.binders_wrapped,
                s.parse_failures, s.other_presents);
            records = gaps = wakes = 0;
            lastLogTick = now;
        }
    }

    TapClientClose();
    LOG("tap probe: exiting\n");
}

#endif
