@echo off
title Switch Cam Launcher

rem ============================================================================
rem  SysDVR-UVC low-latency viewer (Windows)
rem
rem  - FFPLAY: full path to ffplay.exe if it is not in PATH
rem  - VIDEO_DEVICE: the dshow device name. On a clean PC this is
rem    "SysDVR-UVC Capture". On a PC with Logitech drivers/software installed,
rem    Windows labels the device with Logitech's own name (we report a Logitech
rem    C270's USB IDs), so the script also tries VIDEO_DEVICE_ALT. If neither
rem    matches, the script lists the devices it can see so you can copy in the
rem    right name
rem  - AUDIO_DEVICE: optional dshow audio input to play alongside the video
rem    (e.g. the line-in/interface the Switch's headphone jack is plugged
rem    into). Leave empty for video only. List the exact device names with:
rem      ffmpeg -list_devices true -f dshow -i dummy
rem    IMPORTANT: if the device name has accented/non-ASCII characters (e.g.
rem    a localized "Entree ligne (Realtek...)"), cmd's code page will corrupt
rem    it and ffmpeg won't find it. In that case use the device's ASCII
rem    "Alternative name" instead - the "@device_cm_{...}\wave_{...}" line that
rem    -list_devices prints under the friendly name. Paste that into
rem    AUDIO_DEVICE (it is immune to code-page mangling). The same trick works
rem    for VIDEO_DEVICE via its "@device_pnp_..." alternative name.
rem
rem  - BORDER: 0 = borderless window (clean for OBS Window Capture), 1 = normal
rem    titled/resizable window.
rem
rem  A borderless window has no title bar to drag - move it with Win+Arrow
rem  keys, or set an initial position by adding: -left X -top Y
rem
rem  Up to ~2s of black/garbage at startup is normal (waiting for the
rem  Switch encoder's next keyframe).
rem ============================================================================

set FFPLAY=ffplay
set VIDEO_DEVICE=SysDVR-UVC Capture
set VIDEO_DEVICE_ALT=Logi C270 HD WebCam

set AUDIO_DEVICE=

rem RTBUFSIZE: dshow input buffer. SMALLER = stays more live, drops more on
rem demanding scenes; LARGER = fewer drops but lag can pile up then drain.
rem Tuning range ~2M-8M for live viewing (32M lets seconds of lag accumulate).
set RTBUFSIZE=8M

rem BORDER: 0 = borderless window (clean for OBS Window Capture),
rem         1 = normal window with a title bar and resizable borders.
set BORDER=0
set "BORDER_FLAG=-noborder"
if "%BORDER%"=="1" set "BORDER_FLAG="

rem Try the primary name first, then the Logitech fallback.
call :play "%VIDEO_DEVICE%"
if not errorlevel 1 goto :eof

echo.
echo "%VIDEO_DEVICE%" not found - trying "%VIDEO_DEVICE_ALT%"...
echo.
call :play "%VIDEO_DEVICE_ALT%"
if not errorlevel 1 goto :eof

rem Both names failed - show what dshow actually sees and how to fix it.
echo.
echo Neither "%VIDEO_DEVICE%" nor "%VIDEO_DEVICE_ALT%" worked.
echo DirectShow devices it can see:
echo ----------------------------------------------------------------------
ffmpeg -hide_banner -list_devices true -f dshow -i dummy 2>&1 | findstr /v /i "dummy"
echo ----------------------------------------------------------------------
echo Copy the exact video device name from the list above into VIDEO_DEVICE
echo at the top of this script ^(no quotes - the script adds them^), then re-run.
echo Other things to check:
echo   - the Switch is plugged in with a data-capable USB cable
echo   - ffplay.exe / ffmpeg.exe are in PATH, or set FFPLAY= at the top
echo   - no other app is using the camera ^(OBS source active?^)
pause
goto :eof

rem ----------------------------------------------------------------------------
rem :play <quoted video device name>
rem   Launches ffplay with the tuned low-latency flags. Branches on whether
rem   AUDIO_DEVICE is set. Returns ffplay's exit code (non-zero = device not
rem   found / failed to open; 0 = ran and was closed normally).
rem ----------------------------------------------------------------------------
:play
set "DEV=%~1"
if defined AUDIO_DEVICE goto play_av

%FFPLAY% -hide_banner ^
  -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 ^
  -framedrop -rtbufsize %RTBUFSIZE% ^
  -f dshow -i video="%DEV%" ^
  -window_title "Nintendo Switch" %BORDER_FLAG% -x 1280 -y 720
goto :eof

:play_av
%FFPLAY% -hide_banner ^
  -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 ^
  -framedrop -rtbufsize %RTBUFSIZE% -audio_buffer_size 50 ^
  -f dshow -i video="%DEV%":audio="%AUDIO_DEVICE%" ^
  -window_title "Nintendo Switch" %BORDER_FLAG% -x 1280 -y 720
goto :eof
