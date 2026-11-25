#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""break-fixtures.py [--check] - the damaged streams of test_robust.py (developer-guide.md section 4.2) into tests/fixtures/broken/.

From clip720_slices.h265 (make-fixtures.sh: four slices a picture, the parameter sets before every IRAP, open
GOPs): pictures dropped, duplicated, swapped and cut short, slices dropped, cut short and overwritten, junk after a
picture, units a Main decoder ignores, and the stream cut at its start and its end.  EXPECT (JSON) holds what
test_robust.py needs: the reference hashes' file, each picture's place in display order, and for each fixture the
pictures its damage can reach (broken.py).

--check decodes every fixture with the host FFmpeg and holds its frames to the same check: an expectation that
FFmpeg, a decoder of its own, does not meet is wrong.  Needs out/stream_check (make) for the source's picture types
and POCs, and ffmpeg.
"""
import json
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I leaves the script's own directory off the path

import broken  # noqa: E402

FIX = os.path.join(HERE, "..", "fixtures")
OUT = os.path.join(FIX, "broken")
SOURCE = "clip720_slices"
SIZE = "1280x720"
START = b"\x00\x00\x00\x01"
SEED = 2026


def pictures(path: str) -> tuple[list[int], list[int]]:
    """Each picture's NAL unit type and POC in decoding order, as the stream layer has them (stream_check)."""
    text = subprocess.run([os.path.join(HERE, "out", "stream_check"), "--verbose", path], capture_output=True,
                          text=True, check=True).stdout
    found = re.findall(r"^picture \d+: type (\d+) POC (-?\d+)", text, re.M)
    return [int(t) for t, _ in found], [int(p) for _, p in found]


def junk(rng: random.Random, n: int) -> bytes:
    """Bytes that can form no start code and no emulation prevention byte: none below 4."""
    return bytes(rng.randrange(4, 256) for _ in range(n))


def unit(nal_type: int, payload: bytes, layer: int = 0, tid: int = 0) -> broken.Nal:
    """A NAL unit with a four-byte start code; @payload must hold no zero byte and end in rbsp_trailing_bits."""
    header = bytes([(nal_type << 1) | (layer >> 5), ((layer & 31) << 3) | (tid + 1)])
    return broken.Nal(START, header + payload)


def vcl(au: list[broken.Nal]) -> list[int]:
    """The positions of an access unit's slices."""
    return [k for k, n in enumerate(au) if n.vcl]


class Source:
    """The source stream, its access units, and the choice of pictures to damage."""

    def __init__(self):
        path = os.path.join(FIX, SOURCE + ".h265")
        self.stream = open(path, "rb").read()
        nals, self.tail = broken.nal_units(self.stream)
        self.aus = broken.access_units(nals)
        self.types, self.pocs = pictures(path)
        assert len(self.aus) == len(self.types), f"{len(self.aus)} access units, {len(self.types)} pictures"
        self.display = broken.display_order(self.types, self.pocs)
        self.irap = [i for i, t in enumerate(self.types) if t in broken.IRAP]
        assert len(self.irap) >= 5, f"{len(self.irap)} IRAP pictures: the choices below need five"

    def pick(self, gop: int, offset: int, reference: bool | None = None, slices: int = 1) -> int:
        """The first trailing picture of a GOP from @offset on: a reference one, a non-reference one or either,
        with at least @slices slices."""
        end = self.irap[gop + 1] if gop + 1 < len(self.irap) else len(self.types)
        for i in range(self.irap[gop] + offset, end):
            t = self.types[i]
            if t in broken.IRAP or t in broken.LEADING:
                continue
            if reference is not None and (t & 1 == 1) != reference:
                continue
            if len(vcl(self.aus[i])) >= slices:
                return i
        raise ValueError(f"no picture in GOP {gop} from {offset} on")

    def copy(self) -> list[list[broken.Nal]]:
        return [[broken.Nal(n.prefix, n.data) for n in au] for au in self.aus]


def damages(src: Source, rng: random.Random):
    """Every fixture: (name, what, the stream, the first and last picture damaged, pictures it may add)."""
    n = len(src.types)

    aus = src.copy()
    yield "none", "the source itself, undamaged", broken.joined(aus, src.tail), None, 0

    d = src.pick(1, 1, reference=True)
    aus = src.copy()
    aus[d] = [x for x in aus[d] if not x.vcl]
    yield "drop_picture", f"picture {d} (a reference) gone", broken.joined(aus, src.tail), (d, d), 0

    d = src.irap[2]
    aus = src.copy()
    aus[d] = [x for x in aus[d] if not x.vcl]  # its parameter sets stay
    yield "drop_irap", f"IRAP picture {d} gone, its parameter sets kept", broken.joined(aus, src.tail), (d, d), 0

    d = src.pick(3, 2, reference=True)
    aus = src.copy()
    twin = [broken.Nal(START if k == 0 else x.prefix, x.data) for k, x in enumerate(y for y in aus[d] if y.vcl)]
    aus.insert(d + 1, twin)
    yield "dup_picture", f"picture {d} (a reference) twice", broken.joined(aus, src.tail), (d, d), 1

    d = src.pick(1, 6)
    assert src.types[d + 1] not in broken.IRAP
    aus = src.copy()
    aus[d], aus[d + 1] = aus[d + 1], aus[d]
    yield "swap_pair", f"pictures {d} and {d + 1} swapped", broken.joined(aus, src.tail), (d, d + 1), 0

    d = src.pick(2, 3)
    aus = src.copy()
    body = broken.joined([aus[d]])
    kept = body[:len(body) // 2]
    before, after = broken.joined(aus[:d]), broken.joined(aus[d + 1:], src.tail)
    yield "trunc_picture", f"picture {d} cut to half its bytes", before + kept + after, (d, d), 0

    d = src.pick(3, 5, slices=2)
    aus = src.copy()
    k = vcl(aus[d])[1]
    aus[d][k].data = aus[d][k].data[:len(aus[d][k].data) * 2 // 5]
    yield "trunc_slice", f"the second slice of picture {d} cut to 40 %", broken.joined(aus, src.tail), (d, d), 0

    d = src.pick(4, 4, slices=3)
    aus = src.copy()
    del aus[d][vcl(aus[d])[2]]
    yield "drop_slice", f"the third slice of picture {d} gone", broken.joined(aus, src.tail), (d, d), 0

    d = src.pick(2, 9)
    while src.types[d - 1] in broken.IRAP or src.types[d - 1] in broken.LEADING:
        d = src.pick(2, d - src.irap[2] + 1)
    aus = src.copy()
    del aus[d][vcl(aus[d])[0]]
    yield ("drop_first_slice", f"the first slice of picture {d} gone: its others follow picture {d - 1}'s",
           broken.joined(aus, src.tail), (d - 1, d), 0)

    d = src.pick(1, 9, reference=True, slices=2)
    aus = src.copy()
    k = vcl(aus[d])[1]
    data = bytearray(aus[d][k].data)
    at = len(data) * 3 // 10
    data[at:at + 48] = junk(rng, 48)
    aus[d][k].data = bytes(data)
    yield "corrupt_slice", f"48 bytes of the second slice of picture {d} overwritten", broken.joined(aus, src.tail), (d, d), 0

    d = src.pick(3, 8)
    aus = src.copy()
    aus[d][-1].data += junk(rng, 1500)
    yield "trailing_junk", f"1500 bytes of junk after picture {d}'s last slice", broken.joined(aus, src.tail), (d, d), 0

    aus = src.copy()
    sei = bytes([5, 24]) + junk(rng, 24) + b"\x80"  # user_data_unregistered: a UUID and eight bytes
    places = [src.pick(1, 2), src.pick(2, 4), src.pick(3, 6), src.pick(4, 8)]
    for i in sorted(places, reverse=True):
        first = vcl(aus[i])[0]
        slice_copy = broken.Nal(START, bytes([aus[i][first].data[0] & 0x7E, (1 << 3) | (aus[i][first].data[1] & 7)]) +
                                aus[i][first].data[2:])
        aus[i][first:first] = [unit(35, b"\x50"),                    # an access unit delimiter
                               unit(39, sei),                         # a prefix SEI
                               unit(41, junk(rng, 40) + b"\x80"),     # reserved non-VCL types
                               unit(47, junk(rng, 40) + b"\x80"),
                               unit(48, junk(rng, 40) + b"\x80"),     # unspecified
                               unit(55, junk(rng, 40) + b"\x80"),
                               unit(33, junk(rng, 60) + b"\x80", layer=1)]  # an SPS of another layer: nonsense
        aus[i].append(unit(38, b"\xff" * 30 + b"\x80"))               # filler data after the slices
        aus[i].append(slice_copy)                                     # the picture's first slice again, in layer 1
    # the delimiter has to open its access unit: put it first, ahead of any parameter sets
    for i in places:
        k = next(k for k, x in enumerate(aus[i]) if x.type == 35)
        aus[i].insert(0, aus[i].pop(k))
    yield "ignored_units", f"units a Main decoder ignores before and after pictures {sorted(places)}", broken.joined(aus, src.tail), None, 0

    d = src.pick(1, 7)
    body_at = len(broken.joined(src.aus[:d]))
    cut = body_at + len(broken.joined([src.aus[d]])) * 2 // 5
    yield "cut_start", f"the stream from 40 % into picture {d}", src.stream[cut:], (0, d), 0

    e = src.pick(len(src.irap) - 1, 5, slices=2)
    aus = src.copy()
    k = vcl(aus[e])[1]
    aus[e][k].data = aus[e][k].data[:len(aus[e][k].data) * 3 // 10]
    yield "cut_end", f"the stream ending 30 % into picture {e}'s second slice", broken.joined(aus[:e + 1]), (e, n - 1), 0


def ffmpeg_hashes(path: str, scratch: str) -> list[str]:
    """FFmpeg's decode of a fixture, hashed as test_robust.py's frames are (colmd5.py)."""
    raw = os.path.join(scratch, "raw.yuv")
    ref = os.path.join(scratch, "raw.colmd5")
    subprocess.run(["ffmpeg", "-nostdin", "-v", "quiet", "-i", path, "-f", "rawvideo", "-pix_fmt", "yuv420p", "-y", raw],
                   check=False)
    if not os.path.exists(raw) or os.path.getsize(raw) == 0:
        return []
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "colmd5.py"), raw, SIZE, ref], check=True,
                   capture_output=True)
    return broken.hashes(ref)


def main() -> int:
    check = "--check" in sys.argv[1:]
    src = Source()
    rng = random.Random(SEED)
    os.makedirs(OUT, exist_ok=True)
    expect = {"source": SOURCE + ".h265", "reference": SOURCE + ".colmd5", "display": src.display, "fixtures": {}}
    reference = broken.hashes(os.path.join(FIX, SOURCE + ".colmd5"))
    failed = 0
    with tempfile.TemporaryDirectory() as scratch:
        for name, what, stream, damaged, extra in damages(src, rng):
            reached = sorted(broken.reach(src.types, *damaged)) if damaged else []
            path = os.path.join(OUT, name + ".h265")
            open(path, "wb").write(stream)
            expect["fixtures"][name] = {"file": name + ".h265", "what": what, "reached": reached, "extra": extra}
            line = f"{name}: {what}; {len(reached)} pictures within reach"
            if check:
                got = ffmpeg_hashes(path, scratch)
                why = broken.check(got, reference, src.display, set(reached), extra)
                line += f"; FFmpeg {len(got)} frames, {'passes' if why is None else 'FAILS: ' + why}"
                failed += why is not None
            print(line)
    with open(os.path.join(OUT, "EXPECT"), "w") as f:
        json.dump(expect, f, indent=1)
        f.write("\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
