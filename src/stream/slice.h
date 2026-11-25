/* SPDX-License-Identifier: GPL-2.0 */
/*
 * slice.h - the slice segment header (docs/architecture.md section 6.3).
 *
 * 7.3.6.1 to 7.3.6.3 parsed in full against the active parameter sets, the
 * inferences of 7.4.7 applied, the entry points and the extension passed
 * over, the slice data's byte offset in the NAL unit found.  A dependent
 * slice segment sends only its first fields and takes the rest from the
 * independent segment before it in the picture.
 *
 * The header feeds the record's slice parameters (record.c), the reference
 * picture set and the reference lists (rps.c), and the checks that a
 * picture's slices agree where the standard says they must (stream.c).
 */
#ifndef RP_SLICE_H
#define RP_SLICE_H

#include <exec/types.h>
#include <types.h>

#include "ps.h"
#include "v4l2_hevc_ctrls.h" /* V4L2_HEVC_SLICE_TYPE_*, V4L2_HEVC_DPB_ENTRIES_NUM_MAX */

#define RP_REF_LISTS 2                            /* the reference picture lists, L0 and L1 */
#define RP_CHROMA_COMPONENTS 2                    /* Cb and Cr, which the chroma weights go by */
#define RP_REFS_MAX V4L2_HEVC_DPB_ENTRIES_NUM_MAX /* the room for a list's references: the record's, the lists and weights are copied into it */
#define RP_ACTIVE_REFS_MAX 15                     /* num_ref_idx_lX_active_minus1 + 1 at most (7.4.7.1) */
#define RP_LT_MAX 32                              /* the long-term entries a slice may name, the SPS's candidates and its own together */
#define RP_MERGE_CANDIDATES_MAX 5                 /* MaxNumMergeCand: 1 to 5, sent as five_minus_max_num_merge_cand */

/* pred_weight_table() (7.3.6.3) by list and reference; a reference sent without weights keeps 0s, the default ones */
struct rp_weights
{
	u8 luma_log2_denom;                                                      /* luma_log2_weight_denom */
	u8 chroma_log2_denom;                                                    /* ChromaLog2WeightDenom: the luma's plus delta_chroma_log2_weight_denom */
	s8 delta_luma_weight[RP_REF_LISTS][RP_REFS_MAX];                         /* delta_luma_weight_l0/l1 */
	s8 luma_offset[RP_REF_LISTS][RP_REFS_MAX];                               /* luma_offset_l0/l1 */
	s8 delta_chroma_weight[RP_REF_LISTS][RP_REFS_MAX][RP_CHROMA_COMPONENTS]; /* delta_chroma_weight_l0/l1 */
	s8 chroma_offset[RP_REF_LISTS][RP_REFS_MAX][RP_CHROMA_COMPONENTS];       /* ChromaOffsetL0/L1, derived from delta_chroma_offset (7.4.7.3) */
};

/* A slice segment's header, its values in effect: what it sent, what it inferred, and a dependent segment's copied */
struct rp_slice_header
{
	/* the unit */
	u32 nal_type;                          /* nal_unit_type */
	u32 temporal_id;                       /* TemporalId */
	u32 data_byte_offset;                  /* the slice data's offset in the NAL unit, prevention bytes counted */
	/* what every segment sends */
	BOOL no_output_of_prior_pics;          /* no_output_of_prior_pics_flag: an IRAP picture's; FALSE for the others */
	u8 pps_id;                             /* slice_pic_parameter_set_id */
	BOOL dependent_slice_segment;          /* dependent_slice_segment_flag: what follows is the independent segment's */
	u32 slice_segment_address;             /* the segment's first CTB, in the picture's raster order */
	/* what an independent segment sends, and a dependent one copies */
	u8 slice_type;                         /* V4L2_HEVC_SLICE_TYPE_B, _P or _I: their values are the standard's (7.4.7.1, Table 7-7) */
	BOOL pic_output;                       /* pic_output_flag; TRUE when the PPS has it not sent */
	u8 colour_plane_id;                    /* colour_plane_id, with separate colour planes */
	u32 pic_order_cnt_lsb;                 /* slice_pic_order_cnt_lsb; 0 for an IDR picture */
	struct rp_st_rps st_rps;               /* the short-term set in use: the slice's own, or a copy of the SPS's it picks */
	u8 num_long_term_sps;                  /* num_long_term_sps: the long-term entries picked from the SPS's candidates, first */
	u8 num_long_term_pics;                 /* num_long_term_pics: the entries the slice sends itself, after them */
	u32 poc_lsb_lt[RP_LT_MAX];             /* PocLsbLt of each entry */
	u8 used_by_curr_pic_lt[RP_LT_MAX];     /* UsedByCurrPicLt: the current picture refers to the entry */
	u8 delta_poc_msb_present[RP_LT_MAX];   /* delta_poc_msb_present_flag: the entry names its POC's high bits too */
	s32 delta_poc_msb_cycle_lt[RP_LT_MAX]; /* DeltaPocMsbCycleLt, cumulative (7-52) */
	BOOL temporal_mvp;                     /* slice_temporal_mvp_enabled_flag */
	BOOL sao_luma;                         /* slice_sao_luma_flag */
	BOOL sao_chroma;                       /* slice_sao_chroma_flag */
	u8 num_ref_idx_active[RP_REF_LISTS];      /* num_ref_idx_l0/l1_active_minus1 + 1; 0 for a list the slice does not use */
	BOOL list_modification[RP_REF_LISTS];     /* ref_pic_list_modification_flag_l0/l1 */
	u8 list_entry[RP_REF_LISTS][RP_REFS_MAX]; /* list_entry_l0/l1: each entry's place in the current reference set */
	BOOL mvd_l1_zero;                      /* mvd_l1_zero_flag */
	BOOL cabac_init;                       /* cabac_init_flag */
	BOOL collocated_from_l0;               /* collocated_from_l0_flag; TRUE when not sent */
	u8 collocated_ref_idx;                 /* collocated_ref_idx: the collocated picture's index in its list */
	BOOL weighted;                         /* a pred_weight_table() was read, into weights */
	struct rp_weights weights;             /* the slice's weights, when weighted */
	u8 max_num_merge_cand;                 /* MaxNumMergeCand: 5 - five_minus_max_num_merge_cand */
	s8 slice_qp_delta;                     /* slice_qp_delta */
	s8 cb_qp_offset;                       /* slice_cb_qp_offset */
	s8 cr_qp_offset;                       /* slice_cr_qp_offset */
	BOOL deblocking_disabled;              /* slice_deblocking_filter_disabled_flag in effect: the PPS's when not sent */
	s8 beta_offset_div2;                   /* slice_beta_offset_div2 in effect: the PPS's when not sent */
	s8 tc_offset_div2;                     /* slice_tc_offset_div2 in effect: the PPS's when not sent */
	BOOL loop_filter_across_slices;        /* slice_loop_filter_across_slices_enabled_flag in effect: the PPS's when not sent */
	u32 num_entry_point_offsets;           /* num_entry_point_offsets; the offsets are passed over, the block finds them itself */
	u32 num_pic_total_curr;                /* NumPicTotalCurr (7-55): the pictures the current one may refer to */
};

BOOL rp_slice_parse(struct rp_slice_header *sh, const u8 *nal, u32 len, u32 nal_type, u32 temporal_id,
                    const struct rp_sps *sps_table, const struct rp_pps *pps_table, const struct rp_slice_header *independent);

#endif /* RP_SLICE_H */
