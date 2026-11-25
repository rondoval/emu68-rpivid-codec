/* SPDX-License-Identifier: GPL-2.0 */
/*
 * session.h - the session layer (docs/architecture.md section 5): a decoder
 * session's state, its packets and frames, its pictures in flight, and what
 * the backend's entry points call.  Only the session's files (session.c,
 * session_objects.c, session_parse.c) and rpivid_backend.c see this; the
 * lower layers do not.
 */
#ifndef RP_SESSION_H
#define RP_SESSION_H

#include <rpivid_private.h>

#include <bits.h> /* CONTAINER_OF */

#include "stream/nal.h"
#include "stream/record.h"
#include "stream/stream.h"

#define RP_SESSION_MAGIC 0x52505653u /* 'RPVS': what a handle points at when it is a live session */
#define RP_PICTURES_IN_FLIGHT HEVC_D_P1BUF_COUNT /* being parsed, in phase 1, in phase 2 (architecture.md section 3.3) */
#define RP_EVENTS_MAX 8             /* events queued for the client's polls */
#define RP_INPUT_COUNT 4            /* own packets, unless the client says */
#define RP_INPUT_SIZE (256u << 10)  /* bytes each */
#define RP_OUTPUT_COUNT 3           /* frames beyond the DPB: shown, being drawn, in the ISP */
#define RP_UNIT_FIRST (256u << 10)  /* the NAL unit buffer's first size; it doubles */
#define RP_UNIT_MAX (16u << 20)     /* a unit larger than this is not a stream (architecture.md section 11) */
#define RP_BITSTREAM_MAX (16u << 20) /* a picture's slices larger than this drop the picture */
#define RP_FLUSH_WAIT_MS 2000       /* for the block to finish a session's pictures: past the wedge watch, so it ends quietly (pictures_withdraw()) */
#define RP_MAIN10_WIDTH_MAX 1920    /* Main10 past this is refused: the pass costs too much (architecture.md section 10) */
#define RP_MAIN10_HEIGHT_MAX 1088
#define RP_TENBIT_SAMPLES 96        /* samples in a 10-bit column, three to a word */
#define RP_NV12_PLANES 2            /* a frame's planes: the luma, then the interleaved chroma */

/* A phase that never ends is wedged within RP_WEDGE_US and a tick, which fails its pictures and frees their slots: the flush wait must outlast that */
_Static_assert(RP_FLUSH_WAIT_MS * 1000ull > RP_WEDGE_US + RP_TICK_MS * 1000ull, "the flush wait outlasts the wedge watch");

/* Where a session is in its life */
enum rp_session_state : u8
{
	RP_SESSION_OPEN,      /* no packet sent yet: imports allowed */
	RP_SESSION_STREAMING, /* packets going in, frames coming out */
	RP_SESSION_ENDED,     /* the end mark went through */
	RP_SESSION_FAILED,    /* VCEV_Error queued; only the close is left */
};

/* a frame's flags */
#define RPF_AT_CLIENT (1u << 0) /* handed out, not released */
#define RPF_ON_OUTPUT (1u << 1) /* on the output list, waiting to be handed out */
#define RPF_DECODED (1u << 2)   /* the block is done with it */
#define RPF_CORRUPT (1u << 3)   /* its picture failed, or used a substitute reference */

struct rp_session;

/* An input packet: the backend's own or one the client imported */
struct rp_packet
{
	struct VCPacket pub;        /* first: the client's view */
	struct MinNode node;        /* on the session's packets list */
	struct MinNode link;        /* on pending, then returned */
	struct rp_session *session; /* the session it belongs to */
	BOOL owned;                 /* the backend's memory */
	BOOL at_client;             /* handed out and not sent back */
	BOOL handed;                /* an own packet the client has had once */
};

/* A frame: one the block decodes into, or a narrowed Main10 one the 68k writes */
struct rp_frame
{
	struct VCFrame pub;         /* first: the client's view */
	struct MinNode node;        /* on the session's frames list */
	struct MinNode link;        /* on the output list */
	struct rp_session *session; /* the session it belongs to */
	BOOL owned;                 /* the backend's memory, not one the client imported */
	BOOL decode_target;         /* the block decodes into it; else a narrowed frame the 68k writes (Main10) */
	struct hevc_d_hwbuf buf;    /* the picture: columns x column bytes, 64-byte aligned, within the ISP's reach */
	struct hevc_d_q_aux *aux;   /* the collocated vectors, the block's only; none for a narrowed frame */
	u32 number;                 /* the picture it holds, in decoding order; 0 for none */
	s32 poc;                    /* that picture's PicOrderCntVal */
	VCTime pts;                 /* the picture's */
	BOOL key;                   /* an IRAP picture */
	u32 flags;                  /* RPF_* */
};

/* A picture in flight: the hardware layer's view first, the stream layer's record behind */
struct rp_session_picture
{
	struct rp_picture hw;                                 /* first: what the hardware layer queues and retires */
	struct rp_session *session;                           /* the session it belongs to */
	BOOL in_use;                                          /* from the first slice to the reap */
	volatile enum rp_picture_state retired_state;         /* RP_PIC_FREE until the retire hook (the decode task) stores how it ended: DONE or FAILED */
	struct rp_record rec;                                 /* the stream layer's record; its slice array, RP_SLICES_MAX long, is made with the session */
	u32 slice_base[RP_SLICES_MAX];                        /* each slice's NAL unit's offset in the bitstream buffer */
	u32 slice_len[RP_SLICES_MAX];                         /* and its bytes */
	struct hevc_d_hwbuf bitstream;                        /* the slices' NAL units, back to back, for the block */
	u32 bitstream_used;                                   /* bytes of it taken */
	struct rp_frame *frame;                               /* the frame it is decoded into */
	struct rp_frame *refs[V4L2_HEVC_DPB_ENTRIES_NUM_MAX]; /* the frames of the record's DPB array; NULL for a missing one */
	VCTime pts;                                           /* the packet's that held the first slice */
	BOOL corrupt_input;                                   /* a VCPF_Corrupt packet fed it: dropped at completion */
};

/* A decoder session */
struct rp_session
{
	u32 magic;                      /* RP_SESSION_MAGIC while open */
	struct MinNode node;            /* on the library's session list */
	struct RpividBase *base;        /* the library */
	struct SignalSemaphore lock;    /* every call into the session takes it; the decode task does not */
	struct VCSessionParams params;  /* a copy; Extradata not kept */
	struct Task *sig_task;          /* the client's task, signalled when there is something to poll */
	ULONG sig_mask;                 /* the signal it is given */
	enum rp_session_state state;    /* where the session is */
	LONG failure;                   /* the VCERR_* it failed with, once FAILED */

	struct rp_stream *stream;       /* the stream layer's state, allocated once */
	struct hevc_d_ctx ctx;          /* the command generator's, for the shape */
	BOOL ctx_ready;                 /* ctx is made for the shape */
	u32 width;                      /* the shape the frames and the context are made for: the coded size */
	u32 height;
	u32 bit_depth;                  /* its bit depth: RP_DEPTH_MAIN, or RP_DEPTH_MAIN10 */
	u32 columns;                    /* the block's frames: 128-sample columns at 8 bits; 96-sample ones, rounded up to four, at 10 */
	u32 client_columns;             /* the client's frames: the same at 8 bits; three per four of the block's at 10 */
	struct rp_session_picture pictures[RP_PICTURES_IN_FLIGHT];
	struct rp_session_picture *parsing;    /* the one being built, or NULL */
	BOOL first_slice_taken;         /* the stream layer took the first slice held in the unit buffer: its picture waits for a frame or an environment */
	BOOL held_unit;                 /* the unit buffer holds a unit not yet processed */
	u32 held_type;                  /* the held unit's nal_unit_type */
	u32 held_temporal_id;           /* and its TemporalId */

	struct MinList packets;         /* struct rp_packet, every one */
	struct MinList pending;         /* sent, not yet scanned, in order */
	struct rp_packet *scanning;     /* the one the scanner is in */
	struct MinList returned;        /* consumed: VCIT_PacketReturned at the next polls */
	struct rp_nal nal;              /* the Annex B scanner, across packets */
	u8 *unit;                       /* the NAL unit being accumulated */
	u32 unit_len;                   /* its bytes so far */
	u32 unit_cap;                   /* the buffer's size: RP_UNIT_FIRST, doubled as needed up to RP_UNIT_MAX */
	BOOL unit_overflow;             /* the unit outgrew RP_UNIT_MAX: dropped at its end */
	BOOL scanning_done;             /* the scanner reached the packet's end */
	BOOL spanning;                  /* Framed: inside a picture spread over packets, from its VCPF_FrameStart one to its VCPF_FrameEnd one */

	struct MinList frames;          /* struct rp_frame, the client's: the block's at 8 bits, the narrowed ones at 10 */
	struct MinList tenbit;          /* struct rp_frame, Main10's 10-bit frames: the block's and the pass's only */
	struct MinList output;          /* the block's frames in display order; the head goes out when decoded */

	struct VCEvent events[RP_EVENTS_MAX]; /* the event ring */
	u32 event_head;                 /* its oldest entry */
	u32 event_count;                /* entries queued */
	struct VCStreamInfo info;       /* what the stream says of itself: size, crop, colours */
	BOOL info_pending;              /* info changed: a VCEV_StreamInfo item at the next poll */
	BOOL end_pending;               /* the end mark went in: VCEV_EndOfStream once the output is out */
	BOOL end_sent;                  /* VCEV_EndOfStream went out */
};

/**
 * rp_main10_too_large() - Whether a shape is a Main10 one past what the 68k's pass narrows in time (architecture.md section 10).
 * @depth:  the bit depth
 * @width:  the coded width
 * @height: the coded height
 *
 * Return: TRUE for 10 bits past RP_MAIN10_WIDTH_MAX or RP_MAIN10_HEIGHT_MAX: the session refuses the stream.
 */
static inline BOOL rp_main10_too_large(u32 depth, u32 width, u32 height)
{
	return depth == RP_DEPTH_MAIN10 && (width > RP_MAIN10_WIDTH_MAX || height > RP_MAIN10_HEIGHT_MAX);
}

/* What became of a NAL unit given to the session's parser */
enum unit_result : u8
{
	UNIT_DONE, /* taken */
	UNIT_HELD, /* waits for a frame or a picture slot: the unit stays, parsing stops */
	UNIT_STOP, /* the session failed */
};

/* session_objects.c: packets, frames, the shape */
void signal_client(struct rp_session *s);
void events_push(struct rp_session *s, ULONG event, LONG status);
BOOL events_pop(struct rp_session *s, struct VCItem *item);
void session_fail(struct rp_session *s, LONG error, const char *what);
struct rp_packet *packet_make(struct rp_session *s, APTR data, u32 capacity, BOOL owned);
struct rp_packet *packet_of(struct rp_session *s, const struct VCPacket *pub);
void packets_free(struct rp_session *s);
struct rp_frame *frame_make(struct rp_session *s, APTR data, BOOL decode_target);
struct rp_frame *frame_of(struct rp_session *s, const struct VCFrame *pub);
struct rp_frame *frame_by_number(struct rp_session *s, u32 number);
BOOL frame_is_free(struct rp_session *s, const struct rp_frame *f);
struct rp_frame *frame_take(struct rp_session *s);
struct rp_frame *narrowed_take(struct rp_session *s);
BOOL frames_all_free(struct rp_session *s);
BOOL frames_imported(const struct rp_session *s);
void shape_free(struct rp_session *s);
LONG shape_make(struct rp_session *s, u32 width, u32 height, const struct rp_sps *sps);
LONG shape_from_sps(struct rp_session *s, const struct rp_sps *sps);

/* session_parse.c: units into pictures, pictures into the queue, retired pictures reaped */
void picture_release(struct rp_session *s, struct rp_session_picture *picture);
void picture_abandon(struct rp_session *s, const char *why);
void session_reap(struct rp_session *s);
void output_drain(struct rp_session *s);
void session_parse(struct rp_session *s);
void session_parse_reset(struct rp_session *s);
LONG extradata_feed(struct rp_session *s, const u8 *data, u32 size);

/* session.c */
struct rp_session *session_open(struct RpividBase *base, const struct VCSessionParams *params, LONG *error);
void session_close(struct rp_session *session);
struct rp_session *session_lookup(struct RpividBase *base, APTR handle);
struct VCPacket *session_import_packet(struct rp_session *session, ULONG side, APTR data, ULONG capacity);
struct VCFrame *session_import_frame(struct rp_session *session, ULONG side, APTR data);
LONG session_send_packet(struct rp_session *session, struct VCPacket *packet);
BOOL session_poll(struct rp_session *session, struct VCItem *item);
void session_release_frame(struct rp_session *session, struct VCFrame *frame);
LONG session_flush(struct rp_session *session);
LONG session_set_attrs(struct rp_session *session, const struct VCSessionParams *changes);
struct VCFrame *session_output_frame(struct rp_session *session, ULONG index);
BOOL hardware_up(struct RpividBase *base);
void hardware_down(struct RpividBase *base);

#endif /* RP_SESSION_H */
