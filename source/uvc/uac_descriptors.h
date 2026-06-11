#pragma once
#include <switch.h>

// USB Audio Class (UAC) Descriptor Definitions
// Based on USB Audio Class 1.0 Specification (for broad compatibility)

// ============================================================================
// UAC Class Codes
// ============================================================================

#define USB_CLASS_AUDIO                     0x01

// UAC Subclass Codes
#define UAC_SC_UNDEFINED                    0x00
#define UAC_SC_AUDIOCONTROL                 0x01
#define UAC_SC_AUDIOSTREAMING               0x02
#define UAC_SC_MIDISTREAMING                0x03

// UAC Protocol Codes
#define UAC_PC_PROTOCOL_UNDEFINED           0x00

// ============================================================================
// UAC Descriptor Types
// ============================================================================

#define UAC_CS_UNDEFINED                    0x20
#define UAC_CS_DEVICE                       0x21
#define UAC_CS_CONFIGURATION                0x22
#define UAC_CS_STRING                       0x23
#define UAC_CS_INTERFACE                    0x24
#define UAC_CS_ENDPOINT                     0x25

// ============================================================================
// AudioControl Interface Descriptor Subtypes
// ============================================================================

#define UAC_AC_DESCRIPTOR_UNDEFINED         0x00
#define UAC_AC_HEADER                       0x01
#define UAC_AC_INPUT_TERMINAL               0x02
#define UAC_AC_OUTPUT_TERMINAL              0x03
#define UAC_AC_MIXER_UNIT                   0x04
#define UAC_AC_SELECTOR_UNIT                0x05
#define UAC_AC_FEATURE_UNIT                 0x06
#define UAC_AC_PROCESSING_UNIT              0x07
#define UAC_AC_EXTENSION_UNIT               0x08

// ============================================================================
// AudioStreaming Interface Descriptor Subtypes
// ============================================================================

#define UAC_AS_DESCRIPTOR_UNDEFINED         0x00
#define UAC_AS_GENERAL                      0x01
#define UAC_AS_FORMAT_TYPE                  0x02
#define UAC_AS_FORMAT_SPECIFIC              0x03

// ============================================================================
// Audio Data Format Type I Codes
// ============================================================================

#define UAC_FORMAT_TYPE_I_UNDEFINED         0x0000
#define UAC_FORMAT_TYPE_I_PCM               0x0001
#define UAC_FORMAT_TYPE_I_PCM8              0x0002
#define UAC_FORMAT_TYPE_I_IEEE_FLOAT        0x0003
#define UAC_FORMAT_TYPE_I_ALAW              0x0004
#define UAC_FORMAT_TYPE_I_MULAW             0x0005

// ============================================================================
// Audio Terminal Types
// ============================================================================

// USB Terminal Types
#define UAC_TT_USB_UNDEFINED                0x0100
#define UAC_TT_USB_STREAMING                0x0101
#define UAC_TT_USB_VENDOR_SPECIFIC          0x01FF

// Input Terminal Types
#define UAC_ITT_UNDEFINED                   0x0200
#define UAC_ITT_MICROPHONE                  0x0201
#define UAC_ITT_DESKTOP_MICROPHONE          0x0202
#define UAC_ITT_PERSONAL_MICROPHONE         0x0203
#define UAC_ITT_OMNI_DIR_MICROPHONE         0x0204
#define UAC_ITT_MICROPHONE_ARRAY            0x0205
#define UAC_ITT_PROC_MICROPHONE_ARRAY       0x0206

// Output Terminal Types
#define UAC_OTT_UNDEFINED                   0x0300
#define UAC_OTT_SPEAKER                     0x0301
#define UAC_OTT_HEADPHONES                  0x0302
#define UAC_OTT_HEAD_MOUNTED_DISPLAY        0x0303
#define UAC_OTT_DESKTOP_SPEAKER             0x0304
#define UAC_OTT_ROOM_SPEAKER                0x0305
#define UAC_OTT_COMMUNICATION_SPEAKER       0x0306
#define UAC_OTT_LOW_FREQ_SPEAKER            0x0307

// ============================================================================
// Audio Endpoint Descriptor Subtypes
// ============================================================================

#define UAC_EP_GENERAL                      0x01

// ============================================================================
// Audio Endpoint Control Selectors
// ============================================================================

#define UAC_EP_CS_UNDEFINED                 0x00
#define UAC_EP_CS_SAMPLING_FREQ_CONTROL     0x01
#define UAC_EP_CS_PITCH_CONTROL             0x02

// ============================================================================
// Feature Unit Control Selectors
// ============================================================================

#define UAC_FU_CONTROL_UNDEFINED            0x00
#define UAC_FU_MUTE_CONTROL                 0x01
#define UAC_FU_VOLUME_CONTROL               0x02
#define UAC_FU_BASS_CONTROL                 0x03
#define UAC_FU_MID_CONTROL                  0x04
#define UAC_FU_TREBLE_CONTROL               0x05
#define UAC_FU_GRAPHIC_EQUALIZER_CONTROL    0x06
#define UAC_FU_AUTOMATIC_GAIN_CONTROL       0x07
#define UAC_FU_DELAY_CONTROL                0x08
#define UAC_FU_BASS_BOOST_CONTROL           0x09
#define UAC_FU_LOUDNESS_CONTROL             0x0A

// ============================================================================
// UAC Descriptor Structures
// ============================================================================

// Audio Control Interface Header Descriptor (UAC 1.0)
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubtype;
    u16 bcdADC;             // Audio Device Class spec release (0x0100)
    u16 wTotalLength;       // Total size of class-specific descriptors
    u8  bInCollection;      // Number of streaming interfaces
    u8  baInterfaceNr1;     // First streaming interface number
} UacAcHeaderDescriptor;

// Input Terminal Descriptor
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubtype;
    u8  bTerminalID;
    u16 wTerminalType;
    u8  bAssocTerminal;
    u8  bNrChannels;
    u16 wChannelConfig;
    u8  iChannelNames;
    u8  iTerminal;
} UacInputTerminalDescriptor;

// Output Terminal Descriptor
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubtype;
    u8  bTerminalID;
    u16 wTerminalType;
    u8  bAssocTerminal;
    u8  bSourceID;
    u8  iTerminal;
} UacOutputTerminalDescriptor;

// Feature Unit Descriptor (for volume/mute control)
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubtype;
    u8  bUnitID;
    u8  bSourceID;
    u8  bControlSize;
    u8  bmaControls0;       // Master channel
    u8  bmaControls1;       // Channel 1 (Left)
    u8  bmaControls2;       // Channel 2 (Right)
    u8  iFeature;
} UacFeatureUnitDescriptor;

// Audio Streaming Interface Descriptor
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubtype;
    u8  bTerminalLink;
    u8  bDelay;
    u16 wFormatTag;
} UacAsGeneralDescriptor;

// Type I Format Type Descriptor
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubtype;
    u8  bFormatType;
    u8  bNrChannels;
    u8  bSubframeSize;
    u8  bBitResolution;
    u8  bSamFreqType;       // Number of sample frequencies (0 = continuous)
    u8  tSamFreq[3];        // Sample frequency (3 bytes, little-endian)
} UacFormatTypeIDescriptor;

// Audio Streaming Isochronous Endpoint Descriptor
typedef struct __attribute__((packed)) {
    u8  bLength;
    u8  bDescriptorType;
    u8  bDescriptorSubtype;
    u8  bmAttributes;
    u8  bLockDelayUnits;
    u16 wLockDelay;
} UacAsEndpointDescriptor;

// ============================================================================
// Audio Format Constants (Nintendo Switch)
// ============================================================================

#define UAC_SAMPLE_RATE         48000   // 48 kHz
#define UAC_CHANNELS            2       // Stereo
#define UAC_BIT_DEPTH           16      // 16-bit
#define UAC_SUBFRAME_SIZE       2       // 2 bytes per sample

// Bytes per sample frame (stereo 16-bit = 4 bytes)
#define UAC_BYTES_PER_FRAME     (UAC_CHANNELS * UAC_SUBFRAME_SIZE)

// Samples per millisecond at 48kHz
#define UAC_SAMPLES_PER_MS      48

// Bytes per millisecond
#define UAC_BYTES_PER_MS        (UAC_SAMPLES_PER_MS * UAC_BYTES_PER_FRAME)

// Audio buffer from grc:d is 0x1000 bytes = 1024 stereo samples
#define UAC_GRC_BUFFER_SIZE     0x1000
#define UAC_GRC_SAMPLES         (UAC_GRC_BUFFER_SIZE / UAC_BYTES_PER_FRAME)

// For isochronous transfer at 48kHz stereo 16-bit:
// 1ms of audio = 48 samples * 4 bytes = 192 bytes
// High-speed USB has 8 microframes per ms (125µs each)
// We send 1ms worth of audio per frame = 192 bytes
#define UAC_ISO_PACKET_SIZE     192     // 1ms of audio data

// Maximum packet size for high-speed isochronous (1024 bytes)
#define UAC_ISO_MAX_PACKET_SIZE 1024

// Isochronous endpoint attributes
#define UAC_ISO_SYNC_TYPE_ASYNC     (1 << 2)  // Asynchronous
#define UAC_ISO_SYNC_TYPE_ADAPTIVE  (2 << 2)  // Adaptive
#define UAC_ISO_SYNC_TYPE_SYNC      (3 << 2)  // Synchronous
#define UAC_ISO_USAGE_DATA          (0 << 4)  // Data endpoint
#define UAC_ISO_USAGE_FEEDBACK      (1 << 4)  // Feedback endpoint
#define UAC_ISO_USAGE_IMPLICIT      (2 << 4)  // Implicit feedback

// ============================================================================
// Entity IDs (separate from UVC IDs)
// ============================================================================

#define UAC_ENTITY_INPUT_TERMINAL   10
#define UAC_ENTITY_OUTPUT_TERMINAL  11
#define UAC_ENTITY_FEATURE_UNIT     12

// Channel configuration for stereo
#define UAC_CHANNEL_CONFIG_STEREO   0x0003  // Left + Right front
