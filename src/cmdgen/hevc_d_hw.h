/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hevc_d_hw.h - the block's registers, and the writes the kept functions
 * make.  The register map is Linux's hevc_d_hw.h (GPL-2.0, Raspberry Pi Ltd,
 * based on the Cedrus driver), the only description of the block; the
 * accessors go through the stack's MMIO helpers, which swap for the
 * little-endian registers.
 *
 * The parts taken from Linux's hevc_dec driver are
 * Copyright (C) 2025 Raspberry Pi Ltd
 * Based on the Cedrus VPU driver, that is:
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 * (NOTICE section 1.)
 */
#ifndef HEVC_D_HW_H
#define HEVC_D_HW_H

#include <bits.h>  /* BIT() */
#include <iomem.h>

#include "hevc_d.h"

/* struct hevc_d_dec_env carries one of these; the hardware layer here does not use it */
typedef void (*hevc_d_irq_callback)(struct hevc_d_dev *dev, void *ctx);
struct hevc_d_hw_irq_ent
{
	struct hevc_d_hw_irq_ent *next; /* upstream's list of callbacks waiting for the interrupt */
	hevc_d_irq_callback cb;         /* the callback */
	void *v;                        /* its argument */
};


/* Phase 1 register offsets */
#define RPI_SPS0 0
#define RPI_SPS1 4
#define RPI_PPS 8
#define RPI_SLICE 12
#define RPI_TILESTART 16
#define RPI_TILEEND 20
#define RPI_SLICESTART 24
#define RPI_MODE 28
#define RPI_LEFT0 32
#define RPI_LEFT1 36
#define RPI_LEFT2 40
#define RPI_LEFT3 44
#define RPI_QP 48
#define RPI_CONTROL 52
#define RPI_STATUS 56
#define RPI_VERSION 60
#define RPI_BFBASE 64
#define RPI_BFNUM 68
#define RPI_BFCONTROL 72
#define RPI_BFSTATUS 76
#define RPI_PUWBASE 80
#define RPI_PUWSTRIDE 84
#define RPI_COEFFWBASE 88
#define RPI_COEFFWSTRIDE 92
#define RPI_SLICECMDS 96
#define RPI_BEGINTILEEND 100
#define RPI_TRANSFER 104
#define RPI_CFBASE 108
#define RPI_CFNUM 112
#define RPI_CFSTATUS 116

/* Phase 2 register offsets */
#define RPI_PURBASE 0x8000
#define RPI_PURSTRIDE 0x8004
#define RPI_COEFFRBASE 0x8008
#define RPI_COEFFRSTRIDE 0x800C
#define RPI_NUMROWS 0x8010
#define RPI_CONFIG2 0x8014
#define RPI_OUTYBASE 0x8018
#define RPI_OUTYSTRIDE 0x801C
#define RPI_OUTCBASE 0x8020
#define RPI_OUTCSTRIDE 0x8024
#define RPI_STATUS2 0x8028
#define RPI_FRAMESIZE 0x802C
#define RPI_MVBASE 0x8030
#define RPI_MVSTRIDE 0x8034
#define RPI_COLBASE 0x8038
#define RPI_COLSTRIDE 0x803C
#define RPI_CURRPOC 0x8040

/* Reference frame register values: 16 of these, sequential */
#define RPI_REFYBASE0 0x9000
#define RPI_REFYSTRIDE0 0x9004
#define RPI_REFCBASE0 0x9008
#define RPI_REFCSTRIDE0 0x900c
#define RPI_REFREGS_SIZE 16 /* from REFYBASEn to REFYBASEn+1 */

/* The version register's value this driver knows */
#define HEVC_D_VERSION 0x202

/*
 * The interrupt control register (the second register block's first word).
 * The *_INT bits latch an edge of the block's activity lines (a 1 written
 * clears each); *_EN lets a latched bit out onto the interrupt line; *_STATUS
 * is the line itself; TEST_INT forces the interrupt.  Bits 19:12 and 11 are
 * reserved: written 0.
 */
#define ARG_IC_ICTRL 0
#define ARG_IC_ICTRL_ACTIVE1_INT_SET BIT(0)
#define ARG_IC_ICTRL_ACTIVE1_EDGE_SET BIT(1)
#define ARG_IC_ICTRL_ACTIVE1_EN_SET BIT(2)
#define ARG_IC_ICTRL_ACTIVE1_STATUS_SET BIT(3)
#define ARG_IC_ICTRL_ACTIVE2_INT_SET BIT(4)
#define ARG_IC_ICTRL_ACTIVE2_EDGE_SET BIT(5)
#define ARG_IC_ICTRL_ACTIVE2_EN_SET BIT(6)
#define ARG_IC_ICTRL_ACTIVE2_STATUS_SET BIT(7)
#define ARG_IC_ICTRL_TEST_INT BIT(8)
#define ARG_IC_ICTRL_SPARE BIT(9)
#define ARG_IC_ICTRL_VP9_INTERRUPT_STATUS BIT(10)
#define ARG_IC_ICTRL_AIO_INT_ENABLE BIT(20)
#define ARG_IC_ICTRL_H264_ACTIVE_INT BIT(21)
#define ARG_IC_ICTRL_H264_ACTIVE_EDGE BIT(22)
#define ARG_IC_ICTRL_H264_ACTIVE_EN BIT(23)
#define ARG_IC_ICTRL_H264_ACTIVE_STATUS BIT(24)
#define ARG_IC_ICTRL_H264_INTERRUPT_INT BIT(25)
#define ARG_IC_ICTRL_H264_INTERRUPT_EDGE BIT(26)
#define ARG_IC_ICTRL_H264_INTERRUPT_EN BIT(27)
#define ARG_IC_ICTRL_H264_INTERRUPT_STATUS BIT(28)
#define ARG_IC_ICTRL_VP9_INTERRUPT_INT BIT(29)
#define ARG_IC_ICTRL_VP9_INTERRUPT_EDGE BIT(30)
#define ARG_IC_ICTRL_VP9_INTERRUPT_EN BIT(31)
#define ARG_IC_ICTRL_SET_ZERO_MASK ((0xffu << 12) | BIT(11))
#define ARG_IC_ICTRL_ALL_IRQ_MASK \
	(ARG_IC_ICTRL_VP9_INTERRUPT_INT | ARG_IC_ICTRL_H264_INTERRUPT_INT | ARG_IC_ICTRL_ACTIVE1_INT_SET | ARG_IC_ICTRL_ACTIVE2_INT_SET)

/*
 * The phase-1 STATUS register's bits that ask for a bigger scratch buffer,
 * for hw_task.c.  The kept hevc_d_h265.c defines them too, as upstream does;
 * a definition that differed would warn, and the build allows no warnings.
 */
#define STATUS_COEFF_EXHAUSTED 8
#define STATUS_PU_EXHAUSTED 16

/**
 * apb_write() - A register written, with no barrier: the order among these does not matter.
 * @dev:    the block
 * @offset: the register's byte offset
 * @val:    the value
 *
 * Linux's writel_relaxed().  Writes to the block's registers reach it in
 * program order: Emu68 maps the /scb range as Device-nGnRE memory, as
 * Linux's ioremap() does.  Nothing reads them before the write that starts
 * the phase, whose barrier comes before it (apb_write_final()).
 */
static inline void apb_write(const struct hevc_d_dev *const dev, const unsigned int offset, const u32 val)
{
	mmio_write32_relaxed(val, (volatile u32 *)((volatile u8 *)dev->base_h265 + offset));
}

/**
 * apb_write_final() - The write that starts a phase: after every other, and after what the block reads from memory.
 * @dev:    the block
 * @offset: the register's byte offset
 * @val:    the value
 *
 * The block may start reading the command list and the buffers as soon as
 * this write lands, so they and every register write before must be done
 * first: Linux's writel() puts a barrier before the store for that, and so
 * does this one (emu68_barrier(), a NOP that Emu68 runs as dsb sy).  The
 * store itself is a full mmio_write32(), its own barrier after.
 */
static inline void apb_write_final(const struct hevc_d_dev *const dev, const unsigned int offset, const u32 val)
{
	emu68_barrier();
	mmio_write32(val, (volatile u32 *)((volatile u8 *)dev->base_h265 + offset));
}

/**
 * apb_read() - A register read.
 * @dev:    the block
 * @offset: the register's byte offset
 *
 * Return: its value.
 */
static inline u32 apb_read(const struct hevc_d_dev *const dev, const unsigned int offset)
{
	return mmio_read32((volatile u32 *)((volatile u8 *)dev->base_h265 + offset));
}

/**
 * irq_write() - A register of the interrupt control block written.
 * @dev:    the block
 * @offset: the register's byte offset in that block
 * @val:    the value
 */
static inline void irq_write(const struct hevc_d_dev *const dev, const unsigned int offset, const u32 val)
{
	mmio_write32(val, (volatile u32 *)((volatile u8 *)dev->base_irq + offset));
}

/**
 * irq_read() - A register of the interrupt control block read.
 * @dev:    the block
 * @offset: the register's byte offset in that block
 *
 * Return: its value.
 */
static inline u32 irq_read(const struct hevc_d_dev *const dev, const unsigned int offset)
{
	return mmio_read32((volatile u32 *)((volatile u8 *)dev->base_irq + offset));
}

/**
 * apb_write_vc_addr() - An address written as the block takes it, in 64-byte units.
 * @dev:    the block
 * @offset: the register's byte offset
 * @a:      the address, 64-byte aligned
 */
static inline void apb_write_vc_addr(const struct hevc_d_dev *const dev, const unsigned int offset, const dma_addr_t a)
{
	apb_write(dev, offset, (u32)(a >> HEVC_D_UNIT_SHIFT));
}

/**
 * apb_write_vc_addr_final() - An address written in 64-byte units, as the write that starts a phase (apb_write_final()).
 * @dev:    the block
 * @offset: the register's byte offset
 * @a:      the address, 64-byte aligned
 */
static inline void apb_write_vc_addr_final(const struct hevc_d_dev *const dev, const unsigned int offset, const dma_addr_t a)
{
	apb_write_final(dev, offset, (u32)(a >> HEVC_D_UNIT_SHIFT));
}

/**
 * apb_write_vc_len() - A length written as the block takes it: in 64-byte units, rounded up.
 * @dev:    the block
 * @offset: the register's byte offset
 * @x:      the length in bytes
 */
static inline void apb_write_vc_len(const struct hevc_d_dev *const dev, const unsigned int offset, const unsigned int x)
{
	apb_write(dev, offset, (x + HEVC_D_UNIT_MASK) >> HEVC_D_UNIT_SHIFT);
}

#endif /* HEVC_D_HW_H */
