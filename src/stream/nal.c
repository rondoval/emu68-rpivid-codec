/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nal.c - the Annex B scanner (nal.h).
 *
 * A start code is 00 00 01, and any number of zero bytes may come before it:
 * a leading zero_byte, or the trailing_zero_8bits after the unit before
 * (B.2).  So a zero byte can't be told apart when it is seen: it may be the
 * unit's own data or the beginning of the next start code.  Zeros are
 * therefore only counted (zero_run).  When a byte other than 00 or 01
 * follows them, they were the unit's own and are given out before it
 * (owed_zeros, give_zeros()); when the 01 of a start code follows, they were
 * the start code's and are dropped.
 *
 * Inside a unit no more than two zero bytes come in a row: 7.4.2 forbids
 * 00 00 00 there, and the encoder's prevention bytes see to it.  In a valid
 * stream, then, no more than two zeros are ever held back.  In a damaged one
 * the run may be any length, and its zeros go out ZEROS_PER_SPAN per event.
 *
 * The two bytes after a start code are the unit's header, whatever they are:
 * a TRAIL_N unit's header begins 00 01, for one (type 0, layer 0, temporal
 * id 0).  The exception is 00 00, which can't be a header, as its
 * nuh_temporal_id_plus1 would be 0: it is taken as the zeros of the next
 * start code.  The header is decoded for the type, layer and temporal id and
 * given out as the unit's first span, as the session keeps a unit whole,
 * header included: that is how the HEVC block takes it.
 *
 * Bytes before the first start code are dropped, and so are the bytes after
 * a header that turned out to be zeros, up to the next start code.
 */
#include "nal.h"

#define START_CODE_ZEROS 2u    /* the zero bytes a start code begins with */
#define START_CODE_LAST  0x01u /* and the byte that ends it */
#define ZEROS_PER_SPAN   2u    /* the most owed zeros one span gives out: the size of zero_bytes */

/* The bytes give_zeros() points its spans at: the zeros may have been counted in an earlier packet */
static const u8 zero_bytes[ZEROS_PER_SPAN] = {0, 0};

/**
 * rp_nal_init() - Set a scanner to the start of a stream.
 * @n: the scanner
 *
 * No unit is open, no zeros are counted and no packet is given: the first
 * rp_nal_packet() comes next.  The session calls it as it starts, after the
 * extradata and at a flush, so a unit a flush cuts short is forgotten.
 */
void rp_nal_init(struct rp_nal *n)
{
	n->in_unit = FALSE;
	n->expect_header = FALSE;
	n->zero_run = 0;
	n->owed_zeros = 0;
	n->header_len = 0;
	n->type = n->layer = n->temporal_id = 0;
	n->data = NULL;
	n->len = 0;
	n->packet = NULL;
	n->packet_len = n->packet_pos = 0;
}

/**
 * rp_nal_packet() - Give the scanner the next packet's bytes.
 * @n:    the scanner
 * @data: the packet's bytes, which the caller keeps until rp_nal_next() has used them up
 * @len:  the packet's length in bytes
 *
 * The scan carries on from the packet before: a unit, a header or a run of
 * zeros may continue in this one.
 */
void rp_nal_packet(struct rp_nal *n, const u8 *data, u32 len)
{
	n->packet = data;
	n->packet_len = len;
	n->packet_pos = 0;
}

/**
 * give_zeros() - Give out zeros the unit is owed, at most ZEROS_PER_SPAN of them, as a span.
 * @n: the scanner, owing zeros
 *
 * The span points at zero_bytes rather than into the packet: the zeros may
 * have been counted in an earlier packet.  Zeros still owed go out on the
 * next calls, before anything else.
 *
 * Return: RP_NAL_BYTES.
 */
static enum rp_nal_event give_zeros(struct rp_nal *n)
{
	u32 take = n->owed_zeros > ZEROS_PER_SPAN ? ZEROS_PER_SPAN : n->owed_zeros;
	n->owed_zeros -= take;
	n->data = zero_bytes;
	n->len = take;
	return RP_NAL_BYTES;
}

/**
 * take_header_byte() - Take a byte after a start code: one of the unit's header, or the next start code's zeros.
 * @n:    the scanner, expecting a header
 * @byte: the byte
 *
 * The header bytes are kept in the scanner, as a header may straddle two
 * packets.  A first byte 00 with a second 00 is no header (see the head of
 * this file): the two are counted as the next start code's zeros, and no
 * unit is open until it comes.
 *
 * Return: TRUE when the header is complete and the unit starts: its type,
 * layer and temporal id are set, and the span is the header.
 */
static BOOL take_header_byte(struct rp_nal *n, u8 byte)
{
	if (byte == 0 && n->header_len == 1 && n->header[0] == 0)
	{
		n->expect_header = FALSE;
		n->header_len = 0;
		n->zero_run = NAL_HEADER_BYTES; /* both header bytes, zeros */
		return FALSE;
	}
	n->header[n->header_len++] = byte;
	if (n->header_len < NAL_HEADER_BYTES)
		return FALSE;
	n->type = nal_header_type(n->header);
	n->layer = nal_header_layer(n->header);
	n->temporal_id = nal_header_temporal_id(n->header);
	n->in_unit = TRUE;
	n->expect_header = FALSE;
	n->data = n->header;
	n->len = NAL_HEADER_BYTES;
	return TRUE;
}

/**
 * take_start_code() - Take the 01 of a start code: the open unit ends, a header comes next.
 * @n: the scanner, after two or more zeros
 *
 * The zeros counted before the 01 were the start code's and are dropped.
 *
 * Return: TRUE when a unit was open and ends here, FALSE when none was.
 */
static BOOL take_start_code(struct rp_nal *n)
{
	BOOL was_in_unit = n->in_unit;
	n->zero_run = 0;
	n->header_len = 0;
	n->expect_header = TRUE;
	n->in_unit = FALSE;
	return was_in_unit;
}

/**
 * give_span() - Give out the unit's bytes from the scan position up to the next zero byte, as a span.
 * @n: the scanner, at a byte of the unit that is not zero
 *
 * The span points into the packet.  It stops at a zero byte, which the next
 * call counts, or at the end of the packet.
 *
 * Return: RP_NAL_BYTES.
 */
static enum rp_nal_event give_span(struct rp_nal *n)
{
	u32 start = n->packet_pos;
	while (n->packet_pos < n->packet_len && n->packet[n->packet_pos] != 0)
		n->packet_pos++;
	n->data = n->packet + start;
	n->len = n->packet_pos - start;
	return RP_NAL_BYTES;
}

/**
 * rp_nal_next() - The packet's next event.
 * @n: the scanner, a packet given
 *
 * Owed zeros go out first.  Then each byte is sorted (see the head of this
 * file): a header byte, a zero to count, the 01 of a start code, a byte
 * outside any unit to drop, or the unit's data to give out.  The scan goes
 * on until a byte makes an event.
 *
 * Return: RP_NAL_START (the span is the header), RP_NAL_BYTES (the span is
 * the unit's next bytes) or RP_NAL_END; RP_NAL_NONE when the packet is used
 * up.  A unit still open then carries on in the next packet, or is ended by
 * rp_nal_finish().
 */
enum rp_nal_event rp_nal_next(struct rp_nal *n)
{
	if (n->owed_zeros != 0)
		return give_zeros(n);

	while (n->packet_pos < n->packet_len)
	{
		u8 byte = n->packet[n->packet_pos];

		if (n->expect_header)
		{
			n->packet_pos++;
			if (take_header_byte(n, byte))
				return RP_NAL_START;
		}
		else if (byte == 0)
		{
			n->zero_run++;
			n->packet_pos++;
		}
		else if (byte == START_CODE_LAST && n->zero_run >= START_CODE_ZEROS)
		{
			n->packet_pos++;
			if (take_start_code(n))
				return RP_NAL_END;
		}
		else if (!n->in_unit)
		{
			/* before the first start code, or after a header that was a start code's zeros: dropped */
			n->zero_run = 0;
			n->packet_pos++;
		}
		else if (n->zero_run != 0)
		{
			/* a data byte: the zeros before it were the unit's own */
			n->owed_zeros = n->zero_run;
			n->zero_run = 0;
			return give_zeros(n);
		}
		else
			return give_span(n);
	}
	return RP_NAL_NONE;
}

/**
 * rp_nal_finish() - End the open unit where a frame or the stream ends.
 * @n: the scanner, its packet used up
 *
 * Annex B ends a unit only with the next start code, so the last unit of a
 * frame needs this.  Zeros still counted are dropped, as no unit ends in a
 * zero byte (7.4.2: a 0x03 goes after one that would), and so is half a
 * header.  The session calls it when a packet ends a frame or the stream,
 * and at the end of Annex B extradata.
 *
 * Return: RP_NAL_END when a unit was open, RP_NAL_NONE when none was.
 */
enum rp_nal_event rp_nal_finish(struct rp_nal *n)
{
	n->zero_run = 0;
	n->owed_zeros = 0;
	n->header_len = 0;
	n->expect_header = FALSE;
	if (!n->in_unit)
		return RP_NAL_NONE;
	n->in_unit = FALSE;
	return RP_NAL_END;
}
