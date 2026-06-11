# SysDVR-UVC - Research & Debugging Notes

Reference document for everything established during the stabilization and
latency work. Each section states the problem, the root cause, the fix (or
the verdict when no fix is possible on our side), and the evidence.

---

## 1. Console crash on USB connect (usb sysmodule assert)

**Symptom:** Atmosphère crash when plugging into a PC. Crash report named
`usb` (Program ID `0100000000000006`), exception **User Break** - an internal
assertion inside Nintendo's usb sysmodule, not a fault in our process.

**Root cause:** our EP0 handling pre-posted CtrlIn buffers on the control
interfaces *outside* the SetupEvent → `GetSetupPacket` window, and re-posted
blindly without tracking completion. usb:ds asserts on this.

**Fix (the rules, mirrored from `ams::usb::DsInterface` - Nintendo's own
client pattern, reimplemented in Atmosphère
`libstratosphere/source/usb/usb_device.cpp`):**

- Only touch EP0 inside the SetupEvent → `GetSetupPacket` window.
- The status stage is **not** automatic: a GET (data IN) must be completed
  with a zero-length OUT, a SET (data OUT) with a zero-length IN.
- `StallCtrl` on any failure.
- Note: haze (Atmosphère's MTP gadget) does **zero** EP0 handling - do not
  use it as a reference for control transfers.

## 2. Linux never created /dev/video (silent device rejection)

**Root cause:** the camera Input Terminal descriptor was 8 bytes.
`uvc_parse_standard_control()` in the Linux kernel requires ≥ 15 +
bControlSize bytes for `ITT_CAMERA` and rejects the **entire device**
(`-EINVAL`, no `/dev/video` node, no error message in dmesg).

**Fix:** full 18-byte camera terminal (focal length fields + bControlSize=3 + zeroed bmControls).

## 3. Host probe/commit requirements (verified in kernel source)

- Linux sequence: `GET_DEF` → `SET_CUR` → `GET_CUR` (must return **exactly
  48 bytes** for bcdUVC 0x0150) → `GET_MIN`/`GET_MAX` → `SET_CUR` →
  `GET_CUR` → `SET_CUR(COMMIT)`.
- Linux never sends `GET_INFO`/`GET_LEN` on probe/commit; Windows does -
  implement them anyway (INFO = 1 byte 0x03, LEN = 2 bytes = 48).
- For a single-alt bulk device, stream stop = `CLEAR_FEATURE(HALT)` on the
  endpoint, which usb:ds consumes **without notifying us**. A stalled bulk
  transfer is the only stop signal the device ever sees.

## 4. Console crash at stream stop

**Root cause:** on transfer timeout we called `usbDsEndpoint_Cancel` without
**reaping** the cancelled URB's completion report, then re-posted. usb:ds
asserts on unreaped-cancel + re-post.

**Rule:** always `eventClear` before posting; after Cancel, wait the
CompletionEvent and call `GetReportData` to reap; never re-post over an
unreaped URB.

## 5. OBS feed freezing until source re-created

**Root cause:** we treated a single 1 s transfer timeout as "host stopped
streaming" and dropped to CONNECTED, waiting for a new commit. OBS stalls
dequeuing for > 1 s routinely (scene switches, render hiccups) without
closing the device - and a host that never closed never re-negotiates →
permanent freeze.

**Fix:** a stalled transfer drops the current frame and **stays STREAMING**
(cancel + reap, no state change). If the host genuinely stopped, the device
idles at one reaped cancel per second until the next commit or disconnect.

**Follow-up bug:** aborted frames kept the same FID, so the host merged the
next frame into the unterminated one → corrupted frames → decoder stalls →
host buffers fill → more timeouts (feedback loop, 1–2 s lag + `Dequeued
v4l2 buffer contains corrupted data`).

**Fix:** toggle FID after every frame
attempt, success or not - a FID flip without EOF makes the host close the
bad frame immediately.

**Also fixed:** payloads that are an exact multiple of 512 bytes produce no
short packet, so the host cannot see the payload boundary (~1/512 frames
corrupted). A ZLP is now sent to terminate those.

## 6. Audio: UAC/isochronous is impossible on usb:ds

- On HOS 5.x+ `RegisterEndpoint` takes only an endpoint **address**; the
  transfer type lives in the appended descriptor blobs.
- The usb sysmodule's **configuration assembler chokes on an isochronous
  endpoint descriptor**: the device then serves a corrupt configuration
  descriptor and stops enumerating entirely
  (`config index 0 descriptor too short (expected 9, got 1)` on the host).
  Video dies with it.
- Appended configuration data **cannot be removed** - closing the interface
  does not clean the config, so "graceful fallback after appending" is
  impossible. Anything risky must happen before the first append.
- Bulk and interrupt are the only endpoint types with working precedent
  (all gadget homebrew is bulk; haze adds interrupt).
- Standard audio (UAC) **requires** iso - `snd-usb-audio` hard-rejects
  non-iso streaming endpoints. Therefore: no standard USB microphone is
  possible from this device.

## 7. Latency - the H.264 SPS was the stream-side root cause

**Symptom:** constant 1–2 s in OBS Windows; Linux ffplay only fast with
`-flags low_delay`.

**Analysis of the grc:d SPS** (tool: `tools/patch_sps_vui.py`):
High profile, level 3.2, 1280x720, `pic_order_cnt_type=2` (reordering
**structurally impossible**), max_ref=1 - but the VUI had
`timing_info_present=0` and `bitstream_restriction=0`.

Decoders therefore
reserved a reorder buffer for B-frames that can never exist.

**Fix:** the injected SPS (constant in `capture.c`, 20 → 33 bytes) now
declares 30 fps fixed timing and `max_num_reorder_frames=0`
(`max_dec_frame_buffering=1`), colour metadata preserved bit-exact.

Verified: field-by-field re-parse, FFmpeg parser, and on-device - FFmpeg now
reports `has_b_frames=0` and decode pacing is a rock-steady 33 ms with
~140 ms open-to-first-frame.

**Also:** UVC payload headers reduced from 12 bytes (PTS + garbage SCR) to
the minimal 2 bytes (EOH + FID/EOF). Windows schedules presentation against
device PTS it cannot correlate; Linux ignores it. Omitting PTS makes every
host render on arrival.

Bonus: all payloads now end on a short packet.

**Expected residual behavior (not bugs):**
- Plain `ffplay` (no flags) lags by its stream-probe duration on any live
  source - use `-fflags nobuffer -flags low_delay -probesize 32
  -analyzeduration 0 -framedrop`.
- Up to ~2 s of black/garbage when a consumer joins mid-stream: the Switch
  encoder's keyframe interval. Unavoidable.
- "Freezes" that always show the last frame and recover when on-screen
  content moves: grc:d skips static frames. Source property, unfixable.
- ffplay `real-time buffer too full ... frame dropped`: dshow's 3 MB
  default ring vs keyframe bursts - use `-rtbufsize 32M`.

## 8. The remaining OBS-Windows latency is an OBS bug (upstream)

**This is the part that is on OBS's side, with an open PR.**

With everything above fixed, `ffplay -f dshow` on Windows is essentially
zero-latency while OBS's Video Capture Device source still shows a constant
1–2 s. Verified in OBS source code (32.0.4 and master):

- `plugins/win-dshow/ffmpeg-decode.c` opens the H.264 decoder with
  `thread_count = 0` (auto) and **no `AV_CODEC_FLAG_LOW_DELAY`**.
- FFmpeg then enables *frame-based* multithreading
  (`libavcodec/pthread_frame.c`: `avctx->delay = thread_count - 1`,
  auto = min(cores+1, 16)), which structurally delays decoder output by
  up to 15 **frames** regardless of CPU speed.
- At an effective 10–30 fps
  delivery that is 0.5–1.5 s of constant latency. ffplay is fast because
  `-flags low_delay` disables frame threading.
- The Media Source is equally affected: its "FFmpeg Options" field reaches
  only the demuxer, never the decoder
  (`shared/media-playback/media-playback/decode.c`, same `thread_count=0`).
- libobs itself is exonerated: with Buffering=Disable the async source
  displays the newest frame immediately (`obs-source.c`,
  `ready_async_frame`). The delay is created entirely inside the decoder.
- **Beware "Buffering: Auto"**: `IsDelayedDevice()` (win-dshow.cpp) force-
  enables buffered mode for every H264/HEVC device. Always set **Disable**.
- Device-side fixes are impossible: win-dshow never reads UVC payload
  PTS/SCR (only `IMediaSample::GetTime()` from usbvideo.sys), and MJPEG -
  which would avoid frame threading entirely (FFmpeg's MJPEG decoder has no
  frame-thread capability; forum-confirmed fast on C920/C930e) - cannot be
  offered because grc:d outputs pre-encoded H.264 only.

**Upstream tracking:**
- Open PR: <https://github.com/obsproject/obs-studio/pull/13462>
  ("shared/media-playback: make the decoder accept custom FFmpeg options",
  opened 2026-05-23). Once merged, an OBS Media Source with Input Format
  `dshow` and FFmpeg options `flags=low_delay` should reach ffplay-class
  latency.
- The win-dshow path itself has no fix or PR; the actual change is trivial
  (`thread_count = 1` or `AV_CODEC_FLAG_LOW_DELAY` in
  `ffmpeg_decode_init`) and would be a welcome upstream contribution.
- Related forum evidence: "Capture card documentation (latency, decode
  modes...)" and the C930e MJPEG-vs-H264 threads on obsproject.com.

**Workarounds today:**
1. Tuned ffplay window + OBS **Window Capture** (see `switch-cam.bat`):
   `ffplay -fflags nobuffer -flags low_delay -probesize 32
   -analyzeduration 0 -framedrop -rtbufsize 32M -f dshow
   -i video="SysDVR-UVC Capture" -window_title "Nintendo Switch" -noborder`
2. OBS Linux is unaffected (different plugin, v4l2): device "default",
   Video Format H264, works with low latency after the SPS fix.

## 9. UVC MPEG2-TS format (audio muxed into the camera stream) - dead end

Investigated as a way to deliver A/V to ffplay with no companion app
(VS_FORMAT_MPEG2TS carries a TS container, which players demux with audio).
Verdict: NO-GO - the OS class drivers reject it on both platforms:

- Linux uvcvideo: `uvc_parse_format()` has an explicit
  `case UVC_VS_FORMAT_MPEG2TS: /* Not supported yet */ return -EINVAL`.
  A 2011 RFC patch adding it was never merged. Also: uvcvideo has no
  read() path (mmap streaming only), and FFmpeg's libavdevice has no
  V4L2_PIX_FMT_MPEG mapping and no TS re-demux for v4l2 inputs anyway.
  GStreamer has the plumbing (`v4l2src` -> `video/mpegts`) but it is
  unreachable because uvcvideo never exposes that format.
- Windows usbvideo.sys: documented "Compressed format: MPEG2TS -
  Not Supported" on all Windows versions.
- Only consolation: an MPEG2TS format descriptor is gracefully skipped by
  both drivers, so it would not have broken the H.264 format - it would
  just never be seen.

Conclusion: the only no-companion-app A/V path is streaming MPEG-TS over
the NETWORK directly from the console (ffplay/VLC/mpv read tcp:// or udp://
natively); needs a TS muxer + an MP2/MP3 audio encoder (TwoLAME/Shine,
LGPL, trivially realtime on the A57) + the socket-on-small-heap fix.

## 10. Host setup (Linux)

- User must be in the `video` group (`usermod -aG video $USER` + re-login),
  otherwise OBS/v4l2-ctl silently list no devices while sudo works.
- The UVC *metadata* node (`/dev/video3`) sits next to the capture node
  (`/dev/video2`); selecting it in OBS gives a black screen - use device
  "default" or verify the path.
- UDP logging from the sysmodule: build with `DEFINES="-DUDP_LOGGING"`;
  the socket needs `SO_BROADCAST` for broadcast destinations (silent
  EACCES otherwise) and WiFi APs often filter client-to-client broadcast -
  unicast to the dev PC is more reliable. Listener: `nc -ulk -p 9999`.
