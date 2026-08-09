#include <pico.h>
#include <i386.h>
#include <bios/bios.h>

#define __in_systable(group) __attribute__((section(".dos_api" group)))

extern CPU* cpu;

CPU* __not_in_flash_func(get_cpu)() {
    return cpu;
}

// To be placed on 0x10100000
unsigned long __in_systable() __aligned(4096) dos_api_table_ptrs[] = {
    (unsigned long)get_cpu,
    (unsigned long)bios_10h,
    0
};
