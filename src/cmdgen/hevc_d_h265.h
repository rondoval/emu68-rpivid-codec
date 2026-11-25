/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hevc_d_h265.h - the command generator's interface to the session and
 * hardware layers (docs/architecture.md section 7.3).  The kept functions are
 * static in hevc_d_h265.c; cmdgen.c, in the same translation unit, is what
 * the rest of the backend calls.  rpivid_private.h includes this, so every
 * file of the module sees it.
 *
 * The session makes a context per shape (session_objects.c), a
 * collocated-vector buffer per frame, and the block's buffers; it takes an
 * environment per picture in flight and fills a struct cmdgen_input from
 * the picture's record and frames for cmdgen_picture() (session_parse.c).
 * The decode task runs the two phases with the phase functions
 * (hw_task.c).
 */
#ifndef HEVC_D_H265_H
#define HEVC_D_H265_H

#include "hevc_d.h"
#include "hevc_d_video.h" /* the buffer sizes: the session's bitstream buffers, the hardware layer's slots */

struct hevc_d_q_aux; /* a frame's collocated-vector buffer: cmdgen_aux_alloc() */

#define CMDGEN_COLUMN_WIDTH 128u /* bytes across one column of a frame (docs/architecture.md section 5.3) */

/* What one picture's command list is built from */
struct cmdgen_input
{
	struct hevc_d_run run;          /* the record: the parameter sets, the slices, the decode parameters, the matrix */
	dma_addr_t bitstream;           /* the slices' NAL units, back to back, 64-byte aligned */
	u32 bitstream_used;             /* bytes of them */
	const u32 *slice_base;          /* each slice's NAL unit's offset in it, run.h265.slice_ents of them */
	const u32 *slice_len;           /* and its bytes */
	dma_addr_t frame_luma;          /* the frame the picture is written to; its chroma follows the rows */
	struct hevc_d_q_aux *frame_aux; /* the frame's collocated-vector buffer */
	dma_addr_t ref_luma[V4L2_HEVC_DPB_ENTRIES_NUM_MAX]; /* run.h265.dec->dpb[i]'s frame, 0 when missing */
	struct hevc_d_q_aux *ref_aux[V4L2_HEVC_DPB_ENTRIES_NUM_MAX]; /* their collocated-vector buffers, NULL when missing */
};

/* a session's context: the state, the environments, the geometry */
int cmdgen_ctx_init(struct hevc_d_ctx *ctx, struct hevc_d_dev *dev, u32 width, u32 height);
void cmdgen_ctx_exit(struct hevc_d_ctx *ctx);

/* an environment per picture in flight */
struct hevc_d_dec_env *cmdgen_env_take(struct hevc_d_ctx *ctx);
void cmdgen_env_give(struct hevc_d_ctx *ctx, struct hevc_d_dec_env *de);

/* a frame's collocated-vector buffer, sized for the context's pictures */
struct hevc_d_q_aux *cmdgen_aux_alloc(struct hevc_d_ctx *ctx);
void cmdgen_aux_free(struct hevc_d_ctx *ctx, struct hevc_d_q_aux *aux);

/* the picture: the command list and the phase-2 values into @de, cleaned for the block */
int cmdgen_picture(struct hevc_d_ctx *ctx, struct hevc_d_dec_env *de, const struct cmdgen_input *in);

/* the phases (the decode task): phase 1's result is 0 when it is done, else the STATUS bits
 * (STATUS_PU_EXHAUSTED, STATUS_COEFF_EXHAUSTED, hevc_d_hw.h) that ask for a bigger scratch buffer */
void cmdgen_phase1_regs(struct hevc_d_dev *dev, struct hevc_d_dec_env *de, const struct hevc_d_hwbuf *pu, const struct hevc_d_hwbuf *coeff);
int cmdgen_phase1_result(const struct hevc_d_dev *dev);
void cmdgen_phase2_regs(struct hevc_d_dev *dev, struct hevc_d_dec_env *de);

/* buffers the block touches, from the pool: whole cache lines, zeroed and cleaned;
 * hwbuf_realloc_copy() is in hevc_d.h, as the kept code calls it too */
int hwbuf_alloc(struct hevc_d_dev *dev, struct hevc_d_hwbuf *hwbuf, size_t size);
void hwbuf_free(struct hevc_d_dev *dev, struct hevc_d_hwbuf *hwbuf);
int hwbuf_realloc_new(struct hevc_d_dev *dev, struct hevc_d_hwbuf *hwbuf, size_t size);

#endif /* HEVC_D_H265_H */
