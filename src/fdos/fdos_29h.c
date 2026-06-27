#include <stdio.h>
#include "286/cpu.h"
#include "bios/bios.h"
#include "fdos.h"

/*
DOS 2+ - FAST CONSOLE OUTPUT
AL = character to display
*/
bool fdos_29h(CPU* cpu) {
    u16 ax = CPU_AX;
    u16 bx = CPU_BX;
    CPU_AH = 0x0e;
    CPU_BX = 0x0007;
    bios_10h(cpu);
    CPU_AX = ax;
    CPU_BX = bx;
    return true;
}
