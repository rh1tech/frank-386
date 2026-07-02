#include "286/cpu.h"
#include "bios/bios.h"
#include "fdos.h"

static bool no_handler(CPU* cpu) {
    cpu_err_msg(cpu, "DOS 2FH - ERROR: no handler defined ");
while(1); // remove it
    return true;
}

bool fdos_2fh(CPU* cpu) {
    if (CPU_AX == 0x4300)
        CPU_AL = 0x80;
    else 
        no_handler(cpu);
    return true;
}
