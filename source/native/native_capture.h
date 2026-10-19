#pragma once
// The native capture (default build, -DNATIVE_CAPTURE): the running game's
// own frames, through VIC and NVENC, to the UVC camera at 720p and the game's
// frame rate, while a camera app streams. See native_capture.c and
// docs/NATIVE-CAPTURE.md.
void NativeCaptureThread(void* arg);
