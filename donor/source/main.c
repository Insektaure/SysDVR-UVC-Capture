// SysDVR-UVC memory donor. Protocol and rationale: include/uvcdonor.h.
//
// Launched by SysDVR-UVC through pm:shell, never at boot (no boot2.flag).
// Lives in the Applet pool and the Applet resource-limit group; holds
// nothing but its own image until it is asked, and nothing at all once its
// memory comes back.
//
// After Switch Frame Tap's tier4/donor/source/main.c (GPL-2.0, Tomas Uribe).
#include <switch.h>
#include "uvcdonor.h"

#define SYSDVR_UVC_PROGRAM_ID 0x00FF0000A53BB666ull

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

// libnx's own heap stays tiny and static, so svcSetHeapSize is ours alone
#define INNER_HEAP_SIZE 0x4000
static char g_innerHeap[INNER_HEAP_SIZE];

void __libnx_initheap(void)
{
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = g_innerHeap;
    fake_heap_end = g_innerHeap + sizeof(g_innerHeap);
}

void __appInit(void)
{
    if (R_FAILED(smInitialize()))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
}

void __appExit(void)
{
    smExit();
}

// pm:dmnt GetProcessId, sent as command 2 (firmware 5.0.0+) directly. NOT
// libnx's pmdmntGetProcessId: it picks the command from the firmware version,
// which this program never tells libnx (no setsys, to stay tiny), so it sends
// the pre-5.0 id - HookToCreateProcess on current firmware. That hooked the
// next launch of SysDVR-UVC: pm created it and held it, never started, until
// a reboot (found on hardware, phase 2b-1; SFT's launcher warns of the same).
static bool _pidOf(u64 programId, u64* pid)
{
    *pid = 0;
    return R_SUCCEEDED(serviceDispatchInOut(pmdmntGetServiceSession(), 2, programId, *pid)) && *pid != 0;
}

// Atmosphère sm AtmosphereHasService: smGetService on a name that is not
// registered waits forever (sm defers it), and SysDVR-UVC may have died
// between launching us and now
static bool _registered(const char* name)
{
    u8 has = 0;
    SmServiceName sn = smEncodeName(name);
    return R_SUCCEEDED(tipcDispatchInOut(smGetServiceSessionTipc(), 65100, sn, has)) && (has & 1);
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    bool there = false;
    for (int i = 0; i < 50 && !(there = _registered(UVCDONOR_SERVICE_NAME)); i++)
        svcSleepThread(100000000ULL);
    if (!there)
        return 1;  // nobody to lend to: exit holding nothing

    // the borrower, to watch: see the loop at the end
    u64 borrower = 0;
    if (R_FAILED(pmdmntInitialize()))
        return 1;
    if (!_pidOf(SYSDVR_UVC_PROGRAM_ID, &borrower)) {
        pmdmntExit();
        return 1;
    }

    Service ctl;
    if (R_FAILED(smGetService(&ctl, UVCDONOR_SERVICE_NAME)))
        return 1;
    smExit();  // sm only for this: each session counts in the Applet group (6 for every applet)

    u32 wantMb = 0;
    if (R_FAILED(serviceDispatchOut(&ctl, UvcDonorCmd_GetSizeMb, wantMb)) || wantMb < 4 || wantMb > 64) {
        serviceClose(&ctl);
        return 1;
    }

    // the largest the Applet pool grants, in the heap's 2 MB steps
    void* addr = NULL;
    u64 size = 0;
    for (u32 mb = (wantMb + 1) & ~1u; mb >= 4; mb -= 2) {
        if (R_SUCCEEDED(svcSetHeapSize(&addr, (u64)mb << 20))) {
            size = (u64)mb << 20;
            break;
        }
    }

    Handle tmem = INVALID_HANDLE, nvTmem = INVALID_HANDLE;
    Result rc = size > UVCDONOR_NV_SIZE ? 0 : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    // the top 256 KB is nvdrv's: transfer memory cannot be made from memory
    // that is itself lent, so SysDVR-UVC cannot cut it from the region
    if (R_SUCCEEDED(rc))
        rc = svcCreateTransferMemory(&tmem, addr, size - UVCDONOR_NV_SIZE, Perm_None);
    if (R_SUCCEEDED(rc))
        rc = svcCreateTransferMemory(&nvTmem, (u8*)addr + size - UVCDONOR_NV_SIZE, UVCDONOR_NV_SIZE, Perm_None);

    if (R_SUCCEEDED(rc)) {
        const struct { u64 size, nv_size; } in = { size - UVCDONOR_NV_SIZE, UVCDONOR_NV_SIZE };
        serviceDispatchIn(&ctl, UvcDonorCmd_Give, in, .in_num_handles = 2, .in_handles = { tmem, nvTmem });
    } else {
        const u32 urc = rc;
        serviceDispatchIn(&ctl, UvcDonorCmd_Failed, urc);
    }
    // SysDVR-UVC holds its own copies: the memory stays lent until it closes them
    if (tmem != INVALID_HANDLE)
        svcCloseHandle(tmem);
    if (nvTmem != INVALID_HANDLE)
        svcCloseHandle(nvTmem);
    serviceClose(&ctl);

    // Shrinking the heap is refused while the pages are lent. Once SysDVR-UVC
    // unmaps and closes them, the next try succeeds and the memory is back in
    // the Applet pool within ~5 ms.
    //
    // If SysDVR-UVC is killed (ovlSysmodules) while the memory is MAPPED, the
    // kernel closes its handle but never unlocks our pages: mesosphere's
    // KTransferMemory::Finalize unlocks only when the borrower had unmapped
    // (kern_k_transfer_memory.cpp). The shrink would then fail forever. So
    // watch the borrower too: gone, or replaced by a new instance, and we
    // exit - a dead process's memory is freed, locked or not.
    for (u32 i = 0; size != 0; i++) {
        if (R_SUCCEEDED(svcSetHeapSize(&addr, 0)))
            break;
        if (i % 20 == 19) {  // every ~100 ms
            u64 pid = 0;
            if (!_pidOf(SYSDVR_UVC_PROGRAM_ID, &pid) || pid != borrower)
                break;
        }
        svcSleepThread(5000000ULL);
    }
    pmdmntExit();
    return 0;
}
