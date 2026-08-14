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
#define DOS_DIAG_PSP_GUARD_INDEX 102u

static inline void dos_diag_set(uint32_t code)
{
    /* Diagnostics disabled. This inlines to nothing at every APP call site. */
    (void)code;
    return;

    const unsigned long *table =
        (const unsigned long *)DOS_OS_API_SYS_TABLE_BASE;
    volatile uint32_t *latch =
        (volatile uint32_t *)(uintptr_t)table[DOS_DIAG_API_INDEX];
    *latch = code;

    /*
     * While hunting the WAD/PSP corruption, probe only coarse renderer/WAD
     * markers.  Do not call into firmware for the very hot 30/31/32 renderer
     * markers; that would perturb every rendered column.
     *
     * APP remains the exact code which triggered the check.  If cu_psp no
     * longer matches the PSP which opened the WAD, the firmware guard freezes
     * core0 and KRN shows the current PSP.
     */
    {
        uint8_t family = (uint8_t)(code >> 24);
        if ((family >= 0x11u && family <= 0x14u)
            || (family >= 0x21u && family <= 0x22u)
            || family == 0x30u
            || family == 0x31u
            || family == 0x32u
            || family == 0x33u
            || family == 0x34u)
        {
            typedef void (*guard_fn_t)(unsigned);
            ((guard_fn_t)table[DOS_DIAG_PSP_GUARD_INDEX])(0x20u);
        }
    }
}

#endif /* DIAG */

#endif /* __NATIVE_DOS_DIAG_H__ */
