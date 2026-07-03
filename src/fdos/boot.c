#include <stdio.h>
#include "286/cpu.h"
#include "fdos.h"

// like kernel/boot/boot.asm
// without redundant actions
void _boot(CPU* cpu) {
   CPU_BP = 0x7C00;
   SET_SS (0x1FE0);
   CPU_SP = CPU_BP - 0x60;
   u16 drive = CPU_DL;
   CPU_BL = drive; // FreeDOS expects drive there
   // Native FreeDos handlers
   cpu_install_dos_handlers(cpu);
}
