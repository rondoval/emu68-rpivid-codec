/* SPDX-License-Identifier: GPL-2.0 */
/*
 * record.h - the picture record (docs/architecture.md section 6.6): one picture
 * as the command generator and the session need it.
 *
 * It holds the V4L2 structures Raspberry Pi's command generator reads (the
 * SPS, the PPS, the scaling matrix, the decode parameters with the DPB
 * array and the Curr lists, one set of slice parameters per slice), and
 * what the session needs besides to place the picture: its number and
 * POC, its size and bit depth, whether it is shown, and the picture behind
 * each entry of the DPB array, which the session turns into frames.
 *
 * stream.c fills it in three steps: rp_record_picture() at the picture's
 * first slice, rp_rps_apply() (rps.c) right after for the DPB array and the
 * Curr lists, and rp_record_slice() for every slice, the first included.
 * The session (session_parse.c) then points the command generator's input
 * at its structures.  The slice array is the session's, kept from one
 * picture to the next.
 */
#ifndef RP_RECORD_H
#define RP_RECORD_H

#include <exec/types.h>
#include <types.h>

#include "ps.h"
#include "slice.h"
#include "v4l2_hevc_ctrls.h"

#define RP_SLICES_MAX 600        /* the most slices in a picture: the slice parameters control's size in Raspberry Pi's Linux driver (hevc_d.c) */
#define RP_COLLOCATED_NONE (~0u) /* no slice of the picture has named a collocated picture yet */

/* One picture, for the command generator and the session */
struct rp_record
{
	u32 number;           /* the picture's number in decoding order, from 1 */
	s32 poc;              /* its PicOrderCntVal */
	u32 nal_type;         /* its slices' nal_unit_type */
	BOOL irap;            /* an IRAP picture (BLA, IDR or CRA) */
	BOOL idr;             /* an IDR picture: its reference picture set is empty */
	BOOL no_rasl_output;  /* NoRaslOutputFlag: the picture starts a coded video sequence; rps.c unmarks the buffer for it */
	BOOL output;          /* PicOutputFlag: the picture is shown */
	BOOL corrupt;         /* a reference the picture uses is missing and was substituted (8.3.3) */
	u32 width;            /* the decoded picture's size in luma samples, conformance window not applied */
	u32 height;
	u32 bit_depth;        /* BitDepthY: 8 or 10, the depths an SPS is taken with (ps.c) */

	/* the command generator's structures */
	struct v4l2_ctrl_hevc_sps sps;                   /* the active SPS's */
	struct v4l2_ctrl_hevc_pps pps;                   /* the active PPS's */
	struct v4l2_ctrl_hevc_decode_params decode;      /* POC and IRAP flags; the DPB array and the Curr lists (rps.c) */
	struct v4l2_ctrl_hevc_scaling_matrix scaling;    /* the PPS's lists, else the SPS's; valid when has_scaling */
	BOOL has_scaling;                                /* the picture uses a scaling matrix */
	struct v4l2_ctrl_hevc_slice_params *slices;      /* the session's array: one entry per slice, slice_cap long */
	u32 slice_cap;                                   /* the array's length */
	u32 slice_count;                                 /* the slices taken so far */

	/* for the session: the frames of the DPB array */
	u32 ref_number[V4L2_HEVC_DPB_ENTRIES_NUM_MAX]; /* decode.dpb[i]'s picture number; 0 for a missing one: the session substitutes */

	/* what the picture's other slices are held to (7.4.7.1) */
	u32 collocated;                     /* the collocated picture as an index into decode.dpb[], from the first slice that has one; RP_COLLOCATED_NONE before */
	struct rp_slice_header first;       /* the first slice segment's header */
	struct rp_slice_header independent; /* the last independent segment's header, for the dependent ones */
};

void rp_record_clear(struct rp_record *rec);
void rp_record_picture(struct rp_record *rec, u32 number, s32 poc, const struct rp_slice_header *sh, const struct rp_sps *sps,
                       const struct rp_pps *pps, BOOL no_rasl_output, BOOL no_output_of_prior);
const char *rp_record_slice(struct rp_record *rec, const struct rp_slice_header *sh, u32 len);

#endif /* RP_RECORD_H */
