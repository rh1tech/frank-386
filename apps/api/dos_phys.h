#ifndef __NATIVE_DOS_PHYS_H__
#define __NATIVE_DOS_PHYS_H__

#include <stdint.h>

#ifndef DOS_OS_API_SYS_TABLE_BASE
#define DOS_OS_API_SYS_TABLE_BASE ((void*)(0x10100000ul))
#endif

static const unsigned long* const _dos_phys_sys_table_ptrs =
    (const unsigned long* const)DOS_OS_API_SYS_TABLE_BASE;

#ifdef __cplusplus
extern "C" {
#endif

inline static uint8_t dos_phys_read8(uint32_t addr) {
    typedef uint8_t (*fn_ptr_t)(uint32_t);
    return ((fn_ptr_t)_dos_phys_sys_table_ptrs[3])(addr);
}

inline static uint16_t dos_phys_read16(uint32_t addr) {
    typedef uint16_t (*fn_ptr_t)(uint32_t);
    return ((fn_ptr_t)_dos_phys_sys_table_ptrs[4])(addr);
}

inline static uint32_t dos_phys_read32(uint32_t addr) {
    typedef uint32_t (*fn_ptr_t)(uint32_t);
    return ((fn_ptr_t)_dos_phys_sys_table_ptrs[5])(addr);
}

inline static void dos_phys_write8(uint32_t addr, uint8_t val) {
    typedef void (*fn_ptr_t)(uint32_t, uint8_t);
    ((fn_ptr_t)_dos_phys_sys_table_ptrs[6])(addr, val);
}

inline static void dos_phys_write16(uint32_t addr, uint16_t val) {
    typedef void (*fn_ptr_t)(uint32_t, uint16_t);
    ((fn_ptr_t)_dos_phys_sys_table_ptrs[7])(addr, val);
}

inline static void dos_phys_write32(uint32_t addr, uint32_t val) {
    typedef void (*fn_ptr_t)(uint32_t, uint32_t);
    ((fn_ptr_t)_dos_phys_sys_table_ptrs[8])(addr, val);
}

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_PHYS_H__ */
