#include "i286.h"
#include "bios.h"

/*
PRINT SCREEN
Desc: Dump the current text screen to the first printer

Notes: Normally invoked by the INT 09 handler when PrtSc key is pressed, but may be invoked directly by applications.
Byte at 0050h:0000h contains status used by default handler 00h not active 01h PrtSc in progress FFh last PrtSc encountered error.
Default handler is at F000h:FF54h in IBM PC and 100%-compatible BIOSes.
Since the BOUND instruction also calls INT 05h, but returns control to the BOUND instruction, a failed BOUND check will cause an
infinite loop of PrtScreens unless the INT 05 handler is aware of the problem and checks whether the interrupt was invoked by a BOUND instruction
*/
bool bios_05h(void) {
    if (fake_bios_area()) { // print screen
        print_line("PRINT SCREEN", 23);
        pstore8(0x500, 0xFF); // error on the attempt
        return true;
    }
    print_line("BOUND EXCEPTION", 23);
    CPU_IP += 1;
    /* Set IF=1 in the flags word already pushed on stack by intcall86,
     * so that after any IRQ's IRET we still have interrupts enabled. */
    uint16_t flags_on_stack = readw86((CPU_SS << 4) + CPU_SP + 4);
    writew86((CPU_SS << 4) + CPU_SP + 4, flags_on_stack | 0x0200); /* IF bit */
    ifl = 1; /* allow IRQs while waiting for keypress */
    return false; // bound exception, TODO: ???
}
