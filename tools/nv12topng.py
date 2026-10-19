#!/usr/bin/env python3
"""Turn a phase 2b-3 NV12 dump into a PNG.

The console writes, in sdmc:/config/sysdvr-uvc/dump/:
  <program>-<n>.nv12  the VIC's output: Y plane (width x height), then the
                      interleaved U,V plane (width x height/2), pitch = width
  <program>-<n>.txt   width, height and how the frame was made

The VIC writes BT.709 limited range (Y 16-235, Cb/Cr 16-240); this converts
back to full-range RGB with the standard BT.709 inverse. It also prints the
Y/U/V ranges, which tell at a glance whether the colour matrix took effect
(U and V centred near 128) or the engine passed channels through.

Usage:  python3 tools/nv12topng.py <program>-0.nv12 [out.png]
"""
import struct
import sys
import zlib
from pathlib import Path


def read_info(path):
    info = {}
    if Path(path).exists():
        for line in Path(path).read_text().splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                info[k.strip()] = v.strip()
    return info


def write_png(path, w, h, rows):
    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def clamp(v):
    return 0 if v < 0 else 255 if v > 255 else int(v + 0.5)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_suffix(".png")
    info = read_info(src.with_suffix(".txt"))
    w, h = int(info.get("width", 1280)), int(info.get("height", 720))
    data = src.read_bytes()
    if len(data) < w * h * 3 // 2:
        sys.exit(f"{src}: {len(data)} bytes, an NV12 {w}x{h} frame needs {w * h * 3 // 2}")
    y_plane, uv_plane = data[:w * h], data[w * h:w * h * 3 // 2]

    rows = []
    for y in range(h):
        row = bytearray(w * 3)
        yr = y * w
        uvr = (y // 2) * w
        for x in range(w):
            Y = (y_plane[yr + x] - 16) * 1.164383
            U = uv_plane[uvr + (x & ~1)] - 128
            V = uv_plane[uvr + (x & ~1) + 1] - 128
            i = x * 3
            row[i] = clamp(Y + 1.792741 * V)
            row[i + 1] = clamp(Y - 0.213249 * U - 0.532909 * V)
            row[i + 2] = clamp(Y + 2.112402 * U)
        rows.append(row)
    write_png(out, w, h, rows)

    us, vs = uv_plane[0::2], uv_plane[1::2]
    mean = lambda b: sum(b) / len(b)
    print(f"{out}: {w}x{h} | Y {min(y_plane)}-{max(y_plane)} mean {mean(y_plane):.0f} | "
          f"U {min(us)}-{max(us)} mean {mean(us):.0f} | V {min(vs)}-{max(vs)} mean {mean(vs):.0f} "
          f"| rect {info.get('rect', '?')}, flip {info.get('flip', '?')}, via {info.get('via', '?')}")


if __name__ == "__main__":
    main()
