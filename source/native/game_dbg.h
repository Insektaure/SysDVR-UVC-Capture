#pragma once
// Reading the running game's memory (docs/NATIVE-CAPTURE.md §5.2, §8).
//
// The kernel allows ONE debugger per process. Direct svcDebugActiveProcess
// first; when that fails and Atmosphère's cheat engine already holds the
// game (cheats loaded for it), read through dmnt:cht instead - slower, but
// cheats keep working. Never ForceOpenCheatProcess: a game that runs
// debugger-free stays that way.
//
// Kill safety: if this process dies, the kernel closes the debug handle,
// which detaches and resumes the game. The dmnt:cht session is dmnt's
// business; we only read through it.
//
// Follows Switch Frame Tap's applet_mitm_dmdbg.cpp and
// applet_mitm_dbgpump.cpp (GPL-2.0, Tomas Uribe).
#include <switch.h>

typedef struct {
    Handle handle;  // direct attach; INVALID_HANDLE when reading through dmnt
    bool   dmnt;
    u64    pid;
} GameDbg;

// Direct attach, else dmnt:cht if it holds this game. *direct_rc: the direct
// attach's result, for the log.
bool GameDbgAttach(u64 pid, GameDbg* out, Result* direct_rc);

// Through dmnt: the cheats it has loaded for the game, -1 if unknown. 0 means
// dmnt was force-opened (a cheat overlay such as EdiZon, or a tool), not
// attached at launch for a cheat file: at launch it attaches only when the
// game's cheats/<build id>.txt exists (dmnt_cheat_api.cpp).
s64 GameDbgDmntCheatCount(GameDbg* d);

// Drain the attach burst and resume the game (no-op through dmnt, which
// pumps its own handle). *nev: events drained.
Result GameDbgResume(GameDbg* d, u32* nev);

// Through dmnt the destination is an IPC buffer, and the kernel refuses pages
// pinned for a device (nvmap) as one (SFT: the read fails, small stack reads
// work). With a bounce buffer set, dmnt reads go through it in pieces.
void GameDbgSetBounce(void* buf, size_t size);
bool GameDbgRead(GameDbg* d, void* dst, u64 va, size_t len);
bool GameDbgQuery(GameDbg* d, MemoryInfo* mi, u64 addr);

// Stops the pump first if it runs on this handle, then closes it.
void GameDbgDetach(GameDbg* d);

// ---------------------------------------------------------------------------
// The debug event pump.
//
// While a debugger is attached, every thread start and exit in the game
// pushes a debug event, and pushing one suspends ALL the game's threads until
// the debugger continues (mesosphere KDebugBase::ProcessDebugEvent). Loading
// screens start and stop worker threads: without a pump the game freezes on
// the first one (SFT Run I). Like dmnt's cheat engine, the pump drains and
// continues, on the core of the thread just created ("prevents a kernel
// deadlock") through helper threads on cores 0-2.
//
// Real exceptions are left to the game (not marked handled): a crash stays a
// crash, and a game that handles its own faults still gets them.
// ---------------------------------------------------------------------------

typedef struct {
    u32 wakes, events, created, exited, other, faults;
    u32 continues, busy, failed, last_fail_rc;
    u32 per_core[4];
    u64 max_hold_ns;  // longest wake -> continued: our share of a game pause
    bool gone;        // the game exited, or the handle stopped working
} DebugPumpStats;

void DebugPumpStart(Handle dbg);
void DebugPumpStop(void);
bool DebugPumpGone(void);
void DebugPumpGetStats(DebugPumpStats* out);
