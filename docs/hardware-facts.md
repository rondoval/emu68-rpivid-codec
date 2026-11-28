# Hardware facts

What the hardware really does, measured; each fact with where it comes from.
Facts about the VideoCore's ISP also go to videocodec-library's
`docs/firmware-facts.md`, which is where videocore.codec's readers look.

Measured on the CM4 under Emu68 1.1 unless said. Some facts were measured
by the bring-up's own probes and replay tool, no longer in the repository;
the rest by the suite (`tests/`), with VCTest through `videocodec.library`
and the clips of `tests/fixtures/`.

## The block under Emu68

A register probe, run from `RAM:` with no serial log.

* **Reachable as the device tree says.** `/__symbols__` has `hevc_dec`;
  `compatible` is `brcm,bcm2711-hevc-dec`; the two `reg` records come back
  through `DT_GetBaseAddressVirtual()` as `0xF8B00000` (the registers) and
  `0xF8B10000` (the interrupt control block); `DT_GetInterrupt()` gives 130
  (SPI 98). No Emu68 change needed.
* **The clock runs already.** Clock 11 is on at boot, at 500000992 Hz; the
  firmware's maximum (tag `0x30004`) is 500000000. Setting the maximum
  changes nothing. So the firmware does not gate it; the backend still asks
  for it, for the day the firmware does.
* **Version `0x202`, as Linux expects.** Status register (offset 56): `0x1`.
* **Interrupt control as found: `0x88800044`.** With Linux's names:
  `ACTIVE1_EN` (2), `ACTIVE2_EN` (6), `H264_ACTIVE_EN` (23),
  `H264_INTERRUPT_EN` (27), `VP9_INTERRUPT_EN` (31): every enable set, no
  source latched. Nothing on the Amiga side owns interrupt 130 (not enabled
  or pending at the GIC).
* **The test bit (8) raises the line.** With the GIC still masked it goes
  pending there and nowhere else. With a `gic400.library` server
  (`AddIntServerEx(130, 0, level)`), one interrupt is served within 149 us of
  the write (polled at 100 us steps: the latency is less); the server's
  write-back with the test bit off and the source bits written 1 leaves the
  register as found and nothing pending. The server is in assembly and
  answers with the Z flag, as the Emu68 drivers' servers do; `A1 = is_Data`,
  `A6 = SysBase` from gic400's dispatcher.

So interrupts, not polling, for the backend; the block and its clock need no
Emu68 or firmware work.

## The block decoding

The bring-up's replay tool fed the picture records of a tracing FFmpeg
through the command generator and the hardware layer as in `src/`, and
checksummed the block's output on the 68k against FFmpeg's, with one or
three pictures in flight. The suite measured the rest.

* **Bit exact from FFmpeg's own records.** Every picture of every clip
  matches FFmpeg's output in all three planes: 1080p Main (301 pictures,
  plain and pipelined), 720p Main (301), 4K Main (49, pipelined), 1080p
  Main10 (301, pipelined), and 139 of the 141 traced conformance streams
  (the two `SAODBLK` ones differ as through the session, below).
* **The scaling-factor RAM persists.** With the factors written only when
  the SPS enables lists (upstream's condition), a stream without lists
  decoded after one with them comes out with its luma wrong and its chroma
  right, on every picture; 4K and Main10 streams after the 1080p clip (which
  has lists) failed the same way. Writing the factors for every picture's
  first slice, flat 16s when lists are off, makes all of them match. The
  one deviation from upstream in `src/cmdgen/hevc_d_h265.c`; Linux has the
  same exposure between contexts.
* **Block time per picture**, from the queue to the retirement, one in
  flight: 720p 1.7 ms, 1080p 3.2 to 3.3 ms, 1080p Main10 3.5 ms, 4K 12.4
  ms. With three in flight and no checks, 1080p 6.8 ms and 4K 30.7 ms: the
  wait in the queue counts, so these are latencies, not the throughput;
  three deep beats one at a time on the block's rate. The interrupts are two
  per picture, one per phase.
* **The rate through the session,** no hashing, the frames taken as fast as
  they come: 1080p about 344 pictures/s, 4K about 82 (the suite's
  baselines). The block's own time (above) is 3.3 ms and 12.4 ms a picture,
  so the 68k's parsing and the frontend's calls hide behind the block at
  both sizes.
* **The scratch path works.** With the coefficient and prediction-unit
  buffers cut to 64 KiB, 720p pictures exhaust them: five reruns over 20
  pictures, each regrowing the slots and requeuing the picture at the head,
  and every picture still matches.
* **A halved command list does not wedge the block.** With the third
  picture's list cut in two, the block completes both phases, the picture
  comes out with its luma wrong and its chroma right, and the next pictures
  decode normally. So a wedge needs another method: a command list that
  never ends phase 1, or a bitstream window of zero length. Still open.
* **The layout the block writes is the ISP's.** The frame is laid out with
  the luma rows aligned to 16 (1088 for 1080) and the chroma at
  `rows x 128` into each column, as the ISP implies (below); the checksums
  over that layout match FFmpeg's planes, so the block's writing agrees with
  the ISP's reading with no copy between them.

## The ISP and the block's column format

Firmware of 14 May 2025. A converter session (`ril.isp`) fed 1920x1080
charts in the column formats through `VCTest CONVERT`, BGRA32 out, compared
with ffmpeg's conversion of the same charts.

* **8-bit columns (`YUVUV128`, "SAND") are taken as input.** The pictures
  come out right: 50.7 dB against the reference, 195 to 202 pictures/s at
  1080p over two runs (YUYV manages 454 at 720p, for comparison).
* **The format goes as a plain picture: width and rows.** The column stride
  as the width, with `MMAL_ES_FORMAT_FLAG_COL_FMTS_WIDTH_IS_COL_STRIDE` (what
  Linux's bcm2835-codec sends), is refused with `MMAL_EINVAL`. Without the
  flag the ISP implies the layout from the rows.
* **The layout the ISP implies:** luma rows aligned to 16 (1080 reads as
  1088), the interleaved chroma rows right after them inside each column, so
  a column of `16-aligned rows x 3/2` lines: 1632 lines for 1080 rows, 128
  bytes each. `buffer_size_min` after the commit says so (3133440 bytes for
  15 columns). A picture laid out at 8-row alignment (1620 lines) is refused
  at the import as too small.
* **Full range, whatever the content, unless the ISP gets a matrix.** Left
  to itself the ISP makes a limited-range chart (16 to 235) come out with 16
  as RGB 16 and 235 as RGB 236; the full-range chart matches to 50.6 dB. The
  same holds for every other YUV input of the ISP. The converter of
  videocodec-library corrects it with the ISP's custom matrix, applied to the samples
  before that one conversion (its firmware-facts.md, "The ISP's colours"): it
  needs the stream's colour space, which this backend reports in its stream
  information and the frontend's chain passes on.
* **10-bit columns (`Y10C`) are not taken.** The ISP's input lists 48
  encodings and `Y10C` is not one of them; `SAND` is. (`VCInfo COMPONENTS`,
  a firmware survey since removed, showed the input's list as "none given":
  it asked differently.) So Main10
  cannot be shown through the ISP. Linux agrees: its ISP driver carries the
  8-bit column format only, and its 10-bit HEVC pictures go to the display
  hardware or through software.

## The 68k's costs

* **The command generation** costs 50 to 170 us a picture, 0.16 to 0.18 ms
  at 1080p (the parsing not included: the records came from a trace); the
  first call takes 2 ms, Emu68's JIT compiling it.
* **The 68k's time per 1080p picture through the session,** decoded as fast
  as it comes: about 0.7 ms, 623 to 742 us over eleven samples (median
  705), each sample ten decodes in one script, across reboots; Executive's
  used time less the idle machine's 5 to 6 %, the CPU 25 to 29 % busy at
  about 340 pictures/s. The library's calls and the program's own work are
  in it. A sample of one decode reads anywhere from 120 to 700 us: a second
  is too short for Executive's totals.
* **A packet a byte at a time** costs about 190 us a packet: 56 s for the
  720p clip against 25 s.

### The Main10 pass

The pass (now `src/narrow/narrow.S`) timed alone, from `RAM:` at task
priority 6 (above Executive's reach).

* **About 6.1 ms per 1920x1080 picture** (6194 and 6089 us in two runs),
  the mean of 50 passes over the same picture: 4177920 bytes of 10-bit
  columns read, 3133440 bytes of 8-bit columns written, 1.2 GB/s through the
  68k. The pass is ten instructions a word (a load, a three-instruction byte
  swap, three `bfextu` and three byte stores), one unrolled line of 128
  words, the line count its only loop, by truncation. The same pass in plain
  C at `-Os` takes 45597 us. Timed again inside the suite: 6.16 ms on the
  CM4, 6.14 on the PI4, byte for byte its C reference's.
* **Scaled by the pixels:** about 2.7 ms at 1280x720, 25 ms at 3840x2160.
  At 1080p the pass is a sixth of a 25 pictures/s frame period and a third
  at 60; at 4K it is most of one.
* **Not measured:** rounding instead of truncation (a few instructions a
  sample more), and a picture freshly written by the block instead of one
  read 50 times (the picture is four times the Pi's L2, so most of it came
  from memory anyway).

## The session

* **Bit exact through the whole path.** Every picture of the 720p, 1080p
  (plain and one access unit per packet) and 4K Main clips equals FFmpeg's
  in the block's own column layout (`colmd5` references), two sessions at
  once included; the end mark comes after the last picture; a flush at
  picture 100 drops nothing stale.
* **Main10 is bit exact** against the host's truncation of FFmpeg's 10-bit
  output: all 300 pictures of the 1080p Main10 clip, through the 68k's pass
  in the client's task. The session's rate with the pass is 112.3
  pictures/s at 1080p (8.9 ms a picture against 3.5 ms in the block: the
  pass and the frontend's calls are the rest), against 346 for Main. 4K
  Main10 is refused at its first picture with `VCERR_UNSUPPORTED`.
* **The conformance set, 147 streams:** the five the backend refuses by
  design refuse with `VCERR_UNSUPPORTED` and no hang. Of the 142 it decodes,
  ten of them Main10 (narrowed, against the truncation), 139 are bit exact,
  with slices, dependent slices, tiles, wavefronts, entry points, AMP, PCM,
  scaling lists, long-term references, CRA and RASL handling, weighted
  prediction, a conformance window. The three others:
  * `SAODBLK_A` and `_B` (1016 x 760, SAO with deblocking): the luma is
    FFmpeg's in every picture; a few chroma samples a picture are off by
    one (3 to 24 of 193040, scattered, from the second picture on, growing
    through prediction). The generator sends what upstream sends (the
    chroma QP offsets included), so this is the block's own chroma
    filtering; whether Linux shows the same is not known. The test records
    it as a known deviation (`KNOWN` in `test_conformance.py`).
  * `VPSSPSPPS_A` (six SPSs of six sizes, one picture each): the backend
    decodes six pictures through six shape changes; FFmpeg shows two, so
    there is no reference. Skipped with that reason.
* **The chain works.** With `OUTFORMAT RGBA32` the frontend puts the ISP
  converter behind the decoder: all 300 pictures of the 720p clip come out
  as RGBA, the decoder's frames imported into the converter once (the
  converter takes imports only before its first send, which is why the
  backend lists its frames: `VCB_OutputFrame()`, ABI 1.1). BGRA32 comes out
  over 40 dB and RGB16 at 37 dB against the host's conversion. RGB16's
  figure is the ISP's own against a dithered RGB565 reference (the vchiq
  suite's baseline for the ISP's RGB16 is 36 dB), not a fault.
* **Extradata works both ways:** the parameter sets taken out of the
  stream as Annex B, and FFmpeg's own hvcC record with a stream that has
  none (which FFmpeg alone cannot decode), each 300 pictures bit exact.
* **The column frames whole** to a file, unpacked on the host: every plane
  of ten 720p pictures FFmpeg's.
* **A shape change mid-stream works:** `clip720` then `clip1080` end to
  end, `VCEV_StreamInfo` twice, the frames remade once the client had let
  the old ones go, all 600 pictures bit exact.
* **Little memory in the block's reach:** with 64, 16 and 4 MiB left
  (`VCTest HOLD`, the library's pattern), a 720p session either runs or is
  refused with `VCERR_NOMEM`, no session stays open, and a whole clip is
  bit exact once the memory is back. The log shows the block's scratch
  slots refusing to grow ("no memory for a scratch slot") under the hold;
  the session fails cleanly for that.
* **Every damaged stream plays** (`test_damaged`, the fourteen fixtures of
  `break-fixtures.py`, each as a byte stream and one access unit a packet):
  no hang, no session error, the end mark, every picture the damage cannot
  reach bit exact and in display order. What the backend drops, by its
  `dropped` counter:

  | Damage | Frames out of 300 | Dropped |
  |---|---|---|
  | none, trailing junk, units to ignore (an SPS and a slice of another layer among them) | 300 | 0 |
  | a slice cut short, a slice gone, 48 bytes of a slice overwritten, a picture cut to half | 300 | 1 |
  | a reference picture gone | 299 | 2 |
  | a pair swapped | 300 | 2 |
  | an IRAP gone | 299 | 3 |
  | a first slice gone (its picture and the one before) | 298 | 4 |
  | a reference picture twice | 301 | 0 |
  | the stream cut at its start, at its end | 180, 244 | 0, 1 |

* **Packets cut anywhere:** the 720p clip in packets of 1, 2, 3, 5, 188,
  1500 and 65537 bytes, all 300 pictures bit exact. A picture sent in parts
  (`VCPF_FrameStart` on the first, `VCPF_FrameEnd` on the last) makes one
  picture: the 720p clip framed in 1500-byte parts is bit exact.
* **A reset during a decode** (C:Reboot four seconds into a hashed 1080p
  one): the machine comes back and the next decode is bit exact.

## The PI4

The PI4 gives the CM4's results as far as it was run: the conformance set
the same (139 bit exact, the two `SAODBLK` deviations, one stream without a
reference, the five refusals), the session's tests, 342.8 pictures/s at
1080p and 82.4 at 4K, the Main10 pass 6.14 ms a 1080p picture.

## The archive

* **The driver stack's Installer**, which shipped `rpivid.codec` up to 0.5,
  EXPERT mode, videocodec alone ticked, on a
  machine whose `rpivid.codec` was deleted: it copies `rpivid.codec`,
  `gic400.library`, `VCInfo` and `VCGrab` (the rest of videocodec was the
  same build and is left), and after a reboot the backend offers its codec
  and decodes `clip1080` bit exact. Its gadgets have no labels the agent
  reads: each page is told by its checkboxes (21x18) and radio buttons
  (16x9).

## Open

* The two `SAODBLK` streams' chroma: off by one in a few samples a
  picture, the generator upstream's. Whether Linux's decoder shows the
  same would tell block from generator.
* Whether the implied luma alignment is 16 or 32 rows: both give 1088 for
  1080. A height such as 1072 tells (16: 1072, 32: 1088).
* A wedged block: how to wedge it on purpose, and whether a clock cycle
  through the firmware recovers it ("The block decoding" above).
