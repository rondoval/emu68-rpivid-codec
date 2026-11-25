# SPDX-License-Identifier: GPL-2.0
"""broken.py - the damaged streams' model (developer-guide.md section 4.2), shared by break-fixtures.py, which makes them,
and test_robust.py, which decodes them.

An Annex B H.265 stream as NAL units and access units (7.4.2.4.4), the pictures a damage can reach, and the check
a decode's frame hashes must pass against the undamaged stream's.  A damage reaches the pictures it touches and
everything after them in decoding order up to the next IRAP picture, and that IRAP's RASL pictures: a trailing
picture never refers past its IRAP (8.3.2).  Every other picture is "clean": it must come out, bit exact, in
display order.  The reached ones may come out wrong, not at all, or once more for a duplicated one, but only
between the last clean picture before them and the first clean one after.
"""
import re

IRAP = range(16, 24)            # BLA, IDR, CRA and the reserved IRAP types
LEADING = range(6, 10)          # RADL_N, RADL_R, RASL_N, RASL_R
RASL = range(8, 10)
CVS_START = range(16, 21)       # BLA and IDR: a new coded video sequence, its POCs from 0
OPENS_AU = set(range(32, 36)) | {39} | set(range(41, 45)) | set(range(48, 56))  # VPS SPS PPS AUD, prefix SEI, reserved, unspecified


class Nal:
    """One NAL unit: the bytes before it that belong to it (zero bytes, the start code) and its own bytes."""

    def __init__(self, prefix: bytes, data: bytes):
        self.prefix = prefix
        self.data = data

    @property
    def type(self) -> int:
        return (self.data[0] >> 1) & 0x3F

    @property
    def vcl(self) -> bool:
        return self.type < 32

    @property
    def first_slice(self) -> bool:
        return self.vcl and len(self.data) > 2 and (self.data[2] & 0x80) != 0

    def bytes(self) -> bytes:
        return self.prefix + self.data


def nal_units(stream: bytes) -> tuple[list[Nal], bytes]:
    """The stream's NAL units, each with the start code and zero bytes before it; and the zero bytes after the last."""
    starts = [m.start() for m in re.finditer(b"\x00\x00\x01", stream)]
    nals, at = [], 0
    for k, s in enumerate(starts):
        begin = s + 3
        end = starts[k + 1] if k + 1 < len(starts) else len(stream)
        while end > begin and stream[end - 1] == 0:  # trailing zeros belong to the next start code
            end -= 1
        nals.append(Nal(stream[at:begin], stream[begin:end]))
        at = end
    tail = stream[at:]
    assert b"".join(n.bytes() for n in nals) + tail == stream
    return nals, tail


def access_units(nals: list[Nal]) -> list[list[Nal]]:
    """The NAL units grouped into access units (7.4.2.4.4): a new one at a first slice, or at a VPS, SPS, PPS,
    delimiter, prefix SEI, reserved or unspecified unit after a picture's slices."""
    aus, cur, have_vcl = [], [], False
    for n in nals:
        opens = n.first_slice if n.vcl else n.type in OPENS_AU
        if have_vcl and opens:
            aus.append(cur)
            cur, have_vcl = [], False
        cur.append(n)
        have_vcl = have_vcl or n.vcl
    if cur:
        aus.append(cur)
    return aus


def joined(aus: list[list[Nal]], tail: bytes = b"") -> bytes:
    """The stream of these access units."""
    return b"".join(n.bytes() for au in aus for n in au) + tail


def display_order(types: list[int], pocs: list[int]) -> list[int]:
    """Each picture's place in display order, from its type and POC in decoding order: by coded video sequence, then POC."""
    cvs, keys = -1, []
    for i, (t, poc) in enumerate(zip(types, pocs)):
        if i == 0 or t in CVS_START:
            cvs += 1
        keys.append((cvs, poc))
    order = sorted(range(len(keys)), key=lambda i: keys[i])
    place = [0] * len(keys)
    for rank, i in enumerate(order):
        place[i] = rank
    return place


def reach(types: list[int], first: int, last: int) -> set[int]:
    """The pictures (decoding order) a damage to pictures @first..@last can reach: from @first up to the next IRAP
    after @last, and that IRAP's RASL pictures (they refer to pictures before it)."""
    n = len(types)
    b = next((i for i in range(last + 1, n) if types[i] in IRAP), n)
    reached = set(range(first, b))
    i = b + 1
    while i < n and types[i] in LEADING:  # an IRAP's leading pictures come right after it (7.4.3.2)
        if types[i] in RASL:
            reached.add(i)
        i += 1
    return reached


def hashes(path) -> list[str]:
    """The digests of a colmd5 or framemd5 file, one a picture: the last 32 characters of each line not a comment."""
    out = []
    for line in open(path, encoding="ascii"):
        line = line.rstrip()
        if line and not line.startswith("#") and len(line) >= 32:
            out.append(line[-32:].lower())
    return out


def check(frames: list[str], reference: list[str], display: list[int], reached: set[int], extra: int = 0) -> str | None:
    """Whether a decode's frames (hashes in output order) pass against the undamaged stream's.

    @reference: the undamaged stream's hashes in display order; @display: each picture's place in it, by decoding
    order; @reached: the pictures the damage can reach (decoding order); @extra: pictures the damage may add.

    Return: None when they pass, else what is wrong.
    """
    if not reached:
        if frames == reference:
            return None
        diff = next((i for i, (a, b) in enumerate(zip(frames, reference)) if a != b), min(len(frames), len(reference)))
        return f"{len(frames)} frames for {len(reference)} pictures, the first difference at {diff}"
    places = {display[i] for i in reached if i < len(display)}
    p, q = min(places), max(places) + 1
    if frames[:p] != reference[:p]:
        diff = next(i for i in range(p) if i >= len(frames) or frames[i] != reference[i])
        return f"before the damage's reach (display {p}): frame {diff} differs or is missing"
    after = reference[q:]
    if len(frames) < p + len(after):
        return f"{len(frames)} frames: fewer than the {p + len(after)} the damage cannot reach"
    if after and frames[len(frames) - len(after):] != after:
        return f"after the damage's reach (display {q}): the last {len(after)} frames are not the reference's"
    middle = frames[p:len(frames) - len(after)]
    if len(middle) > q - p + extra:
        return f"{len(middle)} frames within the damage's reach (display {p} to {q}), at most {q - p + extra} allowed"
    at = 0
    for j in range(p, q):
        if j in places:
            continue
        try:
            at = middle.index(reference[j], at) + 1
        except ValueError:
            return f"display picture {j}, out of the damage's reach, missing or out of order"
    return None
