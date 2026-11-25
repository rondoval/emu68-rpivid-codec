/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hevc_d_video.h - the buffer sizes Linux's driver uses, from its formulas
 * (hevc_d_video.c upstream; the bodies are in cmdgen.c).
 *
 * hevc_d_bit_buf_size() sizes a picture's bitstream buffer from Annex A's
 * least compression: the session makes each picture's at the shape's size
 * (session_objects.c).  hevc_d_round_up_size() takes a size to the next
 * step up: the smallest 3 or 4 times a power of two above it, at least 384,
 * so that a buffer grows in large steps: the session's bitstream buffer for
 * a picture past it
 * (session_parse.c), the scratch slots' coefficient and PU buffers for a
 * shape (hw.c) and one that ran out (hw_task.c).
 *
 * The parts taken from Linux's hevc_dec driver are
 * Copyright (C) 2025 Raspberry Pi Ltd
 * Based on the Cedrus VPU driver, that is:
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 * (NOTICE section 1.)
 */
#ifndef HEVC_D_VIDEO_H
#define HEVC_D_VIDEO_H

#include <stddef.h> /* size_t */

size_t hevc_d_round_up_size(size_t x);
size_t hevc_d_bit_buf_size(unsigned int w, unsigned int h, unsigned int bits_minus8);

#endif /* HEVC_D_VIDEO_H */
