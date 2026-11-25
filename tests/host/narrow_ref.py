#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""narrow_ref.py RAW10 WxH OUT - FFmpeg's yuv420p10le pictures truncated to 8 bits.

The reference for a Main10 decode shown through the backend's narrowing pass
(docs/architecture.md section 10): each 10-bit sample's top eight bits, planar
yuv420p out, picture by picture.
"""
import sys


def main():
    raw, size, out = sys.argv[1], sys.argv[2], sys.argv[3]
    w, h = map(int, size.split('x'))
    samples = w * h + 2 * ((w + 1) // 2) * ((h + 1) // 2)
    with open(raw, 'rb') as f, open(out, 'wb') as o:
        while True:
            pic = f.read(samples * 2)
            if len(pic) < samples * 2:
                break
            # little-endian 16-bit samples: the high byte holds bits 9..8, the low byte bits 7..0
            o.write(bytes(((pic[2 * i] >> 2) | ((pic[2 * i + 1] & 3) << 6)) for i in range(samples)))


if __name__ == '__main__':
    main()
