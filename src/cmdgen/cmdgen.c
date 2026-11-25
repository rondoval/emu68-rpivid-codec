/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cmdgen.c - the plumbing around Linux's command generator: what its V4L2
 * framework did, done for this backend (docs/architecture.md section 7.3).
 * Included at the end of hevc_d_h265.c, in its translation unit, so that it
 * reaches the kept static functions.  Where a block of code is upstream's, a
 * comment says so; everything else here is ours.  The blocks: the buffer
 * size formulas (hevc_d_video.c); the collocated-vector geometry
 * (aux_q_alloc()); hevc_d_h265_setup()'s frame start in picture_setup(),
 * its per-slice loop body in slice_commands() and its reference loop in
 * references_place(); the register blocks of phase1_claimed() and
 * phase2_claimed().  They differ from upstream only where this backend must:
 * types and casts for this file's -Wconversion rules, each slice counted from
 * its own NAL unit instead of one source buffer, the session's frames in
 * place of the V4L2 queues, and the changes marked "rpivid:".
 *
 * The parts taken from Linux's hevc_dec driver are
 * Copyright (C) 2025 Raspberry Pi Ltd
 * Based on the Cedrus VPU driver, that is:
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 * (NOTICE section 1.)
 */
#include <exec/memory.h>

#include <cache_ops.h> /* cache_pre_dma() */
#include <dma_mem.h>   /* dma_zalloc(), dma_free() */

#define ISP_ROW_ALIGN 16u      /* the luma rows of a frame: aligned as the ISP that converts it wants (docs/hardware-facts.md) */
#define FLAT_SCALING_FACTOR 16 /* m[x][y] without scaling lists (8.6.4.2) */

/* ---- sizes: Linux's formulas (hevc_d_video.c) ------------------------------ */

#define LEVEL_3_1_MAX_LUMA_PS 983040 /* MaxLumaPs of level 3.1 (Table A.8): where Annex A's least compression goes from 2 to 4 */

/**
 * hevc_d_round_up_size() - A buffer size rounded up as Linux's driver rounds: the next step up.
 * @x: the size wanted
 *
 * Always more than @x, so that a buffer that ran out grows (hw_task.c asks
 * for one byte more than it had): the smallest 3 or 4 times a power of two
 * above @x, and at least 384 (upstream: "admit no size < 256").  0 to 383
 * give 384, 384 gives 512, 512 gives 768.
 *
 * Return: the size.
 */
size_t hevc_d_round_up_size(const size_t x)
{
	const unsigned int n = x < 256 ? 7 : log2_floor_u32((u32)x) - 1;
	return x >= ((size_t)3 << n) ? (size_t)4 << n : (size_t)3 << n;
}

/**
 * hevc_d_bit_buf_size() - A bitstream buffer for a picture of a size, as Linux sizes it.
 * @w:           width
 * @h:           height
 * @bits_minus8: the bit depth above 8
 *
 * Annex A's least compression: 2 up to level 3.1's picture size, 4 above,
 * times 4:2:0's 3/2 samples per luma sample (so 3/4 and 3/8 of the luma
 * samples), plus the bit depth's share.  From level 3.1's size to twice it
 * the buffer stays level 3.1's, so that 983041 samples do not get less room
 * than 983040 (upstream's reason).
 *
 * Return: the bytes, rounded by hevc_d_round_up_size().
 */
size_t hevc_d_bit_buf_size(const unsigned int w, const unsigned int h, const unsigned int bits_minus8)
{
	const size_t wxh = (size_t)w * h;
	size_t bits_alloc = wxh < LEVEL_3_1_MAX_LUMA_PS       ? wxh * 3 / 4
	                    : wxh < LEVEL_3_1_MAX_LUMA_PS * 2 ? LEVEL_3_1_MAX_LUMA_PS * 3 / 4
	                                                      : wxh * 3 / 8;
	bits_alloc += (bits_alloc * bits_minus8) / 8;
	return hevc_d_round_up_size(bits_alloc);
}

/* ---- memory ------------------------------------------------------------------ */

/**
 * hwbuf_alloc() - A buffer the block touches, from the pool.
 * @dev:   the block
 * @hwbuf: filled in
 * @size:  bytes
 *
 * Whole cache lines, zeroed, then cleaned and invalidated so that no line of
 * it is in the 68k's cache: what the block writes there the 68k reads only
 * after an invalidate of its own.
 *
 * Return: 0, or -ENOMEM with @hwbuf empty.
 */
int hwbuf_alloc(struct hevc_d_dev *const dev, struct hevc_d_hwbuf *const hwbuf, const size_t size)
{
	hwbuf->size = size;
	hwbuf->ptr = dma_zalloc(dev->pool, 1u << HEVC_D_UNIT_SHIFT, (ULONG)size); /* the block's 64-byte unit, a cache line too */
	hwbuf->addr = (dma_addr_t)hwbuf->ptr;
	if (hwbuf->ptr == NULL)
	{
		hwbuf->size = 0;
		return -ENOMEM;
	}
	cache_pre_dma(hwbuf->ptr, (ULONG)size, 0);
	return 0;
}

/**
 * hwbuf_free() - Give a buffer back; an empty one is nothing.
 * @dev:   the block
 * @hwbuf: the buffer, left empty
 */
void hwbuf_free(struct hevc_d_dev *const dev, struct hevc_d_hwbuf *const hwbuf)
{
	if (hwbuf->ptr != NULL)
		dma_free(dev->pool, hwbuf->ptr);
	hwbuf->size = 0;
	hwbuf->ptr = NULL;
	hwbuf->addr = 0;
}

/**
 * hwbuf_realloc_new() - A bigger buffer, the contents not kept.
 * @dev:   the block
 * @hwbuf: the buffer
 * @size:  the new size
 *
 * The new one is taken before the old one goes, so a failure leaves the old
 * buffer as it was (Linux frees first and may end with none).
 *
 * Return: 0, or -ENOMEM with @hwbuf unchanged.
 */
int hwbuf_realloc_new(struct hevc_d_dev *const dev, struct hevc_d_hwbuf *const hwbuf, const size_t size)
{
	struct hevc_d_hwbuf bnew;
	if (size == hwbuf->size)
		return 0;
	if (hwbuf_alloc(dev, &bnew, size))
		return -ENOMEM;
	hwbuf_free(dev, hwbuf);
	*hwbuf = bnew;
	return 0;
}

/**
 * hwbuf_realloc_copy() - A bigger buffer with the contents kept (cmds_check_space()).
 * @dev:     the block
 * @hwbuf:   the buffer
 * @newsize: the new size; nothing happens unless it is larger
 *
 * Return: 0, or -ENOMEM with @hwbuf unchanged.
 */
int hwbuf_realloc_copy(struct hevc_d_dev *const dev, struct hevc_d_hwbuf *const hwbuf, const size_t newsize)
{
	struct hevc_d_hwbuf bnew;
	if (newsize <= hwbuf->size)
		return 0;
	if (hwbuf_alloc(dev, &bnew, newsize))
		return -ENOMEM;
	memcpy(bnew.ptr, hwbuf->ptr, hwbuf->size);
	hwbuf_free(dev, hwbuf);
	*hwbuf = bnew;
	return 0;
}

/**
 * hevc_d_kmalloc() - Ordinary memory for the context's tables.
 * @ctx:   the context
 * @bytes: how much
 *
 * Return: the memory, NULL without.
 */
void *hevc_d_kmalloc(struct hevc_d_ctx *const ctx, const size_t bytes)
{
	struct ExecBase *SysBase = ctx->dev->SysBase;
	return AllocVec((ULONG)bytes, MEMF_PUBLIC);
}

/**
 * hevc_d_kfree() - Free what hevc_d_kmalloc() gave; NULL is nothing.
 * @ctx: the context
 * @p:   the memory
 */
void hevc_d_kfree(struct hevc_d_ctx *const ctx, void *const p)
{
	struct ExecBase *SysBase = ctx->dev->SysBase;
	if (p != NULL)
		FreeVec(p);
}

/* The decode state lives in a block that knows its context */
struct cmdgen_state_block
{
	struct hevc_d_dec_state state; /* the kept functions' state: hevc_d_ctx_of() finds the block from it */
	struct hevc_d_ctx *ctx;        /* the context it belongs to */
};

/**
 * hevc_d_ctx_of() - The context a decode state belongs to (kmalloc_array(), kfree()).
 * @s: the state
 *
 * Return: the context.
 */
struct hevc_d_ctx *hevc_d_ctx_of(const struct hevc_d_dec_state *const s)
{
	return CONTAINER_OF(s, struct cmdgen_state_block, state)->ctx;
}

/* ---- the context --------------------------------------------------------------- */

/**
 * cmdgen_ctx_init() - A session's context: its state, its environments, its geometry.
 * @ctx:    filled in
 * @dev:    the block
 * @width:  the coded picture's width, as the SPS
 * @height: its height
 *
 * The frame geometry is the single-plane column layout's: the chroma rows
 * after the luma rows, one column 128 bytes wide (docs/architecture.md section
 * 5.3).  The luma rows are 16-aligned, as the ISP that converts the frames
 * wants them (docs/hardware-facts.md); upstream aligns its columns to 8.  The
 * collocated vectors' stride and picture size are upstream's (aux_q_alloc()).
 * Each environment gets a command list of CMD_BUFFER_SIZE_INIT entries;
 * cmds_check_space() grows one on need.
 *
 * Return: 0, or -ENOMEM with nothing kept.
 */
int cmdgen_ctx_init(struct hevc_d_ctx *const ctx, struct hevc_d_dev *const dev, const u32 width, const u32 height)
{
	struct ExecBase *SysBase = dev->SysBase;

	memset(ctx, 0, sizeof(*ctx));
	ctx->dev = dev;
	ctx->width = width;
	ctx->height = height;
	ctx->rows = ALIGN(height, ISP_ROW_ALIGN);
	ctx->column_bytes = CMDGEN_COLUMN_WIDTH * (ctx->rows + ctx->rows / 2u); /* the luma rows, then 4:2:0's half as many chroma rows */
	ctx->colmv_stride = ALIGN(width, 64u);                               /* upstream's (aux_q_alloc()) */
	ctx->colmv_picsize = ctx->colmv_stride * (ALIGN(height, 64u) >> 4);

	struct cmdgen_state_block *block = AllocVec(sizeof(*block), MEMF_PUBLIC | MEMF_CLEAR);
	if (block == NULL)
		return -ENOMEM;
	block->ctx = ctx;
	ctx->state = &block->state;

	ctx->dec_pool = AllocVec(HEVC_D_DEC_ENV_COUNT * sizeof(*ctx->dec_pool), MEMF_PUBLIC | MEMF_CLEAR);
	if (ctx->dec_pool == NULL)
		goto fail;
	ctx->dec_free = ctx->dec_pool;
	for (unsigned int i = 0; i != HEVC_D_DEC_ENV_COUNT - 1; ++i)
		ctx->dec_pool[i].next = ctx->dec_pool + i + 1;
	for (unsigned int i = 0; i != HEVC_D_DEC_ENV_COUNT; ++i)
	{
		struct hevc_d_dec_env *const de = ctx->dec_pool + i;
		de->ctx = ctx;
		de->decode_order = i;
		de->cmd_max = CMD_BUFFER_SIZE_INIT;
		de->state = HEVC_D_DECODE_END;
		if (hwbuf_alloc(dev, &de->cmd, de->cmd_max * sizeof(u64)))
			goto fail;
	}
	return 0;

fail:
	cmdgen_ctx_exit(ctx);
	return -ENOMEM;
}

/**
 * cmdgen_ctx_exit() - Free what cmdgen_ctx_init() made; nothing of a picture may be in the block.
 * @ctx: the context
 */
void cmdgen_ctx_exit(struct hevc_d_ctx *const ctx)
{
	struct ExecBase *SysBase = ctx->dev->SysBase;

	if (ctx->dec_pool != NULL)
	{
		for (unsigned int i = 0; i != HEVC_D_DEC_ENV_COUNT; ++i)
			hwbuf_free(ctx->dev, &ctx->dec_pool[i].cmd);
		FreeVec(ctx->dec_pool);
		ctx->dec_pool = NULL;
		ctx->dec_free = NULL;
	}
	if (ctx->state != NULL)
	{
		free_ps_info(ctx->state);
		FreeVec(CONTAINER_OF(ctx->state, struct cmdgen_state_block, state));
		ctx->state = NULL;
	}
}

/**
 * cmdgen_env_take() - An environment for a picture, or NULL while all are in flight.
 * @ctx: the context
 *
 * Return: the environment.
 */
struct hevc_d_dec_env *cmdgen_env_take(struct hevc_d_ctx *const ctx)
{
	struct hevc_d_dec_env *const de = ctx->dec_free;
	if (de != NULL)
	{
		ctx->dec_free = de->next;
		de->next = NULL;
		de->state = HEVC_D_DECODE_SLICE_START;
	}
	return de;
}

/**
 * cmdgen_env_give() - An environment back, its picture done with.
 * @ctx: the context
 * @de:  the environment
 */
void cmdgen_env_give(struct hevc_d_ctx *const ctx, struct hevc_d_dec_env *const de)
{
	de->state = HEVC_D_DECODE_END;
	de->frame_aux = NULL;
	de->col_aux = NULL;
	de->next = ctx->dec_free;
	ctx->dec_free = de;
}

/**
 * cmdgen_aux_alloc() - A frame's collocated-vector buffer, the context's picture size.
 * @ctx: the context
 *
 * Return: the buffer, NULL without memory.
 */
struct hevc_d_q_aux *cmdgen_aux_alloc(struct hevc_d_ctx *const ctx)
{
	struct ExecBase *SysBase = ctx->dev->SysBase;
	struct hevc_d_q_aux *const aq = AllocVec(sizeof(*aq), MEMF_PUBLIC | MEMF_CLEAR);
	if (aq == NULL)
		return NULL;
	if (hwbuf_alloc(ctx->dev, &aq->col, ctx->colmv_picsize))
	{
		FreeVec(aq);
		return NULL;
	}
	aq->refcount = 1;
	return aq;
}

/**
 * cmdgen_aux_free() - Free a frame's collocated-vector buffer.
 * @ctx: the context
 * @aq:  the buffer
 */
void cmdgen_aux_free(struct hevc_d_ctx *const ctx, struct hevc_d_q_aux *const aq)
{
	struct ExecBase *SysBase = ctx->dev->SysBase;
	hwbuf_free(ctx->dev, &aq->col);
	FreeVec(aq);
}

/* ---- a picture ----------------------------------------------------------------- */

/**
 * chroma_addr() - Where a frame's chroma is: after the luma rows of every column.
 * @ctx:  the session's context
 * @luma: the frame's address
 *
 * Return: the chroma's address.
 */
static dma_addr_t chroma_addr(const struct hevc_d_ctx *const ctx, const dma_addr_t luma)
{
	return luma + ctx->rows * CMDGEN_COLUMN_WIDTH;
}

/**
 * state_take_sets() - The picture's parameter sets into the state, the derived tables remade when they changed.
 * @s:   the context's state
 * @run: the picture's record
 *
 * Return: 0, or what updated_ps() refused the sets with (logged).
 */
static int state_take_sets(struct hevc_d_dec_state *const s, const struct hevc_d_run *const run)
{
	const bool sps_changed = memcmp(&s->sps, run->h265.sps, sizeof(s->sps)) != 0;
	if (sps_changed)
		memcpy(&s->sps, run->h265.sps, sizeof(s->sps));
	if (!sps_changed && memcmp(&s->pps, run->h265.pps, sizeof(s->pps)) == 0)
		return 0;
	memcpy(&s->pps, run->h265.pps, sizeof(s->pps));
	const int rv = updated_ps(s);
	if (rv)
		Kprintf("[rpivid] parameter sets refused (%ld)\n", (LONG)rv);
	return rv;
}

/**
 * picture_setup() - What a picture has before its slices: the geometry, the frame's addresses, the phase-2 values.
 * @ctx: the session's context
 * @de:  the picture's environment
 * @in:  the record and the frames
 *
 * Return: 0, or -1 for a picture larger than the session's frames (logged).
 */
static int picture_setup(struct hevc_d_ctx *const ctx, struct hevc_d_dec_env *const de, const struct cmdgen_input *const in)
{
	/* sh0 used where slice header contents should be constant over all slices, or first slice of frame */
	const struct v4l2_ctrl_hevc_slice_params *const sh0 = in->run.h265.slice_params;
	struct hevc_d_dec_state *const s = ctx->state;
	const unsigned int ctb_size_y =
	    1U << (s->sps.log2_min_luma_coding_block_size_minus3 + 3 + s->sps.log2_diff_max_min_luma_coding_block_size);
	unsigned int i;

	de->pic_width_in_ctbs_y = (s->sps.pic_width_in_luma_samples + ctb_size_y - 1) / ctb_size_y;   /* 7-15 */
	de->pic_height_in_ctbs_y = (s->sps.pic_height_in_luma_samples + ctb_size_y - 1) / ctb_size_y; /* 7-17 */
	de->cmd_len = 0;
	de->dpbno_col = ~0U;

	/* the single-plane column layout: the chroma rows after the luma rows of every column */
	de->luma_stride = ctx->column_bytes;
	de->frame_luma_addr = in->frame_luma;
	de->chroma_stride = de->luma_stride;
	de->frame_chroma_addr = chroma_addr(ctx, de->frame_luma_addr);
	de->frame_aux = NULL;
	de->col_aux = NULL;

	if (s->sps.pic_width_in_luma_samples > ctx->width || s->sps.pic_height_in_luma_samples > ctx->height)
	{
		Kprintf("[rpivid] SPS size (%ux%u) > the session's frames (%ux%u)\n", s->sps.pic_width_in_luma_samples,
		        s->sps.pic_height_in_luma_samples, ctx->width, ctx->height);
		return -1;
	}

	/* Fill in ref planes with our address s.t. if we mess up refs somehow then we still have a valid address entry */
	for (i = 0; i != 16; ++i)
	{
		de->ref_addrs[i][0] = de->frame_luma_addr;
		de->ref_addrs[i][1] = de->frame_chroma_addr;
	}

	/* Stash initial temporal_mvp flag. This must be the same for all pic slices (7.4.7.1) */
	s->slice_temporal_mvp = (sh0->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED) != 0;

	/* Need Aux ents for all (ref) DPB ents if temporal MV could be enabled for any pic */
	s->use_aux = ((s->sps.flags & V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED) != 0);
	s->mk_aux = s->use_aux && (s->sps.sps_max_sub_layers_minus1 >= sh0->nuh_temporal_id_plus1 || is_ref_unit_type(sh0->nal_unit_type));

	/* Phase 2 reg pre-calc */
	de->rpi_config2 = mk_config2(s);
	de->rpi_framesize = ((u32)s->sps.pic_height_in_luma_samples << 16) | s->sps.pic_width_in_luma_samples;
	de->rpi_currpoc = (u32)sh0->slice_pic_order_cnt;

	s->slice_idx = 0;
	s->dec = in->run.h265.dec;
	s->idx_inuse = 0;
	return 0;
}

/**
 * slice_window_valid() - Whether a slice's data lies within the picture and within its own NAL unit.
 * @s:  the context's state
 * @sh: the slice's parameters
 * @in: the slices' bytes
 * @i:  the slice
 *
 * Return: true; false with the cause logged.
 */
static bool slice_window_valid(const struct hevc_d_dec_state *const s, const struct v4l2_ctrl_hevc_slice_params *const sh,
                               const struct cmdgen_input *const in, const unsigned int i)
{
	const unsigned int bit_size = sh->bit_size - 8 * sh->data_byte_offset;
	const u32 byte_size = DIV_ROUND_UP_POW2(bit_size, 8);

	/* slice_segment_addr indexes ctb_addr_rs_to_ts so must be checked before use. */
	if (sh->slice_segment_addr >= s->ctb_size)
	{
		Kprintf("[rpivid] Slice %u: segment addr %u >= pic size in CTBs %u\n", i, sh->slice_segment_addr, s->ctb_size);
		return false;
	}
	if (sh->bit_size <= 8 * (u64)sh->data_byte_offset)
	{
		Kprintf("[rpivid] data_byte_offset %u * 8 >= bits %u\n", sh->data_byte_offset, sh->bit_size);
		return false;
	}
	if ((u64)sh->data_byte_offset + byte_size > in->slice_len[i])
	{
		Kprintf("[rpivid] data_byte_offset %u + bits %u (= %u bytes) > the slice's %u\n", sh->data_byte_offset, bit_size, byte_size,
		        in->slice_len[i]);
		return false;
	}
	return true;
}

/**
 * slice_commands() - One slice's commands: the body of upstream's per-slice loop (hevc_d_h265_setup()).
 * @de: the picture's environment
 * @s:  the context's state, the picture set up
 * @in: the record and the slices' bytes
 * @i:  the slice
 *
 * Each slice's window is counted from its own NAL unit.
 *
 * Return: 0, or not 0 with the picture to be dropped.
 */
static int slice_commands(struct hevc_d_dec_env *const de, struct hevc_d_dec_state *const s, const struct cmdgen_input *const in,
                          const unsigned int i)
{
	const struct hevc_d_run *const run = &in->run;
	const struct v4l2_ctrl_hevc_slice_params *const sh = run->h265.slice_params + i;
	const bool last_slice = i + 1 == run->h265.slice_ents;
	const unsigned int bit_size = sh->bit_size - 8 * sh->data_byte_offset;
	unsigned int prev_rs;
	unsigned int j;

	s->sh = sh;
	s->src_addr = in->bitstream + in->slice_base[i]; /* rpivid: this slice's NAL unit */

	if (!slice_window_valid(s, sh, in, i))
		return -1;
	/* BFNUM (data_len) includes the byte with rbsp_stop_one_bit which is not part of slice_segment_data but is all but
	 * certain to be in the input stream so add that when calculating the value we need, but limit to the actual size
	 * of the buffer (which may well be what is used to set bit_size if the caller isn't being very pedantic). */
	s->data_len = min(bit_size / 8 + 1, in->slice_len[i] - sh->data_byte_offset);

	s->slice_qp = 26 + s->pps.init_qp_minus26 + sh->slice_qp_delta;
	s->max_num_merge_cand = sh->slice_type == HEVC_SLICE_I ? 0 : (5 - sh->five_minus_max_num_merge_cand);
	s->dependent_slice_segment_flag = ((sh->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT) != 0);

	s->nb_refs[0] = (sh->slice_type == HEVC_SLICE_I) ? 0 : sh->num_ref_idx_l0_active_minus1 + 1;
	s->nb_refs[1] = (sh->slice_type != HEVC_SLICE_B) ? 0 : sh->num_ref_idx_l1_active_minus1 + 1;

	for (j = 0; j != s->nb_refs[0]; ++j)
		s->idx_inuse |= 1u << sh->ref_idx_l0[j];
	for (j = 0; j != s->nb_refs[1]; ++j)
		s->idx_inuse |= 1u << sh->ref_idx_l1[j];

	if (s->sps.flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED)
		populate_scaling_factors(run, de, s);
	else /* rpivid: flat factors, which new_slice_segment() writes for every stream (the factor RAM persists, hevc_d_h265.c) */
		memset(de->scaling_factors, FLAT_SCALING_FACTOR, sizeof(de->scaling_factors));

	/* Calc all the random coord info to avoid repeated conversion in/out */
	s->start_ts = (unsigned int)s->ctb_addr_rs_to_ts[sh->slice_segment_addr];
	s->start_ctb_x = sh->slice_segment_addr % de->pic_width_in_ctbs_y;
	s->start_ctb_y = sh->slice_segment_addr / de->pic_width_in_ctbs_y;
	/* Last CTB of previous slice */
	prev_rs = !s->start_ts ? 0 : (unsigned int)s->ctb_addr_ts_to_rs[s->start_ts - 1];
	s->prev_ctb_x = prev_rs % de->pic_width_in_ctbs_y;
	s->prev_ctb_y = prev_rs / de->pic_width_in_ctbs_y;

	if ((s->pps.flags & V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED))
		return wpp_decode_slice(de, s, last_slice);
	return decode_slice(de, s, last_slice);
}

/**
 * references_place() - The reference frames' addresses and the collocated-vector buffers, from the record's DPB array.
 * @ctx: the session's context
 * @de:  the picture's environment, its slices done
 * @in:  the frames
 *
 * Upstream's reference loop, the session's frames in place of the V4L2
 * queue.  A reference the slices use and the session has no frame for keeps
 * the picture's own address (picture_setup()) and is logged.
 *
 * Return: 0, or -1 when a buffer of collocated vectors the block will
 * touch is missing (logged): phase 2 may explode on bad data.
 */
static int references_place(struct hevc_d_ctx *const ctx, struct hevc_d_dec_env *const de, const struct cmdgen_input *const in)
{
	const struct v4l2_ctrl_hevc_decode_params *const dec = in->run.h265.dec;
	const struct hevc_d_dec_state *const s = ctx->state;

	for (unsigned int i = 0; i < dec->num_active_dpb_entries; ++i)
	{
		if (in->ref_luma[i] == 0)
		{
			if (s->idx_inuse & (1u << i))
				Kprintf("[rpivid] DPB entry %u in use but missing\n", i);
			continue;
		}
		de->ref_addrs[i][0] = in->ref_luma[i];
		de->ref_addrs[i][1] = chroma_addr(ctx, in->ref_luma[i]);
	}

	if (s->mk_aux)
	{
		de->frame_aux = in->frame_aux;
		if (de->frame_aux == NULL)
		{
			Kprintf("[rpivid] no aux storage for the frame\n");
			return -1;
		}
	}

	if (de->dpbno_col == ~0U)
		return 0;
	if (de->dpbno_col >= dec->num_active_dpb_entries)
	{
		Kprintf("[rpivid] Col ref index %u >= %u\n", de->dpbno_col, dec->num_active_dpb_entries);
		return 0;
	}
	/* Standard requires that the col pic is constant for the duration of the pic (7.4.7.1) */
	de->col_aux = in->ref_aux[de->dpbno_col];
	if (de->col_aux == NULL)
	{
		Kprintf("[rpivid] Missing DPB ent for col\n");
		return -1;
	}
	return 0;
}

/**
 * cmdgen_picture() - A picture's command list and phase-2 values, from its record.
 * @ctx: the session's context
 * @de:  the picture's environment (cmdgen_env_take())
 * @in:  the record, the slices' bytes, the frames
 *
 * What hevc_d_h265_setup() did without the V4L2 queues: the parameter sets
 * into the state when they changed, the geometry, the frame and reference
 * addresses, then upstream's per-slice loop.  The list and the slices are
 * cleaned for the block before this returns.
 *
 * Return: 0 with @de ready for phase 1, -1 with the picture to be dropped
 * (the reason logged).
 */
int cmdgen_picture(struct hevc_d_ctx *const ctx, struct hevc_d_dec_env *const de, const struct cmdgen_input *const in)
{
	struct hevc_d_dec_state *const s = ctx->state;

	s->sh = NULL; /* Avoid use until in the slice loop */

	if (state_take_sets(s, &in->run) || picture_setup(ctx, de, in))
		goto fail;
	for (unsigned int i = 0; i != in->run.h265.slice_ents; ++i)
	{
		if (slice_commands(de, s, in, i))
			goto fail;
		++s->slice_idx;
	}
	if (references_place(ctx, de, in))
		goto fail;

	/* the block reads these: out of the 68k's cache */
	cache_pre_dma(de->cmd.ptr, (ULONG)(de->cmd_len * sizeof(u64)), DMA_ReadFromRAM);
	cache_pre_dma((APTR)in->bitstream, in->bitstream_used, DMA_ReadFromRAM);

	de->state = HEVC_D_DECODE_PHASE1;
	return 0;

fail:
	de->state = HEVC_D_DECODE_ERROR_DONE;
	return -1;
}

/* ---- the phases ---------------------------------------------------------------- */

/**
 * cmdgen_phase1_regs() - Start phase 1: the scratch buffers, the command list.
 * @dev:   the block
 * @de:    the picture's environment, ready
 * @pu:    the slot's prediction-unit buffer
 * @coeff: the slot's coefficient buffer
 *
 * Upstream's phase1_claimed() register block; the last write starts the
 * block, so the interrupt is expected from then on.
 */
void cmdgen_phase1_regs(struct hevc_d_dev *const dev, struct hevc_d_dec_env *const de, const struct hevc_d_hwbuf *const pu,
                        const struct hevc_d_hwbuf *const coeff)
{
	de->pu_base_vc = pu->addr;
	de->pu_stride = (u32)ALIGN_DOWN(pu->size / de->pic_height_in_ctbs_y, 64);

	de->coeff_base_vc = coeff->addr;
	de->coeff_stride = (u32)ALIGN_DOWN(coeff->size / de->pic_height_in_ctbs_y, 64);

	apb_write_vc_addr(dev, RPI_PUWBASE, de->pu_base_vc);
	apb_write_vc_len(dev, RPI_PUWSTRIDE, de->pu_stride);
	apb_write_vc_addr(dev, RPI_COEFFWBASE, de->coeff_base_vc);
	apb_write_vc_len(dev, RPI_COEFFWSTRIDE, de->coeff_stride);

	/* Trigger command FIFO */
	apb_write(dev, RPI_CFNUM, de->cmd_len);

	/* Start the h/w */
	apb_write_vc_addr_final(dev, RPI_CFBASE, de->cmd.addr);
}

/**
 * cmdgen_phase1_result() - How phase 1 ended, once its interrupt came.
 * @dev: the block
 *
 * Return: 0 done; STATUS_PU_EXHAUSTED and/or STATUS_COEFF_EXHAUSTED for a
 * scratch buffer to grow and the phase to run again; -1 for an error.
 */
int cmdgen_phase1_result(const struct hevc_d_dev *const dev)
{
	return check_status(dev);
}

/**
 * cmdgen_phase2_regs() - Start phase 2: the frame, the references, the vectors.
 * @dev: the block
 * @de:  the picture's environment, phase 1 done
 *
 * Upstream's phase2_claimed() register block; the last write starts the block.
 */
void cmdgen_phase2_regs(struct hevc_d_dev *const dev, struct hevc_d_dec_env *const de)
{
	unsigned int i;

	apb_write_vc_addr(dev, RPI_PURBASE, de->pu_base_vc);
	apb_write_vc_len(dev, RPI_PURSTRIDE, de->pu_stride);
	apb_write_vc_addr(dev, RPI_COEFFRBASE, de->coeff_base_vc);
	apb_write_vc_len(dev, RPI_COEFFRSTRIDE, de->coeff_stride);

	apb_write_vc_addr(dev, RPI_OUTYBASE, de->frame_luma_addr);
	apb_write_vc_addr(dev, RPI_OUTCBASE, de->frame_chroma_addr);
	apb_write_vc_len(dev, RPI_OUTYSTRIDE, de->luma_stride);
	apb_write_vc_len(dev, RPI_OUTCSTRIDE, de->chroma_stride);

	for (i = 0; i < 16; i++)
	{
		/* Strides are in fact unused but fill in anyway */
		unsigned int roff = i * RPI_REFREGS_SIZE;

		apb_write_vc_addr(dev, RPI_REFYBASE0 + roff, de->ref_addrs[i][0]);
		apb_write_vc_len(dev, RPI_REFYSTRIDE0 + roff, de->luma_stride);
		apb_write_vc_addr(dev, RPI_REFCBASE0 + roff, de->ref_addrs[i][1]);
		apb_write_vc_len(dev, RPI_REFCSTRIDE0 + roff, de->chroma_stride);
	}

	apb_write(dev, RPI_CONFIG2, de->rpi_config2);
	apb_write(dev, RPI_FRAMESIZE, de->rpi_framesize);
	apb_write(dev, RPI_CURRPOC, de->rpi_currpoc);

	/* collocated reads/writes */
	apb_write_vc_len(dev, RPI_COLSTRIDE, de->ctx->colmv_stride);
	apb_write_vc_len(dev, RPI_MVSTRIDE, de->ctx->colmv_stride);
	apb_write_vc_addr(dev, RPI_MVBASE, !de->frame_aux ? 0 : de->frame_aux->col.addr);
	apb_write_vc_addr(dev, RPI_COLBASE, !de->col_aux ? 0 : de->col_aux->col.addr);

	apb_write_final(dev, RPI_NUMROWS, de->pic_height_in_ctbs_y);
}
