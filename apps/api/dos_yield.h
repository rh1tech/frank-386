#ifndef __NATIVE_DOS_YIELD_H__
#define __NATIVE_DOS_YIELD_H__

#include <stdint.h>

#ifndef DOS_OS_API_SYS_TABLE_BASE
#define DOS_OS_API_SYS_TABLE_BASE ((void *)(0x10100000ul))
#endif

/*
 * Cooperative native-ELF service point, system-table index 12 (API v5).
 * Services emulator devices without executing guest CPU code and returns the
 * current microsecond tick counter used by the emulator.
 */
static const unsigned long * const _dos_yield_sys_table_ptrs =
    (const unsigned long * const)DOS_OS_API_SYS_TABLE_BASE;

static inline uint32_t dos_yield(void)
{
    typedef uint32_t (*fn_ptr_t)(void);
    return ((fn_ptr_t)_dos_yield_sys_table_ptrs[12])();
}

#endif /* __NATIVE_DOS_YIELD_H__ */
