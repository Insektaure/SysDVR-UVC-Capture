#!/usr/bin/env bash
# Build vi-tap (a libstratosphere sysmodule) inside a pinned Atmosphère tree.
#
#   tools/build-vi-tap.sh            -> vi-tap/vi-tap.nsp
#
# - Atmosphère is cloned into external/ (gitignored) at AMS_TAG, never into
#   your own checkout, and patched with tools/patch_libstrat.py.
# - The toolchain is devkitpro/devkita64 plus libnx from git (tools/docker):
#   Atmosphère 1.12.1 needs libnx APIs newer than the 4.12.0 release.
# - The first build compiles libstratosphere: expect 10+ minutes.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AMS_TAG="${AMS_TAG:-1.12.1}"
AMS="$REPO/external/Atmosphere-$AMS_TAG"
LIBNX_COMMIT="$(sed -n 's/^ARG LIBNX_COMMIT=//p' "$REPO/tools/docker/Dockerfile")"
IMG="${DKP_IMG:-sysdvr-uvc/devkita64:${LIBNX_COMMIT:0:7}}"
UIDGID="$(id -u):$(id -g)"
DST="$AMS/stratosphere/vi-tap"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "== building toolchain image $IMG"
    docker build -t "$IMG" "$REPO/tools/docker"
fi

if [ ! -d "$AMS" ]; then
    echo "== cloning Atmosphère $AMS_TAG"
    git clone -q -c advice.detachedHead=false --depth 1 --branch "$AMS_TAG" \
        https://github.com/Atmosphere-NX/Atmosphere.git "$AMS"
fi
python3 "$REPO/tools/patch_libstrat.py" "$AMS"

# sources by content (an older checkout must not keep newer objects); the
# Makefile pair is Atmosphère's own sysmodule template (memlet's)
mkdir -p "$DST"
rsync -rlpt --checksum --delete --exclude build/ --exclude out/ \
    "$REPO/vi-tap/source/" "$DST/source/"
cp "$REPO/include/uvctap.h" "$DST/source/"
cp "$REPO/vi-tap/vi-tap.json" "$DST/"
cp "$AMS/stratosphere/memlet/Makefile" "$AMS/stratosphere/memlet/system_module.mk" "$DST/"

set +e
docker run --rm -e MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}" -v "$AMS":/ams -w /ams/stratosphere/vi-tap "$IMG" \
    bash -lc "make nx_release; rc=\$?; chown -R $UIDGID /ams; exit \$rc"
rc=$?
set -e
[ $rc -eq 0 ] || { echo "BUILD FAILED (rc=$rc)"; exit 1; }

NSP="$(find "$DST/out" -name '*.nsp' | head -1)"
cp "$NSP" "$REPO/vi-tap/vi-tap.nsp"
ls -l "$REPO/vi-tap/vi-tap.nsp"
