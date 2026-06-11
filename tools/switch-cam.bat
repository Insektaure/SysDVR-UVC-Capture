@echo off
title Switch Cam Launcher

rem ============================================================================
rem  SysDVR-UVC low-latency viewer (Windows)
rem
rem  - FFPLAY: full path to ffplay.exe if it is not in PATH
rem  - AUDIO_DEVICE: optional dshow audio input to play alongside the video
rem    (e.g. the line-in/interface the Switch's headphone jack is plugged
rem    into). Leave empty for video only. List the exact device names with:
rem      ffmpeg -list_devices true -f dshow -i dummy
rem
rem  The window is borderless (clean for OBS Window Capture). Move it with
rem  Win+Arrow keys, or set an initial position by adding: -left X -top Y
rem
rem  Up to ~2s of black/garbage at startup is normal (waiting for the
rem  Switch encoder's next keyframe).
rem ============================================================================

set FFPLAY=ffplay
set VIDEO_DEVICE=SysDVR-UVC Capture
set AUDIO_DEVICE=

if defined AUDIO_DEVICE goto with_audio

%FFPLAY% -hide_banner ^
  -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 ^
  -framedrop -rtbufsize 32M ^
  -f dshow -i video="%VIDEO_DEVICE%" ^
  -window_title "Nintendo Switch" -noborder -x 1280 -y 720
goto check_error

:with_audio
%FFPLAY% -hide_banner ^
  -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 ^
  -framedrop -rtbufsize 32M -audio_buffer_size 50 ^
  -f dshow -i video="%VIDEO_DEVICE%":audio="%AUDIO_DEVICE%" ^
  -window_title "Nintendo Switch" -noborder -x 1280 -y 720

:check_error
if errorlevel 1 (
  echo.
  echo ffplay failed. Check that:
  echo   - the Switch is plugged in ^(device "%VIDEO_DEVICE%" in Device Manager^)
  echo   - ffplay.exe is in PATH, or set FFPLAY= at the top of this file
  echo   - no other app is using the camera ^(OBS source active?^)
  echo   - if AUDIO_DEVICE is set, the name matches ffmpeg -list_devices exactly
  pause
)
