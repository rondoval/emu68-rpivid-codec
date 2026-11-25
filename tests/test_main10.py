# SPDX-License-Identifier: GPL-2.0
"""Main10 through VCTest DECODE (developer-guide.md section 4.4): the narrowed frames bit exact against the host's truncation, the rate with the pass, the refusal past 1080p."""
import pytest
from vchiqtest.tools import assert_nothing_left, backend, status, vctest

NATIVE = "OUTFORMAT NV12_COL128"


def decode(device, material, clip, extra="", ref=True, timeout=300):
    """VCTest DECODE of a clip of the fixtures, natively, against its column reference unless @ref is False."""
    args = f"DECODE {material.ram(clip)} {NATIVE}"
    if ref:
        args += f" REF {material.ram(clip.replace('.h265', '.colmd5'))}"
    return vctest(device, f"{args} {extra}", timeout)


@pytest.mark.smoke
def test_bit_exact_10(device, material):
    """Three hundred pictures of 1080p Main10, each narrowed by the 68k to the top eight bits: the same as ffmpeg's output truncated on the host."""
    r = decode(device, material, "clip1080_10.h265")
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.frames") == 300 and r.num("stream.0.width") == 1920
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


@pytest.mark.covers("VCB_GetInfo")
def test_narrowed_counted(device, material):
    """The backend counts every narrowed picture; the status lists it."""
    before = int(backend(status(device), "rpivid.codec").get("counter.narrowed", 0))
    r = decode(device, material, "clip1080_10.h265", "NOHASH FRAMED", ref=False)
    assert r.passed, r.kv.get("fail")
    after = int(backend(status(device), "rpivid.codec").get("counter.narrowed", 0))
    assert after - before == 300, f"narrowed {before} -> {after}"


def test_speed_10(device, material, baselines):
    """The rate at 1080p Main10 with the pass in the client's task: the baseline of the hardware and the 68k."""
    r = decode(device, material, "clip1080_10.h265", "NOHASH", ref=False, timeout=300)
    assert r.passed, r.kv.get("fail")
    baselines.check_near("h265.1080_10.fps", r.num("decode.fps"), higher=True, tolerance="10%")


def test_refused_4k_10(device, material):
    """Main10 past 1080p is refused with VCERR_UNSUPPORTED (-6) at its first picture, no hang."""
    r = decode(device, material, "clip4k_10.h265", "NOHASH", ref=False, timeout=60)
    assert r.rc == 20 and r.num("session.error", r.num("error", 0)) == -6, f"rc {r.rc}: {r.kv.get('fail')}"


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left on the backend, every picture accounted for."""
    r = status(device)
    b = backend(r, "rpivid.codec")
    assert int(b["sessions"]) == 0, f"{b['sessions']} sessions still open"
    assert_nothing_left(device)
