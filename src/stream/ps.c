/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ps.c - the parameter sets (ps.h), written from the syntax tables and the
 * semantics of H.265 (V11, 01/2026): 7.3.2.2.1 and 7.4.3.2.1 for the SPS,
 * 7.3.2.3.1 and 7.4.3.3.1 for the PPS, 7.3.3 for profile_tier_level, 7.3.4
 * and 7.4.5 for the scaling lists, 7.3.7 and 7.4.8 for the short-term sets,
 * E.2.1 to E.2.3 for the VUI.
 *
 * Each parse follows its syntax table in order, as a list of steps.  A value
 * the standard bounds is read whole and checked where it is read, before it
 * is narrowed into its field: a damaged set is refused as a stream error
 * then and there, and never hands the HEVC block a value that wrapped into
 * range.  What only this backend or the block cannot take is checked once
 * the set is parsed, and refused as unsupported.  Values that hold between
 * an SPS and a PPS wait for rp_pps_resolve(), when both are known.
 *
 * The V4L2 structures are filled as FFmpeg fills them for the same hardware;
 * the scaling lists go into them in raster order, as Linux's driver for the
 * HEVC block reads them.
 */
#include <memory.h>

#include "nal.h"
#include "ps.h"

/* The standard's limits on the SPS (7.4.3.2.1) */
#define SUB_LAYERS_MAX 7                 /* sps_max_sub_layers_minus1 is 0 to 6 */
#define CHROMA_FORMAT_420 1              /* chroma_format_idc: 4:2:0, the only one decoded */
#define CHROMA_FORMAT_444 3              /* and the largest; separate colour planes only with it */
#define PICTURE_SIZE_FIELD_MAX 65535     /* what the size fields hold: the standard's own limits are the levels' */
#define BIT_DEPTH_MINUS8_MAX 8           /* bit_depth_luma_minus8, bit_depth_chroma_minus8: up to 16 bits */
#define LOG2_MAX_POC_LSB_MINUS4_MAX 12   /* log2_max_pic_order_cnt_lsb_minus4: up to 16 bits of POC */
#define LOG2_CTB_SIZE_MIN 4              /* CtbLog2SizeY: a CTB of 16 to 64 samples */
#define LOG2_CTB_SIZE_MAX 6
#define LOG2_MAX_TB_SIZE_MAX 5           /* MaxTbLog2SizeY: a transform block of 32 samples at most */
#define LOG2_MAX_IPCM_SIZE_MAX 5         /* Log2MaxIpcmCbSizeY: a PCM block of 32 samples at most */
#define ST_DELTA_MINUS1_MAX 32767        /* delta_poc_s0_minus1, delta_poc_s1_minus1, abs_delta_rps_minus1 (7.4.8) */

/* The standard's limits on the PPS (7.4.3.3.1) */
#define NUM_REF_IDX_MINUS1_MAX 14        /* num_ref_idx_l0/l1_default_active_minus1: 15 references at most */
#define INIT_QP_MINUS26_MAX 25           /* init_qp_minus26 is -(26 + QpBdOffsetY) to 25 */
#define CU_QP_DELTA_DEPTH_MAX 3          /* diff_cu_qp_delta_depth: the CTB down to the coding block, 64 to 8 at most */
#define TILE_SIZE_MAX (PICTURE_SIZE_FIELD_MAX >> LOG2_CTB_SIZE_MIN) /* a tile's width or height in CTBs, at most a picture's */
#define LOG2_PARALLEL_MERGE_LEVEL_MAX 6  /* Log2ParMrgLevel: the CTB's size at most; Linux's driver holds it to 6 */

/* What this backend and the block decode */
#define PICTURE_SIZE_MIN 16              /* what the block's driver takes, each way: Linux's HEVC_D_MIN_WIDTH and _HEIGHT */
#define PICTURE_SIZE_MAX 4096            /* HEVC_D_MAX_WIDTH and _HEIGHT */

/* SubWidthC and SubHeightC by chroma_format_idc (Table 6-1): monochrome, 4:2:0, 4:2:2, 4:4:4 */
static const u8 sub_width_c[CHROMA_FORMAT_444 + 1] = {1, 2, 2, 1};
static const u8 sub_height_c[CHROMA_FORMAT_444 + 1] = {1, 2, 1, 1};

/* ---- helpers ------------------------------------------------------------------ */

/**
 * refuse() - A set refused: the reason kept beside it, for the log and the tests.
 * @why:    the set's reason field
 * @result: RP_PS_UNSUPPORTED or RP_PS_STREAM
 * @reason: plain words
 *
 * Return: @result.
 */
static enum rp_ps_result refuse(const char **why, enum rp_ps_result result, const char *reason)
{
	*why = reason;
	return result;
}

/**
 * top_sub_layer() - The index of an SPS's highest sub-layer: HighestTid, where its buffering limits are read.
 * @sps: the set, its sub-layer count read
 *
 * Return: sps_max_sub_layers_minus1.
 */
static u32 top_sub_layer(const struct rp_sps *sps)
{
	return (u32)sps->max_sub_layers - 1u;
}

/* profile_tier_level() (7.3.3): the general profile and level, then each sub-layer's when present */
#define PTL_SUB_LAYER_SLOTS 8  /* the sub-layers it has flags for; those past the SPS's are reserved_zero_2bits */
#define PTL_PROFILE_BITS (2 + 1 + 5 + 32 + 4 + 43 + 1) /* profile_space, tier_flag, profile_idc, 32 compatibility flags,
                                                         * the four source flags, 43 constraint bits, inbld_flag */
#define PTL_LEVEL_BITS 8       /* level_idc */

/**
 * skip_ptl() - profile_tier_level(1, maxNumSubLayersMinus1) (7.3.3), read through.
 * @b:              the reader
 * @max_sub_minus1: the SPS's sub-layers minus one
 *
 * Nothing of it is kept: sps_supported() decides from the SPS's own fields
 * what is decoded, whatever profile and level the stream claims.
 */
static void skip_ptl(struct rp_bits *b, u32 max_sub_minus1)
{
	BOOL profile_present[PTL_SUB_LAYER_SLOTS], level_present[PTL_SUB_LAYER_SLOTS];
	rp_bits_skip(b, PTL_PROFILE_BITS + PTL_LEVEL_BITS); /* the general profile and level */
	for (u32 i = 0; i < max_sub_minus1; i++)
	{
		profile_present[i] = rp_bits_flag(b); /* sub_layer_profile_present_flag */
		level_present[i] = rp_bits_flag(b);   /* sub_layer_level_present_flag */
	}
	if (max_sub_minus1 > 0)
		rp_bits_skip(b, 2 * (PTL_SUB_LAYER_SLOTS - max_sub_minus1)); /* reserved_zero_2bits for the slots left */
	for (u32 i = 0; i < max_sub_minus1; i++)
	{
		if (profile_present[i])
			rp_bits_skip(b, PTL_PROFILE_BITS);
		if (level_present[i])
			rp_bits_skip(b, PTL_LEVEL_BITS);
	}
}

#define EXTENSION_RANGE (1u << 3)    /* of the four extension flags: the range extension, sent first */
#define EXTENSIONS_OTHER 7u          /* the other three: multilayer, 3D, screen content */
#define SPS_RANGE_EXTENSION_FLAGS 9  /* sps_range_extension() (7.3.2.2.2): nine tool flags */

/**
 * parse_extension_flags() - The extension flags that end an SPS and a PPS alike.
 * @b: the reader
 *
 * The four bits after them (sps/pps_extension_4bits) announce extension
 * data a decoder ignores (7.4.3.2.1).
 *
 * Return: EXTENSION_RANGE and EXTENSIONS_OTHER bits; 0 without extensions.
 */
static u32 parse_extension_flags(struct rp_bits *b)
{
	if (!rp_bits_flag(b)) /* sps/pps_extension_present_flag */
		return 0;
	u32 flags = rp_bits_u(b, 4); /* the range, multilayer, 3D and screen content extension flags */
	rp_bits_skip(b, 4);          /* sps/pps_extension_4bits */
	return flags;
}

/* ---- scaling lists (7.3.4, 7.4.5) ------------------------------------------------ */

#define SCALING_SIZES 4              /* sizeId: 4x4, 8x8, 16x16, 32x32 */
#define SCALING_SIZE_4X4 0
#define SCALING_SIZE_8X8 1
#define SCALING_SIZE_16X16 2         /* the first size with a DC value of its own */
#define SCALING_SIZE_32X32 3
#define SCALING_MATRICES 6           /* matrixId: intra Y, Cb, Cr, then inter Y, Cb, Cr */
#define SCALING_INTRA_MATRICES 3     /* matrixId 0 to 2 are the intra lists */
#define SCALING_32X32_STEP 3         /* 32x32 has luma lists only, matrixId 0 and 3 */
#define SCALING_COEFS_4X4 16         /* the coefficients of a 4x4 list */
#define SCALING_COEFS 64             /* and of every larger one: 8x8, upsampled for 16x16 and 32x32 */
#define SCALING_FLAT 16              /* a flat list's factor, and a DC value's default */
#define SCALING_NEXT_START 8         /* nextCoef before a list's first delta, when it has no DC value */
#define SCALING_DC_MINUS8_MIN -7     /* scaling_list_dc_coef_minus8: a DC value of 1 to 255 */
#define SCALING_DC_MINUS8_MAX 247
#define SCALING_DELTA_MIN -128       /* scaling_list_delta_coef */
#define SCALING_DELTA_MAX 127
#define SCALING_COEF_WRAP 256        /* nextCoef counts modulo a byte */

/* Table 7-5 and 7-6: the default lists, in the coded (up-right diagonal) order */
static const u8 default_4x4[SCALING_COEFS_4X4] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16};
static const u8 default_intra[SCALING_COEFS] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18, 17, 18, 18, 17, 18, 21,
                                                19, 20, 21, 20, 19, 21, 24, 22, 22, 24, 24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29,
                                                31, 35, 35, 31, 29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115};
static const u8 default_inter[SCALING_COEFS] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 20,
                                                20, 20, 20, 20, 20, 20, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28,
                                                28, 28, 28, 28, 28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91};

/* The up-right diagonal scans (6.5.3): the raster position of each coded coefficient; test_ps holds them to the clause */
static const u8 scan_4x4[SCALING_COEFS_4X4] = {0, 4, 1, 8, 5, 2, 12, 9, 6, 3, 13, 10, 7, 14, 11, 15};
static const u8 scan_8x8[SCALING_COEFS] = {0,  8,  1,  16, 9,  2,  24, 17, 10, 3,  32, 25, 18, 11, 4,  40, 33, 26, 19, 12, 5,  48,
                                           41, 34, 27, 20, 13, 6,  56, 49, 42, 35, 28, 21, 14, 7,  57, 50, 43, 36, 29, 22, 15, 58,
                                           51, 44, 37, 30, 23, 59, 52, 45, 38, 31, 60, 53, 46, 39, 61, 54, 47, 62, 55, 63};

/**
 * matrix_step() - The step from one matrixId to the next of a size.
 * @size_id: 0 to 3
 *
 * Return: 1; SCALING_32X32_STEP for 32x32, which has a luma list for intra and one for inter only.
 */
static u32 matrix_step(u32 size_id)
{
	return size_id == SCALING_SIZE_32X32 ? SCALING_32X32_STEP : 1;
}

/**
 * coefs_of() - How many coefficients a list of a size has.
 * @size_id: 0 to 3
 *
 * Return: SCALING_COEFS_4X4 for 4x4, else SCALING_COEFS.
 */
static u32 coefs_of(u32 size_id)
{
	return size_id == SCALING_SIZE_4X4 ? SCALING_COEFS_4X4 : SCALING_COEFS;
}

/**
 * scan_of() - The scan a list of a size is coded in.
 * @size_id: 0 to 3
 *
 * Return: scan_4x4 for 4x4, else scan_8x8: the larger lists are coded as 8x8 and upsampled.
 */
static const u8 *scan_of(u32 size_id)
{
	return size_id == SCALING_SIZE_4X4 ? scan_4x4 : scan_8x8;
}

/**
 * has_dc() - Whether a list of a size has a DC value of its own: 16x16 and 32x32 do.
 * @size_id: 0 to 3
 *
 * Return: TRUE for those two.
 */
static BOOL has_dc(u32 size_id)
{
	return size_id >= SCALING_SIZE_16X16;
}

/**
 * list_of() - Where a list lives in the V4L2 matrix.
 * @matrix:    the matrix
 * @size_id:   0 to 3
 * @matrix_id: 0 to 5 (0 and 3 for 32x32)
 *
 * Return: the list, coefs_of() entries, in raster order.
 */
static u8 *list_of(struct v4l2_ctrl_hevc_scaling_matrix *matrix, u32 size_id, u32 matrix_id)
{
	switch (size_id)
	{
	case SCALING_SIZE_4X4:
		return matrix->scaling_list_4x4[matrix_id];
	case SCALING_SIZE_8X8:
		return matrix->scaling_list_8x8[matrix_id];
	case SCALING_SIZE_16X16:
		return matrix->scaling_list_16x16[matrix_id];
	default:
		return matrix->scaling_list_32x32[matrix_id / SCALING_32X32_STEP];
	}
}

/**
 * dc_of() - Where a 16x16 or 32x32 list's DC value lives in the V4L2 matrix.
 * @matrix:    the matrix
 * @size_id:   2 or 3
 * @matrix_id: the list
 *
 * Return: the entry.
 */
static u8 *dc_of(struct v4l2_ctrl_hevc_scaling_matrix *matrix, u32 size_id, u32 matrix_id)
{
	if (size_id == SCALING_SIZE_16X16)
		return &matrix->scaling_list_dc_coef_16x16[matrix_id];
	return &matrix->scaling_list_dc_coef_32x32[matrix_id / SCALING_32X32_STEP];
}

/**
 * defaults_of() - The default list for a size and matrix (Table 7-5, 7-6), in coded order.
 * @size_id:   0 to 3
 * @matrix_id: the list
 *
 * Return: the flat 4x4 list, or the 8x8 intra or inter one, which the larger sizes take too.
 */
static const u8 *defaults_of(u32 size_id, u32 matrix_id)
{
	if (size_id == SCALING_SIZE_4X4)
		return default_4x4;
	return matrix_id < SCALING_INTRA_MATRICES ? default_intra : default_inter;
}

/**
 * list_default() - A list set to its default, the DC value to SCALING_FLAT.
 * @matrix:    the matrix
 * @size_id:   the size
 * @matrix_id: the list
 */
static void list_default(struct v4l2_ctrl_hevc_scaling_matrix *matrix, u32 size_id, u32 matrix_id)
{
	u8 *list = list_of(matrix, size_id, matrix_id);
	const u8 *scan = scan_of(size_id);
	const u8 *defaults = defaults_of(size_id, matrix_id);
	for (u32 i = 0; i < coefs_of(size_id); i++)
		list[scan[i]] = defaults[i];
	if (has_dc(size_id))
		*dc_of(matrix, size_id, matrix_id) = SCALING_FLAT;
}

/**
 * rp_scaling_default() - The matrix of default lists: what scaling_list_enabled_flag means without data.
 * @matrix: the matrix
 */
void rp_scaling_default(struct v4l2_ctrl_hevc_scaling_matrix *matrix)
{
	memset(matrix, 0, sizeof(*matrix));
	for (u32 size_id = 0; size_id < SCALING_SIZES; size_id++)
		for (u32 matrix_id = 0; matrix_id < SCALING_MATRICES; matrix_id += matrix_step(size_id))
			list_default(matrix, size_id, matrix_id);
}

/**
 * list_predicted() - A list not sent: the default, or a copy of an earlier list of its size.
 * @b:         the reader
 * @matrix:    the matrix
 * @size_id:   the size
 * @matrix_id: the list
 *
 * scaling_list_pred_matrix_id_delta counts lists of the size back: 0 for
 * the default, else refMatrixId = matrixId - delta * matrix_step().  It is
 * checked before it is multiplied, so that no damaged value wraps into a
 * list that exists.
 *
 * Return: RP_PS_OK, or RP_PS_STREAM for a reference before the first list.
 */
static enum rp_ps_result list_predicted(struct rp_bits *b, struct v4l2_ctrl_hevc_scaling_matrix *matrix, u32 size_id,
                                        u32 matrix_id)
{
	u32 step = matrix_step(size_id);
	u32 delta = rp_bits_ue(b); /* scaling_list_pred_matrix_id_delta */
	if (delta > matrix_id / step)
		return RP_PS_STREAM;
	if (delta == 0)
	{
		list_default(matrix, size_id, matrix_id);
		return RP_PS_OK;
	}
	u32 reference = matrix_id - delta * step; /* refMatrixId */
	memcpy(list_of(matrix, size_id, matrix_id), list_of(matrix, size_id, reference), coefs_of(size_id));
	if (has_dc(size_id))
		*dc_of(matrix, size_id, matrix_id) = *dc_of(matrix, size_id, reference);
	return RP_PS_OK;
}

/**
 * list_coded() - A list sent: its DC value for the larger sizes, then its coefficients as deltas, in scan order.
 * @b:         the reader
 * @matrix:    the matrix
 * @size_id:   the size
 * @matrix_id: the list
 *
 * Each coefficient is the one before plus its delta, modulo 256, starting
 * from the DC value or SCALING_NEXT_START; a factor of 0 is not allowed
 * (7.4.5).  Both values are range checked before they are added, so that
 * no damaged one overflows.
 *
 * Return: RP_PS_OK, or RP_PS_STREAM for a value out of range or a factor of 0.
 */
static enum rp_ps_result list_coded(struct rp_bits *b, struct v4l2_ctrl_hevc_scaling_matrix *matrix, u32 size_id, u32 matrix_id)
{
	u8 *list = list_of(matrix, size_id, matrix_id);
	const u8 *scan = scan_of(size_id);
	s32 next = SCALING_NEXT_START; /* nextCoef */
	if (has_dc(size_id))
	{
		s32 dc_minus8 = rp_bits_se(b); /* scaling_list_dc_coef_minus8 */
		if (dc_minus8 < SCALING_DC_MINUS8_MIN || dc_minus8 > SCALING_DC_MINUS8_MAX)
			return RP_PS_STREAM;
		next = dc_minus8 + 8;
		*dc_of(matrix, size_id, matrix_id) = (u8)next;
	}
	for (u32 i = 0; i < coefs_of(size_id); i++)
	{
		s32 delta = rp_bits_se(b); /* scaling_list_delta_coef */
		if (delta < SCALING_DELTA_MIN || delta > SCALING_DELTA_MAX)
			return RP_PS_STREAM;
		next = (next + delta + SCALING_COEF_WRAP) % SCALING_COEF_WRAP;
		if (next == 0)
			return RP_PS_STREAM;
		list[scan[i]] = (u8)next;
	}
	return RP_PS_OK;
}

/**
 * parse_scaling_list_data() - scaling_list_data() (7.3.4) into a V4L2 matrix, raster order.
 * @b:      the reader
 * @matrix: the matrix, filled whole
 *
 * Return: RP_PS_OK, or RP_PS_STREAM for a reference or a value out of range, or the data cut short.
 */
static enum rp_ps_result parse_scaling_list_data(struct rp_bits *b, struct v4l2_ctrl_hevc_scaling_matrix *matrix)
{
	memset(matrix, 0, sizeof(*matrix));
	for (u32 size_id = 0; size_id < SCALING_SIZES; size_id++)
		for (u32 matrix_id = 0; matrix_id < SCALING_MATRICES; matrix_id += matrix_step(size_id))
		{
			enum rp_ps_result result;
			if (rp_bits_flag(b)) /* scaling_list_pred_mode_flag */
				result = list_coded(b, matrix, size_id, matrix_id);
			else
				result = list_predicted(b, matrix, size_id, matrix_id);
			if (result != RP_PS_OK)
				return result;
		}
	return b->overrun ? RP_PS_STREAM : RP_PS_OK;
}

/* ---- short-term reference picture sets (7.3.7, 7.4.8) ----------------------------- */

/* The flags of a predicted set: one per picture of the reference set, the last for the reference set's own picture */
struct st_rps_flags
{
	u8 used[RP_ST_PICS_MAX + 1];      /* used_by_curr_pic_flag */
	u8 use_delta[RP_ST_PICS_MAX + 1]; /* use_delta_flag, 1 when not sent */
};

/* One side of a set being derived: the pictures before the current one, or those after */
struct st_rps_side
{
	s32 *delta_poc; /* RP_ST_PICS_MAX entries */
	u8 *used;       /* RP_ST_PICS_MAX entries: UsedByCurrPicS0 or S1 */
	u32 count;      /* entries wanted; those past RP_ST_PICS_MAX are counted and not kept */
	BOOL before;    /* the side of negative deltas */
};

/**
 * side_take() - A candidate picture joins the side it lies on, if its flag keeps it.
 * @side:  the side being derived
 * @delta: the candidate's POC delta to the current picture
 * @flags: the predicted set's flags
 * @j:     the candidate's flag
 */
static void side_take(struct st_rps_side *side, s32 delta, const struct st_rps_flags *flags, u32 j)
{
	BOOL on_side = side->before ? delta < 0 : delta > 0;
	if (!on_side || !flags->use_delta[j])
		return;
	if (side->count < RP_ST_PICS_MAX)
	{
		side->delta_poc[side->count] = delta;
		side->used[side->count] = flags->used[j];
	}
	side->count++;
}

/**
 * side_derive() - One side of a predicted set, (7-61) for the pictures before and (7-62) for those after.
 * @side:      the side, empty
 * @reference: the reference set
 * @delta_rps: deltaRps
 * @flags:     the predicted set's flags
 *
 * In order of distance from the current picture: the reference set's
 * pictures of the other side from the farthest, the reference set's own
 * picture, its pictures of this side from the nearest.  The flags follow
 * the reference set's order: the negative pictures, the positive ones,
 * then its own.
 *
 * Return: the pictures on the side.
 */
static u32 side_derive(struct st_rps_side *side, const struct rp_st_rps *reference, s32 delta_rps,
                       const struct st_rps_flags *flags)
{
	const s32 *other = side->before ? reference->delta_poc_s1 : reference->delta_poc_s0;
	const s32 *same = side->before ? reference->delta_poc_s0 : reference->delta_poc_s1;
	u32 num_other = side->before ? reference->num_positive : reference->num_negative;
	u32 num_same = side->before ? reference->num_negative : reference->num_positive;
	u32 other_flags = side->before ? reference->num_negative : 0;
	u32 same_flags = side->before ? 0 : reference->num_negative;
	for (u32 j = num_other; j-- > 0;)
		side_take(side, other[j] + delta_rps, flags, other_flags + j);
	side_take(side, delta_rps, flags, (u32)reference->num_negative + reference->num_positive);
	for (u32 j = 0; j < num_same; j++)
		side_take(side, same[j] + delta_rps, flags, same_flags + j);
	return side->count;
}

/**
 * st_rps_predicted() - A set predicted from an earlier one (inter_ref_pic_set_prediction_flag).
 * @b:        the reader
 * @out:      the set, cleared
 * @idx:      stRpsIdx
 * @sets:     the SPS's candidate sets so far
 * @num_sets: num_short_term_ref_pic_sets
 *
 * Its pictures are the reference set's moved by deltaRps, and the reference
 * set's own picture, as far as their flags keep them.  deltaRps is bounded
 * (7.4.8), which also keeps every derived delta far from overflowing.
 *
 * Return: RP_PS_OK, or RP_PS_STREAM for a reference set before the first or a deltaRps out of range.
 */
static enum rp_ps_result st_rps_predicted(struct rp_bits *b, struct rp_st_rps *out, u32 idx, const struct rp_st_rps *sets,
                                          u32 num_sets)
{
	struct st_rps_flags flags;
	struct st_rps_side before = {out->delta_poc_s0, out->used_s0, 0, TRUE};
	struct st_rps_side after = {out->delta_poc_s1, out->used_s1, 0, FALSE};
	u32 delta_idx_minus1 = idx == num_sets ? rp_bits_ue(b) : 0; /* delta_idx_minus1: sent for a slice header's own set only */
	if (delta_idx_minus1 + 1 > idx)
		return RP_PS_STREAM;
	const struct rp_st_rps *reference = &sets[idx - (delta_idx_minus1 + 1)];
	s32 sign = rp_bits_flag(b) ? -1 : 1; /* delta_rps_sign */
	u32 abs_delta_rps_minus1;
	if (!rp_bits_ue_at_most(b, ST_DELTA_MINUS1_MAX, &abs_delta_rps_minus1))
		return RP_PS_STREAM;
	s32 delta_rps = sign * (s32)(abs_delta_rps_minus1 + 1);
	for (u32 j = 0; j <= (u32)reference->num_negative + reference->num_positive; j++)
	{
		flags.used[j] = (u8)rp_bits_u(b, 1);                              /* used_by_curr_pic_flag */
		flags.use_delta[j] = flags.used[j] ? 1 : (u8)rp_bits_u(b, 1); /* use_delta_flag */
	}
	out->num_negative = (u8)side_derive(&before, reference, delta_rps, &flags);
	out->num_positive = (u8)side_derive(&after, reference, delta_rps, &flags);
	return RP_PS_OK;
}

/**
 * st_rps_explicit_side() - One side of a set sent in full: each picture's distance from the one before it.
 * @b:         the reader
 * @delta_poc: the side's deltas, filled
 * @used:      the side's used flags, filled
 * @count:     the pictures on the side
 * @sign:      -1 for the pictures before the current one ((7-67), (7-69)), 1 for those after ((7-68), (7-70))
 *
 * Each distance is bounded (7.4.8), so that their sum cannot overflow.
 *
 * Return: TRUE; FALSE for a distance out of range.
 */
static BOOL st_rps_explicit_side(struct rp_bits *b, s32 *delta_poc, u8 *used, u32 count, s32 sign)
{
	s32 poc = 0;
	for (u32 i = 0; i < count; i++)
	{
		u32 delta_minus1;
		if (!rp_bits_ue_at_most(b, ST_DELTA_MINUS1_MAX, &delta_minus1)) /* delta_poc_s0_minus1 or delta_poc_s1_minus1 */
			return FALSE;
		poc += sign * (s32)(delta_minus1 + 1);
		delta_poc[i] = poc;
		used[i] = (u8)rp_bits_u(b, 1); /* used_by_curr_pic_s0_flag or used_by_curr_pic_s1_flag */
	}
	return TRUE;
}

/**
 * rp_st_rps_parse() - st_ref_pic_set(stRpsIdx): parsed and derived.
 * @b:        the reader
 * @out:      the set
 * @idx:      stRpsIdx: the set's index in the SPS, or num_sets for a slice header's own
 * @sets:     the SPS's candidate sets so far
 * @num_sets: num_short_term_ref_pic_sets
 *
 * Return: RP_PS_OK, or RP_PS_STREAM.
 */
enum rp_ps_result rp_st_rps_parse(struct rp_bits *b, struct rp_st_rps *out, u32 idx, const struct rp_st_rps *sets,
                                  u32 num_sets)
{
	memset(out, 0, sizeof(*out));
	if (idx != 0 && rp_bits_flag(b)) /* inter_ref_pic_set_prediction_flag */
	{
		if (st_rps_predicted(b, out, idx, sets, num_sets) != RP_PS_OK)
			return RP_PS_STREAM;
	}
	else
	{
		u32 negative = rp_bits_ue(b), positive = rp_bits_ue(b); /* num_negative_pics, num_positive_pics */
		if (negative > RP_ST_PICS_MAX || positive > RP_ST_PICS_MAX)
			return RP_PS_STREAM;
		out->num_negative = (u8)negative;
		out->num_positive = (u8)positive;
		if (!st_rps_explicit_side(b, out->delta_poc_s0, out->used_s0, negative, -1) ||
		    !st_rps_explicit_side(b, out->delta_poc_s1, out->used_s1, positive, 1))
			return RP_PS_STREAM;
	}
	if ((u32)out->num_negative + out->num_positive > RP_ST_PICS_MAX)
		return RP_PS_STREAM;
	return b->overrun ? RP_PS_STREAM : RP_PS_OK;
}

/* ---- the VUI (E.2) ----------------------------------------------------------------- */

#define EXTENDED_SAR 255        /* aspect_ratio_idc: the ratio is sent as two numbers */
#define SAR_TABLE_ENTRIES 17    /* aspect_ratio_idc 0 to 16 (Table E.1); 17 to 254 are reserved */

/* Table E.1: the sample aspect ratio of each aspect_ratio_idc, width then height; 0 is unspecified */
static const u16 sar_table[SAR_TABLE_ENTRIES][2] = {{0, 0},   {1, 1},   {12, 11}, {10, 11}, {16, 11},  {40, 33},
                                                    {24, 11}, {20, 11}, {32, 11}, {80, 33}, {18, 11},  {15, 11},
                                                    {64, 33}, {160, 99}, {4, 3},   {3, 2},   {2, 1}};

/**
 * skip_sub_layer_hrd() - sub_layer_hrd_parameters() read through (E.2.3).
 * @b:       the reader
 * @cpb_cnt: CpbCnt
 * @sub_pic: sub_pic_hrd_params_present_flag
 */
static void skip_sub_layer_hrd(struct rp_bits *b, u32 cpb_cnt, BOOL sub_pic)
{
	for (u32 i = 0; i < cpb_cnt; i++)
	{
		rp_bits_ue(b); /* bit_rate_value_minus1 */
		rp_bits_ue(b); /* cpb_size_value_minus1 */
		if (sub_pic)
		{
			rp_bits_ue(b); /* cpb_size_du_value_minus1 */
			rp_bits_ue(b); /* bit_rate_du_value_minus1 */
		}
		rp_bits_skip(b, 1); /* cbr_flag */
	}
}

/**
 * skip_hrd_common() - The common part of hrd_parameters() read through (E.2.2).
 * @b: the reader
 *
 * Return: the sub_pic_hrd_params_present_flag the sub-layers' parameters depend on.
 */
static BOOL skip_hrd_common(struct rp_bits *b)
{
	BOOL sub_pic = rp_bits_flag(b);
	if (sub_pic)
		rp_bits_skip(b, 8 + 5 + 1 + 5); /* tick_divisor_minus2, du_cpb_removal_delay_increment_length_minus1,
		                                 * sub_pic_cpb_params_in_pic_timing_sei_flag, dpb_output_delay_du_length_minus1 */
	rp_bits_skip(b, 4 + 4);             /* bit_rate_scale, cpb_size_scale */
	if (sub_pic)
		rp_bits_skip(b, 4);             /* cpb_size_du_scale */
	rp_bits_skip(b, 5 + 5 + 5);         /* initial_cpb_removal_delay_length_minus1, au_cpb_removal_delay_length_minus1,
		                                 * dpb_output_delay_length_minus1 */
	return sub_pic;
}

/**
 * skip_hrd() - hrd_parameters(1, maxNumSubLayersMinus1) read through (E.2.2).
 * @b:              the reader
 * @max_sub_minus1: the SPS's sub-layers minus one
 */
static void skip_hrd(struct rp_bits *b, u32 max_sub_minus1)
{
	BOOL nal_hrd = rp_bits_flag(b); /* nal_hrd_parameters_present_flag */
	BOOL vcl_hrd = rp_bits_flag(b); /* vcl_hrd_parameters_present_flag */
	BOOL sub_pic = (nal_hrd || vcl_hrd) ? skip_hrd_common(b) : FALSE;
	for (u32 i = 0; i <= max_sub_minus1; i++)
	{
		BOOL fixed_general = rp_bits_flag(b);                         /* fixed_pic_rate_general_flag */
		BOOL fixed_cvs = fixed_general ? TRUE : rp_bits_flag(b);      /* fixed_pic_rate_within_cvs_flag, 1 when not sent */
		BOOL low_delay = FALSE;
		u32 cpb_cnt = 1;
		if (fixed_cvs)
			rp_bits_ue(b);                                            /* elemental_duration_in_tc_minus1 */
		else
			low_delay = rp_bits_flag(b);                              /* low_delay_hrd_flag */
		if (!low_delay)
			cpb_cnt = rp_bits_ue(b) + 1;                              /* cpb_cnt_minus1 */
		if (nal_hrd)
			skip_sub_layer_hrd(b, cpb_cnt, sub_pic);
		if (vcl_hrd)
			skip_sub_layer_hrd(b, cpb_cnt, sub_pic);
	}
}

/**
 * parse_vui_aspect_ratio() - The VUI's sample aspect ratio (E.2.1, Table E.1).
 * @b:   the reader
 * @sps: filled in; 0, 0 for an unspecified or reserved ratio
 */
static void parse_vui_aspect_ratio(struct rp_bits *b, struct rp_sps *sps)
{
	if (!rp_bits_flag(b)) /* aspect_ratio_info_present_flag */
		return;
	u32 aspect_ratio_idc = rp_bits_u(b, 8);
	if (aspect_ratio_idc == EXTENDED_SAR)
	{
		sps->sar_width = (u16)rp_bits_u(b, 16);
		sps->sar_height = (u16)rp_bits_u(b, 16);
	}
	else if (aspect_ratio_idc < SAR_TABLE_ENTRIES)
	{
		sps->sar_width = sar_table[aspect_ratio_idc][0];
		sps->sar_height = sar_table[aspect_ratio_idc][1];
	}
}

/**
 * parse_vui_signal() - The VUI from the overscan to the default display window: the range, the matrix and the fields kept.
 * @b:   the reader
 * @sps: filled in
 */
static void parse_vui_signal(struct rp_bits *b, struct rp_sps *sps)
{
	if (rp_bits_flag(b))    /* overscan_info_present_flag */
		rp_bits_skip(b, 1); /* overscan_appropriate_flag */
	sps->video_signal_present = rp_bits_flag(b);
	if (sps->video_signal_present)
	{
		rp_bits_skip(b, 3); /* video_format */
		sps->full_range = rp_bits_flag(b);
		sps->colour_description_present = rp_bits_flag(b);
		if (sps->colour_description_present)
		{
			rp_bits_skip(b, 8 + 8); /* colour_primaries, transfer_characteristics: the stream info has no room for them */
			sps->matrix_coeffs = (u8)rp_bits_u(b, 8);
		}
	}
	if (rp_bits_flag(b)) /* chroma_loc_info_present_flag */
	{
		rp_bits_ue(b); /* chroma_sample_loc_type_top_field */
		rp_bits_ue(b); /* chroma_sample_loc_type_bottom_field */
	}
	rp_bits_skip(b, 1); /* neutral_chroma_indication_flag */
	sps->field_seq = rp_bits_flag(b);
	rp_bits_skip(b, 1); /* frame_field_info_present_flag */
	if (rp_bits_flag(b)) /* default_display_window_flag: its four offsets, a display hint the stream info has no room for */
	{
		rp_bits_ue(b);
		rp_bits_ue(b);
		rp_bits_ue(b);
		rp_bits_ue(b);
	}
}

/**
 * parse_vui_timing() - The VUI's timing, its HRD parameters read through, and the bitstream restrictions read through.
 * @b:   the reader
 * @sps: filled in, its sub-layer count read
 */
static void parse_vui_timing(struct rp_bits *b, struct rp_sps *sps)
{
	sps->timing_present = rp_bits_flag(b);
	if (sps->timing_present)
	{
		sps->num_units_in_tick = rp_bits_u(b, 32);
		sps->time_scale = rp_bits_u(b, 32);
		if (rp_bits_flag(b)) /* vui_poc_proportional_to_timing_flag */
			rp_bits_ue(b);   /* vui_num_ticks_poc_diff_one_minus1 */
		if (rp_bits_flag(b)) /* vui_hrd_parameters_present_flag */
			skip_hrd(b, top_sub_layer(sps));
	}
	if (rp_bits_flag(b)) /* bitstream_restriction_flag */
	{
		rp_bits_skip(b, 3); /* tiles_fixed_structure_flag, motion_vectors_over_pic_boundaries_flag, restricted_ref_pic_lists_flag */
		rp_bits_ue(b);      /* min_spatial_segmentation_idc */
		rp_bits_ue(b);      /* max_bytes_per_pic_denom */
		rp_bits_ue(b);      /* max_bits_per_min_cu_denom */
		rp_bits_ue(b);      /* log2_max_mv_length_horizontal */
		rp_bits_ue(b);      /* log2_max_mv_length_vertical */
	}
}

/* ---- the SPS (7.3.2.2.1, 7.4.3.2.1) ------------------------------------------------ */

/**
 * sps_parse_format() - The SPS's id, its chroma format and its picture size.
 * @b:   the reader
 * @sps: filled in
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result sps_parse_format(struct rp_bits *b, struct rp_sps *sps)
{
	u32 sps_id, chroma_format_idc;
	if (!rp_bits_ue_at_most(b, RP_SPS_MAX - 1, &sps_id))
		return refuse(&sps->why, RP_PS_STREAM, "sps id over 15");
	sps->sps_id = (u8)sps_id;
	if (!rp_bits_ue_at_most(b, CHROMA_FORMAT_444, &chroma_format_idc))
		return refuse(&sps->why, RP_PS_STREAM, "chroma_format_idc over 3");
	sps->chroma_format_idc = (u8)chroma_format_idc;
	if (chroma_format_idc == CHROMA_FORMAT_444)
		sps->separate_colour_plane = rp_bits_flag(b);
	sps->chroma_array_type = sps->separate_colour_plane ? 0 : sps->chroma_format_idc;
	u32 width = rp_bits_ue(b), height = rp_bits_ue(b); /* pic_width_in_luma_samples, pic_height_in_luma_samples */
	if (width == 0 || height == 0 || width > PICTURE_SIZE_FIELD_MAX || height > PICTURE_SIZE_FIELD_MAX)
		return refuse(&sps->why, RP_PS_STREAM, "a picture size of 0 or over 65535");
	sps->width = (u16)width;
	sps->height = (u16)height;
	return RP_PS_OK;
}

/**
 * sps_parse_window() - The conformance window, in luma samples, held to leave some of the picture.
 * @b:   the reader
 * @sps: filled in, its chroma format and picture size read
 *
 * The offsets come in chroma samples, which SubWidthC and SubHeightC make
 * luma samples; together they must leave at least one sample each way
 * (7.4.3.2.1).  Each is first held under the picture's size, so that their
 * sum cannot wrap.
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result sps_parse_window(struct rp_bits *b, struct rp_sps *sps)
{
	if (!rp_bits_flag(b)) /* conformance_window_flag */
		return RP_PS_OK;
	u32 sub_width = sub_width_c[sps->chroma_format_idc], sub_height = sub_height_c[sps->chroma_format_idc];
	u32 left = rp_bits_ue(b), right = rp_bits_ue(b), top = rp_bits_ue(b), bottom = rp_bits_ue(b); /* conf_win_*_offset */
	if (left >= sps->width || right >= sps->width || top >= sps->height || bottom >= sps->height ||
	    sub_width * (left + right) >= sps->width || sub_height * (top + bottom) >= sps->height)
		return refuse(&sps->why, RP_PS_STREAM, "a conformance window that leaves no picture");
	sps->conf_left = left * sub_width;
	sps->conf_right = right * sub_width;
	sps->conf_top = top * sub_height;
	sps->conf_bottom = bottom * sub_height;
	return RP_PS_OK;
}

/**
 * sps_parse_depths() - The bit depths and the POC's width.
 * @b:   the reader
 * @sps: filled in
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result sps_parse_depths(struct rp_bits *b, struct rp_sps *sps)
{
	u32 luma_minus8, chroma_minus8, poc_lsb_minus4;
	if (!rp_bits_ue_at_most(b, BIT_DEPTH_MINUS8_MAX, &luma_minus8) || !rp_bits_ue_at_most(b, BIT_DEPTH_MINUS8_MAX, &chroma_minus8))
		return refuse(&sps->why, RP_PS_STREAM, "a bit depth over 16");
	if (!rp_bits_ue_at_most(b, LOG2_MAX_POC_LSB_MINUS4_MAX, &poc_lsb_minus4))
		return refuse(&sps->why, RP_PS_STREAM, "log2_max_pic_order_cnt_lsb over 16");
	sps->bit_depth_luma = (u8)(luma_minus8 + 8);
	sps->bit_depth_chroma = (u8)(chroma_minus8 + 8);
	sps->log2_max_poc_lsb = (u8)(poc_lsb_minus4 + 4);
	return RP_PS_OK;
}

/**
 * sps_parse_sub_layers() - The buffering limits: the highest sub-layer's kept, the DPB size and the reordering held to the standard's.
 * @b:   the reader
 * @sps: filled in, its sub-layer count read
 *
 * With sps_sub_layer_ordering_info_present_flag every sub-layer's limits
 * are sent, lowest first; without, only the highest's.  Either way the last
 * ones read are the highest sub-layer's, the only ones used.
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result sps_parse_sub_layers(struct rp_bits *b, struct rp_sps *sps)
{
	u32 top = top_sub_layer(sps);
	BOOL ordering_present = rp_bits_flag(b); /* sps_sub_layer_ordering_info_present_flag */
	for (u32 i = ordering_present ? 0 : top; i <= top; i++)
	{
		sps->max_dec_pic_buffering = rp_bits_ue(b) + 1;  /* sps_max_dec_pic_buffering_minus1 */
		sps->max_num_reorder = rp_bits_ue(b);            /* sps_max_num_reorder_pics */
		sps->max_latency_increase_plus1 = rp_bits_ue(b); /* sps_max_latency_increase_plus1 */
	}
	if (sps->max_dec_pic_buffering > RP_DPB_SIZE_MAX)
		return refuse(&sps->why, RP_PS_STREAM, "sps_max_dec_pic_buffering over 16");
	if (sps->max_num_reorder > sps->max_dec_pic_buffering - 1)
		return refuse(&sps->why, RP_PS_STREAM, "reorder pictures over the buffering");
	return RP_PS_OK;
}

/**
 * sps_parse_coding_blocks() - The coding block and CTB sizes, and the picture's size in CTBs.
 * @b:   the reader
 * @sps: filled in, its picture size read
 *
 * The CTB is 16 to 64 samples.  Both values are held to that first, so that
 * their sum cannot wrap; a set cut short here would read as a CTB of 8, and
 * is told apart.
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result sps_parse_coding_blocks(struct rp_bits *b, struct rp_sps *sps)
{
	u32 min_cb_minus3 = rp_bits_ue(b); /* log2_min_luma_coding_block_size_minus3 */
	u32 ctb_diff = rp_bits_ue(b);      /* log2_diff_max_min_luma_coding_block_size */
	if (b->overrun)
		return refuse(&sps->why, RP_PS_STREAM, "the SPS ends early");
	if (min_cb_minus3 > LOG2_CTB_SIZE_MAX || ctb_diff > LOG2_CTB_SIZE_MAX || min_cb_minus3 + 3 + ctb_diff < LOG2_CTB_SIZE_MIN ||
	    min_cb_minus3 + 3 + ctb_diff > LOG2_CTB_SIZE_MAX)
		return refuse(&sps->why, RP_PS_STREAM, "a CTB not 16 to 64");
	sps->log2_min_cb_size = (u8)(min_cb_minus3 + 3);
	sps->log2_ctb_size = (u8)(sps->log2_min_cb_size + ctb_diff);
	u32 ctb_size = 1u << sps->log2_ctb_size;
	sps->ctb_width = (sps->width + ctb_size - 1u) >> sps->log2_ctb_size;
	sps->ctb_height = (sps->height + ctb_size - 1u) >> sps->log2_ctb_size;
	return RP_PS_OK;
}

/**
 * sps_parse_transform_blocks() - The transform block sizes and the transform hierarchy's depths.
 * @b:   the reader
 * @sps: filled in, its coding block sizes read
 *
 * A transform block is 4 to 32 samples, smaller than the smallest coding
 * block at the least and no larger than the CTB at the most; the hierarchy
 * goes no deeper than from the CTB down to the smallest transform block.
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result sps_parse_transform_blocks(struct rp_bits *b, struct rp_sps *sps)
{
	u32 min_tb_minus2 = rp_bits_ue(b); /* log2_min_luma_transform_block_size_minus2 */
	u32 tb_diff = rp_bits_ue(b);       /* log2_diff_max_min_luma_transform_block_size */
	u32 depth_inter = rp_bits_ue(b);   /* max_transform_hierarchy_depth_inter */
	u32 depth_intra = rp_bits_ue(b);   /* max_transform_hierarchy_depth_intra */
	u32 log2_min_tb = min_tb_minus2 + 2, log2_max_tb = log2_min_tb + tb_diff;
	if (min_tb_minus2 > LOG2_MAX_TB_SIZE_MAX || tb_diff > LOG2_MAX_TB_SIZE_MAX || log2_min_tb >= sps->log2_min_cb_size ||
	    log2_max_tb > LOG2_MAX_TB_SIZE_MAX || log2_max_tb > sps->log2_ctb_size)
		return refuse(&sps->why, RP_PS_STREAM, "a transform block over 32, over the CTB, or not under the coding block");
	u32 depth_max = sps->log2_ctb_size - log2_min_tb;
	if (depth_inter > depth_max || depth_intra > depth_max)
		return refuse(&sps->why, RP_PS_STREAM, "a transform hierarchy depth over the CTB to transform block range");
	sps->log2_min_tb_size = (u8)log2_min_tb;
	sps->log2_max_tb_size = (u8)log2_max_tb;
	sps->v4l2.max_transform_hierarchy_depth_inter = (u8)depth_inter;
	sps->v4l2.max_transform_hierarchy_depth_intra = (u8)depth_intra;
	return RP_PS_OK;
}

/**
 * sps_parse_pcm() - The PCM samples' depths and the PCM blocks' sizes.
 * @b:   the reader
 * @sps: filled in, its bit depths and block sizes read
 *
 * A PCM sample is no deeper than the picture's; a PCM block is from the
 * smaller of the smallest coding block and 32 samples to the smaller of the
 * CTB and 32 samples (7.4.3.2.1).
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result sps_parse_pcm(struct rp_bits *b, struct rp_sps *sps)
{
	u32 depth_luma_minus1 = rp_bits_u(b, 4);   /* pcm_sample_bit_depth_luma_minus1 */
	u32 depth_chroma_minus1 = rp_bits_u(b, 4); /* pcm_sample_bit_depth_chroma_minus1 */
	u32 min_minus3 = rp_bits_ue(b);            /* log2_min_pcm_luma_coding_block_size_minus3 */
	u32 diff = rp_bits_ue(b);                  /* log2_diff_max_min_pcm_luma_coding_block_size */
	sps->pcm_loop_filter_disabled = rp_bits_flag(b);
	if (depth_luma_minus1 + 1 > sps->bit_depth_luma || depth_chroma_minus1 + 1 > sps->bit_depth_chroma)
		return refuse(&sps->why, RP_PS_STREAM, "a PCM sample deeper than the picture's");
	u32 lowest = sps->log2_min_cb_size < LOG2_MAX_IPCM_SIZE_MAX ? sps->log2_min_cb_size : LOG2_MAX_IPCM_SIZE_MAX;
	u32 highest = sps->log2_ctb_size < LOG2_MAX_IPCM_SIZE_MAX ? sps->log2_ctb_size : LOG2_MAX_IPCM_SIZE_MAX;
	if (min_minus3 > highest || diff > highest || min_minus3 + 3 < lowest || min_minus3 + 3 + diff > highest)
		return refuse(&sps->why, RP_PS_STREAM, "a PCM block under the coding block, or over 32 or the CTB");
	sps->v4l2.pcm_sample_bit_depth_luma_minus1 = (u8)depth_luma_minus1;
	sps->v4l2.pcm_sample_bit_depth_chroma_minus1 = (u8)depth_chroma_minus1;
	sps->v4l2.log2_min_pcm_luma_coding_block_size_minus3 = (u8)min_minus3;
	sps->v4l2.log2_diff_max_min_pcm_luma_coding_block_size = (u8)diff;
	return RP_PS_OK;
}

/**
 * sps_parse_coding_tools() - The scaling lists, AMP, SAO and PCM.
 * @b:   the reader
 * @sps: filled in
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result sps_parse_coding_tools(struct rp_bits *b, struct rp_sps *sps)
{
	sps->scaling_list_enabled = rp_bits_flag(b);
	if (sps->scaling_list_enabled)
	{
		sps->scaling_list_present = rp_bits_flag(b);
		if (!sps->scaling_list_present)
			rp_scaling_default(&sps->scaling);
		else if (parse_scaling_list_data(b, &sps->scaling) != RP_PS_OK)
			return refuse(&sps->why, RP_PS_STREAM, "scaling list data");
	}
	sps->amp = rp_bits_flag(b);
	sps->sao = rp_bits_flag(b);
	sps->pcm = rp_bits_flag(b);
	return sps->pcm ? sps_parse_pcm(b, sps) : RP_PS_OK;
}

/**
 * sps_parse_reference_sets() - The candidate short-term sets and long-term pictures.
 * @b:   the reader
 * @sps: filled in
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result sps_parse_reference_sets(struct rp_bits *b, struct rp_sps *sps)
{
	u32 num_st;
	if (!rp_bits_ue_at_most(b, RP_ST_RPS_MAX, &num_st)) /* num_short_term_ref_pic_sets */
		return refuse(&sps->why, RP_PS_STREAM, "over 64 short-term sets");
	sps->num_st_rps = (u8)num_st;
	for (u32 i = 0; i < num_st; i++)
		if (rp_st_rps_parse(b, &sps->st_rps[i], i, sps->st_rps, num_st) != RP_PS_OK)
			return refuse(&sps->why, RP_PS_STREAM, "a short-term reference picture set");
	sps->long_term_present = rp_bits_flag(b);
	if (!sps->long_term_present)
		return RP_PS_OK;
	u32 num_lt;
	if (!rp_bits_ue_at_most(b, RP_LT_SPS_MAX, &num_lt)) /* num_long_term_ref_pics_sps */
		return refuse(&sps->why, RP_PS_STREAM, "over 32 long-term candidates");
	sps->num_lt_sps = (u8)num_lt;
	for (u32 i = 0; i < num_lt; i++)
	{
		sps->lt_ref_pic_poc_lsb_sps[i] = (u16)rp_bits_u(b, sps->log2_max_poc_lsb);
		sps->used_by_curr_pic_lt_sps[i] = (u8)rp_bits_u(b, 1); /* used_by_curr_pic_lt_sps_flag */
	}
	return RP_PS_OK;
}

/**
 * sps_parse_tail() - The two flags before the VUI, and the VUI.
 * @b:   the reader
 * @sps: filled in, its sub-layer count read
 *
 * Return: RP_PS_OK.
 */
static enum rp_ps_result sps_parse_tail(struct rp_bits *b, struct rp_sps *sps)
{
	sps->temporal_mvp = rp_bits_flag(b);
	sps->strong_intra_smoothing = rp_bits_flag(b);
	if (rp_bits_flag(b)) /* vui_parameters_present_flag */
	{
		parse_vui_aspect_ratio(b, sps);
		parse_vui_signal(b, sps);
		parse_vui_timing(b, sps);
	}
	return RP_PS_OK;
}

/**
 * sps_parse_extensions() - The SPS's extensions: none of their tools is Main or Main10.
 * @b:   the reader
 * @sps: filled in
 *
 * A range extension with every tool off (7.3.2.2.2) is what Main and
 * Main10 do anyway, and passes.
 *
 * Return: RP_PS_OK, or RP_PS_UNSUPPORTED with the reason kept.
 */
static enum rp_ps_result sps_parse_extensions(struct rp_bits *b, struct rp_sps *sps)
{
	u32 extensions = parse_extension_flags(b);
	if ((extensions & EXTENSION_RANGE) != 0 && rp_bits_u(b, SPS_RANGE_EXTENSION_FLAGS) != 0)
		return refuse(&sps->why, RP_PS_UNSUPPORTED, "a range extension tool");
	if ((extensions & EXTENSIONS_OTHER) != 0)
		return refuse(&sps->why, RP_PS_UNSUPPORTED, "a multilayer, 3D or screen content extension");
	return RP_PS_OK;
}

/* The SPS's syntax after profile_tier_level(), in order: each step reads its part and checks what it read */
static enum rp_ps_result (*const sps_steps[])(struct rp_bits *b, struct rp_sps *sps) = {
	sps_parse_format,        sps_parse_window,         sps_parse_depths,       sps_parse_sub_layers,
	sps_parse_coding_blocks, sps_parse_transform_blocks, sps_parse_coding_tools, sps_parse_reference_sets,
	sps_parse_tail,          sps_parse_extensions,
};

/**
 * sps_fill_record() - The SPS's V4L2 structure, as FFmpeg fills it.
 * @sps: the parsed set; what the parse read straight into the structure stays
 */
static void sps_fill_record(struct rp_sps *sps)
{
	struct v4l2_ctrl_hevc_sps *v4l2 = &sps->v4l2;
	v4l2->video_parameter_set_id = sps->vps_id;
	v4l2->seq_parameter_set_id = sps->sps_id;
	v4l2->pic_width_in_luma_samples = sps->width;
	v4l2->pic_height_in_luma_samples = sps->height;
	v4l2->bit_depth_luma_minus8 = (u8)(sps->bit_depth_luma - 8);
	v4l2->bit_depth_chroma_minus8 = (u8)(sps->bit_depth_chroma - 8);
	v4l2->log2_max_pic_order_cnt_lsb_minus4 = (u8)(sps->log2_max_poc_lsb - 4);
	v4l2->sps_max_dec_pic_buffering_minus1 = (u8)(sps->max_dec_pic_buffering - 1);
	v4l2->sps_max_num_reorder_pics = (u8)sps->max_num_reorder;
	v4l2->sps_max_latency_increase_plus1 = (u8)sps->max_latency_increase_plus1;
	v4l2->log2_min_luma_coding_block_size_minus3 = (u8)(sps->log2_min_cb_size - 3);
	v4l2->log2_diff_max_min_luma_coding_block_size = (u8)(sps->log2_ctb_size - sps->log2_min_cb_size);
	v4l2->log2_min_luma_transform_block_size_minus2 = (u8)(sps->log2_min_tb_size - 2);
	v4l2->log2_diff_max_min_luma_transform_block_size = (u8)(sps->log2_max_tb_size - sps->log2_min_tb_size);
	v4l2->num_short_term_ref_pic_sets = sps->num_st_rps;
	v4l2->num_long_term_ref_pics_sps = sps->num_lt_sps;
	v4l2->chroma_format_idc = sps->chroma_format_idc;
	v4l2->sps_max_sub_layers_minus1 = (u8)top_sub_layer(sps);
	v4l2->flags = rp_flag(sps->separate_colour_plane, V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE) |
	              rp_flag(sps->scaling_list_enabled, V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED) |
	              rp_flag(sps->amp, V4L2_HEVC_SPS_FLAG_AMP_ENABLED) |
	              rp_flag(sps->sao, V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET) |
	              rp_flag(sps->pcm, V4L2_HEVC_SPS_FLAG_PCM_ENABLED) |
	              rp_flag(sps->pcm_loop_filter_disabled, V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED) |
	              rp_flag(sps->long_term_present, V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT) |
	              rp_flag(sps->temporal_mvp, V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED) |
	              rp_flag(sps->strong_intra_smoothing, V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED);
}

/**
 * sps_supported() - What this backend and the block decode: 4:2:0, 8 or 10 bits, the block's sizes.
 * @sps: the parsed set
 *
 * Return: RP_PS_OK, or RP_PS_UNSUPPORTED with the reason kept.
 */
static enum rp_ps_result sps_supported(struct rp_sps *sps)
{
	if (sps->chroma_format_idc != CHROMA_FORMAT_420)
		return refuse(&sps->why, RP_PS_UNSUPPORTED, "chroma format not 4:2:0");
	if ((sps->bit_depth_luma != RP_DEPTH_MAIN && sps->bit_depth_luma != RP_DEPTH_MAIN10) || sps->bit_depth_luma != sps->bit_depth_chroma)
		return refuse(&sps->why, RP_PS_UNSUPPORTED, "bit depth not 8 or 10, or luma and chroma depths that differ");
	if (sps->width < PICTURE_SIZE_MIN || sps->height < PICTURE_SIZE_MIN || sps->width > PICTURE_SIZE_MAX ||
	    sps->height > PICTURE_SIZE_MAX)
		return refuse(&sps->why, RP_PS_UNSUPPORTED, "a picture under 16 or over 4096 pixels");
	return RP_PS_OK;
}

/**
 * sps_parse_body() - The SPS's syntax elements, in order.
 * @b:   the reader, after the NAL unit header
 * @sps: filled in
 *
 * Return: RP_PS_OK, RP_PS_STREAM, or RP_PS_UNSUPPORTED.
 */
static enum rp_ps_result sps_parse_body(struct rp_bits *b, struct rp_sps *sps)
{
	sps->vps_id = (u8)rp_bits_u(b, 4);               /* sps_video_parameter_set_id */
	sps->max_sub_layers = (u8)(rp_bits_u(b, 3) + 1); /* sps_max_sub_layers_minus1 */
	if (sps->max_sub_layers > SUB_LAYERS_MAX)
		return refuse(&sps->why, RP_PS_STREAM, "over 7 sub-layers");
	rp_bits_skip(b, 1); /* sps_temporal_id_nesting_flag */
	skip_ptl(b, top_sub_layer(sps));
	for (u32 i = 0; i < sizeof(sps_steps) / sizeof(sps_steps[0]); i++)
	{
		enum rp_ps_result result = sps_steps[i](b, sps);
		if (result != RP_PS_OK)
			return result;
	}
	if (b->overrun)
		return refuse(&sps->why, RP_PS_STREAM, "the SPS ends early");
	return RP_PS_OK;
}

/**
 * rp_sps_parse() - An SPS from its NAL unit.
 * @sps: filled in; valid only on RP_PS_OK
 * @nal: the unit, header included
 * @len: its bytes
 *
 * Return: RP_PS_OK, RP_PS_STREAM, or RP_PS_UNSUPPORTED.
 */
enum rp_ps_result rp_sps_parse(struct rp_sps *sps, const u8 *nal, u32 len)
{
	struct rp_bits b;

	memset(sps, 0, sizeof(*sps));
	rp_bits_init(&b, nal, len, NAL_HEADER_BYTES);
	enum rp_ps_result result = sps_parse_body(&b, sps);
	if (result != RP_PS_OK)
		return result;
	sps_fill_record(sps);
	result = sps_supported(sps);
	sps->valid = result == RP_PS_OK;
	return result;
}

/* ---- the PPS (7.3.2.3.1, 7.4.3.3.1) ------------------------------------------------ */

/**
 * init_qp_minus26_min() - The lowest init_qp_minus26 a bit depth allows: -(26 + QpBdOffsetY).
 * @bit_depth_luma: BitDepthY
 *
 * Return: the value.
 */
static s32 init_qp_minus26_min(u32 bit_depth_luma)
{
	return -(RP_QP_INIT_BASE + rp_qp_bd_offset(bit_depth_luma));
}

/**
 * pps_parse_ids() - The PPS's id, its SPS's, and the flags up to cabac_init_present_flag.
 * @b:   the reader
 * @pps: filled in
 *
 * sign_data_hiding_enabled_flag goes straight into the V4L2 structure: the
 * backend itself never asks for it.
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result pps_parse_ids(struct rp_bits *b, struct rp_pps *pps)
{
	u32 pps_id = rp_bits_ue(b), sps_id = rp_bits_ue(b);
	if (pps_id >= RP_PPS_MAX || sps_id >= RP_SPS_MAX)
		return refuse(&pps->why, RP_PS_STREAM, "pps id over 63 or sps id over 15");
	pps->pps_id = (u8)pps_id;
	pps->sps_id = (u8)sps_id;
	pps->dependent_slice_segments_enabled = rp_bits_flag(b);
	pps->output_flag_present = rp_bits_flag(b);
	pps->num_extra_slice_header_bits = (u8)rp_bits_u(b, 3);
	pps->v4l2.flags |= rp_flag(rp_bits_flag(b), V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED);
	pps->cabac_init_present = rp_bits_flag(b);
	return RP_PS_OK;
}

/**
 * pps_parse_qp() - The default reference counts, the QP values and the flags among them, up to the chroma QP offsets' flag.
 * @b:   the reader
 * @pps: filled in
 *
 * init_qp_minus26 is held here to the widest range any bit depth allows,
 * so that it fits its field; rp_pps_resolve() holds it to the SPS's.
 * constrained_intra_pred_flag and transform_skip_enabled_flag go straight
 * into the V4L2 structure.
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result pps_parse_qp(struct rp_bits *b, struct rp_pps *pps)
{
	u32 l0_minus1, l1_minus1, depth = 0;
	s32 init_qp_minus26, cb_offset, cr_offset;
	if (!rp_bits_ue_at_most(b, NUM_REF_IDX_MINUS1_MAX, &l0_minus1) || !rp_bits_ue_at_most(b, NUM_REF_IDX_MINUS1_MAX, &l1_minus1))
		return refuse(&pps->why, RP_PS_STREAM, "num_ref_idx_default_active over 15");
	init_qp_minus26 = rp_bits_se(b);
	if (init_qp_minus26 < init_qp_minus26_min(8 + BIT_DEPTH_MINUS8_MAX) || init_qp_minus26 > INIT_QP_MINUS26_MAX)
		return refuse(&pps->why, RP_PS_STREAM, "init_qp_minus26 out of range");
	pps->v4l2.flags |= rp_flag(rp_bits_flag(b), V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED);
	pps->v4l2.flags |= rp_flag(rp_bits_flag(b), V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED);
	pps->cu_qp_delta_enabled = rp_bits_flag(b);
	if (pps->cu_qp_delta_enabled && !rp_bits_ue_at_most(b, CU_QP_DELTA_DEPTH_MAX, &depth)) /* diff_cu_qp_delta_depth */
		return refuse(&pps->why, RP_PS_STREAM, "diff_cu_qp_delta_depth over the CTB to coding block range");
	if (!rp_bits_se_in(b, -RP_CHROMA_QP_OFFSET_MAX, RP_CHROMA_QP_OFFSET_MAX, &cb_offset) ||
	    !rp_bits_se_in(b, -RP_CHROMA_QP_OFFSET_MAX, RP_CHROMA_QP_OFFSET_MAX, &cr_offset))
		return refuse(&pps->why, RP_PS_STREAM, "a chroma QP offset over 12");
	pps->num_ref_idx_l0_default = (u8)(l0_minus1 + 1);
	pps->num_ref_idx_l1_default = (u8)(l1_minus1 + 1);
	pps->init_qp_minus26 = (s8)init_qp_minus26;
	pps->diff_cu_qp_delta_depth = (u8)depth;
	pps->cb_qp_offset = (s8)cb_offset;
	pps->cr_qp_offset = (s8)cr_offset;
	pps->slice_chroma_qp_offsets_present = rp_bits_flag(b);
	return RP_PS_OK;
}

/**
 * pps_parse_tools() - The prediction and coding flags from weighted_pred_flag to entropy_coding_sync_enabled_flag.
 * @b:   the reader
 * @pps: filled in
 *
 * Return: RP_PS_OK.
 */
static enum rp_ps_result pps_parse_tools(struct rp_bits *b, struct rp_pps *pps)
{
	pps->weighted_pred = rp_bits_flag(b);
	pps->weighted_bipred = rp_bits_flag(b);
	pps->transquant_bypass = rp_bits_flag(b);
	pps->tiles_enabled = rp_bits_flag(b);
	pps->entropy_coding_sync = rp_bits_flag(b);
	return RP_PS_OK;
}

/**
 * parse_tile_sizes() - The sizes sent for one dimension of a tile grid not uniformly spaced: all but the last tile's.
 * @b:     the reader
 * @sizes: filled in, in CTBs
 * @count: the tiles in the dimension
 *
 * Each size is held here to what the field can take, at most a picture's
 * size in CTBs; rp_pps_resolve() holds the sum to the picture.
 *
 * Return: TRUE; FALSE for a size out of range.
 */
static BOOL parse_tile_sizes(struct rp_bits *b, u16 *sizes, u32 count)
{
	for (u32 i = 0; i + 1 < count; i++)
	{
		u32 size_minus1; /* column_width_minus1 or row_height_minus1 */
		if (!rp_bits_ue_at_most(b, TILE_SIZE_MAX - 1, &size_minus1))
			return FALSE;
		sizes[i] = (u16)(size_minus1 + 1);
	}
	return TRUE;
}

/**
 * pps_parse_tiles() - The tile grid; the sizes of a grid not uniformly spaced, but for the last column and row.
 * @b:   the reader
 * @pps: filled in, the values of a picture without tiles preset
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result pps_parse_tiles(struct rp_bits *b, struct rp_pps *pps)
{
	if (!pps->tiles_enabled)
		return RP_PS_OK;
	u32 columns = rp_bits_ue(b) + 1, rows = rp_bits_ue(b) + 1; /* num_tile_columns_minus1, num_tile_rows_minus1 */
	if (columns > RP_TILE_COLS || rows > RP_TILE_ROWS)
		return refuse(&pps->why, RP_PS_STREAM, "over 20 tile columns or 22 rows");
	pps->num_tile_columns = (u8)columns;
	pps->num_tile_rows = (u8)rows;
	pps->uniform_spacing = rp_bits_flag(b);
	if (!pps->uniform_spacing &&
	    (!parse_tile_sizes(b, pps->column_width, columns) || !parse_tile_sizes(b, pps->row_height, rows)))
		return refuse(&pps->why, RP_PS_STREAM, "a tile larger than any picture");
	pps->loop_filter_across_tiles = rp_bits_flag(b);
	return RP_PS_OK;
}

/**
 * pps_parse_loop_filters() - The loop filter across slices and the deblocking filter's control.
 * @b:   the reader
 * @pps: filled in
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result pps_parse_loop_filters(struct rp_bits *b, struct rp_pps *pps)
{
	pps->loop_filter_across_slices = rp_bits_flag(b);
	pps->deblocking_filter_control_present = rp_bits_flag(b);
	if (!pps->deblocking_filter_control_present)
		return RP_PS_OK;
	pps->deblocking_filter_override_enabled = rp_bits_flag(b);
	pps->deblocking_filter_disabled = rp_bits_flag(b);
	if (pps->deblocking_filter_disabled)
		return RP_PS_OK;
	s32 beta_offset_div2, tc_offset_div2;
	if (!rp_bits_se_in(b, -RP_DEBLOCKING_OFFSET_MAX, RP_DEBLOCKING_OFFSET_MAX, &beta_offset_div2) ||
	    !rp_bits_se_in(b, -RP_DEBLOCKING_OFFSET_MAX, RP_DEBLOCKING_OFFSET_MAX, &tc_offset_div2))
		return refuse(&pps->why, RP_PS_STREAM, "a deblocking offset over 6");
	pps->beta_offset_div2 = (s8)beta_offset_div2;
	pps->tc_offset_div2 = (s8)tc_offset_div2;
	return RP_PS_OK;
}

/**
 * pps_parse_tail() - The scaling lists and the elements up to the extensions.
 * @b:   the reader
 * @pps: filled in
 *
 * log2_parallel_merge_level is held to Linux's driver's limit here; the
 * standard's, the CTB size, waits for rp_pps_resolve().
 *
 * Return: RP_PS_OK, or RP_PS_STREAM with the reason kept.
 */
static enum rp_ps_result pps_parse_tail(struct rp_bits *b, struct rp_pps *pps)
{
	pps->scaling_list_present = rp_bits_flag(b);
	if (pps->scaling_list_present && parse_scaling_list_data(b, &pps->scaling) != RP_PS_OK)
		return refuse(&pps->why, RP_PS_STREAM, "scaling list data");
	pps->lists_modification_present = rp_bits_flag(b);
	u32 merge_minus2;
	if (!rp_bits_ue_at_most(b, LOG2_PARALLEL_MERGE_LEVEL_MAX - 2, &merge_minus2)) /* log2_parallel_merge_level_minus2 */
		return refuse(&pps->why, RP_PS_STREAM, "log2_parallel_merge_level over 6");
	pps->log2_parallel_merge_level = (u8)(merge_minus2 + 2);
	pps->slice_segment_header_extension_present = rp_bits_flag(b);
	return RP_PS_OK;
}

/**
 * pps_range_extension_in_use() - pps_range_extension() (7.3.2.3.2): whether it turns a tool on.
 * @b:   the reader
 * @pps: the set, its transform skip flag read
 *
 * The values Main and Main10 imply are all 0.  The chroma QP offset lists
 * are not read: a set that has them is refused anyway.
 *
 * Return: TRUE when a value is not 0.
 */
static BOOL pps_range_extension_in_use(struct rp_bits *b, const struct rp_pps *pps)
{
	u32 in_use = 0;
	if ((pps->v4l2.flags & V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED) != 0)
		in_use |= rp_bits_ue(b); /* log2_max_transform_skip_block_size_minus2 */
	in_use |= rp_bits_u(b, 2);   /* cross_component_prediction_enabled_flag, chroma_qp_offset_list_enabled_flag */
	if (in_use == 0)
	{
		in_use |= rp_bits_ue(b); /* log2_sao_offset_scale_luma */
		in_use |= rp_bits_ue(b); /* log2_sao_offset_scale_chroma */
	}
	return in_use != 0;
}

/**
 * pps_parse_extensions() - The PPS's extensions: none of their tools is Main or Main10.
 * @b:   the reader
 * @pps: filled in
 *
 * Return: RP_PS_OK, or RP_PS_UNSUPPORTED with the reason kept.
 */
static enum rp_ps_result pps_parse_extensions(struct rp_bits *b, struct rp_pps *pps)
{
	u32 extensions = parse_extension_flags(b);
	if ((extensions & EXTENSION_RANGE) != 0 && pps_range_extension_in_use(b, pps))
		return refuse(&pps->why, RP_PS_UNSUPPORTED, "a range extension tool");
	if ((extensions & EXTENSIONS_OTHER) != 0)
		return refuse(&pps->why, RP_PS_UNSUPPORTED, "a multilayer, 3D or screen content extension");
	return RP_PS_OK;
}

/* The PPS's syntax in order: each step reads its part and checks what it read */
static enum rp_ps_result (*const pps_steps[])(struct rp_bits *b, struct rp_pps *pps) = {
	pps_parse_ids,          pps_parse_qp,   pps_parse_tools,     pps_parse_tiles,
	pps_parse_loop_filters, pps_parse_tail, pps_parse_extensions,
};

/**
 * pps_fill_record() - The PPS's V4L2 structure, as FFmpeg fills it; the tiles' sizes come with rp_pps_resolve().
 * @pps: the parsed set; the flags the parse set in the structure stay
 */
static void pps_fill_record(struct rp_pps *pps)
{
	struct v4l2_ctrl_hevc_pps *v4l2 = &pps->v4l2;
	v4l2->pic_parameter_set_id = pps->pps_id;
	v4l2->num_extra_slice_header_bits = pps->num_extra_slice_header_bits;
	v4l2->num_ref_idx_l0_default_active_minus1 = (u8)(pps->num_ref_idx_l0_default - 1);
	v4l2->num_ref_idx_l1_default_active_minus1 = (u8)(pps->num_ref_idx_l1_default - 1);
	v4l2->init_qp_minus26 = pps->init_qp_minus26;
	v4l2->diff_cu_qp_delta_depth = pps->diff_cu_qp_delta_depth;
	v4l2->pps_cb_qp_offset = pps->cb_qp_offset;
	v4l2->pps_cr_qp_offset = pps->cr_qp_offset;
	v4l2->num_tile_columns_minus1 = (u8)(pps->num_tile_columns - 1);
	v4l2->num_tile_rows_minus1 = (u8)(pps->num_tile_rows - 1);
	v4l2->pps_beta_offset_div2 = pps->beta_offset_div2;
	v4l2->pps_tc_offset_div2 = pps->tc_offset_div2;
	v4l2->log2_parallel_merge_level_minus2 = (u8)(pps->log2_parallel_merge_level - 2);
	v4l2->flags |= rp_flag(pps->dependent_slice_segments_enabled, V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED) |
	               rp_flag(pps->output_flag_present, V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT) |
	               rp_flag(pps->cabac_init_present, V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT) |
	               rp_flag(pps->cu_qp_delta_enabled, V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED) |
	               rp_flag(pps->slice_chroma_qp_offsets_present, V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT) |
	               rp_flag(pps->weighted_pred, V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED) |
	               rp_flag(pps->weighted_bipred, V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED) |
	               rp_flag(pps->transquant_bypass, V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED) |
	               rp_flag(pps->tiles_enabled, V4L2_HEVC_PPS_FLAG_TILES_ENABLED) |
	               rp_flag(pps->entropy_coding_sync, V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED) |
	               rp_flag(pps->loop_filter_across_tiles, V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED) |
	               rp_flag(pps->loop_filter_across_slices, V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED) |
	               rp_flag(pps->deblocking_filter_override_enabled, V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED) |
	               rp_flag(pps->deblocking_filter_disabled, V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER) |
	               rp_flag(pps->lists_modification_present, V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT) |
	               rp_flag(pps->slice_segment_header_extension_present, V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT) |
	               rp_flag(pps->deblocking_filter_control_present, V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) |
	               rp_flag(pps->tiles_enabled && pps->uniform_spacing, V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING);
}

/**
 * pps_parse_body() - The PPS's syntax elements, in order.
 * @b:   the reader, after the NAL unit header
 * @pps: filled in
 *
 * Return: RP_PS_OK, RP_PS_STREAM, or RP_PS_UNSUPPORTED.
 */
static enum rp_ps_result pps_parse_body(struct rp_bits *b, struct rp_pps *pps)
{
	for (u32 i = 0; i < sizeof(pps_steps) / sizeof(pps_steps[0]); i++)
	{
		enum rp_ps_result result = pps_steps[i](b, pps);
		if (result != RP_PS_OK)
			return result;
	}
	if (b->overrun)
		return refuse(&pps->why, RP_PS_STREAM, "the PPS ends early");
	return RP_PS_OK;
}

/**
 * rp_pps_parse() - A PPS from its NAL unit; the tiles in CTBs wait for rp_pps_resolve().
 * @pps: filled in; valid only on RP_PS_OK
 * @nal: the unit, header included
 * @len: its bytes
 *
 * Return: RP_PS_OK, RP_PS_STREAM, or RP_PS_UNSUPPORTED.
 */
enum rp_ps_result rp_pps_parse(struct rp_pps *pps, const u8 *nal, u32 len)
{
	struct rp_bits b;

	memset(pps, 0, sizeof(*pps));
	rp_bits_init(&b, nal, len, NAL_HEADER_BYTES);
	pps->num_tile_columns = 1; /* what a PPS without tiles means (7.4.3.3.1) */
	pps->num_tile_rows = 1;
	pps->uniform_spacing = TRUE;
	pps->loop_filter_across_tiles = TRUE;
	enum rp_ps_result result = pps_parse_body(&b, pps);
	if (result != RP_PS_OK)
		return result;
	pps_fill_record(pps);
	if (pps->entropy_coding_sync && pps->tiles_enabled && (pps->num_tile_columns > 1 || pps->num_tile_rows > 1))
		return refuse(&pps->why, RP_PS_STREAM, "wavefronts and tiles together"); /* what Linux's driver refuses */
	pps->valid = TRUE;
	return RP_PS_OK;
}

/**
 * tile_sizes() - One dimension's tile sizes in CTBs (6.5.1).
 * @sizes:   the sizes: all filled for a uniform grid, the last one for a grid whose others were sent
 * @count:   the tiles in the dimension
 * @ctbs:    the picture's size in the dimension, in CTBs
 * @uniform: uniform_spacing_flag
 *
 * Return: TRUE; FALSE when the sizes sent leave nothing for the last tile.
 */
static BOOL tile_sizes(u16 *sizes, u32 count, u32 ctbs, BOOL uniform)
{
	if (uniform)
	{
		for (u32 i = 0; i < count; i++)
			sizes[i] = (u16)(((i + 1) * ctbs) / count - (i * ctbs) / count);
		return TRUE;
	}
	u32 sum = 0;
	for (u32 i = 0; i + 1 < count; i++)
		sum += sizes[i];
	if (sum >= ctbs)
		return FALSE;
	sizes[count - 1] = (u16)(ctbs - sum);
	return TRUE;
}

/**
 * tile_sizes_record() - One dimension's tile sizes into the V4L2 structure, each held to be at least one CTB.
 * @sizes:  the sizes in CTBs
 * @minus1: the structure's array
 * @count:  the tiles in the dimension
 * @record: the PPS has tiles; without, the structure's arrays stay 0, as FFmpeg leaves them: the driver never reads them
 *
 * Return: TRUE; FALSE for a tile of no size.
 */
static BOOL tile_sizes_record(const u16 *sizes, u8 *minus1, u32 count, BOOL record)
{
	for (u32 i = 0; i < count; i++)
	{
		if (sizes[i] == 0)
			return FALSE;
		if (record)
			minus1[i] = (u8)(sizes[i] - 1);
	}
	return TRUE;
}

/**
 * rp_pps_resolve() - What the PPS needs its SPS for: the tiles' sizes in CTBs (6.5.1), and the limits the SPS sets.
 * @pps: the set
 * @sps: the SPS it refers to
 *
 * Return: RP_PS_OK, or RP_PS_STREAM for tiles that do not fit the picture or a value over the SPS's limit.
 */
enum rp_ps_result rp_pps_resolve(struct rp_pps *pps, const struct rp_sps *sps)
{
	if (!tile_sizes(pps->column_width, pps->num_tile_columns, sps->ctb_width, pps->uniform_spacing))
		return refuse(&pps->why, RP_PS_STREAM, "tile columns wider than the picture");
	if (!tile_sizes(pps->row_height, pps->num_tile_rows, sps->ctb_height, pps->uniform_spacing))
		return refuse(&pps->why, RP_PS_STREAM, "tile rows taller than the picture");
	if (!tile_sizes_record(pps->column_width, pps->v4l2.column_width_minus1, pps->num_tile_columns, pps->tiles_enabled))
		return refuse(&pps->why, RP_PS_STREAM, "a tile column of no width");
	if (!tile_sizes_record(pps->row_height, pps->v4l2.row_height_minus1, pps->num_tile_rows, pps->tiles_enabled))
		return refuse(&pps->why, RP_PS_STREAM, "a tile row of no height");
	if (pps->diff_cu_qp_delta_depth > sps->log2_ctb_size - sps->log2_min_cb_size)
		return refuse(&pps->why, RP_PS_STREAM, "diff_cu_qp_delta_depth over the CTB to coding block range");
	if (pps->init_qp_minus26 < init_qp_minus26_min(sps->bit_depth_luma))
		return refuse(&pps->why, RP_PS_STREAM, "init_qp_minus26 under the bit depth's range");
	if (pps->log2_parallel_merge_level > sps->log2_ctb_size)
		return refuse(&pps->why, RP_PS_STREAM, "log2_parallel_merge_level over the CTB");
	pps->resolved = TRUE;
	return RP_PS_OK;
}
