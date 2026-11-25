/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rpivid_private.h - rpivid.codec's own structures: the hardware layer's
 * (docs/architecture.md section 8), the library base and the backend's entry
 * points.
 *
 * Every file of the module includes this first: it defines __NOLIBBASE__,
 * which the proto headers included after it must see.  It also brings the
 * command generator's interface (hevc_d_h265.h) to every file.
 */
#ifndef RPIVID_PRIVATE_H
#define RPIVID_PRIVATE_H

#define __NOLIBBASE__ /* the proto headers declare no global bases: every call names its base */

#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/semaphores.h>
#include <libraries/videocodec_backend.h>

#include <dma_mem.h>
#include <drv_waitq.h>
#include <hw_claim.h>
#include <minlist.h>
#include <reset_guard.h>
#include <types.h>

#include "hevc_d_h265.h" /* the command generator's interface (src/cmdgen/) */

#define LIBRARY_PRIORITY 0 /* the resident's priority (rpivid_lib.c): a library opened on demand, no order to keep */

/* ---- the hardware layer (docs/architecture.md section 8) ---------------------- */

#define RP_SLOTS HEVC_D_P2BUF_COUNT   /* scratch slots: one picture in each phase and one between */
#define RP_HEVC_SYMBOL "hevc_dec"      /* the device tree's label */
#define RP_HEVC_CLOCK 11               /* the firmware's clock id */
#define RP_TASK_NAME "rpivid.codec"   /* the decode task's name, and the interrupt server's */
#define RP_TASK_PRIORITY 10            /* above Executive's band: a busy player never starves the block */
#define RP_TASK_STACK 8192             /* the decode task's stack in bytes */
#define RP_TICK_MS 250                 /* the wedge watch while a phase runs */
#define RP_WEDGE_US 1000000u           /* a phase older than this is a wedged block */
#define RP_ISP_REACH_END 0x40000000u   /* the VPU reaches the lowest GiB: where every frame must be */
#define RP_FRAME_ALIGN (1u << HEVC_D_UNIT_SHIFT) /* what a frame's memory is aligned to: the block's 64-byte unit, a cache line too */

/* Where a picture is on its way through the block */
enum rp_picture_state : u8
{
	RP_PIC_FREE,        /* no picture in it */
	RP_PIC_PARSING,     /* the session is taking its slices */
	RP_PIC_QUEUED,      /* on hw->queue */
	RP_PIC_PHASE1,      /* in the block's entropy stage */
	RP_PIC_PHASE1_DONE, /* on hw->phase1_done, holding its slot */
	RP_PIC_PHASE2,      /* in the reconstruction stage */
	RP_PIC_DONE,        /* retired, decoded */
	RP_PIC_FAILED,      /* retired, dropped */
};

/* A picture as the hardware layer sees it; the stream layer's fields join it (architecture.md section 6.6) */
struct rp_picture
{
	struct MinNode node;                  /* on hw->queue, then hw->phase1_done */
	APTR owner;                           /* who queued it: a session (rp_hw_withdraw()) */
	struct hevc_d_ctx *ctx;               /* its command-generator context */
	struct hevc_d_dec_env *de;            /* its environment: the command list, the phase-2 values */
	enum rp_picture_state state;          /* where it is on its way through the block */
	struct rp_slot *slot;                 /* from phase 1 to the end of phase 2 */
	int status;                           /* phase 1's last result (cmdgen_phase1_result()) */
	u32 reruns;                           /* phase 1 run again after a scratch buffer grew */
	void (*retire)(struct rp_picture *p); /* the owner's, called by the task once DONE or FAILED, no lock held */
};

/* A scratch slot: phase 1 writes its buffers, phase 2 reads them; only the block touches them */
struct rp_slot
{
	struct hevc_d_hwbuf coeff;  /* the coefficients */
	struct hevc_d_hwbuf pu;     /* the prediction units */
	struct rp_picture *picture; /* from phase 1's start to phase 2's end, else NULL */
};

/*
 * What the interrupt server works on (is_Data); hw_isr.c reads it by these
 * offsets.  A phase's end reaches the task as a signal of its own, so
 * nothing here is written by both the server and the task: Wait() hands the
 * task the ends that came, atomically.
 */
struct rp_isr_data
{
	volatile u32 *ictrl; /* the interrupt control register */
	struct Task *task;   /* the decode task */
	ULONG phase1_mask;   /* its signal for phase 1's end (ACTIVE1) */
	ULONG phase2_mask;   /* its signal for phase 2's end (ACTIVE2) */
	ULONG count;         /* interrupts served */
};

/* The block: what hw.c brings up and down, and what the decode task runs */
struct rp_hw
{
	struct RpividBase *base;           /* the library */
	struct hevc_d_dev dev;             /* what the command generator sees: SysBase, the pool, the registers */
	APTR DeviceTreeBase;               /* devicetree.resource: the block's registers and interrupt */
	APTR MailboxBase;                  /* mailbox.resource: the block's firmware clock */
	struct Library *gic400Base;        /* gic400.library: the interrupt server goes there */
	LONG irq;                          /* the GIC interrupt */
	struct hw_claim claim;             /* the stack's claim on the block, by its device tree label */
	BOOL claimed;                      /* the claim is held */
	ULONG clock_state_found;           /* the clock's state at up: put back at down */
	u32 ictrl_found;                   /* the interrupt control register at up: put back, its sources cleared, at down */
	struct Interrupt server;           /* the interrupt server (hw_isr.c) */
	BOOL server_added;                 /* it is on the GIC's chain */
	struct rp_isr_data isr;            /* what the server works on */
	struct Task *task;                 /* the decode task; NULL while down */
	ULONG sig_queue;                   /* the task's: a picture queued, or something else to look at */
	struct SignalSemaphore queue_lock; /* the queue and the slots' states; never held across a wait (architecture.md section 3.2) */
	struct drv_waitq waitq;            /* waiters for "none of my pictures in the block" (rp_hw_wait_quiet()) */
	struct MinList queue;              /* struct rp_picture, QUEUED, arrival order, every owner's */
	struct MinList phase1_done;        /* PHASE1_DONE, in order */
	struct rp_slot slots[RP_SLOTS];    /* the scratch slots */
	size_t slot_coeff_want;            /* the size the slots' coefficient buffers are to have: the largest session's, grown by exhaustion */
	size_t slot_pu_want;               /* the same for the PU buffers */
	struct rp_picture *in_phase1;      /* the picture in phase 1, else NULL */
	struct rp_picture *in_phase2;      /* the picture in phase 2, else NULL */
	u32 phase1_started_us;             /* when phase 1 started, for the wedge watch */
	u32 phase2_started_us;             /* when phase 2 started */
	BOOL wedged;                       /* a phase never ended: nothing starts any more */
	volatile BOOL halted;              /* the reset guard's prepare ran */
	u32 reruns;                        /* phase 1 runs again after a scratch buffer grew */
	u32 wedges;                        /* phases that never ended; the interrupts are counted by the server (isr.count) */
};

/* hw.c */
int rp_hw_up(struct rp_hw *hw, struct RpividBase *base);
void rp_hw_down(struct rp_hw *hw);
void rp_hw_slots_want(struct rp_hw *hw, u32 width, u32 height);
int rp_hw_queue(struct rp_hw *hw, struct rp_picture *p);
void rp_hw_withdraw(struct rp_hw *hw, APTR owner);
void rp_hw_slot_release(struct rp_hw *hw, struct rp_slot *slot);
LONG rp_hw_wait_quiet(struct rp_hw *hw, APTR owner, ULONG timeout_ms);
void rp_hw_reset_prepare(APTR user);

/* hw_task.c */
void rp_hw_task(struct rp_hw *hw, struct Task *parent);

/* hw_isr.c: the server gic400 calls; answers with the Z flag */
ULONG rp_hw_isr(struct ExecBase *SysBase asm("a6"), struct rp_isr_data *d asm("a1"), ULONG irq asm("d0"));

/* ---- the library ----------------------------------------------------------- */

struct rp_counters
{
	u32 pictures; /* queued for the block */
	u32 dropped;  /* pictures not decoded (architecture.md section 11) */
	u32 narrowed; /* Main10 pictures narrowed to 8 bits by the 68k (architecture.md section 10) */
};

/* Whether the device tree has the block, as rpivid_codecs.c asks once */
enum rp_block : s8
{
	RP_BLOCK_UNKNOWN = -1, /* not asked yet */
	RP_BLOCK_NONE,         /* no hevc_dec node: a Pi 3, the codec table is empty */
	RP_BLOCK_PRESENT,      /* the block is there */
};

struct RpividBase
{
	struct Library lib;                   /* the library node */
	ULONG segList;                        /* the code's segments, given back at the expunge */
	struct ExecBase *SysBase;             /* exec */
	struct SignalSemaphore sessions_lock; /* the session list, the hardware's up and down */
	struct MinList sessions;              /* struct rp_session, the open ones */
	u32 session_count;                    /* written under sessions_lock; read without it by VCB_GetInfo() */
	struct dma_mem_ctx dma_ctx;           /* Emu68's RAM regions, those wholly within the ISP's reach (pool_make(), hw.c) */
	struct dma_pool *pool;                /* made at the first hardware up, deleted at the expunge */
	struct rp_hw hw;                      /* the block */
	BOOL hw_up;                           /* the block is up: brought up by the first session, let go at the frontend's idle with none open (VCB_Idle()) */
	struct reset_guard reset_guard;       /* quiets the block before a reset (rp_hw_reset_prepare()) */
	BOOL reset_guarded;                   /* the guard is installed: removed at the expunge */
	struct rp_counters counters;          /* what VCB_GetInfo() reports */
	enum rp_block block_found;            /* the device tree's answer (rpivid_codecs.c) */
};

/* the backend's functions (rpivid_backend.c) */
LONG VCB_GetInfo(struct VCInfo *info asm("a0"), struct RpividBase *base asm("a6"));
LONG VCB_QueryCodec(ULONG index asm("d0"), struct VCCodecInfo *info asm("a0"), struct RpividBase *base asm("a6"));
APTR VCB_OpenSession(struct VCSessionParams *params asm("a0"), LONG *error asm("a1"), struct RpividBase *base asm("a6"));
VOID VCB_CloseSession(APTR handle asm("a0"), struct RpividBase *base asm("a6"));
struct VCPacket *VCB_ImportPacket(APTR handle asm("a0"), ULONG side asm("d0"), APTR data asm("a1"), ULONG capacity asm("d1"),
                                  struct RpividBase *base asm("a6"));
struct VCFrame *VCB_ImportFrame(APTR handle asm("a0"), ULONG side asm("d0"), APTR data asm("a1"), struct RpividBase *base asm("a6"));
LONG VCB_SendPacket(APTR handle asm("a0"), struct VCPacket *packet asm("a1"), struct RpividBase *base asm("a6"));
LONG VCB_SendFrame(APTR handle asm("a0"), struct VCFrame *frame asm("a1"), struct RpividBase *base asm("a6"));
LONG VCB_Poll(APTR handle asm("a0"), struct VCItem *item asm("a1"), struct RpividBase *base asm("a6"));
VOID VCB_ReleaseFrame(APTR handle asm("a0"), struct VCFrame *frame asm("a1"), struct RpividBase *base asm("a6"));
VOID VCB_ReleasePacket(APTR handle asm("a0"), struct VCPacket *packet asm("a1"), struct RpividBase *base asm("a6"));
LONG VCB_Flush(APTR handle asm("a0"), struct RpividBase *base asm("a6"));
LONG VCB_SetAttrs(APTR handle asm("a0"), struct VCSessionParams *params asm("a1"), struct RpividBase *base asm("a6"));
LONG VCB_Image(struct VCImageParams *params asm("a0"), struct RpividBase *base asm("a6"));
VOID VCB_Idle(struct RpividBase *base asm("a6"));
struct VCFrame *VCB_OutputFrame(APTR handle asm("a0"), ULONG index asm("d0"), struct RpividBase *base asm("a6"));

/* the codec table (rpivid_codecs.c) */
ULONG codec_count(struct RpividBase *base);
LONG codec_query(struct RpividBase *base, ULONG index, struct VCCodecInfo *info);

#endif /* RPIVID_PRIVATE_H */
