#include <stdio.h>
#include "../cpu.h"
#include "../bios.h"
#include "../fdos.h"

/*
DOS 2+ - FAST CONSOLE OUTPUT
AL = character to display
*/
bool fdos_29h(CPU* cpu) {
    u8 ah = CPU_AH;
    u16 bx = CPU_BX;
    CPU_AH = 0x0e;
    CPU_BX = 0x00f0;
    bios_10h(cpu);
    CPU_AH = ah;
    CPU_BX = bx;
    return true;
}
