/* SPDX-License-Identifier: GPL-2.0 */
/*
 * stream.c - the stream layer's state (stream.h): the parameter sets
 * kept, a picture's slices taken in order through slice.c, rps.c, dpb.c
 * and record.c, the sequence's position moved on.
 *
 * A picture's first slice settles, in this order: NoRaslOutputFlag (8.1.3)
 * and whether the picture is a RASL one to skip; the SPS, which the
 * sequence's first picture activates and the rest must keep (7.4.2.4.2);
 * the POC (8.3.1); the record's picture part; the reference picture set
 * and its marking (8.3.2); the buffer's output before the picture
 * (C.5.2.2).  Each later slice is held to the first (7.4.7.1) and appended.
 * The buffer takes the picture when the session says it is complete
 * (C.5.2.3).
 */
#include <memory.h>

#include "nal.h"
#include "rps.h"
#include "slice.h"
#include "stream.h"

/**
 * rp_stream_init() - A stream at its start: no sets, an empty buffer, the next picture an IRAP.
 * @s: the stream
 */
void rp_stream_init(struct rp_stream *s)
{
	memset(s, 0, sizeof(*s));
	s->first_in_cvs = TRUE;
	s->first_picture = TRUE;
	s->next_number = 1;
}

/**
 * sps_keep() - An SPS NAL unit: parsed, kept under its id, the PPSs that name it resolved again.
 * @s:   the stream
 * @nal: the unit
 * @len: its bytes
 *
 * New content under an id counts a generation: the picture that uses the
 * set decides whether that was allowed (7.4.2.4.2: only where a coded
 * video sequence starts).
 *
 * Return: RP_PS_OK, RP_PS_STREAM or RP_PS_UNSUPPORTED, the reason in why.
 */
static enum rp_ps_result sps_keep(struct rp_stream *s, const u8 *nal, u32 len)
{
	struct rp_sps parsed;
	enum rp_ps_result rc = rp_sps_parse(&parsed, nal, len);
	if (rc != RP_PS_OK)
	{
		s->why = parsed.why;
		return rc;
	}
	struct rp_sps *sps = &s->sps[parsed.sps_id];
	if (sps->valid)
	{
		parsed.why = sps->why; /* the reason pointer is no content: the compare then holds the content alone (ps.c zeroes every set it parses) */
		if (memcmp(sps, &parsed, sizeof(parsed)) != 0)
			s->sps_generation[parsed.sps_id]++;
	}
	*sps = parsed;
	for (u32 i = 0; i < RP_PPS_MAX; i++)
		if (s->pps[i].valid && s->pps[i].sps_id == sps->sps_id && rp_pps_resolve(&s->pps[i], sps) != RP_PS_OK)
		{
			s->why = s->pps[i].why;
			rc = RP_PS_STREAM;
		}
	return rc;
}

/**
 * pps_keep() - A PPS NAL unit: parsed, kept under its id, resolved against its SPS if that is there.
 * @s:   the stream
 * @nal: the unit
 * @len: its bytes
 *
 * Return: RP_PS_OK, RP_PS_STREAM or RP_PS_UNSUPPORTED, the reason in why.
 */
static enum rp_ps_result pps_keep(struct rp_stream *s, const u8 *nal, u32 len)
{
	struct rp_pps parsed;
	enum rp_ps_result rc = rp_pps_parse(&parsed, nal, len);
	if (rc != RP_PS_OK)
	{
		s->why = parsed.why;
		return rc;
	}
	struct rp_pps *pps = &s->pps[parsed.pps_id];
	*pps = parsed;
	if (!s->sps[pps->sps_id].valid)
		return RP_PS_OK; /* resolved when its SPS comes */
	rc = rp_pps_resolve(pps, &s->sps[pps->sps_id]);
	if (rc != RP_PS_OK)
		s->why = pps->why;
	return rc;
}

/**
 * rp_stream_parameter_set() - A VPS, SPS or PPS NAL unit: kept by id, the VPS ignored.
 * @s:        the stream
 * @nal_type: the unit's type
 * @nal:      the unit
 * @len:      its bytes
 *
 * Return: RP_PS_OK, RP_PS_STREAM or RP_PS_UNSUPPORTED.
 */
enum rp_ps_result rp_stream_parameter_set(struct rp_stream *s, u32 nal_type, const u8 *nal, u32 len)
{
	if (nal_type == NAL_SPS_NUT)
		return sps_keep(s, nal, len);
	if (nal_type == NAL_PPS_NUT)
		return pps_keep(s, nal, len);
	return RP_PS_OK; /* a VPS: nothing of it is used */
}

/**
 * rp_stream_unit_ends_picture() - Whether a NAL unit ends the picture being taken (7.4.2.4.4).
 * @s:        the stream
 * @nal_type: the unit's type
 * @nal:      the unit, header included
 * @len:      its bytes
 *
 * The first slice segment of a picture does, and so does a non-VCL unit of
 * a type that none of the picture's slices can follow
 * (nal_type_ends_picture()).  The caller completes the picture before
 * giving the unit.
 *
 * Return: TRUE when it does.
 */
BOOL rp_stream_unit_ends_picture(const struct rp_stream *s, u32 nal_type, const u8 *nal, u32 len)
{
	if (!s->in_picture)
		return FALSE;
	if (nal_is_vcl(nal_type))
		return nal_starts_picture(nal, len);
	return nal_type_ends_picture(nal_type);
}

/**
 * starts_sequence() - Whether a picture starts a coded video sequence: an IRAP with NoRaslOutputFlag (8.1.3).
 * @s:        the stream
 * @nal_type: the picture's NAL unit type
 *
 * Return: TRUE for an IDR, a BLA, or a CRA that is the first picture after a start, an end of sequence or a flush.
 */
static BOOL starts_sequence(const struct rp_stream *s, u32 nal_type)
{
	if (!nal_is_irap(nal_type))
		return FALSE;
	return nal_is_idr(nal_type) || nal_is_bla(nal_type) || s->first_in_cvs;
}

/**
 * picture_start() - The first slice: NoRaslOutputFlag, POC, the set and the marking, the buffer's output, the record.
 * @s:   the stream
 * @rec: the record, cleared
 * @sh:  the first slice's header
 * @sps: the active SPS
 * @pps: the active PPS
 *
 * The sequence's first picture activates its SPS: an IRAP with
 * NoRaslOutputFlag, or any picture after the stream's start, an end of
 * sequence or of bitstream, or a flush, so that a stream or a seek that
 * starts at no IRAP takes the SPS its pictures name.  Every later picture
 * of the sequence keeps it, its id and its content.
 *
 * Return: RP_STREAM_OK; RP_STREAM_SKIP for a RASL picture that is skipped;
 * RP_STREAM_ERROR when the picture's POC is out of range; RP_STREAM_BAD
 * when the picture names another SPS than its sequence's, or the SPS's
 * content changed under its id since the sequence began.
 */
static enum rp_stream_result picture_start(struct rp_stream *s, struct rp_record *rec, const struct rp_slice_header *sh,
                                           const struct rp_sps *sps, const struct rp_pps *pps)
{
	BOOL irap = nal_is_irap(sh->nal_type);
	BOOL no_rasl_output = starts_sequence(s, sh->nal_type);
	BOOL cra_start = no_rasl_output && sh->nal_type == NAL_CRA_NUT;
	BOOL no_output_of_prior = cra_start || sh->no_output_of_prior_pics; /* NoOutputOfPriorPicsFlag (C.5.2.2) */
	BOOL activates = no_rasl_output || s->first_in_cvs;                 /* the sequence's first picture: it activates its SPS */

	/* 8.1.3: an IRAP with NoRaslOutputFlag drops its RASL pictures */
	if (irap)
		s->skip_rasl = no_rasl_output;
	if (nal_is_rasl(sh->nal_type) && s->skip_rasl)
	{
		s->why = "a RASL picture after a random access point";
		return RP_STREAM_SKIP;
	}
	/* 7.4.2.4.2, 7.4.3.3.1: the active SPS holds, its id and its content, for the whole coded video sequence */
	if (!activates && (sps != s->active_sps || s->sps_generation[sps->sps_id] != s->active_generation))
	{
		s->why = "an SPS changing inside a coded video sequence";
		return RP_STREAM_BAD;
	}

	s32 poc;
	if (!rp_poc_derive(s->prev_tid0_poc, sps->log2_max_poc_lsb, sh->pic_order_cnt_lsb, no_rasl_output, &poc))
	{
		s->why = "a picture order count past the 32 bits the standard allows";
		return RP_STREAM_ERROR;
	}
	rp_record_picture(rec, s->next_number, poc, sh, sps, pps, no_rasl_output, no_output_of_prior);

	rp_rps_apply(rec, &s->dpb, sh, sps); /* 8.3.2, the unmarking for NoRaslOutputFlag included */

	/* C.5.2.2 (a changed shape may set the flag, "but should not": the flag as sent is preferred) */
	rp_dpb_before_picture(&s->dpb, sps, no_rasl_output && !s->first_picture, no_output_of_prior);

	if (activates)
	{
		s->active_sps = sps;
		s->active_generation = s->sps_generation[sps->sps_id];
	}
	s->first_in_cvs = FALSE;
	s->first_picture = FALSE;
	s->in_picture = TRUE;
	return RP_STREAM_OK;
}

/**
 * same_st_rps() - Whether two short-term sets name the same pictures.
 * @set:   one set
 * @other: the other
 *
 * The counts, then the deltas and used flags they cover: a compare of the
 * whole structure would take in the padding after the counts and the
 * entries past them.
 *
 * Return: TRUE when they do.
 */
static BOOL same_st_rps(const struct rp_st_rps *set, const struct rp_st_rps *other)
{
	if (set->num_negative != other->num_negative || set->num_positive != other->num_positive)
		return FALSE;
	return memcmp(set->delta_poc_s0, other->delta_poc_s0, set->num_negative * sizeof(set->delta_poc_s0[0])) == 0 &&
	       memcmp(set->used_s0, other->used_s0, set->num_negative) == 0 &&
	       memcmp(set->delta_poc_s1, other->delta_poc_s1, set->num_positive * sizeof(set->delta_poc_s1[0])) == 0 &&
	       memcmp(set->used_s1, other->used_s1, set->num_positive) == 0;
}

/**
 * same_long_term() - Whether a later slice names the long-term pictures the first one did (7.4.7.1).
 * @sh:    the slice's header
 * @first: the first slice's
 *
 * Return: TRUE when both counts and every entry's values agree.
 */
static BOOL same_long_term(const struct rp_slice_header *sh, const struct rp_slice_header *first)
{
	if (sh->num_long_term_sps != first->num_long_term_sps || sh->num_long_term_pics != first->num_long_term_pics)
		return FALSE;
	u32 count = (u32)sh->num_long_term_sps + sh->num_long_term_pics;
	return memcmp(sh->poc_lsb_lt, first->poc_lsb_lt, count * sizeof(sh->poc_lsb_lt[0])) == 0 &&
	       memcmp(sh->used_by_curr_pic_lt, first->used_by_curr_pic_lt, count) == 0 &&
	       memcmp(sh->delta_poc_msb_present, first->delta_poc_msb_present, count) == 0 &&
	       memcmp(sh->delta_poc_msb_cycle_lt, first->delta_poc_msb_cycle_lt, count * sizeof(sh->delta_poc_msb_cycle_lt[0])) == 0;
}

/**
 * same_values() - Whether an independent slice's per-picture values are the first one's (7.4.7.1).
 * @sh:    the slice's header
 * @first: the first slice's
 *
 * NumPicTotalCurr follows from the sets, which are compared after; it is
 * the quick test.
 *
 * Return: TRUE when they agree.
 */
static BOOL same_values(const struct rp_slice_header *sh, const struct rp_slice_header *first)
{
	return sh->pic_output == first->pic_output && sh->no_output_of_prior_pics == first->no_output_of_prior_pics &&
	       sh->pic_order_cnt_lsb == first->pic_order_cnt_lsb && sh->temporal_mvp == first->temporal_mvp &&
	       sh->num_pic_total_curr == first->num_pic_total_curr;
}

/**
 * same_picture() - Whether a later slice segment says what the picture's first one did (7.4.2.2, 7.4.7.1).
 * @sh:    the segment's header
 * @first: the first segment's
 *
 * The NAL unit type (one per picture), TemporalId (one per access unit)
 * and slice_pic_parameter_set_id come in every segment.  A dependent
 * segment copies the rest from its independent one, which was held to the
 * first already.  The collocated picture, which may sit at another index
 * in another slice's list, is checked by record.c.
 *
 * Return: TRUE when the values the standard fixes per picture agree.
 */
static BOOL same_picture(const struct rp_slice_header *sh, const struct rp_slice_header *first)
{
	if (sh->nal_type != first->nal_type || sh->temporal_id != first->temporal_id || sh->pps_id != first->pps_id)
		return FALSE;
	if (sh->dependent_slice_segment)
		return TRUE;
	return same_values(sh, first) && same_st_rps(&sh->st_rps, &first->st_rps) && same_long_term(sh, first);
}

/**
 * rp_stream_slice() - A slice segment NAL unit: parsed, its picture started if it is the first, its record filled.
 * @s:           the stream
 * @rec:         the current picture's record; cleared by a first slice
 * @nal:         the unit, header included
 * @len:         its bytes
 * @nal_type:    the unit's type
 * @temporal_id: its TemporalId
 *
 * Return: RP_STREAM_OK; RP_STREAM_SKIP for a RASL picture that is not
 * decoded; RP_STREAM_ERROR when the slice does not parse, names sets
 * that are not there, contradicts the picture's first slice, comes with
 * no picture started, or starts a picture whose POC is out of range;
 * RP_STREAM_BAD when the stream cannot go on.
 * Every answer but RP_STREAM_OK leaves its reason in why.
 */
enum rp_stream_result rp_stream_slice(struct rp_stream *s, struct rp_record *rec, const u8 *nal, u32 len, u32 nal_type,
                                      u32 temporal_id)
{
	struct rp_slice_header sh;
	BOOL first = nal_starts_picture(nal, len);

	if (!first && !s->in_picture)
	{
		s->why = "a slice with no picture started"; /* after a skipped or dropped picture */
		return RP_STREAM_ERROR;
	}
	if (!rp_slice_parse(&sh, nal, len, nal_type, temporal_id, s->sps, s->pps, first ? NULL : &rec->independent))
	{
		s->why = "a slice header that does not parse, or names sets that are not there";
		return RP_STREAM_ERROR;
	}
	if (first)
	{
		const struct rp_pps *pps = &s->pps[sh.pps_id];
		rp_record_clear(rec);
		enum rp_stream_result rc = picture_start(s, rec, &sh, &s->sps[pps->sps_id], pps);
		if (rc != RP_STREAM_OK)
			return rc;
	}
	else if (!same_picture(&sh, &rec->first))
	{
		s->why = "a slice contradicting the picture's first slice";
		return RP_STREAM_ERROR;
	}
	if (!sh.dependent_slice_segment)
		rec->independent = sh;
	const char *why = rp_record_slice(rec, &sh, len);
	if (why != NULL)
	{
		s->why = why;
		return RP_STREAM_ERROR;
	}
	return RP_STREAM_OK;
}

/**
 * rp_stream_picture_done() - The picture's slices are all in: it joins the buffer, the sequence moves on.
 * @s:   the stream
 * @rec: its record
 *
 * prevTid0Pic becomes this picture when its TemporalId is 0 and it is not
 * RASL, RADL or a sub-layer non-reference picture (8.3.1).
 */
void rp_stream_picture_done(struct rp_stream *s, const struct rp_record *rec)
{
	if (!s->in_picture)
		return;
	s->in_picture = FALSE;
	rp_dpb_store(&s->dpb, s->active_sps, rec->number, rec->poc, rec->output);
	if (rec->first.temporal_id == 0 && !nal_is_rasl(rec->nal_type) && !nal_is_radl(rec->nal_type) &&
	    !nal_is_sub_layer_non_reference(rec->nal_type))
		s->prev_tid0_poc = rec->poc;
	s->next_number++;
}

/**
 * rp_stream_picture_dropped() - The picture is not decoded after all: its number is still spent.
 * @s: the stream
 */
void rp_stream_picture_dropped(struct rp_stream *s)
{
	if (!s->in_picture)
		return;
	s->in_picture = FALSE;
	s->next_number++;
}

/**
 * rp_stream_end_of_sequence() - An end of sequence: the next picture is an IRAP that starts one.
 * @s: the stream
 *
 * Nothing is output here: the pictures waiting stay in the buffer, and the
 * IRAP's NoOutputOfPriorPicsFlag decides (C.5.2.2: a CRA discards them, an
 * IDR or BLA as its flag says).
 */
void rp_stream_end_of_sequence(struct rp_stream *s)
{
	s->first_in_cvs = TRUE;
}

/**
 * rp_stream_end_of_bitstream() - The bitstream ends: everything waiting goes out, in POC order.
 * @s: the stream
 */
void rp_stream_end_of_bitstream(struct rp_stream *s)
{
	rp_dpb_bump_all(&s->dpb);
	s->first_in_cvs = TRUE;
	s->first_picture = TRUE;
}

/**
 * rp_stream_flush() - A seek: the buffer emptied with nothing shown, the next picture an IRAP and "picture 0".
 * @s: the stream
 */
void rp_stream_flush(struct rp_stream *s)
{
	rp_dpb_discard_all(&s->dpb);
	s->first_in_cvs = TRUE;
	s->first_picture = TRUE;
	s->in_picture = FALSE;
}
