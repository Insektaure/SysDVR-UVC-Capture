#pragma once
#include <switch.h>

// USB Video Class (UVC) Descriptor Definitions
// Based on USB Video Class 1.5 Specification

// ============================================================================
// UVC Class Codes
// ============================================================================

#define USB_CLASS_VIDEO                     0x0E

// UVC Subclass Codes
#define UVC_SC_UNDEFINED                    0x00
#define UVC_SC_VIDEOCONTROL                 0x01
#define UVC_SC_VIDEOSTREAMING               0x02
#define UVC_SC_VIDEO_INTERFACE_COLLECTION   0x03

// UVC Protocol Codes
#define UVC_PC_PROTOCOL_UNDEFINED           0x00
#define UVC_PC_PROTOCOL_15                  0x01

// ============================================================================
// UVC Descriptor Types
// ============================================================================

#define UVC_CS_UNDEFINED                    0x20
#define UVC_CS_DEVICE                       0x21
#define UVC_CS_CONFIGURATION                0x22
#define UVC_CS_STRING                       0x23
#define UVC_CS_INTERFACE                    0x24
#define UVC_CS_ENDPOINT                     0x25

// ============================================================================
// VideoControl Interface Descriptor Subtypes
// ============================================================================

#define UVC_VC_DESCRIPTOR_UNDEFINED         0x00
#define UVC_VC_HEADER                       0x01
#define UVC_VC_INPUT_TERMINAL               0x02
#define UVC_VC_OUTPUT_TERMINAL              0x03
#define UVC_VC_SELECTOR_UNIT                0x04
#define UVC_VC_PROCESSING_UNIT              0x05
#define UVC_VC_EXTENSION_UNIT               0x06
#define UVC_VC_ENCODING_UNIT                0x07

// ============================================================================
// VideoStreaming Interface Descriptor Subtypes
// ============================================================================

#define UVC_VS_UNDEFINED                    0x00
#define UVC_VS_INPUT_HEADER                 0x01
#define UVC_VS_OUTPUT_HEADER                0x02
#define UVC_VS_STILL_IMAGE_FRAME            0x03
#define UVC_VS_FORMAT_UNCOMPRESSED          0x04
#define UVC_VS_FRAME_UNCOMPRESSED           0x05
#define UVC_VS_FORMAT_MJPEG                 0x06
#define UVC_VS_FRAME_MJPEG                  0x07
#define UVC_VS_FORMAT_MPEG2TS               0x0A
#define UVC_VS_FORMAT_DV                    0x0C
#define UVC_VS_COLORFORMAT                  0x0D
#define UVC_VS_FORMAT_FRAME_BASED           0x10
#define UVC_VS_FRAME_FRAME_BASED            0x11
#define UVC_VS_FORMAT_STREAM_BASED          0x12
#define UVC_VS_FORMAT_H264                  0x13
#define UVC_VS_FRAME_H264                   0x14
#define UVC_VS_FORMAT_H264_SIMULCAST        0x15
#define UVC_VS_FORMAT_VP8                   0x16
#define UVC_VS_FRAME_VP8                    0x17
#define UVC_VS_FORMAT_VP8_SIMULCAST         0x18

// ============================================================================
// Terminal Types
// ============================================================================

// USB Terminal Types
#define UVC_TT_VENDOR_SPECIFIC              0x0100
#define UVC_TT_STREAMING                    0x0101

// Input Terminal Types
#define UVC_ITT_VENDOR_SPECIFIC             0x0200
#define UVC_ITT_CAMERA                      0x0201
#define UVC_ITT_MEDIA_TRANSPORT_INPUT       0x0202

// Output Terminal Types
#define UVC_OTT_VENDOR_SPECIFIC             0x0300
#define UVC_OTT_DISPLAY                     0x0301
#define UVC_OTT_MEDIA_TRANSPORT_OUTPUT      0x0302

// ============================================================================
// UVC Request Codes
// ============================================================================

#define UVC_RC_UNDEFINED                    0x00
#define UVC_SET_CUR                         0x01
#define UVC_SET_CUR_ALL                     0x11
#define UVC_GET_CUR                         0x81
#define UVC_GET_MIN                         0x82
#define UVC_GET_MAX                         0x83
#define UVC_GET_RES                         0x84
#define UVC_GET_LEN                         0x85
#define UVC_GET_INFO                        0x86
#define UVC_GET_DEF                         0x87
#define UVC_GET_CUR_ALL                     0x91
#define UVC_GET_MIN_ALL                     0x92
#define UVC_GET_MAX_ALL                     0x93
#define UVC_GET_RES_ALL                     0x94
#define UVC_GET_DEF_ALL                     0x97

// ============================================================================
// VideoControl Interface Control Selectors
// ============================================================================

#define UVC_VC_CONTROL_UNDEFINED            0x00
#define UVC_VC_VIDEO_POWER_MODE_CONTROL     0x01
#define UVC_VC_REQUEST_ERROR_CODE_CONTROL   0x02

// Request Error Code Control values (UVC 1.5 §4.2.1.2)
#define UVC_ERROR_NONE                      0x00
#define UVC_ERROR_NOT_READY                 0x01
#define UVC_ERROR_WRONG_STATE               0x02
#define UVC_ERROR_OUT_OF_RANGE              0x04
#define UVC_ERROR_INVALID_UNIT              0x05
#define UVC_ERROR_INVALID_CONTROL           0x06
#define UVC_ERROR_INVALID_REQUEST           0x07

// ============================================================================
// VideoStreaming Interface Control Selectors
// ============================================================================

#define UVC_VS_CONTROL_UNDEFINED            0x00
#define UVC_VS_PROBE_CONTROL                0x01
#define UVC_VS_COMMIT_CONTROL               0x02
#define UVC_VS_STILL_PROBE_CONTROL          0x03
#define UVC_VS_STILL_COMMIT_CONTROL         0x04
#define UVC_VS_STILL_IMAGE_TRIGGER_CONTROL  0x05
#define UVC_VS_STREAM_ERROR_CODE_CONTROL    0x06
#define UVC_VS_GENERATE_KEY_FRAME_CONTROL   0x07
#define UVC_VS_UPDATE_FRAME_SEGMENT_CONTROL 0x08
#define UVC_VS_SYNCH_DELAY_CONTROL          0x09

// ============================================================================
// Video Payload Header Bits
// ============================================================================

#define UVC_STREAM_HEADER_FID               (1 << 0)
#define UVC_STREAM_HEADER_EOF               (1 << 1)
#define UVC_STREAM_HEADER_PTS               (1 << 2)
#define UVC_STREAM_HEADER_SCR               (1 << 3)
#define UVC_STREAM_HEADER_RES               (1 << 4)
#define UVC_STREAM_HEADER_STI               (1 << 5)
#define UVC_STREAM_HEADER_ERR               (1 << 6)
#define UVC_STREAM_HEADER_EOH               (1 << 7)

// ============================================================================
// Video Format GUIDs
// ============================================================================

// YUY2 (YUYV 4:2:2) - Widely supported uncompressed format
#define UVC_GUID_FORMAT_YUY2 \
    { 'Y', 'U', 'Y', '2', 0x00, 0x00, 0x10, 0x00, \
      0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }

// NV12 - Common on Switch
#define UVC_GUID_FORMAT_NV12 \
    { 'N', 'V', '1', '2', 0x00, 0x00, 0x10, 0x00, \
      0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }

// H.264 Format GUID (UVC 1.5 extension)
#define UVC_GUID_FORMAT_H264 \
    { 'H', '2', '6', '4', 0x00, 0x00, 0x10, 0x00, \
      0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }

// ============================================================================
// UVC Descriptor Structures
// ============================================================================

// Video Control Interface Header Descriptor
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubType;
    u16 bcdUVC;
    u16 wTotalLength;
    u32 dwClockFrequency;
    u8  bInCollection;
    u8  baInterfaceNr1;
} UvcVcHeaderDescriptor;

// Input Terminal Descriptor (Camera)
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubType;
    u8  bTerminalID;
    u16 wTerminalType;
    u8  bAssocTerminal;
    u8  iTerminal;
    // Camera-specific fields (optional, for ITT_CAMERA)
    u16 wObjectiveFocalLengthMin;
    u16 wObjectiveFocalLengthMax;
    u16 wOcularFocalLength;
    u8  bControlSize;
    u8  bmControls[3];
} UvcInputTerminalDescriptor;

// Output Terminal Descriptor
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubType;
    u8  bTerminalID;
    u16 wTerminalType;
    u8  bAssocTerminal;
    u8  bSourceID;
    u8  iTerminal;
} UvcOutputTerminalDescriptor;

// Video Streaming Input Header Descriptor
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubType;
    u8  bNumFormats;
    u16 wTotalLength;
    u8  bEndpointAddress;
    u8  bmInfo;
    u8  bTerminalLink;
    u8  bStillCaptureMethod;
    u8  bTriggerSupport;
    u8  bTriggerUsage;
    u8  bControlSize;
    u8  bmaControls1;
} UvcVsInputHeaderDescriptor;

// Format Uncompressed Descriptor
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubType;
    u8  bFormatIndex;
    u8  bNumFrameDescriptors;
    u8  guidFormat[16];
    u8  bBitsPerPixel;
    u8  bDefaultFrameIndex;
    u8  bAspectRatioX;
    u8  bAspectRatioY;
    u8  bmInterlaceFlags;
    u8  bCopyProtect;
} UvcFormatUncompressedDescriptor;

// Frame Uncompressed Descriptor
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubType;
    u8  bFrameIndex;
    u8  bmCapabilities;
    u16 wWidth;
    u16 wHeight;
    u32 dwMinBitRate;
    u32 dwMaxBitRate;
    u32 dwMaxVideoFrameBufferSize;
    u32 dwDefaultFrameInterval;
    u8  bFrameIntervalType;
    u32 dwFrameInterval1;
} UvcFrameUncompressedDescriptor;

// H.264 Format Descriptor (UVC 1.5)
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubType;
    u8  bFormatIndex;
    u8  bNumFrameDescriptors;
    u8  bDefaultFrameIndex;
    u8  bMaxCodecConfigDelay;
    u8  bmSupportedSliceModes;
    u8  bmSupportedSyncFrameTypes;
    u8  bResolutionScaling;
    u8  Reserved1;
    u8  bmSupportedRateControlModes;
    u16 wMaxMBperSecOneResolutionNoScalability;
    u16 wMaxMBperSecTwoResolutionsNoScalability;
    u16 wMaxMBperSecThreeResolutionsNoScalability;
    u16 wMaxMBperSecFourResolutionsNoScalability;
    u16 wMaxMBperSecOneResolutionTemporalScalability;
    u16 wMaxMBperSecTwoResolutionsTemporalScalability;
    u16 wMaxMBperSecThreeResolutionsTemporalScalability;
    u16 wMaxMBperSecFourResolutionsTemporalScalability;
    u16 wMaxMBperSecOneResolutionTemporalQualityScalability;
    u16 wMaxMBperSecTwoResolutionsTemporalQualityScalability;
    u16 wMaxMBperSecThreeResolutionsTemporalQualityScalability;
    u16 wMaxMBperSecFourResolutionsTemporalQualityScalability;
    u16 wMaxMBperSecOneResolutionTemporalSpatialScalability;
    u16 wMaxMBperSecTwoResolutionsTemporalSpatialScalability;
    u16 wMaxMBperSecThreeResolutionsTemporalSpatialScalability;
    u16 wMaxMBperSecFourResolutionsTemporalSpatialScalability;
    u16 wMaxMBperSecOneResolutionFullScalability;
    u16 wMaxMBperSecTwoResolutionsFullScalability;
    u16 wMaxMBperSecThreeResolutionsFullScalability;
    u16 wMaxMBperSecFourResolutionsFullScalability;
} UvcFormatH264Descriptor;

// H.264 Frame Descriptor (UVC 1.5)
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubType;
    u8  bFrameIndex;
    u16 wWidth;
    u16 wHeight;
    u16 wSARwidth;
    u16 wSARheight;
    u16 wProfile;
    u8  bLevelIDC;
    u16 wConstrainedToolset;
    u32 bmSupportedUsages;
    u16 bmCapabilities;
    u32 bmSVCCapabilities;
    u32 bmMVCCapabilities;
    u32 dwMinBitRate;
    u32 dwMaxBitRate;
    u32 dwDefaultFrameInterval;
    u8  bNumFrameIntervals;
    u32 dwFrameInterval1;
} UvcFrameH264Descriptor;

// Color Matching Descriptor
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubType;
    u8  bColorPrimaries;
    u8  bTransferCharacteristics;
    u8  bMatrixCoefficients;
} UvcColorMatchingDescriptor;

// Video Probe/Commit Control Structure
typedef struct __attribute__((packed)) {
    u16 bmHint;
    u8  bFormatIndex;
    u8  bFrameIndex;
    u32 dwFrameInterval;
    u16 wKeyFrameRate;
    u16 wPFrameRate;
    u16 wCompQuality;
    u16 wCompWindowSize;
    u16 wDelay;
    u32 dwMaxVideoFrameSize;
    u32 dwMaxPayloadTransferSize;
    u32 dwClockFrequency;
    u8  bmFramingInfo;
    u8  bPreferedVersion;
    u8  bMinVersion;
    u8  bMaxVersion;
    // UVC 1.5 additions
    u8  bUsage;
    u8  bBitDepthLuma;
    u8  bmSettings;
    u8  bMaxNumberOfRefFramesPlus1;
    u16 bmRateControlModes;
    u64 bmLayoutPerStream;
} UvcProbeCommitControl;

// UVC Payload Header (for bulk transfers)
typedef struct __attribute__((packed)) {
    u8 bHeaderLength;
    u8 bmHeaderInfo;
    // Optional fields follow based on bmHeaderInfo flags
} UvcPayloadHeader;

// Extended payload header with PTS and SCR
typedef struct __attribute__((packed)) {
    u8  bHeaderLength;
    u8  bmHeaderInfo;
    u32 dwPresentationTime;  // PTS if bmHeaderInfo & UVC_STREAM_HEADER_PTS
    u8  scrSourceClock[6];   // SCR if bmHeaderInfo & UVC_STREAM_HEADER_SCR
} UvcPayloadHeaderExtended;

// ============================================================================
// Video Resolution Constants (Nintendo Switch)
// ============================================================================

#define UVC_VIDEO_WIDTH         1280
#define UVC_VIDEO_HEIGHT        720
#define UVC_VIDEO_FPS           30

// Frame interval in 100ns units (30fps = 333333)
#define UVC_FRAME_INTERVAL_30FPS    333333
#define UVC_FRAME_INTERVAL_60FPS    166666

// Bit rates (approximations for H.264 720p30)
#define UVC_MIN_BITRATE         5000000     // 5 Mbps
#define UVC_MAX_BITRATE         15000000    // 15 Mbps

// YUY2 frame size: 1280 * 720 * 2 bytes per pixel
#define UVC_YUY2_FRAME_SIZE     (UVC_VIDEO_WIDTH * UVC_VIDEO_HEIGHT * 2)

// H.264 max frame size (NAL unit buffer)
#define UVC_H264_MAX_FRAME_SIZE 0x54000

// Clock frequency (90kHz standard for video)
#define UVC_CLOCK_FREQUENCY     90000

// ============================================================================
// Entity IDs
// ============================================================================

#define UVC_ENTITY_INPUT_TERMINAL   1
#define UVC_ENTITY_OUTPUT_TERMINAL  2
#define UVC_ENTITY_PROCESSING_UNIT  3

// Interface Numbers
#define UVC_INTERFACE_CONTROL       0
#define UVC_INTERFACE_STREAMING     1
