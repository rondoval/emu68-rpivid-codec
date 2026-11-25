/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hevc_d.h - the structures hevc_d_h265.c's kept functions see, in this
 * backend's shape (docs/architecture.md section 7): the block, a session's context,
 * a buffer, the record.  The names are upstream's so that the kept text
 * compiles as it is; the kernel's fields are gone.  The counts are upstream's
 * numbers, one of them for a reason of its own (HEVC_D_DEC_ENV_COUNT).
 *
 * The parts taken from Linux's hevc_dec driver are
 * Copyright (C) 2025 Raspberry Pi Ltd
 * Based on the Cedrus VPU driver, that is:
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 * (NOTICE section 1.)
 */
#ifndef HEVC_D_H
#define HEVC_D_H

#include <stddef.h> /* size_t */
#include <types.h>  /* u8, u32, dma_addr_t */

#include <v4l2_hevc_ctrls.h> /* the picture record */

#define HEVC_D_UNIT_SHIFT 6 /* the block takes addresses and lengths in 64-byte units (hevc_d_hw.h); frames start on one */
#define HEVC_D_UNIT_MASK ((1u << HEVC_D_UNIT_SHIFT) - 1)

/*
 * Not linux_compat.h: this header reaches every file of the module
 * (rpivid_private.h), and the kernel's macros stay in hevc_d_h265.c's
 * translation unit, which includes them first.
 */

/*
 * Pictures in flight per session: being parsed, in phase 1, in phase 2
 * (architecture.md section 3.3).  Upstream's 3 is its phase 0 to phase 1 queue per
 * context; here it bounds all of a session's pictures.
 */
#define HEVC_D_P1BUF_COUNT 3
/* Scratch slots per block: phase 1 may run this far ahead of phase 2 (architecture.md section 8.1) */
#define HEVC_D_P2BUF_COUNT 3
/*
 * Decode environments a session has.  Upstream has P1 + P2, as nothing there
 * bounds the environments in use apart; here a picture holds its environment
 * from its first slice until the session releases it after the retire
 * (session_parse.c), and a session has at most HEVC_D_P1BUF_COUNT pictures,
 * so one each is enough.
 */
#define HEVC_D_DEC_ENV_COUNT HEVC_D_P1BUF_COUNT

/* The picture record, as the stream layer hands it over (architecture.md section 6.6): the session points these at its record */
struct hevc_d_h265_run
{
	u32 slice_ents;                                             /* the slices in slice_params */
	const struct v4l2_ctrl_hevc_sps *sps;                       /* the active SPS's */
	const struct v4l2_ctrl_hevc_pps *pps;                       /* the active PPS's */
	const struct v4l2_ctrl_hevc_decode_params *dec;             /* the POC, the DPB array, the flags */
	const struct v4l2_ctrl_hevc_slice_params *slice_params;     /* one per slice, slice_ents of them */
	const struct v4l2_ctrl_hevc_scaling_matrix *scaling_matrix; /* the matrix in use; NULL for none */
};

/*
 * Upstream's run for a picture, less its source and destination buffers (the session's): kept as a wrapper
 * because the kept populate_scaling_factors() and cmdgen.c's upstream per-slice loop read run->h265
 */
struct hevc_d_run
{
	struct hevc_d_h265_run h265; /* the picture record */
};

/* A buffer the block reads or writes: from the pool, whole cache lines */
struct hevc_d_hwbuf
{
	size_t size;     /* its bytes; 0 for no buffer */
	u8 *ptr;         /* the 68k's address; NULL for no buffer */
	dma_addr_t addr; /* the block's address: the same, as Emu68 maps Fast RAM at its physical addresses (architecture.md section 9) */
};

struct dma_pool; /* dma_mem.h's: hevc_d_dev holds the pool */

/* Two pointer fields of struct hevc_d_dec_env name these; nothing here uses them */
struct vb2_v4l2_buffer;
struct media_request;

struct ExecBase;

/* The block (the hardware layer fills it in) */
struct hevc_d_dev
{
	struct ExecBase *SysBase; /* for the plumbing's allocations */
	struct dma_pool *pool;    /* every buffer the block touches */
	volatile u32 *base_h265;  /* the registers */
	volatile u32 *base_irq;   /* the interrupt control block */
};

struct hevc_d_dec_state;
struct hevc_d_dec_env;

/* A session's context: its decode state, its environments, its picture geometry */
struct hevc_d_ctx
{
	struct hevc_d_dev *dev;          /* the block */
	struct hevc_d_dec_state *state;  /* allocated with the context (cmdgen.c) */
	struct hevc_d_dec_env *dec_pool; /* HEVC_D_DEC_ENV_COUNT of them */
	struct hevc_d_dec_env *dec_free; /* the ones no picture holds, a list through ->next */
	u32 width;                       /* the coded picture's size in luma samples, as the SPS gives it */
	u32 height;
	u32 rows;                        /* luma rows of a frame, 16-aligned: where the chroma starts */
	u32 column_bytes;                /* one column: 128 x (rows + rows / 2) */
	u32 colmv_stride;                /* the collocated vectors: ALIGN(width, 64) ... */
	u32 colmv_picsize;               /* ... x ALIGN(height, 64) / 16 */
};

/* The plumbing's functions the kept functions call (cmdgen.c) */
int hwbuf_realloc_copy(struct hevc_d_dev *dev, struct hevc_d_hwbuf *hwbuf, size_t newsize);
void *hevc_d_kmalloc(struct hevc_d_ctx *ctx, size_t bytes);
void hevc_d_kfree(struct hevc_d_ctx *ctx, void *p);
struct hevc_d_ctx *hevc_d_ctx_of(const struct hevc_d_dec_state *s);

#endif /* HEVC_D_H */
