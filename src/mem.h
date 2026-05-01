#include <stdint.h>
#include <stdbool.h>
#include "ems.h"

#if EMULATE_LTEMS
#define IF_EMS(x) x
#else
#define IF_EMS(x)
#endif

#define PC_RAM ((uint8_t*)0x11000000)
#define PC_RAM32 ((uint32_t*)0x11000000)
#define CHECH_RAM_BOARDER_ENABLED 1
extern unsigned long phys_mem_size;
extern void* g_pc;

#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)

#define VGA_WINDOW(addr) (((addr - 0xa0000u) < 0x20000u) | (addr >= 0xe0000000u))

uint8_t iomem_read8(void* iomem, uint32_t addr);
uint16_t iomem_read16(void* iomem, uint32_t addr);
uint32_t iomem_read32(void* iomem, uint32_t addr);
void iomem_write8(void *iomem, uint32_t addr, uint8_t val);
void iomem_write16(void *iomem, uint32_t addr, uint16_t val);
void iomem_write32(void *iomem, uint32_t addr, uint32_t val);
bool iomem_write_string(void *iomem, uint32_t addr, uint32_t buf, int len);
bool iomem_write_string_ptr(void *iomem, uint32_t addr, const uint8_t *buf, int len);

static inline uint8_t __attribute__((always_inline)) pload8(uint32_t addr)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_read8(g_pc, addr);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        return *ems_host_ptr(addr);
    }
#endif
#if CHECH_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return 0xFF;
	}
#endif
	return PC_RAM[addr];
}

static inline uint16_t __attribute__((always_inline)) pload16(uint32_t addr)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_read16(g_pc, addr);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        return *(uint16_t*)ems_host_ptr(addr);
    }
#endif
#if CHECH_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return 0xFFFF;
	}
#endif
	return *(uint16_t*)(PC_RAM + addr);
}

static inline uint32_t __attribute__((always_inline)) pload32(uint32_t addr)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_read32(g_pc, addr);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        return *(uint32_t*)ems_host_ptr(addr);
    }
#endif
#if CHECH_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return 0xFFFFFFFF;
	}
#endif
	return *(uint32_t*)(PC_RAM + addr);
}

static inline void __attribute__((always_inline)) pstore8(uint32_t addr, uint8_t val)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_write8(g_pc, addr, val);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        *ems_host_ptr(addr) = val;
        return;
    }
#endif
#if CHECH_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return;
	}
#endif
	PC_RAM[addr] = val;
}

static inline void __attribute__((always_inline)) pstore16(uint32_t addr, uint16_t val)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_write16(g_pc, addr, val);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        *(uint16_t*)ems_host_ptr(addr) = val;
        return;
    }
#endif
#if CHECH_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return;
	}
#endif
	*(uint16_t*)(PC_RAM + addr) = val;
}

static inline void __attribute__((always_inline)) pstore32(uint32_t addr, uint32_t val)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_write32(g_pc, addr, val);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        *(uint32_t*)ems_host_ptr(addr) = val;
        return;
    }
#endif
#if CHECH_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return;
	}
#endif
	*(uint32_t*)(PC_RAM + addr) = val;
}

static inline bool __attribute__((always_inline))
pstore_block(uint32_t dst, uint32_t src, int len)
{
#if EMULATE_LTEMS
    bool src_ems = unlikely(EMS_WINDOW(src) || EMS_WINDOW(src + len - 1));
    bool dst_ems = unlikely(EMS_WINDOW(dst) || EMS_WINDOW(dst + len - 1));

    if (src_ems || dst_ems) {
        if (src_ems && VGA_WINDOW(dst)) {
            /* EMS → VGA: собираем блок из EMS и отправляем через ptr-вариант */
            /* Блок гарантированно выровнен и помещается в одну EMS-страницу
             * (гарантируется вызывающим через count из MOVS_helper2),
             * поэтому ems_host_ptr(src) даёт непрерывный буфер */
            return iomem_write_string_ptr(g_pc, dst, ems_host_ptr(src), len);
        }
        /* EMS ↔ RAM или EMS ↔ EMS: word-wide через pload32/pstore32 */
        while (len > 0 && (dst & 3)) {
            pstore8(dst++, pload8(src++));
            len--;
        }
        int n32 = len >> 2;
        while (n32--) {
            pstore32(dst, pload32(src));
            dst += 4; src += 4;
        }
        len &= 3;
        while (len--) pstore8(dst++, pload8(src++));
        return true;
    }
#endif

    if (likely(!VGA_WINDOW(dst) && !VGA_WINDOW(dst + len - 1))) {
#if CHECH_RAM_BOARDER_ENABLED
        if (unlikely((uint32_t)(dst + len) > phys_mem_size)) return false;
#endif
        const uint8_t *s = PC_RAM + src;
        uint8_t       *d = PC_RAM + dst;
        int n32 = len >> 2;
        int rem = len & 3;
        const uint32_t *s32 = (const uint32_t *)s;
        uint32_t       *d32 = (uint32_t *)d;
        while (n32--) *d32++ = *s32++;
        s = (const uint8_t *)s32;
        d = (uint8_t *)d32;
        while (rem--) *d++ = *s++;
        return true;
    }

    /* RAM → VGA */
    return iomem_write_string(g_pc, dst, src, len);
}
