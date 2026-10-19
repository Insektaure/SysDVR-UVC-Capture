#!/usr/bin/env python3
"""Turn a phase 2a slot dump into a PNG.

The console writes two files per dump in sdmc:/config/sysdvr-uvc/dump/:
  <program>-<n>.bin   the swapchain buffer exactly as in the game's memory
  <program>-<n>.txt   its geometry (width, height, stride_px, block_h_log2,
                      format, crop, transform...)

The buffer is Tegra block-linear: GOBs of 64 bytes x 8 rows, stacked
2^block_h_log2 high into blocks, blocks laid left to right across the
stride. Pixels are 4 bytes; NvColorFormat A8B8G8R8 (0x100532120) is R,G,B,A
in memory, A8R8G8B8 (0x100d12120) is B,G,R,A.

The picture is cropped to the present's crop rectangle and the present's
transform is applied (games often present flipped: transform 0x2 = flip-V),
so the PNG shows what was on screen.

Usage:  python3 tools/deswizzle.py <program>-0.bin [out.png]
        (the .txt must sit next to the .bin)
"""
import struct
import sys
import zlib
from pathlib import Path

A8B8G8R8 = 0x100532120
A8R8G8B8 = 0x100D12120


def read_info(path):
    info = {}
    for line in Path(path).read_text().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            info[k.strip()] = v.strip()
    return info


def bl_offset(x, y, stride_px, bh_log2):
    """Byte offset of pixel (x, y) in a block-linear 4-byte-per-pixel surface."""
    xb = x * 4
    block_rows = 8 << bh_log2
    block_bytes = 512 << bh_log2
    gobs_x = (stride_px * 4 + 63) // 64
    return ((y // block_rows) * gobs_x * block_bytes + (xb // 64) * block_bytes
            + ((y % block_rows) // 8) * 512 + ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64
            + ((xb % 32) // 16) * 32 + (y % 2) * 16 + (xb % 16))


def write_png(path, w, h, rows):
    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_suffix(".png")
    info = read_info(src.with_suffix(".txt"))
    data = src.read_bytes()

    w, h = int(info["width"]), int(info["height"])
    stride = int(info["stride_px"])
    bh = int(info["block_h_log2"])
    fmt = int(info.get("format", hex(A8B8G8R8)), 16)
    bgra = fmt == A8R8G8B8
    if fmt not in (A8B8G8R8, A8R8G8B8):
        print(f"warning: format {fmt:#x} unknown - assuming R,G,B,A")

    # the present's crop (left, top, right, bottom); empty = whole surface
    x0, y0, x1, y1 = 0, 0, w, h
    crop = [int(v) for v in info.get("crop", "0,0,0,0").split(",")]
    if crop[2] > crop[0] and crop[3] > crop[1]:
        x0, y0, x1, y1 = crop[0], crop[1], min(crop[2], w), min(crop[3], h)
    xf = int(info.get("transform", "0"), 16)

    rows = []
    for y in range(y0, y1):
        row = bytearray((x1 - x0) * 4)
        for x in range(x0, x1):
            o = bl_offset(x, y, stride, bh)
            px = data[o:o + 4]
            if len(px) < 4:
                continue
            r, g, b, _a = (px[2], px[1], px[0], px[3]) if bgra else tuple(px)
            i = (x - x0) * 4
            row[i:i + 4] = bytes((r, g, b, 255))
        rows.append(row)

    if xf & 1:  # flip-H
        rows = [bytearray(b"".join(bytes(r[i:i + 4]) for i in range(len(r) - 4, -1, -4))) for r in rows]
    if xf & 2:  # flip-V
        rows.reverse()
    if xf & 4:
        print("warning: rot-90 transform not applied")

    write_png(out, x1 - x0, y1 - y0, rows)
    print(f"{out}: {x1 - x0}x{y1 - y0} (surface {w}x{h}, stride {stride}, block 2^{bh}, "
          f"{'BGRA' if bgra else 'RGBA'}, transform {xf:#x}, found by {info.get('found', '?')})")


if __name__ == "__main__":
    main()
