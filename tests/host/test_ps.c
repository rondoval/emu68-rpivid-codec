/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_ps - the parameter set and slice header parsers on hand-made units (developer-guide.md section 4.3).
 *
 * A 1280x720 Main SPS and a PPS for it, written element by element with the
 * bit writer, parse with every value as written: the block sizes, the PCM
 * values, a short-term set predicted from another, the scaling lists placed
 * in raster order by the up-right diagonal scan (worked out here from 6.5.3,
 * so that ps.c's tables of it are checked), a 32x32 list predicted from the
 * one before, the tiles resolved against the SPS.  Then each case changes
 * one value to one the standard forbids and the set must be refused as a
 * stream error, with its reason: one past each bound, and the values that a
 * cast to the field's width would once have wrapped back into range.  A P
 * slice for that SPS and PPS parses the same way, with long-term pictures,
 * weights, entry points and a header extension, and each of its cases must
 * be refused (rp_slice_parse() keeps no reason).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bit_writer.h"
#include "host_check.h"
#include "nal.h"
#include "ps.h"
#include "slice.h"

#define UNIT_BYTES 4096        /* room for one hand-made unit */
#define SCALING_SIZES 4        /* sizeId: 4x4, 8x8, 16x16, 32x32 */
#define SCALING_MATRICES 6     /* matrixId */
#define SCALING_32X32 3        /* the sizeId of 32x32, whose lists are matrixId 0 and 3 */
#define SCALING_COEFS_4X4 16   /* a 4x4 list's coefficients */
#define SCALING_COEFS 64       /* every larger list's */
#define SCALING_DC 16          /* the DC value every 16x16 and 32x32 list is written with */
#define PREDICTION_CODED -1    /* sps_fields.scaling_32x32_pred: the list is coded, not predicted */

/* The values test_ps writes into its SPS, by syntax element, wide enough for any value a stream may hold */
struct sps_fields
{
	int64_t chroma_format_idc;       /* chroma_format_idc */
	int64_t width;                   /* pic_width_in_luma_samples */
	int64_t height;                  /* pic_height_in_luma_samples */
	int64_t window_left;             /* conf_win_left_offset: the window is always sent */
	int64_t window_right;            /* conf_win_right_offset */
	int64_t window_top;              /* conf_win_top_offset */
	int64_t window_bottom;           /* conf_win_bottom_offset */
	int64_t depth_luma_minus8;       /* bit_depth_luma_minus8 */
	int64_t depth_chroma_minus8;     /* bit_depth_chroma_minus8 */
	int64_t poc_lsb_minus4;          /* log2_max_pic_order_cnt_lsb_minus4 */
	int64_t dpb_minus1;              /* sps_max_dec_pic_buffering_minus1 */
	int64_t reorder;                 /* sps_max_num_reorder_pics */
	int64_t latency_plus1;           /* sps_max_latency_increase_plus1 */
	int64_t min_cb_minus3;           /* log2_min_luma_coding_block_size_minus3 */
	int64_t ctb_diff;                /* log2_diff_max_min_luma_coding_block_size */
	int64_t min_tb_minus2;           /* log2_min_luma_transform_block_size_minus2 */
	int64_t tb_diff;                 /* log2_diff_max_min_luma_transform_block_size */
	int64_t depth_inter;             /* max_transform_hierarchy_depth_inter */
	int64_t depth_intra;             /* max_transform_hierarchy_depth_intra */
	int64_t scaling_dc_minus8;       /* scaling_list_dc_coef_minus8 of every 16x16 and 32x32 list */
	int64_t scaling_32x32_pred;      /* the second 32x32 list's scaling_list_pred_matrix_id_delta; PREDICTION_CODED: coded */
	int64_t pcm_depth_luma_minus1;   /* pcm_sample_bit_depth_luma_minus1 */
	int64_t pcm_depth_chroma_minus1; /* pcm_sample_bit_depth_chroma_minus1 */
	int64_t pcm_min_minus3;          /* log2_min_pcm_luma_coding_block_size_minus3 */
	int64_t pcm_diff;                /* log2_diff_max_min_pcm_luma_coding_block_size */
	int64_t num_st_rps;              /* num_short_term_ref_pic_sets; two sets are written, whatever it says */
	int64_t st_delta_minus1;         /* set 0, sent in full: delta_poc_s0_minus1 of its one picture */
	int64_t st_abs_delta_rps_minus1; /* set 1, predicted from set 0: abs_delta_rps_minus1, the sign negative */
};

/* A valid SPS: 8 bits, coding blocks 8 to 32, transform blocks 4 to 32, PCM blocks 8 to 16, scaling lists sent */
static const struct sps_fields sps_valid = {
	.chroma_format_idc = 1,
	.width = 1280,
	.height = 720,
	.poc_lsb_minus4 = 4,
	.dpb_minus1 = 4,
	.reorder = 2,
	.ctb_diff = 2,
	.tb_diff = 3,
	.depth_inter = 1,
	.depth_intra = 1,
	.scaling_dc_minus8 = SCALING_DC - 8,
	.scaling_32x32_pred = PREDICTION_CODED,
	.pcm_depth_luma_minus1 = 7,
	.pcm_depth_chroma_minus1 = 7,
	.pcm_diff = 1,
	.num_st_rps = 2,
};

/* The values test_ps writes into its PPS: cu_qp_delta, a 2x2 tile grid not uniformly spaced and the deblocking control on */
struct pps_fields
{
	int64_t ref_l0_minus1;       /* num_ref_idx_l0_default_active_minus1 */
	int64_t ref_l1_minus1;       /* num_ref_idx_l1_default_active_minus1 */
	int64_t init_qp_minus26;     /* init_qp_minus26 */
	int64_t cu_qp_delta_depth;   /* diff_cu_qp_delta_depth */
	int64_t cb_offset;           /* pps_cb_qp_offset */
	int64_t cr_offset;           /* pps_cr_qp_offset */
	int64_t tile_columns_minus1; /* num_tile_columns_minus1 */
	int64_t tile_rows_minus1;    /* num_tile_rows_minus1 */
	int64_t column_width_minus1; /* column_width_minus1 of every column but the last */
	int64_t row_height_minus1;   /* row_height_minus1 of every row but the last */
	int64_t beta_offset_div2;    /* pps_beta_offset_div2 */
	int64_t tc_offset_div2;      /* pps_tc_offset_div2 */
	int64_t merge_minus2;        /* log2_parallel_merge_level_minus2 */
};

/* A valid PPS for sps_valid: two tile columns of 10 and 30 CTBs, two rows of 10 and 13 */
static const struct pps_fields pps_valid = {
	.cu_qp_delta_depth = 1,
	.cb_offset = -2,
	.cr_offset = 2,
	.tile_columns_minus1 = 1,
	.tile_rows_minus1 = 1,
	.column_width_minus1 = 9,
	.row_height_minus1 = 9,
	.beta_offset_div2 = 1,
	.tc_offset_div2 = -1,
};

/* A case: one value changed to one the standard forbids, and the reason ps.c must refuse the set with */
struct refusal_case
{
	const char *name;   /* what the value breaks */
	size_t field;       /* the value's offset in struct sps_fields or struct pps_fields */
	int64_t value;      /* what it is set to */
	const char *reason; /* the refusal's reason, as ps.c words it */
};

#define SPS_CASE(name, field, value, reason) {name, offsetof(struct sps_fields, field), value, reason}
#define PPS_CASE(name, field, value, reason) {name, offsetof(struct pps_fields, field), value, reason}

#define WINDOW "a conformance window that leaves no picture"
#define CTB "a CTB not 16 to 64"
#define TRANSFORM "a transform block over 32, over the CTB, or not under the coding block"
#define HIERARCHY "a transform hierarchy depth over the CTB to transform block range"
#define PCM_BLOCK "a PCM block under the coding block, or over 32 or the CTB"

/* The SPS cases, refused by rp_sps_parse() */
static const struct refusal_case sps_cases[] = {
	SPS_CASE("chroma_format_idc 4", chroma_format_idc, 4, "chroma_format_idc over 3"),
	SPS_CASE("chroma_format_idc 257, 4:2:0 once cut to a byte", chroma_format_idc, 257, "chroma_format_idc over 3"),
	SPS_CASE("a window offset of half the width", window_left, 640, WINDOW),
	SPS_CASE("a window offset of half the height", window_top, 360, WINDOW),
	SPS_CASE("a window offset that would wrap the sum", window_right, 0x80000000LL, WINDOW),
	SPS_CASE("bit_depth_luma_minus8 9", depth_luma_minus8, 9, "a bit depth over 16"),
	SPS_CASE("bit_depth_luma_minus8 256, 8 bits once cut to a byte", depth_luma_minus8, 256, "a bit depth over 16"),
	SPS_CASE("bit_depth_chroma_minus8 9", depth_chroma_minus8, 9, "a bit depth over 16"),
	SPS_CASE("log2_max_pic_order_cnt_lsb_minus4 13", poc_lsb_minus4, 13, "log2_max_pic_order_cnt_lsb over 16"),
	SPS_CASE("log2_max_pic_order_cnt_lsb_minus4 256", poc_lsb_minus4, 256, "log2_max_pic_order_cnt_lsb over 16"),
	SPS_CASE("sps_max_dec_pic_buffering_minus1 16", dpb_minus1, 16, "sps_max_dec_pic_buffering over 16"),
	SPS_CASE("sps_max_num_reorder_pics over the buffering", reorder, 5, "reorder pictures over the buffering"),
	SPS_CASE("a coding block of 128", min_cb_minus3, 4, CTB),
	SPS_CASE("log2_min_luma_coding_block_size_minus3 253, 0 once cut to a byte", min_cb_minus3, 253, CTB),
	SPS_CASE("a CTB of 128", ctb_diff, 4, CTB),
	SPS_CASE("a CTB of 8", ctb_diff, 0, CTB),
	SPS_CASE("a smallest transform block as large as the coding block", min_tb_minus2, 1, TRANSFORM),
	SPS_CASE("log2_min_luma_transform_block_size_minus2 256", min_tb_minus2, 256, TRANSFORM),
	SPS_CASE("a transform block of 64", tb_diff, 4, TRANSFORM),
	SPS_CASE("max_transform_hierarchy_depth_inter 4", depth_inter, 4, HIERARCHY),
	SPS_CASE("max_transform_hierarchy_depth_intra 256, 0 once cut to a byte", depth_intra, 256, HIERARCHY),
	SPS_CASE("PCM luma samples deeper than the picture's", pcm_depth_luma_minus1, 8, "a PCM sample deeper than the picture's"),
	SPS_CASE("PCM chroma samples deeper than the picture's", pcm_depth_chroma_minus1, 8, "a PCM sample deeper than the picture's"),
	SPS_CASE("a PCM block of 64", pcm_diff, 3, PCM_BLOCK),
	SPS_CASE("a smallest PCM block of 64", pcm_min_minus3, 3, PCM_BLOCK),
	SPS_CASE("num_short_term_ref_pic_sets 65", num_st_rps, 65, "over 64 short-term sets"),
	SPS_CASE("delta_poc_s0_minus1 32768", st_delta_minus1, 32768, "a short-term reference picture set"),
	SPS_CASE("abs_delta_rps_minus1 32768", st_abs_delta_rps_minus1, 32768, "a short-term reference picture set"),
	SPS_CASE("a DC value of 256", scaling_dc_minus8, 248, "scaling list data"),
	SPS_CASE("a DC value of 0", scaling_dc_minus8, -8, "scaling list data"),
	SPS_CASE("a 32x32 list predicted from before the first", scaling_32x32_pred, 2, "scaling list data"),
	SPS_CASE("a prediction delta that times 3 would wrap to 2", scaling_32x32_pred, 0x55555556LL, "scaling list data"),
};

#define REFS "num_ref_idx_default_active over 15"
#define QP_DEPTH "diff_cu_qp_delta_depth over the CTB to coding block range"

/* The PPS cases, refused by rp_pps_parse() */
static const struct refusal_case pps_cases[] = {
	PPS_CASE("num_ref_idx_l0_default_active_minus1 15", ref_l0_minus1, 15, REFS),
	PPS_CASE("num_ref_idx_l1_default_active_minus1 255, no reference once cut to a byte", ref_l1_minus1, 255, REFS),
	PPS_CASE("init_qp_minus26 26", init_qp_minus26, 26, "init_qp_minus26 out of range"),
	PPS_CASE("init_qp_minus26 -75", init_qp_minus26, -75, "init_qp_minus26 out of range"),
	PPS_CASE("diff_cu_qp_delta_depth 4", cu_qp_delta_depth, 4, QP_DEPTH),
	PPS_CASE("diff_cu_qp_delta_depth 256, 0 once cut to a byte", cu_qp_delta_depth, 256, QP_DEPTH),
	PPS_CASE("pps_cb_qp_offset 13", cb_offset, 13, "a chroma QP offset over 12"),
	PPS_CASE("pps_cr_qp_offset -13", cr_offset, -13, "a chroma QP offset over 12"),
	PPS_CASE("21 tile columns", tile_columns_minus1, 20, "over 20 tile columns or 22 rows"),
	PPS_CASE("23 tile rows", tile_rows_minus1, 22, "over 20 tile columns or 22 rows"),
	PPS_CASE("a tile column of 4096 CTBs", column_width_minus1, 4095, "a tile larger than any picture"),
	PPS_CASE("column_width_minus1 65535, 0 once cut to 16 bits", column_width_minus1, 65535, "a tile larger than any picture"),
	PPS_CASE("pps_beta_offset_div2 7", beta_offset_div2, 7, "a deblocking offset over 6"),
	PPS_CASE("pps_tc_offset_div2 -7", tc_offset_div2, -7, "a deblocking offset over 6"),
	PPS_CASE("log2_parallel_merge_level_minus2 5", merge_minus2, 5, "log2_parallel_merge_level over 6"),
	PPS_CASE("log2_parallel_merge_level_minus2 254, 0 once cut to a byte", merge_minus2, 254, "log2_parallel_merge_level over 6"),
};

/* The PPS cases that parse, and are refused by rp_pps_resolve() against sps_valid */
static const struct refusal_case resolve_cases[] = {
	PPS_CASE("a first tile column as wide as the picture", column_width_minus1, 39, "tile columns wider than the picture"),
	PPS_CASE("a first tile row as tall as the picture", row_height_minus1, 22, "tile rows taller than the picture"),
	PPS_CASE("diff_cu_qp_delta_depth 3 with a CTB of 32 over coding blocks of 8", cu_qp_delta_depth, 3, QP_DEPTH),
	PPS_CASE("init_qp_minus26 -27 at 8 bits", init_qp_minus26, -27, "init_qp_minus26 under the bit depth's range"),
	PPS_CASE("a merge level of 64 with a CTB of 32", merge_minus2, 4, "log2_parallel_merge_level over the CTB"),
};

/* The values test_ps writes into its slice: the first of a TRAIL_R picture, P, for pps_valid and sps_valid */
struct slice_fields
{
	int64_t num_long_term_sps;  /* num_long_term_sps: the SPS has one candidate */
	int64_t num_long_term_pics; /* num_long_term_pics */
	int64_t msb_cycle;          /* delta_poc_msb_cycle_lt of every long-term entry, delta_poc_msb_present_flag set */
	int64_t refs_minus1;        /* num_ref_idx_l0_active_minus1, num_ref_idx_active_override_flag set */
	int64_t collocated_ref_idx; /* collocated_ref_idx */
	int64_t luma_denom;         /* luma_log2_weight_denom */
	int64_t chroma_denom_delta; /* delta_chroma_log2_weight_denom */
	int64_t luma_weight;        /* delta_luma_weight_l0 of every reference, luma_weight_l0_flag set */
	int64_t luma_offset;        /* luma_offset_l0 */
	int64_t chroma_weight;      /* delta_chroma_weight_l0 of both components, chroma_weight_l0_flag set */
	int64_t chroma_offset;      /* delta_chroma_offset_l0 */
	int64_t merge;              /* five_minus_max_num_merge_cand */
	int64_t qp_delta;           /* slice_qp_delta */
	int64_t cb_offset;          /* slice_cb_qp_offset */
	int64_t cr_offset;          /* slice_cr_qp_offset */
	int64_t beta_offset_div2;   /* slice_beta_offset_div2, deblocking_filter_override_flag set */
	int64_t tc_offset_div2;     /* slice_tc_offset_div2 */
	int64_t entry_points;       /* num_entry_point_offsets */
	int64_t offset_len_minus1;  /* offset_len_minus1 */
	int64_t extension_bytes;    /* slice_segment_header_extension_length */
};

/* A valid slice: two references, three long-term entries (one the SPS's), weights, three entry points (2x2 tiles) */
static const struct slice_fields slice_valid = {
	.num_long_term_sps = 1,
	.num_long_term_pics = 2,
	.msb_cycle = 1,
	.refs_minus1 = 1,
	.collocated_ref_idx = 1,
	.luma_denom = 6,
	.merge = 0,
	.entry_points = 3,
	.offset_len_minus1 = 7,
};

/* A slice case: one value changed to one the standard forbids; rp_slice_parse() must refuse it */
struct slice_case
{
	const char *name; /* what the value breaks */
	size_t field;     /* the value's offset in struct slice_fields */
	int64_t value;    /* what it is set to */
};

#define SLICE_CASE(name, field, value) {name, offsetof(struct slice_fields, field), value}

/* The slice cases; the SPS holds the POC in 8 bits and one long-term candidate, the PPS cb -2 and cr 2 */
static const struct slice_case slice_cases[] = {
	SLICE_CASE("num_long_term_sps 2, past the SPS's one candidate", num_long_term_sps, 2),
	SLICE_CASE("num_long_term_sps 257, 1 once cut to a byte", num_long_term_sps, 257),
	SLICE_CASE("33 long-term entries", num_long_term_pics, 32),
	SLICE_CASE("num_long_term_pics 256, 0 once cut to a byte", num_long_term_pics, 256),
	SLICE_CASE("delta_poc_msb_cycle_lt 2^24 + 1, past 2^(32 - 8)", msb_cycle, 16777217),
	SLICE_CASE("two own MSB cycles that sum past 2^24", msb_cycle, 8388609),
	SLICE_CASE("num_ref_idx_l0_active_minus1 15", refs_minus1, 15),
	SLICE_CASE("num_ref_idx_l0_active_minus1 255, no reference once cut to a byte", refs_minus1, 255),
	SLICE_CASE("collocated_ref_idx 2 of two references", collocated_ref_idx, 2),
	SLICE_CASE("collocated_ref_idx 256, 0 once cut to a byte", collocated_ref_idx, 256),
	SLICE_CASE("luma_log2_weight_denom 8", luma_denom, 8),
	SLICE_CASE("luma_log2_weight_denom 256, 0 once cut to a byte", luma_denom, 256),
	SLICE_CASE("a chroma denominator of 8", chroma_denom_delta, 2),
	SLICE_CASE("a chroma denominator of -1", chroma_denom_delta, -7),
	SLICE_CASE("delta_luma_weight_l0 128", luma_weight, 128),
	SLICE_CASE("delta_luma_weight_l0 -129", luma_weight, -129),
	SLICE_CASE("luma_offset_l0 128", luma_offset, 128),
	SLICE_CASE("luma_offset_l0 -129", luma_offset, -129),
	SLICE_CASE("delta_chroma_weight_l0 -129", chroma_weight, -129),
	SLICE_CASE("delta_chroma_offset_l0 512", chroma_offset, 512),
	SLICE_CASE("delta_chroma_offset_l0 -513", chroma_offset, -513),
	SLICE_CASE("five_minus_max_num_merge_cand 5", merge, 5),
	SLICE_CASE("a SliceQpY of 52", qp_delta, 26),
	SLICE_CASE("a SliceQpY of -1 at 8 bits", qp_delta, -27),
	SLICE_CASE("slice_cb_qp_offset 13", cb_offset, 13),
	SLICE_CASE("slice_cb_qp_offset -11, -13 with the PPS's", cb_offset, -11),
	SLICE_CASE("slice_cr_qp_offset 11, 13 with the PPS's", cr_offset, 11),
	SLICE_CASE("slice_cr_qp_offset -13", cr_offset, -13),
	SLICE_CASE("slice_beta_offset_div2 7", beta_offset_div2, 7),
	SLICE_CASE("slice_tc_offset_div2 -7", tc_offset_div2, -7),
	SLICE_CASE("four entry points in four tiles", entry_points, 4),
	SLICE_CASE("offset_len_minus1 32", offset_len_minus1, 32),
	SLICE_CASE("slice_segment_header_extension_length 257", extension_bytes, 257),
};

static u8 unit[UNIT_BYTES];
static struct rp_sps sps;
static struct rp_pps pps;

/**
 * write_header() - A NAL unit header: the type given, layer 0, TemporalId 0.
 * @w:    the writer
 * @type: the NAL unit type
 */
static void write_header(struct writer *w, u32 type)
{
	writer_bits(w, type, 7);   /* forbidden_zero_bit, nal_unit_type */
	writer_bits(w, 0, 6);      /* nuh_layer_id */
	writer_bits(w, 1, 3);      /* nuh_temporal_id_plus1 */
}

/**
 * write_ptl() - profile_tier_level(1, 0): Main, level 3.1.
 * @w: the writer
 */
static void write_ptl(struct writer *w)
{
	writer_bits(w, 0, 2);           /* general_profile_space */
	writer_bits(w, 0, 1);           /* general_tier_flag */
	writer_bits(w, 1, 5);           /* general_profile_idc: Main */
	writer_bits(w, 0x60000000u, 32); /* general_profile_compatibility_flag[1] and [2]: Main and Main 10 */
	writer_bits(w, 0x9, 4);         /* progressive_source, interlaced_source, non_packed_constraint, frame_only_constraint */
	writer_bits(w, 0, 32);          /* the 43 constraint bits */
	writer_bits(w, 0, 11);
	writer_bits(w, 0, 1);           /* general_inbld_flag */
	writer_bits(w, 93, 8);          /* general_level_idc: 30 times 3.1 */
}

/**
 * write_ramp_list() - A scaling list sent in full whose coded coefficient i is i + 1, its DC value from @dc_minus8.
 * @w:         the writer
 * @size_id:   the list's size
 * @dc_minus8: scaling_list_dc_coef_minus8, for 16x16 and 32x32
 */
static void write_ramp_list(struct writer *w, u32 size_id, int64_t dc_minus8)
{
	s32 next = 8; /* nextCoef */
	writer_bits(w, 1, 1); /* scaling_list_pred_mode_flag: sent */
	if (size_id >= 2)
	{
		writer_se(w, (s32)dc_minus8);
		next = (s32)dc_minus8 + 8;
	}
	for (u32 i = 0; i < (size_id == 0 ? SCALING_COEFS_4X4 : SCALING_COEFS); i++)
	{
		writer_se(w, (s32)(i + 1) - next); /* scaling_list_delta_coef */
		next = (s32)(i + 1);
	}
}

/**
 * write_scaling_lists() - scaling_list_data(): every list a ramp, but the second 32x32 one when it is predicted.
 * @w: the writer
 * @f: the SPS's values
 */
static void write_scaling_lists(struct writer *w, const struct sps_fields *f)
{
	for (u32 size_id = 0; size_id < SCALING_SIZES; size_id++)
		for (u32 matrix_id = 0; matrix_id < SCALING_MATRICES; matrix_id += size_id == SCALING_32X32 ? 3 : 1)
		{
			if (size_id == SCALING_32X32 && matrix_id != 0 && f->scaling_32x32_pred != PREDICTION_CODED)
			{
				writer_bits(w, 0, 1); /* scaling_list_pred_mode_flag: predicted */
				writer_ue(w, (u32)f->scaling_32x32_pred);
			}
			else
				write_ramp_list(w, size_id, f->scaling_dc_minus8);
		}
}

/**
 * write_st_rps() - Two short-term sets: set 0 sent in full, one picture before; set 1 predicted from it.
 * @w: the writer
 * @f: the SPS's values
 */
static void write_st_rps(struct writer *w, const struct sps_fields *f)
{
	writer_ue(w, 1);                              /* num_negative_pics */
	writer_ue(w, 0);                              /* num_positive_pics */
	writer_ue(w, (u32)f->st_delta_minus1);        /* delta_poc_s0_minus1 */
	writer_bits(w, 1, 1);                         /* used_by_curr_pic_s0_flag */
	writer_bits(w, 1, 1);                         /* inter_ref_pic_set_prediction_flag */
	writer_bits(w, 1, 1);                         /* delta_rps_sign: negative */
	writer_ue(w, (u32)f->st_abs_delta_rps_minus1); /* abs_delta_rps_minus1 */
	writer_bits(w, 1, 1);                         /* used_by_curr_pic_flag of set 0's picture */
	writer_bits(w, 1, 1);                         /* and of set 0's own */
}

/**
 * write_sps_sizes() - The SPS from its id to the transform hierarchy's depths.
 * @w: the writer
 * @f: the SPS's values
 */
static void write_sps_sizes(struct writer *w, const struct sps_fields *f)
{
	writer_ue(w, 0); /* sps_seq_parameter_set_id */
	writer_ue(w, (u32)f->chroma_format_idc);
	writer_ue(w, (u32)f->width);
	writer_ue(w, (u32)f->height);
	writer_bits(w, 1, 1); /* conformance_window_flag */
	writer_ue(w, (u32)f->window_left);
	writer_ue(w, (u32)f->window_right);
	writer_ue(w, (u32)f->window_top);
	writer_ue(w, (u32)f->window_bottom);
	writer_ue(w, (u32)f->depth_luma_minus8);
	writer_ue(w, (u32)f->depth_chroma_minus8);
	writer_ue(w, (u32)f->poc_lsb_minus4);
	writer_bits(w, 1, 1); /* sps_sub_layer_ordering_info_present_flag */
	writer_ue(w, (u32)f->dpb_minus1);
	writer_ue(w, (u32)f->reorder);
	writer_ue(w, (u32)f->latency_plus1);
	writer_ue(w, (u32)f->min_cb_minus3);
	writer_ue(w, (u32)f->ctb_diff);
	writer_ue(w, (u32)f->min_tb_minus2);
	writer_ue(w, (u32)f->tb_diff);
	writer_ue(w, (u32)f->depth_inter);
	writer_ue(w, (u32)f->depth_intra);
}

/**
 * write_sps() - The SPS of @f as a NAL unit into the unit buffer.
 * @f: the SPS's values
 *
 * Return: the unit's length in bytes.
 */
static u32 write_sps(const struct sps_fields *f)
{
	struct writer w;
	writer_init(&w, unit, sizeof(unit));
	write_header(&w, NAL_SPS_NUT);
	writer_bits(&w, 0, 4); /* sps_video_parameter_set_id */
	writer_bits(&w, 0, 3); /* sps_max_sub_layers_minus1 */
	writer_bits(&w, 1, 1); /* sps_temporal_id_nesting_flag */
	write_ptl(&w);
	write_sps_sizes(&w, f);
	writer_bits(&w, 1, 1); /* scaling_list_enabled_flag */
	writer_bits(&w, 1, 1); /* sps_scaling_list_data_present_flag */
	write_scaling_lists(&w, f);
	writer_bits(&w, 1, 1); /* amp_enabled_flag */
	writer_bits(&w, 1, 1); /* sample_adaptive_offset_enabled_flag */
	writer_bits(&w, 1, 1); /* pcm_enabled_flag */
	writer_bits(&w, (u32)f->pcm_depth_luma_minus1, 4);
	writer_bits(&w, (u32)f->pcm_depth_chroma_minus1, 4);
	writer_ue(&w, (u32)f->pcm_min_minus3);
	writer_ue(&w, (u32)f->pcm_diff);
	writer_bits(&w, 1, 1); /* pcm_loop_filter_disabled_flag */
	writer_ue(&w, (u32)f->num_st_rps);
	write_st_rps(&w, f);
	writer_bits(&w, 1, 1); /* long_term_ref_pics_present_flag */
	writer_ue(&w, 1);      /* num_long_term_ref_pics_sps */
	writer_bits(&w, 0, 8); /* lt_ref_pic_poc_lsb_sps[0]: log2_max_pic_order_cnt_lsb bits */
	writer_bits(&w, 1, 1); /* used_by_curr_pic_lt_sps_flag[0] */
	writer_bits(&w, 1, 1); /* sps_temporal_mvp_enabled_flag */
	writer_bits(&w, 1, 1); /* strong_intra_smoothing_enabled_flag */
	writer_bits(&w, 0, 1); /* vui_parameters_present_flag */
	writer_bits(&w, 0, 1); /* sps_extension_present_flag */
	writer_trailing(&w);
	return w.len;
}

/**
 * write_pps_tiles() - The PPS's tile grid: not uniformly spaced, every size but the last ones sent.
 * @w: the writer
 * @f: the PPS's values
 */
static void write_pps_tiles(struct writer *w, const struct pps_fields *f)
{
	writer_ue(w, (u32)f->tile_columns_minus1);
	writer_ue(w, (u32)f->tile_rows_minus1);
	writer_bits(w, 0, 1); /* uniform_spacing_flag */
	for (int64_t i = 0; i < f->tile_columns_minus1; i++)
		writer_ue(w, (u32)f->column_width_minus1);
	for (int64_t i = 0; i < f->tile_rows_minus1; i++)
		writer_ue(w, (u32)f->row_height_minus1);
	writer_bits(w, 1, 1); /* loop_filter_across_tiles_enabled_flag */
}

/**
 * write_pps() - The PPS of @f as a NAL unit into the unit buffer.
 * @f: the PPS's values
 *
 * Return: the unit's length in bytes.
 */
static u32 write_pps(const struct pps_fields *f)
{
	struct writer w;
	writer_init(&w, unit, sizeof(unit));
	write_header(&w, NAL_PPS_NUT);
	writer_ue(&w, 0);      /* pps_pic_parameter_set_id */
	writer_ue(&w, 0);      /* pps_seq_parameter_set_id */
	writer_bits(&w, 0, 7); /* dependent_slice_segments_enabled_flag, output_flag_present_flag, num_extra_slice_header_bits,
	                        * sign_data_hiding_enabled_flag, cabac_init_present_flag */
	writer_ue(&w, (u32)f->ref_l0_minus1);
	writer_ue(&w, (u32)f->ref_l1_minus1);
	writer_se(&w, (s32)f->init_qp_minus26);
	writer_bits(&w, 0, 2); /* constrained_intra_pred_flag, transform_skip_enabled_flag */
	writer_bits(&w, 1, 1); /* cu_qp_delta_enabled_flag */
	writer_ue(&w, (u32)f->cu_qp_delta_depth);
	writer_se(&w, (s32)f->cb_offset);
	writer_se(&w, (s32)f->cr_offset);
	writer_bits(&w, 0xc, 4); /* pps_slice_chroma_qp_offsets_present_flag and weighted_pred_flag set, weighted_bipred_flag,
	                          * transquant_bypass_enabled_flag clear */
	writer_bits(&w, 1, 1); /* tiles_enabled_flag */
	writer_bits(&w, 0, 1); /* entropy_coding_sync_enabled_flag */
	write_pps_tiles(&w, f);
	writer_bits(&w, 1, 1); /* pps_loop_filter_across_slices_enabled_flag */
	writer_bits(&w, 1, 1); /* deblocking_filter_control_present_flag */
	writer_bits(&w, 2, 2); /* deblocking_filter_override_enabled_flag set, pps_deblocking_filter_disabled_flag clear */
	writer_se(&w, (s32)f->beta_offset_div2);
	writer_se(&w, (s32)f->tc_offset_div2);
	writer_bits(&w, 0, 2); /* pps_scaling_list_data_present_flag, lists_modification_present_flag */
	writer_ue(&w, (u32)f->merge_minus2);
	writer_bits(&w, 2, 2); /* slice_segment_header_extension_present_flag set, pps_extension_present_flag clear */
	writer_trailing(&w);
	return w.len;
}

/**
 * write_slice_long_term() - The slice's long-term entries: the SPS's one candidate as often as asked, then its own.
 * @w: the writer
 * @f: the slice's values
 */
static void write_slice_long_term(struct writer *w, const struct slice_fields *f)
{
	writer_ue(w, (u32)f->num_long_term_sps);
	writer_ue(w, (u32)f->num_long_term_pics);
	for (int64_t i = 0; i < f->num_long_term_sps + f->num_long_term_pics && i <= RP_LT_MAX; i++)
	{
		if (i >= f->num_long_term_sps) /* lt_idx_sps is not sent for the SPS's: it has one candidate */
		{
			writer_bits(w, (u32)i, 8); /* poc_lsb_lt */
			writer_bits(w, 1, 1);      /* used_by_curr_pic_lt_flag */
		}
		writer_bits(w, 1, 1); /* delta_poc_msb_present_flag */
		writer_ue(w, (u32)f->msb_cycle);
	}
}

/**
 * write_slice_weights() - pred_weight_table() for list 0: luma and chroma weights for every reference.
 * @w: the writer
 * @f: the slice's values
 */
static void write_slice_weights(struct writer *w, const struct slice_fields *f)
{
	int64_t refs = f->refs_minus1 + 1 < RP_REFS_MAX ? f->refs_minus1 + 1 : RP_REFS_MAX;
	writer_ue(w, (u32)f->luma_denom);
	writer_se(w, (s32)f->chroma_denom_delta);
	for (int64_t i = 0; i < 2 * refs; i++)
		writer_bits(w, 1, 1); /* luma_weight_l0_flag of each reference, then chroma_weight_l0_flag */
	for (int64_t i = 0; i < refs; i++)
	{
		writer_se(w, (s32)f->luma_weight);
		writer_se(w, (s32)f->luma_offset);
		for (u32 j = 0; j < RP_CHROMA_COMPONENTS; j++)
		{
			writer_se(w, (s32)f->chroma_weight);
			writer_se(w, (s32)f->chroma_offset);
		}
	}
}

/**
 * write_slice_tail() - The slice from slice_qp_delta to its byte_alignment().
 * @w: the writer
 * @f: the slice's values
 */
static void write_slice_tail(struct writer *w, const struct slice_fields *f)
{
	writer_se(w, (s32)f->qp_delta);
	writer_se(w, (s32)f->cb_offset);
	writer_se(w, (s32)f->cr_offset);
	writer_bits(w, 2, 2); /* deblocking_filter_override_flag set, slice_deblocking_filter_disabled_flag clear */
	writer_se(w, (s32)f->beta_offset_div2);
	writer_se(w, (s32)f->tc_offset_div2);
	writer_bits(w, 1, 1); /* slice_loop_filter_across_slices_enabled_flag */
	writer_ue(w, (u32)f->entry_points);
	if (f->entry_points > 0)
	{
		writer_ue(w, (u32)f->offset_len_minus1);
		for (int64_t i = 0; i < f->entry_points && i < 8; i++)
			writer_bits(w, 0, f->offset_len_minus1 < 32 ? (u32)f->offset_len_minus1 + 1 : 32); /* entry_point_offset_minus1 */
	}
	writer_ue(w, (u32)f->extension_bytes);
	for (int64_t i = 0; i < f->extension_bytes && i < 300; i++)
		writer_bits(w, 0xa5, 8); /* slice_segment_header_extension_data_byte */
	writer_trailing(w);      /* byte_alignment(): the slice data would follow */
}

/**
 * write_slice() - The slice of @f as a NAL unit into the unit buffer.
 * @f: the slice's values
 *
 * Return: the unit's length in bytes.
 */
static u32 write_slice(const struct slice_fields *f)
{
	struct writer w;
	writer_init(&w, unit, sizeof(unit));
	write_header(&w, NAL_TRAIL_R);
	writer_bits(&w, 1, 1);         /* first_slice_segment_in_pic_flag */
	writer_ue(&w, 0);              /* slice_pic_parameter_set_id */
	writer_ue(&w, V4L2_HEVC_SLICE_TYPE_P);
	writer_bits(&w, 5, 8);         /* slice_pic_order_cnt_lsb */
	writer_bits(&w, 3, 2);         /* short_term_ref_pic_set_sps_flag set; short_term_ref_pic_set_idx 1, the predicted set */
	write_slice_long_term(&w, f);
	writer_bits(&w, 7, 3);         /* slice_temporal_mvp_enabled_flag, slice_sao_luma_flag, slice_sao_chroma_flag */
	writer_bits(&w, 1, 1);         /* num_ref_idx_active_override_flag */
	writer_ue(&w, (u32)f->refs_minus1);
	if (f->refs_minus1 > 0)        /* the collocated picture, from list 0 in a P slice */
		writer_ue(&w, (u32)f->collocated_ref_idx);
	write_slice_weights(&w, f);
	writer_ue(&w, (u32)f->merge);
	write_slice_tail(&w, f);
	return w.len;
}

/**
 * diagonal_scan() - The up-right diagonal scan of 6.5.3, as the clause writes it: the raster position of each coded coefficient.
 * @size:   the block's size, 4 or 8
 * @raster: size x size entries, filled
 */
static void diagonal_scan(s32 size, u8 *raster)
{
	s32 i = 0, x = 0, y = 0;
	while (i < size * size)
	{
		while (y >= 0)
		{
			if (x < size && y < size)
				raster[i++] = (u8)(y * size + x);
			y--;
			x++;
		}
		y = x;
		x = 0;
	}
}

/**
 * check_scaling() - The ramp lists of write_scaling_lists() in raster order, by the scan of 6.5.3, the DC values as written.
 * @matrix: the parsed lists
 */
static void check_scaling(const struct v4l2_ctrl_hevc_scaling_matrix *matrix)
{
	u8 scan_4x4[SCALING_COEFS_4X4], scan_8x8[SCALING_COEFS];
	diagonal_scan(4, scan_4x4);
	diagonal_scan(8, scan_8x8);
	for (u32 matrix_id = 0; matrix_id < SCALING_MATRICES; matrix_id++)
	{
		for (u32 i = 0; i < SCALING_COEFS_4X4; i++)
			CHECK(matrix->scaling_list_4x4[matrix_id][scan_4x4[i]] == i + 1, "4x4 list %u, coefficient %u", matrix_id, i);
		for (u32 i = 0; i < SCALING_COEFS; i++)
			CHECK(matrix->scaling_list_8x8[matrix_id][scan_8x8[i]] == i + 1 &&
			          matrix->scaling_list_16x16[matrix_id][scan_8x8[i]] == i + 1 &&
			          matrix->scaling_list_32x32[matrix_id % 2][scan_8x8[i]] == i + 1,
			      "8x8, 16x16 or 32x32 list %u, coefficient %u", matrix_id, i);
		CHECK(matrix->scaling_list_dc_coef_16x16[matrix_id] == SCALING_DC && matrix->scaling_list_dc_coef_32x32[matrix_id % 2] == SCALING_DC,
		      "the DC values of list %u", matrix_id);
	}
}

/**
 * check_sps_values() - The valid SPS, parsed: its sizes, depths, buffering and PCM values as written.
 */
static void check_sps_values(void)
{
	CHECK(sps.width == 1280 && sps.height == 720 && sps.bit_depth_luma == 8 && sps.log2_max_poc_lsb == 8, "the size and depths");
	CHECK(sps.log2_min_cb_size == 3 && sps.log2_ctb_size == 5 && sps.log2_min_tb_size == 2 && sps.log2_max_tb_size == 5,
	      "the block sizes");
	CHECK(sps.ctb_width == 40 && sps.ctb_height == 23, "the size in CTBs: %ux%u", sps.ctb_width, sps.ctb_height);
	CHECK(sps.max_dec_pic_buffering == 5 && sps.max_num_reorder == 2 && sps.max_latency_increase_plus1 == 0, "the buffering");
	CHECK(sps.v4l2.pcm_sample_bit_depth_luma_minus1 == 7 && sps.v4l2.log2_diff_max_min_pcm_luma_coding_block_size == 1,
	      "the PCM values");
}

/**
 * check_sps_valid() - The valid SPS parses with every value as written; with its second 32x32 list predicted, too.
 */
static void check_sps_valid(void)
{
	CHECK(rp_sps_parse(&sps, unit, write_sps(&sps_valid)) == RP_PS_OK, "the valid SPS refused: %s", sps.why);
	check_sps_values();
	const struct rp_st_rps *predicted = &sps.st_rps[1];
	CHECK(predicted->num_negative == 2 && predicted->num_positive == 0 && predicted->delta_poc_s0[0] == -1 &&
	          predicted->delta_poc_s0[1] == -2 && predicted->used_s0[0] && predicted->used_s0[1],
	      "the predicted set: %u before, %u after", predicted->num_negative, predicted->num_positive);
	check_scaling(&sps.scaling);

	struct sps_fields fields = sps_valid;
	fields.scaling_32x32_pred = 1; /* the second 32x32 list a copy of the first */
	CHECK(rp_sps_parse(&sps, unit, write_sps(&fields)) == RP_PS_OK, "a predicted 32x32 list refused: %s", sps.why);
	check_scaling(&sps.scaling);
}

/**
 * check_pps_valid() - The valid PPS parses with every value as written, and resolves against the valid SPS.
 */
static void check_pps_valid(void)
{
	CHECK(rp_sps_parse(&sps, unit, write_sps(&sps_valid)) == RP_PS_OK, "the valid SPS refused: %s", sps.why);
	CHECK(rp_pps_parse(&pps, unit, write_pps(&pps_valid)) == RP_PS_OK, "the valid PPS refused: %s", pps.why);
	CHECK(rp_pps_resolve(&pps, &sps) == RP_PS_OK, "the valid PPS not resolved: %s", pps.why);
	CHECK(pps.num_ref_idx_l0_default == 1 && pps.cb_qp_offset == -2 && pps.cr_qp_offset == 2 && pps.diff_cu_qp_delta_depth == 1,
	      "the QP values");
	CHECK(pps.beta_offset_div2 == 1 && pps.tc_offset_div2 == -1, "the deblocking offsets");
	CHECK(pps.column_width[0] == 10 && pps.column_width[1] == 30 && pps.row_height[0] == 10 && pps.row_height[1] == 13,
	      "the tile sizes: %u %u x %u %u", pps.column_width[0], pps.column_width[1], pps.row_height[0], pps.row_height[1]);
	CHECK(pps.v4l2.column_width_minus1[1] == 29 && pps.v4l2.row_height_minus1[1] == 12, "the tile sizes in the record");
}

/**
 * check_slice_valid() - The valid slice parses with every value as written, its data starting right after the header.
 * @sh: the header, filled in
 */
static void check_slice_valid(struct rp_slice_header *sh)
{
	u32 len = write_slice(&slice_valid);
	CHECK(rp_slice_parse(sh, unit, len, NAL_TRAIL_R, 0, &sps, &pps, NULL), "the valid slice refused");
	CHECK(sh->slice_type == V4L2_HEVC_SLICE_TYPE_P && sh->pic_order_cnt_lsb == 5 && sh->st_rps.num_negative == 2,
	      "the slice type, POC and short-term set");
	CHECK(sh->num_long_term_sps == 1 && sh->num_long_term_pics == 2 && sh->delta_poc_msb_cycle_lt[0] == 1 &&
	          sh->delta_poc_msb_cycle_lt[2] == 2 && sh->num_pic_total_curr == 5,
	      "the long-term entries: %u + %u, %u in all", sh->num_long_term_sps, sh->num_long_term_pics, sh->num_pic_total_curr);
	CHECK(sh->num_ref_idx_active[0] == 2 && sh->collocated_ref_idx == 1 && sh->max_num_merge_cand == 5, "the references");
	CHECK(sh->weighted && sh->weights.luma_log2_denom == 6 && sh->weights.chroma_log2_denom == 6 &&
	          sh->weights.chroma_offset[0][1][1] == 0,
	      "the weights");
	CHECK(sh->num_entry_point_offsets == 3 && sh->data_byte_offset == len, "the entry points and the data's offset: %u of %u",
	      sh->data_byte_offset, len);
}

/**
 * check_slices() - The valid slice, then every slice case refused, against the valid SPS and PPS.
 */
static void check_slices(void)
{
	static struct rp_slice_header sh;
	CHECK(rp_sps_parse(&sps, unit, write_sps(&sps_valid)) == RP_PS_OK, "the valid SPS refused: %s", sps.why);
	CHECK(rp_pps_parse(&pps, unit, write_pps(&pps_valid)) == RP_PS_OK && rp_pps_resolve(&pps, &sps) == RP_PS_OK,
	      "the valid PPS refused: %s", pps.why);
	check_slice_valid(&sh);
	for (u32 i = 0; i < sizeof(slice_cases) / sizeof(slice_cases[0]); i++)
	{
		struct slice_fields fields = slice_valid;
		memcpy((u8 *)&fields + slice_cases[i].field, &slice_cases[i].value, sizeof(slice_cases[i].value));
		CHECK(!rp_slice_parse(&sh, unit, write_slice(&fields), NAL_TRAIL_R, 0, &sps, &pps, NULL), "slice %s: taken",
		      slice_cases[i].name);
	}
}

/**
 * set_field() - One value of a set's fields changed, as a case says.
 * @fields: the fields, a struct sps_fields or a struct pps_fields
 * @c:      the case
 */
static void set_field(void *fields, const struct refusal_case *c)
{
	memcpy((u8 *)fields + c->field, &c->value, sizeof(c->value));
}

/**
 * check_refusal() - A parse or a resolve answered a case as it must: a stream error with the case's reason.
 * @what:   "SPS", "PPS" or "resolve", for the message
 * @c:      the case
 * @result: what the call answered
 * @why:    the reason it kept, read after the call: the caller makes the call first
 */
static void check_refusal(const char *what, const struct refusal_case *c, enum rp_ps_result result, const char *why)
{
	CHECK(result == RP_PS_STREAM && why != NULL && strcmp(why, c->reason) == 0, "%s %s: answered %d, \"%s\", not \"%s\"", what,
	      c->name, (int)result, why != NULL ? why : "(no reason)", c->reason);
}

/**
 * check_cases() - Every case refused with its reason.
 */
static void check_cases(void)
{
	for (u32 i = 0; i < sizeof(sps_cases) / sizeof(sps_cases[0]); i++)
	{
		struct sps_fields fields = sps_valid;
		set_field(&fields, &sps_cases[i]);
		enum rp_ps_result result = rp_sps_parse(&sps, unit, write_sps(&fields));
		check_refusal("SPS", &sps_cases[i], result, sps.why);
	}
	for (u32 i = 0; i < sizeof(pps_cases) / sizeof(pps_cases[0]); i++)
	{
		struct pps_fields fields = pps_valid;
		set_field(&fields, &pps_cases[i]);
		enum rp_ps_result result = rp_pps_parse(&pps, unit, write_pps(&fields));
		check_refusal("PPS", &pps_cases[i], result, pps.why);
	}
	CHECK(rp_sps_parse(&sps, unit, write_sps(&sps_valid)) == RP_PS_OK, "the valid SPS refused: %s", sps.why);
	for (u32 i = 0; i < sizeof(resolve_cases) / sizeof(resolve_cases[0]); i++)
	{
		struct pps_fields fields = pps_valid;
		set_field(&fields, &resolve_cases[i]);
		CHECK(rp_pps_parse(&pps, unit, write_pps(&fields)) == RP_PS_OK, "resolve %s: refused at the parse: %s", resolve_cases[i].name,
		      pps.why);
		enum rp_ps_result result = rp_pps_resolve(&pps, &sps);
		check_refusal("resolve", &resolve_cases[i], result, pps.why);
	}
}

/**
 * main() - Run the parameter set parser's checks.
 *
 * Return: 0 when all passed, 1 when not.
 */
int main(void)
{
	check_sps_valid();
	check_pps_valid();
	check_cases();
	check_slices();
	printf("test_ps: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
