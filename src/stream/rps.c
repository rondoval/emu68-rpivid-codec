/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rps.c - POC, the reference picture set and the reference lists (rps.h).
 * The record's DPB array is filled here as FFmpeg's hook fills it: every
 * reference picture after the marking, with its POC and long-term flag.
 * The Curr lists hold indices into that array, as V4L2 defines them (Linux's
 * hantro driver reads them so), and the lists of 8.3.4 are built from them.
 */
#include <memory.h>

#include "rps.h"

#define DPB_INDEX_NONE 0xffu        /* a Curr list entry whose picture found no room in the DPB array (part_curr()) */
#define NOT_IN_ARRAY (~0u)          /* dpb_index_of(), dpb_add(): the picture has no entry in the record's DPB array */
#define CURR_LISTS 3                /* StCurrBefore, StCurrAfter, LtCurr */
#define TEMP_LIST_MAX 32            /* RefPicListTemp's room: NumRpsCurrTempList is Max(15 active references at most, NumPicTotalCurr), which A.4.1 e) bounds to 8 */
#define POC_MIN (-2147483647LL - 1) /* PicOrderCntVal is -2^31 to 2^31 - 1 (8.3.1) */
#define POC_MAX 2147483647LL

/**
 * poc_fits() - A POC worked out in 64 bits, held to PicOrderCntVal's range (8.3.1).
 * @wide: the POC
 * @poc:  set to it when it fits; else to POC_MIN, which no picture has
 *
 * The standard keeps every PicOrderCntVal, and so every difference a
 * stream may send, within 32 bits; a stream past that would overflow a
 * 32-bit sum.
 *
 * Return: TRUE when it fits.
 */
static BOOL poc_fits(s64 wide, s32 *poc)
{
	BOOL fits = wide >= POC_MIN && wide <= POC_MAX;
	*poc = fits ? (s32)wide : (s32)POC_MIN;
	return fits;
}

/**
 * rp_poc_derive() - PicOrderCntVal (8.3.1).
 * @prev_tid0_poc:    PicOrderCntVal of prevTid0Pic
 * @log2_max_poc_lsb: log2_max_pic_order_cnt_lsb_minus4 + 4
 * @poc_lsb:          slice_pic_order_cnt_lsb
 * @irap_no_rasl:     the picture is an IRAP with NoRaslOutputFlag: the MSB is 0
 * @poc:              set to the POC
 *
 * PicOrderCntMsb steps by MaxPicOrderCntLsb each time the LSB wraps, so a
 * long enough stream with no IRAP to start it afresh runs it past 32 bits.
 *
 * Return: TRUE; FALSE when the POC is out of range (the picture is
 * dropped).
 */
BOOL rp_poc_derive(s32 prev_tid0_poc, u32 log2_max_poc_lsb, u32 poc_lsb, BOOL irap_no_rasl, s32 *poc)
{
	s64 max_lsb = (s64)1 << log2_max_poc_lsb;
	s64 lsb = poc_lsb;
	s64 prev_lsb = prev_tid0_poc & (max_lsb - 1);
	s64 prev_msb = prev_tid0_poc - prev_lsb;
	s64 msb;
	if (irap_no_rasl)
		msb = 0;
	else if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2)
		msb = prev_msb + max_lsb;
	else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2)
		msb = prev_msb - max_lsb;
	else
		msb = prev_msb;
	return poc_fits(msb + lsb, poc);
}

/**
 * dpb_index_of() - Where a picture is in the record's DPB array.
 * @rec:    the record
 * @number: the picture
 * @poc:    its POC, for a missing one (number 0)
 *
 * Return: the index, or NOT_IN_ARRAY when the array lacks it.
 */
static u32 dpb_index_of(const struct rp_record *rec, u32 number, s32 poc)
{
	for (u32 i = 0; i < rec->decode.num_active_dpb_entries; i++)
		if (rec->ref_number[i] == number && (number != 0 || rec->decode.dpb[i].pic_order_cnt_val == poc))
			return i;
	return NOT_IN_ARRAY;
}

/**
 * dpb_add() - A picture joins the record's DPB array, once.
 * @rec:       the record
 * @entry:     its entry in the buffer, NULL for a missing picture
 * @poc:       its POC
 * @long_term: marked long-term
 *
 * A missing picture the current one uses gets an entry with number 0: the
 * session has no frame for it, so the command generator leaves the
 * picture's own frame in its place (what it held before stands in for
 * 8.3.3's generated picture), and the picture is marked corrupt.
 *
 * Return: its index, NOT_IN_ARRAY when the array is full.
 */
static u32 dpb_add(struct rp_record *rec, const struct rp_dpb_entry *entry, s32 poc, BOOL long_term)
{
	struct v4l2_ctrl_hevc_decode_params *decode = &rec->decode;
	u32 number = entry != NULL ? entry->number : 0;
	u32 index = dpb_index_of(rec, number, poc);
	if (index != NOT_IN_ARRAY)
		return index;
	if (decode->num_active_dpb_entries >= V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
		return NOT_IN_ARRAY;
	index = decode->num_active_dpb_entries++;
	decode->dpb[index].timestamp = number;
	decode->dpb[index].flags = long_term ? V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE : 0;
	decode->dpb[index].pic_order_cnt_val = poc;
	rec->ref_number[index] = number;
	if (entry == NULL)
		rec->corrupt = TRUE;
	return index;
}

/* One part of the reference picture set, as the slice header names it: the short-term pictures before or after, or the long-term ones */
struct rps_part
{
	struct rp_dpb_entry *found[RP_LT_MAX]; /* the buffer's picture for each, NULL when it is missing */
	s32 poc[RP_LT_MAX];                    /* each one's POC: the picture's when found, else the one the header names */
	const u8 *used;                        /* used_by_curr_pic: the current picture references it (Curr); else it is kept for later ones (Foll) */
	u32 count;                             /* the pictures in the part */
	BOOL long_term;                        /* the long-term part: its pictures enter the DPB array marked long-term */
};

/* A part's arrays, sized for the long-term part, hold a short-term side as well */
_Static_assert(RP_ST_PICS_MAX <= RP_LT_MAX, "a short-term side fits a part's arrays");

/* A Curr list in the record's decode parameters: StCurrBefore, StCurrAfter or LtCurr, as indices into its DPB array */
struct curr_list
{
	u8 *indices; /* poc_st_curr_before, poc_st_curr_after or poc_lt_curr */
	u8 *count;   /* the matching num_poc_* */
};

/**
 * part_keep() - A part's picture found in the buffer stays a reference.
 * @part:   the part
 * @i:      the picture's place in it
 * @dpb:    the buffer
 * @entry:  the entry, NULL for a missing picture
 * @in_set: per buffer entry: named by the set
 */
static void part_keep(struct rps_part *part, u32 i, const struct rp_dpb *dpb, struct rp_dpb_entry *entry, BOOL *in_set)
{
	part->found[i] = entry;
	if (entry == NULL)
		return;
	part->poc[i] = entry->poc;
	in_set[entry - dpb->entries] = TRUE;
}

/**
 * long_term_find() - The long-term pictures the header names, found and marked long-term (8-5, 8-6).
 * @part:   filled in
 * @dpb:    the buffer
 * @rec:    the record, its POC set
 * @sh:     the first slice's header
 * @sps:    the active SPS
 * @in_set: per buffer entry: named by the set
 *
 * By the POC's low bits, unless the header sends the MSB.
 */
static void long_term_find(struct rps_part *part, struct rp_dpb *dpb, const struct rp_record *rec, const struct rp_slice_header *sh,
                           const struct rp_sps *sps, BOOL *in_set)
{
	s32 max_lsb = 1 << sps->log2_max_poc_lsb;
	part->used = sh->used_by_curr_pic_lt;
	part->count = (u32)sh->num_long_term_sps + sh->num_long_term_pics;
	part->long_term = TRUE;
	for (u32 i = 0; i < part->count; i++)
	{
		s64 poc = sh->poc_lsb_lt[i];
		s32 mask = max_lsb - 1;
		if (sh->delta_poc_msb_present[i])
		{
			/* (8-5) in 64 bits: the cycle times MaxPicOrderCntLsb may reach a POC's whole range back */
			poc += (s64)rec->poc - (s64)sh->delta_poc_msb_cycle_lt[i] * max_lsb - (rec->poc & (max_lsb - 1));
			mask = -1;
		}
		BOOL fits = poc_fits(poc, &part->poc[i]); /* else no picture has it: the entry is missing */
		struct rp_dpb_entry *entry = fits ? rp_dpb_find_reference(dpb, part->poc[i], mask) : NULL;
		if (entry != NULL)
			entry->ref = RP_REF_LONG;
		part_keep(part, i, dpb, entry, in_set);
	}
}

/**
 * short_term_find() - One side's short-term pictures, found by their full POC.
 * @part:      filled in
 * @dpb:       the buffer, the long-term pictures marked already so that this search never takes them
 * @poc:       the current picture's POC
 * @delta_poc: the side's deltas
 * @used:      the side's used flags
 * @count:     the pictures on the side
 * @in_set:    per buffer entry: named by the set
 */
static void short_term_find(struct rps_part *part, struct rp_dpb *dpb, s32 poc, const s32 *delta_poc, const u8 *used, u32 count,
                            BOOL *in_set)
{
	part->used = used;
	part->count = count;
	part->long_term = FALSE;
	for (u32 i = 0; i < count; i++)
	{
		BOOL fits = poc_fits((s64)poc + delta_poc[i], &part->poc[i]); /* else no picture has it: the entry is missing */
		part_keep(part, i, dpb, fits ? rp_dpb_find_short_term(dpb, part->poc[i]) : NULL, in_set);
	}
}

/**
 * part_curr() - A part's pictures the current one references: into the DPB array, and their indices into a Curr list.
 * @rec:  the record
 * @part: the part
 * @list: the Curr list
 *
 * A missing one gets its substitute entry.  A stream naming more than a
 * list holds is past the standard: the rest is left out and the picture
 * marked corrupt.  A picture the full DPB array has no room for is listed
 * as DPB_INDEX_NONE, which fails any reference list that picks it.
 */
static void part_curr(struct rp_record *rec, const struct rps_part *part, const struct curr_list *list)
{
	for (u32 i = 0; i < part->count; i++)
	{
		if (!part->used[i])
			continue;
		if (*list->count >= V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
		{
			rec->corrupt = TRUE;
			return;
		}
		u32 index = dpb_add(rec, part->found[i], part->poc[i], part->long_term);
		list->indices[(*list->count)++] = index == NOT_IN_ARRAY ? DPB_INDEX_NONE : (u8)index;
	}
}

/**
 * part_foll() - A part's pictures kept for later ones only: into the DPB array when the buffer holds them.
 * @rec:  the record
 * @part: the part
 */
static void part_foll(struct rp_record *rec, const struct rps_part *part)
{
	for (u32 i = 0; i < part->count; i++)
		if (!part->used[i] && part->found[i] != NULL)
			dpb_add(rec, part->found[i], part->poc[i], part->long_term);
}

/**
 * rp_rps_apply() - The reference picture set (8.3.2): the buffer marked, the record's DPB array and Curr lists filled.
 * @rec: the record as rp_record_picture() left it: its POC and flags set, its DPB array and Curr lists empty
 * @dpb: the buffer
 * @sh:  the first slice's header
 * @sps: the active SPS
 *
 * A picture with NoRaslOutputFlag, an IRAP that starts a sequence, first
 * marks every picture unused for reference; an IDR picture then has an
 * empty set.  Otherwise the long-term entries are matched first and
 * marked, so a short-term search never takes them; then the short-term
 * ones by full POC.  Every other picture is marked unused for reference.
 * The DPB array lists StCurrBefore, StCurrAfter, StFoll, LtCurr, LtFoll in
 * that order, the Foll ones only when the buffer holds them; a missing
 * Curr picture gets a substitute entry.
 */
void rp_rps_apply(struct rp_record *rec, struct rp_dpb *dpb, const struct rp_slice_header *sh, const struct rp_sps *sps)
{
	if (rec->no_rasl_output)
		rp_dpb_unmark_all(dpb);
	if (rec->idr)
		return;

	const struct rp_st_rps *rps = &sh->st_rps;
	struct rps_part before;
	struct rps_part after;
	struct rps_part long_term;
	BOOL in_set[RP_DPB_MAX];
	memset(in_set, 0, sizeof(in_set));
	long_term_find(&long_term, dpb, rec, sh, sps, in_set);
	short_term_find(&before, dpb, rec->poc, rps->delta_poc_s0, rps->used_s0, rps->num_negative, in_set);
	short_term_find(&after, dpb, rec->poc, rps->delta_poc_s1, rps->used_s1, rps->num_positive, in_set);
	for (u32 i = 0; i < RP_DPB_MAX; i++)
		if (dpb->entries[i].used && !in_set[i])
			dpb->entries[i].ref = RP_REF_NONE;

	struct v4l2_ctrl_hevc_decode_params *decode = &rec->decode;
	const struct curr_list before_list = {decode->poc_st_curr_before, &decode->num_poc_st_curr_before};
	const struct curr_list after_list = {decode->poc_st_curr_after, &decode->num_poc_st_curr_after};
	const struct curr_list long_term_list = {decode->poc_lt_curr, &decode->num_poc_lt_curr};
	part_curr(rec, &before, &before_list);
	part_curr(rec, &after, &after_list);
	part_foll(rec, &before);
	part_foll(rec, &after);
	part_curr(rec, &long_term, &long_term_list);
	part_foll(rec, &long_term);
}

/**
 * temp_list() - RefPicListTemp0 or 1 (8-8, 8-10): the Curr lists in the list's order, repeated to the length wanted.
 * @rec:    the record, its Curr lists filled (rp_rps_apply())
 * @list:   0: StCurrBefore, StCurrAfter, LtCurr; 1: StCurrAfter, StCurrBefore, LtCurr
 * @temp:   filled in
 * @length: NumRpsCurrTempList
 *
 * Return: TRUE; FALSE when the picture has no Curr pictures at all.
 */
static BOOL temp_list(const struct rp_record *rec, u32 list, u8 *temp, u32 length)
{
	const struct v4l2_ctrl_hevc_decode_params *decode = &rec->decode;
	const u8 *parts[CURR_LISTS] = {decode->poc_st_curr_before, decode->poc_st_curr_after, decode->poc_lt_curr};
	u32 counts[CURR_LISTS] = {decode->num_poc_st_curr_before, decode->num_poc_st_curr_after, decode->num_poc_lt_curr};
	if (counts[0] + counts[1] + counts[2] == 0)
		return FALSE;
	if (list == 1)
	{
		parts[0] = decode->poc_st_curr_after;
		counts[0] = decode->num_poc_st_curr_after;
		parts[1] = decode->poc_st_curr_before;
		counts[1] = decode->num_poc_st_curr_before;
	}
	u32 filled = 0;
	while (filled < length)
		for (u32 part = 0; part < CURR_LISTS; part++)
			for (u32 i = 0; i < counts[part] && filled < length; i++)
				temp[filled++] = parts[part][i];
	return TRUE;
}

/**
 * rp_ref_list() - RefPicList0 or 1 (8.3.4), as indices into the record's DPB array.
 * @rec:     the record
 * @sh:      the slice's header
 * @list:    0 or 1
 * @ref_idx: the slice parameters' ref_idx_l0 or ref_idx_l1: its first num_ref_idx_active[list] entries filled
 *
 * RefPicListTemp0 is StCurrBefore, StCurrAfter, LtCurr repeated to
 * NumRpsCurrTempList0, list 1 the other way round; list_entry picks from
 * it when the modification flag is set.
 *
 * Return: TRUE; FALSE when NumPicTotalCurr is 0 or an entry is out of
 * range (a stream error).
 */
BOOL rp_ref_list(const struct rp_record *rec, const struct rp_slice_header *sh, u32 list, u8 ref_idx[V4L2_HEVC_DPB_ENTRIES_NUM_MAX])
{
	u8 temp[TEMP_LIST_MAX];
	u32 num_active = sh->num_ref_idx_active[list];
	u32 total = sh->num_pic_total_curr;
	u32 num_temp = num_active > total ? num_active : total; /* NumRpsCurrTempList, cut to TEMP_LIST_MAX: a list_entry past that fails */
	if (num_temp > sizeof(temp))
		num_temp = sizeof(temp);
	if (total == 0 || !temp_list(rec, list, temp, num_temp))
		return FALSE;
	for (u32 i = 0; i < num_active; i++)
	{
		u32 temp_index = sh->list_modification[list] ? sh->list_entry[list][i] : i;
		if (temp_index >= num_temp || temp[temp_index] == DPB_INDEX_NONE)
			return FALSE;
		ref_idx[i] = temp[temp_index];
	}
	return TRUE;
}
