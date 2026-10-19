#!/usr/bin/env bash
# Build the memory donor (00FF0000A53BB668) in docker.
#
#   tools/build-donor.sh   -> donor/sysdvr-uvc-donor.nsp
#
# - No logging variant: the donor never logs.
# - Install: donor/sysdvr-uvc-donor.nsp as atmosphere/contents/00FF0000A53BB668/exefs.nsp
#   on the SD card, without a boot2.flag - SysDVR-UVC launches it while a game
#   runs. A new donor is used from the next game launch (no reboot needed).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="${DKP_IMG:-devkitpro/devkita64:20260219}"
UIDGID="$(id -u):$(id -g)"

set +e
docker run --rm -e MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}" -v "$REPO":/src -w /src/donor "$IMG" \
    bash -lc "make clean >/dev/null && make; rc=\$?; chown -R $UIDGID /src; exit \$rc"
rc=$?
set -e
[ $rc -eq 0 ] || { echo "BUILD FAILED (rc=$rc)"; exit 1; }

ls -l "$REPO/donor/sysdvr-uvc-donor.nsp"
