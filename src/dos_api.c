#include <pico.h>
#include <pc.h>
#include <bios/bios.h>

#define __in_systable(group) __attribute__((section(".dos_api" group)))

extern PC *pc;

PC* __not_in_flash_func(get_PC)() {
    return pc;
}

// To be placed on 0x10100000
unsigned long __in_systable() __aligned(4096) dos_api_table_ptrs[] = {
    (unsigned long)get_PC,
    (unsigned long)handlers,
    (unsigned long)bios_intcall,
    0
};
