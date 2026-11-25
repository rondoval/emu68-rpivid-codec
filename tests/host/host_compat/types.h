/* SPDX-License-Identifier: GPL-2.0 */
/* types.h for the host builds: emu68-common's fixed-width names. */
#ifndef HOST_TYPES_H
#define HOST_TYPES_H
#include <stdint.h>
#include <exec/types.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t s64;
typedef int32_t s32;
typedef int16_t s16;
typedef int8_t s8;
typedef ULONG dma_addr_t;
#endif
