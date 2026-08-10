#ifndef __NATIVE_DOS_API__
#define __NATIVE_DOS_API__

#ifndef DOS_API_VERSION
#define DOS_API_VERSION (0)
#endif

#ifndef DOS_OS_API_SYS_TABLE_BASE
#define DOS_OS_API_SYS_TABLE_BASE ((void*)(0x10100000ul))
static const unsigned long* const _sys_table_ptrs = (const unsigned long* const)DOS_OS_API_SYS_TABLE_BASE;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include "pc.h"

inline static PC* get_PC() {
    typedef PC* (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[0])();
}

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_API__ */
