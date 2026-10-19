// uvcdonor - how the memory donor lends SysDVR-UVC its streaming memory
// (docs/NATIVE-CAPTURE.md §7). Plain C, shared by both sides.
//
// SysDVR-UVC lives in the System pool, which has a few MB for every
// sysmodule together. The memory for frames and engines is in the Applet
// pool, but anything resident there while idle breaks the browser applets
// (manuals, news, eShop: 2210-1024). So a separate program, the donor, is
// launched only while capture needs memory:
//
//   SysDVR-UVC registers UVCDONOR_SERVICE_NAME, launches the donor (pm:shell)
//   donor -> GetSizeMb()              how much to take
//   donor    takes it (svcSetHeapSize, Applet pool), makes two transfer
//            memories: the capture region and nvdrv's
//   donor -> Give(size, nv_size) + 2 copy handles, closes its own handles
//   donor    polls svcSetHeapSize(0): refused while the memory is lent,
//            succeeds once SysDVR-UVC has unmapped and closed it, and the
//            donor exits within ~5 ms. It also watches SysDVR-UVC's pid
//            (pm:dmnt, every 100 ms): if SysDVR-UVC dies with the memory
//            still mapped, the kernel never unlocks the donor's pages
//            (KTransferMemory::Finalize unlocks only after an unmap), so the
//            donor exits instead - its memory is freed with it.
//            (pm:dmnt GetProcessId is sent as command 2 directly: libnx's
//            wrapper, without the firmware version, sends
//            HookToCreateProcess and freezes SysDVR-UVC's next launch.)
//
// After SFT's tier4/donor (GPL-2.0, Tomas Uribe).
#pragma once

#define UVCDONOR_SERVICE_NAME "uvcdonr"
#define UVCDONOR_PROGRAM_ID   0x00FF0000A53BB668ull

enum {
    UvcDonorCmd_Give      = 0,  // in { u64 size; u64 nv_size; } + copy handles: capture tmem, nvdrv tmem
    UvcDonorCmd_GetSizeMb = 1,  // () -> u32 MB wanted for the capture region
    UvcDonorCmd_Failed    = 2,  // in u32 result: the Applet pool refused every size
};

#define UVCDONOR_NV_SIZE 0x40000  // nvdrv's transfer memory (libnx's default is 3 MB; SFT uses 256 KB)
