#ifndef FDOS_H
#define FDOS_H

#include <stdbool.h>
#include "i386.h"

#ifndef __hfa_func
#define __hfa(group) __attribute__((section(".hfa." group)))
#define __hfa_func(func_name) func_name
// __hfa(__STRING(func_name))
#endif

void _boot(CPU*); // like kernel/boot/boot.asm
void kernel(CPU*); // like kernel/kernel

#define dos_puts(x) bios_puts(cpu, x)
void dos_printf(const char *fmt, ...);

bool fdos_20h(CPU*); // OLD-STYLE (CP/M) TERMINATE
bool fdos_21h(CPU*); // MAIN DOS HANDLER
bool fdos_25h(CPU*); // ABSOLUTE DISK READ
bool fdos_26h(CPU*); // ABSOLUTE DISK WRITE
bool fdos_27h(CPU*); // OLD-STYLE TERMINATE AND STAY RESIDENT
bool fdos_28h(CPU*); // DOS IDLE
bool fdos_2fh(CPU*); // XMS
bool fdos_29h(CPU*); // FAST CONSOLE OUTPUT
bool fdos_30h(CPU*); // CP/M CALL-5 FAR-CALL ENTRY

void cpu_far_call(CPU* cpu, uint16_t seg, uint16_t off);

#endif // FBIOS_H
