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

/* Physical base of EMS storage in PSRAM — must match ems.c.inl */
#define EMS_PSRAM_OFFSET ((EMU_MEM_SIZE_MB * 1024 - 2048ul) << 10)
#define EMS_BASE_PTR     ((uint8_t *)0x11000000 + EMS_PSRAM_OFFSET)

/* Guest physical window occupied by the four 16-KB EMS frames */
#define EMS_START  (0xD0000ul)
#define EMS_END    (0xE0000ul)

/* Page-selector array — defined once in pc.c */
extern uint8_t ems_pages[4];

/* True when guest physical address falls inside the EMS window */
static inline int ems_in_window(uint32_t addr)
{
    return (addr - EMS_START) < (EMS_END - EMS_START);
}

/* Translate a guest-physical address inside the EMS window to a host pointer */
static inline uint8_t *ems_host_ptr(uint32_t addr)
{
    uint32_t offset    = addr - EMS_START;          /* 0x000..0xFFFF */
    uint8_t  selector  = ems_pages[(offset >> 14) & 3];
    uint32_t page_off  = offset & 0x3FFF;
    return EMS_BASE_PTR + (uint32_t)selector * 0x4000u + page_off;
}

/*
 * ems_copy_to_guest / ems_copy_from_guest
 *
 * Safe replacements for the raw  memcpy(phys_mem+addr, src, len)  pattern
 * used in disk and DMA code.  If [addr, addr+len) is entirely inside the EMS
 * window the copy goes to/from EMS PSRAM.  If it is entirely outside, the
 * caller's phys_mem pointer is used directly (fast path).  Crossing the
 * boundary is handled byte-by-byte so correctness is preserved even for
 * transfers that straddle 0xD0000 or 0xE0000 (unusual but legal).
 *
 * phys_mem  — cpu_get_phys_mem() result
 * guest     — guest physical destination/source address
 * buf       — host buffer
 * len       — byte count
 */
static inline void ems_copy_to_guest(uint8_t *phys_mem, uint32_t guest,
                                     const uint8_t *buf, uint32_t len)
{
    /* Fast path: entirely outside EMS window */
    if (!ems_in_window(guest) && !ems_in_window(guest + len - 1)) {
        memcpy(phys_mem + guest, buf, len);
        return;
    }
    /* General path: byte-by-byte with per-byte window test */
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = guest + i;
        if (ems_in_window(a))
            *ems_host_ptr(a) = buf[i];
        else
            phys_mem[a] = buf[i];
    }
}

static inline void ems_copy_from_guest(uint8_t *phys_mem, uint32_t guest,
                                       uint8_t *buf, uint32_t len)
{
    /* Fast path: entirely outside EMS window */
    if (!ems_in_window(guest) && !ems_in_window(guest + len - 1)) {
        memcpy(buf, phys_mem + guest, len);
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = guest + i;
        buf[i] = ems_in_window(a) ? *ems_host_ptr(a) : phys_mem[a];
    }
}

/*
 * ems_verify_guest — compare guest memory to buf, return 0 on mismatch.
 * Replaces the direct disk_mem[addr] reads in the INT 13h verify path.
 */
static inline int ems_verify_guest(uint8_t *phys_mem, uint32_t guest,
                                   const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = guest + i;
        uint8_t v = ems_in_window(a) ? *ems_host_ptr(a) : phys_mem[a];
        if (v != buf[i]) return 0;
    }
    return 1;
}

#else /* !EMULATE_LTEMS — compile away to nothing */

static inline int  ems_in_window(uint32_t addr)               { (void)addr; return 0; }
static inline void ems_copy_to_guest(uint8_t *m, uint32_t g,
                                     const uint8_t *b, uint32_t l)
                                     { memcpy(m + g, b, l); }
static inline void ems_copy_from_guest(uint8_t *m, uint32_t g,
                                       uint8_t *b, uint32_t l)
                                       { memcpy(b, m + g, l); }
static inline int  ems_verify_guest(uint8_t *m, uint32_t g,
                                    const uint8_t *b, uint32_t l)
{
    return memcmp(m + g, b, l) == 0;
}

#endif /* EMULATE_LTEMS */
