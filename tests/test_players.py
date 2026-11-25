# SPDX-License-Identifier: GPL-2.0
"""What a user meets (developer-guide.md section 4.4): Kinora plays H.265 at the film's rate through rpivid.codec,
the library's example player VCPlay plays it in a window, and Kinora's archive, installed by its own Installer,
puts the backend in place on a machine without it."""
import re
import time
from dataclasses import dataclass
from pathlib import Path

import pytest
from videocodectest.suite import RESIDENT
from emu68test.results import expect
from videocodectest.tools import backend, status, vctest

# -- Kinora

KINORA = "SYS:Utilities/Kinora/Kinora"  # where Kinora's Installer puts it
PORT_WAIT_S = 20        # for Kinora's ARexx port after its start
PLAY_S = 6              # seconds of the host's time the rate is taken over, in the window: the films last 10
RATE_TOLERANCE = 0.02   # the pictures shown against the film's rate over that time
DROPPED_MAX = 2         # pictures Kinora may drop meanwhile, as its own suite allows
FILM_RATE = 30          # the players' MP4s: 30 pictures a second (make-fixtures.sh)


@dataclass
class KinoraStatus:
    """Kinora's answer to STATUS."""

    state: str          # EMPTY, PAUSED, PLAYING or ENDED
    position_ms: int
    shown: int
    dropped: int
    error: int


@pytest.fixture(scope="module")
def kinora(request, device) -> str:
    """The Kinora program on the Amiga: --kinora, or where its Installer puts it; the tests skip without one."""
    program = request.config.getoption("--kinora") or KINORA
    rc, _ = device.exec(f"List >NIL: {program}", timeout=15)
    if rc != 0:
        pytest.skip(f"no Kinora at {program} on the Amiga: install it, or give --kinora")
    return program


def kinora_rexx(device, command: str) -> str:
    """One command to Kinora's ARexx port KINORA; its RESULT. Fails when the port refuses it."""
    source = "options results\naddress KINORA\n'%s'\nreturn rc '|' result\n" % command.replace("'", "''")
    _, answer = device.retry(lambda: device.ami.arexx(source))
    code, _, result = answer.partition(" | ")
    assert code.strip() == "0", f"{command}: Kinora's port answered {answer!r}"
    return "" if result.strip() == "RESULT" else result


def kinora_status(device) -> KinoraStatus:
    """STATUS, read: the state, the position, shown, dropped and the error (the duration, the size and the full screen
    left out)."""
    words = kinora_rexx(device, "STATUS").split()
    assert len(words) == 9, f"STATUS answered {words}"
    state, position, _, _, _, shown, dropped, error, _ = words
    return KinoraStatus(state, int(position), int(shown), int(dropped), int(error))


def kinora_wait(device, what, timeout: float, why: str) -> KinoraStatus:
    """STATUS polled until @what(status) holds; the port's absence counts as not yet."""
    deadline = time.time() + timeout
    while True:
        try:
            status = kinora_status(device)
            if what(status):
                return status
        except (AssertionError, device.mod.AmigaError):
            status = None
        assert time.time() < deadline, f"{why}: not within {timeout} s, last {status}"
        time.sleep(0.5)


def kinora_quit(device) -> None:
    """QUIT, and wait for Kinora to be gone; Ctrl-C when the port does not answer."""
    try:
        kinora_rexx(device, "QUIT")
    except (AssertionError, device.mod.AmigaError):
        pass
    deadline = time.time() + 15
    while device.procs("Kinora") and time.time() < deadline:
        time.sleep(1)
    if device.procs("Kinora"):
        device.break_all("Kinora")
    assert not device.procs("Kinora"), "Kinora did not end"


def kinora_plays(device, program: str, film: str, width: int, record_property) -> tuple[int, int]:
    """Kinora plays @film, @width pictures wide, in its window: the picture decoded by rpivid.codec, shown at the
    film's rate, nothing failed. Return: the width and height of the frames Kinora was handed.

    The window, not the full screen: Kinora's full screen, a screen of four parts, drops a picture a second at 1080p
    and 4K alike, its own cost, where the window drops none."""
    before = int(backend(status(device), "rpivid.codec").get("counter.pictures", 0))
    device.break_all("Kinora")
    device.wake()
    device.start(program, film)
    try:
        kinora_wait(device, lambda s: s.state == "PLAYING" and s.position_ms >= 1000, PORT_WAIT_S + 15, "playing")
        start = kinora_status(device)
        time.sleep(PLAY_S)
        end = kinora_status(device)
        lines = dict(line.split(": ", 1) for line in kinora_rexx(device, "STATISTICS").splitlines() if ": " in line)
    finally:
        kinora_quit(device)
    after = int(backend(status(device), "rpivid.codec").get("counter.pictures", 0))
    record_property("video", lines.get("Video", ""))
    record_property("pictures", lines.get("Pictures", ""))
    video = lines.get("Video", "")
    assert video.startswith(f"H.265 {width}x") and f"{FILM_RATE}.000 pictures a second" in video, f"Video: {video}"
    assert end.state == "PLAYING" and end.error == 0, f"at the end: {end}"
    moved, shown = end.position_ms - start.position_ms, end.shown - start.shown
    expected = moved * FILM_RATE / 1000
    assert abs(moved - PLAY_S * 1000) < 500, f"the film moved {moved} ms in {PLAY_S} s"
    assert abs(shown - expected) <= expected * RATE_TOLERANCE, f"{shown} pictures shown in {moved} ms of the film"
    assert end.dropped - start.dropped <= DROPPED_MAX, f"{end.dropped - start.dropped} pictures dropped"
    assert after - before >= end.shown, f"rpivid.codec decoded {after - before} pictures, Kinora showed {end.shown}"
    frame = re.search(r" of (\d+)x(\d+)", video)
    assert frame, f"no frame size in: {video}"
    return int(frame.group(1)), int(frame.group(2))


def test_kinora_1080(device, material, kinora, record_property):
    """Kinora plays a 1080p H.265 MP4 in its window, rpivid.codec decoding, at the film's rate."""
    kinora_plays(device, kinora, material.ram("player1080.mp4"), 1920, record_property)


def test_kinora_4k(device, material, kinora, record_property):
    """The same at 4K: the frames Kinora is handed are already the window's size, scaled by the VideoCore behind the
    decoder, not the film's."""
    frame_width, _ = kinora_plays(device, kinora, material.ram("player4k.mp4"), 3840, record_property)
    assert frame_width < 3840, f"Kinora was handed frames {frame_width} wide: the film's size, not the window's"


# -- VCPlay

def test_example_player_h265(device, material):
    """VCPlay, the library's example player, on an H.265 file: the screen's format from the row's ChainOutputFormats,
    the converter behind the decoder, the whole clip shown, return code 0."""
    rc, text = device.run_text("VCPlay", material.ram("clip720.h265"), timeout=60)
    assert rc == 0, f"return code {rc}:\n{text[-600:]}"
    expect(text, r"300 frames shown", "VCPlay's summary")


# -- the Installer

INSTALLER = "SYS:System/Installer"
PACKAGE = "RAM:hw/pkg"
INSTALL_LOG = "RAM:hw/install.log"
# What installing the video libraries alone may copy: the library, its codec drivers and VCInfo
WITH_VIDEO = {"LIBS/videocodec.library", "LIBS/videocodec/videocore.codec", "LIBS/videocodec/rpivid.codec", "C/VCInfo"}
PAGES_MAX = 20


def archive_of(request) -> Path:
    """The archive to install: --archive, or the newest Kinora .lha the package target left in Kinora's build dir
    (the parent of this repository's)."""
    chosen = request.config.getoption("--archive")
    if chosen:
        return Path(chosen)
    found = sorted(Path(request.config.getoption("--build-dir")).parent.joinpath("package").glob("Kinora-*.lha"),
                   key=lambda p: p.stat().st_mtime)
    if not found:
        pytest.skip("no archive: build Kinora's package target, or give --archive")
    return found[-1]


def window_of(archive: Path) -> str:
    """The Installer's window title's start: its script's path in the archive's drawer, which unpacks into PACKAGE."""
    return f"{PACKAGE}/{archive.stem}/Install"


def installer_page(device, window: str):
    """The Installer's window: (bounds, title, {gadget id: (kind, selected)}), or None once it has closed."""
    page, gadgets = None, {}
    for line in device.ami.ui_tree().splitlines():
        w = re.match(r'W\s+\d+\s+(-?\d+)\s+(-?\d+)\s+(\d+)x(\d+)\s+\S+\s+"(.*)"', line)
        if w:
            if page is not None:
                break
            if w.group(5).startswith(window):
                page = (tuple(int(w.group(i)) for i in range(1, 5)), w.group(5))
            continue
        g = re.match(r"G\s+(\d+)\s+-?\d+\s+-?\d+\s+(\d+)x(\d+)\s+button\s+(\S+)", line)
        if page is not None and g:
            kind = "check" if int(g.group(2)) > 18 else "radio"  # checkboxes are 21x18, radio buttons 16x9
            gadgets[int(g.group(1))] = (kind, g.group(4) == "selected")
    return None if page is None else (page[0], page[1], gadgets)


def next_page(device, window: str, before: int | None, timeout: float = 120):
    """The Installer's next page once it shows and holds still (@before: the last page's region hash), or None."""
    t0, last, still = time.time(), None, 0
    while time.time() - t0 < timeout:
        page = installer_page(device, window)
        if page is None:
            return None
        bounds, title, _ = page
        shown = device.ami.region_hash(*bounds)
        if "Press 'Esc'" not in title and shown != before and shown == last:
            still += 1
            if still >= 2:
                return page
        else:
            still = 0
        last = shown
        time.sleep(0.5)
    raise AssertionError("the Installer's window did not move on")


def answer(gadgets: dict) -> tuple[str, list[int]]:
    """What to click on a page, told by its gadgets: everything left out but the video libraries."""
    radios = sorted(i for i, (kind, _) in gadgets.items() if kind == "radio")
    checks = sorted(i for i, (kind, _) in gadgets.items() if kind == "check")
    if radios[:1] == [1]:
        return "the Installer's own: install for real, log to a file", [1, 4]
    if len(checks) == 5:  # Kinora, the video libraries, vcjpeg.datatype, VCGrab, DMXInfo and ACInfo
        want = {checks[1]}
        return "what to install: the video libraries only", [i for i in checks if gadgets[i][1] != (i in want)]
    if not radios and not checks:
        return "a message", []
    raise AssertionError(f"an Installer page this test does not know: {gadgets}")


def run_installer(device, window: str) -> list[str]:
    """The archive's Install script in EXPERT mode with only the video libraries ticked, driven page by page."""
    device.upload_bytes("RAM:hw/install.s", (
        f"CD {window.rsplit('/', 1)[0]}\n"
        f'Run >NIL: {INSTALLER} SCRIPT {window} APPNAME "Kinora" MINUSER EXPERT DEFUSER EXPERT '
        f"LOGFILE {INSTALL_LOG}\n").encode())
    device.wake()
    device.exec("Execute RAM:hw/install.s", timeout=20)
    seen, before = [], None
    for n in range(PAGES_MAX):
        page = next_page(device, window, before)
        if page is None:
            return seen
        bounds, _, gadgets = page
        what, clicks = answer(gadgets)
        seen.append(what)
        device.screenshot(bounds, name=f"installer-{n:02d}")
        for gadget in clicks:
            device.ami.ui_click(str(gadget), window)
        before = device.ami.region_hash(*bounds)
        device.ami.ui_click("90", window)  # Proceed
    raise AssertionError(f"the Installer still runs after {PAGES_MAX} pages: {seen}")


@pytest.mark.reboot
def test_install(request, device, material, record_property):
    """Kinora's archive installed by its Installer, the video libraries alone ticked, on a machine without
    rpivid.codec: the backend copied, and after a reboot listed with its codec and decoding bit exact."""
    rc, _ = device.exec("Which >NIL: LhA", timeout=15)
    if rc != 0:
        pytest.skip("no LhA on the Amiga to unpack the archive with")
    archive = archive_of(request)
    window = window_of(archive)
    drawer = window.rsplit("/", 1)[0]
    device.upload_once(archive, "RAM:hw/kinora.lha")
    device.exec(f"Delete >NIL: {PACKAGE} ALL QUIET", timeout=30)
    rc, text = device.exec(f"LhA >NIL: -q x RAM:hw/kinora.lha {PACKAGE}/", timeout=120)
    assert rc == 0, f"LhA: {text}"
    # the files the suites install that the Installer would copy: the archive's must be the install tree's, or the
    # Installer would put an older build over the one under test
    install_dir = Path(request.config.getoption("--install-dir"))
    stale = [local for local, _ in RESIDENT
             if local in WITH_VIDEO and device.download(f"{drawer}/{local}") != (install_dir / local).read_bytes()]
    assert not stale, f"{archive.name} is not of the installed build ({', '.join(stale)}): build the package target"
    ours = (install_dir / "LIBS/videocodec/rpivid.codec").read_bytes()

    device.exec(f"Delete >NIL: {INSTALL_LOG} QUIET", timeout=10)
    rc, _ = device.exec("Delete >NIL: LIBS:videocodec/rpivid.codec", timeout=10)
    assert rc == 0, "rpivid.codec was not there to take away"
    pages = run_installer(device, window)
    record_property("pages", "; ".join(pages))
    log = device.download_text(INSTALL_LOG)
    device.keep("install.log", log)
    assert "Pretend: No" in log, "the Installer only pretended"
    # the script names its files by the archive's drawer: what lies under it, as the archive holds it
    copied = {src.split(f"{archive.stem}/", 1)[-1] for src, _ in re.findall(r'Copy file "([^"]+)" to "([^"]+)"', log)}
    record_property("copied", ", ".join(sorted(copied)))
    assert "LIBS/videocodec/rpivid.codec" in copied, "the Installer did not copy rpivid.codec"
    assert copied <= WITH_VIDEO, f"copied beyond the video libraries: {sorted(copied - WITH_VIDEO)}"
    assert device.download("LIBS:videocodec/rpivid.codec") == ours

    time.sleep(20)  # PFS3 writes late: a reboot right after a copy can lose it
    device.reboot()
    b = backend(status(device), "rpivid.codec")
    assert int(b["codecs"]) == 1, f"rpivid.codec offers {b['codecs']} codecs"
    r = vctest(device, f"DECODE {material.ram('clip1080.h265')} OUTFORMAT NV12_COL128 "
                       f"REF {material.ram('clip1080.colmd5')}", 300)
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0
