/* SPDX-License-Identifier: GPL-2.0 */
/*
 * record.c - the picture record (record.h), filled as stream.c goes
 * through a picture.
 *
 * rp_record_clear() empties it at the first slice, keeping the session's
 * slice array.  rp_record_picture() then fills the picture's part: its
 * identity and flags, its size, the active sets' V4L2 structures and the
 * scaling matrix, the decode parameters' POC and flags.  rps.c's
 * rp_rps_apply() fills the DPB array and the Curr lists next.
 * rp_record_slice() appends each slice's parameters: the header's values,
 * the reference lists through rps.c's rp_ref_list(), the weight table as
 * the command generator wants it.
 *
 * The V4L2 fields Raspberry Pi's command generator never reads stay 0: the
 * decode parameters' short_term_ref_pic_set_size,
 * long_term_ref_pic_set_size and num_delta_pocs_of_ref_rps_idx.
 */
#include <memory.h>

#include "bits.h"
#include "nal.h"
#include "record.h"
#include "rps.h"

/**
 * rp_record_clear() - The record emptied for a new picture, its slice array kept.
 * @rec: the record
 */
void rp_record_clear(struct rp_record *rec)
{
	struct v4l2_ctrl_hevc_slice_params *slices = rec->slices;
	u32 slice_cap = rec->slice_cap;
	memset(rec, 0, sizeof(*rec));
	rec->slices = slices;
	rec->slice_cap = slice_cap;
}

/**
 * rp_record_picture() - The picture's part of the record, at its first slice.
 * @rec:                the record, cleared (rp_record_clear())
 * @number:             the picture's number in decoding order
 * @poc:                its PicOrderCntVal
 * @sh:                 the first slice's header
 * @sps:                the active SPS
 * @pps:                the active PPS
 * @no_rasl_output:     NoRaslOutputFlag
 * @no_output_of_prior: NoOutputOfPriorPicsFlag as derived (1 for a CRA with NoRaslOutputFlag, else as sent)
 *
 * A scaling matrix is given only when the SPS enables scaling lists, as
 * the command generator goes by the SPS's flag: the PPS's lists when it
 * sends them, else the SPS's (the defaults when the SPS sends none).  A
 * PPS that sends lists for an SPS without them breaks 7.4.3.3.1, and the
 * block decodes its pictures flat either way.
 */
void rp_record_picture(struct rp_record *rec, u32 number, s32 poc, const struct rp_slice_header *sh, const struct rp_sps *sps,
                       const struct rp_pps *pps, BOOL no_rasl_output, BOOL no_output_of_prior)
{
	rec->number = number;
	rec->poc = poc;
	rec->nal_type = sh->nal_type;
	rec->irap = nal_is_irap(sh->nal_type);
	rec->idr = nal_is_idr(sh->nal_type);
	rec->no_rasl_output = no_rasl_output;
	rec->output = sh->pic_output;
	rec->width = sps->width;
	rec->height = sps->height;
	rec->bit_depth = sps->bit_depth_luma;
	rec->sps = sps->v4l2;
	rec->pps = pps->v4l2;
	if (sps->scaling_list_enabled)
	{
		const struct v4l2_ctrl_hevc_scaling_matrix *matrix = pps->scaling_list_present ? &pps->scaling : &sps->scaling;
		rec->scaling = *matrix;
		rec->has_scaling = TRUE;
	}
	rec->decode.pic_order_cnt_val = poc;
	rec->decode.flags = rp_flag(rec->irap, V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC) |
	                    rp_flag(rec->idr, V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC) |
	                    rp_flag(no_output_of_prior, V4L2_HEVC_DECODE_PARAM_FLAG_NO_OUTPUT_OF_PRIOR);
	rec->collocated = RP_COLLOCATED_NONE;
	rec->first = *sh;
	rec->independent = *sh;
}

/**
 * list_weights() - One list's weights from the header's into the record's table.
 * @table:   the record's table
 * @weights: the header's weights
 * @list:    0 or 1
 * @count:   active references in the list
 */
static void list_weights(struct v4l2_hevc_pred_weight_table *table, const struct rp_weights *weights, u32 list, u32 count)
{
	s8 *luma_weight = list == 0 ? table->delta_luma_weight_l0 : table->delta_luma_weight_l1;
	s8 *luma_offset = list == 0 ? table->luma_offset_l0 : table->luma_offset_l1;
	s8 (*chroma_weight)[RP_CHROMA_COMPONENTS] = list == 0 ? table->delta_chroma_weight_l0 : table->delta_chroma_weight_l1;
	s8 (*chroma_offset)[RP_CHROMA_COMPONENTS] = list == 0 ? table->chroma_offset_l0 : table->chroma_offset_l1;
	for (u32 i = 0; i < count && i < RP_ACTIVE_REFS_MAX; i++)
	{
		luma_weight[i] = weights->delta_luma_weight[list][i];
		luma_offset[i] = weights->luma_offset[list][i];
		for (u32 component = 0; component < RP_CHROMA_COMPONENTS; component++)
		{
			chroma_weight[i][component] = weights->delta_chroma_weight[list][i][component];
			chroma_offset[i][component] = weights->chroma_offset[list][i][component];
		}
	}
}

/**
 * slice_weights() - The slice's weight table, as the command generator wants it.
 * @sp: the slice's parameters
 * @sh: the slice's header, a pred_weight_table() read
 */
static void slice_weights(struct v4l2_ctrl_hevc_slice_params *sp, const struct rp_slice_header *sh)
{
	struct v4l2_hevc_pred_weight_table *table = &sp->pred_weight_table;
	const struct rp_weights *weights = &sh->weights;
	table->luma_log2_weight_denom = weights->luma_log2_denom;
	table->delta_chroma_log2_weight_denom = (s8)((s32)weights->chroma_log2_denom - weights->luma_log2_denom);
	list_weights(table, weights, 0, sh->num_ref_idx_active[0]);
	if (sh->slice_type == V4L2_HEVC_SLICE_TYPE_B)
		list_weights(table, weights, 1, sh->num_ref_idx_active[1]);
}

/**
 * slice_collocated() - The slice's collocated picture, held to be the picture's one (8.3.5, 7.4.7.1).
 * @rec: the record
 * @sh:  the slice's header, temporal MVP on, its collocated_ref_idx within its list (slice.c)
 * @sp:  the slice's parameters, its reference lists built
 *
 * Return: NULL; else why the picture is dropped.
 */
static const char *slice_collocated(struct rp_record *rec, const struct rp_slice_header *sh,
                                    const struct v4l2_ctrl_hevc_slice_params *sp)
{
	u32 list = sh->slice_type == V4L2_HEVC_SLICE_TYPE_B && !sh->collocated_from_l0 ? 1 : 0;
	u32 dpb_index = list == 0 ? sp->ref_idx_l0[sh->collocated_ref_idx] : sp->ref_idx_l1[sh->collocated_ref_idx];
	if (rec->collocated == RP_COLLOCATED_NONE)
		rec->collocated = dpb_index;
	else if (rec->collocated != dpb_index)
		return "a slice with another collocated picture than the first";
	return NULL;
}

/**
 * slice_references() - A P or B slice's reference lists and its collocated picture.
 * @rec: the record
 * @sh:  the slice's header
 * @sp:  the slice's parameters
 *
 * Return: NULL; else why the picture is dropped.
 */
static const char *slice_references(struct rp_record *rec, const struct rp_slice_header *sh, struct v4l2_ctrl_hevc_slice_params *sp)
{
	if (!rp_ref_list(rec, sh, 0, sp->ref_idx_l0))
		return "reference list 0 cannot be built";
	if (sh->slice_type == V4L2_HEVC_SLICE_TYPE_B && !rp_ref_list(rec, sh, 1, sp->ref_idx_l1))
		return "reference list 1 cannot be built";
	return sh->temporal_mvp ? slice_collocated(rec, sh, sp) : NULL;
}

/**
 * slice_params() - A slice's V4L2 parameters from its header, but for the references and the weights.
 * @sp:  filled in
 * @sh:  the slice's header
 * @poc: the picture's POC
 * @len: the NAL unit's bytes
 */
static void slice_params(struct v4l2_ctrl_hevc_slice_params *sp, const struct rp_slice_header *sh, s32 poc, u32 len)
{
	BOOL intra = sh->slice_type == V4L2_HEVC_SLICE_TYPE_I;
	memset(sp, 0, sizeof(*sp));
	sp->bit_size = len * RP_BITS_PER_BYTE;
	sp->data_byte_offset = sh->data_byte_offset;
	sp->num_entry_point_offsets = sh->num_entry_point_offsets;
	sp->nal_unit_type = (u8)sh->nal_type;
	sp->nuh_temporal_id_plus1 = (u8)(sh->temporal_id + 1);
	sp->slice_type = sh->slice_type;
	sp->colour_plane_id = sh->colour_plane_id;
	sp->slice_pic_order_cnt = poc;
	if (sh->num_ref_idx_active[0] != 0)
		sp->num_ref_idx_l0_active_minus1 = (u8)(sh->num_ref_idx_active[0] - 1);
	if (sh->num_ref_idx_active[1] != 0)
		sp->num_ref_idx_l1_active_minus1 = (u8)(sh->num_ref_idx_active[1] - 1);
	if (sh->temporal_mvp)
		sp->collocated_ref_idx = sh->collocated_ref_idx;
	if (!intra)
		sp->five_minus_max_num_merge_cand = (u8)(RP_MERGE_CANDIDATES_MAX - sh->max_num_merge_cand);
	sp->slice_qp_delta = sh->slice_qp_delta;
	sp->slice_cb_qp_offset = sh->cb_qp_offset;
	sp->slice_cr_qp_offset = sh->cr_qp_offset;
	sp->slice_beta_offset_div2 = sh->beta_offset_div2;
	sp->slice_tc_offset_div2 = sh->tc_offset_div2;
	sp->pic_struct = 0; /* frames: no picture timing SEI is read (docs/architecture.md section 15) */
	sp->slice_segment_addr = sh->slice_segment_address;
	sp->flags = rp_flag(sh->sao_luma, V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA) |
	            rp_flag(sh->sao_chroma, V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA) |
	            rp_flag(sh->temporal_mvp, V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED) |
	            rp_flag(sh->mvd_l1_zero, V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO) |
	            rp_flag(sh->cabac_init, V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT) |
	            rp_flag(sh->collocated_from_l0, V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0) |
	            rp_flag(sh->deblocking_disabled, V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED) |
	            rp_flag(sh->loop_filter_across_slices, V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED) |
	            rp_flag(sh->dependent_slice_segment, V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT);
}

/**
 * rp_record_slice() - A slice's parameters appended to the record.
 * @rec: the record
 * @sh:  the slice's header
 * @len: the NAL unit's bytes
 *
 * Return: NULL; else why the slice cannot be taken (the picture is
 * dropped): no room, a reference list that cannot be built, a collocated
 * picture not in its list.
 */
const char *rp_record_slice(struct rp_record *rec, const struct rp_slice_header *sh, u32 len)
{
	if (rec->slice_count >= rec->slice_cap)
		return "more slices than the record holds";
	struct v4l2_ctrl_hevc_slice_params *sp = &rec->slices[rec->slice_count];
	slice_params(sp, sh, rec->poc, len);
	if (sh->slice_type != V4L2_HEVC_SLICE_TYPE_I)
	{
		const char *why = slice_references(rec, sh, sp);
		if (why != NULL)
			return why;
	}
	if (sh->weighted)
		slice_weights(sp, sh);
	rec->slice_count++;
	return NULL;
}
