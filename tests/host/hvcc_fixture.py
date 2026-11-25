#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""hvcc_fixture.py CLIP.h265 OUT.hvcc BARE.h265 - the fixtures of test_h265.py's test_extradata.

FFmpeg's own hvcC record of the clip, taken from an MP4 it muxes (ISO/IEC 14496-15: the box in the hvc1 sample
entry), and the clip with its VPS, SPS and PPS taken out: decoded with the record as its configuration, the
pictures can have their parameter sets from the record only.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I leaves the script's own directory off the path

import broken  # noqa: E402  (its NAL unit split)


def hvcc_of(mp4: bytes) -> bytes:
    """The payload of the first hvcC box: the configuration record."""
    at = mp4.find(b"hvcC")
    while at >= 4:
        size = int.from_bytes(mp4[at - 4:at], "big")
        record = mp4[at + 4:at - 4 + size]
        if size > 8 + 23 and record[0] == 1:
            return record
        at = mp4.find(b"hvcC", at + 4)
    raise SystemExit("no hvcC box in FFmpeg's MP4")


def main() -> int:
    clip, out_hvcc, out_bare = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as scratch:
        mp4 = os.path.join(scratch, "clip.mp4")
        subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", clip, "-c", "copy", "-tag:v", "hvc1", "-y", mp4],
                       check=True)
        record = hvcc_of(open(mp4, "rb").read())
    open(out_hvcc, "wb").write(record)
    nals, tail = broken.nal_units(open(clip, "rb").read())
    kept = [n for n in nals if not 32 <= n.type <= 34]
    open(out_bare, "wb").write(b"".join(n.bytes() for n in kept) + tail)
    print(f"{out_hvcc}: {len(record)} bytes, {record[22]} arrays; {out_bare}: {len(nals) - len(kept)} parameter sets out")
    return 0


if __name__ == "__main__":
    sys.exit(main())
