#pragma once
#include <switch.h>
#include "uvc_descriptors.h"
#include "uac_descriptors.h"

// ============================================================================
// UVC Device State
// ============================================================================

typedef enum {
    UVC_STATE_DISCONNECTED = 0,
    UVC_STATE_CONNECTED,
    UVC_STATE_STREAMING,
} UvcDeviceState;

typedef enum {
    UVC_FORMAT_H264 = 1,
    UVC_FORMAT_YUY2 = 2,
} UvcVideoFormat;

// ============================================================================
// UVC Device Configuration
// ============================================================================

typedef struct {
    u16 vendorId;
    u16 productId;
    const char* manufacturer;
    const char* product;
    const char* serialNumber;
} UvcDeviceConfig;

// ============================================================================
// UVC Device Context
// ============================================================================

typedef struct {
    UvcDeviceState state;
    UvcVideoFormat activeFormat;

    // Current streaming parameters
    UvcProbeCommitControl probeControl;
    UvcProbeCommitControl commitControl;

    // USB interfaces
    UsbDsInterface* controlInterface;
    UsbDsInterface* streamingInterface;

    // Endpoints
    UsbDsEndpoint* videoEndpointIn;
    UsbDsEndpoint* interruptEndpointIn;  // For status interrupts (optional)

    // Buffers (must be 0x1000-aligned for USB DMA)
    u8* videoBuffer;
    u8* controlBuffer;

    // Frame tracking
    u8 frameId;                 // Toggles between frames (FID bit)
    u32 frameNumber;
    u64 presentationTime;

    // Synchronization
    RwLock lock;
    Mutex streamingMutex;

    bool initialized;
} UvcDeviceContext;

// ============================================================================
// Public API
// ============================================================================

/**
 * Initialize the UVC device subsystem
 * @param config Device configuration (VID/PID, strings)
 * @return Result code
 */
Result UvcDeviceInitialize(const UvcDeviceConfig* config);

/**
 * Cleanup and release UVC device resources
 */
void UvcDeviceExit(void);

/**
 * Check if a USB host is connected
 * @return true if connected
 */
bool UvcDeviceIsConnected(void);

/**
 * Check if actively streaming video
 * @return true if streaming
 */
bool UvcDeviceIsStreaming(void);

/**
 * Get the current video format requested by host
 * @return Active format or 0 if not streaming
 */
UvcVideoFormat UvcDeviceGetFormat(void);

/**
 * Send a video frame to the USB host
 * @param data Video frame data (H.264 NAL or raw pixels)
 * @param size Size of the frame data
 * @param timestamp Presentation timestamp in microseconds
 * @param isKeyframe True if this is a keyframe (for H.264)
 * @return true if successfully sent
 */
bool UvcDeviceSendFrame(const void* data, size_t size, u64 timestamp, bool isKeyframe);

/**
 * Service pending EP0 control requests and track connection state.
 * Must be called periodically, and ONLY from one thread (the video thread):
 * the EP0 handling is synchronous and shares static buffers.
 */
void UvcDeviceProcessRequests(void);

/**
 * Wait for USB device to be ready
 * @param timeout Timeout in nanoseconds
 * @return Result code
 */
Result UvcDeviceWaitReady(u64 timeout);

// ============================================================================
// Internal helpers (exposed for testing)
// ============================================================================

/**
 * Build default probe control response
 * @param ctrl Probe control structure to fill
 * @param format Requested format
 */
void UvcBuildDefaultProbeControl(UvcProbeCommitControl* ctrl, UvcVideoFormat format);

/**
 * Build a minimal UVC payload header (EOH + FID/EOF, no PTS/SCR - see the
 * implementation for why timestamps are deliberately omitted)
 * @param header Header buffer (2 bytes used)
 * @param pts Ignored (kept for API stability)
 * @param eof End of frame flag
 * @param frameId Frame ID toggle bit
 * @return Header length
 */
u8 UvcBuildPayloadHeader(u8* header, u32 pts, bool eof, u8 frameId);

// ============================================================================
// UAC (Audio) Support
// ============================================================================

/**
 * Check if UAC audio streaming is active
 * @return true if streaming
 */
bool UacDeviceIsStreaming(void);

/**
 * Send audio data to the USB host
 * @param data PCM audio data (16-bit stereo 48kHz)
 * @param size Size in bytes
 * @param timestamp Timestamp in microseconds
 * @return true if successfully sent
 */
bool UacDeviceSendAudio(const void* data, size_t size, u64 timestamp);
