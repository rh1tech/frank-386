#include <stdint.h>

#define PC_RAM ((uint8_t*)0x11000000)
#define PC_RAM32 ((uint32_t*)0x11000000)
extern unsigned long phys_mem_size;
extern void* g_pc;

#define unlikely(x) __builtin_expect(!!(x), 0)

#define VGA_WINDOW(addr) (((addr - 0xa0000u) < 0x20000u) | (addr >= 0xe0000000u))

uint8_t iomem_read8(void* iomem, uint32_t addr);
uint16_t iomem_read16(void* iomem, uint32_t addr);
uint32_t iomem_read32(void* iomem, uint32_t addr);
void iomem_write8(void *iomem, uint32_t addr, uint8_t val);
void iomem_write16(void *iomem, uint32_t addr, uint16_t val);
void iomem_write32(void *iomem, uint32_t addr, uint32_t val);

static inline uint8_t __attribute__((always_inline)) pload8(uint32_t addr)
{
#if CHECH_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return 0xFF;
	}
#endif
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_read8(g_pc, addr);
    }
	return PC_RAM[addr];
}

static inline uint16_t __attribute__((always_inline)) pload16(uint32_t addr)
{
#if CHECH_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return 0xFFFF;
	}
#endif
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_read16(g_pc, addr);
    }
	return *(uint16_t *)(PC_RAM + addr);
}

static inline uint32_t __attribute__((always_inline)) pload32(uint32_t addr)
{
#if CHECH_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return 0xFFFFFFFF;
	}
#endif
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_read32(g_pc, addr);
    }
	return *(uint32_t *)(PC_RAM + addr);
}

static inline void __attribute__((always_inline)) pstore8(uint32_t addr, uint8_t val)
{
#if CHECH_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return;
	}
#endif
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_write8(g_pc, addr, val);
    }
	PC_RAM[addr] = val;
}

static inline void __attribute__((always_inline)) pstore16(uint32_t addr, uint16_t val)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_write16(g_pc, addr, val);
    }
	*(uint16_t *)(PC_RAM + addr) = val;
}

static inline void __attribute__((always_inline)) pstore32(uint32_t addr, uint32_t val)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_write32(g_pc, addr, val);
    }
	*(uint32_t *)(PC_RAM + addr) = val;
}
