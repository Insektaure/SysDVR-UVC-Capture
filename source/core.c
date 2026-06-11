#include <string.h>
#include <stdarg.h>
#include "core.h"
#include "grcd.h"
#include "capture.h"
#include "uvc/uvc_device.h"

// ============================================================================
// Static Data
// ============================================================================

static char g_serialNumber[32] = "SysDVR-UVC";
static bool g_coreInitialized = false;

// ============================================================================
// Logging Implementation
// ============================================================================

#if UDP_LOGGING
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int g_logSocket = -1;
static struct sockaddr_in g_logAddr;

static void _initUdpLogging(void)
{
    g_logSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_logSocket < 0) return;

    memset(&g_logAddr, 0, sizeof(g_logAddr));
    g_logAddr.sin_family = AF_INET;
    g_logAddr.sin_port = htons(9999);
    g_logAddr.sin_addr.s_addr = inet_addr("192.168.1.255");  // Broadcast
}

void LogFunctionImpl(const char* fmt, ...)
{
    if (g_logSocket < 0) return;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0) {
        sendto(g_logSocket, buf, len, 0,
               (struct sockaddr*)&g_logAddr, sizeof(g_logAddr));
    }
}
#elif FILE_LOGGING
#include <stdio.h>

static FsFileSystem g_sdCard;
static bool g_sdMounted = false;

static void _initFileLogging(void)
{
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&g_sdCard))) {
        g_sdMounted = true;
        // Truncate existing log
        fsFsDeleteFile(&g_sdCard, "/logfile.txt");
        fsFsCreateFile(&g_sdCard, "/logfile.txt", 0, 0);
    }
}

static void _exitFileLogging(void)
{
    if (g_sdMounted) {
        fsFsClose(&g_sdCard);
        g_sdMounted = false;
    }
}

void LogFunctionImpl(const char* fmt, ...)
{
    if (!g_sdMounted) return;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len <= 0) return;

    FsFile file;
    if (R_SUCCEEDED(fsFsOpenFile(&g_sdCard, "/logfile.txt", FsOpenMode_Write | FsOpenMode_Append, &file))) {
        s64 offset = 0;
        fsFileGetSize(&file, &offset);
        fsFileWrite(&file, offset, buf, len, FsWriteOption_Flush);
        fsFileClose(&file);
    }
}
#endif

// ============================================================================
// Core Implementation
// ============================================================================

static Result _getSerialNumber(void)
{
    Result rc;
    SetSysSerialNumber serial;

    rc = setsysInitialize();
    if (R_FAILED(rc)) {
        LOG("setsysInitialize failed: 0x%x\n", rc);
        return rc;
    }

    rc = setsysGetSerialNumber(&serial);
    if (R_SUCCEEDED(rc)) {
        strncpy(g_serialNumber, serial.number, sizeof(g_serialNumber) - 1);
        g_serialNumber[sizeof(g_serialNumber) - 1] = '\0';
    }

    setsysExit();
    return rc;
}

Result CoreInit(void)
{
    if (g_coreInitialized) {
        return 0;
    }

    Result rc;

#if UDP_LOGGING
    // Logging stays silently disabled if socket init fails (suspected on
    // the small 128KB sysmodule heap: socketInitializeDefault wants a large
    // bsd transfer memory) - the sysmodule itself keeps running
    rc = socketInitializeDefault();
    if (R_SUCCEEDED(rc)) {
        _initUdpLogging();
    }
#elif FILE_LOGGING
    rc = fsInitialize();
    if (R_SUCCEEDED(rc)) {
        _initFileLogging();
    }
#endif

    LOG("SysDVR-UVC %s initializing...\n", SYSDVR_UVC_VERSION);

    // Real console serial makes the USB device identifiable; the default
    // string is fine if this fails
    rc = _getSerialNumber();
    if (R_FAILED(rc)) {
        LOG("Warning: Could not get serial number: 0x%x\n", rc);
    }

    LOG("Serial: %s\n", g_serialNumber);

    rc = CaptureInitialize();
    if (R_FAILED(rc)) {
        LOG("CaptureInitialize failed: 0x%x\n", rc);
        return rc;
    }

    g_coreInitialized = true;
    LOG("Core initialization complete\n");
    return 0;
}

void CoreExit(void)
{
    if (!g_coreInitialized) {
        return;
    }

    CaptureFinalize();

#if UDP_LOGGING
    if (g_logSocket >= 0) {
        close(g_logSocket);
        g_logSocket = -1;
    }
    socketExit();
#elif FILE_LOGGING
    _exitFileLogging();
    fsExit();
#endif

    g_coreInitialized = false;
}

// Thread creation failure is unrecoverable for this sysmodule: R_THROW
// fatals the console rather than leaving a half-started capture pipeline
void LaunchThread(Thread* t, ThreadFunc f, void* arg, void* stackLocation, u32 stackSize, u32 prio)
{
    Result rc = threadCreate(t, f, arg, stackLocation, stackSize, prio, -2);
    if (R_FAILED(rc)) {
        LOG("threadCreate failed: 0x%x\n", rc);
        R_THROW(rc);
    }

    rc = threadStart(t);
    if (R_FAILED(rc)) {
        LOG("threadStart failed: 0x%x\n", rc);
        R_THROW(rc);
    }
}

void JoinThread(Thread* t)
{
    threadWaitForExit(t);
    threadClose(t);
}

const char* CoreGetSerialNumber(void)
{
    return g_serialNumber;
}
