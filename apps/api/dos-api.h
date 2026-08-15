#ifndef __NATIVE_DOS_API__
#define __NATIVE_DOS_API__

#include "dos_api_version.h"

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

inline static void bios_intcall(CPU* cpu, uint8_t intnum, const char* owner) {
    typedef void (*fn_ptr_t)(CPU*, uint8_t, const char*);
    ((fn_ptr_t)_sys_table_ptrs[2])(cpu, intnum, owner);
}

/* Native ARM applications execute directly from the emulator's guest RAM.
   Keep the mapping in the public ABI header so code which needs to inspect
   standard DOS structures (PSP, DTA, etc.) does not duplicate the platform
   address literal. */
#ifndef DOS_GUEST_RAM_BASE
#define DOS_GUEST_RAM_BASE (0x11000000ul)
#endif

inline static void* dos_guest_linear_ptr(uint32_t linear) {
    return (void*)(uintptr_t)(DOS_GUEST_RAM_BASE + linear);
}

inline static void* dos_guest_far_ptr(uint16_t seg, uint16_t off) {
    return dos_guest_linear_ptr(((uint32_t)seg << 4) + off);
}

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_API__ */
