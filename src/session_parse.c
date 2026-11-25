/* SPDX-License-Identifier: GPL-2.0 */
/*
 * session_parse.c - packets into pictures (docs/architecture.md section 3.3):
 * the Annex B scanner over the packets sent, each NAL unit through the
 * stream layer, a slice into the picture being built, a complete picture
 * through the command generator into the hardware layer's queue, and the
 * pictures the decode task retired reaped into their frames.  Parsing
 * stops, with the unit kept, when a picture slot or a frame is wanted,
 * and goes on in the client's next call.
 */
#include "session.h" /* rpivid_private.h first: __NOLIBBASE__ */

#include <proto/exec.h>
#include <exec/memory.h>

#include <debug.h>
#include <memory.h>
#include <minlist.h>

#define HVCC_VERSION 1         /* an hvcC record's first byte: configurationVersion; Annex B starts with a 0 */
#define HVCC_ARRAYS_AT 22      /* numOfArrays sits after the fixed part */
#define HVCC_ARRAY_HEADER 3    /* an array: its NAL unit type, then the count of its units */
#define HVCC_UNIT_HEADER 2     /* a unit: its length, then its bytes */
#define HVCC_TYPE_MASK 0x3fu
#define BITSTREAM_ALIGN 4u     /* each slice's NAL unit starts at a multiple of this in the bitstream buffer */

/* ---- pictures ---------------------------------------------------------------------- */

/**
 * picture_retired() - The decode task is done with a picture: the client is told (the retire hook).
 * @p: the picture, its state DONE or FAILED
 *
 * No lock: how it ended, in one volatile field, and a signal; the client's
 * next call does the rest.  The one field carries both that it ended and
 * how, so the reap reads nothing the task wrote before it: a read of the
 * state ahead of a flag could see the state as it was.
 */
static void picture_retired(struct rp_picture *p)
{
	struct rp_session_picture *picture = (struct rp_session_picture *)p;
	picture->retired_state = p->state;
	signal_client(picture->session);
}

/**
 * picture_unused() - One of the session's pictures that is not in use.
 * @s: the session
 *
 * Return: the picture, NULL when all are in flight or being built.
 */
static struct rp_session_picture *picture_unused(struct rp_session *s)
{
	for (unsigned int i = 0; i < RP_PICTURES_IN_FLIGHT; i++)
		if (!s->pictures[i].in_use)
			return &s->pictures[i];
	return NULL;
}

/**
 * picture_release() - A picture's slot is free again.
 * @s:  the session
 * @picture: the picture, retired or never queued
 */
void picture_release(struct rp_session *s, struct rp_session_picture *picture)
{
	if (picture->hw.de != NULL)
	{
		cmdgen_env_give(&s->ctx, picture->hw.de);
		picture->hw.de = NULL;
	}
	memset(picture->refs, 0, sizeof(picture->refs));
	picture->frame = NULL;
	picture->in_use = FALSE;
	picture->retired_state = RP_PIC_FREE;
	picture->corrupt_input = FALSE;
}

/**
 * picture_failed() - A picture the block never decoded: its frame shows as it is, counted dropped.
 * @s:  the session
 * @picture: the picture, in the stream layer's buffer already
 */
static void picture_failed(struct rp_session *s, struct rp_session_picture *picture)
{
	if (picture->frame != NULL)
		picture->frame->flags |= RPF_DECODED | RPF_CORRUPT;
	s->base->counters.dropped++;
	picture_release(s, picture);
}

/**
 * picture_abandon() - The picture being built is dropped before it was stored: as if it never was.
 * @s:   the session
 * @why: for the log
 */
void picture_abandon(struct rp_session *s, const char *why)
{
	struct rp_session_picture *picture = s->parsing;
	(void)why; /* only the log reads it, and a release build has none */
	if (picture == NULL)
		return;
	Kprintf("[rpivid] picture %lu dropped: %s\n", (ULONG)s->stream->next_number, why);
	rp_stream_picture_dropped(s->stream);
	if (picture->frame != NULL)
		picture->frame->number = 0;
	s->base->counters.dropped++;
	picture_release(s, picture);
	s->parsing = NULL;
	s->first_slice_taken = FALSE;
}

/**
 * session_reap() - The pictures the task retired since the last call: their frames decoded, their slots free.
 * @s: the session
 *
 * A wedged block fails the session.
 */
void session_reap(struct rp_session *s)
{
	for (unsigned int i = 0; i < RP_PICTURES_IN_FLIGHT; i++)
	{
		struct rp_session_picture *picture = &s->pictures[i];
		if (!picture->in_use || picture == s->parsing)
			continue;
		enum rp_picture_state ended = picture->retired_state; /* read once: the hook may store it at any moment */
		if (ended == RP_PIC_FREE)
			continue;
		if (ended == RP_PIC_DONE)
		{
			if (picture->frame != NULL)
				picture->frame->flags |= RPF_DECODED;
			picture_release(s, picture);
		}
		else
			picture_failed(s, picture);
	}
	if (s->base->hw.wedged)
		session_fail(s, VCERR_TRANSPORT, "the block is wedged");
}

/**
 * output_drain() - The pictures the stream layer bumped out, onto the output list in order.
 * @s: the session
 */
void output_drain(struct rp_session *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	for (;;)
	{
		u32 number = rp_dpb_output_next(&s->stream->dpb);
		if (number == 0)
			break;
		struct rp_frame *f = frame_by_number(s, number);
		if (f == NULL || (f->flags & RPF_ON_OUTPUT) != 0)
			continue;
		f->flags |= RPF_ON_OUTPUT;
		AddTail((struct List *)&s->output, (struct Node *)&f->link);
	}
}

/**
 * command_input() - What the command generator builds a picture's list from: the record, the slices, the frames.
 * @s:       the session
 * @picture: the picture, its record complete
 * @in:      filled in
 *
 * The frames of the record's DPB array are looked up and kept with the
 * picture.  A missing one leaves its entry empty and marks the picture
 * corrupt: the frame itself stands in (8.3.3), as the generator presets.
 */
static void command_input(struct rp_session *s, struct rp_session_picture *picture, struct cmdgen_input *in)
{
	struct rp_record *rec = &picture->rec;

	memset(in, 0, sizeof(*in));
	in->run.h265.slice_ents = rec->slice_count;
	in->run.h265.sps = &rec->sps;
	in->run.h265.pps = &rec->pps;
	in->run.h265.dec = &rec->decode;
	in->run.h265.slice_params = rec->slices;
	in->run.h265.scaling_matrix = rec->has_scaling ? &rec->scaling : NULL;
	in->bitstream = picture->bitstream.addr;
	in->bitstream_used = picture->bitstream_used;
	in->slice_base = picture->slice_base;
	in->slice_len = picture->slice_len;
	in->frame_luma = picture->frame->buf.addr;
	in->frame_aux = picture->frame->aux;
	for (u32 i = 0; i < rec->decode.num_active_dpb_entries; i++)
	{
		struct rp_frame *f = frame_by_number(s, rec->ref_number[i]);
		picture->refs[i] = f;
		if (f == NULL)
		{
			rec->corrupt = TRUE;
			continue;
		}
		in->ref_luma[i] = f->buf.addr;
		in->ref_aux[i] = f->aux;
	}
}

/**
 * complete_picture() - The picture being built is whole: stored, its command list built, queued.
 * @s: the session, parsing set
 *
 * Return: UNIT_DONE, or UNIT_STOP when the block took nothing.
 */
static enum unit_result complete_picture(struct rp_session *s)
{
	struct rp_session_picture *picture = s->parsing;
	struct rp_record *rec = &picture->rec;
	struct cmdgen_input in;

	if (picture->corrupt_input)
	{
		picture_abandon(s, "a packet flagged corrupt");
		return UNIT_DONE;
	}
	s->parsing = NULL;
	s->first_slice_taken = FALSE;
	rp_stream_picture_done(s->stream, rec);
	output_drain(s);

	command_input(s, picture, &in);
	if (rec->corrupt)
		picture->frame->flags |= RPF_CORRUPT;
	if (cmdgen_picture(&s->ctx, picture->hw.de, &in) != 0)
	{
		Kprintf("[rpivid] picture %lu: the command generator refused it\n", (ULONG)rec->number);
		picture_failed(s, picture);
		return UNIT_DONE;
	}
	picture->hw.owner = s;
	picture->hw.ctx = &s->ctx;
	picture->hw.retire = picture_retired;
	picture->retired_state = RP_PIC_FREE;
	s->base->counters.pictures++;
	if (rp_hw_queue(&s->base->hw, &picture->hw) != 0)
	{
		picture_failed(s, picture);
		session_fail(s, VCERR_TRANSPORT, "the block takes no pictures");
		return UNIT_STOP;
	}
	return UNIT_DONE;
}

/* ---- units ---------------------------------------------------------------------------- */

/**
 * unit_reset() - The unit buffer emptied for the next NAL unit.
 * @s: the session
 */
static void unit_reset(struct rp_session *s)
{
	s->unit_len = 0;
	s->unit_overflow = FALSE;
}

/**
 * unit_room() - Room in the unit buffer for more bytes: it doubles as needed, up to RP_UNIT_MAX.
 * @s:   the session
 * @len: the bytes to come
 *
 * Return: TRUE with the room there; FALSE past RP_UNIT_MAX or without memory.
 */
static BOOL unit_room(struct rp_session *s, u32 len)
{
	struct ExecBase *SysBase = s->base->SysBase;
	if (len > RP_UNIT_MAX - s->unit_len)
		return FALSE; /* checked first: the sum below then neither wraps nor outgrows the limit */
	u32 cap = s->unit_cap;
	if (s->unit_len + len <= cap)
		return TRUE;
	while (cap < s->unit_len + len)
		cap *= 2;
	if (cap > RP_UNIT_MAX)
		return FALSE;
	u8 *bigger = AllocVec(cap, MEMF_PUBLIC);
	if (bigger == NULL)
		return FALSE;
	memcpy(bigger, s->unit, s->unit_len);
	FreeVec(s->unit);
	s->unit = bigger;
	s->unit_cap = cap;
	return TRUE;
}

/**
 * unit_append() - Bytes of the NAL unit being accumulated.
 * @s:    the session
 * @data: the bytes
 * @len:  how many
 *
 * A unit the buffer cannot hold is marked and dropped at its end.
 */
static void unit_append(struct rp_session *s, const u8 *data, u32 len)
{
	if (s->unit_overflow)
		return;
	if (!unit_room(s, len))
	{
		s->unit_overflow = TRUE;
		return;
	}
	memcpy(s->unit + s->unit_len, data, len);
	s->unit_len += len;
}

/**
 * unit_collect() - A scanner event taken into the unit buffer.
 * @s:     the session
 * @nal:   the scanner the event came from
 * @event: RP_NAL_START, RP_NAL_BYTES or RP_NAL_END
 *
 * Return: TRUE at the unit's end: it is whole in the buffer, or marked as overflown.
 */
static BOOL unit_collect(struct rp_session *s, const struct rp_nal *nal, enum rp_nal_event event)
{
	if (event == RP_NAL_END)
		return TRUE;
	if (event == RP_NAL_START)
		unit_reset(s);
	unit_append(s, nal->data, nal->len);
	return FALSE;
}

/**
 * slice_copy() - The slice unit into the picture's bitstream buffer, at a BITSTREAM_ALIGN boundary.
 * @s:  the session
 * @picture: the picture
 *
 * Return: TRUE; FALSE without memory (the caller drops the picture).
 */
static BOOL slice_copy(struct rp_session *s, struct rp_session_picture *picture)
{
	struct hevc_d_dev *dev = &s->base->hw.dev;
	u32 padded = ALIGN_UP(s->unit_len, BITSTREAM_ALIGN);
	u32 idx = picture->rec.slice_count - 1;
	if (picture->bitstream_used + padded > picture->bitstream.size)
	{
		size_t want = hevc_d_round_up_size(picture->bitstream_used + padded);
		if (want > RP_BITSTREAM_MAX || hwbuf_realloc_copy(dev, &picture->bitstream, want) != 0)
			return FALSE;
	}
	picture->slice_base[idx] = picture->bitstream_used;
	picture->slice_len[idx] = s->unit_len;
	memcpy(picture->bitstream.ptr + picture->bitstream_used, s->unit, s->unit_len);
	if (padded > s->unit_len)
		memset(picture->bitstream.ptr + picture->bitstream_used + s->unit_len, 0, padded - s->unit_len);
	picture->bitstream_used += padded;
	if (s->scanning != NULL && (s->scanning->pub.Flags & VCPF_Corrupt) != 0)
		picture->corrupt_input = TRUE;
	return TRUE;
}

/**
 * stream_answer() - A stream layer answer that is not OK, acted on.
 * @s:  the session
 * @rc: RP_STREAM_*
 *
 * Return: UNIT_DONE for a dropped or skipped picture, UNIT_STOP for a
 * stream that cannot go on.
 */
static enum unit_result stream_answer(struct rp_session *s, enum rp_stream_result rc)
{
	if (rc == RP_STREAM_BAD)
	{
		session_fail(s, VCERR_STREAM, s->stream->why);
		return UNIT_STOP;
	}
	if (rc == RP_STREAM_ERROR)
	{
		if (s->parsing != NULL)
			picture_abandon(s, s->stream->why);
		else
			Kprintf("[rpivid] slice dropped: %s\n", s->stream->why);
	}
	return UNIT_DONE;
}

/**
 * sps_of_record() - The SPS a picture's record was made from.
 * @s:   the session
 * @rec: the record
 *
 * Return: the SPS, from the stream layer's tables.
 */
static const struct rp_sps *sps_of_record(const struct rp_session *s, const struct rp_record *rec)
{
	return &s->stream->sps[s->stream->pps[rec->first.pps_id].sps_id];
}

/**
 * picture_begin() - A first slice, stage one: the picture before it completed, a new one started in the stream layer.
 * @s:           the session
 * @type:        the unit's type
 * @temporal_id: its TemporalId
 *
 * On success the picture is the one being built and first_slice_taken is
 * set: the unit now only waits for the picture's frame and environment.
 *
 * Return: what became of the unit when first_slice_taken stays clear:
 * UNIT_HELD without a free picture, UNIT_DONE for a picture skipped or
 * dropped, UNIT_STOP.
 */
static enum unit_result picture_begin(struct rp_session *s, u32 type, u32 temporal_id)
{
	if (s->parsing != NULL)
	{
		enum unit_result result = complete_picture(s);
		if (result != UNIT_DONE)
			return result;
	}
	struct rp_session_picture *picture = picture_unused(s);
	if (picture == NULL)
		return UNIT_HELD;
	enum rp_stream_result rc = rp_stream_slice(s->stream, &picture->rec, s->unit, s->unit_len, type, temporal_id);
	if (rc != RP_STREAM_OK)
		return stream_answer(s, rc);
	output_drain(s); /* what the picture's start bumped out holds its frames until they are shown */
	picture->in_use = TRUE;
	picture->retired_state = RP_PIC_FREE;
	picture->frame = NULL;
	picture->hw.de = NULL;
	picture->hw.state = RP_PIC_PARSING;
	picture->bitstream_used = 0;
	picture->corrupt_input = FALSE;
	picture->pts = s->scanning != NULL ? s->scanning->pub.Pts : VC_TIME_UNKNOWN;
	memset(picture->refs, 0, sizeof(picture->refs));
	s->parsing = picture;
	s->first_slice_taken = TRUE;
	return UNIT_DONE;
}

/**
 * shape_for_picture() - The session's shape made the picture's, once the old shape's frames are all free.
 * @s:   the session
 * @rec: the picture's record
 *
 * Return: UNIT_DONE with the shape right; UNIT_HELD while the old frames
 * drain to the client; UNIT_STOP for a shape the session cannot take.
 */
static enum unit_result shape_for_picture(struct rp_session *s, const struct rp_record *rec)
{
	if (rp_main10_too_large(rec->bit_depth, rec->width, rec->height))
	{
		session_fail(s, VCERR_UNSUPPORTED, "Main10 past 1080p");
		return UNIT_STOP;
	}
	if (s->ctx_ready && rec->width == s->width && rec->height == s->height && rec->bit_depth == s->bit_depth)
		return UNIT_DONE;
	if (frames_imported(s))
	{
		session_fail(s, VCERR_BADPARAM, "the client's frames are not the stream's shape");
		return UNIT_STOP;
	}
	if (s->ctx_ready && !frames_all_free(s))
		return UNIT_HELD;
	LONG error = shape_from_sps(s, sps_of_record(s, rec));
	if (error != 0)
	{
		session_fail(s, error, "no memory for the frames");
		return UNIT_STOP;
	}
	return UNIT_DONE;
}

/**
 * picture_resources() - A first slice, stage two: the shape, a frame and an environment for the picture being built.
 * @s: the session, parsing set
 *
 * Whatever is there already is kept, so a wait leaves nothing half done.
 *
 * Return: UNIT_DONE with all three; UNIT_HELD while one is wanted; UNIT_STOP.
 */
static enum unit_result picture_resources(struct rp_session *s)
{
	struct rp_session_picture *picture = s->parsing;
	enum unit_result result = shape_for_picture(s, &picture->rec);
	if (result != UNIT_DONE)
		return result;
	if (picture->frame == NULL)
	{
		struct rp_frame *f = frame_take(s);
		if (f == NULL)
			return UNIT_HELD;
		f->number = picture->rec.number;
		f->poc = picture->rec.poc;
		f->pts = picture->pts;
		f->key = picture->rec.irap;
		picture->frame = f;
	}
	if (picture->hw.de == NULL)
		picture->hw.de = cmdgen_env_take(&s->ctx);
	return picture->hw.de != NULL ? UNIT_DONE : UNIT_HELD;
}

/**
 * unit_slice() - A slice segment unit: a picture started or continued, the slice copied.
 * @s:           the session
 * @type:        the unit's type
 * @temporal_id: its TemporalId
 *
 * A first slice goes in two stages, so that a wait leaves nothing half
 * done: the stream layer takes it (picture_begin()), then the picture
 * gets what it is decoded with (picture_resources()).
 *
 * Return: UNIT_DONE, UNIT_HELD (a picture, the old frames or a frame wanted), UNIT_STOP.
 */
static enum unit_result unit_slice(struct rp_session *s, u32 type, u32 temporal_id)
{
	if (nal_starts_picture(s->unit, s->unit_len))
	{
		enum unit_result result = UNIT_DONE;
		if (!s->first_slice_taken)
			result = picture_begin(s, type, temporal_id);
		if (!s->first_slice_taken)
			return result;
		result = picture_resources(s);
		if (result != UNIT_DONE)
			return result;
		s->first_slice_taken = FALSE;
	}
	else
	{
		if (s->parsing == NULL)
			return UNIT_DONE; /* after a skipped or dropped picture */
		enum rp_stream_result rc = rp_stream_slice(s->stream, &s->parsing->rec, s->unit, s->unit_len, type, temporal_id);
		if (rc != RP_STREAM_OK)
			return stream_answer(s, rc);
	}
	if (!slice_copy(s, s->parsing))
		picture_abandon(s, "no memory for the slices");
	return UNIT_DONE;
}

/**
 * parameter_set_error() - What a parameter set the stream layer refused ends the session with.
 * @rc: RP_PS_*
 *
 * Return: 0 for RP_PS_OK, else VCERR_UNSUPPORTED or VCERR_STREAM.
 */
static LONG parameter_set_error(enum rp_ps_result rc)
{
	if (rc == RP_PS_OK)
		return 0;
	return rc == RP_PS_UNSUPPORTED ? VCERR_UNSUPPORTED : VCERR_STREAM;
}

/**
 * unit_parameter_set() - A VPS, SPS or PPS unit: kept by the stream layer, or the session fails.
 * @s:    the session
 * @type: the unit's type
 *
 * Return: UNIT_DONE or UNIT_STOP.
 */
static enum unit_result unit_parameter_set(struct rp_session *s, u32 type)
{
	LONG error = parameter_set_error(rp_stream_parameter_set(s->stream, type, s->unit, s->unit_len));
	if (error == 0)
		return UNIT_DONE;
	session_fail(s, error, s->stream->why);
	return UNIT_STOP;
}

/**
 * unit_process() - One NAL unit, whole in the unit buffer.
 * @s:           the session
 * @type:        its type
 * @temporal_id: its TemporalId
 *
 * Return: UNIT_DONE, UNIT_HELD or UNIT_STOP.
 */
static enum unit_result unit_process(struct rp_session *s, u32 type, u32 temporal_id)
{
	if (s->unit_overflow)
	{
		Kprintf("[rpivid] a NAL unit past %lu bytes dropped\n", (ULONG)RP_UNIT_MAX);
		return UNIT_DONE;
	}
	if (!nal_is_vcl(type) && s->parsing != NULL && rp_stream_unit_ends_picture(s->stream, type, s->unit, s->unit_len))
	{
		enum unit_result result = complete_picture(s);
		if (result != UNIT_DONE)
			return result;
	}
	if (nal_is_parameter_set(type))
		return unit_parameter_set(s, type);
	if (nal_is_slice_segment(type))
		return unit_slice(s, type, temporal_id);
	if (type == NAL_EOS_NUT)
		rp_stream_end_of_sequence(s->stream);
	if (type == NAL_EOB_NUT)
	{
		rp_stream_end_of_bitstream(s->stream);
		output_drain(s);
	}
	return UNIT_DONE; /* SEI, delimiters, filler, reserved types: ignored */
}

/* ---- packets ---------------------------------------------------------------------------- */

/**
 * packet_ends_picture() - Whether the scanner's packet ends where a picture does (architecture.md section 3.3).
 * @s: the session, scanning a packet
 *
 * A packet flagged VCPF_FrameEnd or VCPF_EndOfStream does.  In a Framed
 * session so does any other, unless it is part of a picture spread over
 * packets: from one flagged VCPF_FrameStart alone to the one flagged
 * VCPF_FrameEnd, as a client sends a picture larger than its packets.
 *
 * Return: TRUE at a boundary.
 */
static BOOL packet_ends_picture(const struct rp_session *s)
{
	if ((s->scanning->pub.Flags & (VCPF_FrameEnd | VCPF_EndOfStream)) != 0)
		return TRUE;
	return s->params.Framed && !s->spanning;
}

/**
 * packet_finish() - The scanner reached a packet's end: the picture completed where the packet says, the packet returned.
 * @s: the session
 *
 * Return: UNIT_DONE, or UNIT_STOP when completing the picture failed the session.
 */
static enum unit_result packet_finish(struct rp_session *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct rp_packet *packet = s->scanning;
	ULONG flags = packet->pub.Flags;
	BOOL boundary = packet_ends_picture(s);
	enum unit_result result = UNIT_DONE;
	if ((flags & (VCPF_FrameEnd | VCPF_EndOfStream)) != 0)
		s->spanning = FALSE;
	if (boundary && s->parsing != NULL)
		result = complete_picture(s);
	if ((flags & VCPF_EndOfStream) != 0)
	{
		rp_stream_end_of_bitstream(s->stream);
		output_drain(s);
		s->end_pending = TRUE;
		s->end_sent = FALSE;
	}
	s->scanning = NULL;
	s->scanning_done = FALSE;
	AddTail((struct List *)&s->returned, (struct Node *)&packet->link);
	signal_client(s);
	return result;
}

/**
 * held_unit_process() - The unit whole in the buffer, processed if what it waits for is there.
 * @s: the session, a unit held
 *
 * Return: TRUE when the unit is done with and parsing goes on; FALSE while
 * it is still held, or when it failed the session.
 */
static BOOL held_unit_process(struct rp_session *s)
{
	enum unit_result result = unit_process(s, s->held_type, s->held_temporal_id);
	if (result == UNIT_HELD)
		return FALSE;
	s->held_unit = FALSE;
	unit_reset(s);
	return result != UNIT_STOP;
}

/**
 * packet_next() - The next pending packet to the scanner.
 * @s: the session, scanning none
 *
 * A packet flagged VCPF_FrameStart alone starts a picture spread over packets.
 *
 * Return: TRUE with a packet being scanned; FALSE when none is pending.
 */
static BOOL packet_next(struct rp_session *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct MinNode *node = (struct MinNode *)RemHead((struct List *)&s->pending);
	if (node == NULL)
		return FALSE;
	s->scanning = CONTAINER_OF(node, struct rp_packet, link);
	rp_nal_packet(&s->nal, s->scanning->pub.Data, s->scanning->pub.Length);
	if ((s->scanning->pub.Flags & VCPF_FrameStart) != 0)
		s->spanning = (s->scanning->pub.Flags & (VCPF_FrameEnd | VCPF_EndOfStream)) == 0;
	return TRUE;
}

/**
 * scan_step() - The scanner's next event: bytes into the unit buffer, a whole unit held for processing.
 * @s: the session, scanning a packet
 *
 * At the packet's end an open unit ends too, where the packet ends a
 * picture.  A unit of another layer ends here and is forgotten (architecture.md
 * section 6.1): the next one starts afresh.
 */
static void scan_step(struct rp_session *s)
{
	enum rp_nal_event event = rp_nal_next(&s->nal);
	if (event == RP_NAL_NONE)
	{
		if (packet_ends_picture(s))
			event = rp_nal_finish(&s->nal);
		s->scanning_done = TRUE;
		if (event == RP_NAL_NONE)
			return;
	}
	if (unit_collect(s, &s->nal, event) && s->nal.layer == NAL_BASE_LAYER)
	{
		s->held_unit = TRUE;
		s->held_type = s->nal.type;
		s->held_temporal_id = s->nal.temporal_id;
	}
}

/**
 * session_parse() - Everything sent and not yet parsed, as far as the resources go.
 * @s: the session, reaped
 *
 * The held unit first, then the packet the scanner is in, then the next
 * pending packet; a unit held for a picture or a frame stops the loop
 * until a release or a reap lets it go on.
 */
void session_parse(struct rp_session *s)
{
	while (s->state != RP_SESSION_FAILED)
	{
		if (s->held_unit)
		{
			if (!held_unit_process(s))
				return;
		}
		else if (s->scanning_done)
		{
			if (packet_finish(s) == UNIT_STOP)
				return;
		}
		else if (s->scanning == NULL && !packet_next(s))
			return;
		else
			scan_step(s);
	}
}

/**
 * session_parse_reset() - The scanner and the unit forgotten: a flush, or the extradata done.
 * @s: the session
 */
void session_parse_reset(struct rp_session *s)
{
	rp_nal_init(&s->nal);
	unit_reset(s);
	s->held_unit = FALSE;
	s->first_slice_taken = FALSE;
	s->scanning_done = FALSE;
	s->spanning = FALSE;
}

/**
 * hvcc_u16() - A 16-bit number of an hvcC record: big-endian.
 * @data: its two bytes
 *
 * Return: the number.
 */
static u32 hvcc_u16(const u8 *data)
{
	return ((u32)data[0] << 8) | data[1];
}

/**
 * extradata_hvcc() - The parameter sets of an hvcC record (ISO 14496-15).
 * @s:    the session
 * @data: the record: the fixed part, then arrays of NAL units of one type each
 * @size: its bytes, more than HVCC_ARRAYS_AT
 *
 * A record that ends inside an array is read as far as it is whole.
 *
 * Return: 0, or the VCERR_* of a set the stream layer refused.
 */
static LONG extradata_hvcc(struct rp_session *s, const u8 *data, u32 size)
{
	u32 arrays = data[HVCC_ARRAYS_AT];
	u32 pos = HVCC_ARRAYS_AT + 1;
	for (u32 array = 0; array < arrays && pos + HVCC_ARRAY_HEADER <= size; array++)
	{
		u32 type = data[pos] & HVCC_TYPE_MASK;
		u32 units = hvcc_u16(data + pos + 1);
		pos += HVCC_ARRAY_HEADER;
		for (u32 unit = 0; unit < units && pos + HVCC_UNIT_HEADER <= size; unit++)
		{
			u32 len = hvcc_u16(data + pos);
			pos += HVCC_UNIT_HEADER;
			if (pos + len > size)
				return 0;
			if (nal_is_parameter_set(type) && len >= NAL_HEADER_BYTES && nal_header_layer(data + pos) == NAL_BASE_LAYER)
			{
				LONG error = parameter_set_error(rp_stream_parameter_set(s->stream, type, data + pos, len));
				if (error != 0)
					return error;
			}
			pos += len;
		}
	}
	return 0;
}

/**
 * extradata_annexb() - The parameter sets of an Annex B codec configuration.
 * @s:    the session
 * @data: the configuration
 * @size: its bytes
 *
 * Return: 0, or the VCERR_* of a set the stream layer refused.
 */
static LONG extradata_annexb(struct rp_session *s, const u8 *data, u32 size)
{
	struct rp_nal nal;
	rp_nal_init(&nal);
	rp_nal_packet(&nal, data, size);
	for (;;)
	{
		enum rp_nal_event event = rp_nal_next(&nal);
		if (event == RP_NAL_NONE)
			event = rp_nal_finish(&nal);
		if (event == RP_NAL_NONE)
			return 0;
		if (!unit_collect(s, &nal, event))
			continue;
		if (s->unit_overflow || nal.layer != NAL_BASE_LAYER || !nal_is_parameter_set(nal.type))
			continue;
		LONG error = parameter_set_error(rp_stream_parameter_set(s->stream, nal.type, s->unit, s->unit_len));
		if (error != 0)
			return error;
	}
}

/**
 * extradata_feed() - The codec configuration's parameter sets, before the first packet.
 * @s:    the session
 * @data: the configuration: Annex B, or an hvcC record
 * @size: its bytes
 *
 * An hvcC record is told from Annex B by its first byte.  An SPS among
 * the sets makes the shape at once, so that the frames and the stream
 * info are ready at the first poll.
 *
 * Return: 0, or the VCERR_* the open fails with.
 */
LONG extradata_feed(struct rp_session *s, const u8 *data, u32 size)
{
	LONG error = size > HVCC_ARRAYS_AT && data[0] == HVCC_VERSION ? extradata_hvcc(s, data, size) : extradata_annexb(s, data, size);
	if (error != 0)
		return error;
	session_parse_reset(s);
	for (u32 id = 0; id < RP_SPS_MAX; id++)
	{
		const struct rp_sps *sps = &s->stream->sps[id];
		if (!sps->valid)
			continue;
		if (rp_main10_too_large(sps->bit_depth_luma, sps->width, sps->height))
			return VCERR_UNSUPPORTED;
		return shape_from_sps(s, sps);
	}
	return 0;
}
