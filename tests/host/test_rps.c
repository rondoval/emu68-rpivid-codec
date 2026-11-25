/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_rps - POC, the reference picture set and the reference lists on hand-made values (developer-guide.md section 4.3).
 *
 * The POC of 8.3.1 with its LSB the same, wrapping forward and back, at an
 * IRAP, at both ends of its 32-bit range, and one step past each, which is
 * refused rather than overflowing.  A short-term reference the buffer
 * holds: its entry in the record's DPB array, StCurrBefore holding that
 * entry's index as V4L2 defines it (not its POC), list 0 built from it, the
 * picture the set leaves out unmarked.  A short-term reference whose POC
 * lies past the range: missing, though a 32-bit sum would have wrapped onto
 * a picture the buffer holds, so substituted and the picture corrupt.  A
 * picture with NoRaslOutputFlag: the buffer unmarked before its set, so a
 * CRA's references are missing and an IDR leaves the DPB array empty.
 */
#include <stdio.h>
#include <string.h>

#include "host_check.h"
#include "rps.h"

#define LOG2_MAX_POC_LSB 4         /* MaxPicOrderCntLsb 16, the smallest: half a range is 8 */
#define POC_MIN (-2147483647 - 1)  /* PicOrderCntVal's range (8.3.1) */
#define POC_MAX 2147483647

/* A POC derivation: its inputs, and what 8.3.1 gives */
struct poc_case
{
	const char *name;  /* what the case holds to */
	s32 prev_tid0_poc; /* PicOrderCntVal of prevTid0Pic */
	u32 poc_lsb;       /* slice_pic_order_cnt_lsb */
	BOOL irap_no_rasl; /* an IRAP with NoRaslOutputFlag */
	BOOL fits;         /* the POC is in range */
	s32 poc;           /* the POC, when it is */
};

static const struct poc_case poc_cases[] = {
	{"the same MSB", 17, 3, FALSE, TRUE, 19},
	{"the LSB wrapping forward: the MSB one step up", 30, 2, FALSE, TRUE, 34},
	{"the LSB wrapping back: the MSB one step down", 17, 12, FALSE, TRUE, 12},
	{"an IRAP that starts a sequence: the MSB 0", 100, 5, TRUE, TRUE, 5},
	{"the largest POC", POC_MAX - 8, 15, FALSE, TRUE, POC_MAX},
	{"the smallest POC", POC_MIN + 5, 0, FALSE, TRUE, POC_MIN},
	{"the MSB one step past the largest", POC_MAX - 7, 0, FALSE, FALSE, 0},
	{"the MSB one step below the smallest", POC_MIN, 9, FALSE, FALSE, 0},
};

static struct rp_record record;
static struct rp_dpb dpb;
static struct rp_slice_header header;
static struct rp_sps sps;

/**
 * check_poc() - A POC derived as 8.3.1 says, or refused past its range.
 * @c: the case
 */
static void check_poc(const struct poc_case *c)
{
	s32 poc = 0;
	BOOL fits = rp_poc_derive(c->prev_tid0_poc, LOG2_MAX_POC_LSB, c->poc_lsb, c->irap_no_rasl, &poc);
	CHECK(fits == c->fits, "%s: %s", c->name, fits ? "taken" : "refused");
	if (fits && c->fits)
		CHECK(poc == c->poc, "%s: POC %d, not %d", c->name, (int)poc, (int)c->poc);
}

/**
 * picture_for() - A P picture with one short-term reference that it uses, ready for rp_rps_apply().
 * @poc:       the current picture's POC
 * @delta_poc: the reference's POC less the current one's: negative for StCurrBefore, positive for StCurrAfter
 */
static void picture_for(s32 poc, s32 delta_poc)
{
	memset(&record, 0, sizeof(record));
	memset(&header, 0, sizeof(header));
	memset(&sps, 0, sizeof(sps));
	sps.log2_max_poc_lsb = LOG2_MAX_POC_LSB;
	record.poc = poc;
	struct rp_st_rps *rps = &header.st_rps;
	if (delta_poc < 0)
	{
		rps->num_negative = 1;
		rps->delta_poc_s0[0] = delta_poc;
		rps->used_s0[0] = 1;
	}
	else
	{
		rps->num_positive = 1;
		rps->delta_poc_s1[0] = delta_poc;
		rps->used_s1[0] = 1;
	}
	header.slice_type = V4L2_HEVC_SLICE_TYPE_P;
	header.num_ref_idx_active[0] = 1;
	header.num_pic_total_curr = 1;
}

/**
 * check_found() - A reference the buffer holds: the DPB array's entry, its index in StCurrBefore, list 0 from it.
 */
static void check_found(void)
{
	const struct v4l2_ctrl_hevc_decode_params *decode = &record.decode;
	memset(&dpb, 0, sizeof(dpb));
	dpb.entries[1] = (struct rp_dpb_entry){.used = TRUE, .poc = 4, .ref = RP_REF_SHORT, .number = 2};
	dpb.entries[3] = (struct rp_dpb_entry){.used = TRUE, .poc = 8, .ref = RP_REF_SHORT, .number = 5};
	picture_for(10, -2);
	rp_rps_apply(&record, &dpb, &header, &sps);
	CHECK(decode->num_active_dpb_entries == 1 && decode->dpb[0].pic_order_cnt_val == 8 && record.ref_number[0] == 5,
	      "the reference is the DPB array's only entry: %u entries", decode->num_active_dpb_entries);
	CHECK(decode->num_poc_st_curr_before == 1 && decode->poc_st_curr_before[0] == 0 && decode->num_poc_st_curr_after == 0 &&
	          decode->num_poc_lt_curr == 0,
	      "StCurrBefore holds the entry's index, 0, not its POC: %u", decode->poc_st_curr_before[0]);
	CHECK(!record.corrupt, "a reference the buffer holds leaves the picture whole");
	CHECK(dpb.entries[3].ref == RP_REF_SHORT && dpb.entries[1].ref == RP_REF_NONE, "the set keeps its picture a reference and unmarks the other");
	u8 ref_idx[V4L2_HEVC_DPB_ENTRIES_NUM_MAX];
	memset(ref_idx, 0xff, sizeof(ref_idx));
	CHECK(rp_ref_list(&record, &header, 0, ref_idx) && ref_idx[0] == 0, "list 0 names the DPB array's entry 0: %u", ref_idx[0]);
}

/**
 * check_past_range() - A reference whose POC lies past 2^31 - 1: missing, not the picture a wrapped sum lands on.
 */
static void check_past_range(void)
{
	const struct v4l2_ctrl_hevc_decode_params *decode = &record.decode;
	memset(&dpb, 0, sizeof(dpb));
	dpb.entries[0] = (struct rp_dpb_entry){.used = TRUE, .poc = POC_MIN + 2, .ref = RP_REF_SHORT, .number = 7};
	picture_for(POC_MAX - 2, 5); /* POC_MAX + 3, which wraps to POC_MIN + 2 in 32 bits */
	rp_rps_apply(&record, &dpb, &header, &sps);
	CHECK(decode->num_active_dpb_entries == 1 && record.ref_number[0] == 0 && decode->dpb[0].pic_order_cnt_val == POC_MIN,
	      "the reference is missing: entry 0 is picture %u, POC %d", record.ref_number[0], (int)decode->dpb[0].pic_order_cnt_val);
	CHECK(decode->num_poc_st_curr_after == 1 && decode->poc_st_curr_after[0] == 0, "StCurrAfter holds the substitute's entry");
	CHECK(record.corrupt, "a missing reference marks the picture corrupt");
	CHECK(dpb.entries[0].ref == RP_REF_NONE, "the picture a wrapped sum would have found is unmarked");
}

/**
 * check_sequence_start() - A picture with NoRaslOutputFlag: every picture unmarked before its set is applied (8.3.2).
 * @idr: an IDR picture, whose set is empty; else a CRA that names the buffer's picture
 */
static void check_sequence_start(BOOL idr)
{
	const struct v4l2_ctrl_hevc_decode_params *decode = &record.decode;
	const char *name = idr ? "an IDR" : "a CRA with NoRaslOutputFlag";
	memset(&dpb, 0, sizeof(dpb));
	dpb.entries[2] = (struct rp_dpb_entry){.used = TRUE, .poc = 8, .ref = RP_REF_SHORT, .number = 5};
	picture_for(10, -2);
	record.irap = TRUE;
	record.no_rasl_output = TRUE;
	record.idr = idr;
	rp_rps_apply(&record, &dpb, &header, &sps);
	CHECK(dpb.entries[2].ref == RP_REF_NONE, "%s unmarks the buffer's picture", name);
	if (idr)
		CHECK(decode->num_active_dpb_entries == 0 && decode->num_poc_st_curr_before == 0 && !record.corrupt,
		      "an IDR leaves the DPB array and the Curr lists empty: %u entries", decode->num_active_dpb_entries);
	else
		CHECK(decode->num_active_dpb_entries == 1 && record.ref_number[0] == 0 && record.corrupt,
		      "a CRA's reference, unmarked first, is missing: entry 0 is picture %u", record.ref_number[0]);
}

/**
 * main() - Run the POC and reference set checks.
 *
 * Return: 0 when all passed, 1 when not.
 */
int main(void)
{
	for (u32 i = 0; i < sizeof(poc_cases) / sizeof(poc_cases[0]); i++)
		check_poc(&poc_cases[i]);
	check_found();
	check_past_range();
	check_sequence_start(FALSE);
	check_sequence_start(TRUE);
	printf("test_rps: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
