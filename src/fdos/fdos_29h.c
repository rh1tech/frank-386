#include <stdio.h>
#include "286/cpu.h"
#include "bios/bios.h"
#include "fdos.h"

/*
DOS 2+ - FAST CONSOLE OUTPUT
AL = character to display
*/
bool fdos_29h(CPU* cpu) {
//    CPU_regs regs;
//    cpu_save_regs(cpu, &regs);
    bios_teletype(cpu, CPU_AH, 0);
//    CPU_AH = 0x0e;
//    CPU_BX = 0x0007;
//    bios_intcall(cpu, 0x10, "INT29h/10h");
//    cpu_restore_regs(cpu, &regs);
    return true;
}
