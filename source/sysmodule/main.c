#include <string.h>
#include <switch.h>

#include "../core.h"
#include "../capture.h"
#include "../uvc/uvc_device.h"
#include "../tap_client.h"
#include "../native/native_probe.h"

// ============================================================================
// libnx Sysmodule Configuration
// ============================================================================

// Required: Tell libnx we're a sysmodule, not an applet/application
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
u32 __nx_fsdev_direntry_cache_size = 1;

// Heap for libnx usbDs internals (transfer memory allocation)
#define INNER_HEAP_SIZE (128 * 1024)
static char nx_inner_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void)
{
    extern char* fake_heap_start;
    extern char* fake_heap_end;

    fake_heap_start = nx_inner_heap;
    fake_heap_end = nx_inner_heap + INNER_HEAP_SIZE;
}

// ============================================================================
// Thread Configuration
// ============================================================================

#define VIDEO_THREAD_STACK_SIZE (0x4000 + LOGGING_STACK_BOOST)
#define AUDIO_THREAD_STACK_SIZE (0x2000 + LOGGING_STACK_BOOST)
#define STREAM_THREAD_PRIORITY  0x2C

static Thread g_videoThread;
static Thread g_audioThread;
static u8 alignas(0x1000) g_videoThreadStack[VIDEO_THREAD_STACK_SIZE];
static u8 alignas(0x1000) g_audioThreadStack[AUDIO_THREAD_STACK_SIZE];

static volatile bool g_running = true;

#ifdef TAP_PROBE
// Phase 1 spike: reads vi-tap's present records and logs them (no capture)
#define TAP_PROBE_STACK_SIZE (0x2000 + LOGGING_STACK_BOOST)
#define TAP_PROBE_PRIORITY   0x3B
static Thread g_tapProbeThread;
static u8 alignas(0x1000) g_tapProbeStack[TAP_PROBE_STACK_SIZE];
volatile bool g_tapProbeRunning = true;
#endif

#ifdef NATIVE_PROBE
// Phase 2a: attach to the game, find its swapchain, read and dump frames
#define NATIVE_PROBE_STACK_SIZE (0x4000 + LOGGING_STACK_BOOST)
#define NATIVE_PROBE_PRIORITY   0x3B
static Thread g_nativeProbeThread;
static u8 alignas(0x1000) g_nativeProbeStack[NATIVE_PROBE_STACK_SIZE];
volatile bool g_nativeProbeRunning = true;
extern volatile bool g_nativeStreaming;  // native_probe.c: native frames are going out
#endif

// ============================================================================
// USB Device Configuration
// ============================================================================

// Our own USB identity rather than a borrowed one. Driverless operation does
// not depend on it (UVC binding is class-based on every OS), but the IDs are
// not harmless: macOS merges vendor-wide overrides from UVCMatching.plist, and
// its Logitech entry (0x046D, any PID) hides the H.264 format - the only one we
// advertise - which left earlier builds with zero formats and invisible to
// QuickTime, OBS and AVFoundation.
#define UVC_VENDOR_ID   0x1209  // pid.codes (open-source hardware VID)
#define UVC_PRODUCT_ID  0xCA57  // SysDVR-UVC Capture

// ============================================================================
// Video Streaming Thread
// ============================================================================

static void VideoStreamThread(void* arg)
{
    (void)arg;

    LOG("Video stream thread started\n");

    while (g_running) {
        Result rc = UvcDeviceWaitReady(1E+9);
        if (R_SUCCEEDED(rc)) {
            break;
        }
        svcSleepThread(100E+6);  // 100ms
    }

    LOG("USB ready, starting video capture loop\n");

    VideoFrame frame;

    while (g_running) {
        // EP0 requests are serviced ONLY from this thread - the EP0 helpers
        // are synchronous and share buffers, so a second caller would race.
        UvcDeviceProcessRequests();

        if (!UvcDeviceIsStreaming()) {
            svcSleepThread(50E+6);  // 50ms
            continue;
        }

#ifdef NATIVE_PROBE
        // the native capture sends the frames: grc:d stays idle, this thread
        // keeps serving EP0
        if (g_nativeStreaming) {
            svcSleepThread(5E+6);
            continue;
        }
#endif

        // Blocks until grc:d produces a frame - this is the loop's pacing
        if (!CaptureReadVideoFrame(&frame)) {
            svcSleepThread(16E+6);  // ~one frame
            continue;
        }

#ifdef NATIVE_PROBE
        // the native stream started while grc:d was blocked: drop this frame
        // (a grc:d P frame in the native stream would corrupt it until the
        // next keyframe)
        if (g_nativeStreaming)
            continue;
#endif

        bool sent = UvcDeviceSendFrame(
            frame.Data,
            frame.DataSize,
            frame.Timestamp,
            frame.IsKeyframe);

        if (!sent) {
            LOG("Failed to send video frame\n");
        }
    }

    LOG("Video stream thread exiting\n");
}

// ============================================================================
// Audio Streaming Thread
// ============================================================================

// NOTE: audio is currently disabled (UAC/isochronous is impossible on
// usb:ds, see docs/RESEARCH.md §6) - UacDeviceIsStreaming() is always false
// and this thread just idles in its 50ms sleep branch.
static void AudioStreamThread(void* arg)
{
    (void)arg;

    LOG("Audio stream thread started\n");

    // Wait for USB and video to be ready first
    while (g_running && !UvcDeviceIsStreaming()) {
        svcSleepThread(100E+6);  // 100ms
    }

    LOG("Starting audio capture loop\n");

    AudioFrame frame;
    u64 lastAudioTime = 0;

    while (g_running) {
        // Check if audio streaming is active
        if (!UacDeviceIsStreaming()) {
            svcSleepThread(50E+6);  // 50ms
            lastAudioTime = 0;
            continue;
        }

        // Capture audio from grc:d
        // grc:d returns 0x1000 bytes = 1024 stereo samples = ~21.3ms of audio
        // Note: This blocks until audio is available
        if (!CaptureReadAudioFrame(&frame)) {
            // Capture failed, wait a bit and retry
            svcSleepThread(10E+6);  // 10ms
            continue;
        }

        // Track timing for smooth audio
        u64 currentTime = frame.Timestamp;
        if (lastAudioTime != 0) {
            // Calculate expected gap (should be ~21.3ms for 0x1000 bytes)
            s64 gap = (s64)(currentTime - lastAudioTime);
            if (gap < 0) gap = 0;

            // If we're getting audio too fast, slow down
            // This shouldn't happen normally but helps prevent buffer overruns
            if (gap < 15000) {  // Less than 15ms since last frame
                svcSleepThread((20000 - gap) * 1000);  // Sleep to make up the difference
            }
        }
        lastAudioTime = currentTime;

        // Send audio via UAC (this internally paces the 192-byte isochronous packets)
        UacDeviceSendAudio(frame.Data, frame.DataSize, frame.Timestamp);

        // Audio send failures during streaming are handled internally
        // No need to check return value as UAC handles timing
    }

    LOG("Audio stream thread exiting\n");
}

// ============================================================================
// Sysmodule Lifecycle
// ============================================================================

// Called before main() to initialize services
void __appInit(void)
{
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc)) {
        fatalThrow(rc);
    }

    // First log line before the wait below: shows that this instance started
    // at all (a restart from ovlSysmodules is otherwise invisible for 20 s)
    CoreEarlyLog();

    // Wait out the system boot (grc:d and usb:ds are not usable right after
    // boot2 launches us; probing them too early fails or fatals)
    svcSleepThread(20E+9);

    // hosversionSet is required for the HOS 5.0+ checks in usbDs setup
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        }
        setsysExit();
    }

    rc = CoreInit();
    if (R_FAILED(rc)) {
        fatalThrow(rc);
    }
}

// Called after main() returns to cleanup
void __appExit(void)
{
    g_running = false;
    UvcDeviceExit();
    CoreExit();
    smExit();
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    LOG("SysDVR-UVC starting...\n");

    UvcDeviceConfig uvcConfig = {
        .vendorId = UVC_VENDOR_ID,
        .productId = UVC_PRODUCT_ID,
        .manufacturer = "Nintendo Switch",
        .product = "SysDVR-UVC Capture",
        .serialNumber = CoreGetSerialNumber(),
    };

    Result rc = UvcDeviceInitialize(&uvcConfig);
    if (R_FAILED(rc)) {
        LOG("UvcDeviceInitialize failed: 0x%x\n", rc);
        return 1;
    }

    // The grc:d stream has no parameter sets; hosts cannot decode without
    // the injected SPS/PPS
    CaptureSetInjectSPSPPS(true);

    LaunchThread(&g_videoThread, VideoStreamThread, NULL,
                 g_videoThreadStack, VIDEO_THREAD_STACK_SIZE,
                 STREAM_THREAD_PRIORITY);

    // Launch audio streaming thread
    LaunchThread(&g_audioThread, AudioStreamThread, NULL,
                 g_audioThreadStack, AUDIO_THREAD_STACK_SIZE,
                 STREAM_THREAD_PRIORITY);

#ifdef TAP_PROBE
    LaunchThread(&g_tapProbeThread, TapProbeThread, NULL,
                 g_tapProbeStack, TAP_PROBE_STACK_SIZE,
                 TAP_PROBE_PRIORITY);
#endif
#ifdef NATIVE_PROBE
    LaunchThread(&g_nativeProbeThread, NativeProbeThread, NULL,
                 g_nativeProbeStack, NATIVE_PROBE_STACK_SIZE,
                 NATIVE_PROBE_PRIORITY);
#endif

    LOG("SysDVR-UVC running (video + audio)\n");

    // Main thread just sleeps - all work is done in streaming threads
    while (g_running) {
        svcSleepThread(1E+9);  // 1 second
    }

    g_running = false;
    JoinThread(&g_videoThread);
    JoinThread(&g_audioThread);
#ifdef TAP_PROBE
    g_tapProbeRunning = false;
    JoinThread(&g_tapProbeThread);
#endif
#ifdef NATIVE_PROBE
    g_nativeProbeRunning = false;
    JoinThread(&g_nativeProbeThread);
#endif
    UvcDeviceExit();

    LOG("SysDVR-UVC exiting\n");
    return 0;
}
