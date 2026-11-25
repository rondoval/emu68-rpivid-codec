/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_nal - the Annex B scanner (developer-guide.md section 4.3): three- and
 * four-byte start codes, trailing zeros, a stream cut into packets at every
 * size, the units reassembled whole with their types.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_check.h"
#include "nal.h"

/* the units of the test stream, header included, no start codes */
struct unit
{
	const u8 *data;
	u32 len;
};

static const u8 u_vps[] = {0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90};
static const u8 u_sps[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5d, 0xa0};
static const u8 u_pps[] = {0x44, 0x01, 0xc1, 0x72, 0xb4, 0x62, 0x40};
static const u8 u_idr[] = {0x26, 0x01, 0xaf, 0x00, 0x00, 0x03, 0x01, 0x00, 0x7f, 0x80, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x02};
static const u8 u_trail_n[] = {0x00, 0x03, 0xe0, 0x24, 0x00, 0x00, 0x03, 0x01, 0x80}; /* TRAIL_N: a header that begins with 00; TemporalId 2 */
static const u8 u_trail[] = {0x02, 0x01, 0xd0, 0x00, 0xa0};
static const struct unit units[] = {{u_vps, sizeof(u_vps)},     {u_sps, sizeof(u_sps)},     {u_pps, sizeof(u_pps)},
                                    {u_idr, sizeof(u_idr)},     {u_trail_n, sizeof(u_trail_n)}, {u_trail, sizeof(u_trail)}};
static const u32 types[] = {32, 33, 34, 19, 0, 1};
static const u32 temporal_ids[] = {0, 0, 0, 0, 2, 0};
#define UNITS 6

/**
 * make_stream() - Build the test stream.
 * @out: the buffer for the stream
 *
 * Start codes of 4, 3, 3, 4, 4, 3 bytes, trailing zeros after two units, zeros at the end.
 *
 * Return: the stream's length.
 */
static u32 make_stream(u8 *out)
{
	u32 n = 0, i;
	for (i = 0; i < UNITS; i++)
	{
		if (i == 0 || i == 3 || i == 4)
			out[n++] = 0;
		out[n++] = 0;
		out[n++] = 0;
		out[n++] = 1;
		memcpy(out + n, units[i].data, units[i].len);
		n += units[i].len;
		if (i == 1 || i == 3)
		{
			out[n++] = 0; /* trailing_zero_8bits */
			out[n++] = 0;
		}
	}
	out[n++] = 0;
	out[n++] = 0;
	return n;
}

/**
 * scan() - Scan a stream in packets and check the units that come out.
 * @stream: the stream
 * @len: its length
 * @chunk: the packet size in bytes
 */
static void scan(const u8 *stream, u32 len, u32 chunk)
{
	struct rp_nal n;
	u8 got[256];
	u32 got_len = 0, count = 0, pos = 0;
	int started = 0;
	rp_nal_init(&n);
	for (;;)
	{
		enum rp_nal_event ev;
		int finishing = pos >= len;
		if (!finishing)
		{
			u32 take = len - pos < chunk ? len - pos : chunk;
			rp_nal_packet(&n, stream + pos, take);
			pos += take;
			ev = rp_nal_next(&n);
		}
		else
			ev = rp_nal_finish(&n);
		while (ev != RP_NAL_NONE)
		{
			if (ev == RP_NAL_START)
			{
				CHECK(!started, "chunk %u: a unit started inside another", chunk);
				started = 1;
				got_len = 0;
				CHECK(count < UNITS && n.type == types[count], "chunk %u unit %u: type %u", chunk, count, n.type);
				CHECK(n.layer == 0, "chunk %u unit %u: layer %u", chunk, count, n.layer);
				CHECK(count < UNITS && n.temporal_id == temporal_ids[count], "chunk %u unit %u: temporal id %u", chunk, count, n.temporal_id);
				memcpy(got + got_len, n.data, n.len);
				got_len += n.len;
			}
			else if (ev == RP_NAL_BYTES)
			{
				CHECK(started, "chunk %u: bytes outside a unit", chunk);
				CHECK(got_len + n.len <= sizeof(got), "chunk %u: a unit too long", chunk);
				memcpy(got + got_len, n.data, n.len);
				got_len += n.len;
			}
			else if (ev == RP_NAL_END)
			{
				CHECK(started, "chunk %u: an end outside a unit", chunk);
				started = 0;
				if (count < UNITS)
					CHECK(got_len == units[count].len && memcmp(got, units[count].data, got_len) == 0,
					      "chunk %u unit %u: %u bytes, want %u", chunk, count, got_len, units[count].len);
				count++;
			}
			if (finishing)
				break; /* the finish gave the last unit */
			ev = rp_nal_next(&n);
		}
		if (finishing)
			break;
	}
	CHECK(count == UNITS, "chunk %u: %u units, want %u", chunk, count, UNITS);
}

/**
 * main() - Scan the stream at every packet size and report.
 *
 * Return: 0 when all passed, 1 when not.
 */
int main(void)
{
	u8 stream[512];
	u32 len = make_stream(stream);
	u32 chunk;
	for (chunk = 1; chunk <= len; chunk++)
		scan(stream, len, chunk);
	scan(stream, len, 65536);
	printf("test_nal: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
