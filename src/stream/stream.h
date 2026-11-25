/* SPDX-License-Identifier: GPL-2.0 */
/*
 * stream.h - the stream layer's state (docs/architecture.md section 6): the
 * parameter sets kept by id, the coded video sequence's position (8.1.3,
 * 8.3.1) and the decoded picture buffer.  Plain C over bytes: no Exec
 * call, no allocation; the session gives it its memory, this structure
 * once, and rp_stream_init() starts it.
 *
 * The session gives it one NAL unit at a time, header included:
 * - before a non-VCL unit, rp_stream_unit_ends_picture() says whether the
 *   picture being taken is complete; a slice segment that starts a
 *   picture ends the one before by itself (nal_starts_picture());
 * - a VPS, SPS or PPS goes to rp_stream_parameter_set(); a slice segment
 *   to rp_stream_slice(), with the record of its picture (the session
 *   keeps one per picture in flight); an end of sequence or of bitstream
 *   to rp_stream_end_of_sequence() or rp_stream_end_of_bitstream();
 * - a complete picture goes to rp_stream_picture_done(), one the session
 *   gives up on to rp_stream_picture_dropped(); the pictures to show then
 *   come out of the buffer with rp_dpb_output_next();
 * - a seek is rp_stream_flush().
 */
#ifndef RP_STREAM_H
#define RP_STREAM_H

#include <exec/types.h>
#include <types.h>

#include "dpb.h"
#include "ps.h"
#include "record.h"

/* What rp_stream_slice() answers; every answer but RP_STREAM_OK leaves its reason in the stream's why */
enum rp_stream_result : s8
{
	RP_STREAM_OK,    /* the slice is in the record */
	RP_STREAM_SKIP,  /* a RASL picture of an IRAP with NoRaslOutputFlag (a BLA, or a CRA that starts a sequence): not decoded, not shown */
	RP_STREAM_ERROR, /* the slice cannot be taken: it does not parse, comes with no picture started, contradicts its picture, does not fit the record, or starts a picture whose POC is out of range; the picture is dropped */
	RP_STREAM_BAD,   /* the stream breaks the standard where the backend cannot go on: the stream ends (VCERR_STREAM) */
};

/* The stream layer's state across NAL units: the sets, the sequence's position, the buffer */
struct rp_stream
{
	struct rp_sps sps[RP_SPS_MAX];   /* the SPSs by sps_seq_parameter_set_id, each the last one sent */
	struct rp_pps pps[RP_PPS_MAX];   /* the PPSs by pps_pic_parameter_set_id, each the last one sent */
	u32 sps_generation[RP_SPS_MAX];  /* counts the content changes under each SPS id */
	const struct rp_sps *active_sps; /* the SPS of the current coded video sequence; NULL before the first picture */
	u32 active_generation;           /* its generation when it was activated */
	BOOL first_in_cvs;               /* the next picture starts a coded video sequence if it is an IRAP, a CRA too: after the stream's start, an end of sequence or of bitstream, a flush (8.1.3) */
	BOOL first_picture;              /* the next picture is "picture 0" (C.5.2.2): the bitstream's first, or the first after an end of bitstream or a flush */
	BOOL skip_rasl;                  /* the current IRAP has NoRaslOutputFlag: its RASL pictures are skipped (8.1.3) */
	s32 prev_tid0_poc;               /* PicOrderCntVal of prevTid0Pic (8.3.1) */
	u32 next_number;                 /* the next picture's number in decoding order, from 1 */
	BOOL in_picture;                 /* a picture's slices are being taken into its record */
	const char *why;                 /* why the last unit was refused or the last picture dropped or skipped, for the log */
	struct rp_dpb dpb;               /* the decoded picture buffer; the session takes its output with rp_dpb_output_next() */
};

void rp_stream_init(struct rp_stream *s);
enum rp_ps_result rp_stream_parameter_set(struct rp_stream *s, u32 nal_type, const u8 *nal, u32 len);
BOOL rp_stream_unit_ends_picture(const struct rp_stream *s, u32 nal_type, const u8 *nal, u32 len);
enum rp_stream_result rp_stream_slice(struct rp_stream *s, struct rp_record *rec, const u8 *nal, u32 len, u32 nal_type,
                                      u32 temporal_id);
void rp_stream_picture_done(struct rp_stream *s, const struct rp_record *rec);
void rp_stream_picture_dropped(struct rp_stream *s);
void rp_stream_end_of_sequence(struct rp_stream *s);
void rp_stream_end_of_bitstream(struct rp_stream *s);
void rp_stream_flush(struct rp_stream *s);

#endif /* RP_STREAM_H */
