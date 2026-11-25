/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bits.h - the bit reader over a NAL unit (docs/architecture.md section 6.1).
 *
 * The parameter set and slice header parsers (ps.c, slice.c) read every
 * syntax element through it: u(n), ue(v), se(v) and byte_alignment() of
 * H.265 7.2, 7.3.2.12 and 9.2.
 *
 * It reads the unit's bytes where they lie and drops each emulation
 * prevention byte as it meets one, so no RBSP copy is made, and the byte
 * position it reports counts the prevention bytes: that is how the HEVC
 * block wants the slice data's offset (data_byte_offset).
 *
 * No read returns an error.  A read past the end of the unit gets zero bits
 * and sets the overrun flag, which stays set, so a parser reads a whole
 * header and tests the flag once, at its end.  A value the standard bounds
 * is read with rp_bits_ue_at_most() or rp_bits_se_in(), whole and checked,
 * before the parser narrows it into its field.
 */
#ifndef RP_BITS_H
#define RP_BITS_H

#include <exec/types.h>
#include <types.h>

#define RP_BITS_PER_BYTE 8u /* the bits of a byte of the bitstream */

/* A reader over one NAL unit: rp_bits_init() starts it, the head of bits.c tells how its cache works */
struct rp_bits
{
	const u8 *data; /* the NAL unit as it arrived: the header first, the prevention bytes still in */
	u32 len;        /* the unit's length in bytes */
	u32 next;       /* the index in data of the next byte to take into the cache */
	u32 zeros;      /* the zero bytes just taken in a row: after two, a 0x03 is a prevention byte */
	u32 cache;      /* bits taken from the bytes but not yet read: the next one at bit 31, zeros below the last */
	u32 cache_bits; /* how many bits the cache holds: fewer than 8 between reads */
	BOOL overrun;   /* a read asked for more bits than the unit had; it stays set */
};

void rp_bits_init(struct rp_bits *b, const u8 *data, u32 len, u32 skip);
u32 rp_bits_u(struct rp_bits *b, u32 n);
BOOL rp_bits_flag(struct rp_bits *b);
u32 rp_bits_ue(struct rp_bits *b);
s32 rp_bits_se(struct rp_bits *b);
void rp_bits_skip(struct rp_bits *b, u32 n);
BOOL rp_bits_ue_at_most(struct rp_bits *b, u32 max, u32 *value);
BOOL rp_bits_se_in(struct rp_bits *b, s32 min, s32 max, s32 *value);
void rp_bits_byte_align(struct rp_bits *b);
u32 rp_bits_byte_offset(const struct rp_bits *b);

#endif /* RP_BITS_H */
