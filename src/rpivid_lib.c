/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rpivid_lib.c - resident, library init/open/close/expunge.
 *
 * The library is a videocodec backend: its function table is the backend
 * ABI (videocodec_backend.sfd).  Nothing touches the block here: the
 * hardware comes up with the first session and goes down at VCB_Idle(), the
 * frontend's last close, so a library nobody has open holds no claim, no
 * task and no interrupt server (docs/architecture.md section 5.1).
 */
#include <proto/exec.h>
#include <exec/memory.h>
#include <exec/resident.h>

#include <rpivid_private.h>

LONG __attribute__((used, section(".text.entry"))) doNotExecute(void);
/**
 * doNotExecute() - The file's first code: run as a program, it returns at once.
 *
 *
 * Return: -1.
 */
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void)
{
	return -1;
}

extern const UBYTE endOfCode;
static const char libraryName[] = LIBRARY_NAME;
static const char libraryIdString[] = LIBRARY_IDSTRING;
static const APTR initTable[4];

const struct Resident rpividResident __attribute__((used, section(".text.modhdr"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&rpividResident,
    (APTR)&endOfCode,
    RTF_AUTOINIT,
    LIBRARY_VERSION,
    NT_LIBRARY,
    LIBRARY_PRIORITY,
    (APTR)&libraryName,
    (APTR)&libraryIdString,
    (APTR)initTable,
};

/**
 * LibExpunge() - Remove the library once nothing uses it.
 * @base: the library base (A6)
 *
 * Refused (delayed) while it is open, a session is left, the hardware is up,
 * or a later patch of ColdReboot() keeps the reset guard in place.  The DMA
 * pool, made at the first hardware up, goes with the base.
 *
 * Return: the segment list to unload, 0 while refused.
 */
static ULONG LibExpunge(struct RpividBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	ULONG segList = base->segList;
	if (base->lib.lib_OpenCnt > 0 || base->session_count > 0 || base->hw_up ||
	    (base->reset_guarded && !reset_guard_remove(&base->reset_guard)))
	{
		base->lib.lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	base->reset_guarded = FALSE;
	if (base->pool != NULL)
	{
		dma_pool_delete(base->pool);
		base->pool = NULL;
	}
	Forbid();
	Remove((struct Node *)base);
	Permit();
	ULONG size = (ULONG)base->lib.lib_NegSize + base->lib.lib_PosSize;
	FreeMem((APTR)((ULONG)base - base->lib.lib_NegSize), size);
	return segList;
}

/**
 * LibInit() - Set the library base up: the lock, no sessions.
 * @lib:     the library base (D0), cleared by MakeLibrary()
 * @seglist: the segment list (A0)
 * @SysBase: exec (A6)
 *
 * Return: the base.
 */
static struct Library *LibInit(struct Library *lib asm("d0"), ULONG seglist asm("a0"), struct ExecBase *SysBase asm("a6"))
{
	struct RpividBase *base = (struct RpividBase *)lib;
	base->segList = seglist;
	base->SysBase = SysBase;
	base->lib.lib_Revision = (UWORD)LIBRARY_REVISION;
	InitSemaphore(&base->sessions_lock);
	_NewMinList(&base->sessions);
	base->block_found = RP_BLOCK_UNKNOWN;
	return lib;
}

/**
 * LibOpen() - Count an opener and cancel a delayed expunge.
 * @version: the version asked for (D0); the frontend checks the ABI instead
 * @base:    the library base (A6)
 *
 * Return: the base.
 */
static struct RpividBase *LibOpen(ULONG version asm("d0"), struct RpividBase *base asm("a6"))
{
	(void)version;
	base->lib.lib_OpenCnt++;
	base->lib.lib_Flags &= (UBYTE)~LIBF_DELEXP;
	return base;
}

/**
 * LibClose() - Count an opener off; the last one runs a delayed expunge.
 * @base: the library base (A6)
 *
 * Return: the segment list if the library was expunged, else 0.
 */
static ULONG LibClose(struct RpividBase *base asm("a6"))
{
	base->lib.lib_OpenCnt--;
	if (base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP))
		return LibExpunge(base);
	return 0;
}

/**
 * LibNull() - The reserved vector.
 *
 * Return: 0.
 */
static ULONG LibNull(void)
{
	return 0;
}

static const APTR funcTable[] = {
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibNull,
    (APTR)VCB_GetInfo,
    (APTR)VCB_QueryCodec,
    (APTR)VCB_OpenSession,
    (APTR)VCB_CloseSession,
    (APTR)VCB_ImportPacket,
    (APTR)VCB_ImportFrame,
    (APTR)VCB_SendPacket,
    (APTR)VCB_SendFrame,
    (APTR)VCB_Poll,
    (APTR)VCB_ReleaseFrame,
    (APTR)VCB_ReleasePacket,
    (APTR)VCB_Flush,
    (APTR)VCB_SetAttrs,
    (APTR)VCB_Image,
    (APTR)VCB_Idle,
    (APTR)VCB_OutputFrame,
    (APTR)-1,
};

static const APTR initTable[4] = {
    (APTR)sizeof(struct RpividBase),
    (APTR)funcTable,
    NULL,
    (APTR)LibInit,
};
