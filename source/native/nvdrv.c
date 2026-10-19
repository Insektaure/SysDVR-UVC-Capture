#include <string.h>
#include "../core.h"
#include "nvdrv.h"

// After Switch Frame Tap's NvIoctl/NvOpen/NvClose and its Initialize with the
// donor's transfer memory (applet_mitm_nv.cpp, GPL-2.0, Tomas Uribe).

static Service g_nv;
static bool g_open = false;
static const char* g_name = "";

Result NvdrvInit(Handle tmem, u32 tmemSize)
{
    if (g_open)
        return 0;
    // never block on sm: an unregistered name waits forever
    g_name = CoreHasService("nvdrv:t") ? "nvdrv:t" : "nvdrv:s";
    Result rc = smGetService(&g_nv, g_name);
    if (R_FAILED(rc))
        return rc;
    rc = serviceDispatchIn(&g_nv, 3, tmemSize,
        .in_num_handles = 2,
        .in_handles = { CUR_PROCESS_HANDLE, tmem },
    );
    if (R_FAILED(rc)) {
        serviceClose(&g_nv);
        return rc;
    }
    g_open = true;
    return 0;
}

void NvdrvExit(void)
{
    if (!g_open)
        return;
    serviceClose(&g_nv);
    g_open = false;
}

bool NvdrvActive(void)
{
    return g_open;
}

const char* NvdrvServiceName(void)
{
    return g_name;
}

Result NvdrvOpen(const char* path, u32* fd, u32* nverr)
{
    struct { u32 fd; u32 error; } out = { 0, 0 };
    Result rc = serviceDispatchOut(&g_nv, 0, out,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
        .buffers = { { path, strlen(path) } },
    );
    *fd = out.fd;
    *nverr = out.error;
    return rc;
}

Result NvdrvIoctl(u32 fd, u32 request, void* arg, size_t size, u32* nverr)
{
    const struct { u32 fd; u32 request; } in = { fd, request };
    u32 error = 0;
    Result rc = serviceDispatchInOut(&g_nv, 1, in, error,
        .buffer_attrs = {
            SfBufferAttr_HipcAutoSelect | SfBufferAttr_In,
            SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out,
        },
        .buffers = { { arg, size }, { arg, size } },
    );
    *nverr = error;
    return rc;
}

void NvdrvClose(u32 fd)
{
    const struct { u32 fd; } in = { fd };
    u32 error = 0;
    serviceDispatchInOut(&g_nv, 2, in, error);
}
