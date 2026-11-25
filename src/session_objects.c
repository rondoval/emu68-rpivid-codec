/* SPDX-License-Identifier: GPL-2.0 */
/*
 * session_objects.c - a session's objects (docs/architecture.md section 5.3):
 * the packets, the frames and the shape they are made for; the events
 * ring and the signal to the client.
 */
#include "session.h" /* rpivid_private.h first: __NOLIBBASE__ */

#include <proto/exec.h>
#include <exec/memory.h>

#include <debug.h>
#include <memory.h>
#include <minlist.h>

#include "narrow/narrow.h"

/* ---- signals and events ---------------------------------------------------------- */

/**
 * signal_client() - The client's task told something is ready.
 * @s: the session
 */
void signal_client(struct rp_session *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	if (s->sig_mask != 0 && s->sig_task != NULL)
		Signal(s->sig_task, s->sig_mask);
}

/**
 * events_push() - An event for the client's poll.
 * @s:      the session
 * @event:  VCEV_*; the stream info is not one of these: the poll gives it from info_pending
 * @status: the VCERR_* of a VCEV_Error
 *
 * A full ring drops the event: the client does not poll.  A VCEV_Error
 * dropped so loses nothing the client needs: the session is FAILED, and
 * every later call answers with its failure.
 */
void events_push(struct rp_session *s, ULONG event, LONG status)
{
	if (s->event_count == RP_EVENTS_MAX)
	{
		Kprintf("[rpivid] event %ld dropped: the client does not poll\n", event);
		return;
	}
	struct VCEvent *slot = &s->events[(s->event_head + s->event_count) % RP_EVENTS_MAX];
	memset(slot, 0, sizeof(*slot));
	slot->Event = event;
	slot->Status = status;
	s->event_count++;
}

/**
 * events_pop() - The oldest event, as a poll item.
 * @s:    the session
 * @item: the poll's item
 *
 * Return: TRUE with @item filled.
 */
BOOL events_pop(struct rp_session *s, struct VCItem *item)
{
	if (s->event_count == 0)
		return FALSE;
	item->Type = VCIT_Event;
	item->u.Event = s->events[s->event_head];
	s->event_head = (s->event_head + 1u) % RP_EVENTS_MAX;
	s->event_count--;
	return TRUE;
}

/**
 * session_fail() - The session ends: the client learns why at its next poll.
 * @s:     the session
 * @error: the VCERR_*
 * @what:  for the log
 */
void session_fail(struct rp_session *s, LONG error, const char *what)
{
	(void)what; /* only the log reads it, and a release build has none */
	if (s->state == RP_SESSION_FAILED)
		return;
	Kprintf("[rpivid] session %08lx failed: %s (%ld)\n", (ULONG)s, what, error);
	s->state = RP_SESSION_FAILED;
	s->failure = error;
	events_push(s, VCEV_Error, error);
	signal_client(s);
}

/* ---- packets ---------------------------------------------------------------------- */

/**
 * packet_make() - A packet object over memory.
 * @s:        the session
 * @data:     the memory
 * @capacity: its bytes
 * @owned:    the backend's memory, freed with the object
 *
 * An own packet starts in the backend's hands and is handed out by a poll;
 * an imported one starts in the client's.
 *
 * Return: the packet, NULL without memory.
 */
struct rp_packet *packet_make(struct rp_session *s, APTR data, u32 capacity, BOOL owned)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct rp_packet *p = AllocVec(sizeof(*p), MEMF_PUBLIC | MEMF_CLEAR);
	if (p == NULL)
		return NULL;
	p->pub.Size = sizeof(p->pub);
	p->pub.Data = data;
	p->pub.Capacity = capacity;
	p->pub.Pts = VC_TIME_UNKNOWN;
	p->session = s;
	p->owned = owned;
	p->at_client = !owned;
	AddTail((struct List *)&s->packets, (struct Node *)&p->node);
	return p;
}

/**
 * packet_of() - The session's packet behind a client's pointer.
 * @s:   the session
 * @pub: the pointer
 *
 * Return: the packet, NULL for a pointer that is none of this session's.
 */
struct rp_packet *packet_of(struct rp_session *s, const struct VCPacket *pub)
{
	for (struct MinNode *node = s->packets.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct rp_packet *p = CONTAINER_OF(node, struct rp_packet, node);
		if (&p->pub == pub)
			return p;
	}
	return NULL;
}

/**
 * packets_free() - Every packet object, and the own ones' memory.
 * @s: the session
 */
void packets_free(struct rp_session *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct MinNode *node;
	while ((node = (struct MinNode *)RemHead((struct List *)&s->packets)) != NULL)
	{
		struct rp_packet *p = CONTAINER_OF(node, struct rp_packet, node);
		if (p->owned)
			FreeVec(p->pub.Data);
		FreeVec(p);
	}
	_NewMinList(&s->pending);
	_NewMinList(&s->returned);
	s->scanning = NULL;
}

/* ---- frames ------------------------------------------------------------------------ */

/**
 * block_frames() - The list of the frames the block decodes into.
 * @s: the session
 *
 * Return: the 10-bit frames of a Main10 session, else the client's frames.
 */
static struct MinList *block_frames(struct rp_session *s)
{
	return s->bit_depth == RP_DEPTH_MAIN10 ? &s->tenbit : &s->frames;
}

/**
 * frame_describe() - The frame's public geometry, from the session's shape.
 * @s: the session, its context made
 * @f: the frame
 *
 * A 10-bit frame has no public format: nobody but the block and the pass
 * sees it.
 */
static void frame_describe(const struct rp_session *s, struct rp_frame *f)
{
	f->pub.Size = sizeof(f->pub);
	f->pub.Data = f->buf.ptr;
	f->pub.Format = f->decode_target && s->bit_depth == RP_DEPTH_MAIN10 ? VCF_Unknown : VCF_NV12_COL128;
	f->pub.Width = (f->decode_target ? s->columns : s->client_columns) * CMDGEN_COLUMN_WIDTH;
	f->pub.Height = s->ctx.rows;
	f->pub.Crop = s->info.Crop;
	f->pub.Planes = RP_NV12_PLANES;
	f->pub.Offset[0] = 0;
	f->pub.Offset[1] = CMDGEN_COLUMN_WIDTH * s->ctx.rows;
	f->pub.Pitch[0] = s->ctx.column_bytes;
	f->pub.Pitch[1] = s->ctx.column_bytes;
	f->pub.Pts = VC_TIME_UNKNOWN;
	f->pub.Flags = 0;
}

/**
 * frame_make() - A frame object, over the pool or the client's memory.
 * @s:             the session, its context made
 * @data:          the client's memory, or NULL for a buffer from the pool
 * @decode_target: the block decodes into it (its columns, its collocated vectors);
 *                 else a narrowed frame of a Main10 session, the 68k's to write
 *
 * Return: the frame, NULL without memory.
 */
struct rp_frame *frame_make(struct rp_session *s, APTR data, BOOL decode_target)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct hevc_d_dev *dev = &s->base->hw.dev;
	u32 size = (decode_target ? s->columns : s->client_columns) * s->ctx.column_bytes;
	struct rp_frame *f = AllocVec(sizeof(*f), MEMF_PUBLIC | MEMF_CLEAR);
	if (f == NULL)
		return NULL;
	f->session = s;
	f->decode_target = decode_target;
	if (data != NULL)
	{
		f->buf.ptr = data;
		f->buf.addr = (dma_addr_t)data;
		f->buf.size = size;
	}
	else if (hwbuf_alloc(dev, &f->buf, size) == 0)
		f->owned = TRUE;
	if (f->buf.ptr != NULL && decode_target)
		f->aux = cmdgen_aux_alloc(&s->ctx);
	if (f->buf.ptr == NULL || (decode_target && f->aux == NULL))
	{
		if (f->owned)
			hwbuf_free(dev, &f->buf);
		FreeVec(f);
		return NULL;
	}
	frame_describe(s, f);
	AddTail((struct List *)(decode_target ? block_frames(s) : &s->frames), (struct Node *)&f->node);
	return f;
}

/**
 * frame_list_free() - Every frame object of a list; the own buffers go back to the pool.
 * @s:    the session, nothing in flight
 * @list: the frames or the 10-bit frames
 */
static void frame_list_free(struct rp_session *s, struct MinList *list)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct MinNode *node;
	while ((node = (struct MinNode *)RemHead((struct List *)list)) != NULL)
	{
		struct rp_frame *f = CONTAINER_OF(node, struct rp_frame, node);
		if (f->aux != NULL)
			cmdgen_aux_free(&s->ctx, f->aux);
		if (f->owned)
			hwbuf_free(&s->base->hw.dev, &f->buf);
		FreeVec(f);
	}
}

/**
 * frames_free() - Every frame object, both lists.
 * @s: the session, nothing in flight
 */
static void frames_free(struct rp_session *s)
{
	frame_list_free(s, &s->frames);
	frame_list_free(s, &s->tenbit);
	_NewMinList(&s->output);
}

/**
 * frame_of() - The session's frame behind a client's pointer.
 * @s:   the session
 * @pub: the pointer
 *
 * Return: the frame, NULL for a pointer that is none of this session's.
 */
struct rp_frame *frame_of(struct rp_session *s, const struct VCFrame *pub)
{
	for (struct MinNode *node = s->frames.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct rp_frame *f = CONTAINER_OF(node, struct rp_frame, node);
		if (&f->pub == pub)
			return f;
	}
	return NULL;
}

/**
 * frame_by_number() - The block's frame holding a picture.
 * @s:      the session
 * @number: the picture, in decoding order
 *
 * Return: the frame, NULL when none holds it.
 */
struct rp_frame *frame_by_number(struct rp_session *s, u32 number)
{
	if (number == 0)
		return NULL;
	for (struct MinNode *node = block_frames(s)->mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct rp_frame *f = CONTAINER_OF(node, struct rp_frame, node);
		if (f->number == number)
			return f;
	}
	return NULL;
}

/**
 * frame_in_flight() - Whether a picture in flight writes or reads a frame.
 * @s: the session
 * @f: the frame
 *
 * Return: TRUE when the block may still touch it.
 */
static BOOL frame_in_flight(const struct rp_session *s, const struct rp_frame *f)
{
	for (unsigned int i = 0; i < RP_PICTURES_IN_FLIGHT; i++)
	{
		const struct rp_session_picture *picture = &s->pictures[i];
		if (!picture->in_use)
			continue;
		if (picture->frame == f)
			return TRUE;
		for (unsigned int j = 0; j < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; j++)
			if (picture->refs[j] == f)
				return TRUE;
	}
	return FALSE;
}

/**
 * frame_is_free() - Whether a frame may take a new picture.
 * @s: the session
 * @f: the frame
 *
 * Free: not with the client, not waiting to go out, its picture no longer
 * in the stream layer's buffer (as a reference or for output), and no
 * picture in flight touching it.
 *
 * Return: TRUE when free.
 */
BOOL frame_is_free(struct rp_session *s, const struct rp_frame *f)
{
	if ((f->flags & (RPF_AT_CLIENT | RPF_ON_OUTPUT)) != 0)
		return FALSE;
	if (f->number != 0 && rp_dpb_holds(&s->stream->dpb, f->number))
		return FALSE;
	return !frame_in_flight(s, f);
}

/**
 * free_frame_of() - The first free frame of a list.
 * @s:    the session
 * @list: the frames or the 10-bit frames
 *
 * Return: the frame, cleared; NULL when none is free.
 */
static struct rp_frame *free_frame_of(struct rp_session *s, struct MinList *list)
{
	for (struct MinNode *node = list->mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct rp_frame *f = CONTAINER_OF(node, struct rp_frame, node);
		if (frame_is_free(s, f))
		{
			f->number = 0;
			f->flags = 0;
			return f;
		}
	}
	return NULL;
}

/**
 * frame_take() - A free frame of the block's for the picture being built.
 * @s: the session
 *
 * Return: the frame, cleared; NULL when none is free.
 */
struct rp_frame *frame_take(struct rp_session *s)
{
	return free_frame_of(s, block_frames(s));
}

/**
 * narrowed_take() - A free narrowed frame of a Main10 session, for the poll's pass.
 * @s: the session, its depth 10
 *
 * Return: the frame, cleared; NULL when the client holds them all.
 */
struct rp_frame *narrowed_take(struct rp_session *s)
{
	return free_frame_of(s, &s->frames);
}

/**
 * frames_all_free() - Whether every frame is free: the old shape can go.
 * @s: the session
 *
 * Return: TRUE when all are, both lists.
 */
BOOL frames_all_free(struct rp_session *s)
{
	struct MinList *lists[] = {&s->frames, &s->tenbit};
	for (unsigned int i = 0; i < ARRAY_SIZE(lists); i++)
	{
		for (struct MinNode *node = lists[i]->mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
			if (!frame_is_free(s, CONTAINER_OF(node, struct rp_frame, node)))
				return FALSE;
	}
	return TRUE;
}

/**
 * frames_imported() - Whether the client brought its own frames.
 * @s: the session
 *
 * Return: TRUE when a frame is not the backend's.
 */
BOOL frames_imported(const struct rp_session *s)
{
	for (struct MinNode *node = s->frames.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
		if (!CONTAINER_OF(node, struct rp_frame, node)->owned)
			return TRUE;
	return FALSE;
}

/* ---- the shape -------------------------------------------------------------------- */

/**
 * shape_free() - The context, the frames and the bitstream buffers of the old shape.
 * @s: the session, nothing in flight
 */
void shape_free(struct rp_session *s)
{
	frames_free(s);
	for (unsigned int i = 0; i < RP_PICTURES_IN_FLIGHT; i++)
		hwbuf_free(&s->base->hw.dev, &s->pictures[i].bitstream);
	if (s->ctx_ready)
	{
		cmdgen_ctx_exit(&s->ctx);
		s->ctx_ready = FALSE;
	}
}

/**
 * stream_info_make() - What the client is told of the shape: the size, the window, the rate, the aspect, the colours.
 * @s:      the session, its context and columns made
 * @width:  the coded width
 * @height: the coded height
 * @sps:    the SPS; NULL for a shape the client gave: the whole picture, nothing known of its colours
 */
static void stream_info_make(struct rp_session *s, u32 width, u32 height, const struct rp_sps *sps)
{
	struct VCStreamInfo *info = &s->info;
	ULONG matrix = VC_MATRIX_UNSPECIFIED;
	BOOL full_range = FALSE;

	memset(info, 0, sizeof(*info));
	info->Width = s->client_columns * CMDGEN_COLUMN_WIDTH;
	info->Height = s->ctx.rows;
	info->Format = VCF_NV12_COL128;
	info->Par.Num = 1;
	info->Par.Den = 1;
	info->FrameRate = s->params.FrameRate;
	info->Crop.Width = (LONG)width;
	info->Crop.Height = (LONG)height;
	if (sps != NULL)
	{
		info->Crop.X = (LONG)sps->conf_left;
		info->Crop.Y = (LONG)sps->conf_top;
		info->Crop.Width = (LONG)(sps->width - sps->conf_left - sps->conf_right);
		info->Crop.Height = (LONG)(sps->height - sps->conf_top - sps->conf_bottom);
		if (sps->timing_present && sps->num_units_in_tick != 0)
		{
			info->FrameRate.Num = (LONG)sps->time_scale;
			info->FrameRate.Den = (LONG)sps->num_units_in_tick;
		}
		if (sps->sar_width != 0 && sps->sar_height != 0)
		{
			info->Par.Num = sps->sar_width;
			info->Par.Den = sps->sar_height;
		}
		if (sps->video_signal_present && sps->colour_description_present)
			matrix = sps->matrix_coeffs;
		full_range = sps->video_signal_present && sps->full_range;
		if (sps->field_seq)
			info->Flags |= VCFF_Interlaced;
	}
	info->ColorSpace = VC_ColorSpaceOf(matrix, full_range, height);
	s->info_pending = TRUE;
}

/**
 * shape_make() - The context, the bitstream buffers and the stream info for a shape.
 * @s:      the session
 * @width:  the coded width
 * @height: the coded height
 * @sps:    the SPS, for the depth, the window, the rate and the aspect; NULL for a shape the client gave (8 bits)
 *
 * The frames are the caller's: the client's imports, or shape_from_sps()'s.
 * A Main10 shape's 10-bit columns are rounded up to whole groups of the
 * pass (architecture.md section 10); the block writes only the first width / 96 of them and
 * the pass narrows the rest into columns past the picture.
 *
 * Return: 0, or VCERR_NOMEM.
 */
LONG shape_make(struct rp_session *s, u32 width, u32 height, const struct rp_sps *sps)
{
	struct hevc_d_dev *dev = &s->base->hw.dev;
	shape_free(s);
	if (cmdgen_ctx_init(&s->ctx, dev, width, height) != 0)
		return VCERR_NOMEM;
	s->ctx_ready = TRUE;
	s->width = width;
	s->height = height;
	s->bit_depth = sps != NULL ? sps->bit_depth_luma : RP_DEPTH_MAIN;
	if (s->bit_depth == RP_DEPTH_MAIN10)
	{
		s->columns = ALIGN_UP(DIV_CEIL(width, RP_TENBIT_SAMPLES), RP_NARROW_IN_COLUMNS);
		s->client_columns = s->columns / RP_NARROW_IN_COLUMNS * RP_NARROW_OUT_COLUMNS;
	}
	else
	{
		s->columns = DIV_CEIL(width, CMDGEN_COLUMN_WIDTH);
		s->client_columns = s->columns;
	}
	rp_hw_slots_want(&s->base->hw, width, height);
	for (unsigned int i = 0; i < RP_PICTURES_IN_FLIGHT; i++)
		if (hwbuf_alloc(dev, &s->pictures[i].bitstream, hevc_d_bit_buf_size(width, height, s->bit_depth - RP_DEPTH_MAIN)) != 0)
			return VCERR_NOMEM;
	stream_info_make(s, width, height, sps);
	return 0;
}

/**
 * frames_make() - A number of the backend's frames.
 * @s:             the session, its context made
 * @count:         how many
 * @decode_target: the block decodes into them; else narrowed frames
 *
 * Return: 0, or VCERR_NOMEM.
 */
static LONG frames_make(struct rp_session *s, u32 count, BOOL decode_target)
{
	for (u32 i = 0; i < count; i++)
		if (frame_make(s, NULL, decode_target) == NULL)
			return VCERR_NOMEM;
	return 0;
}

/**
 * shape_from_sps() - The shape of an SPS and the backend's frames for it: the DPB's size and the client's count.
 * @s:   the session
 * @sps: the SPS
 *
 * At 8 bits one set of frames serves the block and the client.  At 10 the
 * block gets the DPB's size plus one 10-bit frame, the client one narrowed
 * frame more than its count: one being narrowed while it holds the rest
 * (architecture.md section 10).
 *
 * Return: 0, or VCERR_NOMEM.
 */
LONG shape_from_sps(struct rp_session *s, const struct rp_sps *sps)
{
	u32 dpb = sps->max_dec_pic_buffering;
	u32 outputs = s->params.OutputCount != 0 ? s->params.OutputCount : RP_OUTPUT_COUNT;
	LONG error = shape_make(s, sps->width, sps->height, sps);
	if (error != 0)
		return error;
	if (s->bit_depth != RP_DEPTH_MAIN10)
		return frames_make(s, dpb + outputs, TRUE);
	error = frames_make(s, dpb + 1u, TRUE);
	if (error == 0)
		error = frames_make(s, outputs + 1u, FALSE);
	return error;
}
