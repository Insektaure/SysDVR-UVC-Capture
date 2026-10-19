#include <string.h>
#include "../core.h"
#include "donor.h"
#include "nvdrv.h"
#include "vic.h"
#include "vic_engine.h"

// Layout of the donor's capture region
#define OFF_CFG     0x000000  // VicConfigStruct (1552 B)            uncached
#define OFF_CMD     0x004000  // host1x command buffer               uncached
#define OFF_GUARD   0x008000  // pages that hold device address 0    uncached
#define OFF_OUT     0x010000  // NV12 output                         cached, invalidated after a job
#define OFF_BOUNCE  0x200000  // dmnt:cht reads (not pinned)
#define OFF_SLOT    0x400000  // copy of the game's buffer           cached, flushed by the caller
#define CFG_SIZE    0x4000
#define CMD_SIZE    0x1000
#define OUT_SIZE    ((VIC_OUT_BYTES + 0xFFF) & ~0xFFFu)
#define BOUNCE_SIZE 0x200000

_Static_assert(OFF_OUT + OUT_SIZE <= OFF_BOUNCE, "NV12 output overlaps the bounce buffer");
_Static_assert(VIC_OUT_LUMA % 256 == 0, "host1x addresses are carried >> 8: the UV plane must be 256-aligned");

#define SUBMIT_TIMEOUT_POLLS 5000  // x 100 us: 0.5 s (SFT: the compositor shares the VIC)

static u8* g_mem = NULL;
static u64 g_memSize = 0;
static u32 g_slotBytes = 0;
static Handle g_nvTmem = INVALID_HANDLE;
static bool g_open = false, g_wedged = false;
static u32 g_nvmapFd = 0, g_ctrlFd = 0, g_vicFd = 0, g_syncpt = 0;
static u32 g_cmdHandle = 0;
static u32 g_cfgAddr = 0, g_outAddr = 0, g_slotAddr = 0;
static VicEngineStats g_stats;

static bool _nvmapOwn(void* cpu, u32 size, bool cacheable, u32* handle)
{
    u32 nverr = 0;
    struct { u32 size; u32 handle; } cr = { size, 0 };
    Result rc = NvdrvIoctl(g_nvmapFd, NVMAP_IOC_CREATE, &cr, sizeof(cr), &nverr);
    if (R_FAILED(rc) || nverr) {
        LOG("vic: nvmap CREATE 0x%x nverr %u\n", rc, nverr);
        return false;
    }
    struct {
        u32 handle, heapmask, flags, align;
        u8 kind, pad[7];
        u64 addr;
    } al = { cr.handle, 0, cacheable ? 1u : 0u, 0x1000, 0, {0}, (u64)cpu };
    rc = NvdrvIoctl(g_nvmapFd, NVMAP_IOC_ALLOC, &al, sizeof(al), &nverr);
    if (R_FAILED(rc) || nverr) {
        LOG("vic: nvmap ALLOC 0x%x nverr %u\n", rc, nverr);
        return false;
    }
    // libnx's nvMapCreate does the same for a non-cacheable buffer: flush,
    // then make the CPU mapping uncached so the CPU and the engine agree
    armDCacheFlush(cpu, size);
    if (!cacheable)
        svcSetMemoryAttribute(cpu, size, MemAttr_IsUncached, MemAttr_IsUncached);
    *handle = cr.handle;
    return true;
}

static bool _map(u32 handle, u32* addr, const char* what)
{
    struct { u32 num_handles, reserved; u8 is_compr, pad[3]; u32 handle, phys; } a = { 1, 0, 0, {0}, handle, 0 };
    _Static_assert(sizeof(a) == 20, "MAP_CMD_BUFFER args");
    u32 nverr = 0;
    Result rc = NvdrvIoctl(g_vicFd, NVHOST_IOC_CHANNEL_MAP_CMD_BUFFER, &a, sizeof(a), &nverr);
    LOG("vic: map %s -> 0x%x (0x%x, nverr %u)\n", what, a.phys, rc, nverr);
    if (R_FAILED(rc) || nverr)
        return false;
    *addr = a.phys;
    return true;
}

bool VicEngineOpen(void* mem, u64 size, u32 slotBytes)
{
    if (g_open)
        return true;
    g_wedged = false;
    memset(&g_stats, 0, sizeof(g_stats));
    const u32 slotMap = (slotBytes + 0xFFF) & ~0xFFFu;
    if (size < OFF_SLOT + slotMap) {
        LOG("vic: %lu KB of donor memory, need %u KB for a %u B buffer\n", size / 1024, (OFF_SLOT + slotMap) / 1024, slotBytes);
        return false;
    }
    u64 nvSize = 0;
    if (!DonorTakeNvTmem(&g_nvTmem, &nvSize)) {
        LOG("vic: no nvdrv transfer memory from the donor\n");
        return false;
    }
    Result rc = NvdrvInit(g_nvTmem, (u32)nvSize);
    if (R_FAILED(rc)) {
        LOG("vic: %s Initialize 0x%x\n", NvdrvServiceName(), rc);
        svcCloseHandle(g_nvTmem);
        g_nvTmem = INVALID_HANDLE;
        return false;
    }
    g_mem = (u8*)mem;
    g_memSize = size;
    g_slotBytes = slotBytes;
    memset(g_mem, 0, OFF_BOUNCE);
    g_open = true;

    u32 nverr = 0;
    bool ok = R_SUCCEEDED(NvdrvOpen("/dev/nvmap", &g_nvmapFd, &nverr)) && !nverr &&
              R_SUCCEEDED(NvdrvOpen("/dev/nvhost-ctrl", &g_ctrlFd, &nverr)) && !nverr &&
              R_SUCCEEDED(NvdrvOpen("/dev/nvhost-vic", &g_vicFd, &nverr)) && !nverr;
    if (ok) {
        struct { u32 module_id, syncpt; } gs = { 0, 0 };
        ok = R_SUCCEEDED(NvdrvIoctl(g_vicFd, NVHOST_IOC_CHANNEL_GET_SYNCPOINT, &gs, sizeof(gs), &nverr)) && !nverr && gs.syncpt;
        g_syncpt = gs.syncpt;
    }
    if (ok) {
        struct { u32 fd; } sn = { g_nvmapFd };
        struct { u32 timeout; } st = { 1000 };
        NvdrvIoctl(g_vicFd, NVHOST_IOC_CHANNEL_SET_NVMAP_FD, &sn, sizeof(sn), &nverr);
        NvdrvIoctl(g_vicFd, NVHOST_IOC_CHANNEL_SET_SUBMIT_TO, &st, sizeof(st), &nverr);
    }
    LOG("vic: %s, nvmap fd %u, ctrl fd %u, vic fd %u, syncpoint %u%s\n", NvdrvServiceName(),
        g_nvmapFd, g_ctrlFd, g_vicFd, g_syncpt, ok ? "" : " - FAILED");

    // Device address 0 is a real address the channel hands out when free,
    // and the VIC hangs on it: a guard page takes it before anything else
    u32 guard = 0, guardAddr = ~0u, h = 0;
    if (ok && _nvmapOwn(g_mem + OFF_GUARD, 0x1000, false, &guard))
        _map(guard, &guardAddr, "guard");
    if (ok && guardAddr == 0)
        LOG("vic: device address 0 was free - the guard page holds it now\n");

    ok = ok && _nvmapOwn(g_mem + OFF_CFG, CFG_SIZE, false, &h) && _map(h, &g_cfgAddr, "cfg");
    // the command buffer is not pinned: the submit names it by nvmap handle (as SFT)
    ok = ok && _nvmapOwn(g_mem + OFF_CMD, CMD_SIZE, false, &g_cmdHandle);
    ok = ok && _nvmapOwn(g_mem + OFF_OUT, OUT_SIZE, true, &h) && _map(h, &g_outAddr, "nv12 out");
    ok = ok && _nvmapOwn(g_mem + OFF_SLOT, slotMap, true, &h) && _map(h, &g_slotAddr, "slot copy");
    if (ok && (!g_cfgAddr || !g_outAddr || !g_slotAddr)) {
        LOG("vic: a buffer pinned at device address 0 - refusing to use the engine\n");
        ok = false;
    }
    if (!ok) {
        VicEngineClose();
        return false;
    }
    LOG("vic: ready - cfg 0x%x, out 0x%x, slot copy 0x%x (%u KB) in %lu MB of donor memory\n",
        g_cfgAddr, g_outAddr, g_slotAddr, slotMap / 1024, size >> 20);
    return true;
}

void VicEngineClose(void)
{
    if (!g_open)
        return;
    if (g_wedged) {
        // SFT M74: tearing down a channel with a job stuck in it took the
        // compositor with it. Leave everything open: the donor memory stays
        // pinned (DonorRelease will refuse the unmap) until this process ends.
        LOG("vic: engine wedged - leaving the nvdrv session open\n");
        return;
    }
    NvdrvExit();  // frees every fd, channel, nvmap handle and pin
    if (g_nvTmem != INVALID_HANDLE) {
        svcCloseHandle(g_nvTmem);
        g_nvTmem = INVALID_HANDLE;
    }
    g_open = false;
    g_nvmapFd = g_ctrlFd = g_vicFd = g_syncpt = g_cmdHandle = 0;
    g_cfgAddr = g_outAddr = g_slotAddr = 0;
    g_mem = NULL;
}

u64 VicEngineBytesUsed(void) { return g_open ? OFF_SLOT + ((g_slotBytes + 0xFFFu) & ~0xFFFu) : 0; }
u32 VicEngineNvmapFd(void) { return g_nvmapFd; }
u32 VicEngineCtrlFd(void)  { return g_ctrlFd; }
bool VicEngineNvmapOwn(void* cpu, u32 size, bool cacheable, u32* handle) { return g_open && _nvmapOwn(cpu, size, cacheable, handle); }
bool VicEnginePin(u32 handle, u32* vicAddr) { return g_open && _map(handle, vicAddr, "external"); }

bool VicEngineActive(void)  { return g_open && !g_wedged; }
bool VicEngineWedged(void)  { return g_wedged; }
u8* VicEngineSlotCopy(void) { return g_mem ? g_mem + OFF_SLOT : NULL; }
u8* VicEngineBounce(void)   { return g_mem ? g_mem + OFF_BOUNCE : NULL; }
size_t VicEngineBounceSize(void) { return BOUNCE_SIZE; }
const u8* VicEngineOutput(void) { return g_mem ? g_mem + OFF_OUT : NULL; }

static inline u64 _s20(s32 v) { return (u64)((u32)v & 0xFFFFFu); }

static void _fillConfig(VicConfigStruct* c, const VicSource* src, bool blockLinear)
{
    memset(c, 0, sizeof(*c));

    c->outputConfig.TargetRectRight  = VIC_OUT_W - 1;
    c->outputConfig.TargetRectBottom = VIC_OUT_H - 1;
    c->outputConfig.BackgroundAlpha  = 1023;
    c->outputConfig.BackgroundR      = 1023;
    c->outputConfig.BackgroundG      = 1023;
    c->outputConfig.BackgroundB      = 1023;
    c->outputConfig.OutputFlipX      = (src->flip & 1) ? 1 : 0;
    c->outputConfig.OutputFlipY      = (src->flip & 2) ? 1 : 0;

    c->outputSurfaceConfig.OutPixelFormat   = VIC_PIXFMT_Y8_U8V8_N420;
    // NVENC reads 16-row blocks: generic 16Bx2, 2^1 GOBs (SFT M82/M83)
    c->outputSurfaceConfig.OutBlkKind       = blockLinear ? VIC_BLK_KIND_GENERIC_16Bx2 : VIC_BLK_KIND_PITCH;
    c->outputSurfaceConfig.OutBlkHeight     = blockLinear ? 1 : 0;
    c->outputSurfaceConfig.OutSurfaceWidth  = VIC_OUT_W - 1;
    c->outputSurfaceConfig.OutSurfaceHeight = VIC_OUT_H - 1;
    c->outputSurfaceConfig.OutLumaWidth     = VIC_OUT_W - 1;  // stride = width
    c->outputSurfaceConfig.OutLumaHeight    = VIC_OUT_H - 1;
    c->outputSurfaceConfig.OutChromaWidth   = VIC_OUT_W / 2 - 1;
    c->outputSurfaceConfig.OutChromaHeight  = VIC_OUT_H / 2 - 1;

    // RGB -> BT.709 on the way out (the VIC does no implicit conversion)
    VicMatrixStruct* m = &c->outColorMatrixStruct;
    m->matrix_coeff00 = _s20(VicBt709[0][0]); m->matrix_coeff01 = _s20(VicBt709[0][1]);
    m->matrix_coeff02 = _s20(VicBt709[0][2]); m->matrix_coeff03 = _s20(VicBt709[0][3]);
    m->matrix_coeff10 = _s20(VicBt709[1][0]); m->matrix_coeff11 = _s20(VicBt709[1][1]);
    m->matrix_coeff12 = _s20(VicBt709[1][2]); m->matrix_coeff13 = _s20(VicBt709[1][3]);
    m->matrix_coeff20 = _s20(VicBt709[2][0]); m->matrix_coeff21 = _s20(VicBt709[2][1]);
    m->matrix_coeff22 = _s20(VicBt709[2][2]); m->matrix_coeff23 = _s20(VicBt709[2][3]);
    m->matrix_r_shift = VIC_BT709_SHIFT;
    m->matrix_enable  = 1;

    VicSlotConfig* s = &c->slotStruct[0].slotConfig;
    s->SlotEnable         = 1;
    s->CurrentFieldEnable = 1;
    s->PlanarAlpha        = 1023;
    s->ConstantAlpha      = 1;
    s->SourceRectRight    = (u64)(src->rect_w - 1) << 16;  // 16.16 fixed point
    s->SourceRectBottom   = (u64)(src->rect_h - 1) << 16;
    s->DestRectRight      = VIC_OUT_W - 1;
    s->DestRectBottom     = VIC_OUT_H - 1;
    s->SoftClampHigh      = 1023;

    VicSlotSurfaceConfig* ss = &c->slotStruct[0].slotSurfaceConfig;
    ss->SlotPixelFormat   = src->pixfmt;
    ss->SlotBlkKind       = VIC_BLK_KIND_GENERIC_16Bx2;
    ss->SlotBlkHeight     = src->blk_h_log2;
    ss->SlotCacheWidth    = VIC_CACHE_WIDTH_64Bx4;
    ss->SlotSurfaceWidth  = src->w - 1;
    ss->SlotSurfaceHeight = src->h - 1;
    ss->SlotLumaWidth     = src->stride_px - 1;
    ss->SlotLumaHeight    = src->h - 1;
    ss->SlotChromaWidth   = 16383;  // no chroma plane
    ss->SlotChromaHeight  = 16383;
}

static u32 _buildCmd(u32* w, const VicTarget* dst)
{
    u32 n = 0;
    const u32 incr2 = Host1xIncr(0x10, 2);  // METHOD_OFFSET, METHOD_DATA
    w[n++] = Host1xSetClass(0, HOST1X_CLASS_VIC, 0);
    w[n++] = incr2; w[n++] = VIC_SET_APPLICATION_ID >> 2;  w[n++] = 1;
    w[n++] = incr2; w[n++] = VIC_SET_CONTROL_PARAMS >> 2;  w[n++] = (u32)(sizeof(VicConfigStruct) / 16) << 16;
    w[n++] = incr2; w[n++] = VIC_SET_CONFIG_STRUCT_OFFSET >> 2; w[n++] = g_cfgAddr >> 8;
    w[n++] = incr2; w[n++] = VIC_SET_OUTPUT_SURFACE_LUMA_OFFSET >> 2; w[n++] = dst->lumaAddr >> 8;
    w[n++] = incr2; w[n++] = VIC_SET_OUTPUT_SURFACE_CHROMA_U_OFFSET >> 2; w[n++] = dst->chromaAddr >> 8;
    w[n++] = incr2; w[n++] = VIC_SET_SURFACE0_SLOT0_LUMA_OFFSET >> 2; w[n++] = g_slotAddr >> 8;
    w[n++] = incr2; w[n++] = VIC_EXECUTE >> 2; w[n++] = 1u << 8;
    w[n++] = Host1xNonIncr(HOST1X_UCLASS_INCR_SYNCPT, 1);
    w[n++] = (VIC_INCR_SYNCPT_COND_OP_DONE << VIC_INCR_SYNCPT_COND_SHIFT) | g_syncpt;
    return n;
}

static bool _syncptReached(u32 id, u32 want)
{
    struct { u32 id, value; } r = { id, 0 };
    u32 nverr = 0;
    if (R_FAILED(NvdrvIoctl(g_ctrlFd, NVHOST_IOC_CTRL_SYNCPT_READ, &r, sizeof(r), &nverr)) || nverr)
        return true;  // unreadable: do not wait forever on it
    return (s32)(r.value - want) >= 0;
}

bool VicEngineConvert(const VicSource* src, u64* ns)
{
    const VicTarget own = { g_outAddr, g_outAddr + VIC_OUT_LUMA, false };
    if (!VicEngineConvertTo(src, &own, ns))
        return false;
    // the engine wrote through the device side: drop our stale cache lines
    armDCacheFlush(g_mem + OFF_OUT, VIC_OUT_BYTES);
    return true;
}

bool VicEngineConvertTo(const VicSource* src, const VicTarget* dst, u64* ns)
{
    if (!g_open || g_wedged)
        return false;
    if (!g_cfgAddr || !dst->lumaAddr || !dst->chromaAddr || (dst->lumaAddr & 0xFF) || (dst->chromaAddr & 0xFF) ||
        !g_slotAddr || !src->rect_w || !src->rect_h ||
        src->rect_w > src->w || src->rect_h > src->h || src->w > 16384 || src->h > 16384) {
        g_stats.refused++;  // a zero address hangs the VIC; never submit one
        return false;
    }

    VicConfigStruct* cfg = (VicConfigStruct*)(g_mem + OFF_CFG);
    _fillConfig(cfg, src, dst->blockLinear);
    u32* cmd = (u32*)(g_mem + OFF_CMD);
    const u32 words = _buildCmd(cmd, dst);
    armDCacheFlush(cfg, CFG_SIZE);
    armDCacheFlush(cmd, CMD_SIZE);

    // CHANNEL_SUBMIT with its arrays inline: header {cmdbufs, relocs,
    // syncpt_incrs, fences}, one cmdbuf {mem, offset, words}, one syncpt incr
    // {id, incrs, 3 reserved}, one fence (out). No relocs: they are inert on
    // Horizon, the addresses are inlined from MAP_CMD_BUFFER.
    u32 sb[13] = { 1, 0, 1, 1, g_cmdHandle, 0, words, g_syncpt, 1, 0, 0, 0, 0 };
    const u32 req = NV_IOWR(0x00, 0x01, sizeof(sb));
    const u64 t0 = armTicksToNs(armGetSystemTick());
    u32 nverr = 0;
    Result rc = NvdrvIoctl(g_vicFd, req, sb, sizeof(sb), &nverr);
    if (R_FAILED(rc) || nverr) {
        LOG("vic: submit refused (0x%x, nverr %u)\n", rc, nverr);
        g_stats.refused++;
        return false;
    }
    const u32 fence = sb[12];

    // SYNCPT_WAIT returns early (SFT M63b): poll the value instead
    bool done = _syncptReached(g_syncpt, fence);
    for (u32 i = 0; i < SUBMIT_TIMEOUT_POLLS && !done; i++) {
        svcSleepThread(100000);
        done = _syncptReached(g_syncpt, fence);
    }
    const u64 dt = armTicksToNs(armGetSystemTick()) - t0;
    if (!done) {
        g_stats.timeouts++;
        g_wedged = true;
        LOG("vic: job did not complete in 0.5 s (syncpoint %u, fence %u) - engine wedged, no more jobs\n",
            g_syncpt, fence);
        return false;
    }
    g_stats.jobs++;
    g_stats.nsTotal += dt;
    if (dt > g_stats.nsMax)
        g_stats.nsMax = dt;
    if (ns)
        *ns = dt;
    return true;
}

bool VicEngineWaitFence(u32 count, const u64* fence, u64 timeoutNs, u64* waitedNs)
{
    const u64 t0 = armTicksToNs(armGetSystemTick());
    bool ok = true;
    if (g_open && count > 0 && count <= 4) {
        for (u32 k = 0; k < count; k++) {
            const u32 id = (u32)(fence[k] >> 32), want = (u32)fence[k];
            if (id == 0xFFFFFFFFu || id >= 192)
                continue;  // an unused entry
            while (!_syncptReached(id, want)) {
                if (armTicksToNs(armGetSystemTick()) - t0 > timeoutNs) {
                    ok = false;
                    break;
                }
                svcSleepThread(250000);
            }
        }
    }
    if (waitedNs)
        *waitedNs = armTicksToNs(armGetSystemTick()) - t0;
    return ok;
}

bool VicEngineFenceReached(u32 count, const u64* fence)
{
    if (!g_open || count == 0 || count > 4)
        return true;
    for (u32 k = 0; k < count; k++) {
        const u32 id = (u32)(fence[k] >> 32), want = (u32)fence[k];
        if (id == 0xFFFFFFFFu || id >= 192)
            continue;
        if (!_syncptReached(id, want))
            return false;
    }
    return true;
}

void VicEngineGetStats(VicEngineStats* out, bool reset)
{
    *out = g_stats;
    if (reset) {
        g_stats.jobs = 0;
        g_stats.nsTotal = g_stats.nsMax = 0;
    }
}
