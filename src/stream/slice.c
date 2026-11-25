/* SPDX-License-Identifier: GPL-2.0 */
/*
 * slice.c - the slice segment header (slice.h), from 7.3.6.1 (the syntax),
 * 7.4.7.1 (its semantics and inferences), 7.3.6.2 and 7.4.7.2 (the list
 * modification and NumPicTotalCurr), 7.3.6.3 and 7.4.7.3 (the weights).
 *
 * The header is read in syntax order.  A dependent slice segment reads only
 * its first fields and copies the rest from the independent segment before
 * it; an independent one presets what a slice may leave out to what 7.4.7.1
 * infers, and reads the rest: the reference sets, the inter prediction's
 * values for a P or B slice, the QP and the filters.  The entry point
 * offsets and the header extension are passed over, and the header ends at
 * its byte_alignment(), where the slice data begins.
 *
 * As in ps.c, every value the standard bounds is read whole and checked
 * where it is read, before it is narrowed into its field (rp_bits_ue_at_most(),
 * rp_bits_se_in()): a damaged header is refused, and its picture dropped,
 * rather than handing the HEVC block a value that wrapped into range.  The
 * limits that depend on the parameter sets are taken from the active ones.
 */
#include <memory.h>

#include "nal.h"
#include "slice.h"

#define COLOUR_PLANE_ID_BITS 2         /* colour_plane_id, u(2) */
#define LOG2_WEIGHT_DENOM_MAX 7        /* luma_log2_weight_denom and ChromaLog2WeightDenom: 0 to 7 */
#define WP_OFFSET_HALF_RANGE 128       /* wpOffsetHalfRangeY and C without the range extension's high precision: 1 << 7 */
#define WEIGHT_DELTA_MIN -128          /* delta_luma_weight_lX, delta_chroma_weight_lX: -128 to 127 */
#define WEIGHT_DELTA_MAX 127
#define CHROMA_OFFSET_DELTA_SCALE 4    /* delta_chroma_offset_lX: -4 * wpOffsetHalfRangeC to 4 * wpOffsetHalfRangeC - 1 */
#define ENTRY_OFFSET_BITS_MAX 32       /* offset_len_minus1 + 1: 1 to 32 */
#define HEADER_EXTENSION_BYTES_MAX 256 /* slice_segment_header_extension_length: 0 to 256 */
#define POC_BITS 32                    /* a POC's bits: the long-term MSB cycles are bounded by them */
#define U32_BITS 32u                   /* the bits of a u32, for ceil_log2() */

/**
 * ceil_log2() - Ceil(Log2(x)): the bits of a u(v) index into x entries.
 * @x: the entries, at least 1
 *
 * The bits x - 1 needs, from one count of its leading zeros (a single
 * bfffo on the 68040); one entry needs none.
 *
 * Return: the bits, 0 to 32.
 */
static u32 ceil_log2(u32 x)
{
	return x <= 1 ? 0 : U32_BITS - (u32)__builtin_clz((unsigned int)(x - 1));
}

/**
 * clip_chroma_offset() - ChromaOffsetLX held to its range: Clip3(-wpOffsetHalfRangeC, wpOffsetHalfRangeC - 1, value).
 * @value: the offset as derived
 *
 * Return: the offset clipped, which an s8 holds.
 */
static s8 clip_chroma_offset(s32 value)
{
	if (value < -WP_OFFSET_HALF_RANGE)
		return -WP_OFFSET_HALF_RANGE;
	if (value > WP_OFFSET_HALF_RANGE - 1)
		return WP_OFFSET_HALF_RANGE - 1;
	return (s8)value;
}

/**
 * parse_chroma_weights() - One reference's chroma weights and offsets, the offsets derived (7.4.7.3).
 * @b:       the reader
 * @weights: the weights, the denominators read
 * @list:    0 or 1
 * @i:       the reference
 *
 * The derivation keeps the standard's formula with wpOffsetHalfRangeC of
 * 128: the high-precision offsets of the range extension are not Main.
 * Both values are bounded before they are used, so the formula cannot
 * overflow.
 *
 * Return: TRUE; FALSE for a value out of range.
 */
static BOOL parse_chroma_weights(struct rp_bits *b, struct rp_weights *weights, u32 list, u32 i)
{
	for (u32 j = 0; j < RP_CHROMA_COMPONENTS; j++)
	{
		s32 delta_weight, delta_offset;
		if (!rp_bits_se_in(b, WEIGHT_DELTA_MIN, WEIGHT_DELTA_MAX, &delta_weight) || /* delta_chroma_weight_lX */
		    !rp_bits_se_in(b, -CHROMA_OFFSET_DELTA_SCALE * WP_OFFSET_HALF_RANGE,
		                   CHROMA_OFFSET_DELTA_SCALE * WP_OFFSET_HALF_RANGE - 1, &delta_offset)) /* delta_chroma_offset_lX */
			return FALSE;
		s32 weight = (1 << weights->chroma_log2_denom) + delta_weight; /* ChromaWeightLX */
		s32 offset = WP_OFFSET_HALF_RANGE + delta_offset - ((WP_OFFSET_HALF_RANGE * weight) >> weights->chroma_log2_denom);
		weights->delta_chroma_weight[list][i][j] = (s8)delta_weight;
		weights->chroma_offset[list][i][j] = clip_chroma_offset(offset);
	}
	return TRUE;
}

/**
 * parse_luma_weights() - One reference's luma weight and offset.
 * @b:       the reader
 * @weights: the weights
 * @list:    0 or 1
 * @i:       the reference
 *
 * Return: TRUE; FALSE for a value out of range.
 */
static BOOL parse_luma_weights(struct rp_bits *b, struct rp_weights *weights, u32 list, u32 i)
{
	s32 delta_weight, offset;
	if (!rp_bits_se_in(b, WEIGHT_DELTA_MIN, WEIGHT_DELTA_MAX, &delta_weight) ||      /* delta_luma_weight_lX */
	    !rp_bits_se_in(b, -WP_OFFSET_HALF_RANGE, WP_OFFSET_HALF_RANGE - 1, &offset)) /* luma_offset_lX */
		return FALSE;
	weights->delta_luma_weight[list][i] = (s8)delta_weight;
	weights->luma_offset[list][i] = (s8)offset;
	return TRUE;
}

/**
 * parse_list_weights() - One list's part of pred_weight_table(): the flags, then each reference's weights.
 * @b:      the reader
 * @sh:     the header, its reference counts known
 * @list:   0 or 1
 * @chroma: ChromaArrayType is not 0: the chroma flags and weights are sent
 *
 * The flags are sent for every entry: no picture of this layer shares the
 * current one's POC in a Main stream.
 *
 * Return: TRUE; FALSE for a value out of range.
 */
static BOOL parse_list_weights(struct rp_bits *b, struct rp_slice_header *sh, u32 list, BOOL chroma)
{
	struct rp_weights *weights = &sh->weights;
	BOOL luma_flag[RP_REFS_MAX], chroma_flag[RP_REFS_MAX];
	u32 count = sh->num_ref_idx_active[list];
	for (u32 i = 0; i < count; i++)
		luma_flag[i] = rp_bits_flag(b); /* luma_weight_lX_flag */
	for (u32 i = 0; i < count; i++)
		chroma_flag[i] = chroma && rp_bits_flag(b); /* chroma_weight_lX_flag */
	for (u32 i = 0; i < count; i++)
	{
		if (luma_flag[i] && !parse_luma_weights(b, weights, list, i))
			return FALSE;
		if (chroma_flag[i] && !parse_chroma_weights(b, weights, list, i))
			return FALSE;
	}
	return TRUE;
}

/**
 * parse_weights() - pred_weight_table() (7.3.6.3).
 * @b:   the reader
 * @sh:  the header, its reference counts known
 * @sps: the active SPS, for ChromaArrayType
 *
 * Both denominators are 0 to 7: the chroma one is sent as a delta to the
 * luma one, and held to that range as well.
 *
 * Return: TRUE; FALSE for a value out of range.
 */
static BOOL parse_weights(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_sps *sps)
{
	struct rp_weights *weights = &sh->weights;
	BOOL chroma = sps->chroma_array_type != 0;
	u32 luma_denom;
	s32 chroma_delta = 0;
	memset(weights, 0, sizeof(*weights));
	if (!rp_bits_ue_at_most(b, LOG2_WEIGHT_DENOM_MAX, &luma_denom)) /* luma_log2_weight_denom */
		return FALSE;
	if (chroma && !rp_bits_se_in(b, -(s32)luma_denom, LOG2_WEIGHT_DENOM_MAX - (s32)luma_denom, &chroma_delta))
		return FALSE; /* delta_chroma_log2_weight_denom */
	weights->luma_log2_denom = (u8)luma_denom;
	weights->chroma_log2_denom = (u8)((s32)luma_denom + chroma_delta);
	if (!parse_list_weights(b, sh, 0, chroma))
		return FALSE;
	return sh->slice_type != V4L2_HEVC_SLICE_TYPE_B || parse_list_weights(b, sh, 1, chroma);
}

/**
 * parse_short_term_set() - The slice's short-term reference picture set: its own, or one of the SPS's.
 * @b:   the reader
 * @sh:  the header
 * @sps: the active SPS
 *
 * Return: TRUE; FALSE for a set that does not parse or an index past the SPS's sets.
 */
static BOOL parse_short_term_set(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_sps *sps)
{
	if (!rp_bits_flag(b)) /* short_term_ref_pic_set_sps_flag */
		return rp_st_rps_parse(b, &sh->st_rps, sps->num_st_rps, sps->st_rps, sps->num_st_rps) == RP_PS_OK;
	if (sps->num_st_rps == 0)
		return FALSE;
	u32 idx = 0; /* short_term_ref_pic_set_idx: not sent when the SPS has one set */
	if (sps->num_st_rps > 1)
		idx = rp_bits_u(b, ceil_log2(sps->num_st_rps));
	if (idx >= sps->num_st_rps)
		return FALSE;
	sh->st_rps = sps->st_rps[idx];
	return TRUE;
}

/**
 * parse_long_term_candidate() - A long-term entry taken from the SPS's candidates (lt_idx_sps).
 * @b:   the reader
 * @sh:  the header
 * @sps: the active SPS
 * @i:   the entry
 *
 * Return: TRUE; FALSE for an index past the SPS's candidates.
 */
static BOOL parse_long_term_candidate(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_sps *sps, u32 i)
{
	u32 lt_idx_sps = 0;
	if (sps->num_lt_sps > 1)
		lt_idx_sps = rp_bits_u(b, ceil_log2(sps->num_lt_sps));
	if (lt_idx_sps >= sps->num_lt_sps)
		return FALSE;
	sh->poc_lsb_lt[i] = sps->lt_ref_pic_poc_lsb_sps[lt_idx_sps];
	sh->used_by_curr_pic_lt[i] = sps->used_by_curr_pic_lt_sps[lt_idx_sps];
	return TRUE;
}

/**
 * parse_msb_cycle() - An entry's delta_poc_msb_cycle_lt, summed into DeltaPocMsbCycleLt (7-52).
 * @b:   the reader
 * @sh:  the header, the entries before this one read
 * @sps: the active SPS
 * @i:   the entry
 *
 * The cycle is cumulative within the SPS's candidates and within the
 * slice's own entries.  Times MaxPicOrderCntLsb it reaches back no further
 * than a POC's whole range, which bounds each value and the sum alike to
 * 2^(32 - log2_max_pic_order_cnt_lsb) (7.4.7.1).
 *
 * Return: TRUE; FALSE for a cycle out of range.
 */
static BOOL parse_msb_cycle(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_sps *sps, u32 i)
{
	u32 max = 1u << (POC_BITS - sps->log2_max_poc_lsb);
	u32 cycle = 0;
	sh->delta_poc_msb_present[i] = (u8)rp_bits_u(b, 1); /* delta_poc_msb_present_flag */
	if (sh->delta_poc_msb_present[i] && !rp_bits_ue_at_most(b, max, &cycle))
		return FALSE;
	if (i != 0 && i != sh->num_long_term_sps)
		cycle += (u32)sh->delta_poc_msb_cycle_lt[i - 1];
	if (cycle > max)
		return FALSE;
	sh->delta_poc_msb_cycle_lt[i] = (s32)cycle;
	return TRUE;
}

/**
 * parse_long_term_set() - The slice's long-term reference pictures: the SPS's candidates first, then its own.
 * @b:   the reader
 * @sh:  the header
 * @sps: the active SPS, with long-term pictures present
 *
 * Return: TRUE; FALSE for more entries than there may be, or an entry out of range.
 */
static BOOL parse_long_term_set(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_sps *sps)
{
	u32 from_sps = 0, own;
	if (sps->num_lt_sps > 0 && !rp_bits_ue_at_most(b, sps->num_lt_sps, &from_sps)) /* num_long_term_sps */
		return FALSE;
	if (!rp_bits_ue_at_most(b, RP_LT_MAX - from_sps, &own)) /* num_long_term_pics */
		return FALSE;
	sh->num_long_term_sps = (u8)from_sps;
	sh->num_long_term_pics = (u8)own;
	for (u32 i = 0; i < from_sps + own; i++)
	{
		if (i >= from_sps)
		{
			sh->poc_lsb_lt[i] = rp_bits_u(b, sps->log2_max_poc_lsb);
			sh->used_by_curr_pic_lt[i] = (u8)rp_bits_u(b, 1); /* used_by_curr_pic_lt_flag */
		}
		else if (!parse_long_term_candidate(b, sh, sps, i))
			return FALSE;
		if (!parse_msb_cycle(b, sh, sps, i))
			return FALSE;
	}
	return TRUE;
}

/**
 * parse_reference_sets() - What a picture that is not an IDR says of its references: POC, the sets, temporal MVP.
 * @b:   the reader
 * @sh:  the header
 * @sps: the active SPS
 *
 * Return: TRUE; FALSE for a set that does not parse.
 */
static BOOL parse_reference_sets(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_sps *sps)
{
	sh->pic_order_cnt_lsb = rp_bits_u(b, sps->log2_max_poc_lsb);
	if (!parse_short_term_set(b, sh, sps))
		return FALSE;
	if (sps->long_term_present && !parse_long_term_set(b, sh, sps))
		return FALSE;
	if (sps->temporal_mvp)
		sh->temporal_mvp = rp_bits_flag(b);
	return TRUE;
}

/**
 * num_pic_total_curr() - NumPicTotalCurr (7-55): the pictures the current one may reference.
 * @sh: the header, its reference sets read
 *
 * Return: the count.
 */
static u32 num_pic_total_curr(const struct rp_slice_header *sh)
{
	u32 total = 0;
	for (u32 i = 0; i < sh->st_rps.num_negative; i++)
		total += sh->st_rps.used_s0[i];
	for (u32 i = 0; i < sh->st_rps.num_positive; i++)
		total += sh->st_rps.used_s1[i];
	for (u32 i = 0; i < (u32)sh->num_long_term_sps + sh->num_long_term_pics; i++)
		total += sh->used_by_curr_pic_lt[i];
	return total;
}

/**
 * parse_list_modification() - ref_pic_lists_modification() (7.3.6.2).
 * @b:  the reader
 * @sh: the header, its reference counts and NumPicTotalCurr known
 *
 * Each list_entry_lX is a u(v) index into the current set; rp_ref_list()
 * holds it to the set when it builds the list.
 */
static void parse_list_modification(struct rp_bits *b, struct rp_slice_header *sh)
{
	u32 bits = ceil_log2(sh->num_pic_total_curr);
	u32 lists = sh->slice_type == V4L2_HEVC_SLICE_TYPE_B ? RP_REF_LISTS : 1;
	for (u32 list = 0; list < lists; list++)
	{
		sh->list_modification[list] = rp_bits_flag(b);
		if (sh->list_modification[list])
			for (u32 i = 0; i < sh->num_ref_idx_active[list]; i++)
				sh->list_entry[list][i] = (u8)rp_bits_u(b, bits);
	}
}

/**
 * parse_collocated() - Where the collocated picture is: the list, and the index when the list has a choice.
 * @b:  the reader
 * @sh: the header, temporal MVP on, its reference counts known
 *
 * Return: TRUE; FALSE for an index past its list.
 */
static BOOL parse_collocated(struct rp_bits *b, struct rp_slice_header *sh)
{
	if (sh->slice_type == V4L2_HEVC_SLICE_TYPE_B)
		sh->collocated_from_l0 = rp_bits_flag(b);
	u32 count = sh->num_ref_idx_active[sh->collocated_from_l0 ? 0 : 1];
	u32 idx = 0;
	if (count > 1 && !rp_bits_ue_at_most(b, count - 1, &idx)) /* collocated_ref_idx */
		return FALSE;
	sh->collocated_ref_idx = (u8)idx;
	return TRUE;
}

/**
 * parse_reference_counts() - The active references of each list: the PPS's defaults, or the slice's own.
 * @b:   the reader
 * @sh:  the header, a P or B slice's
 * @pps: the active PPS, its defaults held to 15 when it was parsed
 *
 * Return: TRUE; FALSE for a count over 15.
 */
static BOOL parse_reference_counts(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_pps *pps)
{
	BOOL is_b = sh->slice_type == V4L2_HEVC_SLICE_TYPE_B;
	u32 l0_minus1 = pps->num_ref_idx_l0_default - 1u, l1_minus1 = pps->num_ref_idx_l1_default - 1u;
	if (rp_bits_flag(b)) /* num_ref_idx_active_override_flag */
	{
		if (!rp_bits_ue_at_most(b, RP_ACTIVE_REFS_MAX - 1, &l0_minus1) ||
		    (is_b && !rp_bits_ue_at_most(b, RP_ACTIVE_REFS_MAX - 1, &l1_minus1)))
			return FALSE;
	}
	sh->num_ref_idx_active[0] = (u8)(l0_minus1 + 1);
	sh->num_ref_idx_active[1] = is_b ? (u8)(l1_minus1 + 1) : 0;
	return TRUE;
}

/**
 * parse_inter() - What only a P or B slice sends: the reference counts, the lists, the prediction's parameters.
 * @b:   the reader
 * @sh:  the header, a P or B slice's
 * @sps: the active SPS
 * @pps: the active PPS
 *
 * Return: TRUE; FALSE for a value out of range.
 */
static BOOL parse_inter(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_sps *sps, const struct rp_pps *pps)
{
	BOOL is_b = sh->slice_type == V4L2_HEVC_SLICE_TYPE_B;
	if (!parse_reference_counts(b, sh, pps))
		return FALSE;
	if (pps->lists_modification_present && sh->num_pic_total_curr > 1)
		parse_list_modification(b, sh);
	if (is_b)
		sh->mvd_l1_zero = rp_bits_flag(b);
	if (pps->cabac_init_present)
		sh->cabac_init = rp_bits_flag(b);
	if (sh->temporal_mvp && !parse_collocated(b, sh))
		return FALSE;
	sh->weighted = is_b ? pps->weighted_bipred : pps->weighted_pred;
	if (sh->weighted && !parse_weights(b, sh, sps))
		return FALSE;
	u32 five_minus_max_num_merge_cand;
	if (!rp_bits_ue_at_most(b, RP_MERGE_CANDIDATES_MAX - 1, &five_minus_max_num_merge_cand))
		return FALSE;
	sh->max_num_merge_cand = (u8)(RP_MERGE_CANDIDATES_MAX - five_minus_max_num_merge_cand);
	return TRUE;
}

/**
 * chroma_offset_in() - A slice's chroma QP offset, held to -12 to 12 alone and added to the PPS's.
 * @b:          the reader
 * @pps_offset: the PPS's offset for the component, itself held to -12 to 12
 * @value:      out: the slice's offset
 *
 * Return: TRUE when both hold.
 */
static BOOL chroma_offset_in(struct rp_bits *b, s32 pps_offset, s32 *value)
{
	s32 low = -RP_CHROMA_QP_OFFSET_MAX - (pps_offset < 0 ? pps_offset : 0);
	s32 high = RP_CHROMA_QP_OFFSET_MAX - (pps_offset > 0 ? pps_offset : 0);
	return rp_bits_se_in(b, low, high, value);
}

/**
 * parse_qp() - The slice's QP delta and chroma QP offsets.
 * @b:   the reader
 * @sh:  the header
 * @sps: the active SPS, for QpBdOffsetY
 * @pps: the active PPS
 *
 * SliceQpY, 26 + init_qp_minus26 + slice_qp_delta, is -QpBdOffsetY to 51;
 * a chroma offset is -12 to 12, alone and added to the PPS's.
 *
 * Return: TRUE; FALSE for a value out of range.
 */
static BOOL parse_qp(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_sps *sps, const struct rp_pps *pps)
{
	s32 base = RP_QP_INIT_BASE + pps->init_qp_minus26;
	s32 qp_delta, cb_offset = 0, cr_offset = 0;
	if (!rp_bits_se_in(b, -rp_qp_bd_offset(sps->bit_depth_luma) - base, RP_QP_MAX - base, &qp_delta)) /* slice_qp_delta */
		return FALSE;
	if (pps->slice_chroma_qp_offsets_present &&
	    (!chroma_offset_in(b, pps->cb_qp_offset, &cb_offset) || !chroma_offset_in(b, pps->cr_qp_offset, &cr_offset)))
		return FALSE; /* slice_cb_qp_offset, slice_cr_qp_offset */
	sh->slice_qp_delta = (s8)qp_delta;
	sh->cb_qp_offset = (s8)cb_offset;
	sh->cr_qp_offset = (s8)cr_offset;
	return TRUE;
}

/**
 * parse_filters() - The slice's deblocking and loop filter overrides.
 * @b:   the reader
 * @sh:  the header, the PPS's filter values preset
 * @pps: the active PPS
 *
 * Return: TRUE; FALSE for a deblocking offset out of range.
 */
static BOOL parse_filters(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_pps *pps)
{
	if (pps->deblocking_filter_override_enabled && rp_bits_flag(b)) /* deblocking_filter_override_flag */
	{
		sh->deblocking_disabled = rp_bits_flag(b);
		if (!sh->deblocking_disabled)
		{
			s32 beta_offset_div2, tc_offset_div2;
			if (!rp_bits_se_in(b, -RP_DEBLOCKING_OFFSET_MAX, RP_DEBLOCKING_OFFSET_MAX, &beta_offset_div2) ||
			    !rp_bits_se_in(b, -RP_DEBLOCKING_OFFSET_MAX, RP_DEBLOCKING_OFFSET_MAX, &tc_offset_div2))
				return FALSE;
			sh->beta_offset_div2 = (s8)beta_offset_div2;
			sh->tc_offset_div2 = (s8)tc_offset_div2;
		}
	}
	if (pps->loop_filter_across_slices && (sh->sao_luma || sh->sao_chroma || !sh->deblocking_disabled))
		sh->loop_filter_across_slices = rp_bits_flag(b);
	return TRUE;
}

/**
 * parse_independent() - What an independent slice segment sends itself (a dependent one copies it).
 * @b:   the reader, after slice_segment_address
 * @sh:  the header, cleared but for the unit's fields
 * @sps: the active SPS
 * @pps: the active PPS
 *
 * The values a slice may leave out are preset to what 7.4.7.1 infers.
 *
 * Return: TRUE; FALSE for a value out of range or a set that does not parse.
 */
static BOOL parse_independent(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_sps *sps, const struct rp_pps *pps)
{
	rp_bits_skip(b, pps->num_extra_slice_header_bits); /* slice_reserved_flag */
	u32 slice_type;
	if (!rp_bits_ue_at_most(b, V4L2_HEVC_SLICE_TYPE_I, &slice_type))
		return FALSE;
	sh->slice_type = (u8)slice_type;
	sh->pic_output = pps->output_flag_present ? rp_bits_flag(b) : TRUE;
	if (sps->separate_colour_plane)
		sh->colour_plane_id = (u8)rp_bits_u(b, COLOUR_PLANE_ID_BITS);
	sh->collocated_from_l0 = TRUE;
	sh->deblocking_disabled = pps->deblocking_filter_disabled;
	sh->beta_offset_div2 = pps->beta_offset_div2;
	sh->tc_offset_div2 = pps->tc_offset_div2;
	sh->loop_filter_across_slices = pps->loop_filter_across_slices;
	if (!nal_is_idr(sh->nal_type) && !parse_reference_sets(b, sh, sps))
		return FALSE;
	if (sps->sao)
	{
		sh->sao_luma = rp_bits_flag(b);
		if (sps->chroma_array_type != 0)
			sh->sao_chroma = rp_bits_flag(b);
	}
	sh->num_pic_total_curr = num_pic_total_curr(sh);
	if (sh->slice_type != V4L2_HEVC_SLICE_TYPE_I && !parse_inter(b, sh, sps, pps))
		return FALSE;
	return parse_qp(b, sh, sps, pps) && parse_filters(b, sh, pps);
}

/**
 * entry_points_max() - The most entry points a slice may have (7.4.7.1): one per tile or wavefront row past its first.
 * @sps: the active SPS
 * @pps: the active PPS, with tiles or wavefronts
 *
 * Return: the tiles less one with tiles alone, the CTB rows less one with
 * wavefronts alone, the tile columns times the CTB rows less one with both.
 */
static u32 entry_points_max(const struct rp_sps *sps, const struct rp_pps *pps)
{
	if (!pps->entropy_coding_sync)
		return (u32)pps->num_tile_columns * pps->num_tile_rows - 1;
	if (!pps->tiles_enabled)
		return sps->ctb_height - 1;
	return (u32)pps->num_tile_columns * sps->ctb_height - 1;
}

/**
 * skip_entry_points() - The entry point offsets counted and passed over: the HEVC block finds them itself.
 * @b:   the reader
 * @sh:  the header
 * @sps: the active SPS
 * @pps: the active PPS
 *
 * Return: TRUE; FALSE for a count or an offset length out of range.
 */
static BOOL skip_entry_points(struct rp_bits *b, struct rp_slice_header *sh, const struct rp_sps *sps, const struct rp_pps *pps)
{
	sh->num_entry_point_offsets = 0;
	if (!pps->tiles_enabled && !pps->entropy_coding_sync)
		return TRUE;
	u32 count, offset_len_minus1;
	if (!rp_bits_ue_at_most(b, entry_points_max(sps, pps), &count)) /* num_entry_point_offsets */
		return FALSE;
	sh->num_entry_point_offsets = count;
	if (count == 0)
		return TRUE;
	if (!rp_bits_ue_at_most(b, ENTRY_OFFSET_BITS_MAX - 1, &offset_len_minus1))
		return FALSE;
	for (u32 i = 0; i < count; i++)
		rp_bits_skip(b, offset_len_minus1 + 1); /* entry_point_offset_minus1 */
	return TRUE;
}

/**
 * skip_header_extension() - The slice segment header extension, passed over when the PPS announces one.
 * @b:   the reader
 * @pps: the active PPS
 *
 * Return: TRUE; FALSE for a length over 256 bytes.
 */
static BOOL skip_header_extension(struct rp_bits *b, const struct rp_pps *pps)
{
	if (!pps->slice_segment_header_extension_present)
		return TRUE;
	u32 bytes;
	if (!rp_bits_ue_at_most(b, HEADER_EXTENSION_BYTES_MAX, &bytes)) /* slice_segment_header_extension_length */
		return FALSE;
	rp_bits_skip(b, bytes * RP_BITS_PER_BYTE); /* slice_segment_header_extension_data_byte */
	return TRUE;
}

/**
 * active_pps() - The PPS a slice names, when it and its SPS are there.
 * @pps_id:    slice_pic_parameter_set_id
 * @sps_table: the SPSs seen, by id
 * @pps_table: the PPSs seen, by id
 *
 * Return: the PPS, resolved against its SPS; NULL when either is missing.
 */
static const struct rp_pps *active_pps(u32 pps_id, const struct rp_sps *sps_table, const struct rp_pps *pps_table)
{
	if (pps_id >= RP_PPS_MAX || !pps_table[pps_id].valid)
		return NULL;
	const struct rp_pps *pps = &pps_table[pps_id];
	if (!sps_table[pps->sps_id].valid || !pps->resolved)
		return NULL;
	return pps;
}

/**
 * parse_segment() - What every slice segment sends first: whether it starts its picture, its PPS, whether it is dependent, its address.
 * @b:           the reader, after the NAL unit header
 * @sh:          filled in: cleared, or the independent segment's values for a dependent one, then these fields
 * @nal_type:    from the unit's header
 * @temporal_id: from the unit's header
 * @sps_table:   the SPSs seen, by id
 * @pps_table:   the PPSs seen, by id
 * @independent: the picture's last independent slice header; NULL at a picture's start
 *
 * Return: the active PPS; NULL when it is not there, the address is past
 * the picture, or a dependent segment has no independent one before it.
 */
static const struct rp_pps *parse_segment(struct rp_bits *b, struct rp_slice_header *sh, u32 nal_type, u32 temporal_id,
                                          const struct rp_sps *sps_table, const struct rp_pps *pps_table,
                                          const struct rp_slice_header *independent)
{
	BOOL first = rp_bits_flag(b);                                       /* first_slice_segment_in_pic_flag */
	BOOL no_output_of_prior = nal_is_irap(nal_type) && rp_bits_flag(b); /* no_output_of_prior_pics_flag */
	u32 pps_id = rp_bits_ue(b);                                         /* slice_pic_parameter_set_id */
	const struct rp_pps *pps = active_pps(pps_id, sps_table, pps_table);
	if (pps == NULL)
		return NULL;
	const struct rp_sps *sps = &sps_table[pps->sps_id];
	u32 ctbs = sps->ctb_width * sps->ctb_height;
	BOOL dependent = FALSE;
	u32 address = 0;
	if (!first)
	{
		dependent = pps->dependent_slice_segments_enabled && rp_bits_flag(b); /* dependent_slice_segment_flag */
		address = rp_bits_u(b, ceil_log2(ctbs));                              /* slice_segment_address */
	}
	if (address >= ctbs || (dependent && independent == NULL))
		return NULL;
	if (dependent)
		*sh = *independent; /* 7.4.7.1: the independent segment's values */
	else
		memset(sh, 0, sizeof(*sh));
	sh->nal_type = nal_type;
	sh->temporal_id = temporal_id;
	sh->no_output_of_prior_pics = no_output_of_prior;
	sh->pps_id = (u8)pps_id;
	sh->dependent_slice_segment = dependent;
	sh->slice_segment_address = address;
	return pps;
}

/**
 * rp_slice_parse() - A slice segment header from its NAL unit.
 * @sh:          filled in
 * @nal:         the unit, header included
 * @len:         its bytes
 * @nal_type:    from the unit's header
 * @temporal_id: from the unit's header
 * @sps_table:   the SPSs seen, by id
 * @pps_table:   the PPSs seen, by id
 * @independent: the picture's last independent slice header, for a dependent segment; NULL at a picture's start
 *
 * Return: TRUE; FALSE when the header does not parse, holds a value out of
 * range, or names a parameter set that is not there.
 */
BOOL rp_slice_parse(struct rp_slice_header *sh, const u8 *nal, u32 len, u32 nal_type, u32 temporal_id,
                    const struct rp_sps *sps_table, const struct rp_pps *pps_table, const struct rp_slice_header *independent)
{
	struct rp_bits b;

	rp_bits_init(&b, nal, len, NAL_HEADER_BYTES);
	const struct rp_pps *pps = parse_segment(&b, sh, nal_type, temporal_id, sps_table, pps_table, independent);
	if (pps == NULL)
		return FALSE;
	const struct rp_sps *sps = &sps_table[pps->sps_id];
	if (!sh->dependent_slice_segment && !parse_independent(&b, sh, sps, pps))
		return FALSE;
	if (!skip_entry_points(&b, sh, sps, pps) || !skip_header_extension(&b, pps))
		return FALSE;
	/* byte_alignment() (7.3.2.12): the bit equal to one, then zeros to the byte */
	if (rp_bits_u(&b, 1) != 1)
		return FALSE;
	rp_bits_byte_align(&b);
	if (b.overrun)
		return FALSE;
	sh->data_byte_offset = rp_bits_byte_offset(&b);
	return TRUE;
}
