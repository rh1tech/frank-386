#ifndef __NATIVE_DOS_API__
#define __NATIVE_DOS_API__

#include "dos_api_version.h"
#include "dos_guest_ptr.h"

#ifndef DOS_OS_API_SYS_TABLE_BASE
#define DOS_OS_API_SYS_TABLE_BASE ((void*)(0x10100000ul))
#endif
static const unsigned long* const _sys_table_ptrs = (const unsigned long* const)DOS_OS_API_SYS_TABLE_BASE;

#ifdef __cplusplus
extern "C" {
#endif

#include "pc.h"

inline static PC* get_PC() {
    typedef PC* (*fn_ptr_t)();
    return ((fn_ptr_t)_sys_table_ptrs[0])();
}

inline static void bios_intcall(CPU* cpu, uint8_t intnum, const char* owner) {
    typedef void (*fn_ptr_t)(CPU*, uint8_t, const char*);
    ((fn_ptr_t)_sys_table_ptrs[2])(cpu, intnum, owner);
}


#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_API__ */
