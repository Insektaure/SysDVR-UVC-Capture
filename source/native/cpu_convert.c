#include <arm_neon.h>
#include "vic.h"
#include "cpu_convert.h"

// Block-linear (generic 16Bx2): the surface is rows of blocks, a block is
// 2^blk_h_log2 GOBs stacked vertically, a GOB is 64 bytes x 8 rows (512 B).
// Inside a GOB, a 16-byte piece (4 pixels of one row) for bytes x and row r:
//   ((x % 64) / 32) * 256 + (r / 2) * 64 + ((x % 32) / 16) * 32 + (r % 2) * 16
// so the two rows of a pair sit side by side, and one GOB row pair holds 16
// pixels of both rows: pieces at +0, +32, +256, +288 (top) and +16 (bottom).
//
// BT.709 limited range, 8-bit, x256:
//   Y  =  16 + ( 47 R + 157 G +  16 B) / 256
//   Cb = 128 + (-26 R -  87 G + 113 B) / 256
//   Cr = 128 + (112 R - 102 G -  10 B) / 256
// chroma from the 2x2 average (the VIC's 4:2:0 is close to it).

static inline void _deinterleave(const u8* g, uint8x16_t* r, uint8x16_t* gr, uint8x16_t* b)
{
    // 16 pixels of one row: 4 pieces of 4 RGBA pixels
    const uint8x16_t p0 = vld1q_u8(g), p1 = vld1q_u8(g + 32), p2 = vld1q_u8(g + 256), p3 = vld1q_u8(g + 288);
    const uint8x16_t e0 = vuzp1q_u8(p0, p1), o0 = vuzp2q_u8(p0, p1);  // R,B / G,A of pixels 0-7
    const uint8x16_t e1 = vuzp1q_u8(p2, p3), o1 = vuzp2q_u8(p2, p3);  // pixels 8-15
    *r = vuzp1q_u8(e0, e1);
    *b = vuzp2q_u8(e0, e1);
    *gr = vuzp1q_u8(o0, o1);
}

static inline uint8x16_t _luma(uint8x16_t r, uint8x16_t g, uint8x16_t b)
{
    uint16x8_t lo = vmull_u8(vget_low_u8(r), vdup_n_u8(47));
    lo = vmlal_u8(lo, vget_low_u8(g), vdup_n_u8(157));
    lo = vmlal_u8(lo, vget_low_u8(b), vdup_n_u8(16));
    uint16x8_t hi = vmull_high_u8(r, vdupq_n_u8(47));
    hi = vmlal_high_u8(hi, g, vdupq_n_u8(157));
    hi = vmlal_high_u8(hi, b, vdupq_n_u8(16));
    return vaddq_u8(vcombine_u8(vrshrn_n_u16(lo, 8), vrshrn_n_u16(hi, 8)), vdupq_n_u8(16));
}

// 2x2 averages of one channel: 8 values from two rows of 16
static inline int16x8_t _avg4(uint8x16_t top, uint8x16_t bot)
{
    return vreinterpretq_s16_u16(vrshrq_n_u16(vaddq_u16(vpaddlq_u8(top), vpaddlq_u8(bot)), 2));
}

static inline uint8x8_t _chroma(int16x8_t r, int16x8_t g, int16x8_t b, s16 kr, s16 kg, s16 kb)
{
    int16x8_t v = vmulq_n_s16(r, kr);
    v = vmlaq_n_s16(v, g, kg);
    v = vmlaq_n_s16(v, b, kb);
    return vqmovun_s16(vaddq_s16(vrshrq_n_s16(v, 8), vdupq_n_s16(128)));
}

// Where 16 output bytes at x (16-aligned) of row r go: pitch, or 16-row
// block-linear with a 1280-byte pitch (20 GOBs, 2 GOBs a block)
static inline u64 _out(u32 x, u32 r, bool blockLinear)
{
    if (!blockLinear)
        return (u64)r * VIC_OUT_W + x;
    return (u64)(r >> 4) * (VIC_OUT_W / 64 * 1024) + (x >> 6) * 1024 + ((r >> 3) & 1) * 512 +
           ((x & 63) >> 5) * 256 + ((r & 7) >> 1) * 64 + ((x & 31) >> 4) * 32 + (r & 1) * 16;
}

bool CpuCanConvert(const VicSource* src)
{
    return src->rect_w == VIC_OUT_W && src->rect_h == VIC_OUT_H && !(src->flip & 1) && src->h >= VIC_OUT_H &&
           src->stride_px >= VIC_OUT_W && (src->stride_px * 4) % 64 == 0;
}

bool CpuConvertNv12(const u8* slot, const VicSource* src, u8* y, u8* uv, bool blockLinear)
{
    return CpuConvertRows(slot, 0, src, 0, VIC_OUT_H, y, uv, blockLinear);
}

u32 CpuBlockRowLines(const VicSource* src)
{
    return 8u << src->blk_h_log2;
}

bool CpuConvertRows(const u8* data, u64 dataOff, const VicSource* src, u32 rowFirst, u32 rowEnd,
                    u8* y, u8* uv, bool blockLinear)
{
    const u32 W = VIC_OUT_W, H = VIC_OUT_H;
    if (!data || !CpuCanConvert(src) || (rowFirst & 1) || rowEnd > H || rowFirst > rowEnd)
        return false;
    const bool bgr = src->pixfmt == VIC_PIXFMT_A8B8G8R8;  // the game's bytes B,G,R,A (see _source)
    const bool flipV = (src->flip & 2) != 0;
    const u32 bh = 1u << src->blk_h_log2;
    const u64 blockBytes = 512u * bh;
    const u64 rowBytes = (u64)(src->stride_px * 4 / 64) * blockBytes;

    for (u32 row = rowFirst; row < rowEnd; row += 2) {
        const u8* base = data + ((row / (8 * bh)) * rowBytes + ((row / 8) % bh) * 512 + ((row % 8) / 2) * 64 - dataOff);
        const u32 d0 = flipV ? H - 1 - row : row, d1 = flipV ? d0 - 1 : row + 1;
        const u32 dc = flipV ? (H - 2 - row) / 2 : row / 2;
        for (u32 gx = 0; gx < W / 16; gx++) {
            const u32 x = gx * 16;
            const u8* g = base + gx * blockBytes;
            uint8x16_t r0, g0, b0, r1, g1, b1;
            _deinterleave(g, &r0, &g0, &b0);
            _deinterleave(g + 16, &r1, &g1, &b1);
            if (bgr) {
                const uint8x16_t t0 = r0, t1 = r1;
                r0 = b0; b0 = t0;
                r1 = b1; b1 = t1;
            }
            vst1q_u8(y + _out(x, d0, blockLinear), _luma(r0, g0, b0));
            vst1q_u8(y + _out(x, d1, blockLinear), _luma(r1, g1, b1));
            const int16x8_t ar = _avg4(r0, r1), ag = _avg4(g0, g1), ab = _avg4(b0, b1);
            uint8x8x2_t cc;
            cc.val[0] = _chroma(ar, ag, ab, -26, -87, 113);
            cc.val[1] = _chroma(ar, ag, ab, 112, -102, -10);
            vst2_u8(uv + _out(x, dc, blockLinear), cc);
        }
    }
    return true;
}
