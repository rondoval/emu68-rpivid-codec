/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hw_task.c - the decode task: the only thing that drives the block (docs/
 * architecture.md section 8.3).  It starts phase 1 of the queue's head when the
 * entropy stage and a slot are free, phase 2 of the oldest picture that
 * finished phase 1 when the reconstruction stage is free, reads each phase's
 * result when its interrupt comes, grows a scratch buffer that ran out and
 * runs the phase again, and retires finished pictures through their owner's
 * hook.  While a phase runs a timer ticks, and a phase older than a second
 * is a wedged block.  It never parses and never runs client code.
 */
#include <dos/dos.h>

#include <driver_task.h>
#include <drv_timer.h>
#include <timing.h>

#include <rpivid_private.h>
#include <proto/exec.h> /* after rpivid_private.h: __NOLIBBASE__ */

#include <debug.h>

#include "hevc_d_hw.h"

/**
 * retire_all() - Call the owners' retire hooks for a list of pictures, no lock held.
 * @hw:   the hardware layer's state
 * @list: the pictures, emptied
 */
static void retire_all(struct rp_hw *hw, struct MinList *list)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	struct rp_picture *p;
	while ((p = (struct rp_picture *)RemHead((struct List *)list)) != NULL)
		p->retire(p);
}

/**
 * slot_grow() - A slot's buffers at least the sizes wanted.
 * @hw:   the hardware layer's state, queue_lock held
 * @slot: the slot
 *
 * Return: 0, or -1 without memory (the old buffers stay).
 */
static int slot_grow(struct rp_hw *hw, struct rp_slot *slot)
{
	if (slot->coeff.size < hw->slot_coeff_want && hwbuf_realloc_new(&hw->dev, &slot->coeff, hw->slot_coeff_want))
		return -1;
	if (slot->pu.size < hw->slot_pu_want && hwbuf_realloc_new(&hw->dev, &slot->pu, hw->slot_pu_want))
		return -1;
	return 0;
}

/**
 * want_next_size() - A scratch buffer that ran out is wanted at Linux's next size, unless more is wanted already.
 * @want: the size the slots' buffers of this kind are to have
 * @buf:  the buffer that ran out
 */
static void want_next_size(size_t *want, const struct hevc_d_hwbuf *buf)
{
	size_t next = hevc_d_round_up_size(buf->size + 1);
	if (next > *want)
		*want = next;
}

/**
 * fail_picture() - A picture is dropped: its slot freed, itself listed to retire.
 * @hw:   the hardware layer's state, queue_lock held
 * @p:    the picture
 * @done: where it goes, for retire_all() once the lock is released
 */
static void fail_picture(struct rp_hw *hw, struct rp_picture *p, struct MinList *done)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	if (p->slot != NULL)
	{
		rp_hw_slot_release(hw, p->slot);
		p->slot = NULL;
	}
	p->state = RP_PIC_FAILED;
	AddTail((struct List *)done, (struct Node *)&p->node);
}

/**
 * phase1_done() - Phase 1's interrupt: the result read, the picture moved on, back, or dropped.
 * @hw:   the hardware layer's state, queue_lock held
 * @done: pictures to retire once the lock is released
 *
 * An exhausted scratch buffer grows to Linux's next size, or to what the
 * slots are wanted to have, whichever is more, and the picture goes back to
 * the head of the queue for phase 1 again, keeping its slot.
 */
static void phase1_done(struct rp_hw *hw, struct MinList *done)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	struct rp_picture *p = hw->in_phase1;
	if (p == NULL)
	{
		Kprintf("[rpivid] phase 1 interrupt with nothing in phase 1\n");
		return;
	}
	hw->in_phase1 = NULL;
	p->status = cmdgen_phase1_result(&hw->dev);
	if (p->status == 0)
	{
		p->state = RP_PIC_PHASE1_DONE;
		AddTail((struct List *)&hw->phase1_done, (struct Node *)&p->node);
		return;
	}
	if (p->status < 0)
	{
		Kprintf("[rpivid] phase 1 failed\n");
		fail_picture(hw, p, done);
		return;
	}
	struct rp_slot *slot = p->slot;
	if (p->status & STATUS_COEFF_EXHAUSTED)
		want_next_size(&hw->slot_coeff_want, &slot->coeff);
	if (p->status & STATUS_PU_EXHAUSTED)
		want_next_size(&hw->slot_pu_want, &slot->pu);
	if (slot_grow(hw, slot))
	{
		Kprintf("[rpivid] no memory to grow a scratch buffer\n");
		fail_picture(hw, p, done);
		return;
	}
	KprintfT("[rpivid] scratch grown to %lu + %lu, phase 1 again\n", (ULONG)slot->coeff.size, (ULONG)slot->pu.size);
	p->reruns++;
	hw->reruns++;
	p->state = RP_PIC_QUEUED;
	AddHead((struct List *)&hw->queue, (struct Node *)&p->node);
}

/**
 * phase2_done() - Phase 2's interrupt: the picture is decoded.
 * @hw:   the hardware layer's state, queue_lock held
 * @done: pictures to retire once the lock is released
 */
static void phase2_done(struct rp_hw *hw, struct MinList *done)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	struct rp_picture *p = hw->in_phase2;
	if (p == NULL)
	{
		Kprintf("[rpivid] phase 2 interrupt with nothing in phase 2\n");
		return;
	}
	hw->in_phase2 = NULL;
	rp_hw_slot_release(hw, p->slot);
	p->slot = NULL;
	p->state = RP_PIC_DONE;
	AddTail((struct List *)done, (struct Node *)&p->node);
}

/**
 * wedge() - A phase never ended: everything in flight fails, nothing starts again.
 * @hw:   the hardware layer's state, queue_lock held
 * @done: pictures to retire once the lock is released
 */
static void wedge(struct rp_hw *hw, struct MinList *done)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	struct rp_picture *p;
	Kprintf("[rpivid] the block is wedged: phase 1 %s, phase 2 %s\n", hw->in_phase1 ? "running" : "idle",
	        hw->in_phase2 ? "running" : "idle");
	hw->wedged = TRUE;
	hw->wedges++;
	if (hw->in_phase1 != NULL)
	{
		fail_picture(hw, hw->in_phase1, done);
		hw->in_phase1 = NULL;
	}
	if (hw->in_phase2 != NULL)
	{
		fail_picture(hw, hw->in_phase2, done);
		hw->in_phase2 = NULL;
	}
	while ((p = (struct rp_picture *)RemHead((struct List *)&hw->phase1_done)) != NULL)
		fail_picture(hw, p, done);
	while ((p = (struct rp_picture *)RemHead((struct List *)&hw->queue)) != NULL)
		fail_picture(hw, p, done);
}

/**
 * wedge_check() - At a tick: a phase older than RP_WEDGE_US means a wedged block.
 * @hw:   the hardware layer's state, queue_lock held
 * @done: pictures to retire once the lock is released
 */
static void wedge_check(struct rp_hw *hw, struct MinList *done)
{
	u32 now = get_time();
	if ((hw->in_phase1 != NULL && now - hw->phase1_started_us > RP_WEDGE_US) ||
	    (hw->in_phase2 != NULL && now - hw->phase2_started_us > RP_WEDGE_US))
		wedge(hw, done);
}

/**
 * start_phase2() - Phase 2 of the oldest picture past phase 1, if the reconstruction stage is free.
 * @hw: the hardware layer's state, queue_lock held on entry and on return
 *
 * The register writes happen with the lock released: the phase is marked as
 * running first, so the interrupt that follows the final write finds it.
 */
static void start_phase2(struct rp_hw *hw)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	if (hw->in_phase2 != NULL)
		return;
	struct rp_picture *p = (struct rp_picture *)RemHead((struct List *)&hw->phase1_done);
	if (p == NULL)
		return;
	p->state = RP_PIC_PHASE2;
	hw->in_phase2 = p;
	hw->phase2_started_us = get_time();
	ReleaseSemaphore(&hw->queue_lock);
	cmdgen_phase2_regs(&hw->dev, p->de);
	ObtainSemaphore(&hw->queue_lock);
}

/**
 * slot_for() - The scratch slot a picture runs phase 1 in.
 * @hw: the hardware layer's state, queue_lock held
 * @p:  the picture
 *
 * Return: the slot it holds from an earlier run, else a free one; NULL
 * when every slot is held: phase 2 frees one.
 */
static struct rp_slot *slot_for(struct rp_hw *hw, const struct rp_picture *p)
{
	if (p->slot != NULL)
		return p->slot;
	for (unsigned int i = 0; i < RP_SLOTS; i++)
		if (hw->slots[i].picture == NULL)
			return &hw->slots[i];
	return NULL;
}

/**
 * start_phase1() - Phase 1 of the queue's head, if the entropy stage and a slot are free.
 * @hw:   the hardware layer's state, queue_lock held on entry and on return
 * @done: pictures to retire once the lock is released
 *
 * The register writes happen with the lock released, as in start_phase2().
 */
static void start_phase1(struct rp_hw *hw, struct MinList *done)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	struct rp_picture *p = (struct rp_picture *)hw->queue.mlh_Head;
	if (hw->in_phase1 != NULL || p->node.mln_Succ == NULL)
		return;
	struct rp_slot *slot = slot_for(hw, p);
	if (slot == NULL)
		return;
	Remove((struct Node *)&p->node);
	if (slot_grow(hw, slot))
	{
		Kprintf("[rpivid] no memory for a scratch slot\n");
		fail_picture(hw, p, done);
		return;
	}
	p->slot = slot;
	slot->picture = p;
	p->state = RP_PIC_PHASE1;
	hw->in_phase1 = p;
	hw->phase1_started_us = get_time();
	ReleaseSemaphore(&hw->queue_lock);
	cmdgen_phase1_regs(&hw->dev, p->de, &slot->pu, &slot->coeff);
	ObtainSemaphore(&hw->queue_lock);
}

/**
 * interrupts_serve() - The phases whose end the interrupt server signalled, acted on.
 * @hw:   the hardware layer's state, queue_lock held
 * @sigs: the signals Wait() returned
 * @done: pictures to retire once the lock is released
 *
 * Phase 2 before phase 1, as Linux does, so the picture waiting for the
 * reconstruction stage moves on at once.
 */
static void interrupts_serve(struct rp_hw *hw, ULONG sigs, struct MinList *done)
{
	if (sigs & hw->isr.phase2_mask)
		phase2_done(hw, done);
	if (sigs & hw->isr.phase1_mask)
		phase1_done(hw, done);
}

/**
 * rp_hw_task() - The decode task's body (drv_task_spawn()).
 * @hw:     the hardware layer's state
 * @parent: the spawner, told when the loop is reached
 *
 * Waits for the interrupt server's two signals, a queued picture, the tick
 * and the join; serves the phases that ended, checks for a wedge at a tick,
 * then starts what can start: phase 2, then phase 1.
 */
void rp_hw_task(struct rp_hw *hw, struct Task *parent)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	struct drv_timer tick;
	BOOL ticking = FALSE;

	BYTE phase1_signal = AllocSignal(-1);
	BYTE phase2_signal = AllocSignal(-1);
	BYTE queue_signal = AllocSignal(-1);
	if (phase1_signal == -1 || phase2_signal == -1 || queue_signal == -1 || !drv_timer_open(&tick, SysBase))
	{
		Kprintf("[rpivid] %s: no signal or timer\n", __func__);
		FreeSignal(phase1_signal); /* -1 frees nothing */
		FreeSignal(phase2_signal);
		FreeSignal(queue_signal);
		drv_task_exit(SysBase, &hw->task, parent, FALSE);
		return;
	}
	ULONG phases_mask = (1UL << phase1_signal) | (1UL << phase2_signal);
	ULONG queue_mask = 1UL << queue_signal;
	ULONG tick_mask = drv_timer_sigmask(&tick);

	hw->isr.task = FindTask(NULL);
	hw->isr.phase1_mask = 1UL << phase1_signal;
	hw->isr.phase2_mask = 1UL << phase2_signal;
	hw->sig_queue = queue_mask;
	hw->task = FindTask(NULL);
	Signal(parent, SIGBREAKF_CTRL_F);

	for (;;)
	{
		struct MinList done;
		_NewMinList(&done);

		ULONG sigs = Wait(phases_mask | queue_mask | tick_mask | SIGBREAKF_CTRL_C);
		if (sigs & SIGBREAKF_CTRL_C)
			break;

		ObtainSemaphore(&hw->queue_lock);
		if (sigs & phases_mask)
			interrupts_serve(hw, sigs, &done);
		if (sigs & tick_mask)
		{
			drv_timer_consume(&tick);
			ticking = FALSE;
			wedge_check(hw, &done);
		}
		if (!hw->halted && !hw->wedged)
		{
			start_phase2(hw);
			start_phase1(hw, &done);
		}
		BOOL busy = hw->in_phase1 != NULL || hw->in_phase2 != NULL;
		ReleaseSemaphore(&hw->queue_lock);

		retire_all(hw, &done);

		if (busy && !ticking)
		{
			drv_timer_arm_ms(&tick, RP_TICK_MS);
			ticking = TRUE;
		}
	}

	if (ticking)
		drv_timer_cancel(&tick);
	drv_timer_close(&tick);
	hw->isr.task = NULL;
	FreeSignal(phase1_signal);
	FreeSignal(phase2_signal);
	FreeSignal(queue_signal);
	drv_task_exit(SysBase, &hw->task, parent, TRUE);
}
