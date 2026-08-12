#ifndef __NATIVE_DOS_PSRAM_H__
#define __NATIVE_DOS_PSRAM_H__

#include <stdint.h>

#ifndef DOS_OS_API_SYS_TABLE_BASE
#define DOS_OS_API_SYS_TABLE_BASE ((void *)(0x10100000ul))
#endif

#ifndef PSRAM_BASE_ADDR
#define PSRAM_BASE_ADDR (0x11000000ul)
#endif

static const unsigned long * const _psram_sys_table_ptrs =
    (const unsigned long * const)DOS_OS_API_SYS_TABLE_BASE;

static inline uint32_t psram_size(void)
{
    typedef uint32_t (*fn_ptr_t)(void);
    return ((fn_ptr_t)_psram_sys_table_ptrs[9])();
}

#endif /* __NATIVE_DOS_PSRAM_H__ */
