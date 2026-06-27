#include "bios.h"
#include "286/cpu.h"
#include <stdio.h>

void bios_puts(CPU* cpu, const char* str) {
    CPU_regs regs;
    cpu_save_regs(cpu, &regs);
    CPU_AH = 0x0e;
    CPU_BX = 0x0007;
    while(*str) {
        if (*str == '\n') {
            CPU_AL = '\r';
            bios_10h(cpu);
        }
        CPU_AL = *str;
        bios_10h(cpu);
        str++;
    }
    cpu_restore_regs(cpu, &regs);
}

void bios_printf(CPU* cpu, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bios_puts(cpu, buf);
}
