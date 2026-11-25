/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dpb.h - the decoded picture buffer (docs/architecture.md section 6.5).
 *
 * The standard's entries, their marking as references, and the output
 * process of C.5.2, which decides when each picture is shown.  That needs
 * only POCs and counts, so it runs at parse time, before the HEVC block has
 * decoded anything.  Pictures are known by their number in decoding order;
 * the session maps numbers to frames.
 *
 * rps.c finds and marks the references; stream.c runs the output process
 * around each picture (rp_dpb_before_picture(), rp_dpb_store()) and at the
 * stream's end or a flush; the session takes the pictures bumped out, in
 * output order, with rp_dpb_output_next().
 *
 * The bumped pictures wait in a list until the session takes them.  It takes
 * them after every rp_dpb_store() and after rp_dpb_bump_all() at the end of
 * a bitstream, and between two takes only the pictures in the buffer at the
 * first and the one stored since can be bumped, each once: RP_OUT_MAX is
 * room for them all.
 */
#ifndef RP_DPB_H
#define RP_DPB_H

#include <exec/types.h>
#include <types.h>

#include "ps.h"

#define RP_DPB_MAX (RP_DPB_SIZE_MAX + 1) /* the entries: the most pictures an SPS may buffer, and the current one */
#define RP_OUT_MAX (RP_DPB_MAX + 1)      /* the bumped pictures that can wait for the session: see the head of this file */

/* Every picture the buffer holds besides the current one fits the record's DPB array */
_Static_assert(V4L2_HEVC_DPB_ENTRIES_NUM_MAX >= RP_DPB_MAX - 1, "the record's DPB array holds every reference");

/* An entry's marking (8.3.2) */
enum rp_ref : u8
{
	RP_REF_NONE,  /* unused for reference: the entry stays only while it waits for output */
	RP_REF_SHORT, /* used for short-term reference: every picture is, when it is stored */
	RP_REF_LONG,  /* used for long-term reference: a later picture's RPS named it so */
};

/* One picture in the buffer */
struct rp_dpb_entry
{
	BOOL used;        /* the entry holds a picture */
	s32 poc;          /* its PicOrderCntVal */
	enum rp_ref ref;  /* its marking */
	BOOL for_output;  /* "needed for output": not bumped out yet */
	u32 number;       /* the picture's number in decoding order, from 1 */
	u32 latency;      /* PicLatencyCount (C.5.2.3): the pictures stored since, while it waits for output */
};

/* The buffer, and the pictures bumped out of it that the session has not taken yet */
struct rp_dpb
{
	struct rp_dpb_entry entries[RP_DPB_MAX]; /* in no order: a free one is taken for each picture stored */
	u32 out[RP_OUT_MAX];                     /* the numbers bumped, in output order, the oldest first */
	u32 out_count;                           /* how many of them wait */
};

struct rp_dpb_entry *rp_dpb_find_short_term(struct rp_dpb *dpb, s32 poc);
struct rp_dpb_entry *rp_dpb_find_reference(struct rp_dpb *dpb, s32 poc, s32 mask);
void rp_dpb_unmark_all(struct rp_dpb *dpb);
void rp_dpb_before_picture(struct rp_dpb *dpb, const struct rp_sps *sps, BOOL irap_no_rasl, BOOL no_output_of_prior);
BOOL rp_dpb_store(struct rp_dpb *dpb, const struct rp_sps *sps, u32 number, s32 poc, BOOL for_output);
void rp_dpb_bump_all(struct rp_dpb *dpb);
void rp_dpb_discard_all(struct rp_dpb *dpb);
u32 rp_dpb_output_next(struct rp_dpb *dpb);
BOOL rp_dpb_holds(const struct rp_dpb *dpb, u32 number);

#endif /* RP_DPB_H */
