# SysDVR-UVC - USB Video Class Sysmodule

This sysmodule presents the Nintendo Switch as a standard USB Video Class
(UVC) webcam, allowing video capture on a PC without any client software.

By default it captures the running game's own frames (native capture,
720p up to 60 fps, H.264 encoded on the console's hardware encoder). The
original grc:d passthrough (720p30, the console's recording stream) remains
available as a build option.

Verified working end to end on Linux (ffplay, v4l2-ctl, OBS) and Windows
(ffplay, OBS with caveats - see Latency below).

## Features

- **Driverless operation**: enumerates as a standard USB webcam
  (Linux `uvcvideo`, Windows `usbvideo.sys`)
- **Native capture**: the game's frames themselves, not the recording
  service - works whether or not the game allows video capture
- **720p up to 60 fps**: the game's own frame rate, scaled or cropped to 1280x720
- **Works with Atmosphère cheats**: games held by the cheat engine (a cheat
  file, or an overlay such as EdiZon) are read through `dmnt:cht`
- **Toggleable**: turn it on and off at any time with ovlSysmodules; turned
  on after boot it starts at once
- **Low latency**: ~7 ms on the console from the game presenting a frame to
  its last byte leaving over USB; low-delay SPS, minimal
  UVC payload headers, render-on-arrival
- **Keeps the stream alive** between games and on the HOME menu (black or
  the last picture), so viewers never stall

## How it works

Three programs work together (design and measurements:
`docs/NATIVE-CAPTURE.md`):

| Program | Title ID | Role |
|---|---|---|
| **SysDVR-UVC** | `00FF0000A53BB666` | The camera. Reads the game's frames, converts them to NV12 (on the VIC, or with NEON on the CPU), encodes them with NVENC and sends them over USB. Toggleable. |
| **vi-tap** | `00FF0000A53BB667` | Always on (boot2). Sits between programs and the display service and records where each game's frames are and when they are presented. Never toggled while running. |
| **donor** | `00FF0000A53BB668` | Launched by SysDVR-UVC while a game runs: lends it 34 MB of the Applet memory pool for the frame buffers and the encoder, and gives it back when an applet (web browser, eShop, manual) needs it. |

For each frame, SysDVR-UVC picks how to convert it by measuring: the VIC
(the console's video image compositor, ~0.6 ms a frame when idle) or the
CPU. In GPU-heavy games every VIC job waits behind the system compositor's
(17-41 ms measured), and the CPU route (~12-14 ms) delivers more frames.
The choice is re-checked on the fly, without switching to try.

## Requirements

- Nintendo Switch with Atmosphère **1.12.1** (vi-tap is built against its
  libstratosphere; for another version rebuild it with
  `AMS_TAG=<version> tools/build-vi-tap.sh`)
- A data-capable USB-C cable
- **The original SysDVR sysmodule must NOT be installed/enabled**: both
  compete for `usb:ds` (and for `grc:d` in the grc:d build). Running both
  crashes the console at boot.

## Building

Everything builds in docker (devkitpro/devkita64; vi-tap needs libnx from
git, which `tools/docker` provides):

```bash
tools/build-sysmodule.sh                 # exefs.nsp - native capture
LOG=1 tools/build-sysmodule.sh           # the same, logging to /logfile.txt on the SD card
CAPTURE=grcd tools/build-sysmodule.sh    # the grc:d passthrough build (720p30, no vi-tap/donor)
tools/build-donor.sh                     # donor/sysdvr-uvc-donor.nsp
tools/build-vi-tap.sh                    # vi-tap/vi-tap.nsp (clones Atmosphère into external/;
                                         # the first build takes 10+ minutes)
VITAP_LOG=1 tools/build-vi-tap.sh        # vi-tap logging to config/sysdvr-uvc/vi-tap.log
```

With devkitPro installed locally, `make -j` (and `make -j DEFINES=-DFILE_LOGGING`,
`make -j CAPTURE=grcd`) works too - run `make clean` when switching
variants: the Makefile does not track them.

## Installation

| Copy | To (on the SD card) |
|---|---|
| `exefs.nsp` | `atmosphere/contents/00FF0000A53BB666/exefs.nsp` |
| `toolbox.json`, `flags/boot2.flag` (optional: start at boot) | `atmosphere/contents/00FF0000A53BB666/` |
| `vi-tap/vi-tap.nsp` | `atmosphere/contents/00FF0000A53BB667/exefs.nsp` |
| `vi-tap/flags/boot2.flag` (required) | `atmosphere/contents/00FF0000A53BB667/flags/` |
| `donor/sysdvr-uvc-donor.nsp` | `atmosphere/contents/00FF0000A53BB668/exefs.nsp` (no boot2 flag) |

Then reboot (vi-tap must start at boot). The grc:d build needs only the
first two lines.

At boot, SysDVR-UVC waits ~20 seconds before taking over USB, so the device
appears a little after the HOME menu does. Turned on later (ovlSysmodules),
it starts at once.

### Turning it on and off

- **SysDVR-UVC**: toggle it any time with ovlSysmodules.
- **vi-tap**: leave it on - it costs next to nothing while SysDVR-UVC is
  off. Never stop it while running: every program's display connection goes
  through it, and the game or the HOME menu would crash. If you add a
  `toolbox.json` for it, set `"requires_reboot": true` so ovlSysmodules only
  changes the boot flag.
- **donor**: never runs on its own. Started by hand it finds nobody to lend
  to and exits.

## Usage

1. Connect the Switch to the PC via USB-C
2. Launch a game
3. The PC sees a webcam named "SysDVR-UVC Capture" (VID 1209, PID ca57)

The picture starts with a keyframe as soon as an application opens the
camera. When a game launches, the first frame arrives ~1.5 s (direct read)
to 4-9 s (through the cheat engine, measured) after the game appears; until
then, and between games, the camera sends black.

### Linux

One-time setup: your user must be able to read video devices:

```bash
sudo usermod -aG video $USER   # then log out and back in

# Or, to grant access immediately without logging out (resets on
# unplug/reboot):
sudo chmod a+rw /dev/video2
```

The device creates two nodes: capture + UVC metadata. The numbers are not
fixed - the kernel assigns `/dev/videoN` in detection order, so on a
machine with no other camera the Switch gets `video0`/`video1`, while a
laptop with a built-in webcam typically yields `video2`/`video3`.

Find
yours with:

```bash
v4l2-ctl --list-devices
```

Look for "SysDVR-UVC Capture" and use its first node (the capture node),
or let apps pick "default". The examples below assume `/dev/video2` -
substitute your own node.

```bash
# Low-latency viewer (this is the reference pipeline)
ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 \
       -framedrop -f v4l2 -input_format h264 -i /dev/video2

# Record raw H.264
v4l2-ctl -d /dev/video2 --stream-mmap --stream-count=300 --stream-to=out.h264
```

OBS on Linux: add a "Video Capture Device (V4L2)" source, Device
"default", Video Format "H.264", 1280x720 @ 60 (30 with the grc:d build).
Works with low latency.

### Windows

```bat
ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 ^
       -framedrop -rtbufsize 8M -f dshow -i video="SysDVR-UVC Capture"
```

Note: `-rtbufsize` is the dshow input buffer. Keep it small (~2M-8M) for live
viewing - a large buffer (e.g. 32M) lets several seconds of lag accumulate
during demanding scenes that then slowly drains. Smaller drops more frames on
heavy bursts but stays current. `tools/switch-cam.bat` exposes this as the
`RTBUFSIZE` variable.

Note: if the name does not match, list what Windows actually sees and use
that string:

```bat
ffmpeg -list_devices true -f dshow -i dummy
```

Note: Windows Media Foundation apps (the Camera app, browsers, Teams) do
not support H.264 UVC cameras at all - this is a Windows limitation.
DirectShow apps (ffplay, VLC, OBS) work.

### macOS

No setup needed - the camera appears in QuickTime Player (File -> New Movie
Recording), OBS and AVFoundation. macOS decodes the H.264 on the host and
hands out raw frames:

```bash
ffmpeg -f avfoundation -list_devices true -i ""
ffmpeg -f avfoundation -framerate 60 -pixel_format nv12 -i 0 -f null -
```

`nv12` is the decoder's native output; `uyvy422` also works.

## Latency

On the console, a frame leaves ~7 ms after the game presents it (60 fps: read, convert, encode, send). Frames are sent as the
game draws them, up to 60 a second. What you experience depends on the
player:

- **ffplay**: fast only with the flags shown above. Plain `ffplay` buffers
  its stream-probing backlog and stays a constant 1-2 s behind on any live
  source - that is ffplay behavior, not the device.
- **OBS Linux**: fast (V4L2 plugin).
- **OBS Windows**: a constant ~1-2 s delay caused by OBS itself: its
  DirectShow H.264 decoder is opened with auto frame-threading and no
  low-delay flag, which structurally delays output by up to 15 frames.
  Tracked upstream (https://github.com/obsproject/obs-studio/pull/13462).
  Until fixed, the reliable workaround is running the tuned ffplay window
  and adding it to OBS via Window Capture. Always set the source's
  "Buffering" to "Disable" - "Auto" silently force-buffers H.264 devices.

- **A delay that grows over time** comes from the viewer's buffer (dshow
  `-rtbufsize`, see Windows above), not the console: restart the viewer.
  There is no need to toggle the sysmodule.

See `docs/RESEARCH.md` for the full investigation (written for the grc:d
build) and `docs/NATIVE-CAPTURE.md` for the native capture's measurements.

## Audio

There is none, and there cannot be: standard USB audio (UAC) requires an
isochronous endpoint, and the Switch's `usb:ds` service cannot build a
configuration containing one (it corrupts the whole config descriptor and
the device stops enumerating).

The verdict and a planned vendor-bulk fallback (PCM over a vendor interface + a small PC tool exposing a virtual
microphone) are documented in `docs/RESEARCH.md` §6.

### Workaround: Bluetooth audio (A2DP sink)

The Switch can output audio over Bluetooth (HOS 13.0+), and the PC can
act as the receiver (an A2DP *sink*, i.e. pretend to be headphones).
This works on Linux and on Windows 10 since version 2004.

**Linux** (PipeWire or PulseAudio):

1. Make the PC discoverable and pairable:
   ```bash
   bluetoothctl
   power on
   discoverable on
   pairable on
   ```
2. On the Switch: System Settings → Bluetooth Audio → Pair Device, then
   select your PC. Confirm the pairing request in `bluetoothctl`
   (`trust <MAC>` to make it reconnect automatically).
3. With PipeWire the incoming stream appears as an audio source and is
   played automatically by most desktops. With plain PulseAudio, set the
   card profile to `a2dp_source` and loop it to your speakers:
   ```bash
   pactl load-module module-loopback source=$(pactl list sources short | grep -o 'bluez_[^ ]*' | head -1)
   ```

**Windows 10 2004+ / Windows 11**:

1. Settings → Bluetooth & devices → Add device, then on the Switch:
   System Settings → Bluetooth Audio → Pair Device → select your PC.
2. Windows ships the A2DP sink role but no UI to start it - install the
   free "Bluetooth Audio Receiver" app from the Microsoft Store, open
   it, select the Switch and click "Open Connection".

Caveats: Bluetooth adds its own latency (typically 100-200 ms with SBC),
so audio will trail the low-latency video slightly. While Bluetooth audio
is active the Switch limits you to two wireless controllers and disables
local wireless play - that is a console-side restriction.

### Workaround: line-in audio (analog)

Lower latency than Bluetooth: run a 3.5 mm cable from the Switch's
headphone jack (handheld/tabletop mode only - docked, audio goes out HDMI
and the jack is silent) into the PC's **line-in** jack, and capture it as a
second dshow input alongside the video. The `tools/switch-cam.bat` launcher
has an `AUDIO_DEVICE` slot for exactly this. Find the device name with:

```bat
ffmpeg -list_devices true -f dshow -i dummy
```

If the name has accented/non-ASCII characters (e.g. a localized
`Entrée ligne (Realtek...)`), cmd's code page will corrupt it and ffmpeg
won't find it - use the device's ASCII **Alternative name** instead (the
`@device_cm_{...}\wave_{...}` line printed under the friendly name).

> ⚠️ **Ground-loop warning.** Capturing video over USB-C *and* audio over
> the 3.5 mm cable connects the Switch and PC by two separate grounds. The
> resulting ground loop is heard as constant static/hum/buzz on the audio.
> It is **not** a fault in the module, cable, or ffmpeg - it appears only
> when both cables are connected at once. You cannot fix it by unplugging
> USB-C (that is your video capture).
>
> **Mitigation:** put an inline **3.5 mm ground-loop isolator** (~€10,
> transformer-based) on the *audio* cable. It breaks the ground path through
> the audio shield while passing sound, leaving USB-C - and your video -
> untouched. If you'd rather avoid extra hardware, use the Bluetooth/A2DP
> route above instead (no shared ground, no loop).

**Keeping the video live with audio enabled:**

When ffplay plays an audio
stream it normally slaves the video to the audio clock, so a buffering audio
device can drag the video behind.

To avoid that, the launcher's audio path adds `-sync ext` (keep the video on the system clock; audio may drift slightly
out of lip-sync instead) and exposes an `AUDIO_BUFFER` variable - the dshow
audio capture buffer in milliseconds.

Lower = less audio latency; the default is `20`. ffmpeg only *requests* this size and the driver picks the actual
granularity, so going much below `~10-20` buys little and tends to cause
crackle/dropouts - raise it to `30-50` only if the audio stutters.

Note this knob affects *audio* latency, not video: for video lag, lower `RTBUFSIZE`.

## Technical Details

- UVC 1.5, VideoControl + VideoStreaming interfaces, H.264 frame-based
  format over a **bulk** endpoint in alternate setting 0 (bulk UVC must
  not use the iso-style alt0/alt1 split)
- **Codec (native capture)**: H.264 High profile, level 3.2, CABAC,
  1280x720 up to 60 fps, encoded by NVENC with the console recorder's own
  encoder setup; constant QP 20 (P frames 22), a keyframe every 60 frames
  and at every stream start. Every frame carries SPS/PPS (BT.709 VUI, 60 fps
  timing, no reordering), so a decoder can start anywhere
- **Codec (grc:d build)**: H.264 High profile, level 3.2, 1280x720 @ 30 fps,
  `pic_order_cnt_type=2`. The stream from `grc:d` carries no parameter sets;
  the sysmodule injects SPS/PPS before keyframes, extended with VUI timing
  and `max_num_reorder_frames=0` so decoders open in low-delay mode
  (generated by `tools/patch_sps_vui.py`)
- **Frame source (native capture)**: the game's swapchain buffers, read
  directly through the debugger, or through `dmnt:cht` when the cheat
  engine holds the game; converted to NV12 by the VIC or by NEON code on
  core 3 (the system core; games run on 0-2)
- **USB identity**: `1209:ca57` (pid.codes, the open-source-hardware vendor
  ID). Driverless operation does not depend on it - UVC binding is class-based
  everywhere - but the IDs are not free of consequences: macOS merges
  vendor-wide overrides from `UVCMatching.plist`, and its Logitech entry
  (`0x046D`, any PID) hides the H.264 format. Earlier builds reported a
  Logitech C270 (`046D:0825`) and were therefore published with zero formats
  on macOS, invisible to QuickTime, OBS and AVFoundation
- UVC payload headers are the minimal 2 bytes (no PTS/SCR): hosts render
  frames on arrival instead of scheduling against an uncorrelatable
  device clock
- Frames are sent by a separate thread, up to 4 payloads of 16 KB in flight,
  while the capture loop works on the next frame

## Limitations

1. **H.264-only**: apps that cannot consume H.264 webcams will not show
   video (notably all Windows Media Foundation apps). Raw frames do not fit
   the link: the Switch is a USB 2.0 device (~35-40 MB/s), and 720p NV12
   needs 83 MB/s at 60 fps.
2. **Games only**: the native capture shows the running game; the HOME menu
   and applets are not captured (the camera shows black, or the last game
   picture, meanwhile).
3. **720p**: games rendering 1080p (docked) are scaled to 720p by the VIC.
   The CPU route only handles 1280x720 frames, so a GPU-heavy game at 1080p
   has only the VIC route - expect around the ~30 fps the VIC route gives
   such games handheld (not measured docked).
4. **Through the cheat engine**, frames are copied more slowly (~6 ms
   against ~2.3 ms directly): GPU-heavy games reach ~40 fps rather than
   their full rate. Without a cheat file for the game it is read directly.
5. **No audio** (see above).
6. **No dock mode** while streaming (USB-C is used for data).
7. **Exclusive**: incompatible with the original SysDVR sysmodule (and, for
   the grc:d build, any other `grc:d` client).
8. **Memory**: while a game runs, the donor holds 34 MB of the Applet pool
   (of ~467 MB); it gives it back when an applet needs it, and capture
   pauses until the next game.

## Troubleshooting

- **Device not in `lsusb` at all**: check the cable is data-capable, the
  sysmodule is installed, and no other grc:d/usb:ds sysmodule is present.
- **Camera present but always black in games (native capture)**: vi-tap
  is missing or was not running at boot (it needs `flags/boot2.flag` and a
  reboot), or the donor is missing. A logging build (`LOG=1`) explains what
  it sees in `/logfile.txt`.
  Check `/atmosphere/crash_reports/` and `/atmosphere/fatal_errors/` on
  the SD card for fresh files.
- **`config descriptor too short` in dmesg**: the USB configuration is
  corrupt - if you modified descriptor code, see the warnings in
  `uvc_device.c` (appended config data can never be removed; iso
  endpoints are unsupported).
- **Enumerates but no `/dev/video` (Linux)**: permissions (are you in the
  `video` group?) or a descriptor regression - `sudo dmesg` will show
  uvcvideo parse errors.
- **Windows: `Could not find video device with name [SysDVR-UVC Capture]`
  / dshow `I/O error`**: run `ffmpeg -list_devices true -f dshow -i dummy` and
  pass the exact name it prints to `-i video="..."`. Coming from a build older
  than the `1209:ca57` USB identity, Windows sees a brand-new device: saved OBS
  sources and scripts still point at the old one and must be re-selected once
  (older builds reported a Logitech C270, so a PC with Logitech software
  installed also relabelled the device `Logi C270 HD WebCam`).
- **macOS: nothing in QuickTime/OBS/`ffmpeg -f avfoundation`** while
  `system_profiler SPUSBDataType` does show the device: you are running a build
  older than the `1209:ca57` USB identity - macOS hides H.264 for Logitech
  VIDs, which leaves the camera with no formats at all. Update the sysmodule.
  Check what the host merged with
  `ioreg -r -c IOUSBHostInterface -w 0 -l | grep -A 1 CameraAssistantConfiguration`:
  a `UVCCameraHideFormats` entry is the symptom.
- **Black picture in OBS**: wrong node selected (the UVC *metadata* node) -
  use Device "default", Video Format H.264.
- **Stream opens but freezes/lags**: read the Latency section; for the
  device-side protocol rules (EP0 windows, cancel-and-reap, payload
  framing) see `docs/RESEARCH.md`.

## Files

```
├── Makefile
├── sysmodule.json             # NPDM configuration (permissions)
├── toolbox.json               # Atmosphère toolbox integration
├── docs/
│   ├── NATIVE-CAPTURE.md      # native capture: design, measurements, history
│   └── RESEARCH.md            # UVC/USB protocol research (grc:d build)
├── include/                   # vi-tap and donor protocols (uvctap.h, uvcdonor.h)
├── source/
│   ├── sysmodule/main.c       # entry point + threads
│   ├── uvc/                   # UVC device: descriptors, EP0, streaming
│   ├── native/                # native capture
│   │   ├── native_capture.c   # sessions, frame loop, mode choice, keep-alive, send thread
│   │   ├── game_dbg.c         # debugger attach / dmnt:cht reads
│   │   ├── slotmatch.c        # matching the game's buffers to swapchain slots
│   │   ├── vic_engine.c       # VIC conversion
│   │   ├── cpu_convert.c      # NEON conversion (block-linear RGBA -> NV12)
│   │   ├── nvenc_engine.c     # NVENC H.264 encode
│   │   └── donor.c            # borrowing the donor's memory
│   ├── tap_client.c           # vi-tap client
│   ├── capture.c, grcd.c      # grc:d build: capture + SPS/PPS injection
│   └── core.c                 # init, logging, threads
├── vi-tap/                    # vi-tap (libstratosphere mitm)
├── donor/                     # the memory donor
└── tools/
    ├── build-sysmodule.sh, build-donor.sh, build-vi-tap.sh   # docker builds
    ├── docker/                # toolchain image with libnx from git (vi-tap)
    ├── patch_libstrat.py      # libstratosphere patch for vi-tap's mitm
    ├── patch_sps_vui.py       # SPS VUI patcher/verifier (grc:d build)
    ├── switch-cam.sh          # low-latency ffplay launcher (Linux)
    ├── switch-cam.bat         # low-latency ffplay launcher (Windows)
    └── test_slotmatch.c, deswizzle.py, nv12topng.py   # tests and dump tools
```

## Credits

- Based on [SysDVR](https://github.com/exelix11/SysDVR) by exelix11
- The native capture follows [Switch Frame Tap](https://github.com/TomasUribe/switch-frame-tap)
  by Tomas Uribe (GPL-2.0): the vi mitm, the frame-buffer search, the
  dmnt:cht path, the VIC and NVENC setups
- VIC configuration structs from libdrm (NVIDIA, MIT)
- EP0 handling modeled on Atmosphère's `ams::usb::DsInterface`
- libnx by the switchbrew team, Atmosphère / libstratosphere by SciresM and
  contributors

## Disclaimer

This is unofficial homebrew software, not affiliated with or endorsed by
Nintendo. It requires custom firmware; using custom firmware may void your
warranty and carries a risk of bans from Nintendo's online services. This
software is provided as-is, without warranty of any kind - use it at your
own risk. The authors are not responsible for any damage to your console
or consequences arising from its use.