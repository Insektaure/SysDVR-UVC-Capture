#pragma once
// T210 VIC 4.0 (class NVB0B6, host1x class 0x5D): the configuration struct,
// method offsets and host1x opcodes.
//
// The structs are transcribed from libdrm tests/tegra/vic40.h (Copyright (c)
// 2016-2018 NVIDIA Corporation, MIT), as Switch Frame Tap's vic40_config.hpp
// (GPL-2.0, Tomas Uribe) did; the method offsets, formats and the facts in
// the comments are from that header and its hardware runs. GCC aarch64 packs
// u64 bitfields LSB-first, matching the per-field bit layout of the original.
#include <stdint.h>

// Method register byte offsets (pushed as method >> 2)
#define VIC_SET_APPLICATION_ID              0x00000200
#define VIC_EXECUTE                         0x00000300
#define VIC_SET_SURFACE0_SLOT0_LUMA_OFFSET  0x00000400
#define VIC_SET_CONTROL_PARAMS              0x00000704
#define VIC_SET_CONFIG_STRUCT_OFFSET        0x00000708
#define VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET  0x00000720
#define VIC_SET_OUTPUT_SURFACE_CHROMA_U_OFFSET 0x00000724  // NVIDIA clceb6.h; luma there is byte-identical

#define HOST1X_CLASS_VIC          0x5D
#define HOST1X_UCLASS_INCR_SYNCPT 0x00

// The submit only raises the syncpoint's max; the increment must be in the
// command stream or the syncpoint never reaches the fence - and the
// compositor (nvnflinger) composites on the same VIC syncpoint: it would
// wait forever (SFT: a frozen console). VIC 4.0: condition shift 8.
#define VIC_INCR_SYNCPT_COND_OP_DONE 1
#define VIC_INCR_SYNCPT_COND_SHIFT   8

static inline uint32_t Host1xSetClass(uint32_t offset, uint32_t classId, uint32_t mask)
{
    return (0u << 28) | ((offset & 0xFFF) << 16) | ((classId & 0x3FF) << 6) | (mask & 0x3F);
}
static inline uint32_t Host1xIncr(uint32_t offset, uint32_t count)
{
    return (1u << 28) | ((offset & 0xFFF) << 16) | (count & 0xFFFF);
}
static inline uint32_t Host1xNonIncr(uint32_t offset, uint32_t count)
{
    return (2u << 28) | ((offset & 0xFFF) << 16) | (count & 0xFFFF);
}

// Pixel formats. The game's A8B8G8R8 surface (bytes R,G,B,A) is declared
// A8R8G8B8 to the VIC, and the colour matrix below is measured for exactly
// that (SFT M82/M83).
#define VIC_PIXFMT_A8R8G8B8     32
#define VIC_PIXFMT_A8B8G8R8     33
#define VIC_PIXFMT_Y8_U8V8_N420 67  // NV12

#define VIC_BLK_KIND_PITCH         0
#define VIC_BLK_KIND_GENERIC_16Bx2 1  // the VIC's own 4-bit enum, not nvmap's kind 0xFE

#define VIC_CACHE_WIDTH_64Bx4 2  // valid for block-linear and pitch

// RGB -> BT.709 limited-range YCbCr for the output matrix, in the encoding
// SFT measured on hardware (tools/vic_csc.py, M82 Run H; vic_csc_bt709.h):
// rows (Y, Cr, Cb), columns (B, G, R, offset) for a source declared A8R8G8B8
// whose bytes are R,G,B,A; matrix_r_shift 8.
#define VIC_BT709_SHIFT 8
static const int32_t VicBt709[3][4] = {
    {  4064,  40254,  11966,  16896 },
    { -2639, -26145,  28784, 131584 },
    { 28785, -22189,  -6596, 131584 },
};

#pragma pack(push, 1)

typedef struct {
    uint64_t SlotEnable : 1, DeNoise : 1, AdvancedDenoise : 1, CadenceDetect : 1, MotionMap : 1, MMapCombine : 1,
             IsEven : 1, ChromaEven : 1, CurrentFieldEnable : 1, PrevFieldEnable : 1, NextFieldEnable : 1,
             NextNrFieldEnable : 1, CurMotionFieldEnable : 1, PrevMotionFieldEnable : 1, PpMotionFieldEnable : 1,
             CombMotionFieldEnable : 1, FrameFormat : 4, FilterLengthY : 2, FilterLengthX : 2, Panoramic : 12,
             reserved1 : 22;
    uint64_t DetailFltClamp : 6, FilterNoise : 10, FilterDetail : 10, ChromaNoise : 10, ChromaDetail : 10,
             DeinterlaceMode : 4, MotionAccumWeight : 3, NoiseIir : 11, LightLevel : 4, reserved4 : 2;
    uint64_t SoftClampLow : 10, SoftClampHigh : 10, reserved5 : 3, reserved6 : 9, PlanarAlpha : 10,
             ConstantAlpha : 1, StereoInterleave : 3, ClipEnabled : 1, ClearRectMask : 8, DegammaMode : 2,
             reserved7 : 1, DecompressEnable : 1, reserved9 : 5;
    uint64_t DecompressCtbCount : 8, DecompressZbcColor : 32, reserved12 : 24;
    uint64_t SourceRectLeft : 30, reserved14 : 2, SourceRectRight : 30, reserved15 : 2;
    uint64_t SourceRectTop : 30, reserved16 : 2, SourceRectBottom : 30, reserved17 : 2;
    uint64_t DestRectLeft : 14, reserved18 : 2, DestRectRight : 14, reserved19 : 2, DestRectTop : 14,
             reserved20 : 2, DestRectBottom : 14, reserved21 : 2;
    uint64_t reserved22 : 32, reserved23 : 32;
} VicSlotConfig;

typedef struct {
    uint64_t SlotPixelFormat : 7, SlotChromaLocHoriz : 2, SlotChromaLocVert : 2, SlotBlkKind : 4,
             SlotBlkHeight : 4, SlotCacheWidth : 3, reserved0 : 10, SlotSurfaceWidth : 14,
             SlotSurfaceHeight : 14, reserved1 : 4;
    uint64_t SlotLumaWidth : 14, SlotLumaHeight : 14, reserved2 : 4, SlotChromaWidth : 14,
             SlotChromaHeight : 14, reserved3 : 4;
} VicSlotSurfaceConfig;

typedef struct {
    uint64_t luma_coeff0 : 20, luma_coeff1 : 20, luma_coeff2 : 20, luma_r_shift : 4;
    uint64_t luma_coeff3 : 20, LumaKeyLower : 10, LumaKeyUpper : 10, LumaKeyEnabled : 1, reserved0 : 2,
             reserved1 : 21;
} VicLumaKeyStruct;

typedef struct {
    uint64_t matrix_coeff00 : 20, matrix_coeff10 : 20, matrix_coeff20 : 20, matrix_r_shift : 4;
    uint64_t matrix_coeff01 : 20, matrix_coeff11 : 20, matrix_coeff21 : 20, reserved0 : 3, matrix_enable : 1;
    uint64_t matrix_coeff02 : 20, matrix_coeff12 : 20, matrix_coeff22 : 20, reserved1 : 4;
    uint64_t matrix_coeff03 : 20, matrix_coeff13 : 20, matrix_coeff23 : 20, reserved2 : 4;
} VicMatrixStruct;

typedef struct {
    uint64_t ClearRect0Left : 14, reserved0 : 2, ClearRect0Right : 14, reserved1 : 2, ClearRect0Top : 14,
             reserved2 : 2, ClearRect0Bottom : 14, reserved3 : 2;
    uint64_t ClearRect1Left : 14, reserved4 : 2, ClearRect1Right : 14, reserved5 : 2, ClearRect1Top : 14,
             reserved6 : 2, ClearRect1Bottom : 14, reserved7 : 2;
} VicClearRectStruct;

typedef struct {
    uint64_t AlphaK1 : 10, reserved0 : 6, AlphaK2 : 10, reserved1 : 6, SrcFactCMatchSelect : 3, reserved2 : 1,
             DstFactCMatchSelect : 3, reserved3 : 1, SrcFactAMatchSelect : 3, reserved4 : 1,
             DstFactAMatchSelect : 3, reserved5 : 1, reserved6 : 4, reserved7 : 4, reserved8 : 4, reserved9 : 4;
    uint64_t reserved10 : 2, OverrideR : 10, OverrideG : 10, OverrideB : 10, OverrideA : 10, reserved11 : 2,
             UseOverrideR : 1, UseOverrideG : 1, UseOverrideB : 1, UseOverrideA : 1, MaskR : 1, MaskG : 1,
             MaskB : 1, MaskA : 1, reserved12 : 12;
} VicBlendingSlotStruct;

typedef struct {
    uint64_t AlphaFillMode : 3, AlphaFillSlot : 3, BackgroundAlpha : 10, BackgroundR : 10, BackgroundG : 10,
             BackgroundB : 10, RegammaMode : 2, OutputFlipX : 1, OutputFlipY : 1, OutputTranspose : 1,
             reserved1 : 1, reserved2 : 12;
    uint64_t TargetRectLeft : 14, reserved3 : 2, TargetRectRight : 14, reserved4 : 2, TargetRectTop : 14,
             reserved5 : 2, TargetRectBottom : 14, reserved6 : 2;
} VicOutputConfig;

typedef struct {
    uint64_t OutPixelFormat : 7, OutChromaLocHoriz : 2, OutChromaLocVert : 2, OutBlkKind : 4, OutBlkHeight : 4,
             reserved0 : 3, reserved1 : 10, OutSurfaceWidth : 14, OutSurfaceHeight : 14, reserved2 : 4;
    uint64_t OutLumaWidth : 14, OutLumaHeight : 14, reserved3 : 4, OutChromaWidth : 14, OutChromaHeight : 14,
             reserved4 : 4;
} VicOutputSurfaceConfig;

typedef struct {
    uint64_t DownsampleHoriz : 11, reserved0 : 5, DownsampleVert : 11, reserved1 : 5, reserved2 : 32;
    uint64_t reserved3 : 32, reserved4 : 32;
} VicPipeConfig;

typedef struct {
    VicSlotConfig slotConfig;
    VicSlotSurfaceConfig slotSurfaceConfig;
    VicLumaKeyStruct lumaKeyStruct;
    VicMatrixStruct colorMatrixStruct;
    VicMatrixStruct gamutMatrixStruct;
    VicBlendingSlotStruct blendingSlotStruct;
} VicSlotStruct;

typedef struct {
    VicPipeConfig pipeConfig;
    VicOutputConfig outputConfig;
    VicOutputSurfaceConfig outputSurfaceConfig;
    VicMatrixStruct outColorMatrixStruct;
    VicClearRectStruct clearRectStruct[4];
    VicSlotStruct slotStruct[8];
} VicConfigStruct;

#pragma pack(pop)

_Static_assert(sizeof(VicSlotConfig) == 64, "VIC 4.0 SlotConfig");
_Static_assert(sizeof(VicSlotStruct) == 176, "VIC 4.0 SlotStruct");
_Static_assert(sizeof(VicConfigStruct) == 1552, "VIC 4.0 ConfigStruct: SET_CONTROL_PARAMS wants (1552/16) << 16");
