#pragma once
// The streaming memory, borrowed from the Applet pool through the donor
// program (protocol: include/uvcdonor.h; docs/NATIVE-CAPTURE.md §7).
//
// Kill safety: if this process dies with the memory mapped, the kernel does
// NOT give the pages back to the donor (KTransferMemory::Finalize unlocks only
// after an unmap - found on hardware, phase 2b-1). The donor watches our pid
// and exits when we are gone, which frees them.
// What nvservices does with buffers it mapped from this memory is the open
// question of phase 2b (§6).
#include <switch.h>

// Launches the donor and maps `mb` MB of the Applet pool (or the largest it
// grants). Blocks up to ~5 s. Refuses when a web applet runs or the Applet
// pool would keep less than its margin free.
bool DonorObtain(u32 mb, void** addr, u64* size);

// nvdrv's transfer memory from the same donor (256 KB), for nvdrv Initialize.
// The caller takes ownership of the handle.
bool DonorTakeNvTmem(Handle* out, u64* size);

// Unmaps and closes: the donor gets its memory back and exits within ~5 ms.
// Terminated as a backstop if it is still there after 1 s.
void DonorRelease(void);

bool DonorActive(void);

// True when the memory should go back now: a web applet opened, or the
// Applet pool is within its margin of full. A few pm:dmnt calls: call it
// about once a second, not per frame.
bool DonorShouldYield(void);

// Applet group physical memory (Atmosphère pm:dmnt extension), MB; false if
// unknown.
bool DonorAppletGroupMb(u64* used, u64* limit);

// A donor left over by an earlier instance (should not happen: it exits on
// its own) - terminated at startup.
void DonorCleanupStale(void);
