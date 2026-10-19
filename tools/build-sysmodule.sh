#!/usr/bin/env bash
# Build SysDVR-UVC (the main sysmodule, 00FF0000A53BB666) in docker.
#
#   tools/build-sysmodule.sh                  -> exefs.nsp (native capture, no log)
#   LOG=1 tools/build-sysmodule.sh            -> the same, logging to /logfile.txt on the SD card
#   CAPTURE=grcd tools/build-sysmodule.sh     -> the grc:d build (no vi-tap, no donor)
#
# - Always a clean build: the Makefile does not track LOG/CAPTURE, and objects
#   from the other variant would be linked in silently.
# - The toolchain is plain devkitpro/devkita64 (libnx 4.12.0 is enough here;
#   only vi-tap needs libnx from git, see tools/docker).
# - Install: exefs.nsp to atmosphere/contents/00FF0000A53BB666/ on the SD card,
#   then toggle the sysmodule in ovlSysmodules (no reboot needed).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="${DKP_IMG:-devkitpro/devkita64:20260219}"
UIDGID="$(id -u):$(id -g)"
LOG="${LOG:-0}"
CAPTURE="${CAPTURE:-native}"

case "$LOG" in 0|1) ;; *) echo "LOG must be 0 or 1"; exit 1 ;; esac
case "$CAPTURE" in native|grcd) ;; *) echo "CAPTURE must be native or grcd"; exit 1 ;; esac
DEFINES=""
[ "$LOG" = 1 ] && DEFINES="-DFILE_LOGGING"
echo "== SysDVR-UVC: capture $CAPTURE, logging $LOG"

set +e
docker run --rm -e MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}" -v "$REPO":/src -w /src "$IMG" \
    bash -lc "make clean >/dev/null && make CAPTURE=$CAPTURE DEFINES='$DEFINES'; rc=\$?; chown -R $UIDGID /src; exit \$rc"
rc=$?
set -e
[ $rc -eq 0 ] || { echo "BUILD FAILED (rc=$rc)"; exit 1; }

ls -l "$REPO/exefs.nsp"
