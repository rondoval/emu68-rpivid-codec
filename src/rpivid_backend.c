/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rpivid_backend.c - the videocodec backend ABI (VCB_*): the library's
 * functions.  Each session call finds its session under sessions_lock,
 * takes the session's lock, lets sessions_lock go and calls the session
 * layer (docs/architecture.md section 3.2).
 */
#include "session.h" /* rpivid_private.h first: __NOLIBBASE__ */

#include <proto/exec.h>

#include <format.h>
#include <memory.h>
#include <strutil.h>

#include "hevc_d_hw.h" /* HEVC_D_VERSION, for the interface string */

/**
 * session_take() - The session behind a handle, its lock held.
 * @base:   the library base
 * @handle: what the frontend passed
 *
 * Return: the session, NULL for no open session.
 */
static struct rp_session *session_take(struct RpividBase *base, APTR handle)
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->sessions_lock);
	struct rp_session *s = session_lookup(base, handle);
	if (s != NULL)
		ObtainSemaphore(&s->lock);
	ReleaseSemaphore(&base->sessions_lock);
	return s;
}

/**
 * session_give() - The session's lock let go.
 * @s: the session
 */
static void session_give(struct rp_session *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	ReleaseSemaphore(&s->lock);
}

/**
 * add_counter() - One counter into the caller's list, if it has room, and counted.
 * @info:  the structure being filled
 * @name:  the counter's name
 * @value: its value
 */
static void add_counter(struct VCInfo *info, const char *name, ULONG value)
{
	if (info->NumCounters < info->MaxCounters)
	{
		struct VCCounter *c = &info->Counters[info->NumCounters];
		strlcpy((char *)c->Name, name, sizeof(c->Name));
		c->Value = value;
	}
	info->NumCounters++;
}

/**
 * VCB_GetInfo() - What the backend is and how it is doing.
 * @info: the caller's structure (A0), filled as far as its Size reaches
 * @base: the library base (A6)
 *
 * The interface is the block's version while it is up; the counters are
 * pictures (queued), dropped, narrowed, reruns, interrupts and wedged
 * (architecture.md section 5.2).
 *
 * Return: 0, or VCERR_BADPARAM for a structure too small for the ABI version.
 */
LONG VCB_GetInfo(struct VCInfo *info asm("a0"), struct RpividBase *base asm("a6"))
{
	if (info == NULL || !VC_HAS_FIELD(info->Size, struct VCInfo, ABIVersion))
		return VCERR_BADPARAM;
	struct VCInfo all;
	memset(&all, 0, sizeof(all));
	all.Size = info->Size < sizeof(all) ? info->Size : (ULONG)sizeof(all);
	if (VC_HAS_FIELD(all.Size, struct VCInfo, MaxCounters) && info->Counters != NULL)
	{
		all.Counters = info->Counters;
		all.MaxCounters = info->MaxCounters;
	}
	all.ABIVersion = VCB_ABI_VERSION;
	all.Codecs = codec_count(base);
	strlcpy((char *)all.Backend, LIBRARY_NAME, sizeof(all.Backend));
	all.Sessions = base->session_count;
	if (base->hw_up)
		_SNPrintf(all.Interface, sizeof(all.Interface), (CONST_STRPTR) "HEVC 0x%lx", (ULONG)HEVC_D_VERSION);
	add_counter(&all, "pictures", base->counters.pictures);
	add_counter(&all, "dropped", base->counters.dropped);
	add_counter(&all, "narrowed", base->counters.narrowed);
	add_counter(&all, "reruns", base->hw.reruns);
	add_counter(&all, "interrupts", base->hw.isr.count);
	add_counter(&all, "wedged", base->hw.wedges);
	memcpy(info, &all, all.Size);
	return 0;
}

/**
 * VCB_QueryCodec() - One row of the backend's codec table.
 * @index: the row (D0), 0 upward
 * @info:  the caller's structure (A0), filled as far as its Size reaches
 * @base:  the library base (A6)
 *
 * Return: 0, or VCERR_BADPARAM past the last row or for a structure too small.
 */
LONG VCB_QueryCodec(ULONG index asm("d0"), struct VCCodecInfo *info asm("a0"), struct RpividBase *base asm("a6"))
{
	if (info == NULL || !VC_HAS_FIELD(info->Size, struct VCCodecInfo, Kind))
		return VCERR_BADPARAM;
	return codec_query(base, index, info);
}

/**
 * VCB_OpenSession() - Open a decoder session.
 * @params: the frontend's parameters (A0)
 * @error:  where the error goes (A1), 0 on success; may be NULL
 * @base:   the library base (A6)
 *
 * Return: the session's handle, NULL with *@error set.
 */
APTR VCB_OpenSession(struct VCSessionParams *params asm("a0"), LONG *error asm("a1"), struct RpividBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	LONG result = VCERR_BADPARAM;
	struct rp_session *s = NULL;
	if (params != NULL && VC_HAS_FIELD(params->Size, struct VCSessionParams, ExtradataSize))
	{
		ObtainSemaphore(&base->sessions_lock);
		s = session_open(base, params, &result);
		ReleaseSemaphore(&base->sessions_lock);
	}
	if (error != NULL)
		*error = result;
	return s;
}

/**
 * VCB_CloseSession() - Close a session; an unknown handle is ignored.
 * @handle: the session (A0)
 * @base:   the library base (A6)
 *
 * Unlinked under sessions_lock, so no call finds it any more; then its
 * own lock, so a call in flight finishes first; then torn down.
 */
VOID VCB_CloseSession(APTR handle asm("a0"), struct RpividBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->sessions_lock);
	struct rp_session *s = session_lookup(base, handle);
	if (s != NULL)
	{
		Remove((struct Node *)&s->node);
		base->session_count--;
	}
	ReleaseSemaphore(&base->sessions_lock);
	if (s == NULL)
		return;
	ObtainSemaphore(&s->lock);
	session_close(s);
}

/**
 * VCB_ImportPacket() - A packet over the caller's memory: a decoder's input.
 * @handle:   the session (A0)
 * @side:     VCS_Input (D0)
 * @data:     the caller's memory (A1)
 * @capacity: its bytes (D1)
 * @base:     the library base (A6)
 *
 * Return: the packet, NULL if refused.
 */
struct VCPacket *VCB_ImportPacket(APTR handle asm("a0"), ULONG side asm("d0"), APTR data asm("a1"), ULONG capacity asm("d1"),
                                  struct RpividBase *base asm("a6"))
{
	struct rp_session *s = session_take(base, handle);
	if (s == NULL)
		return NULL;
	struct VCPacket *packet = session_import_packet(s, side, data, capacity);
	session_give(s);
	return packet;
}

/**
 * VCB_ImportFrame() - A frame over the caller's memory: a decoder's output.
 * @handle: the session (A0)
 * @side:   VCS_Output (D0)
 * @data:   the caller's memory (A1)
 * @base:   the library base (A6)
 *
 * Return: the frame, described, or NULL if refused.
 */
struct VCFrame *VCB_ImportFrame(APTR handle asm("a0"), ULONG side asm("d0"), APTR data asm("a1"), struct RpividBase *base asm("a6"))
{
	struct rp_session *s = session_take(base, handle);
	if (s == NULL)
		return NULL;
	struct VCFrame *frame = session_import_frame(s, side, data);
	session_give(s);
	return frame;
}

/**
 * VCB_SendPacket() - Send a packet of the stream.
 * @handle: the session (A0)
 * @packet: the packet (A1)
 * @base:   the library base (A6)
 *
 * Return: 0 or VCERR_*.
 */
LONG VCB_SendPacket(APTR handle asm("a0"), struct VCPacket *packet asm("a1"), struct RpividBase *base asm("a6"))
{
	if (packet == NULL)
		return VCERR_BADPARAM;
	struct rp_session *s = session_take(base, handle);
	if (s == NULL)
		return VCERR_BADPARAM;
	LONG error = session_send_packet(s, packet);
	session_give(s);
	return error;
}

/**
 * VCB_SendFrame() - A decoder takes no pictures.
 * @handle: the session (A0)
 * @frame:  the frame (A1)
 * @base:   the library base (A6)
 *
 * Return: VCERR_BADSTATE.
 */
LONG VCB_SendFrame(APTR handle asm("a0"), struct VCFrame *frame asm("a1"), struct RpividBase *base asm("a6"))
{
	(void)handle;
	(void)frame;
	(void)base;
	return VCERR_BADSTATE;
}

/**
 * VCB_Poll() - The next thing for the client.
 * @handle: the session (A0)
 * @item:   where it goes (A1); VCIT_None when there is none
 * @base:   the library base (A6)
 *
 * Return: 1 with @item filled, 0 when nothing is left.
 */
LONG VCB_Poll(APTR handle asm("a0"), struct VCItem *item asm("a1"), struct RpividBase *base asm("a6"))
{
	if (item == NULL)
		return 0;
	BOOL ready = FALSE;
	struct rp_session *s = session_take(base, handle);
	if (s != NULL)
	{
		ready = session_poll(s, item);
		session_give(s);
	}
	if (!ready)
		item->Type = VCIT_None;
	return ready ? 1 : 0;
}

/**
 * VCB_ReleaseFrame() - The client is done with a frame.
 * @handle: the session (A0)
 * @frame:  the frame (A1)
 * @base:   the library base (A6)
 */
VOID VCB_ReleaseFrame(APTR handle asm("a0"), struct VCFrame *frame asm("a1"), struct RpividBase *base asm("a6"))
{
	if (frame == NULL)
		return;
	struct rp_session *s = session_take(base, handle);
	if (s == NULL)
		return;
	session_release_frame(s, frame);
	session_give(s);
}

/**
 * VCB_ReleasePacket() - A decoder gives no packets.
 * @handle: the session (A0)
 * @packet: the packet (A1)
 * @base:   the library base (A6)
 */
VOID VCB_ReleasePacket(APTR handle asm("a0"), struct VCPacket *packet asm("a1"), struct RpividBase *base asm("a6"))
{
	(void)handle;
	(void)packet;
	(void)base;
}

/**
 * VCB_Flush() - A seek: drop everything in flight.
 * @handle: the session (A0)
 * @base:   the library base (A6)
 *
 * Return: 0 or VCERR_*.
 */
LONG VCB_Flush(APTR handle asm("a0"), struct RpividBase *base asm("a6"))
{
	struct rp_session *s = session_take(base, handle);
	if (s == NULL)
		return VCERR_BADPARAM;
	LONG error = session_flush(s);
	session_give(s);
	return error;
}

/**
 * VCB_SetAttrs() - Change a running session.
 * @handle: the session (A0)
 * @params: what changed (A1)
 * @base:   the library base (A6)
 *
 * Return: 0 or VCERR_*.
 */
LONG VCB_SetAttrs(APTR handle asm("a0"), struct VCSessionParams *params asm("a1"), struct RpividBase *base asm("a6"))
{
	if (params == NULL || !VC_HAS_FIELD(params->Size, struct VCSessionParams, SigMask))
		return VCERR_BADPARAM;
	struct rp_session *s = session_take(base, handle);
	if (s == NULL)
		return VCERR_BADPARAM;
	LONG error = session_set_attrs(s, params);
	session_give(s);
	return error;
}

/**
 * VCB_Image() - The image calls: none here, a stream decoder only.
 * @params: the call (A0)
 * @base:   the library base (A6)
 *
 * Return: VCERR_UNSUPPORTED.
 */
LONG VCB_Image(struct VCImageParams *params asm("a0"), struct RpividBase *base asm("a6"))
{
	(void)params;
	(void)base;
	return VCERR_UNSUPPORTED;
}

/**
 * VCB_Idle() - The frontend's last close: the hardware goes down if no session is open.
 * @base: the library base (A6)
 */
VOID VCB_Idle(struct RpividBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->sessions_lock);
	if (base->session_count == 0)
		hardware_down(base);
	ReleaseSemaphore(&base->sessions_lock);
}

/**
 * VCB_OutputFrame() - One of a session's frames (ABI 1.1): what the frontend imports into a converter.
 * @handle: the session (A0)
 * @index:  0 upward (D0)
 * @base:   the library base (A6)
 *
 * Return: the frame, NULL past the last or before the shape is known.
 */
struct VCFrame *VCB_OutputFrame(APTR handle asm("a0"), ULONG index asm("d0"), struct RpividBase *base asm("a6"))
{
	struct rp_session *s = session_take(base, handle);
	if (s == NULL)
		return NULL;
	struct VCFrame *frame = session_output_frame(s, index);
	session_give(s);
	return frame;
}
