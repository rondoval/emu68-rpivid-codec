/* SPDX-License-Identifier: GPL-2.0 */
/*
 * narrow.S - the Main10 pass (docs/architecture.md section 10): the HEVC block's
 * 10-bit column layout narrowed to its 8-bit one, four input columns into
 * three output columns per line, by truncation, group after group of a
 * picture.
 *
 *   void narrow_picture(const void *in, void *out, ULONG groups, ULONG lines);
 *
 *   in      the 10-bit columns, groups x 4 of them, 128 bytes wide
 *   out     the 8-bit columns, groups x 3 of them
 *   groups  groups of four input columns
 *   lines   lines per column, luma and chroma rows alike
 *
 * Nothing is done for no group or no line.  A column's bytes are its lines
 * x 128, in and out.
 *
 * A 32-bit little-endian word holds three 10-bit samples: bits 0-9, 10-19,
 * 20-29 (Linux, pixfmt-yuv-planar.rst).  A line of a group is 128 words,
 * words 0-31 from the first column and so on, and sample s of the 384 goes to
 * output column s/128 at s%128.  Every pointer advances through its own column
 * by an (aN)+ mode, so a line is one unrolled block of 128 words with no
 * address arithmetic; the line count is the inner loop.  A word is swapped to
 * the 68k's order and each sample's top eight bits extracted with bfextu,
 * whose offsets count from bit 31.  When a group's lines are done, its fourth
 * input column's pointer is at the next group's first column and its third
 * output column's at the next group's first output column: the outer loop
 * starts there.
 *
 * The constants and the word counter are named L_*: the a.out assembler
 * keeps a symbol starting with L out of the object, as it does not one
 * starting with .L that .set defines.
 *
 * Registers: a0, a2, a3, a4 the input columns; a1, a5, a6 the output
 * columns; d0 the word, d1 the sample, d2 the line count, d3 the column
 * bytes, d4 the lines, d5 the group count.
 */

	.set L_COLUMN_LOG2, 7                      | a column is 128 bytes wide, in and out
	.set L_IN_WORDS, 32                        | the words of an input column's line: 96 samples, 3 to a word
	.set L_GROUP_WORDS, 4 * L_IN_WORDS         | the words of a group's line, its four input columns
	.set L_OUT_BYTES, 128                      | the bytes of an output column's line: 128 samples
	.set L_WORD_BITS, 32
	.set L_SAMPLE_BITS, 10
	.set L_OUT_BITS, 8                         | the top eight bits of a sample are kept
	.set L_TOP0, L_WORD_BITS - 1 * L_SAMPLE_BITS | bfextu's offset of sample 0's top bits (bits 9-2)
	.set L_TOP1, L_WORD_BITS - 2 * L_SAMPLE_BITS | sample 1's (bits 19-12)
	.set L_TOP2, L_WORD_BITS - 3 * L_SAMPLE_BITS | sample 2's (bits 29-22)

	.macro OUTB s
	.if \s < L_OUT_BYTES
	move.b %d1,(%a1)+
	.elseif \s < 2 * L_OUT_BYTES
	move.b %d1,(%a5)+
	.else
	move.b %d1,(%a6)+
	.endif
	.endm

	.macro WORD w
	.if \w < L_IN_WORDS
	move.l (%a0)+,%d0
	.elseif \w < 2 * L_IN_WORDS
	move.l (%a2)+,%d0
	.elseif \w < 3 * L_IN_WORDS
	move.l (%a3)+,%d0
	.else
	move.l (%a4)+,%d0
	.endif
	ror.w #8,%d0
	swap %d0
	ror.w #8,%d0
	bfextu %d0{#L_TOP0:#L_OUT_BITS},%d1
	OUTB (3*\w)
	bfextu %d0{#L_TOP1:#L_OUT_BITS},%d1
	OUTB (3*\w+1)
	bfextu %d0{#L_TOP2:#L_OUT_BITS},%d1
	OUTB (3*\w+2)
	.endm

	.text
	.even
	.globl _narrow_picture

_narrow_picture:
	movem.l %d2-%d5/%a2-%a6,-(%sp)  | nine registers: the arguments start at 40(sp)
	movea.l 40(%sp),%a0             | in
	movea.l 44(%sp),%a1             | out
	move.l 48(%sp),%d5              | groups
	move.l 52(%sp),%d4              | lines
	tst.l %d5
	beq .Ldone
	tst.l %d4
	beq .Ldone
	move.l %d4,%d3
	lsl.l #L_COLUMN_LOG2,%d3        | the column bytes
.Lgroup:
	lea (%a0,%d3.l),%a2
	lea (%a2,%d3.l),%a3
	lea (%a3,%d3.l),%a4
	lea (%a1,%d3.l),%a5
	lea (%a5,%d3.l),%a6
	move.l %d4,%d2
	.altmacro
	.set L_w, 0
.Lline:
	.rept L_GROUP_WORDS
	WORD %L_w
	.set L_w, L_w+1
	.endr
	.noaltmacro
	subq.l #1,%d2
	bne .Lline
	movea.l %a4,%a0                 | the next group's first input column
	movea.l %a6,%a1                 | and its first output column
	subq.l #1,%d5
	bne .Lgroup
.Ldone:
	movem.l (%sp)+,%d2-%d5/%a2-%a6
	rts
