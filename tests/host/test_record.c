/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_record - the picture record filled from hand-made sets and headers (developer-guide.md section 4.3).
 *
 * The picture's part: its IRAP and IDR flags from the NAL unit type, into
 * the decode parameters too; the scaling matrix only when the SPS enables
 * scaling lists, the PPS's when it sends lists, else the SPS's, and none
 * for a PPS that sends lists under an SPS without them (7.4.3.3.1), as the
 * command generator goes by the SPS's flag.  An I slice's parameters: its
 * size in bits, no merge candidates; and a slice past the array refused.
 */
#include <stdio.h>
#include <string.h>

#include "host_check.h"
#include "nal.h"
#include "record.h"

#define SPS_LIST_VALUE 1 /* the first 4x4 factor of the SPS's lists, to tell them from the PPS's */
#define PPS_LIST_VALUE 2 /* the PPS's */
#define SLICE_BYTES 100  /* the NAL unit of the slice the test appends */

/* A scaling case: the sets' flags, and the matrix the record must hold */
struct scaling_case
{
	const char *name;   /* what the case holds to */
	BOOL sps_enabled;   /* scaling_list_enabled_flag */
	BOOL pps_present;   /* pps_scaling_list_data_present_flag */
	BOOL has_scaling;   /* the record gives a matrix */
	u8 first_factor;    /* its first 4x4 factor, when it does */
};

static const struct scaling_case scaling_cases[] = {
	{"no scaling lists", FALSE, FALSE, FALSE, 0},
	{"the SPS's lists", TRUE, FALSE, TRUE, SPS_LIST_VALUE},
	{"the PPS's lists over the SPS's", TRUE, TRUE, TRUE, PPS_LIST_VALUE},
	{"a PPS's lists under an SPS without them: none", FALSE, TRUE, FALSE, 0},
};

static struct rp_record record;
static struct rp_sps sps;
static struct rp_pps pps;
static struct rp_slice_header header;
static struct v4l2_ctrl_hevc_slice_params slices[1];

/**
 * picture_with() - The record's picture part filled for a first slice of a NAL unit type.
 * @nal_type:    the slice's nal_unit_type
 * @sps_enabled: the SPS's scaling_list_enabled_flag
 * @pps_present: the PPS's pps_scaling_list_data_present_flag
 */
static void picture_with(u32 nal_type, BOOL sps_enabled, BOOL pps_present)
{
	memset(&sps, 0, sizeof(sps));
	memset(&pps, 0, sizeof(pps));
	memset(&header, 0, sizeof(header));
	sps.scaling_list_enabled = sps_enabled;
	sps.scaling.scaling_list_4x4[0][0] = SPS_LIST_VALUE;
	pps.scaling_list_present = pps_present;
	pps.scaling.scaling_list_4x4[0][0] = PPS_LIST_VALUE;
	header.nal_type = nal_type;
	header.slice_type = V4L2_HEVC_SLICE_TYPE_I;
	header.pic_output = TRUE;
	record.slices = slices;
	record.slice_cap = sizeof(slices) / sizeof(slices[0]);
	rp_record_clear(&record);
	rp_record_picture(&record, 1, 0, &header, &sps, &pps, nal_is_irap(nal_type), FALSE);
}

/**
 * check_scaling() - The matrix the record gives for a case's sets.
 * @c: the case
 */
static void check_scaling(const struct scaling_case *c)
{
	picture_with(NAL_TRAIL_R, c->sps_enabled, c->pps_present);
	CHECK(record.has_scaling == c->has_scaling, "%s: %s", c->name, record.has_scaling ? "a matrix given" : "no matrix");
	if (c->has_scaling)
		CHECK(record.scaling.scaling_list_4x4[0][0] == c->first_factor, "%s: first factor %u", c->name,
		      record.scaling.scaling_list_4x4[0][0]);
}

/**
 * check_flags() - The IRAP and IDR flags from the NAL unit type, in the record and its decode parameters.
 * @nal_type: the slice's nal_unit_type
 * @irap:     an IRAP picture
 * @idr:      an IDR picture
 */
static void check_flags(u32 nal_type, BOOL irap, BOOL idr)
{
	picture_with(nal_type, FALSE, FALSE);
	u64 flags = record.decode.flags;
	CHECK(record.irap == irap && record.idr == idr, "type %u: IRAP %d, IDR %d", nal_type, record.irap, record.idr);
	CHECK(((flags & V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC) != 0) == irap && ((flags & V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC) != 0) == idr,
	      "type %u: decode flags 0x%llx", nal_type, (unsigned long long)flags);
}

/**
 * check_slice() - An I slice appended: its size in bits and no merge candidates; a second one past the array refused.
 */
static void check_slice(void)
{
	picture_with(NAL_IDR_W_RADL, FALSE, FALSE);
	header.data_byte_offset = 7;
	CHECK(rp_record_slice(&record, &header, SLICE_BYTES) == NULL && record.slice_count == 1, "an I slice is taken");
	CHECK(slices[0].bit_size == SLICE_BYTES * 8 && slices[0].data_byte_offset == 7, "the slice's size: %u bits", slices[0].bit_size);
	CHECK(slices[0].five_minus_max_num_merge_cand == 0, "an I slice has no merge candidates to send");
	CHECK(rp_record_slice(&record, &header, SLICE_BYTES) != NULL && record.slice_count == 1, "a slice past the array is refused");
}

/**
 * main() - Run the record's checks.
 *
 * Return: 0 when all passed, 1 when not.
 */
int main(void)
{
	for (u32 i = 0; i < sizeof(scaling_cases) / sizeof(scaling_cases[0]); i++)
		check_scaling(&scaling_cases[i]);
	check_flags(NAL_TRAIL_R, FALSE, FALSE);
	check_flags(NAL_CRA_NUT, TRUE, FALSE);
	check_flags(NAL_IDR_N_LP, TRUE, TRUE);
	check_slice();
	printf("test_record: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
