#ifndef GUEST_MEM_H
#define GUEST_MEM_H

#include <stddef.h>
#include <stdint.h>
#include "ems.h"

extern uint8_t *guest_phys_mem;
extern unsigned long guest_phys_mem_size;
extern void *guest_iomem;

uint8_t iomem_read8(void *iomem, uint32_t addr);
void iomem_write8(void *iomem, uint32_t addr, uint8_t val);

static inline int guest_is_iomem(uint32_t addr)
{
    return ((addr - 0xA0000u) < 0x20000u) || addr >= 0xE0000000u;
}

static inline uint8_t guest_load8(uint32_t addr)
{
    if (guest_is_iomem(addr))
        return iomem_read8(guest_iomem, addr);
#if EMULATE_LTEMS
    if (ems_in_window(addr))
        return *ems_host_ptr(addr);
#endif
    if (addr >= guest_phys_mem_size)
        return 0xFF;
    return guest_phys_mem[addr];
}

static inline void guest_store8(uint32_t addr, uint8_t val)
{
    if (guest_is_iomem(addr)) {
        iomem_write8(guest_iomem, addr, val);
        return;
    }
#if EMULATE_LTEMS
    if (ems_in_window(addr)) {
        *ems_host_ptr(addr) = val;
        return;
    }
#endif
    if (addr < guest_phys_mem_size)
        guest_phys_mem[addr] = val;
}

static inline void guest_read_block(uint32_t src, void *dst, size_t len)
{
    uint8_t *out = (uint8_t *)dst;
    while (len--)
        *out++ = guest_load8(src++);
}

static inline void guest_write_block(uint32_t dst, const void *src, size_t len)
{
    const uint8_t *in = (const uint8_t *)src;
    while (len--)
        guest_store8(dst++, *in++);
}

#endif /* GUEST_MEM_H */
