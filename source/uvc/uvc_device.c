#include <string.h>
#include "uvc_device.h"
#include "../core.h"

// USB class codes not defined in libnx
#ifndef USB_CLASS_MISC
#define USB_CLASS_MISC 0xEF
#endif

// USB Setup Packet (standard 8-byte format)
typedef struct __attribute__((packed)) {
    u8  bmRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} UsbSetupPacket;

#define USB_REQTYPE_DIR_MASK     0x80
#define USB_REQTYPE_DIR_TO_HOST  0x80

// ============================================================================
// Static buffers - must be aligned for USB DMA
// ============================================================================

static u8 alignas(0x1000) g_videoBuffer[0x60000];       // ~384KB for video frames
static u8 alignas(0x1000) g_controlBuffer[0x1000];     // 4KB for VS control transfers
static u8 alignas(0x1000) g_vcControlBuffer[0x1000];   // 4KB for VC control transfers (separate from VS)
static u8 alignas(0x1000) g_endpointInBuffer[0x4000];  // Video transfer staging buffer (must fit maxPayload + header)
static u8 alignas(0x1000) g_audioEndpointBuffer[0x1000]; // Audio transfer staging buffer

static UvcDeviceContext g_uvcCtx;

// UAC (Audio) context
static struct {
    bool initialized;
    bool streaming;

    UsbDsInterface* acInterface;    // Audio Control
    UsbDsInterface* asInterface;    // Audio Streaming
    UsbDsEndpoint* audioEndpointIn;

    Mutex audioMutex;
} g_uacCtx;

// ============================================================================
// USB Descriptor Data
// ============================================================================

// Interface Association Descriptor (groups VC and VS interfaces)
static struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bFirstInterface;
    u8  bInterfaceCount;
    u8  bFunctionClass;
    u8  bFunctionSubClass;
    u8  bFunctionProtocol;
    u8  iFunction;
} g_iadDescriptor = {
    .bLength = 8,
    .bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,
    .bFirstInterface = 0,
    .bInterfaceCount = 2,
    .bFunctionClass = USB_CLASS_VIDEO,
    .bFunctionSubClass = UVC_SC_VIDEO_INTERFACE_COLLECTION,
    .bFunctionProtocol = UVC_PC_PROTOCOL_UNDEFINED,
    .iFunction = 0,
};

// Video Control Interface Descriptor
static struct usb_interface_descriptor g_vcInterfaceDesc = {
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 0,  // No interrupt endpoint for simplicity
    .bInterfaceClass = USB_CLASS_VIDEO,
    .bInterfaceSubClass = UVC_SC_VIDEOCONTROL,
    .bInterfaceProtocol = UVC_PC_PROTOCOL_15,
    .iInterface = 0,
};

// Video Control Class-Specific Descriptors
static struct __attribute__((packed)) {
    // VC Header
    UvcVcHeaderDescriptor header;
    // Input Terminal (Camera). Must be the full 18-byte camera terminal:
    // Linux uvc_parse_standard_control() rejects the ENTIRE device (-EINVAL,
    // no /dev/video node) if an ITT_CAMERA terminal is shorter than
    // 15 + bControlSize bytes - the focal-length fields are not optional.
    UvcInputTerminalDescriptor inputTerminal;
    // Output Terminal (USB)
    UvcOutputTerminalDescriptor outputTerminal;
} g_vcDescriptors = {
    .header = {
        .bLength = sizeof(UvcVcHeaderDescriptor),
        .bDescriptorType = UVC_CS_INTERFACE,
        .bDescriptorSubType = UVC_VC_HEADER,
        .bcdUVC = 0x0150,  // UVC 1.5
        .wTotalLength = sizeof(g_vcDescriptors),
        .dwClockFrequency = UVC_CLOCK_FREQUENCY,
        .bInCollection = 1,
        .baInterfaceNr1 = 1,  // VideoStreaming interface
    },
    .inputTerminal = {
        .bLength = sizeof(UvcInputTerminalDescriptor),
        .bDescriptorType = UVC_CS_INTERFACE,
        .bDescriptorSubType = UVC_VC_INPUT_TERMINAL,
        .bTerminalID = UVC_ENTITY_INPUT_TERMINAL,
        .wTerminalType = UVC_ITT_CAMERA,
        .bAssocTerminal = 0,
        .iTerminal = 0,
        .wObjectiveFocalLengthMin = 0,
        .wObjectiveFocalLengthMax = 0,
        .wOcularFocalLength = 0,
        .bControlSize = 3,
        .bmControls = { 0, 0, 0 },  // No camera controls supported
    },
    .outputTerminal = {
        .bLength = sizeof(UvcOutputTerminalDescriptor),
        .bDescriptorType = UVC_CS_INTERFACE,
        .bDescriptorSubType = UVC_VC_OUTPUT_TERMINAL,
        .bTerminalID = UVC_ENTITY_OUTPUT_TERMINAL,
        .wTerminalType = UVC_TT_STREAMING,
        .bAssocTerminal = 0,
        .bSourceID = UVC_ENTITY_INPUT_TERMINAL,
        .iTerminal = 0,
    },
};

// Video Streaming Interface Descriptor (single alternate setting 0).
// For a BULK-transfer UVC stream the streaming endpoint MUST live in the
// default alternate setting (alt 0). The zero-bandwidth-alt0 / streaming-alt1
// split is an isochronous-only convention (iso endpoints reserve bus bandwidth
// and need a zero-bw setting to release it). Host bulk drivers (Linux uvcvideo,
// Windows usbvideo.sys) locate the bulk VS endpoint in alt 0 and never issue
// SET_INTERFACE for bulk - if the endpoint is hidden in alt 1 the host opens no
// pipe and no frames flow even though the device enumerates. This mirrors the
// proven libnx usbComms / SysDVR bulk USB mode (single interface, endpoint in
// the default setting).
static struct usb_interface_descriptor g_vsInterfaceDesc0 = {
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 1,
    .bAlternateSetting = 0,
    .bNumEndpoints = 1,  // Bulk video endpoint in the default alt setting
    .bInterfaceClass = USB_CLASS_VIDEO,
    .bInterfaceSubClass = UVC_SC_VIDEOSTREAMING,
    .bInterfaceProtocol = UVC_PC_PROTOCOL_UNDEFINED,
    .iInterface = 0,
};

// Video Streaming Class-Specific Descriptors
static struct __attribute__((packed)) {
    // VS Input Header
    UvcVsInputHeaderDescriptor inputHeader;
    // H.264 Format - VS_FORMAT_FRAME_BASED (UVC spec: 28 bytes)
    struct __attribute__((packed)) {
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
        u8  bVariableSize;
    } h264Format;
    // H.264 Frame - VS_FRAME_FRAME_BASED (UVC spec: 26 + 4*n bytes)
    struct __attribute__((packed)) {
        u8  bLength;
        u8  bDescriptorType;
        u8  bDescriptorSubType;
        u8  bFrameIndex;
        u8  bmCapabilities;
        u16 wWidth;
        u16 wHeight;
        u32 dwMinBitRate;
        u32 dwMaxBitRate;
        u32 dwDefaultFrameInterval;
        u8  bFrameIntervalType;
        u32 dwBytesPerLine;
        u32 dwFrameInterval1;
    } h264Frame;
    // Color Matching Descriptor
    UvcColorMatchingDescriptor colorMatching;
} g_vsDescriptors = {
    .inputHeader = {
        .bLength = sizeof(UvcVsInputHeaderDescriptor),
        .bDescriptorType = UVC_CS_INTERFACE,
        .bDescriptorSubType = UVC_VS_INPUT_HEADER,
        .bNumFormats = 1,
        .wTotalLength = sizeof(g_vsDescriptors),
        .bEndpointAddress = USB_ENDPOINT_IN | 1,
        .bmInfo = 0,
        .bTerminalLink = UVC_ENTITY_OUTPUT_TERMINAL,
        .bStillCaptureMethod = 0,
        .bTriggerSupport = 0,
        .bTriggerUsage = 0,
        .bControlSize = 1,
        .bmaControls1 = 0,
    },
    .h264Format = {
        .bLength = 28,  // VS_FORMAT_FRAME_BASED: 28 bytes per UVC spec
        .bDescriptorType = UVC_CS_INTERFACE,
        .bDescriptorSubType = UVC_VS_FORMAT_FRAME_BASED,
        .bFormatIndex = 1,
        .bNumFrameDescriptors = 1,
        .guidFormat = UVC_GUID_FORMAT_H264,
        .bBitsPerPixel = 0,         // variable for H.264
        .bDefaultFrameIndex = 1,
        .bAspectRatioX = 16,
        .bAspectRatioY = 9,
        .bmInterlaceFlags = 0,
        .bCopyProtect = 0,
        .bVariableSize = 1,
    },
    .h264Frame = {
        .bLength = 30,  // VS_FRAME_FRAME_BASED: 26 + 4*1 = 30 bytes (1 interval)
        .bDescriptorType = UVC_CS_INTERFACE,
        .bDescriptorSubType = UVC_VS_FRAME_FRAME_BASED,
        .bFrameIndex = 1,
        .bmCapabilities = 0,
        .wWidth = UVC_VIDEO_WIDTH,
        .wHeight = UVC_VIDEO_HEIGHT,
        .dwMinBitRate = UVC_MIN_BITRATE,
        .dwMaxBitRate = UVC_MAX_BITRATE,
        .dwDefaultFrameInterval = UVC_FRAME_INTERVAL_30FPS,
        .bFrameIntervalType = 1,
        .dwBytesPerLine = 0,        // variable for H.264
        .dwFrameInterval1 = UVC_FRAME_INTERVAL_30FPS,
    },
    .colorMatching = {
        .bLength = sizeof(UvcColorMatchingDescriptor),
        .bDescriptorType = UVC_CS_INTERFACE,
        .bDescriptorSubType = UVC_VS_COLORFORMAT,
        .bColorPrimaries = 1,           // BT.709 (sRGB)
        .bTransferCharacteristics = 1,  // BT.709
        .bMatrixCoefficients = 4,       // SMPTE 170M
    },
};

// Bulk endpoint for video streaming
static struct usb_endpoint_descriptor g_videoEndpointDesc = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_ENDPOINT_IN | 1,
    .bmAttributes = USB_TRANSFER_TYPE_BULK,
    .wMaxPacketSize = 512,  // High-speed bulk max
    .bInterval = 0,
};

// Super-speed endpoint companion
static struct usb_ss_endpoint_companion_descriptor g_ssCompanionDesc = {
    .bLength = sizeof(struct usb_ss_endpoint_companion_descriptor),
    .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
    .bMaxBurst = 15,
    .bmAttributes = 0,
    .wBytesPerInterval = 0,
};

// ============================================================================
// UAC (Audio) Descriptor Data - DEAD CODE, kept for reference only.
//
// UAC audio is impossible on usb:ds: its configuration assembler cannot
// handle an isochronous endpoint descriptor in the appended data. Appending
// these descriptors makes the device serve a corrupt configuration
// descriptor and enumeration fails entirely, taking video down with it
// (host: "config index 0 descriptor too short"). See docs/RESEARCH.md §6.
// ============================================================================

// Interface Association Descriptor for Audio
static const struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bFirstInterface;
    u8  bInterfaceCount;
    u8  bFunctionClass;
    u8  bFunctionSubClass;
    u8  bFunctionProtocol;
    u8  iFunction;
} g_audioIadDescriptor = {
    .bLength = 8,
    .bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,
    .bFirstInterface = 2,   // Will be updated at runtime
    .bInterfaceCount = 2,   // AC + AS interfaces
    .bFunctionClass = USB_CLASS_AUDIO,
    .bFunctionSubClass = UAC_SC_AUDIOCONTROL,
    .bFunctionProtocol = UAC_PC_PROTOCOL_UNDEFINED,
    .iFunction = 0,
};

// Audio Control Interface
static struct usb_interface_descriptor g_acInterfaceDesc = {
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 2,  // Will be updated at runtime
    .bAlternateSetting = 0,
    .bNumEndpoints = 0,
    .bInterfaceClass = USB_CLASS_AUDIO,
    .bInterfaceSubClass = UAC_SC_AUDIOCONTROL,
    .bInterfaceProtocol = UAC_PC_PROTOCOL_UNDEFINED,
    .iInterface = 0,
};

// Audio Control Class-Specific Descriptors
static const struct __attribute__((packed)) {
    // AC Header
    UacAcHeaderDescriptor header;
    // Input Terminal (from Switch)
    UacInputTerminalDescriptor inputTerminal;
    // Output Terminal (to USB/Host)
    UacOutputTerminalDescriptor outputTerminal;
} g_acDescriptors = {
    .header = {
        .bLength = sizeof(UacAcHeaderDescriptor),
        .bDescriptorType = UAC_CS_INTERFACE,
        .bDescriptorSubtype = UAC_AC_HEADER,
        .bcdADC = 0x0100,  // UAC 1.0
        .wTotalLength = sizeof(g_acDescriptors),
        .bInCollection = 1,
        .baInterfaceNr1 = 3,  // AS interface number (will be updated)
    },
    .inputTerminal = {
        .bLength = sizeof(UacInputTerminalDescriptor),
        .bDescriptorType = UAC_CS_INTERFACE,
        .bDescriptorSubtype = UAC_AC_INPUT_TERMINAL,
        .bTerminalID = UAC_ENTITY_INPUT_TERMINAL,
        .wTerminalType = UAC_ITT_MICROPHONE,  // Appears as microphone
        .bAssocTerminal = 0,
        .bNrChannels = UAC_CHANNELS,
        .wChannelConfig = UAC_CHANNEL_CONFIG_STEREO,
        .iChannelNames = 0,
        .iTerminal = 0,
    },
    .outputTerminal = {
        .bLength = sizeof(UacOutputTerminalDescriptor),
        .bDescriptorType = UAC_CS_INTERFACE,
        .bDescriptorSubtype = UAC_AC_OUTPUT_TERMINAL,
        .bTerminalID = UAC_ENTITY_OUTPUT_TERMINAL,
        .wTerminalType = UAC_TT_USB_STREAMING,
        .bAssocTerminal = 0,
        .bSourceID = UAC_ENTITY_INPUT_TERMINAL,
        .iTerminal = 0,
    },
};

// Audio Streaming Interface (Alt Setting 0 - zero bandwidth)
static struct usb_interface_descriptor g_asInterfaceDesc0 = {
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 3,  // Will be updated
    .bAlternateSetting = 0,
    .bNumEndpoints = 0,     // Zero bandwidth
    .bInterfaceClass = USB_CLASS_AUDIO,
    .bInterfaceSubClass = UAC_SC_AUDIOSTREAMING,
    .bInterfaceProtocol = UAC_PC_PROTOCOL_UNDEFINED,
    .iInterface = 0,
};

// Audio Streaming Interface (Alt Setting 1 - streaming)
static struct usb_interface_descriptor g_asInterfaceDesc1 = {
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 3,  // Will be updated
    .bAlternateSetting = 1,
    .bNumEndpoints = 1,
    .bInterfaceClass = USB_CLASS_AUDIO,
    .bInterfaceSubClass = UAC_SC_AUDIOSTREAMING,
    .bInterfaceProtocol = UAC_PC_PROTOCOL_UNDEFINED,
    .iInterface = 0,
};

// Audio Streaming Class-Specific Descriptors
static const struct __attribute__((packed)) {
    // AS General
    UacAsGeneralDescriptor general;
    // Format Type I
    UacFormatTypeIDescriptor formatType;
} g_asDescriptors = {
    .general = {
        .bLength = sizeof(UacAsGeneralDescriptor),
        .bDescriptorType = UAC_CS_INTERFACE,
        .bDescriptorSubtype = UAC_AS_GENERAL,
        .bTerminalLink = UAC_ENTITY_OUTPUT_TERMINAL,
        .bDelay = 1,
        .wFormatTag = UAC_FORMAT_TYPE_I_PCM,
    },
    .formatType = {
        .bLength = sizeof(UacFormatTypeIDescriptor),
        .bDescriptorType = UAC_CS_INTERFACE,
        .bDescriptorSubtype = UAC_AS_FORMAT_TYPE,
        .bFormatType = 1,  // FORMAT_TYPE_I
        .bNrChannels = UAC_CHANNELS,
        .bSubframeSize = UAC_SUBFRAME_SIZE,
        .bBitResolution = UAC_BIT_DEPTH,
        .bSamFreqType = 1,  // One discrete frequency
        .tSamFreq = {
            (UAC_SAMPLE_RATE >>  0) & 0xFF,
            (UAC_SAMPLE_RATE >>  8) & 0xFF,
            (UAC_SAMPLE_RATE >> 16) & 0xFF
        },
    },
};

// Audio isochronous endpoint. This is the exact descriptor usb:ds cannot
// assemble into a valid configuration (see section banner above).
static struct usb_endpoint_descriptor g_audioEndpointDesc = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_ENDPOINT_IN | 3,  // Will be updated
    .bmAttributes = USB_TRANSFER_TYPE_ISOCHRONOUS | UAC_ISO_SYNC_TYPE_ASYNC | UAC_ISO_USAGE_DATA,
    .wMaxPacketSize = UAC_ISO_PACKET_SIZE,    // 192 bytes = 1ms of 48kHz stereo 16-bit
    .bInterval = 4,                            // 2^(4-1) = 8 microframes = 1ms (high-speed)
};

// Audio endpoint class-specific descriptor
static const UacAsEndpointDescriptor g_audioEpDescriptor = {
    .bLength = sizeof(UacAsEndpointDescriptor),
    .bDescriptorType = UAC_CS_ENDPOINT,
    .bDescriptorSubtype = UAC_EP_GENERAL,
    .bmAttributes = 0,
    .bLockDelayUnits = 0,
    .wLockDelay = 0,
};

// ============================================================================
// Helper Functions
// ============================================================================

void UvcBuildDefaultProbeControl(UvcProbeCommitControl* ctrl, UvcVideoFormat format)
{
    memset(ctrl, 0, sizeof(UvcProbeCommitControl));

    ctrl->bmHint = 0x0001;  // dwFrameInterval field is valid
    ctrl->bFormatIndex = 1;
    ctrl->bFrameIndex = 1;
    ctrl->dwFrameInterval = UVC_FRAME_INTERVAL_30FPS;
    ctrl->wKeyFrameRate = 0;
    ctrl->wPFrameRate = 0;
    ctrl->wCompQuality = 0;
    ctrl->wCompWindowSize = 0;
    ctrl->wDelay = 0;
    ctrl->dwMaxVideoFrameSize = UVC_H264_MAX_FRAME_SIZE;
    ctrl->dwMaxPayloadTransferSize = 0x4000;  // 16KB chunks
    ctrl->dwClockFrequency = UVC_CLOCK_FREQUENCY;
    ctrl->bmFramingInfo = 0x03;  // Frame ID required, EOF required
    ctrl->bPreferedVersion = 1;
    ctrl->bMinVersion = 1;
    ctrl->bMaxVersion = 1;
}

u8 UvcBuildPayloadHeader(u8* header, u32 pts, bool eof, u8 frameId)
{
    (void)pts;

    // Minimal 2-byte header: EOH + FID/EOF only, NO PTS/SCR.
    // Linux uvcvideo delimits frames by the FID toggle + EOF bit and ignores
    // the payload clock fields, but Windows usbvideo.sys schedules frame
    // presentation against them - a device clock it cannot correlate to bus
    // time makes it queue frames defensively (seconds of capture latency).
    // Omitting PTS is fully spec-compliant and makes every host render
    // frames on arrival, which is exactly what a live capture device wants.
    header[0] = 2;
    header[1] = UVC_STREAM_HEADER_EOH;

    if (eof) {
        header[1] |= UVC_STREAM_HEADER_EOF;
    }

    if (frameId & 1) {
        header[1] |= UVC_STREAM_HEADER_FID;
    }

    return 2;
}

// ============================================================================
// USB Descriptor Setup (HOS 5.0+)
// ============================================================================

static Result _setupDescriptors5x(const UvcDeviceConfig* config)
{
    Result rc = 0;
    u8 iManufacturer, iProduct, iSerialNumber;

    static const u16 supportedLangs[] = { 0x0409 };  // English US

    rc = usbDsAddUsbLanguageStringDescriptor(NULL, supportedLangs, 1);
    if (R_FAILED(rc)) return rc;

    rc = usbDsAddUsbStringDescriptor(&iManufacturer, config->manufacturer);
    if (R_FAILED(rc)) return rc;

    rc = usbDsAddUsbStringDescriptor(&iProduct, config->product);
    if (R_FAILED(rc)) return rc;

    rc = usbDsAddUsbStringDescriptor(&iSerialNumber, config->serialNumber);
    if (R_FAILED(rc)) return rc;

    struct usb_device_descriptor deviceDesc = {
        .bLength = USB_DT_DEVICE_SIZE,
        .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = 0x0200,
        .bDeviceClass = USB_CLASS_MISC,      // Multi-interface device
        .bDeviceSubClass = 0x02,              // Common Class
        .bDeviceProtocol = 0x01,              // Interface Association
        .bMaxPacketSize0 = 64,
        .idVendor = config->vendorId,
        .idProduct = config->productId,
        .bcdDevice = 0x0100,
        .iManufacturer = iManufacturer,
        .iProduct = iProduct,
        .iSerialNumber = iSerialNumber,
        .bNumConfigurations = 1,
    };

    // High Speed (USB 2.0)
    deviceDesc.bcdUSB = 0x0200;
    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &deviceDesc);
    if (R_FAILED(rc)) return rc;

    // Super Speed (USB 3.0)
    deviceDesc.bcdUSB = 0x0300;
    deviceDesc.bMaxPacketSize0 = 9;  // 2^9 = 512 bytes
    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &deviceDesc);
    if (R_FAILED(rc)) return rc;

    // BOS Descriptor for USB 3.0
    static const u8 bosDescriptor[] = {
        0x05,                   // bLength
        USB_DT_BOS,             // bDescriptorType
        0x16, 0x00,             // wTotalLength
        0x02,                   // bNumDeviceCaps

        // USB 2.0 Extension
        0x07,                   // bLength
        USB_DT_DEVICE_CAPABILITY,
        0x02,                   // bDevCapabilityType (USB 2.0 Extension)
        0x02, 0x00, 0x00, 0x00, // bmAttributes (LPM supported)

        // SuperSpeed USB
        0x0A,                   // bLength
        USB_DT_DEVICE_CAPABILITY,
        0x03,                   // bDevCapabilityType (SuperSpeed)
        0x00,                   // bmAttributes
        0x0E, 0x00,             // wSpeedsSupported (FS, HS, SS)
        0x03,                   // bFunctionalitySupport (SS)
        0x00,                   // bU1DevExitLat
        0x00, 0x00,             // wU2DevExitLat
    };
    rc = usbDsSetBinaryObjectStore(bosDescriptor, sizeof(bosDescriptor));
    if (R_FAILED(rc)) return rc;

    return 0;
}

// ============================================================================
// Interface Setup
// ============================================================================

static Result _setupInterfaces5x(void)
{
    Result rc = 0;

    // Register Video Control interface FIRST (gets index 0).
    // The IAD must be appended on the first registered interface.
    rc = usbDsRegisterInterface(&g_uvcCtx.controlInterface);
    if (R_FAILED(rc)) return rc;

    u8 vcIfaceNum = g_uvcCtx.controlInterface->interface_index;
    LOG("VC interface registered (idx=%u)\n", vcIfaceNum);

    // Register Video Streaming interface SECOND (gets index 1)
    rc = usbDsRegisterInterface(&g_uvcCtx.streamingInterface);
    if (R_FAILED(rc)) return rc;

    u8 vsIfaceNum = g_uvcCtx.streamingInterface->interface_index;
    LOG("VS interface registered (idx=%u)\n", vsIfaceNum);

    // Update descriptor interface numbers
    g_iadDescriptor.bFirstInterface = vcIfaceNum;
    g_vcInterfaceDesc.bInterfaceNumber = vcIfaceNum;
    g_vcDescriptors.header.baInterfaceNr1 = vsIfaceNum;
    g_vsInterfaceDesc0.bInterfaceNumber = vsIfaceNum;
    g_videoEndpointDesc.bEndpointAddress = USB_ENDPOINT_IN | (vsIfaceNum + 1);
    g_vsDescriptors.inputHeader.bEndpointAddress = g_videoEndpointDesc.bEndpointAddress;

    // Append VC descriptors (with IAD - must be on the first registered interface)
    // UVC order: IAD → VC Interface → VC CS → VS Interface (alt0) → VS CS → VS bulk EP

    // High Speed VC (IAD + VC interface + VC class-specific)
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.controlInterface,
        UsbDeviceSpeed_High, &g_iadDescriptor, sizeof(g_iadDescriptor)));
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.controlInterface,
        UsbDeviceSpeed_High, &g_vcInterfaceDesc, USB_DT_INTERFACE_SIZE));
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.controlInterface,
        UsbDeviceSpeed_High, &g_vcDescriptors, sizeof(g_vcDescriptors)));

    // Super Speed VC
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.controlInterface,
        UsbDeviceSpeed_Super, &g_iadDescriptor, sizeof(g_iadDescriptor)));
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.controlInterface,
        UsbDeviceSpeed_Super, &g_vcInterfaceDesc, USB_DT_INTERFACE_SIZE));
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.controlInterface,
        UsbDeviceSpeed_Super, &g_vcDescriptors, sizeof(g_vcDescriptors)));

    R_RET_ON_FAIL(usbDsInterface_EnableInterface(g_uvcCtx.controlInterface));

    // Append VS descriptors (single alt 0: interface → VS class-specific → bulk EP)
    // High Speed VS
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.streamingInterface,
        UsbDeviceSpeed_High, &g_vsInterfaceDesc0, USB_DT_INTERFACE_SIZE));
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.streamingInterface,
        UsbDeviceSpeed_High, &g_vsDescriptors, sizeof(g_vsDescriptors)));
    g_videoEndpointDesc.wMaxPacketSize = 512;
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.streamingInterface,
        UsbDeviceSpeed_High, &g_videoEndpointDesc, USB_DT_ENDPOINT_SIZE));

    // Super Speed VS
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.streamingInterface,
        UsbDeviceSpeed_Super, &g_vsInterfaceDesc0, USB_DT_INTERFACE_SIZE));
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.streamingInterface,
        UsbDeviceSpeed_Super, &g_vsDescriptors, sizeof(g_vsDescriptors)));
    g_videoEndpointDesc.wMaxPacketSize = 1024;
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.streamingInterface,
        UsbDeviceSpeed_Super, &g_videoEndpointDesc, USB_DT_ENDPOINT_SIZE));
    R_RET_ON_FAIL(usbDsInterface_AppendConfigurationData(g_uvcCtx.streamingInterface,
        UsbDeviceSpeed_Super, &g_ssCompanionDesc, sizeof(g_ssCompanionDesc)));

    // Register the video endpoint on VS
    R_RET_ON_FAIL(usbDsInterface_RegisterEndpoint(g_uvcCtx.streamingInterface,
        &g_uvcCtx.videoEndpointIn, g_videoEndpointDesc.bEndpointAddress));

    R_RET_ON_FAIL(usbDsInterface_EnableInterface(g_uvcCtx.streamingInterface));
    LOG("VS interface enabled (idx=%u)\n", vsIfaceNum);

    return 0;
}

// ============================================================================
// UAC Interface Setup - DEAD CODE, never called.
//
// Do NOT wire this back in: appended configuration data cannot be removed,
// so once these descriptors are appended the whole device is broken even if
// the audio setup "fails gracefully" afterwards. See docs/RESEARCH.md §6.
// ============================================================================

static Result _setupAudioInterfaces5x(void)
{
    Result rc = 0;

    // Register Audio Control interface
    rc = usbDsRegisterInterface(&g_uacCtx.acInterface);
    if (R_FAILED(rc)) {
        LOG("Failed to register AC interface: 0x%x\n", rc);
        return rc;
    }

    u8 acIfaceNum = g_uacCtx.acInterface->interface_index;
    g_acInterfaceDesc.bInterfaceNumber = acIfaceNum;

    // Update IAD with actual interface number
    struct __attribute__((packed)) {
        u8  bLength;
        u8  bDescriptorType;
        u8  bFirstInterface;
        u8  bInterfaceCount;
        u8  bFunctionClass;
        u8  bFunctionSubClass;
        u8  bFunctionProtocol;
        u8  iFunction;
    } audioIad;
    memcpy(&audioIad, &g_audioIadDescriptor, sizeof(audioIad));
    audioIad.bFirstInterface = acIfaceNum;

    // High Speed AC descriptors
    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.acInterface,
        UsbDeviceSpeed_High, &audioIad, sizeof(audioIad));
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.acInterface,
        UsbDeviceSpeed_High, &g_acInterfaceDesc, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.acInterface,
        UsbDeviceSpeed_High, &g_acDescriptors, sizeof(g_acDescriptors));
    if (R_FAILED(rc)) return rc;

    // Super Speed AC descriptors
    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.acInterface,
        UsbDeviceSpeed_Super, &audioIad, sizeof(audioIad));
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.acInterface,
        UsbDeviceSpeed_Super, &g_acInterfaceDesc, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.acInterface,
        UsbDeviceSpeed_Super, &g_acDescriptors, sizeof(g_acDescriptors));
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_EnableInterface(g_uacCtx.acInterface);
    if (R_FAILED(rc)) return rc;

    // Register Audio Streaming interface
    rc = usbDsRegisterInterface(&g_uacCtx.asInterface);
    if (R_FAILED(rc)) {
        LOG("Failed to register AS interface: 0x%x\n", rc);
        return rc;
    }

    u8 asIfaceNum = g_uacCtx.asInterface->interface_index;
    g_asInterfaceDesc0.bInterfaceNumber = asIfaceNum;
    g_asInterfaceDesc1.bInterfaceNumber = asIfaceNum;
    g_audioEndpointDesc.bEndpointAddress = USB_ENDPOINT_IN | (asIfaceNum + 1);

    // High Speed AS descriptors

    // Alt setting 0 (zero bandwidth - no endpoint)
    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.asInterface,
        UsbDeviceSpeed_High, &g_asInterfaceDesc0, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;

    // Alt setting 1 (streaming with isochronous endpoint)
    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.asInterface,
        UsbDeviceSpeed_High, &g_asInterfaceDesc1, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.asInterface,
        UsbDeviceSpeed_High, &g_asDescriptors, sizeof(g_asDescriptors));
    if (R_FAILED(rc)) return rc;

    // Isochronous endpoint for high-speed: 192 bytes per 1ms frame
    g_audioEndpointDesc.wMaxPacketSize = UAC_ISO_PACKET_SIZE;
    g_audioEndpointDesc.bInterval = 4;  // 2^(4-1) = 8 microframes = 1ms
    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.asInterface,
        UsbDeviceSpeed_High, &g_audioEndpointDesc, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;

    // Audio endpoint class-specific descriptor
    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.asInterface,
        UsbDeviceSpeed_High, &g_audioEpDescriptor, sizeof(g_audioEpDescriptor));
    if (R_FAILED(rc)) return rc;

    // Super Speed AS descriptors

    // Alt setting 0 (zero bandwidth)
    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.asInterface,
        UsbDeviceSpeed_Super, &g_asInterfaceDesc0, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;

    // Alt setting 1 (streaming)
    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.asInterface,
        UsbDeviceSpeed_Super, &g_asInterfaceDesc1, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.asInterface,
        UsbDeviceSpeed_Super, &g_asDescriptors, sizeof(g_asDescriptors));
    if (R_FAILED(rc)) return rc;

    // Isochronous endpoint for super-speed
    g_audioEndpointDesc.wMaxPacketSize = UAC_ISO_PACKET_SIZE;
    g_audioEndpointDesc.bInterval = 4;  // 2^(4-1) = 8 microframes = 1ms
    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.asInterface,
        UsbDeviceSpeed_Super, &g_audioEndpointDesc, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) return rc;

    // SS companion for isochronous endpoint
    struct usb_ss_endpoint_companion_descriptor audioSsCompanion = {
        .bLength = sizeof(struct usb_ss_endpoint_companion_descriptor),
        .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst = 0,
        .bmAttributes = 0,
        .wBytesPerInterval = UAC_ISO_PACKET_SIZE,
    };
    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.asInterface,
        UsbDeviceSpeed_Super, &audioSsCompanion, sizeof(audioSsCompanion));
    if (R_FAILED(rc)) return rc;

    // Audio endpoint class-specific descriptor
    rc = usbDsInterface_AppendConfigurationData(g_uacCtx.asInterface,
        UsbDeviceSpeed_Super, &g_audioEpDescriptor, sizeof(g_audioEpDescriptor));
    if (R_FAILED(rc)) return rc;

    // Register audio endpoint
    rc = usbDsInterface_RegisterEndpoint(g_uacCtx.asInterface,
        &g_uacCtx.audioEndpointIn, g_audioEndpointDesc.bEndpointAddress);
    if (R_FAILED(rc)) return rc;

    rc = usbDsInterface_EnableInterface(g_uacCtx.asInterface);
    if (R_FAILED(rc)) return rc;

    g_uacCtx.initialized = true;
    LOG("UAC audio interface initialized\n");

    return 0;
}

// ============================================================================
// Public API Implementation
// ============================================================================

Result UvcDeviceInitialize(const UvcDeviceConfig* config)
{
    Result rc = 0;

    if (g_uvcCtx.initialized) {
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    }

    memset(&g_uvcCtx, 0, sizeof(g_uvcCtx));
    rwlockInit(&g_uvcCtx.lock);
    mutexInit(&g_uvcCtx.streamingMutex);

    rc = usbDsInitialize();
    if (R_FAILED(rc)) {
        LOG("usbDsInitialize failed: 0x%x\n", rc);
        return rc;
    }

    memset(&g_uacCtx, 0, sizeof(g_uacCtx));
    mutexInit(&g_uacCtx.audioMutex);

    if (hosversionAtLeast(5, 0, 0)) {
        rc = _setupDescriptors5x(config);
        if (R_FAILED(rc)) {
            LOG("_setupDescriptors5x failed: 0x%x\n", rc);
            goto cleanup;
        }

        rc = _setupInterfaces5x();
        if (R_FAILED(rc)) {
            LOG("_setupInterfaces5x failed: 0x%x\n", rc);
            goto cleanup;
        }

        // Audio is intentionally DISABLED: usb:ds cannot assemble a config
        // containing an isochronous endpoint - calling this corrupts the
        // entire configuration descriptor and the device stops enumerating.
        // See docs/RESEARCH.md §6 for the verdict and the bulk fallback plan.
        (void)_setupAudioInterfaces5x;  // suppress unused warning

        rc = usbDsEnable();
        if (R_FAILED(rc)) {
            LOG("usbDsEnable failed: 0x%x\n", rc);
            goto cleanup;
        }
    }
    else {
        // Pre-5.0 firmware not supported for UVC
        LOG("UVC requires HOS 5.0+\n");
        rc = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
        goto cleanup;
    }

    g_uvcCtx.videoBuffer = g_videoBuffer;
    g_uvcCtx.controlBuffer = g_controlBuffer;

    // Probe and commit start from the same defaults; the host overwrites
    // them during the probe/commit negotiation.
    UvcBuildDefaultProbeControl(&g_uvcCtx.probeControl, UVC_FORMAT_H264);
    memcpy(&g_uvcCtx.commitControl, &g_uvcCtx.probeControl, sizeof(UvcProbeCommitControl));

    g_uvcCtx.state = UVC_STATE_DISCONNECTED;
    g_uvcCtx.initialized = true;

    LOG("UVC device initialized\n");
    return 0;

cleanup:
    usbDsExit();
    return rc;
}

void UvcDeviceExit(void)
{
    if (!g_uvcCtx.initialized) {
        return;
    }

    rwlockWriteLock(&g_uvcCtx.lock);

    g_uvcCtx.state = UVC_STATE_DISCONNECTED;
    g_uacCtx.streaming = false;

    if (g_uacCtx.asInterface) {
        usbDsInterface_Close(g_uacCtx.asInterface);
        g_uacCtx.asInterface = NULL;
    }

    if (g_uacCtx.acInterface) {
        usbDsInterface_Close(g_uacCtx.acInterface);
        g_uacCtx.acInterface = NULL;
    }

    g_uacCtx.initialized = false;

    if (g_uvcCtx.streamingInterface) {
        usbDsInterface_Close(g_uvcCtx.streamingInterface);
        g_uvcCtx.streamingInterface = NULL;
    }
    if (g_uvcCtx.controlInterface) {
        usbDsInterface_Close(g_uvcCtx.controlInterface);
        g_uvcCtx.controlInterface = NULL;
    }

    usbDsExit();

    g_uvcCtx.initialized = false;

    rwlockWriteUnlock(&g_uvcCtx.lock);

    LOG("UVC/UAC device exited\n");
}

bool UvcDeviceIsConnected(void)
{
    if (!g_uvcCtx.initialized) return false;

    u32 state = 0;
    if (R_SUCCEEDED(usbDsGetState(&state))) {
        return state == UsbState_Configured;
    }
    return false;
}

bool UvcDeviceIsStreaming(void)
{
    return g_uvcCtx.state == UVC_STATE_STREAMING;
}

UvcVideoFormat UvcDeviceGetFormat(void)
{
    if (g_uvcCtx.state != UVC_STATE_STREAMING) {
        return 0;
    }
    return g_uvcCtx.activeFormat;
}

Result UvcDeviceWaitReady(u64 timeout)
{
    return usbDsWaitReady(timeout);
}

bool UvcDeviceSendFrame(const void* data, size_t size, u64 timestamp, bool isKeyframe)
{
    if (!g_uvcCtx.initialized || g_uvcCtx.state != UVC_STATE_STREAMING) {
        return false;
    }

    mutexLock(&g_uvcCtx.streamingMutex);

    Result rc;
    bool success = false;
    u32 urbId;
    UsbDsReportData reportData;

    // 90kHz PTS, no longer transmitted (the minimal payload header omits
    // PTS/SCR on purpose); computed only to keep the header-builder API.
    u32 pts = (u32)((timestamp * 90) / 1000);

    const u8* srcData = (const u8*)data;
    size_t remaining = size;

    // Keep every payload strictly below dwMaxPayloadTransferSize (0x4000):
    // a payload that never fills 16KB always terminates in a short packet,
    // which is what delimits payloads for the host's bulk parser. The -12
    // slack is inherited from the old 12-byte header and intentionally kept.
    const size_t maxPayload = 0x4000 - 12;

    while (remaining > 0) {
        size_t chunkSize = remaining > maxPayload ? maxPayload : remaining;
        bool isLastChunk = (remaining <= maxPayload);

        u8* dest = g_endpointInBuffer;
        u8 headerLen = UvcBuildPayloadHeader(dest, pts, isLastChunk, g_uvcCtx.frameId);

        memcpy(dest + headerLen, srcData, chunkSize);

        size_t totalSize = headerLen + chunkSize;

        eventClear(&g_uvcCtx.videoEndpointIn->CompletionEvent);  // drop stale signal
        rc = usbDsEndpoint_PostBufferAsync(g_uvcCtx.videoEndpointIn, dest, totalSize, &urbId);
        if (R_FAILED(rc)) {
            LOG("PostBufferAsync failed: 0x%x\n", rc);
            break;
        }

        rc = eventWait(&g_uvcCtx.videoEndpointIn->CompletionEvent, 1E+9);
        if (R_FAILED(rc)) {
            // The host is not reading right now. This is NOT a reliable stop
            // signal: OBS routinely stalls its buffer dequeuing for >1s
            // (scene switches, render hiccups) without closing the device -
            // declaring the stream stopped here freezes its feed permanently,
            // because a host that never closed the device never re-issues
            // probe/commit. So: cancel and REAP the stalled URB (cancel
            // without reaping followed by re-posting asserts usb:ds), drop
            // this frame, and STAY in STREAMING. If the host genuinely
            // stopped (CLEAR_FEATURE(HALT) is invisible to us), this just
            // idles at one reaped cancel per second until the next commit
            // or disconnect.
            usbDsEndpoint_Cancel(g_uvcCtx.videoEndpointIn);
            if (R_SUCCEEDED(eventWait(&g_uvcCtx.videoEndpointIn->CompletionEvent, 100E+6))) {
                eventClear(&g_uvcCtx.videoEndpointIn->CompletionEvent);
                usbDsEndpoint_GetReportData(g_uvcCtx.videoEndpointIn, &reportData);
            }
            LOG("Video transfer timeout - dropping frame\n");
            break;
        }
        eventClear(&g_uvcCtx.videoEndpointIn->CompletionEvent);

        rc = usbDsEndpoint_GetReportData(g_uvcCtx.videoEndpointIn, &reportData);
        if (R_FAILED(rc)) {
            LOG("GetReportData failed: 0x%x\n", rc);
            break;
        }

        u32 transferred;
        rc = usbDsParseReportData(&reportData, urbId, NULL, &transferred);
        if (R_FAILED(rc)) {
            LOG("ParseReportData failed: 0x%x\n", rc);
            break;
        }

        // A payload that is an exact multiple of the bulk packet size (512
        // at high speed) produces no short packet, so the host cannot see
        // the payload boundary and fuses it with the next payload - random
        // corrupted frames (~1 in 512 frames). Terminate it with a ZLP.
        if ((totalSize % 512) == 0) {
            u32 zlpUrbId;
            eventClear(&g_uvcCtx.videoEndpointIn->CompletionEvent);
            if (R_SUCCEEDED(usbDsEndpoint_PostBufferAsync(
                    g_uvcCtx.videoEndpointIn, dest, 0, &zlpUrbId))) {
                if (R_SUCCEEDED(eventWait(&g_uvcCtx.videoEndpointIn->CompletionEvent, 1E+9))) {
                    eventClear(&g_uvcCtx.videoEndpointIn->CompletionEvent);
                    usbDsEndpoint_GetReportData(g_uvcCtx.videoEndpointIn, &reportData);
                } else {
                    usbDsEndpoint_Cancel(g_uvcCtx.videoEndpointIn);
                    if (R_SUCCEEDED(eventWait(&g_uvcCtx.videoEndpointIn->CompletionEvent, 100E+6))) {
                        eventClear(&g_uvcCtx.videoEndpointIn->CompletionEvent);
                        usbDsEndpoint_GetReportData(g_uvcCtx.videoEndpointIn, &reportData);
                    }
                    break;
                }
            }
        }

        srcData += chunkSize;
        remaining -= chunkSize;

        if (isLastChunk) {
            success = true;
        }
    }

    // Toggle the frame ID even when the frame was aborted mid-way: an
    // aborted frame never got its EOF, and re-using the same FID makes the
    // host APPEND the next frame to the unterminated one - merged corrupted
    // frames that stall the decoder, fill the host buffers, time out our
    // next transfer and snowball into permanent lag. A FID flip without EOF
    // instead makes uvcvideo close the bad frame immediately (one corrupted
    // frame, then clean).
    g_uvcCtx.frameId ^= 1;
    g_uvcCtx.frameNumber++;

    mutexUnlock(&g_uvcCtx.streamingMutex);
    return success;
}

// ============================================================================
// UVC Control Request Handler (EP0)
//
// usb:ds EP0 rules (mirrors ams::usb::DsInterface, Nintendo's own client
// library reimplemented in Atmosphère libstratosphere/source/usb/usb_device.cpp):
//  - Only touch EP0 inside the SetupEvent -> GetSetupPacket window. Posting
//    a CtrlIn/CtrlOut buffer with no pending setup drives the usb sysmodule
//    into an internal assertion (User Break) and crashes the whole console.
//  - The status stage is NOT automatic: a GET (data IN) must be completed
//    with a zero-length OUT, a SET (data OUT) with a zero-length IN.
//  - On any failure, StallCtrl (protocol-stalls EP0 in both directions).
// ============================================================================

#define EP0_XFER_TIMEOUT 1E+9  // 1s per stage

// VC Request Error Code Control (UVC 1.5 §4.2.1.2). Hosts read this after a
// stalled class request to learn why it failed.
static u8 g_lastErrorCode = UVC_ERROR_NONE;

static u8* _ctrlBufferFor(UsbDsInterface* iface)
{
    return (iface == g_uvcCtx.controlInterface) ? g_vcControlBuffer : g_controlBuffer;
}

// One EP0 IN transfer (device->host). Data must already be in buf.
static Result _ctrlInXfer(UsbDsInterface* iface, u8* buf, size_t len, u32* transferred)
{
    eventClear(&iface->CtrlInCompletionEvent);  // drop any stale signal

    u32 urbId;
    Result rc = usbDsInterface_CtrlInPostBufferAsync(iface, buf, len, &urbId);
    if (R_FAILED(rc)) return rc;

    rc = eventWait(&iface->CtrlInCompletionEvent, EP0_XFER_TIMEOUT);
    if (R_FAILED(rc)) return rc;
    eventClear(&iface->CtrlInCompletionEvent);

    UsbDsReportData rpt;
    rc = usbDsInterface_GetCtrlInReportData(iface, &rpt);
    if (R_FAILED(rc)) return rc;

    u32 t = 0;
    rc = usbDsParseReportData(&rpt, urbId, NULL, &t);
    if (transferred) *transferred = t;
    return rc;
}

// One EP0 OUT transfer (host->device). Data lands in buf.
static Result _ctrlOutXfer(UsbDsInterface* iface, u8* buf, size_t len, u32* transferred)
{
    eventClear(&iface->CtrlOutCompletionEvent);

    u32 urbId;
    Result rc = usbDsInterface_CtrlOutPostBufferAsync(iface, buf, len, &urbId);
    if (R_FAILED(rc)) return rc;

    rc = eventWait(&iface->CtrlOutCompletionEvent, EP0_XFER_TIMEOUT);
    if (R_FAILED(rc)) return rc;
    eventClear(&iface->CtrlOutCompletionEvent);

    UsbDsReportData rpt;
    rc = usbDsInterface_GetCtrlOutReportData(iface, &rpt);
    if (R_FAILED(rc)) return rc;

    u32 t = 0;
    rc = usbDsParseReportData(&rpt, urbId, NULL, &t);
    if (transferred) *transferred = t;
    return rc;
}

// GET-type request: data IN stage + zero-length OUT status stage.
static void _ctrlWrite(UsbDsInterface* iface, const void* data, size_t len)
{
    u8* buf = _ctrlBufferFor(iface);
    memcpy(buf, data, len);

    Result rc = _ctrlInXfer(iface, buf, len, NULL);
    if (R_SUCCEEDED(rc))
        rc = _ctrlOutXfer(iface, buf, 0, NULL);  // status stage

    if (R_FAILED(rc)) {
        LOG("EP0 write failed: 0x%x\n", rc);
        usbDsInterface_StallCtrl(iface);
    }
}

// SET-type request: data OUT stage + zero-length IN status stage.
// Fills data (up to len bytes) and returns true on success.
static bool _ctrlRead(UsbDsInterface* iface, void* data, size_t len)
{
    u8* buf = _ctrlBufferFor(iface);

    u32 transferred = 0;
    Result rc = _ctrlOutXfer(iface, buf, len, &transferred);
    if (R_SUCCEEDED(rc))
        rc = _ctrlInXfer(iface, buf, 0, NULL);  // status stage

    if (R_FAILED(rc)) {
        LOG("EP0 read failed: 0x%x\n", rc);
        usbDsInterface_StallCtrl(iface);
        return false;
    }

    memcpy(data, buf, transferred < len ? transferred : len);
    return true;
}

// Request with no data stage: zero-length IN status stage only.
static void _ctrlAck(UsbDsInterface* iface)
{
    if (R_FAILED(_ctrlInXfer(iface, _ctrlBufferFor(iface), 0, NULL)))
        usbDsInterface_StallCtrl(iface);
}

static void _ctrlStallError(UsbDsInterface* iface, u8 errorCode)
{
    g_lastErrorCode = errorCode;
    usbDsInterface_StallCtrl(iface);
}

// VideoStreaming interface class requests: Probe/Commit negotiation.
// Linux uvcvideo sequence: GET_DEF, SET_CUR(probe), GET_CUR(probe),
// GET_MIN, GET_MAX, SET_CUR(probe), GET_CUR(probe), SET_CUR(commit).
// GET_CUR must return the full 48-byte UVC 1.5 struct or the host bails.
// Windows additionally issues GET_INFO and GET_LEN.
static void _handleVsSetup(UsbDsInterface* iface, const UsbSetupPacket* setup)
{
    u8 cs = (setup->wValue >> 8) & 0xFF;

    if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL) {
        LOG("VS: unsupported cs=%u -> stall\n", cs);
        _ctrlStallError(iface, UVC_ERROR_INVALID_CONTROL);
        return;
    }

    UvcProbeCommitControl* ctrl = (cs == UVC_VS_PROBE_CONTROL)
        ? &g_uvcCtx.probeControl
        : &g_uvcCtx.commitControl;

    switch (setup->bRequest) {
    case UVC_GET_CUR:
    case UVC_GET_MIN:
    case UVC_GET_MAX: {
        // Single fixed mode: MIN == MAX == CUR
        size_t len = setup->wLength;
        if (len > sizeof(UvcProbeCommitControl))
            len = sizeof(UvcProbeCommitControl);
        _ctrlWrite(iface, ctrl, len);
        break;
    }
    case UVC_GET_DEF: {
        UvcProbeCommitControl def;
        UvcBuildDefaultProbeControl(&def, UVC_FORMAT_H264);
        size_t len = setup->wLength;
        if (len > sizeof(def))
            len = sizeof(def);
        _ctrlWrite(iface, &def, len);
        break;
    }
    case UVC_GET_INFO: {
        u8 info = 0x03;  // supports GET and SET
        _ctrlWrite(iface, &info, 1);
        break;
    }
    case UVC_GET_LEN: {
        u16 len = sizeof(UvcProbeCommitControl);  // 48 for UVC 1.5
        _ctrlWrite(iface, &len, setup->wLength < 2 ? setup->wLength : 2);
        break;
    }
    case UVC_SET_CUR: {
        UvcProbeCommitControl host;
        memset(&host, 0, sizeof(host));
        size_t len = setup->wLength;
        if (len > sizeof(host))
            len = sizeof(host);
        if (!_ctrlRead(iface, &host, len))
            return;

        // Negotiate: we offer exactly one format/frame/payload size, so
        // override whatever the host asked for with our fixed parameters.
        host.bFormatIndex = 1;
        host.bFrameIndex = 1;
        host.dwMaxVideoFrameSize = UVC_H264_MAX_FRAME_SIZE;
        host.dwMaxPayloadTransferSize = 0x4000;  // matches g_endpointInBuffer
        host.dwClockFrequency = UVC_CLOCK_FREQUENCY;
        if (host.dwFrameInterval == 0)
            host.dwFrameInterval = UVC_FRAME_INTERVAL_30FPS;
        memcpy(ctrl, &host, sizeof(UvcProbeCommitControl));

        if (cs == UVC_VS_COMMIT_CONTROL) {
            g_uvcCtx.activeFormat = UVC_FORMAT_H264;
            g_uvcCtx.state = UVC_STATE_STREAMING;
            g_uacCtx.streaming = g_uacCtx.initialized;
            LOG("Commit received - streaming started\n");
        } else {
            LOG("Probe SET_CUR: interval=%u\n", (unsigned)host.dwFrameInterval);
        }
        break;
    }
    default:
        LOG("VS: unsupported bRequest=0x%02x -> stall\n", setup->bRequest);
        _ctrlStallError(iface, UVC_ERROR_INVALID_REQUEST);
        return;
    }

    g_lastErrorCode = UVC_ERROR_NONE;
}

// VideoControl interface class requests. We advertise no camera/terminal
// controls (bmControls all zero), so only the interface-level controls
// (entity 0 in the high byte of wIndex) need real answers.
static void _handleVcSetup(UsbDsInterface* iface, const UsbSetupPacket* setup)
{
    u8 entity = (setup->wIndex >> 8) & 0xFF;
    u8 cs = (setup->wValue >> 8) & 0xFF;

    if (entity == 0) {
        if (cs == UVC_VC_REQUEST_ERROR_CODE_CONTROL) {
            // Must support GET_CUR + GET_INFO and never stall (hosts read
            // this right after any other request stalls).
            if (setup->bRequest == UVC_GET_CUR) {
                _ctrlWrite(iface, &g_lastErrorCode, 1);
                return;
            }
            if (setup->bRequest == UVC_GET_INFO) {
                u8 info = 0x01;  // GET only
                _ctrlWrite(iface, &info, 1);
                return;
            }
        }
        else if (cs == UVC_VC_VIDEO_POWER_MODE_CONTROL) {
            if (setup->bRequest == UVC_GET_CUR) {
                u8 power = 0x00;  // full power
                _ctrlWrite(iface, &power, 1);
                g_lastErrorCode = UVC_ERROR_NONE;
                return;
            }
            if (setup->bRequest == UVC_SET_CUR) {
                u8 power = 0;
                if (_ctrlRead(iface, &power, 1))
                    g_lastErrorCode = UVC_ERROR_NONE;
                return;
            }
            if (setup->bRequest == UVC_GET_INFO) {
                u8 info = 0x03;
                _ctrlWrite(iface, &info, 1);
                g_lastErrorCode = UVC_ERROR_NONE;
                return;
            }
        }
        LOG("VC: unsupported cs=%u req=0x%02x -> stall\n", cs, setup->bRequest);
        _ctrlStallError(iface, UVC_ERROR_INVALID_CONTROL);
        return;
    }

    LOG("VC: entity=%u cs=%u req=0x%02x -> stall\n", entity, cs, setup->bRequest);
    _ctrlStallError(iface, (entity == UVC_ENTITY_INPUT_TERMINAL ||
                            entity == UVC_ENTITY_OUTPUT_TERMINAL)
        ? UVC_ERROR_INVALID_CONTROL : UVC_ERROR_INVALID_UNIT);
}

// Standard request codes that may be forwarded to us on EP0
#define USB_REQUEST_GET_INTERFACE 0x0A
#define USB_REQUEST_SET_INTERFACE 0x0B

static void _handleSetupPacket(UsbDsInterface* iface)
{
    UsbSetupPacket setup;
    Result rc = usbDsInterface_GetSetupPacket(iface, &setup, sizeof(setup));
    if (R_FAILED(rc)) {
        LOG("GetSetupPacket failed: 0x%x\n", rc);
        return;
    }

    LOG("EP0[%u]: reqType=0x%02x req=0x%02x wVal=0x%04x wIdx=0x%04x wLen=%u\n",
        iface->interface_index, setup.bmRequestType, setup.bRequest,
        setup.wValue, setup.wIndex, setup.wLength);

    u8 type = setup.bmRequestType & 0x60;       // 0x00 standard, 0x20 class
    u8 recipient = setup.bmRequestType & 0x1F;  // 0x01 = interface

    // Only answer requests addressed to this interface. If usb:ds ever
    // signals the same request on both interfaces' SetupEvents, this keeps
    // exactly one of them responding.
    if (recipient == 0x01 &&
        (setup.wIndex & 0xFF) != iface->interface_index) {
        LOG("EP0[%u]: wIndex mismatch, ignoring\n", iface->interface_index);
        return;
    }

    if (type == 0x20 && recipient == 0x01) {  // class request to interface
        if (iface == g_uvcCtx.streamingInterface)
            _handleVsSetup(iface, &setup);
        else
            _handleVcSetup(iface, &setup);
        return;
    }

    if (type == 0x00) {  // standard request (usb:ds handles most itself)
        if (setup.bRequest == USB_REQUEST_SET_INTERFACE) {
            // Single alt setting: accept alt 0, reject anything else.
            // Linux sends SET_INTERFACE(0) once at device probe time and
            // tolerates a stall, but accepting is cleaner.
            if (setup.wValue == 0)
                _ctrlAck(iface);
            else
                usbDsInterface_StallCtrl(iface);
            return;
        }
        if (setup.bRequest == USB_REQUEST_GET_INTERFACE &&
            (setup.bmRequestType & USB_REQTYPE_DIR_MASK) == USB_REQTYPE_DIR_TO_HOST) {
            u8 alt = 0;
            _ctrlWrite(iface, &alt, 1);
            return;
        }
    }

    LOG("EP0[%u]: unhandled request -> stall\n", iface->interface_index);
    usbDsInterface_StallCtrl(iface);
}

// ============================================================================
// Process USB Events
// ============================================================================

// Check one interface's SetupEvent and service the request if one is pending.
static bool _serviceSetup(UsbDsInterface* iface)
{
    if (!iface)
        return false;
    if (R_FAILED(eventWait(&iface->SetupEvent, 0)))
        return false;
    eventClear(&iface->SetupEvent);
    _handleSetupPacket(iface);
    return true;
}

void UvcDeviceProcessRequests(void)
{
    u32 state = 0;
    if (R_FAILED(usbDsGetState(&state))) {
        return;
    }

    if (state != UsbState_Configured) {
        if (g_uvcCtx.state != UVC_STATE_DISCONNECTED) {
            LOG("USB host disconnected\n");
            g_uvcCtx.state = UVC_STATE_DISCONNECTED;
            g_uacCtx.streaming = false;
            g_lastErrorCode = UVC_ERROR_NONE;
        }
        return;
    }

    if (g_uvcCtx.state == UVC_STATE_DISCONNECTED) {
        LOG("USB host connected\n");
        g_uvcCtx.state = UVC_STATE_CONNECTED;
        g_uvcCtx.frameId = 0;
        g_uvcCtx.frameNumber = 0;
        g_lastErrorCode = UVC_ERROR_NONE;
    }

    // Service pending setup requests. The host's probe/commit negotiation
    // arrives as a burst of EP0 requests; after servicing one, briefly wait
    // for the follow-up so the sequence isn't paced by our caller's sleep.
    bool serviced = _serviceSetup(g_uvcCtx.controlInterface);
    serviced |= _serviceSetup(g_uvcCtx.streamingInterface);

    while (serviced &&
           g_uvcCtx.controlInterface && g_uvcCtx.streamingInterface) {
        s32 idx;
        if (R_FAILED(waitMulti(&idx, 20E+6,  // 20ms
                waiterForEvent(&g_uvcCtx.controlInterface->SetupEvent),
                waiterForEvent(&g_uvcCtx.streamingInterface->SetupEvent))))
            break;  // no follow-up request - burst is over

        serviced = _serviceSetup(g_uvcCtx.controlInterface);
        serviced |= _serviceSetup(g_uvcCtx.streamingInterface);
    }
}

// ============================================================================
// UAC (Audio) Implementation
// ============================================================================

bool UacDeviceIsStreaming(void)
{
    return g_uacCtx.initialized && g_uacCtx.streaming;
}

bool UacDeviceSendAudio(const void* data, size_t size, u64 timestamp)
{
    (void)timestamp;

    if (!g_uacCtx.initialized || !g_uacCtx.streaming) {
        return false;
    }

    if (!g_uacCtx.audioEndpointIn) {
        return false;
    }

    mutexLock(&g_uacCtx.audioMutex);

    Result rc;
    bool success = true;
    u32 urbId;
    UsbDsReportData reportData;

    const u8* srcData = (const u8*)data;
    size_t remaining = size;

    // 192-byte packets = 1ms of 48kHz stereo 16-bit, matching the endpoint's
    // wMaxPacketSize and bInterval
    const size_t isoPacketSize = UAC_ISO_PACKET_SIZE;

    while (remaining > 0 && success) {
        size_t chunkSize = remaining >= isoPacketSize ? isoPacketSize : remaining;

        // Pad partial tail packets with silence
        memset(g_audioEndpointBuffer, 0, isoPacketSize);
        memcpy(g_audioEndpointBuffer, srcData, chunkSize);

        eventClear(&g_uacCtx.audioEndpointIn->CompletionEvent);  // drop stale signal
        rc = usbDsEndpoint_PostBufferAsync(g_uacCtx.audioEndpointIn,
            g_audioEndpointBuffer, isoPacketSize, &urbId);
        if (R_FAILED(rc)) {
            success = false;
            break;
        }

        // Iso packets go out on the next 1ms service interval; no completion
        // within 10ms means the host is not polling the endpoint.
        rc = eventWait(&g_uacCtx.audioEndpointIn->CompletionEvent, 10E+6);
        if (R_FAILED(rc)) {
            // Cancel AND reap the cancelled URB's report - cancelling without
            // reaping and then re-posting drives usb:ds into an assertion.
            usbDsEndpoint_Cancel(g_uacCtx.audioEndpointIn);
            if (R_SUCCEEDED(eventWait(&g_uacCtx.audioEndpointIn->CompletionEvent, 100E+6))) {
                eventClear(&g_uacCtx.audioEndpointIn->CompletionEvent);
                usbDsEndpoint_GetReportData(g_uacCtx.audioEndpointIn, &reportData);
            }
            success = false;
            break;
        }
        eventClear(&g_uacCtx.audioEndpointIn->CompletionEvent);

        rc = usbDsEndpoint_GetReportData(g_uacCtx.audioEndpointIn, &reportData);
        if (R_FAILED(rc)) {
            success = false;
            break;
        }

        u32 transferred;
        rc = usbDsParseReportData(&reportData, urbId, NULL, &transferred);
        if (R_FAILED(rc)) {
            success = false;
            break;
        }

        srcData += chunkSize;
        remaining -= chunkSize;

        // Small delay to maintain timing (approximately 1ms per packet)
        // This helps maintain proper audio timing
        if (remaining > 0) {
            svcSleepThread(800000);  // ~0.8ms - slightly less than 1ms to account for processing
        }
    }

    mutexUnlock(&g_uacCtx.audioMutex);
    return success;
}
