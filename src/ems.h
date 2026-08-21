#pragma once
/*
 * EMS (Lo-tech 2MB EMS board) shared access helpers.
 *
 * ems.c.inl is #included directly into i386.c so its symbols are local to
 * that translation unit.  This header re-exposes the same constants and a
 * small set of inline helpers for use in other TUs (disk handler, i8257 DMA).
 *
 * Include this header wherever you need to read/write guest memory that may
 * fall in the EMS window and you cannot go through the normal pload/pstore
 * path (e.g. bulk memcpy in BIOS disk callbacks or DMA engine).
 */

#include <stdint.h>
#include <string.h>

#if EMULATE_LTEMS

/* Physical base of the fixed 2 MiB EMS backing store.  Its location is
 * chosen at runtime immediately above local guest RAM, because PSRAM size is
 * auto-detected (1/2/4/8/16 MiB). */
extern uint8_t *ems_base_ptr;
#define EMS_BASE_PTR ems_base_ptr

/* Page-selector array — defined once in pc.c */
extern uint8_t ems_pages[4];

// inilined anyway
#define EMS_WINDOW(addr) ((addr - EMS_START) < (EMS_END - EMS_START))

/* Translate a guest-physical address inside the EMS window to a host pointer */
static inline uint8_t *ems_host_ptr(uint32_t addr)
{
    uint32_t offset    = addr - EMS_START;          /* 0x000..0xFFFF */
    uint8_t  selector  = ems_pages[(offset >> 14) & 3];
    uint32_t page_off  = offset & 0x3FFF;
    return EMS_BASE_PTR + (uint32_t)selector * 0x4000u + page_off;
}

#endif /* EMULATE_LTEMS */
