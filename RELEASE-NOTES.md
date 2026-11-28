# Release notes - rpivid-codec 0.6

- **It comes with Kinora.** `rpivid.codec` now ships in
  [Kinora](https://github.com/rondoval/kinora)'s archive, beside the video
  libraries ([videocodec-library](https://github.com/rondoval/videocodec-library)),
  and Kinora's Install script copies it to `LIBS:videocodec/` with them, with
  the player or without it, on every machine. On a Pi 3, which has no HEVC
  block, it offers no codec and an H.265 video fails to open; nothing else
  changes. It needs `videocodec.library` 0.7.
- Decoding is unchanged.
- **For developers:** the repository is `rpivid-codec` now (it was
  `emu68-rpivid-codec`), and builds inside Kinora.

Version 0.5 shipped in the Emu68 driver stack, as `emu68-rpivid-codec`.

# Release notes - emu68-rpivid-codec 0.5

First release: 0.5. H.265 (HEVC) video on the Raspberry Pi 4's own HEVC block,
for AmigaOS on a PiStorm with a Pi 4 or CM4.

**Experimental.** The stack's Install script copies `rpivid.codec` with its
`videocodec` part, which it offers unticked. A Pi 3 has no HEVC block: there an
H.265 video fails to open.

- **H.265 through `videocodec.library`.** `rpivid.codec` is a second codec
  driver beside `videocore.codec`: a program that plays video through the
  library plays H.265 too, in the format and at the size it asks for, the
  VideoCore converting and scaling each picture.
- **8-bit H.265 up to 4096x2304, 4K full screen at 30 fps.** The block decodes
  about 340 1080p pictures a second; the 68k reads only the headers, about
  0.7 ms a 1080p picture.
- **10-bit H.265 up to 1080p**, shown at 8 bits. Showing 10 bits would need
  the graphics card driver (`VideoCore.card` under Picasso96) to put the
  pictures on screen as a 10-bit overlay, which it does not do; so the 68k
  narrows each picture to 8 bits, about 6 ms at 1080p. That is why larger
  10-bit video is refused; so are 4:2:2, 4:4:4 and 12-bit video.
- **True colours.** The pictures are converted to the screen's colours by what
  the stream says it holds (BT.709 or BT.601, limited or full range), and by
  the usual rule where it says nothing: BT.709 from 720 rows up, BT.601 below.
  This needs emu68-vchiq-library 0.6.
- **Damaged and odd streams keep playing:** a lost or broken picture costs the
  pictures up to the next key picture at most, and packets of any size work.
  Checked against the HEVC conformance set.
