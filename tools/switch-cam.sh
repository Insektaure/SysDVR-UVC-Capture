#!/usr/bin/env bash
# SysDVR-UVC low-latency viewer (Linux)
#
# Finds the Switch's capture node by its stable by-id path (device numbers
# like /dev/video2 change on every replug, the by-id symlink does not),
# then launches ffplay with the proven low-latency flags.
#
# Audio note: on Linux the recommended audio route is Bluetooth (pair the
# Switch to the PC as an A2DP sink) - it plays as normal system audio and
# needs nothing here.

set -euo pipefail

# --- locate the capture node ------------------------------------------------
# index0 = video capture, index1 = UVC metadata (no images - do not use)
DEVICE=""
for path in /dev/v4l/by-id/*SysDVR*video-index0 /dev/v4l/by-id/*Nintendo*video-index0; do
    if [ -e "$path" ]; then
        DEVICE="$path"
        break
    fi
done

if [ -z "$DEVICE" ]; then
    echo "SysDVR-UVC capture device not found." >&2
    echo "Checks:" >&2
    echo "  - Is the Switch plugged in? (lsusb -d 046d:0825)" >&2
    echo "  - Do you have access? (you must be in the 'video' group:" >&2
    echo "      sudo usermod -aG video \$USER   # then log out and back in)" >&2
    echo "  - List devices: v4l2-ctl --list-devices" >&2
    exit 1
fi

if [ ! -r "$DEVICE" ]; then
    echo "Found $DEVICE but it is not readable - add yourself to the 'video' group:" >&2
    echo "  sudo usermod -aG video \$USER   # then log out and back in" >&2
    exit 1
fi

echo "Using device: $DEVICE"

# --- launch -------------------------------------------------------------------
# Up to ~2s of black/garbage at startup is normal (waiting for the Switch
# encoder's next keyframe).
exec ffplay -hide_banner \
    -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 \
    -framedrop \
    -f v4l2 -input_format h264 -i "$DEVICE" \
    -window_title "Nintendo Switch" -x 1280 -y 720 \
    "$@"
