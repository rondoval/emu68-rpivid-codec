/* SPDX-License-Identifier: GPL-2.0 */
/*
 * narrow.h - the Main10 pass (docs/architecture.md section 10): the block's 10-bit
 * column layout narrowed to its 8-bit one by the 68k, by truncation.
 */
#ifndef RP_NARROW_H
#define RP_NARROW_H

#include <exec/types.h>

#define RP_NARROW_IN_COLUMNS 4  /* a group: four 10-bit columns of 96 samples ... */
#define RP_NARROW_OUT_COLUMNS 3 /* ... into three 8-bit columns of 128 */

/**
 * narrow_picture() - A picture's 10-bit columns narrowed into 8-bit columns.
 * @in:     the 10-bit columns, @groups x 4 of them, 128 bytes wide
 * @out:    the 8-bit columns, @groups x 3 of them
 * @groups: groups of four input columns
 * @lines:  lines per column, luma and chroma rows alike: a column's bytes are @lines x 128
 *
 * The top eight bits of every sample; nothing is done for no group or no
 * line.  What the 68k reads must be in memory (invalidated) first; what it
 * writes stays in its cache (cache_ops.h).
 */
void narrow_picture(const void *in, void *out, ULONG groups, ULONG lines);

#endif /* RP_NARROW_H */
