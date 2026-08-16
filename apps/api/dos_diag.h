#ifndef __NATIVE_DOS_DIAG_H__
#define __NATIVE_DOS_DIAG_H__

#if DIAG

#include <stdint.h>

#ifndef DOS_OS_API_SYS_TABLE_BASE
#define DOS_OS_API_SYS_TABLE_BASE ((void *)(0x10100000ul))
#endif

/*
 * API v9 diagnostic entries.
 *
 * The slot contains the address of a firmware-owned volatile uint32_t.
 * Writing it is intentionally only a native ARM memory store.  It does not
 * enter DOS, bios_intcall(), pc_service(), stdio, FatFS, or any scheduler.
 */
#define DOS_DIAG_API_INDEX       101u

static inline void dos_diag_set(uint32_t code)
{
    const unsigned long *table =
        (const unsigned long *)DOS_OS_API_SYS_TABLE_BASE;
    volatile uint32_t *latch =
        (volatile uint32_t *)(uintptr_t)table[DOS_DIAG_API_INDEX];
    *latch = code;
}

#endif /* DIAG */

#endif /* __NATIVE_DOS_DIAG_H__ */
