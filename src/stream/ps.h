/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ps.h - the parameter sets (docs/architecture.md section 6.2).
 *
 * An SPS and a PPS are parsed in full from their NAL units (7.3.2.2,
 * 7.3.2.3, 7.3.3, 7.3.4, 7.3.7, E.2) and refused where they break the
 * standard, where Linux's driver for the HEVC block would refuse them (its
 * try_ctrl checks), or where they go beyond what this backend decodes.
 * stream.c keeps them by id: a slice header names its PPS, the PPS its SPS.
 *
 * Each set is kept in two forms.  The V4L2 structure (v4l2) is the one the
 * picture record takes whole, in Linux's encoding: the _minus1 and _minus8
 * fields, the flags in one u64.  The fields beside it are the parser's: the
 * same values as plain numbers and BOOLs, which the stream layer and the
 * session read, and what the V4L2 structure has no room for (the buffering
 * limits per sub-layer, the derived reference sets, the conformance window,
 * the VUI).
 */
#ifndef RP_PS_H
#define RP_PS_H

#include <exec/types.h>
#include <types.h>

#include "bits.h"
#include "v4l2_hevc_ctrls.h"

#define RP_SPS_MAX 16     /* SPS ids: sps_seq_parameter_set_id is 0 to 15 */
#define RP_PPS_MAX 64     /* PPS ids: pps_pic_parameter_set_id is 0 to 63 */
#define RP_ST_RPS_MAX 64  /* the candidate short-term sets an SPS may hold: num_short_term_ref_pic_sets is 0 to 64 */
#define RP_DPB_SIZE_MAX 16 /* sps_max_dec_pic_buffering_minus1 + 1 at most: MaxDpbSize's largest (A.4.2) */
#define RP_ST_PICS_MAX 16 /* the room for the pictures a short-term set names, both sides together: the DPB size bounds them (7.4.8) */
#define RP_LT_SPS_MAX 32  /* the candidate long-term pictures an SPS may hold: num_long_term_ref_pics_sps is 0 to 32 */
#define RP_TILE_COLS 20   /* the most tile columns any level allows (MaxTileCols, Table A.8), and the V4L2 structure's room */
#define RP_TILE_ROWS 22   /* the most tile rows (MaxTileRows) */

/* QP ranges the PPS and the slice header are both held to (7.4.3.3.1, 7.4.7.1) */
#define RP_DEPTH_MAIN 8              /* the bit depths of Main and Main 10: the only ones an SPS is taken with */
#define RP_DEPTH_MAIN10 10
#define RP_QP_INIT_BASE 26           /* SliceQpY = 26 + init_qp_minus26 + slice_qp_delta */
#define RP_QP_MAX 51                 /* SliceQpY at most; its least is -QpBdOffsetY */
#define RP_QP_BD_OFFSET_PER_BIT 6    /* QpBdOffsetY = 6 * bit_depth_luma_minus8 */
#define RP_CHROMA_QP_OFFSET_MAX 12   /* a chroma QP offset, the PPS's, the slice's and their sum: -12 to 12 */
#define RP_DEBLOCKING_OFFSET_MAX 6   /* beta_offset_div2, tc_offset_div2, the PPS's and the slice's: -6 to 6 */

/* What a parse answers */
enum rp_ps_result : s8
{
	RP_PS_OK,          /* the set parsed and is kept */
	RP_PS_STREAM,      /* the unit breaks the standard, or the reader ran out */
	RP_PS_UNSUPPORTED, /* within the standard, beyond this backend or the HEVC block */
};

/* A short-term reference picture set, derived (7.4.8): the pictures before and after the current one, by POC */
struct rp_st_rps
{
	u8 num_negative;                  /* NumNegativePics: the pictures before, in delta_poc_s0[] */
	u8 num_positive;                  /* NumPositivePics: the pictures after, in delta_poc_s1[] */
	s32 delta_poc_s0[RP_ST_PICS_MAX]; /* DeltaPocS0: each one's POC less the current picture's, negative, the nearest first */
	s32 delta_poc_s1[RP_ST_PICS_MAX]; /* DeltaPocS1: the same for the pictures after, positive */
	u8 used_s0[RP_ST_PICS_MAX];       /* UsedByCurrPicS0: the current picture refers to it; else it is kept for later ones */
	u8 used_s1[RP_ST_PICS_MAX];       /* UsedByCurrPicS1: the same for the pictures after */
};

/* A sequence parameter set (7.3.2.2), with what its VUI says that the session reports */
struct rp_sps
{
	BOOL valid;                          /* the slot holds a set that parsed */
	const char *why;                     /* what the set was refused for, when the parse was not RP_PS_OK */
	struct v4l2_ctrl_hevc_sps v4l2;      /* the record's copy, complete */
	/* the parser's form */
	u8 sps_id;                           /* sps_seq_parameter_set_id */
	u8 vps_id;                           /* sps_video_parameter_set_id: for the record only, as no VPS is used */
	u8 max_sub_layers;                   /* sps_max_sub_layers_minus1 + 1 */
	u8 chroma_format_idc;                /* 1 for 4:2:0, the only format decoded */
	u8 chroma_array_type;                /* ChromaArrayType: chroma_format_idc, or 0 with separate colour planes */
	BOOL separate_colour_plane;          /* separate_colour_plane_flag */
	u16 width;                           /* pic_width_in_luma_samples */
	u16 height;                          /* pic_height_in_luma_samples */
	u32 conf_left;                       /* the conformance window's offsets, in luma samples: conf_win_left_offset * SubWidthC */
	u32 conf_right;                      /* conf_win_right_offset * SubWidthC */
	u32 conf_top;                        /* conf_win_top_offset * SubHeightC */
	u32 conf_bottom;                     /* conf_win_bottom_offset * SubHeightC */
	u8 bit_depth_luma;                   /* BitDepthY: 8 or 10 */
	u8 bit_depth_chroma;                 /* BitDepthC: a set where it differs from the luma's is refused */
	u8 log2_max_poc_lsb;                 /* log2_max_pic_order_cnt_lsb_minus4 + 4: the bits of slice_pic_order_cnt_lsb */
	/* the buffering limits of the highest sub-layer, HighestTid, as every sub-layer is decoded (C.5.2.2);
	 * the lower ones' are read through */
	u32 max_dec_pic_buffering;           /* sps_max_dec_pic_buffering_minus1 + 1: the DPB size */
	u32 max_num_reorder;                 /* sps_max_num_reorder_pics: the most pictures that may wait for output */
	u32 max_latency_increase_plus1;      /* sps_max_latency_increase_plus1: 0 for no latency limit */
	u8 log2_min_cb_size;                 /* MinCbLog2SizeY: the smallest coding block */
	u8 log2_ctb_size;                    /* CtbLog2SizeY: the coding tree block, 16 to 64 samples */
	u8 log2_min_tb_size;                 /* MinTbLog2SizeY: the smallest transform block */
	u8 log2_max_tb_size;                 /* MaxTbLog2SizeY: the largest */
	u32 ctb_width;                       /* PicWidthInCtbsY */
	u32 ctb_height;                      /* PicHeightInCtbsY */
	BOOL scaling_list_enabled;           /* scaling_list_enabled_flag */
	BOOL scaling_list_present;           /* sps_scaling_list_data_present_flag */
	struct v4l2_ctrl_hevc_scaling_matrix scaling; /* the SPS's lists; the default ones when enabled and not sent */
	BOOL amp;                            /* amp_enabled_flag: asymmetric motion partitions */
	BOOL sao;                            /* sample_adaptive_offset_enabled_flag */
	BOOL pcm;                            /* pcm_enabled_flag */
	BOOL pcm_loop_filter_disabled;       /* pcm_loop_filter_disabled_flag */
	u8 num_st_rps;                       /* num_short_term_ref_pic_sets */
	struct rp_st_rps st_rps[RP_ST_RPS_MAX]; /* the candidate short-term sets, derived */
	BOOL long_term_present;              /* long_term_ref_pics_present_flag */
	u8 num_lt_sps;                       /* num_long_term_ref_pics_sps */
	u16 lt_ref_pic_poc_lsb_sps[RP_LT_SPS_MAX];  /* the candidate long-term pictures' POC LSBs */
	u8 used_by_curr_pic_lt_sps[RP_LT_SPS_MAX];  /* and whether a picture that picks one refers to it */
	BOOL temporal_mvp;                   /* sps_temporal_mvp_enabled_flag */
	BOOL strong_intra_smoothing;         /* strong_intra_smoothing_enabled_flag */
	/* from the VUI (E.2.1), for the stream info */
	u16 sar_width;                       /* the sample aspect ratio, from aspect_ratio_idc or sent; 0 when unspecified */
	u16 sar_height;                      /* the same; 0 when unspecified */
	BOOL video_signal_present;           /* video_signal_type_present_flag: the range and the colour description follow */
	BOOL full_range;                     /* video_full_range_flag */
	BOOL colour_description_present;     /* colour_description_present_flag: the matrix follows */
	u8 matrix_coeffs;                    /* matrix_coeffs: the YUV matrix, numbered as Rec. ITU-T H.273 numbers it (1 is BT.709) */
	BOOL field_seq;                      /* field_seq_flag: the pictures are fields */
	BOOL timing_present;                 /* vui_timing_info_present_flag: the frame rate follows */
	u32 num_units_in_tick;               /* vui_num_units_in_tick: the frame rate is time_scale / num_units_in_tick */
	u32 time_scale;                      /* vui_time_scale */
};

/* A picture parameter set (7.3.2.3) */
struct rp_pps
{
	BOOL valid;                          /* the slot holds a set that parsed */
	const char *why;                     /* what the set was refused for, when the parse or the resolve was not RP_PS_OK */
	struct v4l2_ctrl_hevc_pps v4l2;      /* the record's copy; the tiles' widths and heights filled by rp_pps_resolve() */
	/* the parser's form */
	u8 pps_id;                           /* pps_pic_parameter_set_id */
	u8 sps_id;                           /* pps_seq_parameter_set_id: the SPS it needs */
	BOOL dependent_slice_segments_enabled; /* dependent_slice_segments_enabled_flag */
	BOOL output_flag_present;            /* output_flag_present_flag: the slices carry pic_output_flag */
	BOOL cabac_init_present;             /* cabac_init_present_flag: the slices carry cabac_init_flag */
	u8 num_extra_slice_header_bits;      /* the reserved bits a slice header carries, passed over */
	u8 num_ref_idx_l0_default;           /* num_ref_idx_l0_default_active_minus1 + 1 */
	u8 num_ref_idx_l1_default;           /* num_ref_idx_l1_default_active_minus1 + 1 */
	s8 init_qp_minus26;                  /* init_qp_minus26 */
	BOOL cu_qp_delta_enabled;            /* cu_qp_delta_enabled_flag */
	u8 diff_cu_qp_delta_depth;           /* diff_cu_qp_delta_depth */
	s8 cb_qp_offset;                     /* pps_cb_qp_offset */
	s8 cr_qp_offset;                     /* pps_cr_qp_offset */
	BOOL slice_chroma_qp_offsets_present; /* pps_slice_chroma_qp_offsets_present_flag */
	BOOL weighted_pred;                  /* weighted_pred_flag: P slices carry weights */
	BOOL weighted_bipred;                /* weighted_bipred_flag: B slices carry weights */
	BOOL transquant_bypass;              /* transquant_bypass_enabled_flag */
	BOOL tiles_enabled;                  /* tiles_enabled_flag */
	BOOL entropy_coding_sync;            /* entropy_coding_sync_enabled_flag: wavefront rows */
	BOOL uniform_spacing;                /* uniform_spacing_flag: the tile sizes are derived, by rp_pps_resolve() */
	BOOL loop_filter_across_tiles;       /* loop_filter_across_tiles_enabled_flag */
	u8 num_tile_columns;                 /* num_tile_columns_minus1 + 1; 1 without tiles */
	u8 num_tile_rows;                    /* num_tile_rows_minus1 + 1; 1 without tiles */
	u16 column_width[RP_TILE_COLS];      /* each tile column's width in CTBs: sent, or from rp_pps_resolve() */
	u16 row_height[RP_TILE_ROWS];        /* each tile row's height in CTBs, the same way */
	BOOL loop_filter_across_slices;      /* pps_loop_filter_across_slices_enabled_flag */
	BOOL deblocking_filter_control_present; /* deblocking_filter_control_present_flag */
	BOOL deblocking_filter_override_enabled; /* deblocking_filter_override_enabled_flag: a slice may change the filter */
	BOOL deblocking_filter_disabled;     /* pps_deblocking_filter_disabled_flag */
	s8 beta_offset_div2;                 /* pps_beta_offset_div2 */
	s8 tc_offset_div2;                   /* pps_tc_offset_div2 */
	BOOL scaling_list_present;           /* pps_scaling_list_data_present_flag: its lists replace the SPS's */
	struct v4l2_ctrl_hevc_scaling_matrix scaling; /* those lists, when present */
	BOOL lists_modification_present;     /* lists_modification_present_flag */
	u8 log2_parallel_merge_level;        /* log2_parallel_merge_level_minus2 + 2 */
	BOOL slice_segment_header_extension_present; /* slice_segment_header_extension_present_flag */
	BOOL resolved;                       /* rp_pps_resolve() ran for the SPS it names: a slice refuses a PPS that was not */
};

/**
 * rp_qp_bd_offset() - QpBdOffsetY (7-4): how far below 0 a bit depth lets the QP go.
 * @bit_depth_luma: BitDepthY
 *
 * Return: 6 * (BitDepthY - 8).
 */
static inline s32 rp_qp_bd_offset(u32 bit_depth_luma)
{
	return (s32)(RP_QP_BD_OFFSET_PER_BIT * (bit_depth_luma - 8));
}

/**
 * rp_flag() - A flag bit of a V4L2 structure, when its condition holds.
 * @set: the condition
 * @bit: the V4L2_HEVC_*_FLAG_* bit
 *
 * Return: @bit when @set, else 0.
 */
static inline u64 rp_flag(BOOL set, u64 bit)
{
	return set ? bit : 0;
}

enum rp_ps_result rp_sps_parse(struct rp_sps *sps, const u8 *nal, u32 len);
enum rp_ps_result rp_pps_parse(struct rp_pps *pps, const u8 *nal, u32 len);
enum rp_ps_result rp_pps_resolve(struct rp_pps *pps, const struct rp_sps *sps);
enum rp_ps_result rp_st_rps_parse(struct rp_bits *b, struct rp_st_rps *out, u32 idx, const struct rp_st_rps *sets,
                                  u32 num_sets);
void rp_scaling_default(struct v4l2_ctrl_hevc_scaling_matrix *matrix);

#endif /* RP_PS_H */
