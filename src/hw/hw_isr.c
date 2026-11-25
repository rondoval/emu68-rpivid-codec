/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hw_isr.c - the block's interrupt server, which gic400 calls.
 *
 * Reads the interrupt control register; when neither activity source is
 * latched the interrupt is not ours (Z set, the chain goes on).  Otherwise
 * the value is written back with the reserved bits zero, which clears the
 * latched sources (a written 1 clears each) and keeps the enables, the count
 * goes up, and the task gets the signal of each phase that ended, phase 1's
 * for ACTIVE1 and phase 2's for ACTIVE2, in one Signal() (Z clear).  The
 * signals are the only thing the server tells the task: no word is shared,
 * so the task takes the ends with Wait() and needs no Disable().  A phase
 * cannot end twice before the task has seen it, as only the task starts the
 * next run, so a signal that coalesces loses nothing.  The register is little-endian: the value
 * is swapped on the way in and out (LE32).
 *
 * Two differences from Linux's handler.  Upstream takes the interrupt as its
 * own when any of ARG_IC_ICTRL_ALL_IRQ_MASK is latched, the H.264 and VP9
 * sources too; here only the two activity sources are, as block_serve()
 * enables only those, so no other can raise the line.  And the test bit is
 * written back 0, where upstream writes it back as read: a 1 written there
 * would raise the interrupt again.
 *
 * Assembly, as the stack's servers are:
 * the caller tests the Z flag, not D0, and a C epilogue can leave Z from a
 * restored register.  A0, D0, D1 are scratch; A1 becomes Signal()'s task once
 * the data is finished with.  File-scope asm has no IR for LTO, so this file
 * is compiled as a real object (CMakeLists).
 */
#include <stddef.h>

#include <rpivid_private.h>
#include "hevc_d_hw.h"

/* exec.library Signal(), from <lvo/exec_lib.i> */
#define LVO_SIGNAL (-324)

/* the latched sources that make the interrupt ours: the block's two activity lines, phase 1's and phase 2's */
#define ISR_OURS (ARG_IC_ICTRL_ACTIVE1_INT_SET | ARG_IC_ICTRL_ACTIVE2_INT_SET)
/* the bits written back: all but the reserved ones and the test bit, so the latched sources (1) clear and the enables stay */
#define ISR_KEEP (~(ARG_IC_ICTRL_SET_ZERO_MASK | ARG_IC_ICTRL_TEST_INT))

__asm__(
	"	.macro LE32 reg\n" /* a 32-bit word between little-endian and the 68k's order */
	"	ror.w	#8,\\reg\n"
	"	swap	\\reg\n"
	"	ror.w	#8,\\reg\n"
	"	.endm\n"
	"	.section .text.isr.rp_hw_isr,\"ax\"\n"
	"	.even\n"
	"	.globl	_rp_hw_isr\n"
	"_rp_hw_isr:\n"
	"	movea.l	%c[ictrl](%%a1),%%a0\n" /* A0 = d->ictrl */
	"	move.l	(%%a0),%%d0\n"
	"	LE32	%%d0\n"
	"	move.l	%%d0,%%d1\n"
	"	andi.l	#%c[ours],%%d1\n"
	"	bne.s	1f\n"
	"	moveq	#0,%%d0\n" /* not ours: Z set */
	"	rts\n"
	"1:	andi.l	#%c[keep],%%d0\n" /* reserved bits and the test bit off; the latched bits stay 1: written, they clear */
	"	LE32	%%d0\n"
	"	move.l	%%d0,(%%a0)\n"
	"	addq.l	#1,%c[count](%%a1)\n" /* d->count++ */
	"	moveq	#0,%%d0\n"            /* the signals of the phases that ended */
	"	btst	#%c[active1],%%d1\n"
	"	beq.s	2f\n"
	"	or.l	%c[phase1](%%a1),%%d0\n"
	"2:	btst	#%c[active2],%%d1\n"
	"	beq.s	3f\n"
	"	or.l	%c[phase2](%%a1),%%d0\n"
	"3:	movea.l	%c[task](%%a1),%%a1\n"
	"	jsr	%c[lvosignal](%%a6)\n" /* Signal(task, mask) */
	"	moveq	#1,%%d0\n" /* ours: Z clear */
	"	rts\n"
	"	.text\n"
	:
	: [ictrl] "i"(offsetof(struct rp_isr_data, ictrl)),
	  [ours] "i"(ISR_OURS),
	  [keep] "i"(ISR_KEEP),
	  [count] "i"(offsetof(struct rp_isr_data, count)),
	  [phase1] "i"(offsetof(struct rp_isr_data, phase1_mask)),
	  [phase2] "i"(offsetof(struct rp_isr_data, phase2_mask)),
	  [active1] "i"(__builtin_ctz(ARG_IC_ICTRL_ACTIVE1_INT_SET)),
	  [active2] "i"(__builtin_ctz(ARG_IC_ICTRL_ACTIVE2_INT_SET)),
	  [task] "i"(offsetof(struct rp_isr_data, task)),
	  [lvosignal] "i"(LVO_SIGNAL));
