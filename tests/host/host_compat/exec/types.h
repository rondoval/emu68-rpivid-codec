/* SPDX-License-Identifier: GPL-2.0 */
/* exec/types.h for the host builds: the Amiga's basic types, nothing else. */
#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H
#include <stdint.h>
#include <string.h>
typedef void *APTR;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int16_t WORD;
typedef uint16_t UWORD;
typedef int8_t BYTE;
typedef uint8_t UBYTE;
typedef int16_t BOOL;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;
#define TRUE 1
#define FALSE 0
#endif
