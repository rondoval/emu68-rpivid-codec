# emu68-rpivid-codec: developer guide

For programmers playing H.265 through `videocodec.library`, and for whoever
works on the backend itself. Installing it is in [README.md](README.md); how
it works and why, and the rules each layer keeps, in
[docs/architecture.md](docs/architecture.md); what the hardware was
measured to do in [docs/hardware-facts.md](docs/hardware-facts.md).

Section 1 is the programming interface: what a program sees of H.265.
Sections 2 to 5 are the library's tools, building, the tests and changing the code;
section 6 is troubleshooting.

---

## 1. H.265 through videocodec.library

A program never opens `rpivid.codec`: it opens a decoder session on
`videocodec.library` for `VCC_H265`, and the library picks the backend that
offers it. Everything the vchiq library's developer guide says of sessions,
packets, frames and pictures into a screen bitmap (its sections 2.1 and 2.2)
holds for H.265 as for H.264; this section gives what differs.

### 1.1 Opening

```c
s = VC_OpenSessionTags(VCA_Codec, VCC_H265, VCA_Kind, VCK_Decoder,
                       VCA_OutputFormat, VCF_BGRA32, VCA_OutputWidth, 1920, VCA_OutputHeight, 1080,
                       VCA_Extradata, hvcc, VCA_ExtradataSize, hvcc_size,
                       VCA_Framed, TRUE, TAG_DONE);
```

* **The output format and size are the program's choice**, as for H.264:
  the block writes its own column layout (`VCF_NV12_COL128`), and the
  library puts the VideoCore's converter behind it, which scales and
  converts each picture to the format, size and place asked for, reading the
  block's memory directly. A program that wants the column frames
  themselves asks for `VCF_NV12_COL128` and gets no converter.
* **`VCA_CodecTag`** may carry the container's FourCC (`hvc1`, `hev1`;
  `VC_CodecOfTag()` maps it to `VCC_H265`): `rpivid.codec`'s row serves
  every form of H.265, so it changes nothing here.
* **On a machine without the block** (a Pi 3) `rpivid.codec` offers no
  codec, and the open fails with `VCERR_NOBACKEND` (-3). A player can then
  fall back to a decoder of its own before showing anything.

### 1.2 What the decoder takes

| Stream | Taken |
|---|---|
| Main (8 bits, 4:2:0) | 16 to 4096 pixels either way; the codec row says 4096x2304, the largest 16:9 |
| Main10 (10 bits, 4:2:0) | up to 1920x1088; shown at 8 bits: the 68k keeps the top eight bits of every sample, about 6 ms a 1080p picture |
| Main10 past 1920x1088, 4:2:2, 4:4:4, 12 bits, luma and chroma depths that differ | refused, `VCERR_UNSUPPORTED` (-6) |
| multilayer, 3D or screen-content extensions; range extension tools switched on | refused, `VCERR_UNSUPPORTED`; units of a layer other than the base one are skipped, so the base layer of a layered stream plays |
| a stream that breaks the standard's limits (Linux's SPS and PPS checks) | refused, `VCERR_STREAM` (-8) |

`MaxLevel` is 51 (4K at 60); the level a stream declares plays no part.

**The parameter sets** (VPS, SPS, PPS) come in the stream or beside it:
`VCA_Extradata` takes them in Annex B form or as an `hvcC` record, as an MP4
or Matroska file holds it. Pass them where you have them: the open then
refuses what the block cannot do, so a player can fall back before showing
anything, and the frames are made at once. Without them the refusal comes as
`VCEV_Error` once the stream's SPS arrives.

### 1.3 Packets

* **Annex B only**, with start codes, whatever the extradata's form: a
  demuxer that reads length-prefixed samples (MP4, Matroska) replaces each
  length with a start code. A packet with no NAL unit end within 16 MiB
  fails the session with `VCERR_STREAM`.
* **Any size and any cut** without `VCA_Framed`: the backend finds the
  pictures itself. A picture completes at the first unit of the next one (a
  parameter set, a delimiter, a prefix SEI or its first slice), at an
  end-of-sequence unit, or at the end mark.
* **`VCA_Framed`** packets hold whole access units, each with its `Pts`;
  a picture larger than the program's packets goes as several, the first
  flagged `VCPF_FrameStart` alone and the last `VCPF_FrameEnd`.
* **`VCPF_Corrupt`** marks bytes known damaged (a lost chunk): the picture
  they belong to is dropped, not decoded from bad data.
* **`VCPF_EndOfStream`** on the last packet: every picture comes out, then
  `VCEV_EndOfStream`.

### 1.4 Pictures out

* **In display order**, each with the `Pts` of the packet that held its
  first slice; `VCFF_KeyFrame` on an IRAP picture.
* **`VCEV_StreamInfo`** comes before the first picture of a shape, and again
  when the shape changes mid-stream (a new SPS of another size): the
  library opens the converter again for it.
* **A picture the backend cannot decode is dropped**: a slice header it
  cannot read or that breaks the standard's ranges, slices of one picture
  that disagree, a `VCPF_Corrupt` packet, the block failing it. Nothing
  tells the program but the gap; the pictures that depend on it may show
  damage, and everything is right again from the next IRAP picture.
* **A missing reference is stood in for**, not dropped: a picture whose
  reference is gone (a lost picture, a seek into an open GOP) decodes
  against what its own frame held and is shown, damaged, until the next
  IRAP picture.
* **Colour:** the stream information names the colour space (`VCCS_*`): the
  matrix and range of the stream's video signal description, and where it has
  none the usual guess, BT.709 from 720 rows up and BT.601 below, in limited
  range. The frontend's chain gives it to the converter
  behind the decoder, which makes RGB accordingly; a client's `VCA_ColorSpace`
  overrules it.

### 1.5 Seeking

`VC_Flush()` drops what has not reached the block, waits for what has, and
empties the reference pictures without output; the program's frames stay
with it. Send from an IRAP picture after it: the backend expects one, and the RASL
pictures that lean on what came before it are skipped as the standard
says.

### 1.6 Memory and speed

A session's pictures live in Fast RAM within the VideoCore's reach (the
lowest GiB): about 40 MB at 1080p, 160 MB at 4K, 55 MB for 1080p Main10,
taken at the open (with extradata) or at the first SPS, and given back at
the close. `VCERR_NOMEM` (-1) when there is not that much.

The block decodes about 340 1080p pictures a second, about 80 at 4K and 112
at 1080p Main10 (the 68k's pass included). The 68k spends about 0.7 ms on a
1080p Main picture, the library's calls, the header parsing and the block's
command list together, in the program's own calls: the backend has one task
of its own, which only feeds the block. Two sessions share the block.

---

## 2. Tools

### 2.1 VCInfo and VCTest

`VCInfo` lists `rpivid.codec` among the codec drivers with its counters, and
`VCInfo CODECS` its row. The counters, also in `VCTest STATUS`:

| Counter | Counts |
|---|---|
| `pictures` | pictures queued for the block |
| `dropped` | pictures not decoded (section 1.4) |
| `narrowed` | Main10 pictures narrowed to 8 bits by the 68k |
| `reruns` | phase 1 run again after a scratch buffer grew |
| `interrupts` | the block's interrupts |
| `wedged` | times the block stopped answering (section 6) |

`Interface` reads `HEVC 0x202` (the block's version) while the hardware is
up: from the first session until `videocodec.library`'s last close.

`VCTest DECODE` (the vchiq library's developer guide, 6.3) takes H.265 by
the file's extension (`.h265`, `.hevc`, `.265`) or `ENCODING H265`.
`OUTFORMAT NV12_COL128` takes the block's column frames as they are and
hashes their visible window (section 4.1); `REF file` holds the hashes to a
reference made by `tests/host/colmd5.py` and names the first picture that
differs, `NOHASH` decodes without hashing (for the rate), `HASHES` prints
every frame's hash, `OUT file` writes the frames to a file, `FRAMED` sends
one access unit a packet, `EXTRADATA` moves the parameter sets out of the
stream into `VCA_Extradata`, `CONFIG file` passes a file as
`VCA_Extradata` (an `hvcC` record), `PACKET n` cuts the input into packets
of n bytes.

---

## 3. Building

From the stack's root, through its container only:

```sh
./scripts/docker-build.sh --target emu68-rpivid-codec
```

with the build directory's own configuration (`EMU68_CONFIGURE_ARGS`, or
the script configures with its defaults), or `./build.sh --build` for the
whole stack. The module goes to `install/LIBS/videocodec/rpivid.codec`; the
`package` target puts it in the stack's archive. Zero warnings. The version
is the one in `CMakeLists.txt`'s `project()`.

---

## 4. Tests

### 4.1 The references

Every picture is held to FFmpeg's software decoder, bit for bit. `VCTest
DECODE ... OUTFORMAT NV12_COL128 REF file` hashes each frame the session
gives out, the visible window only, column by column, luma lines then chroma
lines, so the padding the block writes does not count. `tests/host/colmd5.py`
places FFmpeg's pictures in the column layout and hashes them the same way;
a Main10 stream's pictures are first truncated to 8 bits as the backend's
pass narrows them (`tests/host/narrow_ref.py`).

A difference is a failure: there is no tolerance on decoded pictures. The
only tolerance is on pictures through the ISP's colour conversion, held to
the host's conversion of the same pictures: over 40 dB, 34 dB for RGB16,
whose reference FFmpeg dithers.

### 4.2 Fixtures

`tests/fixtures/`, never in git:

1. **The clips:** `tests/host/make-fixtures.sh` makes them with the host's
   x265 from the vchiq suite's sample video, each with its `.framemd5` and
   `.colmd5` from the host's FFmpeg: Main at 720p, 1080p and 4K, Main10 at
   1080p and 4K (the refusal past 1080p), a shape change, the extradata
   pair, the colour bars, and the players' MP4s, encoded straight into the
   container. Its head lists each; `MANIFEST` records what it made, with
   sizes and SHA-256s.
2. **The damaged streams** (`broken/`, from the same script through
   `break-fixtures.py`) come from a 720p clip of four slices a picture with
   the parameter sets before every IRAP and open GOPs: a picture, an IRAP, a
   slice or a first slice gone, a picture twice, a pair swapped, a picture or
   a slice cut short, 48 bytes of a slice overwritten, junk after a picture,
   units a Main decoder ignores, the stream cut at its start and at its end.
   `broken.py` gives each picture's place in display order and the pictures
   each damage can reach; `break-fixtures.py --check` holds the host FFmpeg's
   decode of every fixture to the same check. Packets cut at any size need
   no fixture (`VCTest PACKET`).
3. **The conformance set:** `tests/host/fetch-conformance.sh` fetches the
   147 JCT-VC HEVC v1 streams from ITU's site, each held to its SHA-256 in
   `tests/conformance/LIST`. `LIST` gives each stream's size, format,
   picture count, what the backend is to do with it (`ok`, or `refuse` by
   [docs/architecture.md](docs/architecture.md) section 11), its source and
   its hash.

The conformance references are checked in, beside `LIST`: FFmpeg 5.1's
pictures, which the hardware decodes bit exact. A later FFmpeg does not
always agree (8.0 loses reference pictures of `NUT_A_ericsson_5` and
`RPS_D_ericsson_6`), so `make-conformance-refs.sh` remakes only the streams
it is given, and a new reference is held to the hardware's decode before it
is kept. Five streams refuse by design: `PICSIZE_A` to `_D` (pictures over
4096 or under 16 pixels) and `TSUNEQBD_A_MAIN10` (unequal bit depths); v1
has no Main10 past 1080p, which is why the 4K Main10 clip exists.
`VPSSPSPPS_A_MainConcept_1` (six pictures of six sizes) is decoded whole by
no FFmpeg, so it is checked for parsing and output order only.

### 4.3 On the host

```sh
make -C tests/host check     # the unit tests, the clips through the stream layer
tests/host/run.sh            # the above and every conformance stream
```

No machine needed: the stream layer is plain C and builds on the host. The
unit tests (`test_bits`, `test_nal`, `test_ps`, `test_rps`, `test_dpb`,
`test_record`) work on hand-made bytes and values, each value the standard
bounds set one past its bound among them; each program's head says what it
checks. `stream_check` runs a stream through the stream layer as a session
does and checks the output order and the picture count against FFmpeg's,
or, with `--expect-refusal`, that a stream the backend refuses by design is
refused at its parameter set; `--verbose` shows each picture's type, POC and
reference pictures. Beside the clips and the conformance set it takes
streams made to test one rule (`no_irap_start.py`, `mixed_slices.py`).

### 4.4 On the machine

```sh
tests/run.sh --server=amiga-cm4 -m smoke    # after a change, about four minutes
tests/run.sh --server=amiga-cm4             # before a release
```

The suite runs on the stack's test harness, taken from the install tree
(emu68-common's `emu68test` and the vchiq library's `vchiqtest`: build the
stack first), so the vchiq suite's options apply: `--server`,
`--install-dir`, `--no-install`, `--no-reboot`, `--serial`,
`--update-baselines` and the rest. It installs the stack's resident files
and this module from the install tree first and reboots; `RAM:hw` is its
scratch. Its own options: `--kinora` (the Kinora program on the Amiga for
`test_players.py`'s Kinora tests, by default where its Installer puts it; they
skip without one) and `--archive`
(the stack's `.lha` for `test_install`, by default the newest the `package`
target made; it must be of the installed build). Each run leaves
`report.md`, `measured.json`, the debug log and every test's output under
`tests/runs/`.

| Module | Tests |
|---|---|
| `test_h265.py` | the decoder through VCTest: bit exact, extradata, the column frames whole, the chain and two screen formats, two sessions, flush, the end mark, imports, the backend's refusals, the rate, the 68k's cost, a reset |
| `test_robust.py` | packets cut anywhere, the damaged streams, shape changes, low memory, bad parameter sets |
| `test_conformance.py` | the conformance set (`slow`): every stream bit exact or refused as `LIST` says; the two `SAODBLK` streams `xfail` ([docs/hardware-facts.md](docs/hardware-facts.md)) |
| `test_main10.py` | Main10 bit exact against the truncation, its counter, its rate, the refusal past 1080p |
| `test_players.py` | Kinora playing 1080p and 4K in its window at the film's rate; VCPlay; `test_install`, the stack's archive through its Installer (`reboot`) |

Each test's docstring says what it checks. The markers: `smoke` (one of
every path), `slow`, `reboot`, `hands` (an external reset, only with
`--hands`). Every test runs on either machine; the PI4 runs the stock
Kickstart and the stack's `gic400.library` from disk. `tests/run.sh gates`
rewrites `tests/TRACEABILITY.md`: the backend's entry points, events and
errors, each with the tests that cover it (their `covers` marks), or a
known gap with its reason.

A new test goes in the module of its kind, with a docstring that says what
it checks, and a `covers` mark when it reaches an entry point, event or
error that `tests/TRACEABILITY.md` lists.

### 4.5 Baselines

`tests/baselines.json`, per machine: a run checks them, `--update-baselines`
writes them, and the diff is read before a commit.

| Key | Means | Checked |
|---|---|---|
| `h265.1080.fps`, `h265.4k.fps`, `h265.1080_10.fps` | pictures a second, `VCTest DECODE` with no reference | not lower than 10 % |
| `h265.1080.cpu_us_per_picture` | the 68k's time a 1080p picture costs: Executive's used time over ten decodes, less the idle machine's | not higher than 20 %; background load moves it, so a failure is rerun alone before a change is blamed |

### 4.6 When what runs

| When | Runs |
|---|---|
| a change | `tests/run.sh gates` (kernel-doc, complexity, traceability: seconds, no machine) and `tests/host/run.sh`, then the smoke set on the CM4 |
| a release | the same, then the whole suite with `slow` and `reboot` on the CM4 (and the PI4), under an hour, the conformance set most of it, the Kinora tests with Kinora installed; `test_install` after the `package` target, from that archive |

### 4.7 What is not tested

* Picture quality beyond bit exactness: there is none to judge, the block's
  output is the standard's.
* The ISP's conversion itself: the vchiq library's suite covers it.
* Field-coded streams as fields: decoded as frames, checked bit exact as
  frames; nothing weaves them.
* Real-time playback of 4K Main10: refused, so never attempted.
* `VCERR_TRANSPORT`, a block that stops answering: only fault injection in
  the driver could make one, and there is none. It is the one known gap in
  `tests/TRACEABILITY.md`.

---

## 5. Changing the code

[AGENTS.md](AGENTS.md) has the layers and what each may include. In short:
`src/stream/` reads the stream (and builds on the host), `src/cmdgen/` turns
a picture's record into the block's command list, `src/hw/` drives the
block, `src/narrow/` is the Main10 pass, and `src/session*.c` and
`src/rpivid_*.c` tie them to the backend interface.

### 5.1 The command generator from Linux

`src/cmdgen/hevc_d_h265.c` is Raspberry Pi's, from Linux, kept as it is so
that it compares with the original: the V4L2 framework's functions are
replaced, and every other change is marked `rpivid:` with its reason.
`src/cmdgen/UPSTREAM` names the commit and each upstream file's blob. To
see the difference, take the file at that commit from a checkout of
Raspberry Pi's Linux (`git show <commit>:<dir>/hevc_d_h265.c`), check that
`git hash-object` gives the blob recorded, and diff it against the kept
file. To take a newer upstream: apply that difference to the newer file,
carry over what changed in the other files `UPSTREAM` lists (their blobs
tell), put the new commit and blobs in `src/cmdgen/UPSTREAM`, and run the
host checks and the whole suite on the machine, `test_conformance.py` with
it.

### 5.2 The debug log

On a build with the debug backend (the stack's `debug` tier), the module
writes `[rpivid]` lines to the Emu68 debug log: a session that failed and
why, every dropped picture or slice and why, the block's version, a wedge.

### 5.3 Licence

GPL-2.0 only: the command generator and the register map are Raspberry
Pi's, from Linux. Code of the stack's BSD components may be used here;
nothing of here goes back into them. [NOTICE](NOTICE) lists what comes from
where.

---

## 6. Troubleshooting

| Symptom | Cause and cure |
|---|---|
| `VC_OpenSession()` answers `VCERR_NOBACKEND` (-3) for H.265 | `rpivid.codec` is not in `LIBS:videocodec/`, or the machine has no HEVC block (a Pi 3): `VCInfo` shows it with no codec. |
| The open or the first SPS answers `VCERR_UNSUPPORTED` (-6) | A stream the block or the backend cannot take (section 1.2). The debug log says which. |
| `VCERR_TRANSPORT` (-9) at the open | The block did not answer; the debug log says how far the open got. "the block is another driver's" in the log: another program holds it. |
| `VCERR_TRANSPORT` while playing, `wedged` counted | The block stopped answering. Every session ends; Ctrl-Amiga-Amiga restarts the Pi with it (`C:Reboot` restarts only the 68k). |
| Pictures missing, `dropped` counting | A damaged stream: the log names each picture and why. Right again from the next IRAP picture. |
| Wrong pictures, nothing dropped | Hold the stream to FFmpeg's decode: `tests/host/colmd5.py` makes the reference, `VCTest DECODE file OUTFORMAT NV12_COL128 REF reference` names the first picture that differs, and `tests/host/out/stream_check --verbose` shows how the stream layer reads the stream up to it. |
| Pictures shown at the wrong times | The backend passes the times through: a frame's `Pts` is that of the packet that held its first slice, and the frames come out in display order. Check the times the demuxer gives the packets. |
