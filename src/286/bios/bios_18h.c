#include "../cpu.h"
#include "../bios.h"

bool bios_18h(CPU* cpu) {
    print_line("No Basic ROM", 1);
    print_line("System halted", 2);
while(1); // remove it
    return false;
}