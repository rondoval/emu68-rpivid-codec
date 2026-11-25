/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_bits - the bit reader on hand-made bytes (developer-guide.md section 4.3):
 * every Exp-Golomb length, prevention bytes at every byte position, reads
 * past the end flagged, the alignment and the byte offset, the bounded reads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bit_writer.h"
#include "bits.h"
#include "host_check.h"

/**
 * check_bounded_reads() - rp_bits_ue_at_most() and rp_bits_se_in(): a value read whole, and whether it is within its range.
 * @buf: a buffer for the unit
 * @cap: its size
 */
static void check_bounded_reads(u8 *buf, u32 cap)
{
	struct writer w;
	struct rp_bits b;
	u32 value;
	s32 signed_value;
	writer_init(&w, buf, cap);
	writer_byte(&w, 0x40); /* a header: VPS, layer 0, tid 1 */
	writer_byte(&w, 0x01);
	writer_ue(&w, 256);
	writer_ue(&w, 7);
	writer_se(&w, -13);
	writer_se(&w, 12);
	writer_trailing(&w);
	rp_bits_init(&b, buf, w.len, 2);
	CHECK(!rp_bits_ue_at_most(&b, 255, &value) && value == 256, "a ue over its bound, read whole: %u", value);
	CHECK(rp_bits_ue_at_most(&b, 7, &value) && value == 7, "a ue at its bound: %u", value);
	CHECK(!rp_bits_se_in(&b, -12, 12, &signed_value) && signed_value == -13, "an se under its bound: %d", (int)signed_value);
	CHECK(rp_bits_se_in(&b, -12, 12, &signed_value) && signed_value == 12, "an se at its bound: %d", (int)signed_value);
	CHECK(!b.overrun, "no overrun in the bounded reads");
}

/**
 * main() - Run the bit reader's checks.
 *
 * Return: 0 when all passed, 1 when not.
 */
int main(void)
{
	u8 buf[65536];
	struct writer w;
	struct rp_bits b;
	u32 i;
	writer_init(&w, buf, sizeof(buf));

	/* the Exp-Golomb codes: every value of every length, signed and unsigned, around the powers of two */
	static const u32 values[] = {0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256, 1023, 1024, 65535, 65536, 0x7fffffffu, 0xfffffffeu};
	writer_byte(&w, 0x40); /* a header: VPS, layer 0, tid 1 */
	writer_byte(&w, 0x01);
	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++)
		writer_ue(&w, values[i]);
	for (i = 0; i < 20; i++)
		writer_se(&w, (s32)i - 10);
	writer_ue(&w, 0); /* so that a zero run (00 00) can form before the prevention test */
	writer_bits(&w, 0, 24);
	writer_bits(&w, 0x3, 8);   /* 00 00 00 (prevented) then 03: data */
	writer_bits(&w, 0x12345678u, 32);
	writer_trailing(&w);
	rp_bits_init(&b, buf, w.len, 2);
	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++)
	{
		u32 got = rp_bits_ue(&b);
		CHECK(got == values[i], "ue %u: got %u", values[i], got);
	}
	for (i = 0; i < 20; i++)
	{
		s32 got = rp_bits_se(&b);
		CHECK(got == (s32)i - 10, "se %d: got %d", (int)i - 10, (int)got);
	}
	CHECK(rp_bits_ue(&b) == 0, "the zero ue");
	CHECK(rp_bits_u(&b, 24) == 0, "24 zero bits through prevention bytes");
	CHECK(rp_bits_u(&b, 8) == 3, "a data 03 after a prevention byte");
	CHECK(rp_bits_u(&b, 32) == 0x12345678u, "32 bits");
	CHECK(!b.overrun, "no overrun");
	rp_bits_u(&b, 8);
	rp_bits_u(&b, 8);
	CHECK(b.overrun, "reading past the end sets the flag");

	/* prevention bytes at every byte position: 00 00 03 xx written, 00 00 xx read, the offset counts them */
	for (i = 0; i < 64; i++)
	{
		u32 j;
		struct writer w2;
		writer_init(&w2, buf, sizeof(buf));
		writer_byte(&w2, 0x02);
		writer_byte(&w2, 0x01);
		for (j = 0; j < i; j++)
			writer_byte(&w2, 0xa5);
		writer_bits(&w2, 0, 16);
		writer_bits(&w2, 1, 8);  /* 00 00 01 would be a start code: prevented */
		writer_bits(&w2, 0xc3, 8);
		writer_trailing(&w2);
		rp_bits_init(&b, buf, w2.len, 2);
		for (j = 0; j < i; j++)
			CHECK(rp_bits_u(&b, 8) == 0xa5, "filler %u at %u", j, i);
		CHECK(rp_bits_u(&b, 16) == 0, "two zeros at %u", i);
		CHECK(rp_bits_u(&b, 8) == 1, "the 01 after them at %u", i);
		CHECK(rp_bits_byte_offset(&b) == 2 + i + 4, "offset counts the prevention byte at %u: %u", i, rp_bits_byte_offset(&b));
		CHECK(rp_bits_u(&b, 8) == 0xc3, "the byte after at %u", i);
		CHECK(!b.overrun, "no overrun at %u", i);
	}

	/* byte_alignment(): one bit in, the rest of the byte passed over */
	{
		static const u8 unit[] = {0x42, 0x01, 0xc0}; /* header, then 1100 0000: a data bit, the stop bit, zeros */
		rp_bits_init(&b, unit, sizeof(unit), 2);
		CHECK(rp_bits_u(&b, 1) == 1, "the data bit");
		rp_bits_byte_align(&b);
		CHECK(rp_bits_byte_offset(&b) == 3, "aligned to the end");
		CHECK(!b.overrun, "no overrun aligning");
	}

	check_bounded_reads(buf, sizeof(buf));

	printf("test_bits: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
