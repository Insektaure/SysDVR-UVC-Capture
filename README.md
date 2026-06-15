# SysDVR-UVC - USB Video Class Sysmodule

This sysmodule presents the Nintendo Switch as a standard USB Video Class
(UVC) webcam, allowing video capture on a PC without any client software.

Verified working end to end on Linux (ffplay, v4l2-ctl, OBS) and Windows
(ffplay, OBS with caveats - see Latency below).

## Features

- **Driverless operation**: enumerates as a standard USB webcam
  (Linux `uvcvideo`, Windows `usbvideo.sys`)
- **H.264 passthrough**: streams the native H.264 video from the Switch's
  game recording service (grc:d) - no on-console transcoding
- **720p @ 30fps**: native output of Nintendo's encoder
- **Low latency**: tuned for live capture (low-delay SPS, minimal UVC
  payload headers, render-on-arrival)

## Requirements

- Nintendo Switch with Atmosphère, HOS 5.0 or newer
- A game that supports video capture (recording icon visible)
- A data-capable USB-C cable
- **The original SysDVR sysmodule must NOT be installed/enabled**: grc:d
  only supports one client, and both sysmodules also compete for usb:ds.
  Running both crashes the console at boot.

## Building

Requires devkitPro with libnx (https://devkitpro.org/wiki/Getting_Started):

```bash
cd sysmodule-uvc
make -j

# Debug logging over UDP (port 9999). The destination IP is hardcoded in
# source/core.c (_initUdpLogging) - set it to your PC and listen with:
#   nc -ulk -p 9999
make -j DEFINES="-DUDP_LOGGING"

# Or file logging (writes /logfile.txt on the SD card)
make -j DEFINES="-DFILE_LOGGING"
```

## Installation

1. Copy the built `exefs.nsp` to `atmosphere/contents/00FF0000A53BB666/`
   on the SD card (create the directory if needed)
2. Optionally copy `toolbox.json` file & `flags` folder to the same directory
3. Reboot. The sysmodule waits ~20 seconds after boot before taking over
   USB, so the device appears a little after the HOME menu does.

## Usage

1. Connect the Switch to the PC via USB-C
2. Launch a game that supports video capture
3. The PC sees a webcam named "SysDVR-UVC Capture" (VID 046d, PID 0825)

Note: when an application opens the stream, expect up to ~2 seconds of
black/garbage before the first picture - that is the Switch encoder's
keyframe interval and cannot be shortened.

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
"default", Video Format "H.264", 1280x720 @ 30. Works with low latency.

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

Note: the device may **not** appear as `SysDVR-UVC Capture` on every PC. The
sysmodule reports a Logitech C270's USB IDs (`046D:0825`), so a machine that
has Logitech drivers/software installed will label it with Logitech's own name
(e.g. `Logi C270 HD WebCam`) instead of our string. Always confirm the exact
name first and use whatever it prints:

```bat
ffmpeg -list_devices true -f dshow -i dummy
```

Note: Windows Media Foundation apps (the Camera app, browsers, Teams) do
not support H.264 UVC cameras at all - this is a Windows limitation.
DirectShow apps (ffplay, VLC, OBS) work.

## Latency

The stream itself is low-latency by construction (~140 ms to first decoded
frame, real-time 33 ms pacing after - measured). What you experience
depends on the player:

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

See `docs/RESEARCH.md` for the full investigation.

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
- **Codec**: H.264 High profile, level 3.2, 1280x720 @ 30 fps,
  `pic_order_cnt_type=2` (no frame reordering possible)
- The stream from `grc:d` carries no parameter sets; the sysmodule injects
  SPS/PPS before keyframes. The injected SPS extends Nintendo's original
  with VUI timing info and `max_num_reorder_frames=0` so decoders open in
  low-delay mode (generated by `tools/patch_sps_vui.py`)
- UVC payload headers are the minimal 2 bytes (no PTS/SCR): hosts render
  frames on arrival instead of scheduling against an uncorrelatable
  device clock

## Limitations

1. **H.264-only**: apps that cannot consume H.264 webcams will not show
   video (notably all Windows Media Foundation apps). MJPEG cannot be
   offered: grc:d outputs pre-encoded H.264, there are no raw frames.
2. **Game support**: only games with video capture enabled produce frames.
   On static screens grc:d skips frames entirely - the picture appears
   frozen until on-screen content changes. Both are console-side behavior.
3. **No audio** (see above).
4. **No dock mode** while streaming (USB-C is used for data).
5. **Exclusive**: incompatible with the original SysDVR sysmodule and any
   other `grc:d` client.

## Troubleshooting

- **Device not in `lsusb` at all**: check the cable is data-capable, the
  sysmodule is installed, and no other grc:d/usb:ds sysmodule is present.
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
  / dshow `I/O error`**: the device works, but Windows is listing it under a
  different name. We advertise a real Logitech C270's USB IDs (`046D:0825`),
  so a PC with Logitech drivers/software supplies its own friendly name (e.g.
  `Logi C270 HD WebCam`) instead of our `iProduct` string. Run
  `ffmpeg -list_devices true -f dshow -i dummy` and pass the exact name it
  prints to `-i video="..."`. (A clean PC with no Logitech driver falls back
  to the generic UVC class driver and *does* show `SysDVR-UVC Capture` - which
  is why it varies between machines.)
- **Black picture in OBS**: wrong node selected (the UVC *metadata* node) -
  use Device "default", Video Format H.264.
- **Stream opens but freezes/lags**: read the Latency section; for the
  device-side protocol rules (EP0 windows, cancel-and-reap, payload
  framing) see `docs/RESEARCH.md`.

## Files

```
sysmodule-uvc/
├── Makefile
├── sysmodule.json             # NPDM configuration (permissions)
├── toolbox.json               # Atmosphère toolbox integration
├── docs/
│   └── RESEARCH.md            # Full debugging/protocol research notes
├── tools/
│   ├── patch_sps_vui.py       # SPS VUI patcher/verifier (latency fix)
│   ├── switch-cam.sh          # Low-latency ffplay launcher (Linux)
│   └── switch-cam.bat         # Low-latency ffplay launcher (Windows)
└── source/
    ├── sysmodule/main.c       # Entry point + capture threads
    ├── uvc/
    │   ├── uvc_descriptors.h  # USB/UVC descriptor definitions
    │   ├── uac_descriptors.h  # UAC definitions (dead code, kept for reference)
    │   ├── uac_device.h       # UAC API stubs (dead code, kept for reference)
    │   ├── uvc_device.h/c     # UVC device: descriptors, EP0, streaming
    ├── core.h/c               # Init, logging, threads
    ├── capture.h/c            # grc:d capture + SPS/PPS injection
    └── grcd.h/c               # grc:d IPC client
```

## Credits

- Based on [SysDVR](https://github.com/exelix11/SysDVR) by exelix11
- EP0 handling modeled on Atmosphère's `ams::usb::DsInterface`
- libnx by the switchbrew team

## Disclaimer

This is unofficial homebrew software, not affiliated with or endorsed by
Nintendo. It requires custom firmware; using custom firmware may void your
warranty and carries a risk of bans from Nintendo's online services. This
software is provided as-is, without warranty of any kind - use it at your
own risk. The authors are not responsible for any damage to your console
or consequences arising from its use.