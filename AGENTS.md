# rpivid-codec

An H.265 backend of `videocodec.library` on the Pi 4's HEVC block, built and
shipped by Kinora (its submodule `external/rpivid-codec`). Kinora's `AGENTS.md`
rules apply (container builds only, zero warnings, the user owns git), and the
Emu68 drivers' habits: `__NOLIBBASE__` with a local `SysBase` in freestanding
code, C99 declarations at first use.

| Binary | Source | Installed in | What it is |
|---|---|---|---|
| `rpivid.codec` | `src/` | `LIBS:videocodec/` (Kinora's Install script, with the video libraries) | the backend: stream layer, command generator, hardware layer, session layer |

## Layers

| Directory | Layer | May include |
|---|---|---|
| `src/stream/` | NAL scanner, bit reader, parameter sets, slice headers, RPS, DPB, the picture record | C and its own headers only: it builds on the host (`tests/host/`) |
| `src/cmdgen/` | the command generator: `hevc_d_h265.c` kept from Linux, `cmdgen.c` the plumbing, `linux_compat.h` the kernel's names | emu68-common, its own headers |
| `src/hw/` | the block: claim, clock, interrupt server, decode task | emu68-common, gic400 and mailbox (their headers from the toolchain image) |
| `src/narrow/` | the Main10 pass, `narrow_picture()` | nothing |
| `src/session*.c`, `src/rpivid_*.c` | sessions, packets, frames, the backend ABI | all of the above |

## Docs

Each document has one reader; put new text where its reader looks.

| File | Reader | Holds |
|---|---|---|
| `docs/architecture.md` | whoever changes the design or the code | how it works and why: the components, tasks and data paths, then each layer's design and the rules it keeps; licensing (section 14) |
| `docs/hardware-facts.md` | whoever touches the block | what the block, the ISP and the 68k were measured to do. New measurements go here, without dates or local paths |
| `developer-guide.md` | programmers and contributors | what a program sees of H.265, building, the tests (their references, fixtures, baselines, when what runs), changing the code |
| `README.md`, `RELEASE-NOTES.md` | end users | installing and using it; the current release only |
| `NOTICE` | whoever redistributes it | what comes from where, under which licence; shipped in the archive |
| this file | agents | what must not break |

## Build and validate

From Kinora's root, `./build.sh` (or `scripts/docker-build.sh`): it builds
emu68-common, then videocodec-library, then this repository
(`build/rpivid-codec`) into `build/video-prefix`, where it finds `VideoCodec
0.7` and `Emu68Common 2.3`; then Kinora. Debug knobs go in
`KINORA_CONFIGURE_ARGS`. Zero warnings.

From the component root:

- **Host, no machine:** `make -C tests/host check` (the unit tests, the
  clips through the stream layer), then `tests/host/run.sh` (also every
  conformance stream). Both need the fixtures (`tests/fixtures/`, never in
  git): `tests/host/make-fixtures.sh` and `fetch-conformance.sh` make them,
  as the developer guide's section 4.2 says.
- **The gates:** `tests/run.sh gates` (no machine, seconds) after any change
  to C or to the tests:
  - kernel-doc on every function of the backend's own C (Linux's kept
    `src/cmdgen/hevc_d*` left out);
  - cyclomatic complexity at most 15 (`tests/complexity-allow.txt` is empty
    and stays so if it can: an entry is 20 at most, with a reason);
  - traceability: `tests/TRACEABILITY.md`, rewritten each run, lists the
    backend's entry points, events and errors with the tests that cover
    them, or as known gaps in `tests/gates/trace_items.py`.
- **On the hardware:** `tests/run.sh --server=amiga-cm4` (emu68-common's
  harness and videocodec-library's `videocodectest`, both from Kinora's build:
  `build/emu68-prefix/share/emu68-testing` and
  `build/video-prefix/share/videocodec-testing`, never from a sibling's
  source), `-m smoke` after a change, the whole suite before a release. The
  machines are shared: the user runs these or says when to, and another
  session may hold a machine or the build tree. The suite installs the video
  libraries' resident files from Kinora's `install/` and reboots; report what
  it replaced and whether `baselines.json` changed.

## Rules

1. **GPL-2.0 only.** Code flows one way: BSD code of emu68-common and
   videocodec-library may be used here, nothing of here goes back, only
   hardware facts into videocodec-library's documents.
2. **The kept file stays verbatim.** `src/cmdgen/hevc_d_h265.c` is Linux's
   with the V4L2 framework's functions replaced; every other change is marked
   `rpivid:` and explained (one today: the scaling factors written for every
   picture's first slice). `cmdgen.c` keeps a few more of upstream's blocks,
   each marked as upstream's. `src/cmdgen/UPSTREAM` names the originals, by
   commit and blob.
3. **`src/stream/` is never on the m68k include path:** its `bits.h` would
   shadow emu68-common's. The module includes `stream/x.h` from `src/`.
4. **The backend ABI is videocodec-library's.** `rpivid.codec` implements
   `videocodec_backend.sfd` as the library's headers give it (ABI 1.1,
   `VCB_OutputFrame()`); a change there is the library's first.
5. **What the block or the ISP reads stays within their reach:** buffers from
   the module's pool, 64-byte aligned, below `RP_ISP_REACH_END`; imported
   frames are checked against it.
6. **The conformance references stay FFmpeg 5.1's** (`tests/conformance/`,
   checked in), which the hardware matches bit exact. A later FFmpeg does not
   always agree (8.0 loses pictures of `NUT_A` and `RPS_D`): never remake one
   unasked; `make-conformance-refs.sh` remakes only the streams it is given,
   and a new reference is held to the hardware's decode before it is kept.
7. **One version for the component**, `project(... VERSION ...)` in
   `CMakeLists.txt`; the binaries take it.
8. **Functions carry kernel-doc comments** (summary, every parameter,
   `Return:`). Documents stay ASCII.

## Traps

- On the host, `LONG` and `ULONG` stay 32 bits: the V4L2 structures' `__s32`
  and `__u32` are built on them.
- A long suite run's serial capture can stop partway: for the backend's own
  messages about one stream, rerun it with a capture of your own.
- Kinora's `build.sh` stamps a dev build, and the tier is what
  `KINORA_CONFIGURE_ARGS` says; say which tier a measurement came from.
- `test_install` holds Kinora's archive to the install tree: an
  `--install-dir` trimmed to `rpivid.codec` and the rigs fails it with
  `FileNotFoundError`. For the whole suite give Kinora's full install tree, or
  deselect it.
- `test_cpu` reads Executive's totals, which every other task on the machine
  moves: on a failure, rerun it alone on a quiet machine before a change is
  blamed.
