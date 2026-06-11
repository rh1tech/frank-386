#include "i286.h"
#include "bios.h"

/*
BIOS - GET MEMORY SIZE
Return:
AX = kilobytes of contiguous memory starting at absolute address 00000h

Note: This call returns the contents of the word at 0040h:0013h; in PC and XT, this value is set from the switches on the motherboard
*/
bool bios_12h(void) {
    CPU_AX = pload16(0x413);
    return true;
}
