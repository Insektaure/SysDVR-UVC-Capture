#pragma once
// Phase 2a probe: attach to the game, find and match its swapchain buffers,
// read frames, dump a few to sdmc:/config/sysdvr-uvc/dump/. Built only with
// -DNATIVE_PROBE. See native_probe.c.
void NativeProbeThread(void* arg);
