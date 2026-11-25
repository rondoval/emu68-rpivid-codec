/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hw.c - the block: its registers and interrupt through the device tree, its
 * clock through the mailbox, the claim, the interrupt server and the decode
 * task brought up with the first session and down at the frontend's idle;
 * the queue the sessions feed; the reset guard's prepare (docs/architecture.md
 * sections 8.1 and 8.4).
 */
#include <rpivid_private.h> /* first: __NOLIBBASE__ for the proto headers below */
#include <proto/exec.h>
#define GIC400_BASE_NAME hw->gic400Base
#include <proto/gic400.h>
#include <proto/mailbox.h>

#include <debug.h>
#include <devtree.h>
#include <driver_task.h>
#include <memory.h>
#include <timing.h>

#include "hevc_d_hw.h"

#define TAG_GET_MAX_CLOCK_RATE 0x00030004u /* the firmware's property tag: a clock's highest rate */
#define MAILBOX_REQUEST 0u                 /* a property message's code as sent: a request */
#define MAILBOX_OK 0x80000000u             /* its code as answered: the request was understood */
#define TAG_RESPONSE 0x80000000u           /* a tag's length word as answered: bit 31 set, the tag answered */
#define MAILBOX_END_TAG 0u                 /* the tag that ends a message */
#define CLOCK_STATE_ON 1u                  /* the mailbox's clock state: bit 0 */
#define PREPARE_WAIT_US 10000u             /* the reset guard's wait for the block's lines to drop */
#define PREPARE_POLL_US 100u               /* how often the wait looks at them */

/* The words of the property message that asks for a clock's highest rate: one tag, the clock in, the rate out */
enum max_rate_word : u8
{
	MSG_SIZE,    /* the message's bytes */
	MSG_CODE,    /* MAILBOX_REQUEST; MAILBOX_OK when answered */
	TAG_ID,      /* TAG_GET_MAX_CLOCK_RATE */
	TAG_BUFFER,  /* the tag's value buffer in bytes: the clock and the rate */
	TAG_LENGTH,  /* the request's bytes, the clock; answered, TAG_RESPONSE with the answer's bytes */
	VALUE_CLOCK, /* the clock id */
	VALUE_RATE,  /* the rate, in Hz, answered */
	MSG_END,     /* MAILBOX_END_TAG */
	MSG_WORDS,   /* the message's length in words */
};

/**
 * clock_max_rate() - The firmware's maximum rate of a clock.
 * @MailboxBase: the resource
 * @clock:       the clock id
 *
 * mailbox.resource has no call for it, so the property message is built
 * here, as the PCIe library builds its own (vl805_reset.c).
 *
 * Return: the rate in Hz, 0 if the firmware did not answer the message and its tag.
 */
static ULONG clock_max_rate(APTR MailboxBase, ULONG clock)
{
	ULONG command[MSG_WORDS];
	command[MSG_SIZE] = sizeof(command);
	command[MSG_CODE] = MAILBOX_REQUEST;
	command[TAG_ID] = TAG_GET_MAX_CLOCK_RATE;
	command[TAG_BUFFER] = (VALUE_RATE - VALUE_CLOCK + 1) * sizeof(ULONG);
	command[TAG_LENGTH] = sizeof(command[VALUE_CLOCK]);
	command[VALUE_CLOCK] = clock;
	command[VALUE_RATE] = 0;
	command[MSG_END] = MAILBOX_END_TAG;
	MB_RawCommand(command);
	BOOL answered = command[MSG_CODE] == MAILBOX_OK && (command[TAG_LENGTH] & TAG_RESPONSE) != 0;
	return answered ? command[VALUE_RATE] : 0;
}

/**
 * pool_make() - The DMA pool over the Fast RAM the ISP reaches, once.
 * @base: the library base
 *
 * Every buffer the block or the ISP touches comes from it (architecture.md section 9): the
 * regions of Emu68's RAM wholly below the VPU's reach, so that every frame
 * imports.  A region is a MemHeader the pool allocates anywhere in, so one
 * that crosses the reach cannot be cut: it is left out whole.
 *
 * Return: TRUE with base->pool set.
 */
static BOOL pool_make(struct RpividBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	if (base->pool != NULL)
		return TRUE;
	struct dma_mem_ctx *dma = &base->dma_ctx;
	dma_mem_init(dma, SysBase);
	u32 kept = 0;
	for (u32 i = 0; i < dma->count; i++)
		if (dma->regions[i].end <= RP_ISP_REACH_END)
			dma->regions[kept++] = dma->regions[i];
	if (kept == 0)
	{
		Kprintf("[rpivid] no Fast RAM within the ISP's reach\n");
		return FALSE;
	}
	dma->count = kept;
	base->pool = dma_pool_create(dma);
	return base->pool != NULL;
}

/**
 * block_locate() - The block's registers and interrupt, from the device tree.
 * @hw: the hardware layer's state
 *
 * Return: TRUE with the resources open and dev.base_h265, dev.base_irq and irq set; FALSE with the cause logged.
 */
static BOOL block_locate(struct rp_hw *hw)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	hw->DeviceTreeBase = OpenResource((CONST_STRPTR) "devicetree.resource");
	hw->MailboxBase = OpenResource((CONST_STRPTR) "mailbox.resource");
	if (hw->DeviceTreeBase == NULL || hw->MailboxBase == NULL)
	{
		Kprintf("[rpivid] devicetree.resource or mailbox.resource missing\n");
		return FALSE;
	}
	APTR DeviceTreeBase = hw->DeviceTreeBase;
	APTR node = DT_OpenSymbol(DeviceTreeBase, (CONST_STRPTR)RP_HEVC_SYMBOL);
	if (node == NULL)
	{
		Kprintf("[rpivid] no " RP_HEVC_SYMBOL " in the device tree\n");
		return FALSE;
	}
	hw->dev.base_h265 = DT_GetBaseAddressVirtual(DeviceTreeBase, node, 0);
	hw->dev.base_irq = DT_GetBaseAddressVirtual(DeviceTreeBase, node, 1);
	hw->irq = DT_GetInterrupt(DeviceTreeBase, node, 0);
	DT_CloseKey(node);
	if (hw->dev.base_h265 == NULL || hw->dev.base_irq == NULL || hw->irq < 0)
	{
		Kprintf("[rpivid] the node has no usable reg or interrupts\n");
		return FALSE;
	}
	return TRUE;
}

/**
 * clock_on() - The block's clock on, at the firmware's highest rate; how it was found is kept for the down.
 * @hw: the hardware layer's state, the block claimed
 */
static void clock_on(struct rp_hw *hw)
{
	APTR MailboxBase = hw->MailboxBase;
	hw->clock_state_found = MB_GetClockState(RP_HEVC_CLOCK);
	if ((hw->clock_state_found & CLOCK_STATE_ON) == 0)
		MB_SetClockState(RP_HEVC_CLOCK, CLOCK_STATE_ON);
	ULONG max = clock_max_rate(MailboxBase, RP_HEVC_CLOCK);
	if (max != 0)
		MB_SetClockRate(RP_HEVC_CLOCK, max);
}

/**
 * block_serve() - The block's interrupts enabled and served: the task, then the server that signals it.
 * @hw: the hardware layer's state, the block claimed and clocked
 *
 * The interrupt control register is set up as Linux's driver sets it: the
 * two activity enables, then a read written back to clear whatever was
 * latched.
 *
 * Return: TRUE; FALSE with the cause logged (rp_hw_down() undoes what was done).
 */
static BOOL block_serve(struct rp_hw *hw)
{
	struct ExecBase *SysBase = hw->base->SysBase;

	hw->ictrl_found = irq_read(&hw->dev, ARG_IC_ICTRL);
	irq_write(&hw->dev, ARG_IC_ICTRL, ARG_IC_ICTRL_ACTIVE1_EN_SET | ARG_IC_ICTRL_ACTIVE2_EN_SET);
	irq_write(&hw->dev, ARG_IC_ICTRL, irq_read(&hw->dev, ARG_IC_ICTRL));

	hw->gic400Base = OpenLibrary((CONST_STRPTR) "gic400.library", 0);
	if (hw->gic400Base == NULL)
	{
		Kprintf("[rpivid] gic400.library missing\n");
		return FALSE;
	}
	if (drv_task_spawn(SysBase, hw, (APTR)rp_hw_task, RP_TASK_NAME, RP_TASK_STACK, RP_TASK_PRIORITY) != 0)
	{
		Kprintf("[rpivid] no decode task\n");
		return FALSE;
	}
	hw->isr.ictrl = hw->dev.base_irq;
	hw->server.is_Node.ln_Type = NT_INTERRUPT;
	hw->server.is_Node.ln_Pri = 0;
	hw->server.is_Node.ln_Name = (char *)RP_TASK_NAME;
	hw->server.is_Data = &hw->isr;
	hw->server.is_Code = (APTR)rp_hw_isr;
	if (AddIntServerEx((ULONG)hw->irq, 0, FALSE, &hw->server) < 0)
	{
		Kprintf("[rpivid] cannot register interrupt %ld\n", hw->irq);
		return FALSE;
	}
	hw->server_added = TRUE;
	return TRUE;
}

/**
 * rp_hw_up() - The block claimed, clocked, found and served; the task running.
 * @hw:   the hardware layer's state, zeroed here first
 * @base: the library base
 *
 * Under sessions_lock, at the first session.  A failure once the block is
 * claimed takes everything down again.
 *
 * Return: 0, or -1 with the cause logged.
 */
int rp_hw_up(struct rp_hw *hw, struct RpividBase *base)
{
	struct ExecBase *SysBase = base->SysBase;

	memset(hw, 0, sizeof(*hw));
	hw->base = base;
	hw->dev.SysBase = SysBase;
	InitSemaphore(&hw->queue_lock);
	drv_waitq_init(&hw->waitq);
	_NewMinList(&hw->queue);
	_NewMinList(&hw->phase1_done);

	if (!pool_make(base))
		return -1;
	hw->dev.pool = base->pool;
	if (!block_locate(hw))
		return -1;
	if (!hw_claim_take(SysBase, &hw->claim, (CONST_STRPTR)RP_HEVC_SYMBOL))
	{
		Kprintf("[rpivid] the block is another driver's\n");
		return -1;
	}
	hw->claimed = TRUE;
	clock_on(hw);

	u32 version = apb_read(&hw->dev, RPI_VERSION);
	if (version != HEVC_D_VERSION)
		Kprintf("[rpivid] version %08lx, not %08lx\n", (ULONG)version, (ULONG)HEVC_D_VERSION);
	if (version != HEVC_D_VERSION || !block_serve(hw))
	{
		rp_hw_down(hw);
		return -1;
	}
	return 0;
}

/**
 * rp_hw_down() - The inverse of rp_hw_up(), as far as it got; nothing may be in the block.
 * @hw: the hardware layer's state
 *
 * The task joined, the server removed, the register and the clock as found,
 * the slots freed, the claim released, the pool shrunk.
 */
void rp_hw_down(struct rp_hw *hw)
{
	struct RpividBase *base = hw->base;
	struct ExecBase *SysBase = base->SysBase;
	APTR MailboxBase = hw->MailboxBase;

	if (hw->server_added)
	{
		RemIntServerEx((ULONG)hw->irq, &hw->server);
		hw->server_added = FALSE;
	}
	drv_task_join(SysBase, &hw->task);
	if (hw->gic400Base != NULL)
	{
		CloseLibrary(hw->gic400Base);
		hw->gic400Base = NULL;
	}
	if (hw->dev.base_irq != NULL && hw->claimed)
		irq_write(&hw->dev, ARG_IC_ICTRL, hw->ictrl_found | ARG_IC_ICTRL_ALL_IRQ_MASK);
	if (hw->claimed && MailboxBase != NULL && (hw->clock_state_found & CLOCK_STATE_ON) == 0)
		MB_SetClockState(RP_HEVC_CLOCK, 0);
	for (unsigned int i = 0; i < RP_SLOTS; i++)
	{
		hwbuf_free(&hw->dev, &hw->slots[i].coeff);
		hwbuf_free(&hw->dev, &hw->slots[i].pu);
	}
	if (hw->claimed)
	{
		hw_claim_release(SysBase, &hw->claim);
		hw->claimed = FALSE;
	}
	drv_waitq_cleanup(&hw->waitq, SysBase);
	if (base->pool != NULL)
		dma_pool_shrink(base->pool);
	hw->dev.base_irq = NULL; /* the reset guard's prepare then touches nothing */
	hw->dev.base_h265 = NULL;
}

/**
 * rp_hw_slots_want() - The slots are to hold a picture of this size.
 * @hw:     the hardware layer's state
 * @width:  the coded width
 * @height: the coded height
 *
 * Linux's sizes: the coefficients a picture's area, the prediction units a
 * quarter of it, each rounded up.  The task grows a slot that is short
 * before it uses it (rp_hw_task()), so this only raises the wish.
 */
void rp_hw_slots_want(struct rp_hw *hw, u32 width, u32 height)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	size_t wxh = (size_t)width * height;
	size_t coeff = hevc_d_round_up_size(wxh);
	size_t pu = hevc_d_round_up_size(wxh / 4);
	ObtainSemaphore(&hw->queue_lock);
	if (coeff > hw->slot_coeff_want)
		hw->slot_coeff_want = coeff;
	if (pu > hw->slot_pu_want)
		hw->slot_pu_want = pu;
	ReleaseSemaphore(&hw->queue_lock);
}

/**
 * rp_hw_queue() - A picture, its command list built, for the block.
 * @hw: the hardware layer's state
 * @p:  the picture: ctx, de, owner and retire set
 *
 * Return: 0, or -1 when the block is wedged or halted (the picture is not taken).
 */
int rp_hw_queue(struct rp_hw *hw, struct rp_picture *p)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	ObtainSemaphore(&hw->queue_lock);
	if (hw->wedged || hw->halted)
	{
		ReleaseSemaphore(&hw->queue_lock);
		return -1;
	}
	p->state = RP_PIC_QUEUED;
	p->slot = NULL;
	p->reruns = 0;
	AddTail((struct List *)&hw->queue, (struct Node *)&p->node);
	ReleaseSemaphore(&hw->queue_lock);
	Signal(hw->task, hw->sig_queue);
	return 0;
}

/**
 * rp_hw_slot_release() - A slot is free again; waiters for a quiet block are woken.
 * @hw:   the hardware layer's state, queue_lock held
 * @slot: the slot
 */
void rp_hw_slot_release(struct rp_hw *hw, struct rp_slot *slot)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	slot->picture = NULL;
	drv_wake_up_all(&hw->waitq, SysBase);
}

/**
 * rp_hw_withdraw() - An owner's pictures not yet started leave the queue (a flush or close).
 * @hw:    the hardware layer's state
 * @owner: whose
 *
 * They are retired as FAILED, through their retire hook, with the queue lock
 * not held.  One back in the queue for a phase-1 rerun still holds its slot,
 * which is released here: no phase runs in it then.  What is in a slot for a
 * running phase stays until the block is done with it: rp_hw_wait_quiet()
 * waits for that.
 */
void rp_hw_withdraw(struct rp_hw *hw, APTR owner)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	struct MinList taken;
	_NewMinList(&taken);
	ObtainSemaphore(&hw->queue_lock);
	struct rp_picture *p = (struct rp_picture *)hw->queue.mlh_Head;
	while (p->node.mln_Succ != NULL)
	{
		struct rp_picture *next = (struct rp_picture *)p->node.mln_Succ;
		if (p->owner == owner)
		{
			Remove((struct Node *)&p->node);
			if (p->slot != NULL)
			{
				rp_hw_slot_release(hw, p->slot);
				p->slot = NULL;
			}
			AddTail((struct List *)&taken, (struct Node *)&p->node);
		}
		p = next;
	}
	ReleaseSemaphore(&hw->queue_lock);
	while ((p = (struct rp_picture *)RemHead((struct List *)&taken)) != NULL)
	{
		p->state = RP_PIC_FAILED;
		p->retire(p);
	}
}

/**
 * owner_quiet() - Whether none of an owner's pictures holds a slot (drv_wait_event()'s predicate).
 * @state: the hardware layer's state
 * @owner: the owner
 *
 * Return: TRUE when quiet; called with queue_lock held.
 */
static BOOL owner_quiet(APTR state, APTR owner)
{
	struct rp_hw *hw = state;
	for (unsigned int i = 0; i < RP_SLOTS; i++)
		if (hw->slots[i].picture != NULL && hw->slots[i].picture->owner == owner)
			return FALSE;
	return TRUE;
}

/**
 * rp_hw_wait_quiet() - Wait until none of an owner's pictures is in the block.
 * @hw:         the hardware layer's state
 * @owner:      whose
 * @timeout_ms: how long at most
 *
 * Return: DRV_WAIT_OK, DRV_WAIT_TIMEOUT or DRV_WAIT_NO_RESOURCES.
 */
LONG rp_hw_wait_quiet(struct rp_hw *hw, APTR owner, ULONG timeout_ms)
{
	struct ExecBase *SysBase = hw->base->SysBase;
	ObtainSemaphore(&hw->queue_lock);
	LONG rc = drv_wait_event(&hw->waitq, SysBase, &hw->queue_lock, owner_quiet, hw, owner, timeout_ms);
	ReleaseSemaphore(&hw->queue_lock);
	return rc;
}

/**
 * rp_hw_reset_prepare() - Before a reset: the block stops writing (the reset guard's prepare).
 * @user: the hardware layer's state
 *
 * Interrupt-safe and short: nothing starts any more, the interrupt enables go
 * off, and the block's two activity lines are given a few milliseconds to
 * drop, so it writes nothing into memory the next AmigaOS reuses.
 */
void rp_hw_reset_prepare(APTR user)
{
	struct rp_hw *hw = user;
	hw->halted = TRUE;
	if (hw->dev.base_irq == NULL)
		return;
	irq_write(&hw->dev, ARG_IC_ICTRL, 0);
	u32 t0 = get_time();
	while ((irq_read(&hw->dev, ARG_IC_ICTRL) & (ARG_IC_ICTRL_ACTIVE1_STATUS_SET | ARG_IC_ICTRL_ACTIVE2_STATUS_SET)) != 0 &&
	       get_time() - t0 < PREPARE_WAIT_US)
		delay_us(PREPARE_POLL_US);
}
