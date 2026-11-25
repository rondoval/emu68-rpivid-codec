#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
colmd5.py - the reference for VCTest DECODE OUTFORMAT NV12_COL128: ffmpeg's
raw I420 output of a stream, each picture placed in the decoder's 8-bit
column layout (docs/hardware-facts.md: columns of 128 bytes, the 16-aligned
luma rows then the interleaved Cb Cr rows) and hashed as the tool hashes
the frame it got: the visible window only, column by column, luma lines
then chroma lines, so the padding the decoder writes does not count.  One
line per picture in ffmpeg's framemd5 shape, the digest last.

    ffmpeg -v error -flags unaligned -vsync passthrough -i clip.h265 -f rawvideo - |
        colmd5.py - 1920x1080 clip.colmd5 [--at X,Y]

X,Y is the window's place in the coded picture (the conformance window's
left and top), 0,0 unless the stream crops at the left or top.
"""
import argparse
import hashlib
import sys

import numpy as np


def column_window_md5(y, u, v, cx, cy):
    """The digest of a picture's visible window in the column layout; y (h, w), u and v (h/2, w/2), at (cx, cy)."""
    h, w = y.shape
    rows = (cy + h + 15) & ~15
    columns = (cx + w + 127) // 128
    luma = np.zeros((rows, columns * 128), np.uint8)
    luma[cy:cy + h, cx:cx + w] = y
    chroma = np.zeros((rows // 2, columns * 128), np.uint8)
    chroma[cy // 2:cy // 2 + h // 2, cx:cx + w:2] = u
    chroma[cy // 2:cy // 2 + h // 2, cx + 1:cx + w:2] = v
    m = hashlib.md5()
    for c in range(columns):
        b0, b1 = max(0, cx - c * 128), min(128, cx + w - c * 128)
        if b1 <= b0:
            continue
        m.update(np.ascontiguousarray(luma[cy:cy + h, c * 128 + b0:c * 128 + b1]).tobytes())
        m.update(np.ascontiguousarray(chroma[cy // 2:(cy + h) // 2, c * 128 + b0:c * 128 + b1]).tobytes())
    return m.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("raw", help="the I420 frames, or - for stdin")
    ap.add_argument("size", help="WxH of the pictures (the visible window)")
    ap.add_argument("out", help="the reference file to write")
    ap.add_argument("--at", default="0,0", help="the window's left,top in the coded picture")
    args = ap.parse_args()
    w, h = (int(x) for x in args.size.lower().split("x"))
    cx, cy = (int(x) for x in args.at.split(","))
    frame = w * h * 3 // 2
    src = sys.stdin.buffer if args.raw == "-" else open(args.raw, "rb")
    lines = ["#format: column window checksums", "#hash: MD5", f"#window: {w}x{h} at {cx},{cy}"]
    n = 0
    while True:
        data = src.read(frame)
        if len(data) < frame:
            break
        buf = np.frombuffer(data, np.uint8)
        y = buf[:w * h].reshape(h, w)
        u = buf[w * h:w * h + (w // 2) * (h // 2)].reshape(h // 2, w // 2)
        v = buf[w * h + (w // 2) * (h // 2):].reshape(h // 2, w // 2)
        lines.append(f"0, {n:7d}, {n:7d}, 1, {frame:8d}, {column_window_md5(y, u, v, cx, cy)}")
        n += 1
    with open(args.out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{args.out}: {n} pictures")
    return 0 if n > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
