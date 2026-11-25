/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dpb.c - the decoded picture buffer and the output order (dpb.h): C.5.2,
 * with HighestTid the SPS's top sub-layer.
 *
 * The output process runs in two steps per picture: rp_dpb_before_picture()
 * at its first slice, after the RPS marking (C.5.2.2), and rp_dpb_store()
 * when its record is complete (C.5.2.3).  Neither needs more than POCs and
 * counts, so both run at parse time, before the HEVC block has decoded the
 * picture.  "Bumping" a picture out (C.5.2.4) puts its number on the output
 * list, which the session empties.
 */
#include <memory.h>

#include "dpb.h"

/**
 * waits_for_output() - Whether an entry holds a picture "needed for output".
 * @entry: the entry
 *
 * Return: TRUE when it does.
 */
static BOOL waits_for_output(const struct rp_dpb_entry *entry)
{
	return entry->used && entry->for_output;
}

/**
 * rp_dpb_find_short_term() - The short-term reference picture with a POC.
 * @dpb: the buffer
 * @poc: PicOrderCntVal
 *
 * Return: the entry, NULL for none.
 */
struct rp_dpb_entry *rp_dpb_find_short_term(struct rp_dpb *dpb, s32 poc)
{
	for (u32 i = 0; i < RP_DPB_MAX; i++)
	{
		struct rp_dpb_entry *entry = &dpb->entries[i];
		if (entry->used && entry->ref == RP_REF_SHORT && entry->poc == poc)
			return entry;
	}
	return NULL;
}

/**
 * rp_dpb_find_reference() - A reference picture, short-term or long-term, by its POC or by the POC's low bits (8-6).
 * @dpb:  the buffer
 * @poc:  PicOrderCntVal, or its low bits
 * @mask: MaxPicOrderCntLsb - 1 when only the low bits count, else all ones
 *
 * Return: the entry, NULL for none.
 */
struct rp_dpb_entry *rp_dpb_find_reference(struct rp_dpb *dpb, s32 poc, s32 mask)
{
	for (u32 i = 0; i < RP_DPB_MAX; i++)
	{
		struct rp_dpb_entry *entry = &dpb->entries[i];
		if (entry->used && entry->ref != RP_REF_NONE && (entry->poc & mask) == (poc & mask))
			return entry;
	}
	return NULL;
}

/**
 * rp_dpb_unmark_all() - Every picture marked unused for reference (8.3.2, an IRAP with NoRaslOutputFlag).
 * @dpb: the buffer
 */
void rp_dpb_unmark_all(struct rp_dpb *dpb)
{
	for (u32 i = 0; i < RP_DPB_MAX; i++)
		dpb->entries[i].ref = RP_REF_NONE;
}

/**
 * empty() - Every entry leaves the buffer, shown or not.
 * @dpb: the buffer
 */
static void empty(struct rp_dpb *dpb)
{
	for (u32 i = 0; i < RP_DPB_MAX; i++)
		dpb->entries[i].used = FALSE;
}

/**
 * free_entry() - An entry no picture is in.
 * @dpb: the buffer
 *
 * Return: the entry, NULL when the buffer is full.
 */
static struct rp_dpb_entry *free_entry(struct rp_dpb *dpb)
{
	for (u32 i = 0; i < RP_DPB_MAX; i++)
		if (!dpb->entries[i].used)
			return &dpb->entries[i];
	return NULL;
}

/**
 * sweep() - The entries neither referenced nor waiting for output leave.
 * @dpb: the buffer
 */
static void sweep(struct rp_dpb *dpb)
{
	for (u32 i = 0; i < RP_DPB_MAX; i++)
	{
		struct rp_dpb_entry *entry = &dpb->entries[i];
		if (entry->used && entry->ref == RP_REF_NONE && !entry->for_output)
			entry->used = FALSE;
	}
}

/**
 * waiting() - How many pictures wait for output.
 * @dpb: the buffer
 *
 * Return: the count.
 */
static u32 waiting(const struct rp_dpb *dpb)
{
	u32 count = 0;
	for (u32 i = 0; i < RP_DPB_MAX; i++)
		count += waits_for_output(&dpb->entries[i]) ? 1 : 0;
	return count;
}

/**
 * fullness() - How many pictures the buffer holds.
 * @dpb: the buffer
 *
 * Return: the count.
 */
static u32 fullness(const struct rp_dpb *dpb)
{
	u32 count = 0;
	for (u32 i = 0; i < RP_DPB_MAX; i++)
		count += dpb->entries[i].used ? 1 : 0;
	return count;
}

/**
 * bump() - The "bumping" process (C.5.2.4): the smallest POC waiting for output goes out.
 * @dpb: the buffer
 *
 * Its number joins the output list; the entry leaves the buffer if it is
 * no reference either.  The list has room for every picture that can be
 * bumped between two of the session's takes (dpb.h); the test only keeps a
 * caller that broke that contract from writing past it.
 */
static void bump(struct rp_dpb *dpb)
{
	struct rp_dpb_entry *first = NULL; /* the first for output: the smallest POC */
	for (u32 i = 0; i < RP_DPB_MAX; i++)
	{
		struct rp_dpb_entry *entry = &dpb->entries[i];
		if (waits_for_output(entry) && (first == NULL || entry->poc < first->poc))
			first = entry;
	}
	if (first == NULL)
		return;
	first->for_output = FALSE;
	if (dpb->out_count < RP_OUT_MAX)
		dpb->out[dpb->out_count++] = first->number;
	if (first->ref == RP_REF_NONE)
		first->used = FALSE;
}

/**
 * latency_exceeded() - Whether a picture waiting for output has waited SpsMaxLatencyPictures (C.5.2.2).
 * @dpb: the buffer
 * @sps: the active SPS
 *
 * SpsMaxLatencyPictures is sps_max_num_reorder_pics +
 * sps_max_latency_increase_plus1 - 1; a plus1 of 0 sets no limit.
 *
 * Return: TRUE when one has.
 */
static BOOL latency_exceeded(const struct rp_dpb *dpb, const struct rp_sps *sps)
{
	if (sps->max_latency_increase_plus1 == 0)
		return FALSE;
	u32 max_latency = sps->max_num_reorder + sps->max_latency_increase_plus1 - 1; /* SpsMaxLatencyPictures */
	for (u32 i = 0; i < RP_DPB_MAX; i++)
		if (waits_for_output(&dpb->entries[i]) && dpb->entries[i].latency >= max_latency)
			return TRUE;
	return FALSE;
}

/**
 * rp_dpb_before_picture() - The buffer before the current picture is decoded (C.5.2.2), after the RPS marking.
 * @dpb:                the buffer
 * @sps:                the active SPS
 * @irap_no_rasl:       the current picture is an IRAP with NoRaslOutputFlag, and not the first picture
 * @no_output_of_prior: NoOutputOfPriorPicsFlag, derived by the caller
 *
 * An IRAP with NoRaslOutputFlag empties the buffer, with no output or with
 * everything bumped out first.  Otherwise entries neither referenced nor
 * for output leave, then one picture is bumped while more wait for output
 * than the SPS allows, one has waited too long, or the buffer is full.
 */
void rp_dpb_before_picture(struct rp_dpb *dpb, const struct rp_sps *sps, BOOL irap_no_rasl, BOOL no_output_of_prior)
{
	if (irap_no_rasl && no_output_of_prior)
	{
		empty(dpb);
		return;
	}
	sweep(dpb);
	if (irap_no_rasl)
	{
		rp_dpb_bump_all(dpb);
		return;
	}
	while (waiting(dpb) > 0 && (waiting(dpb) > sps->max_num_reorder || latency_exceeded(dpb, sps) ||
	                            fullness(dpb) >= sps->max_dec_pic_buffering))
		bump(dpb);
}

/**
 * count_latency() - The latency count of every picture the current one keeps waiting (C.5.2.3).
 * @dpb: the buffer, the current picture not stored yet
 * @poc: the current picture's PicOrderCntVal
 *
 * Called only for a current picture that is for output: a picture waiting
 * for output counts one more when it follows the current one in output
 * order, its POC the greater.  One that precedes it does not, as the
 * current picture holds nothing back for it.
 */
static void count_latency(struct rp_dpb *dpb, s32 poc)
{
	for (u32 i = 0; i < RP_DPB_MAX; i++)
	{
		struct rp_dpb_entry *entry = &dpb->entries[i];
		if (waits_for_output(entry) && entry->poc > poc)
			entry->latency++;
	}
}

/**
 * rp_dpb_store() - The current picture joins the buffer (C.5.2.3).
 * @dpb:        the buffer
 * @sps:        the active SPS
 * @number:     the picture
 * @poc:        its PicOrderCntVal
 * @for_output: PicOutputFlag
 *
 * It is a short-term reference always (the next picture's RPS decides).
 * When it is for output, the pictures it keeps waiting count their latency
 * up; then the additional bumping runs.
 *
 * Return: TRUE; FALSE when the buffer had no room even after a bump (a
 * stream past its own limits: the picture is not kept).
 */
BOOL rp_dpb_store(struct rp_dpb *dpb, const struct rp_sps *sps, u32 number, s32 poc, BOOL for_output)
{
	if (for_output)
		count_latency(dpb, poc);
	struct rp_dpb_entry *entry = free_entry(dpb);
	if (entry == NULL)
	{
		bump(dpb);
		sweep(dpb);
		entry = free_entry(dpb);
		if (entry == NULL)
			return FALSE;
	}
	entry->used = TRUE;
	entry->poc = poc;
	entry->ref = RP_REF_SHORT;
	entry->for_output = for_output;
	entry->number = number;
	entry->latency = 0;
	while (waiting(dpb) > sps->max_num_reorder || (waiting(dpb) > 0 && latency_exceeded(dpb, sps)))
		bump(dpb);
	return TRUE;
}

/**
 * rp_dpb_bump_all() - Everything waiting goes out, in POC order: the end of a bitstream, or an IRAP that starts afresh.
 * @dpb: the buffer
 */
void rp_dpb_bump_all(struct rp_dpb *dpb)
{
	while (waiting(dpb) > 0)
		bump(dpb);
}

/**
 * rp_dpb_discard_all() - A flush: the buffer and the output list emptied, nothing shown.
 * @dpb: the buffer
 */
void rp_dpb_discard_all(struct rp_dpb *dpb)
{
	empty(dpb);
	dpb->out_count = 0;
}

/**
 * rp_dpb_output_next() - The next picture to show, from the output list.
 * @dpb: the buffer
 *
 * The session calls it until it answers 0 after every rp_dpb_store() and
 * every rp_dpb_bump_all() at a bitstream's end: the list is sized for that.
 *
 * Return: its number, 0 when none is waiting.
 */
u32 rp_dpb_output_next(struct rp_dpb *dpb)
{
	if (dpb->out_count == 0)
		return 0;
	u32 number = dpb->out[0];
	dpb->out_count--;
	memmove(dpb->out, dpb->out + 1, dpb->out_count * sizeof(dpb->out[0]));
	return number;
}

/**
 * rp_dpb_holds() - Whether the buffer still holds a picture, as a reference or for output.
 * @dpb:    the buffer
 * @number: the picture
 *
 * Return: TRUE when it does: the picture's frame is not free.
 */
BOOL rp_dpb_holds(const struct rp_dpb *dpb, u32 number)
{
	for (u32 i = 0; i < RP_DPB_MAX; i++)
		if (dpb->entries[i].used && dpb->entries[i].number == number)
			return TRUE;
	return FALSE;
}
