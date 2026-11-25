# SPDX-License-Identifier: GPL-2.0
"""What a stream or the machine may do to a session (developer-guide.md section 4.4): damaged streams, packets cut at any
size, a shape change mid-stream, and little memory left in the block's reach."""
import json
import sys
from pathlib import Path

import pytest
from videocodectest.tools import assert_nothing_left, backend, held, status, vctest

sys.path.insert(0, str(Path(__file__).resolve().parent / "host"))
import broken  # noqa: E402  (tests/host/broken.py: the damaged streams' model, shared with break-fixtures.py)

NATIVE = "OUTFORMAT NV12_COL128"
LEAVE_MIB = (64, 16, 4)
FIXTURES = Path(__file__).resolve().parent / "fixtures"
EXPECT = FIXTURES / "broken" / "EXPECT"
DAMAGED = json.loads(EXPECT.read_text()) if EXPECT.exists() else {"fixtures": {}}
PACKET_SIZES = (1, 2, 3, 5, 188, 1500, 65537)  # a byte at a time, start codes split every way, a TS packet, an MTU, past the default
VCERR_STREAM = -8
SPS_HEADER = b"\x00\x00\x00\x01\x42\x01"  # a start code, then an SPS's NAL header (type 33, layer 0, temporal id 0)
BAD_SPS_ID = 16                                  # past the 15 an SPS's id may be



def ue(value: int) -> str:
    """@value as an Exp-Golomb code: its bits."""
    code = format(value + 1, "b")
    return "0" * (len(code) - 1) + code


def escaped(payload: bytes) -> bytes:
    """@payload with Annex B's emulation prevention: a 3 after two zeros where a byte of 0 to 3 follows."""
    out, zeros = bytearray(), 0
    for byte in payload:
        if zeros >= 2 and byte <= 3:
            out.append(3)
            zeros = 0
        out.append(byte)
        zeros = zeros + 1 if byte == 0 else 0
    return bytes(out)


def bad_sps() -> bytes:
    """An H.265 SPS that is well formed up to its sps_seq_parameter_set_id, which is 16: a stream error, not a feature
    the backend leaves out."""
    bits = "0000" + "000" + "1"           # VPS id 0, one sub-layer, temporal id nesting
    bits += "00" + "0" + "00001"           # profile space, tier, Main
    bits += format(0x60000000, "032b")     # compatible with Main and Main 10
    bits += "1001" + "0" * 44              # progressive, frame only, the reserved bits
    bits += format(93, "08b")              # level 3.1
    bits += ue(BAD_SPS_ID) + "1"           # the id, then rbsp_stop_one_bit
    bits += "0" * (-len(bits) % 8)
    return SPS_HEADER + escaped(bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8)))

def dropped(device) -> int:
    """The backend's count of pictures it did not decode."""
    return int(backend(status(device), "rpivid.codec").get("counter.dropped", 0))


@pytest.mark.parametrize("framed", (False, True), ids=("stream", "framed"))
@pytest.mark.parametrize("name", sorted(DAMAGED["fixtures"]))
def test_damaged(device, material, record_property, name, framed):
    """A damaged stream (tests/host/break-fixtures.py): no hang, no session error, the end mark; every picture the
    damage cannot reach bit exact and in order, the ones it reaches wrong or missing at most (broken.check())."""
    fixture = DAMAGED["fixtures"][name]
    before = dropped(device)
    r = vctest(device, f"DECODE {material.ram('broken/' + fixture['file'])} {NATIVE} HASHES" + (" FRAMED" if framed else ""), 180)
    record_property("dropped", dropped(device) - before)
    assert r.passed, f"{fixture['what']}: {r.kv.get('fail')} (session error {r.kv.get('session.error')})"
    assert r.kv.get("decode.eos") == "yes", f"{fixture['what']}: no end mark"
    frames = [r.kv.get(f"frame.{i}.md5", "") for i in range(int(r.num("decode.frames")))]
    reference = broken.hashes(FIXTURES / DAMAGED["reference"])
    why = broken.check(frames, reference, DAMAGED["display"], set(fixture["reached"]), fixture["extra"])
    record_property("frames", len(frames))
    assert why is None, f"{fixture['what']}: {why}"


@pytest.mark.parametrize("size", PACKET_SIZES)
def test_cut_anywhere(device, material, size):
    """The 720p clip in packets of @size bytes, whatever falls where: all 300 pictures bit exact."""
    r = vctest(device, f"DECODE {material.ram('clip720.h265')} {NATIVE} PACKET {size} REF {material.ram('clip720.colmd5')}", 600)
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


def test_cut_framed(device, material):
    """One access unit per packet, each split over packets of 1500 bytes (FrameStart, FrameEnd): bit exact."""
    r = vctest(device, f"DECODE {material.ram('clip720.h265')} {NATIVE} FRAMED PACKET 1500 REF {material.ram('clip720.colmd5')}", 300)
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


@pytest.mark.covers("VCEV_StreamInfo")
def test_shape_change(device, material):
    """Two clips end to end, 720p then 1080p: VCEV_StreamInfo twice, the frames remade, all 600 pictures bit exact."""
    r = vctest(device, f"DECODE {material.ram('clip_shape.h265')} {NATIVE} REF {material.ram('clip_shape.colmd5')}", 400)
    assert r.passed, r.kv.get("fail")
    assert r.num("stream.0.width") == 1280 and r.num("stream.1.width") == 1920, "the second shape was not announced"
    assert r.num("decode.frames") == 600
    assert r.num("ref.compared") == 600 and r.num("ref.mismatches") == 0


def test_shape_change_held(device, material):
    """The two clips through the converter, each picture held until the next comes (KEEP), as a player shows one: the
    second shape brings a new converter while the old one stays open for the picture held (one session more), that
    picture stays as it was, and all 600 pictures come."""
    r = vctest(device, f"DECODE {material.ram('clip_shape.h265')} OUTFORMAT BGRA32 KEEP", 400)
    assert r.passed, r.kv.get("fail")
    assert r.num("stream.0.width") == 1280 and r.num("stream.1.width") == 1920, "the second shape was not announced"
    assert r.num("keep.changed") == 0
    assert r.num("keep.sessions_max") == r.num("keep.sessions_min") + 1, "the old converter was not kept for the picture held"
    assert r.num("decode.frames") == 600


@pytest.mark.parametrize("leave_mib", LEAVE_MIB)
@pytest.mark.covers("VCERR_NOMEM")
def test_low_memory(device, material, leave_mib):
    """With this much of the block's reach left: a 1080p session runs, or is refused with VCERR_NOMEM (-1), and nothing stays open."""
    with held(device, leave_mib):
        r = vctest(device, f"DECODE {material.ram('clip720.h265')} {NATIVE} NOHASH FRAMES 30", 120)
        if not r.passed:
            error = r.num("session.error", r.num("open.error", r.num("error", 0)))
            assert error == -1, f"refused with {error}, not VCERR_NOMEM: {r.kv.get('fail')}"
        b = backend(status(device), "rpivid.codec")
        assert int(b["sessions"]) == 0, f"{b['sessions']} sessions still open"


def test_after_low_memory(device, material):
    """With the memory back: a whole clip bit exact, as ever."""
    r = vctest(device, f"DECODE {material.ram('clip720.h265')} {NATIVE} REF {material.ram('clip720.colmd5')}", 300)
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


@pytest.mark.covers("VCERR_STREAM")
def test_a_bad_sps_as_extradata(device):
    """A configuration whose SPS breaks the standard (an id past 15): the open is refused with VCERR_STREAM (-8)."""
    device.upload_bytes("RAM:hw/bad-sps.bin", bad_sps())
    r = vctest(device, f"DECODE RAM:hw/bad-sps.bin ENCODING H265 {NATIVE} CONFIG RAM:hw/bad-sps.bin NOHASH", 60)
    device.delete("RAM:hw/bad-sps.bin")
    assert r.kv.get("decode.session") == "refused" and r.num("error") == VCERR_STREAM, f"rc {r.rc}: {r.kv}"


@pytest.mark.covers("VCERR_STREAM")
def test_a_bad_sps_in_the_stream(device, material):
    """The same SPS ahead of a good clip: the session ends with VCERR_STREAM at that unit, no hang."""
    device.upload_bytes("RAM:hw/bad-sps.h265", bad_sps() + material.path("clip720.h265").read_bytes())
    r = vctest(device, f"DECODE RAM:hw/bad-sps.h265 {NATIVE} NOHASH", 60)
    device.delete("RAM:hw/bad-sps.h265")
    assert r.num("session.error", r.num("error", 0)) == VCERR_STREAM, f"rc {r.rc}: {r.kv.get('fail')}"


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left on the backend."""
    assert_nothing_left(device)
