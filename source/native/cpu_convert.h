#pragma once
// The game's frame (block-linear RGBA, as copied to a slot copy) to NV12
// 1280x720 pitch-linear, BT.709 limited range, on the CPU with NEON: what the
// VIC does, without the VIC. In GPU-heavy games every VIC job waits behind
// the compositor's (17-41 ms for a 0.6 ms job, measured in 01003ec027cc8000).
//
// Only a 1280x720 rect (handheld, no scaling) and no horizontal flip
// (CpuCanConvert); false otherwise. y: 1280x720, uv: 1280x360 interleaved
// U,V; pitch-linear, or NVENC's input layout (blockLinear: 16-row
// block-linear, what the VIC writes for it). The caller flushes the output.
#include <switch.h>
#include "vic_engine.h"

bool CpuCanConvert(const VicSource* src);
bool CpuConvertNv12(const u8* slot, const VicSource* src, u8* y, u8* uv, bool blockLinear);

// Part of the frame: rows [rowFirst, rowEnd) (rowFirst even) from data, which
// holds the surface from byte dataOff on - one block row of the game's
// layout read on its own (CpuBlockRowLines lines; rows outside data must not
// be asked for). y/uv are the whole output planes.
u32  CpuBlockRowLines(const VicSource* src);
bool CpuConvertRows(const u8* data, u64 dataOff, const VicSource* src, u32 rowFirst, u32 rowEnd,
                    u8* y, u8* uv, bool blockLinear);
