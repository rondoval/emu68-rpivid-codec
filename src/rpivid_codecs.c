/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rpivid_codecs.c - the codec table: one row, VCC_H265 decoding on the
 * block, or none on a machine without one (docs/architecture.md section 5.2).
 */
#include <rpivid_private.h> /* first: __NOLIBBASE__ for the proto headers below */

#include <devtree.h>
#include <memory.h>
#include <strutil.h>

#define RP_MAX_WIDTH 4096  /* the largest 16:9 picture within the block's 4096 each way */
#define RP_MAX_HEIGHT 2304
#define RP_MAX_LEVEL 51    /* level 5.1: 4K at 60 */

/**
 * block_present() - Whether the device tree has the block, asked once.
 * @base: the library base
 *
 * A Pi 3 has no hevc_dec node: the table is then empty, so that
 * videocodec.library answers that no backend decodes H.265 rather than
 * opening a session that cannot start.  Asked at the frontend's first query,
 * in its task, not in LibInit()'s Forbid().  The answer is stored once, so
 * a second caller sees either -1 and asks too, or the answer.
 *
 * Return: TRUE with the block.
 */
static BOOL block_present(struct RpividBase *base)
{
	if (base->block_found == RP_BLOCK_UNKNOWN)
	{
		struct ExecBase *SysBase = base->SysBase;
		APTR DeviceTreeBase = OpenResource((CONST_STRPTR) "devicetree.resource");
		APTR node = NULL;
		if (DeviceTreeBase != NULL)
			node = DT_OpenSymbol(DeviceTreeBase, (CONST_STRPTR)RP_HEVC_SYMBOL);
		base->block_found = node != NULL ? RP_BLOCK_PRESENT : RP_BLOCK_NONE;
		if (node != NULL)
			DT_CloseKey(node);
	}
	return base->block_found == RP_BLOCK_PRESENT;
}

/**
 * codec_count() - How many rows VCB_QueryCodec() answers.
 * @base: the library base
 *
 * Return: 1 with the block, 0 without.
 */
ULONG codec_count(struct RpividBase *base)
{
	return block_present(base) ? 1 : 0;
}

/**
 * codec_query() - One row of the table.
 * @base:  the library base
 * @index: the row, 0 upward
 * @info:  the caller's structure, filled as far as its Size reaches
 *
 * Return: 0, or VCERR_BADPARAM past the last row.
 */
LONG codec_query(struct RpividBase *base, ULONG index, struct VCCodecInfo *info)
{
	struct VCCodecInfo row;
	if (index >= codec_count(base))
		return VCERR_BADPARAM;
	memset(&row, 0, sizeof(row));
	row.Size = info->Size < sizeof(row) ? info->Size : (ULONG)sizeof(row);
	row.Codec = VCC_H265;
	row.Kind = VCK_Decoder;
	row.MaxWidth = RP_MAX_WIDTH;
	row.MaxHeight = RP_MAX_HEIGHT;
	row.MaxLevel = RP_MAX_LEVEL;
	row.OutputFormats = VCF_MASK(VCF_NV12_COL128);
	strlcpy((char *)row.Backend, LIBRARY_NAME, sizeof(row.Backend));
	row.Priority = VCB_PRIORITY_HARDWARE;
	row.Flags = VCCF_Session;
	memcpy(info, &row, row.Size);
	return 0;
}
