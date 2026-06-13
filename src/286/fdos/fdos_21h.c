#include <stdio.h>
#include "../cpu.h"
#include "../bios.h"
#include "../fdos.h"

#include "hdr/kconfig.h"
#include "hdr/portab.h"
#include "globals.h"

UWORD ASM Int21AX;
seg ASM cu_psp;

static bool no_handler(CPU* cpu) {
    print_line("DOS 21H - ERROR: no handler defined", 1);
    char buf[10];
    snprintf(buf, 10, "AX: %04xh", CPU_AX); print_line(buf, 2);
    snprintf(buf, 10, "BX: %04xh", CPU_BX); print_line(buf, 3);
    snprintf(buf, 10, "CX: %04xh", CPU_CX); print_line(buf, 4);
    snprintf(buf, 10, "DX: %04xh", CPU_DX); print_line(buf, 5);
    snprintf(buf, 10, "SI: %04xh", CPU_SI); print_line(buf, 5);
    snprintf(buf, 10, "DI: %04xh", CPU_DI); print_line(buf, 6);
    snprintf(buf, 10, "BP: %04xh", CPU_BP); print_line(buf, 7);
    /*
    snprintf(buf, 10, "DS: %04xh", CPU_DS); print_line(buf, 8);
    snprintf(buf, 10, "SS: %04xh", CPU_SS); print_line(buf, 9);
    snprintf(buf, 10, "FS: %04xh", CPU_FS); print_line(buf, 10);
    snprintf(buf, 10, "GS: %04xh", CPU_GS); print_line(buf, 11);
    snprintf(buf, 10, "ES: %04xh", CPU_ES); print_line(buf, 12);
    */
while(1); // remove it
    return true;
}

/*
DOS 1+ - main DOS handler
*/
bool fdos_21h(CPU* cpu) {
    Int21AX = CPU_AX;
    switch (CPU_AH) {
      /* Set PSP                                                      */
      case 0x50:
        cu_psp = CPU_BX;
        break;

      /* Get PSP                                                      */
      case 0x51:
      /* UNDOCUMENTED: return current psp                             */
      case 0x62:
        CPU_BX = cu_psp;

      default:
        no_handler(cpu);
    }
    return true;
}
