/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bits.c - the bit reader over a NAL unit (bits.h).
 *
 * The reader works on the NAL unit as it arrived, not on a cleaned copy.  An
 * encoder puts an emulation prevention byte (0x03) after every two zero
 * bytes that a byte of 0x03 or less would follow, so that no start code can
 * appear inside a unit (H.265 7.4.2).  take_byte() drops each one as it
 * meets it: the syntax readers above it see the RBSP, the payload without
 * those bytes, while the reader's byte position still counts them, as the
 * HEVC block wants the slice data's offset.
 *
 * The bits wait in a 32-bit cache, the next unread one at the top (bit 31).
 * fill() adds whole bytes below the cached bits until a read has the bits it
 * asks for.  It adds a byte only while fewer than FILL_BITS_MAX bits are
 * cached, so a byte always has room, and a read of more bits goes in chunks
 * of at most FILL_BITS_MAX.  As fill() takes no more bytes than the read
 * needs, the cache holds fewer than 8 bits after every read: what it holds
 * is the rest of the byte read last, never a whole unread byte.
 * rp_bits_byte_align() and rp_bits_byte_offset() rely on that.
 *
 * Errors: a read past the end of the unit gets zero bits and sets the
 * overrun flag, which stays set.  The parsers read a whole header without
 * testing and look at the flag once, at its end.
 */
#include "bits.h"

#define PREVENTION_BYTE  0x03u /* emulation_prevention_three_byte (7.4.2) */
#define PREVENTION_ZEROS 2u    /* the zero bytes in a row that come before one */
#define CACHE_BITS       32u   /* the cache's size: one u32 */
#define FILL_BITS_MAX    (CACHE_BITS - RP_BITS_PER_BYTE) /* the most bits fill() promises: room for one more byte is left */
#define UE_ZEROS_MAX     31u   /* the leading zeros of the largest ue(v) the standard allows, 2^32 - 2 */

/**
 * take_byte() - The next byte of the RBSP, emulation prevention bytes dropped.
 * @b: the reader
 *
 * Counts the zero bytes it returns in a row.  A PREVENTION_BYTE after
 * PREVENTION_ZEROS of them was put there by the encoder and is no data: it
 * is passed over and the count starts again from the byte after it, so
 * 00 00 03 00 00 03 01 holds two prevention bytes and reads as 00 00 00 00 01.
 * Reaching the unit's end is no error; reading past it is.
 *
 * Return: the byte; 0 past the end of the unit, with the overrun flag set.
 */
static u32 take_byte(struct rp_bits *b)
{
	for (;;)
	{
		if (b->next >= b->len)
		{
			b->overrun = TRUE;
			return 0;
		}
		u32 byte = b->data[b->next++];
		if (byte == PREVENTION_BYTE && b->zeros >= PREVENTION_ZEROS)
		{
			b->zeros = 0;
			continue;
		}
		b->zeros = byte == 0 ? b->zeros + 1 : 0;
		return byte;
	}
}

/**
 * fill() - Make the cache hold at least @n bits.
 * @b: the reader
 * @n: the bits a read asks for, at most FILL_BITS_MAX
 *
 * Each byte goes in just below the bits already cached.  A byte is added only
 * while fewer than @n bits are cached, so the cache ends with fewer than
 * @n + 8: no byte is taken before a read needs it.  Past the end of the unit
 * the bytes are zeros (take_byte()).
 */
static void fill(struct rp_bits *b, u32 n)
{
	while (b->cache_bits < n)
	{
		b->cache |= take_byte(b) << (CACHE_BITS - RP_BITS_PER_BYTE - b->cache_bits);
		b->cache_bits += RP_BITS_PER_BYTE;
	}
}

/**
 * rp_bits_init() - Start a reader over a NAL unit.
 * @b:    the reader
 * @data: the unit's bytes as they arrived: the two-byte header first, the
 *        emulation prevention bytes still in
 * @len:  the unit's length in bytes
 * @skip: the bytes to pass over before the first read; the parsers pass
 *        NAL_HEADER_BYTES, to start at the payload
 *
 * The skipped bytes are not looked at, so they don't count towards a
 * prevention byte; the header could not start a zero run anyway, as its last
 * byte holds nuh_temporal_id_plus1, which is never 0.  A @skip past the end
 * leaves nothing to read and sets the overrun flag at once.
 */
void rp_bits_init(struct rp_bits *b, const u8 *data, u32 len, u32 skip)
{
	b->data = data;
	b->len = len;
	b->next = skip < len ? skip : len;
	b->zeros = 0;
	b->cache = 0;
	b->cache_bits = 0;
	b->overrun = skip > len;
}

/**
 * rp_bits_u() - u(n) (7.2): the next @n bits as an unsigned number.
 * @b: the reader
 * @n: the bits, at most 32
 *
 * The first bit read is the most significant.  The bits are taken in chunks
 * of at most FILL_BITS_MAX, as fill() promises no more at once; u(0) reads
 * nothing and is 0.
 *
 * Return: the value; past the end of the unit, the missing bits read as zeros.
 */
u32 rp_bits_u(struct rp_bits *b, u32 n)
{
	u32 value = 0;
	while (n > 0)
	{
		u32 take = n > FILL_BITS_MAX ? FILL_BITS_MAX : n;
		fill(b, take);
		value = (value << take) | (b->cache >> (CACHE_BITS - take));
		b->cache <<= take;
		b->cache_bits -= take;
		n -= take;
	}
	return value;
}

/**
 * rp_bits_flag() - u(1): a one-bit flag.
 * @b: the reader
 *
 * Return: TRUE for a 1 bit; FALSE for a 0 bit, or past the end of the unit.
 */
BOOL rp_bits_flag(struct rp_bits *b)
{
	return rp_bits_u(b, 1) != 0;
}

/**
 * rp_bits_ue() - ue(v) (9.2): an unsigned Exp-Golomb code.
 * @b: the reader
 *
 * The code is some zero bits (leadingZeroBits), a one, and as many bits again;
 * its value is 2^leadingZeroBits - 1 plus those last bits, so 1 is 0, 010 is
 * 1, 011 is 2 and 00100 is 3.  The largest value the standard allows,
 * 2^32 - 2, has UE_ZEROS_MAX zeros: a longer run is damage, and so is the
 * unit ending among the zeros.  Either sets the overrun flag.
 *
 * Return: the value; 0 for a damaged code.
 */
u32 rp_bits_ue(struct rp_bits *b)
{
	u32 leading_zeros = 0;
	while (!rp_bits_flag(b))
	{
		if (++leading_zeros > UE_ZEROS_MAX || b->overrun)
		{
			b->overrun = TRUE;
			return 0;
		}
	}
	return ((1u << leading_zeros) - 1u) + rp_bits_u(b, leading_zeros);
}

/**
 * rp_bits_se() - se(v) (9.2.2): a signed Exp-Golomb code.
 * @b: the reader
 *
 * An unsigned code k, mapped as 9.2.2 gives it: (-1)^(k+1) * ceil(k / 2), so
 * k = 0, 1, 2, 3, 4 read as 0, 1, -1, 2, -2.  The largest k, 2^32 - 2, reads
 * as -(2^31 - 1), which an s32 holds.
 *
 * Return: the value; 0 for a damaged code.
 */
s32 rp_bits_se(struct rp_bits *b)
{
	u32 code = rp_bits_ue(b);
	s32 magnitude = (s32)((code + 1u) / 2u);
	return (code & 1u) ? magnitude : -magnitude;
}

/**
 * rp_bits_skip() - Pass over @n bits.
 * @b: the reader
 * @n: the bits, any number
 *
 * For the fields a parser must get past but doesn't keep, such as the
 * profile bits it does not check, the extra slice header bits and the entry
 * point offsets.
 * Past the end of the unit it sets the overrun flag, as a read does.
 */
void rp_bits_skip(struct rp_bits *b, u32 n)
{
	while (n > 0)
	{
		u32 take = n > FILL_BITS_MAX ? FILL_BITS_MAX : n;
		(void)rp_bits_u(b, take);
		n -= take;
	}
}

/**
 * rp_bits_ue_at_most() - A ue(v) the standard bounds, read whole before it is narrowed.
 * @b:     the reader
 * @max:   the largest value the standard allows
 * @value: out: the value as read
 *
 * A value too big for the field it goes into would wrap into range if it
 * were narrowed first; read into 32 bits and checked, it cannot.
 *
 * Return: TRUE when the value is at most @max.
 */
BOOL rp_bits_ue_at_most(struct rp_bits *b, u32 max, u32 *value)
{
	*value = rp_bits_ue(b);
	return *value <= max;
}

/**
 * rp_bits_se_in() - An se(v) the standard bounds, read whole before it is narrowed.
 * @b:     the reader
 * @min:   the smallest value the standard allows
 * @max:   the largest
 * @value: out: the value as read
 *
 * Return: TRUE when the value is within range.
 */
BOOL rp_bits_se_in(struct rp_bits *b, s32 min, s32 max, s32 *value)
{
	*value = rp_bits_se(b);
	return *value >= min && *value <= max;
}

/**
 * rp_bits_byte_align() - byte_alignment() (7.3.2.12): on to the start of the next byte.
 * @b: the reader
 *
 * The bits still in the cache are the rest of the byte read last (see the
 * head of this file), so moving to the next byte is emptying the cache.  On
 * a reader that is aligned already the cache is empty and nothing changes.
 * The alignment bits themselves, a one and then zeros, are not checked.
 */
void rp_bits_byte_align(struct rp_bits *b)
{
	b->cache = 0;
	b->cache_bits = 0;
}

/**
 * rp_bits_byte_offset() - Where the next unread byte lies in the unit.
 * @b: the reader, byte aligned
 *
 * The offset is into the unit as it arrived, from its first header byte and
 * with the prevention bytes counted: what the HEVC block takes as the start
 * of the slice data (data_byte_offset).  An aligned reader's cache is empty
 * (see the head of this file), so the next unread byte is the next one
 * take_byte() looks at.
 *
 * Return: the offset in bytes.
 */
u32 rp_bits_byte_offset(const struct rp_bits *b)
{
	return b->next;
}
