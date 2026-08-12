#include <pico.h>
#include <pc.h>
#include <bios/bios.h>
#include "mem.h"

#define __in_systable(group) __attribute__((section(".dos_api" group)))

extern PC *pc;

PC* __not_in_flash_func(get_PC)() {
    return pc;
}

static uint8_t __not_in_flash_func(dos_phys_read8)(uint32_t addr) {
    return pload8(addr);
}

static uint16_t __not_in_flash_func(dos_phys_read16)(uint32_t addr) {
    return pload16(addr);
}

static uint32_t __not_in_flash_func(dos_phys_read32)(uint32_t addr) {
    return pload32(addr);
}

static void __not_in_flash_func(dos_phys_write8)(uint32_t addr, uint8_t val) {
    pstore8(addr, val);
}

static void __not_in_flash_func(dos_phys_write16)(uint32_t addr, uint16_t val) {
    pstore16(addr, val);
}

static void __not_in_flash_func(dos_phys_write32)(uint32_t addr, uint32_t val) {
    pstore32(addr, val);
}

// To be placed on 0x10100000
unsigned long __in_systable() __aligned(4096) dos_api_table_ptrs[] = {
    (unsigned long)get_PC,
    (unsigned long)handlers,
    (unsigned long)bios_intcall,
    (unsigned long)dos_phys_read8,
    (unsigned long)dos_phys_read16,
    (unsigned long)dos_phys_read32,
    (unsigned long)dos_phys_write8,
    (unsigned long)dos_phys_write16,
    (unsigned long)dos_phys_write32,
    0
};
