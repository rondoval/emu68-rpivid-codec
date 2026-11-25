# SPDX-License-Identifier: GPL-2.0
"""The H.265 decoder through VCTest DECODE (developer-guide.md section 4.4): bit exact in its native column format, with its
parameter sets as extradata, two sessions, a flush, the end mark, a refusal, the codec row and what a chain adds to it, the
frames whole and through the chain, the rate, the 68k's cost and a reset."""
import re
import time

import numpy as np
import pytest
from vchiqtest.material import ffmpeg
from vchiqtest.pictures import best_psnr, judge_bars, pattern, ref_rgb, rgb565_pixels
from emu68test.results import parse
from vchiqtest.tools import assert_nothing_left, backend, status, vctest

NATIVE = "OUTFORMAT NV12_COL128"
EXECUTIVE_STAT = "SYS:Utilities/EXPANSION/Executive/Stat"  # AmiKit's scheduler (CLAUDE.md: what it does to timings)
CPU_DECODES = 10  # decodes in one sample of the 68k's time: one alone is too short for Executive's totals
CPU_IDLE_S = 10   # seconds over which the idle machine's rate is taken, just before the sample
CLIP_READING = "bt709/limited"  # clip720 names no colours: a picture of 720 rows is taken as BT.709 in limited range
VCCS = {"bars709": 2, "bars709full": 4}  # VCCS_BT709 and VCCS_BT709_Full (libraries/videocodec.h): what each bars clip says
# The least PSNR against the host's conversion: RGB16 loses to 5-6-5 bits and ffmpeg dithers its RGB565
# reference, so the ISP's own RGB16 scores some 36 dB there (the vchiq suite's convert_720p_rgb16 baseline)
SCREEN_LEAST_DB = {"BGRA32": 40.0, "RGB16": 34.0}
IMPORTED_FRAMES = 8       # the most DECODE imports: a 1080p stream's references and the pictures shown with them
VCERR_BADPARAM = -4
VCERR_BADSTATE = -5
VCERR_UNSUPPORTED = -6


def decode(device, material, clip, extra="", ref=True, timeout=300):
    """VCTest DECODE of a clip of the fixtures, natively, against its column reference unless @ref is False."""
    args = f"DECODE {material.ram(clip)} {NATIVE}"
    if ref:
        args += f" REF {material.ram(clip.replace('.h265', '.colmd5'))}"
    return vctest(device, f"{args} {extra}", timeout)


@pytest.mark.smoke
@pytest.mark.covers("VCB_OpenSession", "VCB_CloseSession", "VCB_SendPacket", "VCB_Poll", "VCB_ReleaseFrame")
def test_bit_exact(device, material):
    """Three hundred pictures of 1080p, every one the same as ffmpeg's, in the block's own layout."""
    r = decode(device, material, "clip1080.h265")
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.frames") == 300 and r.num("stream.0.width") == 1920
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


@pytest.mark.smoke
def test_bit_exact_framed(device, material):
    """One access unit per packet with timestamps (FRAMED): the same three hundred pictures bit exact."""
    r = decode(device, material, "clip1080.h265", "FRAMED")
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


def test_720p(device, material):
    """A clip without scaling lists after one with them (the factor RAM): bit exact."""
    r = decode(device, material, "clip720.h265")
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


def test_4k(device, material):
    """Forty-eight pictures of 4K: bit exact."""
    r = decode(device, material, "clip4k.h265", timeout=300)
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.frames") == 48 and r.num("stream.0.width") == 3840
    assert r.num("ref.compared") == 48 and r.num("ref.mismatches") == 0


@pytest.mark.covers("VCEV_EndOfStream")
def test_end_of_stream(device, material):
    """The end mark: every picture out, VCEV_EndOfStream after the last."""
    r = decode(device, material, "clip720.h265", "FRAMED")
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.frames") == 300 and r.kv.get("decode.eos") == "yes"


def test_two_sessions(device, material):
    """Two decoder sessions on one block, each its own clip, both bit exact."""
    a = device.start("VCTest", f"DECODE {material.ram('clip1080.h265')} {NATIVE} REF {material.ram('clip1080.colmd5')}")
    b = device.start("VCTest", f"DECODE {material.ram('clip720.h265')} {NATIVE} REF {material.ram('clip720.colmd5')} FRAMED")
    ra, rb = a.wait(400), b.wait(400)
    for r in (ra, rb):
        assert r.passed, r.kv.get("fail")
        assert r.num("ref.mismatches") == 0


@pytest.mark.covers("VCB_Flush")
def test_flush_replays(device, material):
    """VC_Flush() after a hundred pictures, the clip again: no picture from before the flush comes after it."""
    r = decode(device, material, "clip720.h265", "FRAMED NOHASH FLUSH 100", ref=False, timeout=300)
    assert r.passed, r.kv.get("fail")
    assert r.num("flush.rc") == 0 and r.num("flush.frames") > 0
    assert r.num("flush.stale") == 0 and r.num("flush.untimed") == 0


@pytest.mark.covers("VCEV_Error", "VCERR_UNSUPPORTED")
def test_refused_4k_main10(device, material):
    """Main10 past 1080p is refused with VCERR_UNSUPPORTED (-6) at its first picture (the session's error event), no hang."""
    r = decode(device, material, "clip4k_10.h265", "NOHASH", ref=False, timeout=60)
    assert r.rc == 20 and r.num("session.error", r.num("error", 0)) == -6, f"rc {r.rc}: {r.kv.get('fail')}"


@pytest.mark.covers("VCB_QueryCodec")
def test_codec_row(device):
    """VCTest CODECS: rpivid.codec's one row writes its columns only, and VC_QueryCodec() lists what the chain adds
    (ChainOutputFormats), the screen formats test_screen decodes to among them."""
    r = vctest(device, "CODECS", 30)
    assert r.passed, r.kv.get("fail")
    rows = [c for c in r.group("codec") if c["backend"] == "rpivid.codec"]
    assert len(rows) == 1, f"{len(rows)} rows of rpivid.codec"
    row = rows[0]
    assert (row["name"], row["kind"], row["session"]) == ("H265", "decoder", "yes"), row
    assert row.get("output_formats") == "NV12_COL128", f"writes {row.get('output_formats')}"
    chained = set(row.get("chain_output_formats", "").split(","))
    assert {"BGRA32", "RGB16"} <= chained, f"the chain adds {sorted(chained)}"


@pytest.mark.covers("VCB_OutputFrame")
def test_through_isp(device, material):
    """An RGB output: the frontend puts the ISP converter behind the decoder; every picture comes out, scaled to 640x360."""
    r = vctest(device, f"DECODE {material.ram('clip720.h265')} OUTFORMAT RGBA32 NOHASH", 120)
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.frames") == 300 and r.kv.get("stream.0.format") == "RGBA32"


@pytest.mark.covers("VCB_SetAttrs")
def test_resignal_through_isp(device, material):
    """Through the converter, the session moved to another signal bit after thirty pictures (VC_SetAttrsTags(VCA_SigMask)):
    VCTest waits for that bit only from then on, and every picture still comes out, so the chain's sessions signal it."""
    r = vctest(device, f"DECODE {material.ram('clip720.h265')} OUTFORMAT RGBA32 RESIGNAL 30", 120)
    assert r.passed, r.kv.get("fail")
    assert r.num("resignal.at") == 30 and r.num("resignal.rc") == 0
    assert r.num("decode.frames") == 300


def test_speed_1080(device, material, baselines):
    """The decoder's own rate at 1080p, no hashing: the baseline of the hardware."""
    r = decode(device, material, "clip1080.h265", "NOHASH", ref=False, timeout=300)
    assert r.passed, r.kv.get("fail")
    baselines.check_near("h265.1080.fps", r.num("decode.fps"), higher=True, tolerance="10%")


def test_speed_4k(device, material, baselines):
    """The same at 4K."""
    r = decode(device, material, "clip4k.h265", "NOHASH", ref=False, timeout=300)
    assert r.passed, r.kv.get("fail")
    baselines.check_near("h265.4k.fps", r.num("decode.fps"), higher=True, tolerance="10%")


def test_extradata_annexb(device, material):
    """The parameter sets before the first picture taken out of the stream and given as extradata (FRAMED EXTRADATA):
    the session opens with the shape known, and all 300 pictures are bit exact."""
    r = decode(device, material, "clip1080.h265", "FRAMED EXTRADATA")
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.extradata") > 0
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


@pytest.mark.parametrize("framed", (False, True), ids=("stream", "framed"))
def test_extradata_hvcc(device, material, framed):
    """FFmpeg's hvcC record of the clip as the configuration (CONFIG), the stream without any parameter set
    (clip1080_bare.h265, which FFmpeg alone cannot decode): all 300 pictures bit exact."""
    args = (f"DECODE {material.ram('clip1080_bare.h265')} {NATIVE} CONFIG {material.ram('clip1080.hvcc')} "
            f"REF {material.ram('clip1080.colmd5')}" + (" FRAMED" if framed else ""))
    r = vctest(device, args, 300)
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


def column_planes(frame: np.ndarray, w: int, h: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """A column frame (columns, lines, 128) as its Y, U and V planes: the luma rows, then the interleaved chroma rows."""
    rows = (h + 15) & ~15
    luma = np.concatenate(list(frame[:, :rows, :]), axis=1)
    chroma = np.concatenate(list(frame[:, rows:rows + rows // 2, :]), axis=1)
    return luma[:h, :w], chroma[:h // 2, 0:w:2], chroma[:h // 2, 1:w:2]


def test_native(device, material):
    """The column frames whole to a file (OUT), unpacked on the host as the hardware facts give the layout: every plane
    of ten pictures the same as FFmpeg's. The route for looking into a picture whose hash differs."""
    out, count, w, h = "RAM:hw/native.col", 10, 1280, 720
    r = vctest(device, f"DECODE {material.ram('clip720.h265')} {NATIVE} FRAMES {count} OUT {out} NOHASH", 120)
    try:
        assert r.passed, r.kv.get("fail")
        data = device.download(out)
    finally:
        device.delete(out)
    lines = ((h + 15) & ~15) * 3 // 2
    columns = (w + 127) // 128
    size = columns * lines * 128
    assert len(data) == count * size, f"{len(data)} bytes for {count} frames of {size}"
    raw = ffmpeg("-i", str(material.path("clip720.h265")), "-frames:v", str(count), "-f", "rawvideo", "-pix_fmt",
                 "yuv420p", "-")
    plane = w * h // 4
    for i in range(count):
        y, u, v = column_planes(np.frombuffer(data, np.uint8, size, i * size).reshape(columns, lines, 128), w, h)
        ref = np.frombuffer(raw, np.uint8, w * h * 3 // 2, i * w * h * 3 // 2)
        assert np.array_equal(y, ref[:w * h].reshape(h, w)), f"picture {i}: luma differs"
        assert np.array_equal(u, ref[w * h:w * h + plane].reshape(h // 2, w // 2)), f"picture {i}: Cb differs"
        assert np.array_equal(v, ref[w * h + plane:].reshape(h // 2, w // 2)), f"picture {i}: Cr differs"


@pytest.mark.parametrize("fmt", sorted(SCREEN_LEAST_DB))
def test_screen(device, material, fmt):
    """The pictures through the frontend's chain in two of the screen's formats, the ISP converting: the first one
    against the host's conversion of the clip's colours (SCREEN_LEAST_DB: 40 dB for BGRA32, 34 for RGB16)."""
    out = f"RAM:hw/screen.{fmt.lower()}"
    r = vctest(device, f"DECODE {material.ram('clip720.h265')} OUTFORMAT {fmt} FRAMES 1 OUT {out}", 120)
    try:
        assert r.passed, r.kv.get("fail")
        data = device.download(out)
    finally:
        device.delete(out)
    w, h = int(r.num("frame.first.width")), int(r.num("frame.first.height"))
    src = ["-i", str(material.path("clip720.h265")), "-frames:v", "1"]
    if fmt == "BGRA32":
        picture = np.frombuffer(data, np.uint8, w * h * 4).reshape(h, w, 4)[..., [2, 1, 0]]
        refs = ref_rgb(src, w, h)
    else:
        picture = rgb565_pixels(data, w, h)
        refs = ref_rgb(src, w, h, "rgb565le")
    score, how = best_psnr(picture, refs)
    assert how == CLIP_READING and score > SCREEN_LEAST_DB[fmt], f"{fmt}: {score:.1f} dB, read as {how}"


@pytest.mark.parametrize("clip", sorted(VCCS))
def test_colours_through_the_chain(device, material, clip):
    """The colour bars as BT.709, limited and full range, to BGRA32: the stream's colour space reaches the converter
    behind the decoder, and the bars come out in their own colours."""
    out = "RAM:hw/bars.bgra"
    r = vctest(device, f"DECODE {material.ram(clip + '.h265')} OUTFORMAT BGRA32 FRAMES 3 NOHASH OUT {out}", 120)
    try:
        assert r.passed, r.kv.get("fail")
        data = device.download(out)
    finally:
        device.delete(out)
    assert r.num("stream.0.colorspace") == VCCS[clip], "the colour space the session reports"
    picture = np.frombuffer(data[-1280 * 720 * 4:], np.uint8).reshape(720, 1280, 4)[..., [2, 1, 0]]
    problems = judge_bars(picture, pattern(1280, 720)[1])
    assert not problems, "\n".join(problems)


def cpu_seconds(device) -> tuple[float, float]:
    """Executive's total used and idle CPU time, in seconds."""
    rc, text = device.exec(EXECUTIVE_STAT, timeout=20)
    plain = re.sub(r"\x1b\[[0-9;]*m", "", text)
    times = {}
    for kind in ("used", "idle"):
        m = re.search(rf"Total {kind} CPU time:\s*(?:(\d+)d\s*)?(\d+):(\d+):(\d+)\.(\d+)", plain)
        assert m, f"Stat said no {kind} time: {plain[:300]}"
        days, hours, minutes, seconds, hundredths = (int(g or 0) for g in m.groups())
        times[kind] = ((days * 24 + hours) * 60 + minutes) * 60 + seconds + hundredths / 100
    return times["used"], times["idle"]


def test_cpu(device, material, baselines, record_property):
    """The 68k's time a 1080p picture costs, decoded as fast as it comes (NOHASH): Executive's used CPU time across
    CPU_DECODES decodes in one script, less what the machine uses with nothing running (measured over CPU_IDLE_S just
    before), by the pictures decoded. One decode of a second is too short for Executive's totals: such readings spread
    from 120 to 700 us a picture, where samples of ten decodes agree within a few per cent."""
    rc, _ = device.exec(f"List >NIL: {EXECUTIVE_STAT}", timeout=15)
    if rc != 0:
        pytest.skip("no Executive on this machine: it gives the CPU times")
    out, script = f"{device.ram}/cpu.txt", f"{device.ram}/cpu"
    line = f"{device.command('VCTest')} >>{out} DECODE {material.ram('clip1080.h265')} {NATIVE} NOHASH\n"
    device.upload_bytes(script, (line * CPU_DECODES).encode("latin-1"))
    device.exec(f"Delete >NIL: {out} QUIET", timeout=20)
    used0, idle0 = cpu_seconds(device)
    time.sleep(CPU_IDLE_S)
    used1, idle1 = cpu_seconds(device)
    background = (used1 - used0) / max(1e-6, (used1 - used0) + (idle1 - idle0))
    device.exec(f"Execute {script}", timeout=CPU_DECODES * 60)
    used2, idle2 = cpu_seconds(device)
    text = device.download_text(out)
    device.keep(out, text)
    runs = [parse(part, 0) for part in re.split(r"(?m)^(?=format=)", text) if part.strip()]  # each run's output
    assert len(runs) == CPU_DECODES and all(r.passed for r in runs), f"not every decode passed:\n{text[-800:]}"
    used, total = used2 - used1, (used2 + idle2) - (used1 + idle1)
    frames = sum(r.num("decode.frames") for r in runs)
    per_picture_us = max(0.0, used - background * total) / frames * 1e6
    record_property("background_pct", round(background * 100, 1))
    record_property("busy_pct", round(used / max(1e-6, total) * 100, 1))
    baselines.check_near("h265.1080.cpu_us_per_picture", round(per_picture_us), higher=False, tolerance="20%")


@pytest.mark.reboot
def test_reset(device, material):
    """C:Reboot with a decode in flight (the reset guard quiesces the block): the machine comes back, and the next
    decode is bit exact."""
    job = device.start("VCTest", f"DECODE {material.ram('clip1080.h265')} {NATIVE} REF {material.ram('clip1080.colmd5')}")
    time.sleep(4)  # hashing on the 68k makes the decode last some 20 s: well inside it
    assert not job.done(), "the decode ended before the reset"
    device.reboot()
    r = decode(device, material, "clip720.h265")
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


@pytest.mark.covers("VCB_ImportPacket")
def test_imported_packets(device, material):
    """Input packets of the program's own memory (VC_ImportPacket()), sent first: three hundred pictures bit exact."""
    r = decode(device, material, "clip1080.h265", "IMPORT 4")
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.imported_packets") == 4
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


@pytest.mark.covers("VCB_ImportFrame")
def test_imported_frames(device, material):
    """Column frames of the program's own memory (VC_ImportFrame()), their shape from VCA_Width and VCA_Height: the
    block decodes into them, three hundred pictures bit exact."""
    r = decode(device, material, "clip1080.h265", f"IMPORTFRAMES {IMPORTED_FRAMES} WIDTH 1920 HEIGHT 1080")
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.imported_frames") == IMPORTED_FRAMES
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


@pytest.mark.covers("VCB_ImportFrame", "VCERR_BADPARAM")
def test_imported_frames_of_another_shape(device, material):
    """Imported frames of 720p under a 1080p stream: the session ends with VCERR_BADPARAM at the first picture, no
    hang; it cannot remake frames that are the program's."""
    r = decode(device, material, "clip1080.h265", f"IMPORTFRAMES {IMPORTED_FRAMES} WIDTH 1280 HEIGHT 720", ref=False, timeout=60)
    assert r.num("decode.imported_frames") == IMPORTED_FRAMES, r.kv.get("fail")
    assert r.num("session.error", r.num("error", 0)) == VCERR_BADPARAM, f"rc {r.rc}: {r.kv.get('fail')}"


@pytest.mark.covers("VCB_Image", "VCB_SendFrame", "VCB_ReleasePacket", "VCERR_BADPARAM", "VCERR_BADSTATE")
def test_backend_refusals(device):
    """The backend's entry points called directly with what no program may hand them: short structures, a row past
    the table's end, no parameters, a handle no session has, each refused; a picture to send and an image call, which
    a decoder of streams takes neither of, refused too."""
    r = vctest(device, "PROBE rpivid.codec", 30)
    assert r.passed, r.kv.get("fail")
    for key in ("getinfo_short", "querycodec_short", "querycodec_past", "open_null_error", "open_short_error",
                "sendpacket_null", "sendpacket", "flush", "setattrs_null", "setattrs_short"):
        assert r.num(f"probe.{key}") == VCERR_BADPARAM, f"probe.{key} = {r.kv.get(f'probe.{key}')}"
    assert r.num("probe.sendframe") == VCERR_BADSTATE, r.kv.get("probe.sendframe")
    assert not r.flag("probe.images") and r.num("probe.image") == VCERR_UNSUPPORTED, r.kv.get("probe.image")
    for key in ("open_null", "open_short", "importpacket", "importframe", "outputframe", "releases"):
        assert r.flag(f"probe.{key}"), f"probe.{key}"


@pytest.mark.covers("VCB_Idle")
def test_the_block_goes_down_after_the_last_close(device, material):
    """The block is up while a session is open, and down again once the library's last opener closed it (VCB_Idle())."""
    r = decode(device, material, "clip720.h265", "FRAMES 30", ref=False)
    assert r.passed, r.kv.get("fail")
    during = next(row for row in r.group("open.backend") if row["name"] == "rpivid.codec")
    assert during["sessions"] == "1" and during["interface"].startswith("HEVC"), during
    after = backend(status(device), "rpivid.codec")
    assert after["sessions"] == "0" and after["interface"] == "", f"after the last close: {after}"


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left on the backend, every picture accounted for."""
    r = status(device)
    b = backend(r, "rpivid.codec")
    assert int(b["sessions"]) == 0, f"{b['sessions']} sessions still open"
    assert_nothing_left(device)
