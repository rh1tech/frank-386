#ifndef FDOS_H
#define FDOS_H

#include <stdbool.h>
#include "i386.h"

void _boot(CPU*); // like kernel/boot/boot.asm
void kernel(CPU*); // like kernel/kernel

#define dos_puts(x) bios_puts(cpu, x)
void dos_printf(const char *fmt, ...);

bool fdos_20h(CPU*); // OLD-STYLE (CP/M) TERMINATE
bool fdos_21h(CPU*); // MAIN DOS HANDLER
bool fdos_2fh(CPU*); // XMS
bool fdos_29h(CPU*); // FAST CONSOLE OUTPUT

void cpu_far_call(CPU* cpu, uint16_t seg, uint16_t off);

#endif // FBIOS_H
