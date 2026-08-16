#ifndef __NATIVE_DOS_GP_API__
#define __NATIVE_DOS_GP_API__

/* Native ARM applications execute directly from the emulator's guest RAM.
   Keep the mapping in the public ABI header so code which needs to inspect
   standard DOS structures (PSP, DTA, etc.) does not duplicate the platform
   address literal. */
#ifndef DOS_GUEST_RAM_BASE
#define DOS_GUEST_RAM_BASE (0x11000000ul)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

inline static void* dos_guest_linear_ptr(uint32_t linear) {
    return (void*)(uintptr_t)(DOS_GUEST_RAM_BASE + linear);
}

inline static void* dos_guest_far_ptr(uint16_t seg, uint16_t off) {
    return dos_guest_linear_ptr(((uint32_t)seg << 4) + off);
}

#ifdef __cplusplus
}
#endif

#endif
