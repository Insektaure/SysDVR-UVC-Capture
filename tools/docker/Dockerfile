# devkitA64 + libnx from git, for vi-tap.
#
# Atmosphère 1.12.1's libstratosphere uses libnx APIs newer than the last
# libnx release in devkitpro/devkita64 (4.12.0): fsStorageForBatchRead*
# (libnx PR #741). Build with tools/build-vi-tap.sh, which tags the image
# sysdvr-uvc/devkita64:<LIBNX_COMMIT short>.
FROM devkitpro/devkita64:20260219

# libnx master that includes "fs: add StorageForBatchRead commands (#741)"
ARG LIBNX_COMMIT=20cbcfd90567f7bf84df7725f27e345e2164fa6b

RUN git clone https://github.com/switchbrew/libnx.git /tmp/libnx \
 && git -C /tmp/libnx checkout ${LIBNX_COMMIT} \
 && make -C /tmp/libnx -j"$(nproc)" install \
 && rm -rf /tmp/libnx
