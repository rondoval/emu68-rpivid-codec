/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linux_compat.h - what the kept Linux code takes from the kernel: the types
 * and the dozen macros (docs/architecture.md section 7.2).  The kept code is
 * hevc_d_h265.c's functions and the upstream blocks cmdgen.c carries (the
 * context's sizes, a slice's data length, the PU and coefficient strides);
 * ALIGN(), ALIGN_DOWN() and DIV_ROUND_UP_POW2() serve those blocks alone.
 * Nothing of the V4L2 framework; that was replaced by cmdgen.c.
 */
#ifndef LINUX_COMPAT_H
#define LINUX_COMPAT_H

#include <proto/exec.h> /* first in hevc_d_h265.c's translation unit: cmdgen.c's cache_ops.h, at its end, wants it before */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <bits.h>      /* BIT(), ALIGN_UP(), DIV_CEIL(), CONTAINER_OF(), round_up_pow2_u32() */
#include <byteorder.h> /* le64() */
#include <debug.h>     /* Kprintf() */
#include <errors.h>    /* ENOMEM, EINVAL */
#include <memory.h>    /* memcpy(), memset(), memcmp() */
#include <types.h>     /* u8 to u64, s32, dma_addr_t */

#include <v4l2_hevc_ctrls.h> /* the picture record */

/* arithmetic */
#define ALIGN(x, a) ALIGN_UP((x), (a))
#define ALIGN_DOWN(x, a) ((x) & ~((typeof(x))(a) - 1))
#define DIV_ROUND_UP_POW2(n, d) DIV_CEIL((n), (d))
#define roundup_pow_of_two(x) round_up_pow2_u32(x)
/* the second operand taken in the first's type: the kept code mixes an unsigned with a small positive int */
#define min(a, b) ({ typeof(a) _mna = (a); typeof(a) _mnb = (typeof(a))(b); _mna < _mnb ? _mna : _mnb; })
#define max(a, b) ({ typeof(a) _mxa = (a); typeof(a) _mxb = (typeof(a))(b); _mxa > _mxb ? _mxa : _mxb; })
#define clamp(x, lo, hi) min(max((x), (lo)), (hi))

/*
 * The command list: the block reads it as 64-bit little-endian words, so the
 * one store of p1_apb_write() swaps.  This is the only byte-order work in the
 * kept code; the values it composes are values.
 */
#define WRITE_ONCE(x, val) ((x) = le64((u64)(val)))

/*
 * Memory for the CTB address maps and tile boundaries: updated_ps() and
 * free_ps_info() allocate and free with the decode state at hand as `s`, and
 * the state knows its context (hevc_d_ctx_of(), cmdgen.c).  Linux's
 * kmalloc_array() answers NULL when n * size overflows; these callers cannot
 * overflow (at most 65536 CTBs, or a tile column or row count, of 2 or 4
 * bytes), so the product is taken as it is.
 */
#define kmalloc_array(n, size, flags) hevc_d_kmalloc(hevc_d_ctx_of(s), (n) * (size))
#define kfree(p) hevc_d_kfree(hevc_d_ctx_of(s), (p))
#define GFP_KERNEL 0 /* the kept calls pass it; the allocator takes no flags */

/* logging: the first arguments, the level and the V4L2 device, are dropped unread */
#define v4l2_err(dev, fmt, ...) Kprintf("[rpivid] " fmt, ##__VA_ARGS__)          /* the debug tier */
#define hevc_d_dbg(level, dev, fmt, ...) KprintfT("[rpivid] " fmt, ##__VA_ARGS__) /* the trace tier */

#endif /* LINUX_COMPAT_H */
