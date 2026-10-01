#!/usr/bin/env python3
"""Convert a 24-bit BMP (as written by rvk / the harness) to PNG: bmp2png.py in.bmp out.png"""
import struct
import sys
import zlib

d = open(sys.argv[1], "rb").read()
w, h = struct.unpack_from("<ii", d, 18)
off = struct.unpack_from("<I", d, 10)[0]
row = (w * 3 + 3) & ~3
raw = bytearray()
for y in range(h):
    raw.append(0)
    line = d[off + (h - 1 - y) * row: off + (h - 1 - y) * row + w * 3]
    for x in range(w):
        raw += bytes((line[x * 3 + 2], line[x * 3 + 1], line[x * 3]))
chunk = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
