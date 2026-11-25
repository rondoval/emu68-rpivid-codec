/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bit_writer.h - the host tests' bit writer: u(n), ue(v), se(v) and the
 * RBSP trailing bits into a NAL unit's bytes, with the emulation prevention
 * bytes an encoder inserts (H.265 7.4.2), so that the stream layer reads
 * what it would read from a real stream.  test_bits and test_ps build their
 * units with it.
 */
#ifndef BIT_WRITER_H
#define BIT_WRITER_H

#include <stdio.h>
#include <stdlib.h>

#include "bits.h" /* the stream layer's types */

/* A writer into a caller's buffer */
struct writer
{
	u8 *out;        /* the buffer */
	u32 len;        /* the bytes written so far */
	u32 cap;        /* the buffer's size */
	u32 cache;      /* bits not yet a whole byte, the latest lowest */
	u32 cache_bits; /* how many */
	u32 zeros;      /* the zero bytes just written in a row, for the prevention byte */
};

/**
 * writer_init() - Start a writer over a buffer.
 * @w:   the writer
 * @out: the buffer
 * @cap: its size
 */
static inline void writer_init(struct writer *w, u8 *out, u32 cap)
{
	w->out = out;
	w->len = 0;
	w->cap = cap;
	w->cache = 0;
	w->cache_bits = 0;
	w->zeros = 0;
}

/**
 * writer_byte() - Write a byte, inserting a prevention byte as an encoder does after two zeros.
 * @w: the writer
 * @c: the byte
 *
 * A buffer too small for the unit ends the test: it is the test's own fault.
 */
static inline void writer_byte(struct writer *w, u8 c)
{
	if (w->len + 2 > w->cap)
	{
		fprintf(stderr, "bit_writer: the unit does not fit %u bytes\n", w->cap);
		exit(2);
	}
	if (w->zeros >= 2 && c <= 3)
	{
		w->out[w->len++] = 3;
		w->zeros = 0;
	}
	w->out[w->len++] = c;
	w->zeros = c == 0 ? w->zeros + 1 : 0;
}

/**
 * writer_bits() - u(n): write the low @n bits of @v, most significant first.
 * @w: the writer
 * @v: the value
 * @n: how many bits, at most 32
 */
static inline void writer_bits(struct writer *w, u32 v, u32 n)
{
	while (n--)
	{
		w->cache = (w->cache << 1) | ((v >> n) & 1u);
		if (++w->cache_bits == 8)
		{
			writer_byte(w, (u8)w->cache);
			w->cache = 0;
			w->cache_bits = 0;
		}
	}
}

/**
 * writer_ue() - ue(v): write an unsigned Exp-Golomb code.
 * @w: the writer
 * @v: the value, at most 2^32 - 2
 */
static inline void writer_ue(struct writer *w, u32 v)
{
	u32 x = v + 1, bits = 0;
	while ((x >> bits) > 1)
		bits++;
	writer_bits(w, 0, bits);
	writer_bits(w, x, bits + 1);
}

/**
 * writer_se() - se(v): write a signed Exp-Golomb code.
 * @w: the writer
 * @v: the value
 */
static inline void writer_se(struct writer *w, s32 v)
{
	writer_ue(w, v > 0 ? 2u * (u32)v - 1u : 2u * (u32)(-v));
}

/**
 * writer_trailing() - Write the RBSP trailing bits: a one, then zeros to the byte boundary.
 * @w: the writer
 */
static inline void writer_trailing(struct writer *w)
{
	writer_bits(w, 1, 1);
	while (w->cache_bits != 0)
		writer_bits(w, 0, 1);
}

#endif /* BIT_WRITER_H */
