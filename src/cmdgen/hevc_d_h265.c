// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Raspberry Pi HEVC driver
 *
 * Copyright (C) 2025 Raspberry Pi Ltd
 *
 * Based on the Cedrus VPU driver, that is:
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 */

#include "linux_compat.h" /* rpivid: the kernel's types and macros, cmdgen.c has the rest */

#include "hevc_d.h"
#include "hevc_d_h265.h"
#include "hevc_d_hw.h"
#include "hevc_d_video.h"

/*
 * rpivid: one deviation inside a kept function, marked where it is:
 *   new_slice_segment() writes the scaling factors for every picture's first
 *   slice, not only when the SPS enables lists: the block's factor RAM keeps
 *   what the last stream loaded (measured, docs/hardware-facts.md), so a
 *   stream without lists is given flat ones by cmdgen_picture().
 * The kept text is upstream's, not written to this backend's -Wconversion rules; cmdgen.c is.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"

/* Maximum length of command buffer before we rate it an error */
#define CMD_BUFFER_SIZE_MAX 0x100000

/* Initial size of command FIFO in commands.
 * The FIFO will be extended if this value is exceeded but 8192 seems to
 * deal with all streams found in the wild.
 */
#define CMD_BUFFER_SIZE_INIT 8192

enum hevc_slice_type {
	HEVC_SLICE_B = 0,
	HEVC_SLICE_P = 1,
	HEVC_SLICE_I = 2,
};

enum hevc_layer { L0 = 0, L1 = 1 };

#define NUM_SCALING_FACTORS 4064 /* Not a typo = 0xbe0 + 0x400 */

#define AXI_BASE64 0

#define PROB_BACKUP ((20 << 12) + (20 << 6) + (0 << 0))
#define PROB_RELOAD ((20 << 12) + (20 << 0) + (0 << 6))

#define NUM_REF_IDX_ACTIVE_MAX 15

struct hevc_d_q_aux {
	unsigned int refcount;
	unsigned int q_index;
	struct hevc_d_q_aux *next;
	struct hevc_d_hwbuf col;
};

enum hevc_d_decode_state {
	HEVC_D_DECODE_SLICE_START,
	HEVC_D_DECODE_ERROR_DONE,
	HEVC_D_DECODE_PHASE1,
	HEVC_D_DECODE_END,
};

/*
 * Decode environment
 * One dec_env is allocated per active frame decode and holds state that
 * needs to persist between decode phases or callbacks
 */
struct hevc_d_dec_env {
	struct hevc_d_ctx *ctx;
	struct hevc_d_dec_env *next;

	enum hevc_d_decode_state state;
	unsigned int decode_order;
	int p1_status;		/* Phase 1 status - what to realloc */

	struct hevc_d_hwbuf cmd;
	unsigned int cmd_len;
	unsigned int cmd_max;
	unsigned int num_slice_msgs;
	unsigned int pic_width_in_ctbs_y;
	unsigned int pic_height_in_ctbs_y;
	unsigned int dpbno_col;
	u32 reg_slicestart;
	int collocated_from_l0_flag;
	/*
	 * Last CTB/Tile X,Y processed by (wpp_)entry_point
	 * Could be in _state as P0 only but needs updating where _state
	 * is const
	 */
	unsigned int entry_ctb_x;
	unsigned int entry_ctb_y;
	unsigned int entry_tile_x;
	unsigned int entry_tile_y;
	int entry_qp;
	u32 entry_slice;

	u32 rpi_config2;
	u32 rpi_framesize;
	u32 rpi_currpoc;

	struct vb2_v4l2_buffer *frame_buf;
	struct vb2_v4l2_buffer *src_buf;
	dma_addr_t frame_luma_addr;
	unsigned int luma_stride;
	dma_addr_t frame_chroma_addr;
	unsigned int chroma_stride;
	dma_addr_t ref_addrs[16][2];
	struct hevc_d_q_aux *frame_aux;
	struct hevc_d_q_aux *col_aux;

	dma_addr_t pu_base_vc;
	dma_addr_t coeff_base_vc;
	u32 pu_stride;
	u32 coeff_stride;

#define SLICE_MSGS_MAX (2 * NUM_REF_IDX_ACTIVE_MAX * 8 + 3)
	u16 slice_msgs[SLICE_MSGS_MAX];
	u8 scaling_factors[NUM_SCALING_FACTORS];

	struct media_request *request;
	struct hevc_d_hw_irq_ent irq_ent;
};

/*
 * Decode state
 * Decode information that persists between frame decodes but is only
 * used or changed in phase 0 (setup)
 */
struct hevc_d_dec_state {
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_pps pps;

	/* Helper vars & tables derived from sps/pps */
	unsigned int log2_ctb_size;     /* log2 width of a CTB */
	unsigned int ctb_width;         /* Width in CTBs */
	unsigned int ctb_height;        /* Height in CTBs */
	unsigned int ctb_size;          /* Pic area in CTBs */
	unsigned int tile_width;        /* Width in tiles */
	unsigned int tile_height;       /* Height in tiles */

	unsigned int *col_bd;
	unsigned int *row_bd;
	int *ctb_addr_rs_to_ts;
	int *ctb_addr_ts_to_rs;

	/* Aux storage for DPB */
	struct hevc_d_q_aux *ref_aux[V4L2_HEVC_DPB_ENTRIES_NUM_MAX];
	struct hevc_d_q_aux *frame_aux;

	/* Slice vars */
	unsigned int slice_idx;
	unsigned int idx_inuse;
	bool slice_temporal_mvp;  /* Slice flag but constant for frame */
	bool use_aux;
	bool mk_aux;

	/* Temp vars per run - don't actually need to persist */
	dma_addr_t src_addr;
	const struct v4l2_ctrl_hevc_slice_params *sh;
	const struct v4l2_ctrl_hevc_decode_params *dec;
	unsigned int nb_refs[2];
	int slice_qp;
	unsigned int max_num_merge_cand; /* 0 if I-slice */
	bool dependent_slice_segment_flag;
	u32 data_len;

	unsigned int start_ts;          /* slice_segment_addr -> ts */
	unsigned int start_ctb_x;       /* CTB X,Y of start_ts */
	unsigned int start_ctb_y;
	unsigned int prev_ctb_x;        /* CTB X,Y of start_ts - 1 */
	unsigned int prev_ctb_y;
};

/* Phase 1 command and bit FIFOs */
static int cmds_check_space(struct hevc_d_dec_env *const de, unsigned int n)
{
	unsigned int newmax;

	if (de->cmd_len + n <= de->cmd_max)
		return 0;

	newmax = roundup_pow_of_two(de->cmd_len + n);
	if (newmax > CMD_BUFFER_SIZE_MAX) {
		v4l2_err(&de->ctx->dev->v4l2_dev,
			 "%s: n %u implausible\n", __func__, newmax);
		return -ENOMEM;
	}

	if (hwbuf_realloc_copy(de->ctx->dev, &de->cmd, newmax * sizeof(u64))) {
		v4l2_err(&de->ctx->dev->v4l2_dev,
			 "Failed cmd buffer realloc from %u to %u\n",
			 de->cmd_max, newmax);
		return -ENOMEM;
	}
	hevc_d_dbg(1, &de->ctx->dev->v4l2_dev,
		   "cmd buffer realloc from %u to %u\n", de->cmd_max, newmax);

	de->cmd_max = newmax;
	return 0;
}

static void p1_apb_write(struct hevc_d_dec_env *const de, const u16 addr,
			 const u32 data)
{
	WRITE_ONCE(((u64 *)de->cmd.ptr)[de->cmd_len], addr | ((u64)data << 32));
	de->cmd_len++;
}

static int ctb_to_tile(unsigned int ctb, unsigned int *bd)
{
	int i;

	for (i = 1; ctb >= bd[i]; i++)
		; /* bd[] has num+1 elements; bd[0]=0; */

	return i - 1;
}

static unsigned int ctb_to_tile_x(const struct hevc_d_dec_state *const s,
				  const unsigned int ctb_x)
{
	return ctb_to_tile(ctb_x, s->col_bd);
}

static unsigned int ctb_to_tile_y(const struct hevc_d_dec_state *const s,
				  const unsigned int ctb_y)
{
	return ctb_to_tile(ctb_y, s->row_bd);
}

/*
 * Initialisation process for context variables (CABAC init)
 * see H.265 9.3.2.2
 *
 * N.B. If comparing with FFmpeg note that this h/w uses slightly different
 * offsets to FFmpegs array
 */

/* Actual number of values */
#define RPI_PROB_VALS 154U
/* Rounded up as we copy words */
#define RPI_PROB_ARRAY_SIZE ((154 + 3) & ~3)

/* Initialiser values - see tables H.265 9-4 through 9-42 */
static const u8 prob_init[3][156] = {
	{
		153, 200, 139, 141, 157, 154, 154, 154, 154, 154, 184, 154, 154,
		154, 184, 63,  154, 154, 154, 154, 154, 154, 154, 154, 154, 154,
		154, 154, 154, 153, 138, 138, 111, 141, 94,  138, 182, 154, 154,
		154, 140, 92,  137, 138, 140, 152, 138, 139, 153, 74,  149, 92,
		139, 107, 122, 152, 140, 179, 166, 182, 140, 227, 122, 197, 110,
		110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111,
		79,  108, 123, 63,  110, 110, 124, 125, 140, 153, 125, 127, 140,
		109, 111, 143, 127, 111, 79,  108, 123, 63,  91,  171, 134, 141,
		138, 153, 136, 167, 152, 152, 139, 139, 111, 111, 125, 110, 110,
		94,  124, 108, 124, 107, 125, 141, 179, 153, 125, 107, 125, 141,
		179, 153, 125, 107, 125, 141, 179, 153, 125, 140, 139, 182, 182,
		152, 136, 152, 136, 153, 136, 139, 111, 136, 139, 111, 0,   0,
	},
	{
		153, 185, 107, 139, 126, 197, 185, 201, 154, 149, 154, 139, 154,
		154, 154, 152, 110, 122, 95,  79,  63,  31,  31,  153, 153, 168,
		140, 198, 79,  124, 138, 94,  153, 111, 149, 107, 167, 154, 154,
		154, 154, 196, 196, 167, 154, 152, 167, 182, 182, 134, 149, 136,
		153, 121, 136, 137, 169, 194, 166, 167, 154, 167, 137, 182, 125,
		110, 94,  110, 95,  79,  125, 111, 110, 78,  110, 111, 111, 95,
		94,  108, 123, 108, 125, 110, 94,  110, 95,  79,  125, 111, 110,
		78,  110, 111, 111, 95,  94,  108, 123, 108, 121, 140, 61,  154,
		107, 167, 91,  122, 107, 167, 139, 139, 155, 154, 139, 153, 139,
		123, 123, 63,  153, 166, 183, 140, 136, 153, 154, 166, 183, 140,
		136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 123, 123,
		107, 121, 107, 121, 167, 151, 183, 140, 151, 183, 140, 0,   0,
	},
	{
		153, 160, 107, 139, 126, 197, 185, 201, 154, 134, 154, 139, 154,
		154, 183, 152, 154, 137, 95,  79,  63,  31,  31,  153, 153, 168,
		169, 198, 79,  224, 167, 122, 153, 111, 149, 92,  167, 154, 154,
		154, 154, 196, 167, 167, 154, 152, 167, 182, 182, 134, 149, 136,
		153, 121, 136, 122, 169, 208, 166, 167, 154, 152, 167, 182, 125,
		110, 124, 110, 95,  94,  125, 111, 111, 79,  125, 126, 111, 111,
		79,  108, 123, 93,  125, 110, 124, 110, 95,  94,  125, 111, 111,
		79,  125, 126, 111, 111, 79,  108, 123, 93,  121, 140, 61,  154,
		107, 167, 91,  107, 107, 167, 139, 139, 170, 154, 139, 153, 139,
		123, 123, 63,  124, 166, 183, 140, 136, 153, 154, 166, 183, 140,
		136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 138, 138,
		122, 121, 122, 121, 167, 151, 183, 140, 151, 183, 140, 0,   0,
	},
};

#define CMDS_WRITE_PROB ((RPI_PROB_ARRAY_SIZE / 4) + 1)

static void write_prob(struct hevc_d_dec_env *const de,
		       const struct hevc_d_dec_state *const s)
{
	const unsigned int init_type =
		s->sh->slice_type == HEVC_SLICE_I ? 0 :
		(s->sh->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT) ?
		 (s->sh->slice_type == HEVC_SLICE_P ? 2 : 1) :
		 (s->sh->slice_type == HEVC_SLICE_P ? 1 : 2);
	const int q = clamp(s->slice_qp, 0, 51);
	const u8 *p = prob_init[init_type];
	u8 dst[RPI_PROB_ARRAY_SIZE];
	unsigned int i;

	for (i = 0; i < RPI_PROB_VALS; i++) {
		int init_value = p[i];
		int m = (init_value >> 4) * 5 - 45;
		int n = ((init_value & 15) << 3) - 16;
		int pre = 2 * (((m * q) >> 4) + n) - 127;

		pre ^= pre >> 31;
		if (pre > 124)
			pre = 124 + (pre & 1);
		dst[i] = pre;
	}
	for (i = RPI_PROB_VALS; i != RPI_PROB_ARRAY_SIZE; ++i)
		dst[i] = 0;

	for (i = 0; i < RPI_PROB_ARRAY_SIZE; i += 4)
		p1_apb_write(de, 0x1000 + i,
			     dst[i] + (dst[i + 1] << 8) + (dst[i + 2] << 16) +
				     (dst[i + 3] << 24));

	/*
	 * Having written the prob array back it up
	 * This is not always needed but is a small overhead that simplifies
	 * (and speeds up) some multi-tile & WPP scenarios
	 * There are no scenarios where having written a prob we ever want
	 * a previous (non-initial) state back
	 */
	p1_apb_write(de, RPI_TRANSFER, PROB_BACKUP);
}

#define CMDS_WRITE_SCALING_FACTORS (NUM_SCALING_FACTORS / 4)
static void write_scaling_factors(struct hevc_d_dec_env *const de)
{
	const u8 *p = (u8 *)de->scaling_factors;
	int i;

	for (i = 0; i < NUM_SCALING_FACTORS; i += 4, p += 4)
		p1_apb_write(de, 0x2000 + i,
			     p[0] + (p[1] << 8) + (p[2] << 16) + (p[3] << 24));
}

static inline __u32 dma_to_axi_addr(dma_addr_t a)
{
	return (__u32)(a >> 6);
}

#define CMDS_WRITE_BITSTREAM 4
static void write_bitstream(struct hevc_d_dec_env *const de,
			   const struct hevc_d_dec_state *const s)
{
	/* V4L2 always has emulation prevention bytes in the stream */
	const int rpi_use_emu = 1;
	const unsigned int len = s->data_len;
	const dma_addr_t addr = s->src_addr + s->sh->data_byte_offset;
	const unsigned int offset = addr & 63;

	p1_apb_write(de, RPI_BFBASE, dma_to_axi_addr(addr));
	p1_apb_write(de, RPI_BFNUM, len);
	p1_apb_write(de, RPI_BFCONTROL, offset + (1 << 7)); /* Stop */
	p1_apb_write(de, RPI_BFCONTROL, offset + (rpi_use_emu << 6));
}

/*
 * The slice constant part of the slice register - width and height need to
 * be ORed in later as they are per-tile / WPP-row
 */
static u32 slice_reg_const(const struct hevc_d_dec_state *const s)
{
	u32 x = (s->max_num_merge_cand << 0) |
		(s->nb_refs[L0] << 4) |
		(s->nb_refs[L1] << 8) |
		(s->sh->slice_type << 12);

	if (s->sh->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA)
		x |= BIT(14);
	if (s->sh->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA)
		x |= BIT(15);
	if (s->sh->slice_type == HEVC_SLICE_B &&
	    (s->sh->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO))
		x |= BIT(16);

	return x;
}

#define CMDS_NEW_SLICE_SEGMENT (4 + CMDS_WRITE_SCALING_FACTORS)

static void new_slice_segment(struct hevc_d_dec_env *const de,
			      const struct hevc_d_dec_state *const s)
{
	const struct v4l2_ctrl_hevc_sps *const sps = &s->sps;
	const struct v4l2_ctrl_hevc_pps *const pps = &s->pps;

	p1_apb_write(de,
		     RPI_SPS0,
		     ((sps->log2_min_luma_coding_block_size_minus3 + 3) << 0) |
		     (s->log2_ctb_size << 4) |
		     ((sps->log2_min_luma_transform_block_size_minus2 + 2)
							<< 8) |
		     ((sps->log2_min_luma_transform_block_size_minus2 + 2 +
		       sps->log2_diff_max_min_luma_transform_block_size)
						<< 12) |
		     ((sps->bit_depth_luma_minus8 + 8) << 16) |
		     ((sps->bit_depth_chroma_minus8 + 8) << 20) |
		     (sps->max_transform_hierarchy_depth_intra << 24) |
		     (sps->max_transform_hierarchy_depth_inter << 28));

	p1_apb_write(de,
		     RPI_SPS1,
		     ((sps->pcm_sample_bit_depth_luma_minus1 + 1) << 0) |
		     ((sps->pcm_sample_bit_depth_chroma_minus1 + 1) << 4) |
		     ((sps->log2_min_pcm_luma_coding_block_size_minus3 + 3)
						<< 8) |
		     ((sps->log2_min_pcm_luma_coding_block_size_minus3 + 3 +
		       sps->log2_diff_max_min_pcm_luma_coding_block_size)
						<< 12) |
		     (((sps->flags & V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE) ?
				0 : sps->chroma_format_idc) << 16) |
		     ((!!(sps->flags & V4L2_HEVC_SPS_FLAG_AMP_ENABLED)) << 18) |
		     ((!!(sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED)) << 19) |
		     ((!!(sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED))
						<< 20) |
		     ((!!(sps->flags &
			   V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED))
						<< 21));

	p1_apb_write(de,
		     RPI_PPS,
		     ((s->log2_ctb_size - pps->diff_cu_qp_delta_depth) << 0) |
		     ((!!(pps->flags & V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED))
						 << 4) |
		     ((!!(pps->flags &
				V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED))
						 << 5) |
		     ((!!(pps->flags & V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED))
						 << 6) |
		     ((!!(pps->flags &
				V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED))
						<< 7) |
		     (((pps->pps_cb_qp_offset + s->sh->slice_cb_qp_offset) & 255)
						<< 8) |
		     (((pps->pps_cr_qp_offset + s->sh->slice_cr_qp_offset) & 255)
						<< 16) |
		     ((!!(pps->flags &
				V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED))
						<< 24));

	if (!s->start_ts) /* rpivid: always: the factor RAM persists across streams, so a stream without lists gets flat ones (cmdgen.c) */
		write_scaling_factors(de);

	if (!s->dependent_slice_segment_flag) {
		int ctb_col = s->sh->slice_segment_addr %
							de->pic_width_in_ctbs_y;
		int ctb_row = s->sh->slice_segment_addr /
							de->pic_width_in_ctbs_y;

		de->reg_slicestart = (ctb_col << 0) + (ctb_row << 16);
	}

	p1_apb_write(de, RPI_SLICESTART, de->reg_slicestart);
}

/* Slice messages */

static void msg_slice(struct hevc_d_dec_env *const de, const u16 msg)
{
	de->slice_msgs[de->num_slice_msgs++] = msg;
}

#define CMDS_PROGRAM_SLICECMDS (1 + SLICE_MSGS_MAX)
static void program_slicecmds(struct hevc_d_dec_env *const de,
			      const int sliceid)
{
	int i;

	p1_apb_write(de, RPI_SLICECMDS, de->num_slice_msgs + (sliceid << 8));

	for (i = 0; i < de->num_slice_msgs; i++)
		p1_apb_write(de, 0x4000 + 4 * i, de->slice_msgs[i] & 0xffff);
}

/* NoBackwardPredictionFlag 8.3.5 - Simply checks POCs of the frames referenced
 * by the idx array against cur_poc. Needs to be called twice (with L0 & L1) to
 * get NoBackwardPredictionFlag.
 */
static int has_backward(const struct v4l2_hevc_dpb_entry *const dpb,
			const __u8 *const idx, const unsigned int n,
			const s32 cur_poc)
{
	unsigned int i;

	for (i = 0; i < n; ++i) {
		if (cur_poc < dpb[idx[i]].pic_order_cnt_val)
			return 0;
	}
	return 1;
}

static void pre_slice_weights(struct hevc_d_dec_env *const de,
			      const struct v4l2_hevc_pred_weight_table *const w,
			      const __s8 delta_luma_weight[V4L2_HEVC_DPB_ENTRIES_NUM_MAX],
			      const __s8 luma_offset[V4L2_HEVC_DPB_ENTRIES_NUM_MAX],
			      const __s8 delta_chroma_weight[V4L2_HEVC_DPB_ENTRIES_NUM_MAX][2],
			      const __s8 chroma_offset[V4L2_HEVC_DPB_ENTRIES_NUM_MAX][2],
			      const unsigned int idx)
{
	const int luma_log2_weight_denom = min(w->luma_log2_weight_denom, 7);
	const int chroma_log2_weight_denom = clamp(luma_log2_weight_denom +
						   w->delta_chroma_log2_weight_denom,
						   0, 7);
	const int luma_weight_denom = (1 << luma_log2_weight_denom);
	const int chroma_weight_denom = (1 << chroma_log2_weight_denom);

	msg_slice(de, luma_log2_weight_denom |
			(((delta_luma_weight[idx] + luma_weight_denom) & 0x1ff) << 3));
	msg_slice(de, luma_offset[idx] & 0xff);
	msg_slice(de, chroma_log2_weight_denom |
			(((delta_chroma_weight[idx][0] + chroma_weight_denom) & 0x1ff) << 3));
	msg_slice(de, chroma_offset[idx][0] & 0xff);
	msg_slice(de, chroma_log2_weight_denom |
			(((delta_chroma_weight[idx][1] + chroma_weight_denom) & 0x1ff) << 3));
	msg_slice(de, chroma_offset[idx][1] & 0xff);
}

static void pre_slice_decode(struct hevc_d_dec_env *const de,
			     const struct hevc_d_dec_state *const s)
{
	const struct v4l2_ctrl_hevc_slice_params *const sh = s->sh;
	const struct v4l2_ctrl_hevc_decode_params *const dec = s->dec;
	int weighted_pred_flag, idx;
	u16 cmd_slice;
	unsigned int collocated_from_l0_flag;

	de->num_slice_msgs = 0;

	cmd_slice = 0;
	if (sh->slice_type == HEVC_SLICE_I)
		cmd_slice = 1;
	if (sh->slice_type == HEVC_SLICE_P)
		cmd_slice = 2;
	if (sh->slice_type == HEVC_SLICE_B)
		cmd_slice = 3;

	cmd_slice |= (s->nb_refs[L0] << 2) | (s->nb_refs[L1] << 6) |
		     (s->max_num_merge_cand << 11);

	collocated_from_l0_flag =
		!s->slice_temporal_mvp ||
		sh->slice_type != HEVC_SLICE_B ||
		(sh->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0);
	cmd_slice |= collocated_from_l0_flag << 14;

	if (sh->slice_type == HEVC_SLICE_P || sh->slice_type == HEVC_SLICE_B) {
		/* Flag to say all reference pictures are from the past */
		const int no_backward_pred_flag =
			has_backward(dec->dpb, sh->ref_idx_l0, s->nb_refs[L0],
				     sh->slice_pic_order_cnt) &&
			has_backward(dec->dpb, sh->ref_idx_l1, s->nb_refs[L1],
				     sh->slice_pic_order_cnt);
		cmd_slice |= no_backward_pred_flag << 10;
		msg_slice(de, cmd_slice);

		if (s->slice_temporal_mvp)
			de->dpbno_col = collocated_from_l0_flag ?
				(sh->collocated_ref_idx < s->nb_refs[L0] ?
				 sh->ref_idx_l0[sh->collocated_ref_idx] :
				 sh->ref_idx_l0[0]) :
				(sh->collocated_ref_idx < s->nb_refs[L1] ?
				 sh->ref_idx_l1[sh->collocated_ref_idx] :
				 sh->ref_idx_l1[0]);

		/* Write reference picture descriptions */
		weighted_pred_flag =
			sh->slice_type == HEVC_SLICE_P ?
				!!(s->pps.flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED) :
				!!(s->pps.flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED);

		for (idx = 0; idx < s->nb_refs[L0]; ++idx) {
			unsigned int dpb_no = sh->ref_idx_l0[idx];

			msg_slice(de,
				  dpb_no |
				  ((dec->dpb[dpb_no].flags &
					V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE) ?
						 (1 << 4) : 0) |
				  (weighted_pred_flag ? (3 << 5) : 0));
			msg_slice(de, dec->dpb[dpb_no].pic_order_cnt_val & 0xffff);

			if (weighted_pred_flag)
				pre_slice_weights(de,
						  &sh->pred_weight_table,
						  sh->pred_weight_table.delta_luma_weight_l0,
						  sh->pred_weight_table.luma_offset_l0,
						  sh->pred_weight_table.delta_chroma_weight_l0,
						  sh->pred_weight_table.chroma_offset_l0,
						  idx);
		}

		for (idx = 0; idx < s->nb_refs[L1]; ++idx) {
			unsigned int dpb_no = sh->ref_idx_l1[idx];

			msg_slice(de,
				  dpb_no |
				  ((dec->dpb[dpb_no].flags &
					 V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE) ?
						 (1 << 4) : 0) |
					(weighted_pred_flag ? (3 << 5) : 0));
			msg_slice(de, dec->dpb[dpb_no].pic_order_cnt_val & 0xffff);

			if (weighted_pred_flag)
				pre_slice_weights(de,
						  &sh->pred_weight_table,
						  sh->pred_weight_table.delta_luma_weight_l1,
						  sh->pred_weight_table.luma_offset_l1,
						  sh->pred_weight_table.delta_chroma_weight_l1,
						  sh->pred_weight_table.chroma_offset_l1,
						  idx);
		}
	} else {
		msg_slice(de, cmd_slice);
	}

	msg_slice(de,
		  (sh->slice_beta_offset_div2 & 15) |
		  ((sh->slice_tc_offset_div2 & 15) << 4) |
		  ((sh->flags &
		    V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED) ?
						1 << 8 : 0) |
		  ((sh->flags &
			  V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED) ?
						1 << 9 : 0) |
		  ((s->pps.flags &
			  V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED) ?
						1 << 10 : 0));

	msg_slice(de, ((sh->slice_cr_qp_offset & 31) << 5) +
		       (sh->slice_cb_qp_offset & 31)); /* CMD_QPOFF */
}

#define CMDS_WRITE_SLICE 1
static void write_slice(struct hevc_d_dec_env *const de,
			const struct hevc_d_dec_state *const s,
			const u32 slice_const,
			const unsigned int ctb_col,
			const unsigned int ctb_row)
{
	const unsigned int cs = (1 << s->log2_ctb_size);
	const unsigned int w_last = s->sps.pic_width_in_luma_samples & (cs - 1);
	const unsigned int h_last = s->sps.pic_height_in_luma_samples & (cs - 1);

	p1_apb_write(de, RPI_SLICE,
		     slice_const |
		     ((ctb_col + 1 < s->ctb_width || !w_last ?
				cs : w_last) << 17) |
		     ((ctb_row + 1 < s->ctb_height || !h_last ?
				cs : h_last) << 24));
}

#define PAUSE_MODE_WPP  1
#define PAUSE_MODE_TILE 0xffff

/*
 * N.B. This can be called to fill in data from the previous slice so must not
 * use any state data that may change from slice to slice (e.g. qp)
 */
#define CMDS_NEW_ENTRY_POINT (6 + CMDS_WRITE_SLICE)

static void new_entry_point(struct hevc_d_dec_env *const de,
			    const struct hevc_d_dec_state *const s,
			    const bool do_bte,
			    const bool reset_qp_y,
			    const u32 pause_mode,
			    const unsigned int tile_x,
			    const unsigned int tile_y,
			    const unsigned int ctb_col,
			    const unsigned int ctb_row,
			    const int slice_qp,
			    const u32 slice_const)
{
	const unsigned int endx = s->col_bd[tile_x + 1] - 1;
	const unsigned int endy = (pause_mode == PAUSE_MODE_WPP) ?
		ctb_row : s->row_bd[tile_y + 1] - 1;

	p1_apb_write(de, RPI_TILESTART,
		     s->col_bd[tile_x] | (s->row_bd[tile_y] << 16));
	p1_apb_write(de, RPI_TILEEND, endx | (endy << 16));

	if (do_bte)
		p1_apb_write(de, RPI_BEGINTILEEND, endx | (endy << 16));

	write_slice(de, s, slice_const, endx, endy);

	if (reset_qp_y) {
		int sps_qp_bd_offset =
			6 * s->sps.bit_depth_luma_minus8;

		p1_apb_write(de, RPI_QP, clamp(sps_qp_bd_offset + slice_qp,
					       0, sps_qp_bd_offset + 51));
	}

	p1_apb_write(de, RPI_MODE,
		     pause_mode |
			((endx == s->ctb_width - 1) << 17) |
			((endy == s->ctb_height - 1) << 18));

	p1_apb_write(de, RPI_CONTROL, (ctb_col << 0) | (ctb_row << 16));

	de->entry_tile_x = tile_x;
	de->entry_tile_y = tile_y;
	de->entry_ctb_x = ctb_col;
	de->entry_ctb_y = ctb_row;
	de->entry_qp = slice_qp;
	de->entry_slice = slice_const;
}

/* Wavefront mode */

#define CMDS_WPP_PAUSE 4
static void wpp_pause(struct hevc_d_dec_env *const de, int ctb_row)
{
	p1_apb_write(de, RPI_STATUS, (ctb_row << 18) | 0x25);
	p1_apb_write(de, RPI_TRANSFER, PROB_BACKUP);
	p1_apb_write(de, RPI_MODE,
		     ctb_row == de->pic_height_in_ctbs_y - 1 ?
							0x70000 : 0x30000);
	p1_apb_write(de, RPI_CONTROL, (ctb_row << 16) + 2);
}

#define CMDS_WPP_ENTRY_FILL_1 (CMDS_WPP_PAUSE + 2 + CMDS_NEW_ENTRY_POINT)
static int wpp_entry_fill(struct hevc_d_dec_env *const de,
			  const struct hevc_d_dec_state *const s,
			  const unsigned int last_y)
{
	int rv;
	const unsigned int last_x = s->ctb_width - 1;

	rv = cmds_check_space(de, CMDS_WPP_ENTRY_FILL_1 *
				  (last_y - de->entry_ctb_y));
	if (rv)
		return rv;

	while (de->entry_ctb_y < last_y) {
		/* wpp_entry_x/y set by wpp_entry_point */
		if (s->ctb_width > 2)
			wpp_pause(de, de->entry_ctb_y);
		p1_apb_write(de, RPI_STATUS,
			     (de->entry_ctb_y << 18) | (last_x << 5) | 2);

		/* if width == 1 then the saved state is the init one */
		if (s->ctb_width == 2)
			p1_apb_write(de, RPI_TRANSFER, PROB_BACKUP);
		else
			p1_apb_write(de, RPI_TRANSFER, PROB_RELOAD);

		new_entry_point(de, s, false, true, PAUSE_MODE_WPP,
				0, 0, 0, de->entry_ctb_y + 1,
				de->entry_qp, de->entry_slice);
	}
	return 0;
}

static int wpp_end_previous_slice(struct hevc_d_dec_env *const de,
				  const struct hevc_d_dec_state *const s)
{
	int rv;

	rv = wpp_entry_fill(de, s, s->prev_ctb_y);
	if (rv)
		return rv;

	rv = cmds_check_space(de, CMDS_WPP_PAUSE + 2);
	if (rv)
		return rv;

	if (de->entry_ctb_x < 2 &&
	    (de->entry_ctb_y < s->start_ctb_y || s->start_ctb_x > 2) &&
	    s->ctb_width > 2)
		wpp_pause(de, s->prev_ctb_y);
	p1_apb_write(de, RPI_STATUS,
		     1 | (s->prev_ctb_x << 5) | (s->prev_ctb_y << 18));
	if (s->start_ctb_x == 2 ||
	    (s->ctb_width == 2 && de->entry_ctb_y < s->start_ctb_y))
		p1_apb_write(de, RPI_TRANSFER, PROB_BACKUP);
	return 0;
}

/*
 * Only main profile supported so WPP => !Tiles which makes some of the
 * next chunk code simpler
 */
static int wpp_decode_slice(struct hevc_d_dec_env *const de,
			    const struct hevc_d_dec_state *const s,
			    bool last_slice)
{
	bool reset_qp_y = true;
	const bool indep = !s->dependent_slice_segment_flag;
	int rv;

	if (s->start_ts) {
		rv = wpp_end_previous_slice(de, s);
		if (rv)
			return rv;
	}
	pre_slice_decode(de, s);

	rv = cmds_check_space(de,
			      CMDS_WRITE_BITSTREAM +
				CMDS_WRITE_PROB +
				CMDS_PROGRAM_SLICECMDS +
				CMDS_NEW_SLICE_SEGMENT +
				CMDS_NEW_ENTRY_POINT);
	if (rv)
		return rv;

	write_bitstream(de, s);

	if (!s->start_ts || indep || s->ctb_width == 1)
		write_prob(de, s);
	else if (!s->start_ctb_x)
		p1_apb_write(de, RPI_TRANSFER, PROB_RELOAD);
	else
		reset_qp_y = false;

	program_slicecmds(de, s->slice_idx);
	new_slice_segment(de, s);
	new_entry_point(de, s, indep, reset_qp_y, PAUSE_MODE_WPP,
			0, 0, s->start_ctb_x, s->start_ctb_y,
			s->slice_qp, slice_reg_const(s));

	if (last_slice) {
		rv = wpp_entry_fill(de, s, s->ctb_height - 1);
		if (rv)
			return rv;

		rv = cmds_check_space(de, CMDS_WPP_PAUSE + 1);
		if (rv)
			return rv;

		if (de->entry_ctb_x < 2 && s->ctb_width > 2)
			wpp_pause(de, s->ctb_height - 1);

		p1_apb_write(de, RPI_STATUS,
			     1 | ((s->ctb_width - 1) << 5) |
				((s->ctb_height - 1) << 18));
	}
	return 0;
}

/* Tiles mode */

/* Guarantees 1 cmd entry free on exit */
static int tile_entry_fill(struct hevc_d_dec_env *const de,
			   const struct hevc_d_dec_state *const s,
			   const unsigned int last_tile_x,
			   const unsigned int last_tile_y)
{
	while (de->entry_tile_y < last_tile_y ||
	       (de->entry_tile_y == last_tile_y &&
		de->entry_tile_x < last_tile_x)) {
		int rv;
		unsigned int t_x = de->entry_tile_x;
		unsigned int t_y = de->entry_tile_y;
		const unsigned int last_x = s->col_bd[t_x + 1] - 1;
		const unsigned int last_y = s->row_bd[t_y + 1] - 1;

		/* One more than needed here */
		rv = cmds_check_space(de, CMDS_NEW_ENTRY_POINT + 3);
		if (rv)
			return rv;

		p1_apb_write(de, RPI_STATUS,
			     2 | (last_x << 5) | (last_y << 18));
		p1_apb_write(de, RPI_TRANSFER, PROB_RELOAD);

		/* Inc tile */
		if (++t_x >= s->tile_width) {
			t_x = 0;
			++t_y;
		}

		new_entry_point(de, s, false, true, PAUSE_MODE_TILE,
				t_x, t_y, s->col_bd[t_x], s->row_bd[t_y],
				de->entry_qp, de->entry_slice);
	}
	return 0;
}

/* Write STATUS register with expected end CTU address of previous slice */
static int end_previous_slice(struct hevc_d_dec_env *const de,
			      const struct hevc_d_dec_state *const s)
{
	int rv;

	rv = tile_entry_fill(de, s,
			     ctb_to_tile_x(s, s->prev_ctb_x),
			     ctb_to_tile_y(s, s->prev_ctb_y));
	if (rv)
		return rv;

	p1_apb_write(de, RPI_STATUS,
		     1 | (s->prev_ctb_x << 5) | (s->prev_ctb_y << 18));
	return 0;
}

static int decode_slice(struct hevc_d_dec_env *const de,
			const struct hevc_d_dec_state *const s,
			bool last_slice)
{
	bool reset_qp_y;
	unsigned int tile_x = ctb_to_tile_x(s, s->start_ctb_x);
	unsigned int tile_y = ctb_to_tile_y(s, s->start_ctb_y);
	int rv;

	if (s->start_ts) {
		rv = end_previous_slice(de, s);
		if (rv)
			return rv;
	}

	rv = cmds_check_space(de,
			      CMDS_WRITE_BITSTREAM +
				CMDS_WRITE_PROB +
				CMDS_PROGRAM_SLICECMDS +
				CMDS_NEW_SLICE_SEGMENT +
				CMDS_NEW_ENTRY_POINT);
	if (rv)
		return rv;

	pre_slice_decode(de, s);
	write_bitstream(de, s);

	reset_qp_y = !s->start_ts ||
		!s->dependent_slice_segment_flag ||
		tile_x != ctb_to_tile_x(s, s->prev_ctb_x) ||
		tile_y != ctb_to_tile_y(s, s->prev_ctb_y);
	if (reset_qp_y)
		write_prob(de, s);

	program_slicecmds(de, s->slice_idx);
	new_slice_segment(de, s);
	new_entry_point(de, s, !s->dependent_slice_segment_flag, reset_qp_y,
			PAUSE_MODE_TILE,
			tile_x, tile_y, s->start_ctb_x, s->start_ctb_y,
			s->slice_qp, slice_reg_const(s));

	/*
	 * If this is the last slice then fill in the other tile entries
	 * now, otherwise this will be done at the start of the next slice
	 * when it will be known where this slice finishes
	 */
	if (last_slice) {
		rv = tile_entry_fill(de, s,
				     s->tile_width - 1,
				     s->tile_height - 1);
		if (rv)
			return rv;
		p1_apb_write(de, RPI_STATUS,
			     1 | ((s->ctb_width - 1) << 5) |
				((s->ctb_height - 1) << 18));
	}
	return 0;
}

/* Scaling factors */

static void expand_scaling_list(const unsigned int size_id,
				u8 *const dst0,
				const u8 *const src0, uint8_t dc)
{
	u8 *d;
	unsigned int x, y;

	switch (size_id) {
	case 0:
		memcpy(dst0, src0, 16);
		break;
	case 1:
		memcpy(dst0, src0, 64);
		break;
	case 2:
		d = dst0;

		for (y = 0; y != 16; y++) {
			const u8 *s = src0 + (y >> 1) * 8;

			for (x = 0; x != 8; ++x) {
				*d++ = *s;
				*d++ = *s++;
			}
		}
		dst0[0] = dc;
		break;
	default:
		d = dst0;

		for (y = 0; y != 32; y++) {
			const u8 *s = src0 + (y >> 2) * 8;

			for (x = 0; x != 8; ++x) {
				*d++ = *s;
				*d++ = *s;
				*d++ = *s;
				*d++ = *s++;
			}
		}
		dst0[0] = dc;
		break;
	}
}

static void populate_scaling_factors(const struct hevc_d_run *const run,
				     struct hevc_d_dec_env *const de,
				     const struct hevc_d_dec_state *const s)
{
	const struct v4l2_ctrl_hevc_scaling_matrix *const sl =
		run->h265.scaling_matrix;
	/* Array of constants for scaling factors */
	static const u32 scaling_factor_offsets[4][6] = {
		/*
		 * MID0    MID1    MID2    MID3    MID4    MID5
		 */
		/* SID0 (4x4) */
		{ 0x0000, 0x0010, 0x0020, 0x0030, 0x0040, 0x0050 },
		/* SID1 (8x8) */
		{ 0x0060, 0x00A0, 0x00E0, 0x0120, 0x0160, 0x01A0 },
		/* SID2 (16x16) */
		{ 0x01E0, 0x02E0, 0x03E0, 0x04E0, 0x05E0, 0x06E0 },
		/* SID3 (32x32) */
		{ 0x07E0, 0x0BE0, 0x0000, 0x0000, 0x0000, 0x0000 }
	};
	unsigned int mid;

	for (mid = 0; mid < 6; mid++)
		expand_scaling_list(0, de->scaling_factors +
					    scaling_factor_offsets[0][mid],
				    sl->scaling_list_4x4[mid], 0);
	for (mid = 0; mid < 6; mid++)
		expand_scaling_list(1, de->scaling_factors +
					    scaling_factor_offsets[1][mid],
				    sl->scaling_list_8x8[mid], 0);
	for (mid = 0; mid < 6; mid++)
		expand_scaling_list(2, de->scaling_factors +
					    scaling_factor_offsets[2][mid],
				    sl->scaling_list_16x16[mid],
				    sl->scaling_list_dc_coef_16x16[mid]);
	for (mid = 0; mid < 2; mid++)
		expand_scaling_list(3, de->scaling_factors +
					    scaling_factor_offsets[3][mid],
				    sl->scaling_list_32x32[mid],
				    sl->scaling_list_dc_coef_32x32[mid]);
}

static void free_ps_info(struct hevc_d_dec_state *const s)
{
	kfree(s->ctb_addr_rs_to_ts);
	s->ctb_addr_rs_to_ts = NULL;
	kfree(s->ctb_addr_ts_to_rs);
	s->ctb_addr_ts_to_rs = NULL;

	kfree(s->col_bd);
	s->col_bd = NULL;
	kfree(s->row_bd);
	s->row_bd = NULL;
}

static unsigned int tile_width(const struct hevc_d_dec_state *const s,
			       const unsigned int t_x)
{
	return s->col_bd[t_x + 1] - s->col_bd[t_x];
}

static unsigned int tile_height(const struct hevc_d_dec_state *const s,
				const unsigned int t_y)
{
	return s->row_bd[t_y + 1] - s->row_bd[t_y];
}

static void fill_rs_to_ts(struct hevc_d_dec_state *const s)
{
	unsigned int ts = 0;
	unsigned int t_y;
	unsigned int tr_rs = 0;

	for (t_y = 0; t_y != s->tile_height; ++t_y) {
		const unsigned int t_h = tile_height(s, t_y);
		unsigned int t_x;
		unsigned int tc_rs = tr_rs;

		for (t_x = 0; t_x != s->tile_width; ++t_x) {
			const unsigned int t_w = tile_width(s, t_x);
			unsigned int y;
			unsigned int rs = tc_rs;

			for (y = 0; y != t_h; ++y) {
				unsigned int x;

				for (x = 0; x != t_w; ++x) {
					s->ctb_addr_rs_to_ts[rs + x] = ts;
					s->ctb_addr_ts_to_rs[ts] = rs + x;
					++ts;
				}
				rs += s->ctb_width;
			}
			tc_rs += t_w;
		}
		tr_rs += t_h * s->ctb_width;
	}
}

static int updated_ps(struct hevc_d_dec_state *const s)
{
	unsigned int i;
	int rv = -ENOMEM;

	free_ps_info(s);

	/* Inferred parameters */
	s->log2_ctb_size = s->sps.log2_min_luma_coding_block_size_minus3 + 3 +
			   s->sps.log2_diff_max_min_luma_coding_block_size;
	if (s->log2_ctb_size < s->pps.diff_cu_qp_delta_depth)
		goto fail_inval;

	s->ctb_width = (s->sps.pic_width_in_luma_samples +
			(1 << s->log2_ctb_size) - 1) >>
		       s->log2_ctb_size;
	s->ctb_height = (s->sps.pic_height_in_luma_samples +
			 (1 << s->log2_ctb_size) - 1) >>
			s->log2_ctb_size;
	s->ctb_size = s->ctb_width * s->ctb_height;

	s->ctb_addr_rs_to_ts = kmalloc_array(s->ctb_size,
					     sizeof(*s->ctb_addr_rs_to_ts),
					     GFP_KERNEL);
	if (!s->ctb_addr_rs_to_ts)
		goto fail;
	s->ctb_addr_ts_to_rs = kmalloc_array(s->ctb_size,
					     sizeof(*s->ctb_addr_ts_to_rs),
					     GFP_KERNEL);
	if (!s->ctb_addr_ts_to_rs)
		goto fail;

	if (!(s->pps.flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED)) {
		s->tile_width = 1;
		s->tile_height = 1;
	} else {
		s->tile_width = s->pps.num_tile_columns_minus1 + 1;
		s->tile_height = s->pps.num_tile_rows_minus1 + 1;
	}

	s->col_bd = kmalloc_array((s->tile_width + 1), sizeof(*s->col_bd),
				  GFP_KERNEL);
	if (!s->col_bd)
		goto fail;
	s->row_bd = kmalloc_array((s->tile_height + 1), sizeof(*s->row_bd),
				  GFP_KERNEL);
	if (!s->row_bd)
		goto fail;

	/* Can't check col widths/row heights in PPS validation so do here */
	s->col_bd[0] = 0;
	for (i = 1; i < s->tile_width; i++) {
		s->col_bd[i] = s->col_bd[i - 1] +
			s->pps.column_width_minus1[i - 1] + 1;
		if (s->col_bd[i] >= s->ctb_width)
			goto fail_inval;
	}
	s->col_bd[s->tile_width] = s->ctb_width;

	s->row_bd[0] = 0;
	for (i = 1; i < s->tile_height; i++) {
		s->row_bd[i] = s->row_bd[i - 1] +
			s->pps.row_height_minus1[i - 1] + 1;
		if (s->row_bd[i] >= s->ctb_height)
			goto fail_inval;
	}
	s->row_bd[s->tile_height] = s->ctb_height;

	fill_rs_to_ts(s);
	return 0;

fail_inval:
	rv = -EINVAL;
fail:
	free_ps_info(s);
	/* Set invalid to force reload */
	s->sps.pic_width_in_luma_samples = 0;
	return rv;
}

/*
 * Assume that we get exactly the same DPB for every slice it makes no real
 * sense otherwise.
 */
#if V4L2_HEVC_DPB_ENTRIES_NUM_MAX > 16
#error HEVC_DPB_ENTRIES > h/w slots
#endif

static u32 mk_config2(const struct hevc_d_dec_state *const s)
{
	const struct v4l2_ctrl_hevc_sps *const sps = &s->sps;
	const struct v4l2_ctrl_hevc_pps *const pps = &s->pps;
	u32 c;

	c = (sps->bit_depth_luma_minus8 + 8) << 0;	/* BitDepthY */
	c |= (sps->bit_depth_chroma_minus8 + 8) << 4;	/* BitDepthC */
	if (sps->bit_depth_luma_minus8)			/* BitDepthY */
		c |= BIT(8);
	if (sps->bit_depth_chroma_minus8)		/* BitDepthC */
		c |= BIT(9);
	c |= s->log2_ctb_size << 10;
	if (pps->flags & V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED)
		c |= BIT(13);
	if (sps->flags & V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED)
		c |= BIT(14);
	if (s->mk_aux)
		c |= BIT(15); /* Write motion vectors to external memory */
	c |= min(s->log2_ctb_size, pps->log2_parallel_merge_level_minus2 + 2) << 16;
	if (s->slice_temporal_mvp)
		c |= BIT(19);
	if (sps->flags & V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED)
		c |= BIT(20);
	c |= (pps->pps_cb_qp_offset & 31) << 21;
	c |= (pps->pps_cr_qp_offset & 31) << 26;
	return c;
}

static inline bool is_ref_unit_type(const unsigned int nal_unit_type)
{
	/* From Table 7-1
	 * True for 1, 3, 5, 7, 9, 11, 13, 15
	 */
	return (nal_unit_type & ~0xe) != 0;
}

/* Handle PU and COEFF stream overflow
 *
 * Returns:
 * -1  Phase 1 decode error
 *  0  OK
 * >0  Out of space (bitmask)
 */

#define STATUS_COEFF_EXHAUSTED	8
#define STATUS_PU_EXHAUSTED	16

static int check_status(const struct hevc_d_dev *const dev)
{
	const u32 cfstatus = apb_read(dev, RPI_CFSTATUS);
	const u32 cfnum = apb_read(dev, RPI_CFNUM);
	u32 status = apb_read(dev, RPI_STATUS);

	/*
	 * Handle PU and COEFF stream overflow
	 * This is the definition of successful completion of phase 1.
	 * It assures that status register is zero and all blocks in each tile
	 * have completed
	 */
	if (cfstatus == cfnum)
		return 0;

	status &= (STATUS_PU_EXHAUSTED | STATUS_COEFF_EXHAUSTED);
	if (status)
		return status;

	return -1;
}

#pragma GCC diagnostic pop

/* rpivid: the plumbing around the kept functions, in this translation unit so that it reaches them */
#include "cmdgen.c"
