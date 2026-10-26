# SysDVR-UVC - Native Capture Design (draft)

Design for replacing grc:d as the video source with a native frame tap
(game memory -> VIC -> NVENC), while keeping the sysmodule toggleable on the
fly with ovlSysmodules. Nothing here is implemented yet.

Each claim is marked as either **established** (measured, by us or by Switch
Frame Tap, with a reference) or **hypothesis** (reasoned, to be verified on
hardware before anything depends on it).

Reference project: [Switch Frame Tap](https://github.com/TomasUribe/switch-frame-tap)
(GPL-2.0, read at commit `a0e1714`), referred to as **SFT** below. Its UVC layer
is derived from this repository; its capture pipeline is what this document
plans to reuse. Paths like `tier4/...` are in that repository.

---

## 1. Goals and non-goals

**Goals**

- Video source independent of grc:d: the game's own frames, read from its
  memory, encoded by our own NVENC session.
- 720p60 and 1080p60 H.264 over the existing UVC device.
- Later: an uncompressed NV12 camera, so Media Foundation apps (Windows Camera,
  browsers, Teams) can open it - impossible with grc:d (H.264 only).
- **The capture sysmodule stays toggleable with ovlSysmodules, at any time,
  without crashing the running game or the console.** This is the hard
  constraint that shapes the design.
- Works alongside Atmosphère's cheat engine (dmnt.gen2).
- grc:d remains as a fallback source.

**Non-goals**

- SFT's PC viewer, network transport, audio, screenshots, overlay, manager
  app, HOME-menu/applet streaming, ReverseNX integration.
- Audio over USB (still impossible, RESEARCH.md §6).

## 2. Why grc:d limits us (established)

- grc:d hands out the system recorder's H.264: 1280x720, 30 fps, fixed
  keyframe interval (~2 s to first picture, README "Usage").
- Games that disable video capture produce no stream.
- H.264 only: no MJPEG/uncompressed format for Media Foundation apps.

## 3. Why the native tap needs a vi mitm (established, SFT README "How it works")

SFT tried three routes through the graphics stack and closed each one:

| route | verdict |
|---|---|
| import the game's swapchain nvmap handle | pins to `phys=0`: nvservices maps client memory through the handle of the owning process |
| vi indirect layers (`GetIndirectLayerImageMap`) | `0x60A`: attaching an application layer to an indirect layer is AM's job |
| read back the display controller | no such nvdrv ioctl |

The route that works: `svcDebugActiveProcess` + `svcReadDebugProcessMemory`
on the game (~5 ms for a 1080p slot direct). But reading memory is not enough:
we must know **which swapchain slot was just presented** and **when the GPU
finished it**. That information only exists in the game's binder traffic:
`IHOSBinderDriver::TransactParcelAuto` (cmd 3), `queueBuffer` (code 7)
carries the slot, the `NvMultiFence` (up to 4 syncpoint id/value pairs), the
transform and the crop (`tier4/applet-mitm/source/applet_mitm_service.cpp`).
To see it, SFT mitms `vi:u` (games) and `vi:m` (homebrew, application process
only).

The parcels carry **no buffer addresses**: SFT finds each slot's memory by
matching the slots presented against memory regions that change
(`applet_mitm_slotmatch.hpp`). That is reader-side work.

## 4. The core problem: a mitm cannot be killed

**Hypothesis (not tested by SFT, follows from how sm mitm works):** once a
game's vi:u session is routed through a mitm, that session's server is the
mitm process. If ovlSysmodules terminates that process
(`pm:shell TerminateProgram`, no graceful shutdown), the game's display
session dies with it and the game's next present fails - most likely a crash
or fatal.

SFT itself does not support turning the module off at runtime: its README
"If something goes wrong" says to disable "Start with the console" and
reboot; its overlay "Stream" toggle only stops streaming inside a resident
process.

Therefore the mitm and the capture cannot live in the same process if the
capture must be killable.

## 5. Architecture: two processes

```
                 game (application process)
                   |  vi:u / vi:m  (binder: queueBuffer)
                   v
  +--------------------------------------+
  | A. vi-tap  (always on, NOT toggled)  |
  |  - forwards every call unchanged     |
  |  - records the last present          |
  |  - serves "uvctap" (IPC)             |
  +--------------------------------------+
                   ^  GetLatestPresent / present event
                   |
  +--------------------------------------+
  | B. SysDVR-UVC  (toggled by           |
  |    ovlSysmodules, 00FF0000A53BB666)  |
  |  - debug attach + event pump         |
  |  - slot match, memory read           |
  |  - VIC: RGBA -> NV12                 |
  |  - NVENC: H.264                      |
  |  - UVC device (existing code)        |
  |  - fallback: grc:d                   |
  +--------------------------------------+
                   |  usb:ds bulk
                   v
                  PC
```

### 5.1 A - `vi-tap`

- libstratosphere mitm (as SFT), started at boot (`boot2.flag`) so it sees
  every game from launch.
- Mitms `vi:u` and `vi:m` (application process only, as SFT's `ShouldMitm`).
  Declares only `GetDisplayService` (0), `GetDisplayServiceWithProxyNameExchange`
  (1, raw forward), `GetRelayService` (100) and `TransactParcelAuto` (3); every
  other command auto-forwards. **Never** declare `OpenLayer` (SFT: declaring it
  made games fail to launch).
- On `queueBuffer`: parse the parcel, store a present record under a short
  lock, signal the event, then return `ResultShouldForwardToSession` so
  libstratosphere forwards the request unchanged. No IPC, no SD logging, no
  allocation in the handler (logging is IPC too and would clobber the TLS
  message buffer the reply still needs - SFT v0.7.10).
- Also records which slots `setPreallocatedBuffer` names (`slot_mask`) to
  help B's slot matching.
- Serves a small IPC service `uvctap` (protocol: `include/uvctap.h`):
  - `GetVersion() -> u32`
  - `GetPresentEvent() -> handle` (readable event, manual clear, signalled
    on each present of the application)
  - `GetLatestPresent() -> UvcTapPresent` (96 bytes: seq, pid, program id,
    tick, slot, fences, transform, crop, slot mask)
  - `GetStats() -> UvcTapStats`
  - Shared memory instead of IPC polling only if IPC at 60 Hz proves too
    costly (it should not).
- **System resource-limit group**, not Applet: the Applet group allows 6
  sessions for every applet together, and each wrapped sub-object costs a
  session pair; SFT, living in the Applet group, left the keyboard applet
  none (`2001-0132`) and needed a second libstratosphere patch to get
  sessions through sm. vi-tap has nothing that needs Applet memory.
- **Holds no state about B.** A client disappearing (B killed) closes its IPC
  session; A must not care.
- `toolbox.json` absent (or `"requires_reboot": true`), so ovlSysmodules does
  not offer to kill it.
- Memory: System group. Image measured at build: ~490 KB (`.text` 205 KB,
  `.bss` 248 KB of which 64 KB heap and ~72 KB thread stacks), against SFT's
  1.8-3.4 MB whose boot-time cost caused fatal `2001-0132` on consoles with
  many sysmodules (`tier4/donor/source/launcher.c` header). The used memory
  on hardware is still to be read (phase 1 test).
- Another vi:u mitm (SFT) makes the mitm install fail: vi-tap logs it and
  keeps running without it rather than fatal at boot.

### 5.2 B - SysDVR-UVC (this sysmodule)

Source selection at stream start (each UVC commit):

1. `uvctap` registered and a present seen for the current application ->
   native path.
2. Otherwise -> grc:d path (current code, unchanged).

Native path, per stream session:

1. **Attach**: `svcDebugActiveProcess(pid)`. If refused and
   `dmnt:cht HasCheatProcess` (65000) says dmnt holds the game, read through
   `dmnt:cht ReadCheatProcessMemory` (65102) instead. Never
   `ForceOpenCheatProcess` (SFT PR #8).
2. **Debug event pump** thread: block on the debug handle, drain events,
   `ContinueDebugEvent(ExceptionHandled | ContinueAll)`, continue on the new
   thread's core after a `CreateThread` event (dmnt's pattern, SFT
   `applet_mitm_dbgpump.cpp`). Without it, any thread start in the game
   freezes all its threads (established: SFT's first stream froze on a
   loading screen).
3. **Slot match**: find each slot's buffer address (SFT `slotmatch`), using A's
   present records.
4. **Per frame**: wait on `GetPresentEvent`, take the newest record, wait for
   its fence, read the slot into our staging buffer, VIC -> NV12 (BT.709
   limited), NVENC -> H.264, hand the access unit to the UVC sender.
5. **Stop** (UVC stall/disconnect/new commit, or the app exits): close the
   debug handle (game released), free engine buffers, shrink the heap.

UVC side changes (see the previous review of SFT's `applet_mitm_uvc.cpp`):

- Frames 1920x1080 and 1280x720 at 60 fps (H.264 frame-based); B encodes the
  committed size whatever the game draws (VIC scales).
- **Per-interface configuration data cap of 128 bytes per speed**
  (established by SFT Run AH: 130 B refused with `0xcc8c`). Two frame
  descriptors do not fit with our colour-matching descriptor: drop it (the
  H.264 VUI carries colour; hosts default to BT.709). Add a `static_assert`.
- EP0 on its own thread, sender on its own thread (SFT's split).
- Host-stall detection: 3 consecutive dropped frames = host stopped reading;
  probe once per second with an empty 2-byte payload; force an IDR on resume
  (now possible: we own the encoder).
- NV12 camera later as a separate mode (own serial and `bcdDevice` so hosts
  do not reuse cached formats): 1280x720 @ 25, 768x432 @ 60 (USB 2.0
  ~37 MB/s, SFT Run AS).

## 6. Kill safety of B

ovlSysmodules terminates B with no warning. Every resource B holds must be
released correctly by the kernel or the owning service alone.

| resource | on kill | status |
|---|---|---|
| IPC session to A | closed by the kernel; A drops it | design requirement on A |
| debug handle on the game | closed by the kernel; process detached and resumed | **hypothesis** - verify, including with a debug event pending |
| `dmnt:cht` session | closed; dmnt unaffected (B never force-opens) | hypothesis |
| usb:ds | closed; device disappears from the host | established (current behaviour) |
| grc:d | closed | established (current behaviour) |
| nvdrv session, VIC/NVENC channels | nvservices tears down the session | **main risk** - a job may be in flight |
| heap / staging memory | freed by the kernel | **risk** - memory may still be mapped for the engines |
| donor's transfer memory, still mapped | **NOT returned to the donor** (established, phase 2b-1): mesosphere's `KTransferMemory::Finalize` unlocks the lender's pages only if the borrower had unmapped them. The donor would hold its memory forever | fixed: the donor watches B's pid (pm:dmnt, 100 ms) and exits when B is gone or replaced; a dead process's memory is freed, locked or not. B also terminates a stale donor at start (backstop, worked on hardware) |
| (bug found while testing that fix) | libnx's `pmdmntGetProcessId` picks its command id from the firmware version; a program that skips setsys (the donor) sends the pre-5.0 id, which is `HookToCreateProcess` today. The donor hooked SysDVR-UVC's next launch: pm created it and held it with 0 threads, ovlSysmodules showed it ON and could not stop it, until a reboot | every pm:dmnt GetProcessId is sent as command 2 directly (donor and B) |

The nvdrv row is the one most likely to fail. Related evidence: SFT v0.7.13.1
- after a homebrew app exited, its freed buffers left device address 0 open, a
later slot copy mapped there, and the VIC refused every session until reboot;
a zero address handed to the VIC hangs the engine and takes the compositor
down with it. Mitigations to design in:

- Small engine jobs (one frame), never a long queue: a kill can interrupt at
  most one job.
- A guard page at device address 0 (SFT v0.7.13.1 fix) and address checks
  before every submit.
- Never import or keep the game's graphics resources (SFT: adopting the
  game's aruid or buffer handle kept an exited game's memory alive and the
  next launch hung on a black screen).

If kill-during-encode turns out to be unsafe and cannot be fixed, the
fallback is a soft stop requested over IPC before termination - but
ovlSysmodules cannot send one, so that would mean our own overlay. Treat it
as the last resort.

## 7. Memory

- A 1080p RGBA slot is 7,913 KB; SFT's streaming needs ~42 MB, which only the
  Applet pool has (established, SFT STATUS M55 and `applet_mitm_donor.hpp`).
- Two independent gates (established, SFT M54/M55): the physical pool
  (`pool_partition`) **and** the resource-limit group (`application_type`).
  Moving only the pool still spends the System group, which `am` needs
  (fatal `0x10801 LimitReached`).
- Plan: B declares `pool_partition: 1` and `application_type: 2` (Applet pool
  and group). Its heap stays minimal while idle and grows only for a stream
  session (`svcSetHeapSize`), shrinking at stop. Killing B returns everything.
- Browser applets (manuals, news, eShop) need nearly the whole Applet pool
  (SFT v0.7.10). While B streams they may fail to open; B must stop and
  release when one starts, or we document "turn the sysmodule off first".
- **Answered (SFT STATUS v0.7.10):** SFT left the Applet group because
  *any* memory held resident in the Applet pool broke the browser applets
  (`2210-1024`): even a trimmed 2.9 MB did, and Atmosphère 1.12.0's changelog
  reports the same past 7 MB on firmware 23. The Applet group also allows
  only 6 sessions for all applets together. So: **B stays in the System
  pool and group (as today)**, and the streaming memory comes from a
  **donor** process (phase 2b): launched through `pm:shell` only while a
  stream runs, it takes the memory in the Applet pool and lends it to B as
  transfer memory. SFT's donor polls `svcSetHeapSize(0)`, refused while its
  memory is lent, and exits ~5 ms after the lend ends. If B is killed, its
  transfer-memory handle closes with it, so the donor should get its memory
  back and exit on its own - **to verify on hardware**, together with what
  nvservices does with buffers mapped from that memory (§6).
- Phase 2a needs no donor: slot finding reads 64-byte samples, throughput
  reads go through a 128 KB static buffer, and a dump takes a heap of the
  buffer's size (4 MB at 720p) for the instant of the read, only when the
  System group keeps 8 MB free after it, and gives it back at once.

## 8. Cheats

- dmnt attaches at game launch only if a cheat file loads for that title
  (established, SFT STATUS `dmnt_cheat_api.cpp:783` analysis).
- No cheats for the game: B attaches directly (~1 GB/s reads).
- Cheats loaded: B reads through `dmnt:cht` (~190 MB/s, established SFT
  v0.7.9). 720p RGBA at 30 fps needs ~110 MB/s (fits); 720p60 ~220 MB/s
  (likely not); 1080p60 drops to 20-25 fps (SFT README).
- B attached first, then a tool attaches later (dmnt force-open, cheat search
  in EdiZon/Breeze): the tool fails while B streams. Turning B off in
  ovlSysmodules releases the game - this design makes that the documented fix.
- B's NPDM needs `debug_flags` with `"force_debug": true` (as creport and
  dmnt.gen2); the syscall bits alone are not enough (established, SFT M33/M34).
  `svcWriteDebugProcessMemory`, `svcBreakDebugProcess` and
  `svcTerminateDebugProcess` stay undeclared.

## 9. Compatibility

- SFT's pipeline is tested on firmware 21-23 and Atmosphère 1.11-1.12. Our
  grc:d path supports HOS 5.0+. The native path inherits SFT's range; grc:d
  remains the fallback everywhere else.
- A is built against libstratosphere and must be rebuilt per Atmosphère
  release. B can stay on libnx. A is pinned to **Atmosphère 1.12.1**, which
  needs libnx newer than the 4.12.0 release (`fsStorageForBatchRead*`, libnx
  PR #741): `tools/docker/Dockerfile` adds libnx from a pinned git commit to
  `devkitpro/devkita64`. A does not call `ams::CheckApiVersion()`, so an
  older Atmosphère does not fatal at boot; whether it works there is untested.
- Incompatible with SFT and the original SysDVR sysmodule (vi mitm, grc:d,
  usb:ds contention).

## 10. Test plan and acceptance criteria

Every phase is gated on hardware results; log evidence goes into RESEARCH.md.

| test | pass criterion |
|---|---|
| A alone, 10 games incl. homebrew, 30 min each | no launch failure, no frame-rate change, present records for every game |
| kill/start B (dummy reader) during a game, 50 cycles | no crash, game unaffected, A keeps serving |
| kill B while attached, incl. during loading screens | game resumes, no freeze |
| kill B during VIC/NVENC jobs, 50 cycles | next start works without reboot, no compositor hang |
| B with cheats loaded | stream runs through dmnt:cht, cheats still active |
| B off -> cheat search tool attaches | tool works |
| browser applet (manual) while streaming / while B off | opens, or documented limitation |
| boot with many sysmodules | no `2001-0132` fatal, A's System group cost measured |
| UVC: Linux uvcvideo, Windows OBS/ffplay, macOS QuickTime | 720p60 and 1080p60, 0 decode errors |

## 11. Phases

1. **Spike: A + dummy B.** A publishes presents; B only reads records and
   logs. Validates kill safety of the split and A's overhead. Go/no-go for
   the whole design.
2. **Attach + read + VIC**, in two steps:
   - **2a** - attach (direct, else dmnt:cht), debug event pump, slot
     finding and matching, whole-slot reads, raw dumps to SD. No GPU, no
     donor. Kill tests while attached.
   - **2b** - donor memory, fence wait, VIC to NV12. Kill tests during VIC
     jobs.
3. **NVENC + UVC.** H.264 720p60/1080p60 to the existing UVC device, EP0 and
   sender threads split, stall detection, IDR on resume. Kill tests during
   encode.
4. **NV12 camera** for Media Foundation apps.

### Phase 1: what is implemented

| piece | where |
|---|---|
| protocol | `include/uvctap.h` |
| A, vi-tap | `vi-tap/source/`, npdm `vi-tap/vi-tap.json`, `vi-tap/flags/boot2.flag` |
| libstratosphere patch | `tools/patch_libstrat.py` |
| toolchain image | `tools/docker/Dockerfile` |
| A's build | `tools/build-vi-tap.sh` -> `vi-tap/vi-tap.nsp` |
| dummy B | `source/tap_client.c` (`TapProbeThread`), started from `source/sysmodule/main.c` when built with `-DTAP_PROBE` |

Build:

```bash
tools/build-vi-tap.sh                                   # first run: ~10+ min (libstratosphere)
make DEFINES="-DFILE_LOGGING -DTAP_PROBE"               # B, spike variant (devkitPro)
```

SD card:

```
atmosphere/contents/00FF0000A53BB667/exefs.nsp          <- vi-tap/vi-tap.nsp
atmosphere/contents/00FF0000A53BB667/flags/boot2.flag   (no toolbox.json: not toggleable)
atmosphere/contents/00FF0000A53BB666/exefs.nsp          <- exefs.nsp (spike variant)
atmosphere/contents/00FF0000A53BB666/toolbox.json, flags/  (as today)
```

Logs: `sdmc:/config/sysdvr-uvc/vi-tap.log` (A, previous boot in
`vi-tap.prev.log`), `sdmc:/logfile.txt` (B, `tap probe:` lines every 10 s).

Phase 1 procedure (go/no-go for the design):

1. Boot with A only (B disabled): play a game 30 min, a homebrew app, open
   the keyboard and the HOME menu. A's log must show `vi:u: mitm installed`,
   the application pid, and `presents ... (N/s)` at the game's frame rate
   with `parse fail 0`.
2. Enable B in ovlSysmodules during a game: B's log shows `connected to
   uvctap` and `seq` advancing with `age` under ~50 ms.
3. Toggle B off/on in ovlSysmodules 50 times during the game, then between
   games. Pass: no crash, no game slowdown, A's `presents` keeps counting,
   B reconnects every time.
4. Launch/close games 20 times with A running (SFT's vi session leak showed
   at ~15 launches).

### Phase 2a: what is implemented

| piece | where |
|---|---|
| protocol v2: swapchain geometry (`GetSurface`) | `include/uvctap.h`; vi-tap parses `setPreallocatedBuffer`'s NvGraphicBuffer (`vitap_present.cpp`) |
| attach, dmnt:cht fallback, event pump | `source/native/game_dbg.c` |
| slot matcher (C port of SFT's, host test passes: 15 scenarios) | `source/native/slotmatch.c`, `tools/test_slotmatch.c` |
| the session: find, match, read, dump | `source/native/native_probe.c` (`-DFILE_LOGGING -DNATIVE_PROBE`) |
| dump -> PNG | `tools/deswizzle.py` |

B's npdm now allows cores 0-3: the pump continues a new game thread's
event on that thread's core, through helpers on cores 0-2 (dmnt does the
same: "prevents a kernel deadlock").

Not in 2a: the present's GPU fence is not waited on (that needs nvdrv,
phase 2b). Samples and reads start 8 ms after the present, and a dump whose
signature changed during the read is counted torn and not saved.

Phase 2a procedure (needs a reboot: vi-tap v2):

1. Boot, launch a game **after** boot (vi-tap must see it register its
   buffers), play. `logfile.txt` should show `native: game ...` with the
   geometry, `attached directly`, then either `slots ... from the region of
   the swapchain's exact size` or `matched after N intervals`, then 3
   `dump N: ... saved` and every 10 s a line with reads (MB/s) and pump
   counters.
2. Turn each dump into a PNG on the PC:
   `python3 tools/deswizzle.py <program>-0.bin`. It must show the game.
3. Play through loading screens: the game must never freeze (the pump's
   `+N threads` grows, `longest hold` stays in microseconds).
4. **Kill test while attached**: toggle SysDVR-UVC off/on in ovlSysmodules
   ~30 times during the game, some during loading screens. Pass: no freeze,
   no crash, and each restart attaches again.
5. Cheats: with a cheat file for the game, launch it with cheats on:
   `attached through dmnt:cht`, dumps still correct, cheats still work.
6. Close the game: `session end (game exited)`.

### Phase 2b-1: the donor

| piece | where |
|---|---|
| protocol | `include/uvcdonor.h` (service `uvcdonr`, program `00FF0000A53BB668`) |
| donor program (Applet pool and group, no boot2 flag, no toolbox.json) | `donor/` (`make` in devkitPro) |
| SysDVR-UVC side: one-shot IPC server, launch, map, release | `source/native/donor.c` |

The probe borrows 16 MB when a game session starts, checks it (pattern
read back), and gives it back at session end, or as soon as a web applet
opens or the Applet pool comes within 48 MB of full. The release clears the
Uncached attribute, retries the unmap for up to 5 s (nvservices, from 2b-3
on), then waits up to 1 s for the donor to exit before terminating it.

Phase 2b-1 procedure:

1. Copy the donor to `atmosphere/contents/00FF0000A53BB668/exefs.nsp` and
   the new SysDVR-UVC; toggle SysDVR-UVC (no reboot).
2. Launch a game: the log shows `donor: launch`, `16 MB mapped`, the Applet
   group before and after (+16 MB), `donor memory 16 MB checked`.
3. Close the game: `memory unmapped`, `exited by itself`, Applet group back
   to where it was.
4. **Kill test**: in a game (memory held), toggle SysDVR-UVC off, then on.
   The new start must log `donor: none running at start` with the Applet
   group back at its idle value - the donor exited on its own when the
   kernel took the memory back.
5. In a game, open its manual (or the eShop): `giving the donor memory
   back`, and the manual opens.

### Phase 2b-3: the VIC

| piece | where |
|---|---|
| VIC 4.0 structs, methods, BT.709 matrix | `source/native/vic.h` (checked field by field against SFT's header; sizes asserted) |
| nvdrv client on the donor's transfer memory | `source/native/nvdrv.c` |
| engine: guard page, pins, config, command buffer, submit, syncpoint poll, fence wait | `source/native/vic_engine.c` |
| integration: fence wait per present, region copy, a conversion per present, NV12 dumps | `source/native/native_probe.c` |
| NV12 -> PNG | `tools/nv12topng.py` |

Donor memory layout: config 0x0, command buffer 0x4000, guard pages 0x8000,
NV12 output 0x10000 (1.38 MB), dmnt bounce 0x200000 (2 MB, not pinned),
slot copy 0x400000 (up to 11 MB). Output: NV12 1280x720 pitch-linear,
BT.709 limited; the source rect is the present's 16:9 crop from (0,0)
(handheld: 1280x720 of 1920x1080, copied as 6 x 640 KB), else the whole
surface scaled; the present's flip is applied.

A job that does not complete in 0.5 s marks the engine wedged: no more
jobs, and the nvdrv session is left open (closing a stuck channel took the
compositor down in SFT M74), so the donor memory stays pinned until the
process ends.

### Phase 3a: NVENC

| piece | where |
|---|---|
| grc's setups and SPS/PPS (720p, 1080p), generated by script, SFT's invariants re-checked | `source/native/nvenc_setups.h` |
| encoder: mm:u clock (checked through clkrst), msenc channel, arena pinned in msenc and VIC channels, IDR/P chain, status, bitstream | `source/native/nvenc_engine.c` |
| integration: VIC straight into the encoder's picture (16-row block-linear NV12), an encode per present, the first 600 frames to `<program>.h264` | `source/native/native_probe.c` |

Donor 24 MB: VIC area 12.5 MB, the 720p encoder arena 7.1 MB (SFT
Layout720 offsets), the frame buffer 1.5 MB (never in our own .bss: this
process lives in the System pool). Constant QP 20 for keyframes, 22 for P
frames, a keyframe every 60 frames, SPS/PPS in front of every frame.
Not yet: SFT's motion hint for P frames, pipelining, 1080p, the UVC side.

### Build options (native only, grc:d as an option)

| command | what the camera sends |
|---|---|
| `make` | **native capture** (default): the game's frames, VIC + NVENC, 720p at the game's frame rate (up to 60), only while a camera app streams. grc:d is never opened. Needs vi-tap and the donor installed. |
| `make CAPTURE=grcd` | the console's recorder (grc:d): 720p30, the 1.0.1 behaviour; no vi-tap or donor needed |
| `make DEFINES=-DFILE_LOGGING` | either of the above with the SD log (`sdmc:/logfile.txt`); the native build then also has its debug features (a whole-slot read every second without the encoder, a dump of the frame on screen when HOME is pressed) |

Native only means no fall-back: while the native path is not ready (the
first ~20-30 s after start, no game presenting, an applet holding the
Applet pool, an engine wedged) the camera sends nothing, and it resumes on a
keyframe. Switching to grc:d mid-stream would show up to ~2 s of broken
picture (grc's keyframe interval), and grc:d gives nothing in the cases
where it would matter most (games that block recording, HOME).

### Picking the present (vi-tap protocol v3)

vi-tap keeps the last 8 present records (`GetRecentPresents`, command 5,
newest first, map-alias out buffer). Among the presents since the last one
encoded, SysDVR-UVC encodes the newest whose GPU fence is already reached;
if none is, it waits for the oldest (the GPU finishes them in order) - SFT
M96. Measured before: a GPU-heavy game that queues frames ahead (01003ec027cc8000)
cost ~39 ms of fence wait per frame, ~12 fps. A light game is unchanged: the
previous present is already encoded, so the newest is the only candidate.
Through dmnt:cht, no rate cap any more (it was one encode per 33 ms): as fast
as the slower copies allow. A copy the game drew over is re-read once, else skipped (8 samples, one per block row). Pipelining: the next frame is copied (into a second slot copy, after the
encoder's buffers - donor 32 MB) while the VIC converts the current one; the
job in flight is collected, encoded and sent right after that copy, or at
once when no newer present is waiting (no added latency when we keep up).
Kept only where it measures faster: the capture is watched in 2 s windows
(the share of the game's presents encoded). When it falls under 95% of a game
presenting at 20+ fps, a 16 s trial alternates 2 s windows serial/pipelined
(ABBA order) and compares the average share of the steady windows (loading
screens and pauses do not count); pipelined is kept if at least 3% better
(relative). Again every 2 minutes, or after 10 s when fewer than 2 steady
windows per mode. With a job in flight, the next present's fence is waited
on for up to 20 ms (the VIC works meanwhile): in 01003ec027cc8000 the newest
present is usually still being drawn, and without that wait the copy almost
never overlapped the job (VIC 20 ms, of which the loop waited 18; 30 fps
either way). The log counts overlapped and lone jobs. (First version: a fixed trial 3 s into streaming - it ran
on loading screens and never saw game 3 running.) Before it, at
01003ec027cc8000 through dmnt:cht: copy ~10 ms + VIC ~18 ms (the compositor
shares the VIC) + encode ~1.6 ms = ~30 fps of the game's ~47.
Why the VIC is slow in GPU-heavy games (measured, 01003ec027cc8000): the VIC
syncpoint is shared with the compositor, so the gap between the submit's
fence and the syncpoint's value is work queued ahead of ours. Jobs with
nothing ahead took 0.6-2 ms; with compositor work ahead (95-99% of them)
17-41 ms - the compositor's VIC job waits on the game's GPU, ours behind it.
The VIC and EMC clocks never moved (422 / 1331 MHz). So a third mode: the CPU
converts (source/native/cpu_convert.c, NEON, on our core 3: the game has
0-2) straight into NVENC's 16-row block-linear input, BT.709 limited range,
chroma from the 2x2 average. Checked against the VIC on the console: Y within
1 (mean 0.03), UV mean 0.6 (single pixels up to ~90 on sharp colour edges:
the VIC filters chroma differently). ~12 ms a frame in that game (the frame
comes from memory the GPU keeps busy); 1280x720 rects without horizontal flip
only. The trial now alternates VIC, VIC pipelined and CPU windows (12, in a
mirrored order) and keeps the simplest mode unless another is at least 3%
better.
Measured in 01003ec027cc8000 through dmnt:cht: the trial chose the CPU every
time (93%/92%/82% of the game's frames against 51-76% for the VIC modes),
34-40 fps steady (VIC: 22-30): copy ~9.5 ms + CPU ~12.5 ms + encode ~1.6 ms.
The trial only starts after 3 windows in a row at a stable rate (within 20%),
and a trial window whose rate drifted 30% from the start's does not count (a
first trial during loading had chosen wrongly).
Fused read and conversion: the CPU route no longer uses a slot copy. Each
block row (640 KB for a 1280-wide rect) is read into the bounce buffer -
dmnt:cht writes it there with no copy of ours (GameDbgReadTo), a direct read
too - and converted at once while it is still in the cache. Before, dmnt:cht
wrote the bounce, we copied it to the slot copy (3.8 MB a frame), and the
conversion read all of it again from memory (the L2 is 2 MB). The torn check
keeps its 8 samples from the pieces as they pass and compares them with the
game's memory after the frame; a torn or failed frame marks the encoder's
picture bad, and the keep-alive does not re-send it.
Choosing the mode without switching (replaces the trials): a trial spent
~16 s of every 2 minutes in the weaker modes (visible drops, ~31 fps against
~36). The loop does one frame after the other on one core, so a frame costs
the sum of its steps; each step is a moving average measured on every frame
of the current mode, and the other modes' costs are probed beside it while
the capture is behind a steady game (<95% of its presents): in CPU mode a
VIC job into the VIC's own output every 3 s, not waited on - its cost is the
compositor queue in front of it, not the picture - and polled between the
CPU frame's pieces (a few ms of precision); on the VIC one CPU conversion of
the last frame every 3 s until 3 probes newer than 30 s are known. A probed
cost is the median of the last 5 fresh probes (3 let one lucky VIC probe
- the queue swings 1.5-40 ms - cause a switch the check had to undo): a first version averaged them
slowly, and one 50 ms CPU probe taken as a loading screen ended (11-15 ms
after) kept game 3 on the VIC for 2 minutes. Predicted
frame costs: VIC = read through the slot copy + VIC job + encode; CPU =
piecewise read (2/3 of the former through dmnt:cht, measured) + conversion +
encode; pipelined = the longer of read and VIC job + encode, or as measured
while pipelined - a candidate only where the CPU cannot convert. A mode
predicted 15% faster in two windows in a row is switched to; two windows
later its share of the game's presents is compared with the one before, and
a 3% (relative) worse share goes back, the mode not tried again for 20 s,
doubled at each failure in a row up to 160 s (a load spike failed a good
mode once: with a fixed 60 s, game 3 spent 1.5 minutes on the VIC).
The VIC queue diagnostic is gone (its finding is above).
Where a CPU frame's time goes (logging-build diagnostic, game 3 with cheats,
CPU at 1020 MHz): ~14 ms of wall clock for the conversion, of which our
thread ran only ~7.9 ms - other threads on core 3 (system services, dmnt)
preempt it at our priority 0x3B. Flush ~0.25 ms. The last piece converted
again while in the cache costs the same as in the frame, tiled or in row
order: the conversion is compute and read bound (~8 cycles a pixel, ~2.3 ms
of it reading the frame), not write bound. Zeroing each tiled output line
with DC ZVA before writing it (no line fill from memory; checked with
DCZID_EL0) and offsets computed once per row pair saved ~0.3-0.5 ms (7.9 ->
7.4-7.6 ms of CPU time). The remaining lever is the ~6 ms of preemption
(raising the thread's priority during the conversion - not done: it would
delay core 3's services, input and audio among them, by up to ~8 ms a frame).
USB send: measured at 2.2-4.3 ms a frame in game 3 (2.8-5.7 payloads of
16 KB, ~0.75 ms each: every payload was waited on before the next was
posted, ~21 MB/s), all of it time the capture loop waited. Now (1) a send
thread takes each encoded frame - copied into donor memory after the second
slot copy, ~1.5 MB - and the loop goes on with the next frame; a send failure
is known at the next hand-over, so the keyframe the host needs comes one frame
later than before; (2) UvcDeviceSendFrame keeps up to 4 payloads in flight,
each in its own staging buffer, a ZLP still right behind a payload that is a
multiple of 512 bytes. usb:ds reports at most the last 8 transfers, with
status 3 done, 4 cancelled, 5 failed, anything else pending: a transfer counts
as done when it or a later one is reported done (the endpoint completes them
in order); on a 1 s stall every transfer is cancelled and reaped before
returning, as before.
Encode beside the next frame: the send off the loop, the encode (~1.4-2.2
ms) was the last engine the loop waited for. The arena now holds two NVENC
input pictures (+1.33 MB; donor 34 MB): a frame is written (CPU or VIC) to
one while NVENC encodes the other. NvencSubmit starts the encode and flips
the picture; the job is polled between the pieces of the next frame and once
a pass (the loop waits at most 1 ms for a present while an encode is in
flight), and collected before the next submit - which is also what frees the
picture the frame after writes. A collected frame goes to the send thread
at once, so latency does not grow. The status-index check, the 1 s + 3 s
late/wedged rule and the bad-frame rule are unchanged. The keep-alive's
filler re-encodes the picture submitted last (NvencEncodeAgain). The mode
predictions no longer count the encode.
Delay on the Switch side (logging builds: "game present to last byte out",
the game's present to the frame's last byte over USB). Tetris at 60 fps, A/B
against a build encoding and sending in the loop as before: 6.5-6.7 ms on
average before, 6.8-7.8 ms with the encode and send beside the loop (the
loop notices a finished encode up to ~1 ms later), worst frames 18-22 vs
27-29 ms: a finished encode waited while the loop waited on the next
present's GPU fence. That wait now polls the encode too (_waitFence). A
delay seen on the PC beyond these numbers is the viewer's: DirectShow's
-rtbufsize holds up to ~2 s at Tetris's ~30 Mbps, and stays until the viewer
restarts (toggling the sysmodule only helps because it restarts the viewer's
device).
From Switch Frame Tap's commits of 22-25 October 2026 (after 1742954):
(1) a web applet no longer stops the capture by being open, only when it
leaves less than 96 MB beside us or grows 24 MB past the pool when our memory
came - Super Mario 3D All-Stars keeps OfflineWeb open the whole game, so it
never streamed; the yield check now runs every 100 ms (OfflineWeb ran out of
memory one step after a 1 s check in SFT); (2) vi-tap also wraps
TransactParcel (command 0, map-alias buffers), what games built before
firmware 3.0.0 present through (Fast RMX); (3) the swapchain is also found in
adjacent device-mapped regions that add up to the buffer set (Pokemon
Violet's 280 MB set, split by the game); (4) the VIC enlarges a picture
smaller than 1280x720 with its 2-tap filter (the default 1-tap is blocky).
Startup: the 20 s wait
for the system to come up only applies in the first minute after boot;
turned on later from ovlSysmodules, SysDVR-UVC starts at once. SysDVR-UVC still
works with a v2 vi-tap (latest present only) until the next reboot.

Also fixed after the game-switch tests: a stale "game exited" from the debug
pump ended every session through dmnt:cht at once (884 restarts; now a
back-off of 2-30 s for sessions under 2 s), a single region holding the set
is used directly (matching through dmnt:cht never converged), and every new
stream commit (an app reopening the camera) forces a keyframe.

## 12. Licence and credit

Both projects are GPL-2.0. Code taken from SFT (vi mitm parsing, debug pump,
slot match, VIC/NVENC drivers, `vic40_config.hpp` which itself transcribes
libdrm, MIT) keeps its headers, and a NOTICE file credits Switch Frame Tap
(Tomas Uribe) and its contributors (dmnt:cht path: Laliukz, PR #8).
