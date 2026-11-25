# SPDX-License-Identifier: GPL-2.0
"""The HEVC_v1 conformance set through the hardware (developer-guide.md section 4.4): every stream the backend accepts bit exact, every one it refuses refused with the planned code."""
from pathlib import Path

import pytest
from vchiqtest.tools import assert_nothing_left, status, vctest

LIST = Path(__file__).resolve().parent / "conformance" / "LIST"  # checked in, with the references beside it
NATIVE = "OUTFORMAT NV12_COL128"


def rows(expect):
    """The LIST's streams with @expect in the expectation column: (name, pictures)."""
    out = []
    for line in LIST.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        f = line.split()
        if f[5] == expect:
            out.append((f[0], int(f[4]) if f[4].isdigit() else 0))
    return out


ACCEPTED = rows("ok")
REFUSED = rows("refuse")

# Streams the block decodes but not bit exact (docs/hardware-facts.md, "The session"): the luma the same as ffmpeg's, a
# few chroma samples a picture off by one, from the second picture on.  They decode whole; the hashes are not held
# against them.
KNOWN = {
    "SAODBLK_A_MainConcept_4": "chroma off by one in a few samples a picture (luma the same)",
    "SAODBLK_B_MainConcept_4": "chroma off by one in a few samples a picture (luma the same)",
}
# Streams with no reference: ffmpeg cannot decode them whole.
UNVERIFIED = {
    "VPSSPSPPS_A_MainConcept_1": "six SPSs of six sizes, one picture each; ffmpeg shows two of the six the backend decodes",
}


@pytest.mark.slow
@pytest.mark.parametrize("name,pictures", ACCEPTED, ids=[n for n, _ in ACCEPTED])
def test_conformance(device, material, name, pictures):
    """Every picture of the stream the same as ffmpeg's, in the block's own layout; a Main10 stream narrowed to its top eight bits."""
    if name in UNVERIFIED:
        pytest.skip(UNVERIFIED[name])
    r = vctest(device, f"DECODE {material.ram(f'conformance/{name}.h265')} {NATIVE} REF {material.ram(f'conformance/{name}.colmd5')}", 300)
    assert r.num("decode.frames") == pictures, f"{r.num('decode.frames')} pictures, ffmpeg showed {pictures}: {r.kv.get('fail')}"
    if name in KNOWN:
        assert r.kv.get("session.error") is None, r.kv.get("fail")
        pytest.xfail(f"{KNOWN[name]}: {r.num('ref.mismatches')} of {pictures} pictures differ")
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == pictures and r.num("ref.mismatches") == 0


@pytest.mark.parametrize("name,pictures", REFUSED, ids=[n for n, _ in REFUSED])
@pytest.mark.covers("VCERR_UNSUPPORTED")
def test_refused(device, material, name, pictures):
    """A stream the backend cannot take (a picture over 4096 or under 16 pixels a side, unequal bit depths) ends its session with VCERR_UNSUPPORTED (-6), no hang."""
    r = vctest(device, f"DECODE {material.ram(f'conformance/{name}.h265')} {NATIVE} NOHASH", 60)
    assert r.rc == 20 and r.num("session.error", r.num("error", 0)) == -6, f"rc {r.rc}: {r.kv.get('fail')}"


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left on the backend."""
    assert_nothing_left(device)
    status(device)
