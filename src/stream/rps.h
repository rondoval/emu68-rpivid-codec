/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rps.h - the picture order count (8.3.1), the reference picture set and
 * the marking of the buffer (8.3.2), the reference lists (8.3.4): the
 * architecture's section 6.3, steps 2 to 4.
 *
 * At a picture's first slice, stream.c derives its POC with
 * rp_poc_derive(), starts its record with it (rp_record_picture()), and
 * applies the set with rp_rps_apply(), which first marks every picture
 * unused for a picture that starts a sequence.  That needs the record's
 * POC, IDR flag and NoRaslOutputFlag, and must come before the output
 * process of C.5.2.2, which goes by the marking it leaves.  For each P or
 * B slice, record.c then builds the slice's lists with rp_ref_list() from
 * the Curr lists rp_rps_apply() filled.
 */
#ifndef RP_RPS_H
#define RP_RPS_H

#include <exec/types.h>
#include <types.h>

#include "dpb.h"
#include "record.h"
#include "slice.h"

BOOL rp_poc_derive(s32 prev_tid0_poc, u32 log2_max_poc_lsb, u32 poc_lsb, BOOL irap_no_rasl, s32 *poc);
void rp_rps_apply(struct rp_record *rec, struct rp_dpb *dpb, const struct rp_slice_header *sh, const struct rp_sps *sps);
BOOL rp_ref_list(const struct rp_record *rec, const struct rp_slice_header *sh, u32 list, u8 ref_idx[V4L2_HEVC_DPB_ENTRIES_NUM_MAX]);

#endif /* RP_RPS_H */
