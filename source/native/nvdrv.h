#pragma once
// A minimal nvdrv client: our own session, so its transfer memory is the
// donor's (libnx's nvInitialize would allocate 3 MB of our System-pool heap).
//
// nvdrv:t (full permission mask; SFT: "every engine node except TSEC"), else
// nvdrv:s. Commands: 0 Open, 1 Ioctl, 2 Close, 3 Initialize.
//
// Kill safety: closing the session (or dying) frees every fd, channel and
// nvmap handle; nvservices then drops its pins on our memory.
#include <switch.h>

// Linux-style ioctl encoding: (dir << 30) | (size << 16) | (type << 8) | nr
#define NV_IOWR(type, nr, size) ((3u << 30) | ((u32)(size) << 16) | ((type) << 8) | (nr))
#define NV_IOW(type, nr, size)  ((1u << 30) | ((u32)(size) << 16) | ((type) << 8) | (nr))

#define NVMAP_IOC_CREATE  NV_IOWR(0x01, 0x01, 8)
#define NVMAP_IOC_ALLOC   NV_IOWR(0x01, 0x04, 32)
#define NVMAP_IOC_FREE    NV_IOWR(0x01, 0x05, 24)
#define NVMAP_IOC_GET_ID  NV_IOWR(0x01, 0x0E, 8)

#define NVHOST_IOC_CHANNEL_GET_SYNCPOINT   NV_IOWR(0x00, 0x02, 8)   // {u32 module_id; u32 syncpt}
#define NVHOST_IOC_CHANNEL_SET_SUBMIT_TO   NV_IOW(0x00, 0x07, 4)    // {u32 timeout}
#define NVHOST_IOC_CHANNEL_SET_NVMAP_FD    NV_IOW(0x48, 0x01, 4)    // {u32 fd}
// MAP_CMD_BUFFER pins nvmap handles and returns their device address (Horizon
// replaced Linux's per-submit pinning with it; relocs are inert)
#define NVHOST_IOC_CHANNEL_MAP_CMD_BUFFER   NV_IOWR(0x00, 0x09, 20)
#define NVHOST_IOC_CHANNEL_UNMAP_CMD_BUFFER NV_IOWR(0x00, 0x0A, 20)
#define NVHOST_IOC_CTRL_SYNCPT_READ         NV_IOWR(0x00, 0x14, 8)  // {u32 id; u32 value}

// Opens the session and initializes it with `tmem` (the donor's nvdrv
// transfer memory; the caller keeps the handle until NvdrvExit).
Result NvdrvInit(Handle tmem, u32 tmemSize);
void   NvdrvExit(void);
bool   NvdrvActive(void);
const char* NvdrvServiceName(void);

Result NvdrvOpen(const char* path, u32* fd, u32* nverr);
Result NvdrvIoctl(u32 fd, u32 request, void* arg, size_t size, u32* nverr);
void   NvdrvClose(u32 fd);
