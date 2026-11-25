# rpivid-codec

> **Releases:** this codec driver ships with [Kinora](https://github.com/rondoval/kinora),
> beside the video libraries: the downloadable `.lha`, its Install script and the
> bundled documentation are published there. This repository is source-only and
> versioned via git tags.

H.265 (HEVC) video for AmigaOS on a PiStorm with a Raspberry Pi 4 or CM4.
`rpivid.codec` is a codec driver of `videocodec.library` that drives the
Pi's own HEVC block: the block decodes, the VideoCore scales and converts each
picture to the screen, and the 68k only reads the stream's headers. A program
that plays video through `videocodec.library` plays H.265 too.

> **Experimental.** It has run on real hardware in a scripted test cycle, the
> HEVC conformance set included, but expect rough edges: see
> [Known limitations](#known-limitations).

---

## What you get

- **H.265 Main, 8 bits, up to 4096x2304.** The block decodes about 340 1080p
  pictures a second and about 80 at 4K, so 4K plays full screen at 30 fps.
  The 68k spends about 0.7 ms on a 1080p picture: around 2% of it at 30 fps.
- **H.265 Main10, 10 bits, up to 1080p,** shown at 8 bits: the 68k narrows
  each picture, about 6 ms at 1080p. Showing 10 bits would need a 10-bit
  overlay in the graphics card driver (`VideoCore.card`), which it does not
  have.
- **Damaged streams keep playing.** A lost or broken picture is dropped or
  shown damaged, and the stream is right again from its next key picture; a
  stream cut at any point, or sent in packets of any size, plays.

---

## Requirements

- A **Raspberry Pi 4 or CM4** on a PiStorm. A Pi 3 has no HEVC block.
- **`videocodec.library` from the same Kinora archive**, with what it needs:
  `vchiq.device` from the [Emu68 driver stack](https://github.com/rondoval/emu68-driver-stack),
  an Emu68 newer than 1.1.0-beta.1 with the `cma` overlay, and
  `dtoverlay=videocore,mem=32` in `config.txt` for full screen at 24 or 32
  bits. `vchiq.device` keeps 336 KiB in the `cma` zone, which Emu68 makes in
  whole 2 MiB: `dtoverlay=cma,size=2` is enough, and so is the default of 4.
  The [videocodec-library](https://github.com/rondoval/videocodec-library)
  README has the settings.
- **`gic400.library`**: Emu68 carries it in its ROM, so there is nothing to
  do. The driver stack installs a copy too, for an Emu68 without it.
- **Fast RAM:** a 1080p video takes about 40 MB while it plays, 4K about
  160 MB, 1080p Main10 about 55 MB; they come back when the video closes.

---

## Installation

Kinora's Install script copies `rpivid.codec` to `LIBS:videocodec/` with the
video libraries, whether it installs them with the player or on their own, on
every machine. There is nothing else to set up. `VCInfo` then lists
`rpivid.codec` among the codec drivers and opens a session of its decoder, and
`VCInfo CODECS` shows an H.265 decoder up to 4096x2304.

---

## Playing a video

A player opens a `videocodec.library` decoder session for H.265 and gets its
pictures in the format and at the size it asks for, as for H.264.
[Kinora](https://github.com/rondoval/kinora), the player it comes with, plays
H.265 from MKV, MP4, MOV and MPEG-TS files on a Pi 4 or CM4 through
`rpivid.codec`.

---

## Known limitations

- 10-bit video larger than 1920x1088, and 4:2:2, 4:4:4, 12-bit or
  multi-layer streams, are refused (error `-6`): the block or the 68k cannot
  keep up, or the block cannot do them.
- 10-bit video is shown without tone mapping: HDR material looks washed out.
- There is no software decoder: without the block, or with a refused stream,
  nothing plays. On a Pi 3, which has no HEVC block, `rpivid.codec` is
  installed too but offers no codec (`VCInfo` shows it with none), so an
  H.265 video fails to open (error `-3`: no codec driver decodes it); nothing
  else changes.
- If a video freezes and the block stops answering (error `-9`), press
  Ctrl-Amiga-Amiga: that restarts the Pi and the block with it. `C:Reboot`
  restarts only the 68k.

---

## Troubleshooting

| Symptom | Cause and cure |
|---|---|
| `VCInfo` does not list `rpivid.codec` | It is not in `LIBS:videocodec/`. Run Kinora's Install script again. |
| Error `-3` as an H.265 video opens | No codec driver decodes H.265. On a Pi 3, which has no HEVC block, that is how it is: H.265 needs a Pi 4 or CM4. Elsewhere `rpivid.codec` is missing from `LIBS:videocodec/`. |
| Error `-6` as a video starts | 10-bit past 1080p, or a 4:2:2, 4:4:4 or 12-bit video. Re-encode it on a PC: `ffmpeg -i video.mkv -c:v libx265 -pix_fmt yuv420p -c:a copy video-8bit.mkv` |
| Error `-1` | Not enough Fast RAM for the video's pictures: close other programs. |
| The picture is right but the colours are a little flat | An older `videocodec.library` converts every video as full range: install Kinora's current one. HDR video in 10 bits stays flat (see above). |

---

## For developers

In the source repository, [developer-guide.md](developer-guide.md) says what
a program sees of H.265 through `videocodec.library`, how to build and test
it, and how to work on it; [docs/architecture.md](docs/architecture.md)
explains how it works and why, layer by layer,
[docs/hardware-facts.md](docs/hardware-facts.md) what the hardware was
measured to do. The tests are host checks that need no machine
(`tests/host/run.sh`) and an on-device suite (`tests/`). [AGENTS.md](AGENTS.md) is the working guide to the code.

## License

GPL-2.0 only (see `LICENSE`). The block's command generator is Raspberry Pi's,
from the Linux kernel (`hevc_d_h265.c`, GPL-2.0-or-later), kept as it is; the
register names come from its `hevc_d_hw.h` (GPL-2.0), and the picture record
from Linux's V4L2 controls (GPL-2.0+ with the syscall note, or BSD-3-Clause).
[NOTICE](NOTICE) lists what comes from where.
