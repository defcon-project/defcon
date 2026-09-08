#!/usr/bin/env python3
"""Assemble a Windows .ico and a macOS .icns from rendered PNGs.

These two are the one place the artwork cannot stay vector: the containers are
read by the operating system, not by Qt, and both are raster. Generating them
from the same SVG is what stops them drifting into a second, older drawing.

Windows entries up to 48 px are written the classic way, as a bottom-up 32-bit
DIB with an AND mask, because that is what every reader has always accepted --
PNG-compressed entries are fine in the Explorer shell but have a history of not
being with installers and older tooling. Entries at 64 px and above embed the
PNG bytes untouched, which is both conventional and what keeps a 256 px icon
from costing a quarter of a megabyte.

.icns carries PNG throughout; it has since 10.7.
"""
import os
import struct
import sys
import zlib


def read_png(path):
    """Decode an 8-bit RGBA non-interlaced PNG. That is what Qt writes here."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, width, height = 8, b"", 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if (depth, colour, interlace) != (8, 6, 0):
                raise SystemExit(f"{path}: need 8-bit RGBA non-interlaced")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length

    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    at = 0
    for y in range(height):
        filt = raw[at]; at += 1
        line = bytearray(raw[at:at + stride]); at += stride
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if filt == 1: line[x] = (line[x] + a) & 0xFF
            elif filt == 2: line[x] = (line[x] + b) & 0xFF
            elif filt == 3: line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, bytes(out)


def as_dib(width, height, rgba):
    """A bottom-up 32-bit DIB with an all-clear AND mask, as an ICO entry."""
    xor = bytearray()
    for y in range(height - 1, -1, -1):
        row = rgba[y * width * 4:(y + 1) * width * 4]
        for x in range(0, len(row), 4):
            r, g, b, a = row[x], row[x + 1], row[x + 2], row[x + 3]
            xor += bytes((b, g, r, a))
    mask_stride = ((width + 31) // 32) * 4      # 1 bit per pixel, rows padded to 4 bytes
    mask = bytes(mask_stride * height)          # zero: every pixel opaque, alpha decides
    header = struct.pack("<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0,
                         len(xor) + len(mask), 0, 0, 0, 0)
    return header + bytes(xor) + mask


def png_bytes(path):
    return open(path, "rb").read()


mode = sys.argv[1]          # "ico" or "icns"
target = sys.argv[2]
sources = sys.argv[3:]

items = []
for path in sources:
    w, h, rgba = read_png(path)
    if w != h:
        raise SystemExit(f"{path}: {w}x{h} is not square; an icon entry must be")
    items.append((w, h, rgba, path))
items.sort(key=lambda e: e[0])

if mode == "ico":
    DIB_UP_TO = 48
    blobs = []
    for w, h, rgba, path in items:
        blobs.append((w, as_dib(w, h, rgba) if w <= DIB_UP_TO else png_bytes(path),
                      "DIB" if w <= DIB_UP_TO else "PNG"))
    offset = 6 + 16 * len(blobs)
    directory = b""
    payload = b""
    for size, blob, _ in blobs:
        # 0 in the byte fields means 256; that is how ICO says it.
        directory += struct.pack("<BBBBHHII",
                                 size if size < 256 else 0,
                                 size if size < 256 else 0,
                                 0, 0, 1, 32, len(blob), offset)
        payload += blob
        offset += len(blob)
    open(target, "wb").write(struct.pack("<HHH", 0, 1, len(blobs)) + directory + payload)
    detail = ", ".join(f"{size}px {kind}" for size, _, kind in blobs)

elif mode == "icns":
    ostype_for = {32: b"ic11", 64: b"ic12", 128: b"ic07",
                  256: b"ic08", 512: b"ic09", 1024: b"ic10"}
    body = b""
    used = []
    for w, _, _, path in items:
        ostype = ostype_for.get(w)
        if ostype is None:
            print(f"  skipping {os.path.basename(path)}: .icns has no slot for {w}px")
            continue
        blob = png_bytes(path)
        body += ostype + struct.pack(">I", len(blob) + 8) + blob
        used.append(f"{w}px {ostype.decode()}")
    open(target, "wb").write(b"icns" + struct.pack(">I", len(body) + 8) + body)
    detail = ", ".join(used)

else:
    raise SystemExit("mode must be ico or icns")

print(f"wrote {target} ({os.path.getsize(target):,} bytes)")
print(f"  {detail}")
