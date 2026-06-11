#pragma once
#include <switch.h>

// ============================================================================
// Buffer Sizes
// ============================================================================

// Video buffer size - higher than switchbrew suggestion to handle large IDR frames
#define VbufSz 0x54000

// Audio buffer size (16-bit PCM, 48kHz stereo, 1024 samples)
#define AbufSz 0x1000

// ============================================================================
// Video Frame Data
// ============================================================================

typedef struct {
    u8* Data;           // Pointer to H.264 NAL unit data
    u32 DataSize;       // Size of the NAL unit
    u64 Timestamp;      // Timestamp in microseconds
    bool IsKeyframe;    // True if this is an IDR frame
} VideoFrame;

// ============================================================================
// SPS/PPS Constants (Nintendo Switch H.264 parameters)
// ============================================================================

extern const u8 SPS[];
extern const u8 PPS[];
extern const size_t SPS_SIZE;
extern const size_t PPS_SIZE;

// ============================================================================
// Capture API
// ============================================================================

/**
 * Initialize the capture system (opens grc:d service)
 * @return Result code
 */
Result CaptureInitialize(void);

/**
 * Cleanup capture system
 */
void CaptureFinalize(void);

/**
 * Read the next video frame from grc:d
 * @param frame Output frame structure
 * @return true if frame was successfully read
 * @note This blocks until a frame is available
 */
bool CaptureReadVideoFrame(VideoFrame* frame);

/**
 * Get the internal video buffer for direct access
 * @return Pointer to the video buffer
 */
u8* CaptureGetVideoBuffer(void);

/**
 * Configure whether to inject SPS/PPS into keyframes
 * @param inject true to inject SPS/PPS periodically
 */
void CaptureSetInjectSPSPPS(bool inject);

// ============================================================================
// Audio Capture API
// ============================================================================

/**
 * Audio frame data
 */
typedef struct {
    u8* Data;           // PCM audio data (16-bit stereo 48kHz)
    u32 DataSize;       // Size in bytes
    u64 Timestamp;      // Timestamp in microseconds
} AudioFrame;

/**
 * Read the next audio chunk from grc:d
 * @param frame Output frame structure
 * @return true if audio was successfully read
 * @note This blocks until audio is available
 */
bool CaptureReadAudioFrame(AudioFrame* frame);

/**
 * Get the internal audio buffer for direct access
 * @return Pointer to the audio buffer
 */
u8* CaptureGetAudioBuffer(void);
