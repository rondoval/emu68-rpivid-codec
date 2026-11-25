/* SPDX-License-Identifier: GPL-2.0 */
/*
 * session.c - a session's life (docs/architecture.md sections 5.3 to 5.5):
 * open and close, the imports, a send, the poll, a release, a flush, the
 * attributes.  Each runs in the client's task under the session's lock,
 * taken by rpivid_backend.c.
 */
#include "session.h" /* rpivid_private.h first: __NOLIBBASE__ */

#include <proto/exec.h>
#include <exec/memory.h>

#include <cache_ops.h>
#include <debug.h>
#include <memory.h>
#include <minlist.h>

#include "narrow/narrow.h"

/**
 * hardware_up() - The block, the task and the reset guard, for the first session.
 * @base: the library base, sessions_lock held
 *
 * Return: TRUE with the hardware up.
 */
BOOL hardware_up(struct RpividBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	if (base->hw_up)
		return TRUE;
	if (rp_hw_up(&base->hw, base) != 0)
		return FALSE;
	base->hw_up = TRUE;
	if (!base->reset_guarded)
		base->reset_guarded =
		    reset_guard_install(&base->reset_guard, SysBase, rp_hw_reset_prepare, &base->hw, (CONST_STRPTR)RP_TASK_NAME);
	return TRUE;
}

/**
 * hardware_down() - The block let go, at the frontend's idle.
 * @base: the library base, sessions_lock held, no session open
 */
void hardware_down(struct RpividBase *base)
{
	if (!base->hw_up)
		return;
	rp_hw_down(&base->hw);
	base->hw_up = FALSE;
}

/**
 * session_lookup() - The session behind a handle, without touching the handle.
 * @base:   the library base, sessions_lock held
 * @handle: what the frontend passed
 *
 * Return: the session, NULL if @handle is no open session.
 */
struct rp_session *session_lookup(struct RpividBase *base, APTR handle)
{
	for (struct MinNode *node = base->sessions.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct rp_session *s = CONTAINER_OF(node, struct rp_session, node);
		if ((APTR)s == handle && s->magic == RP_SESSION_MAGIC)
			return s;
	}
	return NULL;
}

/**
 * session_free() - Everything a session holds, then the session.
 * @s: the session, unlinked, nothing in flight
 */
static void session_free(struct rp_session *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	for (unsigned int i = 0; i < RP_PICTURES_IN_FLIGHT; i++)
	{
		if (s->ctx_ready)
			picture_release(s, &s->pictures[i]);
		FreeVec(s->pictures[i].rec.slices);
	}
	shape_free(s);
	packets_free(s);
	FreeVec(s->stream);
	FreeVec(s->unit);
	s->magic = 0;
	FreeVec(s);
}

/**
 * session_alloc() - A session object: its parameters copied, its lists empty.
 * @base:   the library base
 * @params: the frontend's parameters, copied as far as their Size reaches; the extradata is not kept
 *
 * Return: the session, NULL without memory.
 */
static struct rp_session *session_alloc(struct RpividBase *base, const struct VCSessionParams *params)
{
	struct ExecBase *SysBase = base->SysBase;
	struct rp_session *s = AllocVec(sizeof(*s), MEMF_PUBLIC | MEMF_CLEAR);
	if (s == NULL)
		return NULL;
	s->magic = RP_SESSION_MAGIC;
	s->base = base;
	InitSemaphore(&s->lock);
	memcpy(&s->params, params, params->Size < sizeof(s->params) ? params->Size : sizeof(s->params));
	s->params.Extradata = NULL;
	s->params.ExtradataSize = 0;
	s->sig_task = params->SigTask != NULL ? params->SigTask : FindTask(NULL);
	s->sig_mask = params->SigMask;
	s->state = RP_SESSION_OPEN;
	_NewMinList(&s->packets);
	_NewMinList(&s->pending);
	_NewMinList(&s->returned);
	_NewMinList(&s->frames);
	_NewMinList(&s->tenbit);
	_NewMinList(&s->output);
	return s;
}

/**
 * session_memory_make() - What a session parses with: the stream layer's state, the unit buffer, each picture's slices.
 * @s: the session
 *
 * Return: TRUE; FALSE without memory (session_free() frees what was made).
 */
static BOOL session_memory_make(struct rp_session *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	s->stream = AllocVec(sizeof(*s->stream), MEMF_PUBLIC | MEMF_CLEAR);
	s->unit = AllocVec(RP_UNIT_FIRST, MEMF_PUBLIC);
	if (s->stream == NULL || s->unit == NULL)
		return FALSE;
	s->unit_cap = RP_UNIT_FIRST;
	rp_stream_init(s->stream);
	session_parse_reset(s);
	for (unsigned int i = 0; i < RP_PICTURES_IN_FLIGHT; i++)
	{
		struct rp_session_picture *picture = &s->pictures[i];
		picture->session = s;
		picture->rec.slices = AllocVec(RP_SLICES_MAX * sizeof(*picture->rec.slices), MEMF_PUBLIC | MEMF_CLEAR);
		if (picture->rec.slices == NULL)
			return FALSE;
		picture->rec.slice_cap = RP_SLICES_MAX;
	}
	return TRUE;
}

/**
 * own_packets_make() - The backend's input packets, unless the client says how many and how large.
 * @s: the session
 *
 * Return: TRUE; FALSE without memory.
 */
static BOOL own_packets_make(struct rp_session *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	u32 count = s->params.InputCount != 0 ? s->params.InputCount : RP_INPUT_COUNT;
	u32 size = s->params.InputSize != 0 ? s->params.InputSize : RP_INPUT_SIZE;
	for (u32 i = 0; i < count; i++)
	{
		APTR data = AllocVec(size, MEMF_PUBLIC);
		if (data == NULL || packet_make(s, data, size, TRUE) == NULL)
		{
			FreeVec(data);
			return FALSE;
		}
	}
	return TRUE;
}

/**
 * session_start() - A fresh session made ready to take packets: its memory, the hardware, the extradata.
 * @s:      the session
 * @params: the frontend's parameters, for the extradata
 *
 * The hardware comes up for the first session.  Extradata, if given, is
 * fed to the stream layer at once (an SPS there makes the frames).
 *
 * Return: 0, or the VCERR_* the open fails with.
 */
static LONG session_start(struct rp_session *s, const struct VCSessionParams *params)
{
	if (!session_memory_make(s) || !own_packets_make(s))
		return VCERR_NOMEM;
	if (!hardware_up(s->base))
		return VCERR_TRANSPORT;
	if (params->Extradata == NULL || params->ExtradataSize == 0)
		return 0;
	return extradata_feed(s, params->Extradata, params->ExtradataSize);
}

/**
 * session_open() - A decoder session (VCB_OpenSession()).
 * @base:   the library base, sessions_lock held
 * @params: the frontend's parameters
 * @error:  where the error goes, 0 on success
 *
 * Return: the session, NULL with *@error set.
 */
struct rp_session *session_open(struct RpividBase *base, const struct VCSessionParams *params, LONG *error)
{
	struct ExecBase *SysBase = base->SysBase;

	*error = VCERR_UNSUPPORTED;
	if (params->Codec != VCC_H265 || params->Kind != VCK_Decoder)
		return NULL;
	if (params->OutputFormat != 0 && params->OutputFormat != VCF_NV12_COL128)
		return NULL;
	*error = VCERR_NOMEM;
	struct rp_session *s = session_alloc(base, params);
	if (s == NULL)
		return NULL;
	*error = session_start(s, params);
	if (*error != 0)
	{
		session_free(s);
		return NULL;
	}
	AddTail((struct List *)&base->sessions, (struct Node *)&s->node);
	base->session_count++;
	return s;
}

/**
 * pictures_withdraw() - Every picture of the session out of the block and given up, the one being built too.
 * @s: the session
 *
 * The queued ones are dropped, those in the block waited for.  The wait
 * outlasts the wedge watch (session.h), so it ends with the pictures out of
 * the block even when a phase never ends: the watch fails them and frees
 * their slots.  Only a decode task kept from running could let it time
 * out; the session then fails, and what follows frees memory the block may
 * still be given.
 */
static void pictures_withdraw(struct rp_session *s)
{
	struct rp_hw *hw = &s->base->hw;
	rp_hw_withdraw(hw, s);
	if (rp_hw_wait_quiet(hw, s, RP_FLUSH_WAIT_MS) != DRV_WAIT_OK)
		session_fail(s, VCERR_TRANSPORT, "the block did not finish the pictures in time");
	for (unsigned int i = 0; i < RP_PICTURES_IN_FLIGHT; i++)
		if (s->pictures[i].in_use)
			picture_release(s, &s->pictures[i]);
	s->parsing = NULL;
}

/**
 * session_close() - The session torn down (VCB_CloseSession()).
 * @s: the session, unlinked from the base, its lock held by the caller
 */
void session_close(struct rp_session *s)
{
	pictures_withdraw(s);
	session_free(s);
}

/**
 * session_import_packet() - A packet over the client's memory (VCB_ImportPacket()).
 * @s:        the session
 * @side:     VCS_Input
 * @data:     the memory
 * @capacity: its bytes
 *
 * Return: the packet, NULL if refused (after the first send, or not the input side).
 */
struct VCPacket *session_import_packet(struct rp_session *s, ULONG side, APTR data, ULONG capacity)
{
	if (side != VCS_Input || s->state != RP_SESSION_OPEN || data == NULL || capacity == 0)
		return NULL;
	struct rp_packet *p = packet_make(s, data, capacity, FALSE);
	return p != NULL ? &p->pub : NULL;
}

/**
 * session_import_frame() - A frame over the client's memory (VCB_ImportFrame()).
 * @s:    the session
 * @side: VCS_Output
 * @data: the memory: 64-byte aligned, within the ISP's reach, as large as a frame of the shape
 *
 * The shape must be known: from the extradata's SPS, or from the Width
 * and Height tags, which then make the context.  A Main10 stream decodes
 * into the backend's frames only (architecture.md section 10): its imports are refused.
 *
 * Return: the frame, described, or NULL if refused.
 */
struct VCFrame *session_import_frame(struct rp_session *s, ULONG side, APTR data)
{
	if (side != VCS_Output || s->state != RP_SESSION_OPEN || data == NULL)
		return NULL;
	if (!s->ctx_ready)
	{
		if (s->params.Width == 0 || s->params.Height == 0 || shape_make(s, s->params.Width, s->params.Height, NULL) != 0)
			return NULL;
	}
	if (s->bit_depth != RP_DEPTH_MAIN)
		return NULL;
	u32 size = s->client_columns * s->ctx.column_bytes;
	ULONG start = (ULONG)data;
	if ((start & (RP_FRAME_ALIGN - 1u)) != 0 || start >= RP_ISP_REACH_END || size > RP_ISP_REACH_END - start)
		return NULL; /* the reach as a room left, so that a frame near the top of memory cannot wrap past the test */
	struct rp_frame *f = frame_make(s, data, TRUE);
	if (f == NULL)
		return NULL;
	f->flags = RPF_AT_CLIENT; /* an import starts in the client's hands */
	return &f->pub;
}

/**
 * session_send_packet() - A packet of the stream (VCB_SendPacket()).
 * @s:   the session
 * @pub: one of its packets, in the client's hands
 *
 * Return: 0, or VCERR_BADPARAM, VCERR_BADSTATE, or the failure the session is in.
 */
LONG session_send_packet(struct rp_session *s, struct VCPacket *pub)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct rp_packet *p = packet_of(s, pub);
	if (p == NULL || pub->Length > pub->Capacity)
		return VCERR_BADPARAM;
	if (!p->at_client)
		return VCERR_BADSTATE;
	if (s->state == RP_SESSION_FAILED)
		return s->failure;
	p->at_client = FALSE;
	if (s->state == RP_SESSION_ENDED)
	{
		s->end_pending = FALSE;
		s->end_sent = FALSE;
	}
	s->state = RP_SESSION_STREAMING;
	AddTail((struct List *)&s->pending, (struct Node *)&p->link);
	session_reap(s);
	session_parse(s);
	return 0;
}

/**
 * pictures_in_flight() - Whether a picture is still being built or in the block.
 * @s: the session
 *
 * Return: TRUE when one is.
 */
static BOOL pictures_in_flight(const struct rp_session *s)
{
	for (unsigned int i = 0; i < RP_PICTURES_IN_FLIGHT; i++)
		if (s->pictures[i].in_use)
			return TRUE;
	return FALSE;
}

/**
 * packet_hand_out() - A packet to the client, empty.
 * @p:    the packet
 * @item: the poll's item
 */
static void packet_hand_out(struct rp_packet *p, struct VCItem *item)
{
	p->at_client = TRUE;
	p->handed = TRUE;
	p->pub.Length = 0;
	p->pub.Flags = 0;
	item->Type = VCIT_PacketReturned;
	item->u.Packet = &p->pub;
}

/**
 * event_item() - A poll item that is an event with nothing but its code.
 * @item:  the poll's item
 * @event: VCEV_*
 *
 * Return: the event, for what else it carries.
 */
static struct VCEvent *event_item(struct VCItem *item, ULONG event)
{
	item->Type = VCIT_Event;
	memset(&item->u.Event, 0, sizeof(item->u.Event));
	item->u.Event.Event = event;
	return &item->u.Event;
}

/**
 * poll_packet() - A packet for the client: one consumed, else an own packet it has not had yet.
 * @s:    the session
 * @item: the poll's item
 *
 * Return: TRUE with @item filled.
 */
static BOOL poll_packet(struct rp_session *s, struct VCItem *item)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct MinNode *node = (struct MinNode *)RemHead((struct List *)&s->returned);
	if (node != NULL)
	{
		packet_hand_out(CONTAINER_OF(node, struct rp_packet, link), item);
		return TRUE;
	}
	for (node = s->packets.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct rp_packet *p = CONTAINER_OF(node, struct rp_packet, node);
		if (p->owned && !p->handed && !p->at_client)
		{
			packet_hand_out(p, item);
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * output_head() - The frame next to go out.
 * @s: the session
 *
 * Return: the head of the output list, NULL when it is empty.
 */
static struct rp_frame *output_head(const struct rp_session *s)
{
	if (s->output.mlh_Head->mln_Succ == NULL)
		return NULL;
	return CONTAINER_OF(s->output.mlh_Head, struct rp_frame, link);
}

/**
 * frame_narrow() - A decoded Main10 picture narrowed into a client's frame, with what the client is told of it.
 * @s:        the session
 * @decoded:  the block's 10-bit frame, invalidated
 * @narrowed: a free narrowed frame
 */
static void frame_narrow(struct rp_session *s, const struct rp_frame *decoded, struct rp_frame *narrowed)
{
	narrow_picture(decoded->buf.ptr, narrowed->buf.ptr, s->columns / RP_NARROW_IN_COLUMNS, s->ctx.column_bytes / CMDGEN_COLUMN_WIDTH);
	narrowed->flags = decoded->flags & RPF_CORRUPT;
	narrowed->pts = decoded->pts;
	narrowed->key = decoded->key;
	s->base->counters.narrowed++;
}

/**
 * poll_frame() - The head of the output for the client, once the block has decoded it.
 * @s:    the session
 * @item: the poll's item
 *
 * A Main10 picture is narrowed into a free narrowed frame first (architecture.md
 * section 10); it waits while the client holds them all.
 *
 * Return: TRUE with @item filled.
 */
static BOOL poll_frame(struct rp_session *s, struct VCItem *item)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct rp_frame *decoded = output_head(s);
	struct rp_frame *out = decoded;
	if (decoded == NULL || (decoded->flags & RPF_DECODED) == 0)
		return FALSE;
	if (s->bit_depth == RP_DEPTH_MAIN10)
		out = narrowed_take(s);
	if (out == NULL)
		return FALSE;
	Remove((struct Node *)&decoded->link);
	decoded->flags &= ~RPF_ON_OUTPUT;
	cache_post_dma(decoded->buf.ptr, (ULONG)decoded->buf.size, 0); /* the block wrote it: the 68k reads what it wrote */
	if (out != decoded)
		frame_narrow(s, decoded, out);
	out->flags |= RPF_AT_CLIENT;
	out->pub.Pts = out->pts;
	out->pub.Flags = out->key ? VCFF_KeyFrame : 0;
	item->Type = VCIT_Frame;
	item->u.Frame = &out->pub;
	return TRUE;
}

/**
 * poll_end() - The end of the stream, once the end mark went in and everything is out.
 * @s:    the session
 * @item: the poll's item
 *
 * Return: TRUE with @item filled.
 */
static BOOL poll_end(struct rp_session *s, struct VCItem *item)
{
	if (!s->end_pending || s->end_sent || output_head(s) != NULL || pictures_in_flight(s))
		return FALSE;
	s->end_sent = TRUE;
	s->state = RP_SESSION_ENDED;
	event_item(item, VCEV_EndOfStream);
	return TRUE;
}

/**
 * session_poll() - The next thing for the client (VCB_Poll()), in the order of docs/architecture.md section 5.5.
 * @s:    the session
 * @item: the poll's item
 *
 * The stream info before any frame of its shape; then events; a packet;
 * the head of the output when decoded; the end of the stream once
 * everything is out.
 *
 * Return: TRUE with @item filled, FALSE when nothing is ready.
 */
BOOL session_poll(struct rp_session *s, struct VCItem *item)
{
	session_reap(s);
	session_parse(s);
	if (s->info_pending)
	{
		s->info_pending = FALSE;
		event_item(item, VCEV_StreamInfo)->Stream = s->info;
		return TRUE;
	}
	return events_pop(s, item) || poll_packet(s, item) || poll_frame(s, item) || poll_end(s, item);
}

/**
 * session_release_frame() - The client is done with a frame (VCB_ReleaseFrame()).
 * @s:   the session
 * @pub: the frame
 *
 * A parse that stopped for want of a frame goes on.  A Main10 picture
 * that waited for this narrowed frame is ready for the next poll: the
 * client is told.
 */
void session_release_frame(struct rp_session *s, struct VCFrame *pub)
{
	struct rp_frame *f = frame_of(s, pub);
	if (f == NULL || (f->flags & RPF_AT_CLIENT) == 0)
		return;
	f->flags &= ~RPF_AT_CLIENT;
	session_reap(s);
	session_parse(s);
	const struct rp_frame *waiting = output_head(s);
	if (!f->decode_target && waiting != NULL && (waiting->flags & RPF_DECODED) != 0)
		signal_client(s);
}

/**
 * session_flush() - A seek (VCB_Flush()): nothing in flight, the buffer empty, every packet back.
 * @s: the session
 *
 * The client's frames stay with it; the next picture is taken as a random
 * access point.
 *
 * Return: 0, or the failure the session is in.
 */
LONG session_flush(struct rp_session *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct MinNode *node;
	pictures_withdraw(s);
	rp_stream_flush(s->stream);
	while ((node = (struct MinNode *)RemHead((struct List *)&s->output)) != NULL)
		CONTAINER_OF(node, struct rp_frame, link)->flags &= ~RPF_ON_OUTPUT;
	if (s->scanning != NULL)
	{
		AddTail((struct List *)&s->returned, (struct Node *)&s->scanning->link);
		s->scanning = NULL;
	}
	while ((node = (struct MinNode *)RemHead((struct List *)&s->pending)) != NULL)
		AddTail((struct List *)&s->returned, (struct Node *)node);
	session_parse_reset(s);
	s->end_pending = FALSE;
	s->end_sent = FALSE;
	if (s->state == RP_SESSION_ENDED)
		s->state = RP_SESSION_STREAMING;
	signal_client(s);
	return s->state == RP_SESSION_FAILED ? s->failure : 0;
}

/**
 * session_set_attrs() - A running session's attributes (VCB_SetAttrs()).
 * @s:       the session
 * @changes: what changed
 *
 * The signal is taken; an output geometry is ignored: a decoder's native
 * output has none, the chain's converter has it.
 *
 * Return: 0.
 */
LONG session_set_attrs(struct rp_session *s, const struct VCSessionParams *changes)
{
	if (changes->SigTask != NULL)
		s->sig_task = changes->SigTask;
	if (changes->SigMask != 0)
		s->sig_mask = changes->SigMask;
	return 0;
}

/**
 * session_output_frame() - One of the session's frames, by index (VCB_OutputFrame()).
 * @s:     the session
 * @index: 0 upward
 *
 * Return: the frame, NULL past the last or before the shape is known.
 */
struct VCFrame *session_output_frame(struct rp_session *s, ULONG index)
{
	for (struct MinNode *node = s->frames.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
		if (index-- == 0)
			return &CONTAINER_OF(node, struct rp_frame, node)->pub;
	return NULL;
}
