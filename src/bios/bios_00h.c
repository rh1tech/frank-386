#include "286/cpu.h"
#include "bios.h"

#define printf(...) bios_printf(cpu, __VA_ARGS__)

static bool bios_00h_waiter(CPU* cpu, bios_callback_params_t* any) {
    // actually do nothing, since reboot only is allowed in this case
    ifl = 1; // allow IRQ
    return false; // in a loop on the same CS:IP, no IRET required there
}

static bios_callback_params_t params = {
    .callback = bios_00h_waiter,
    .expected_cs = 0xFFE0,
    .expected_ip = 0x00FF,
    .owner = "INT 00H"
};

extern const char* last_int_call;

/**
 * INT 00 - CPU-generated - DIVIDE ERROR

Desc:	generated if the divisor of a DIV or IDIV instruction is zero or the
	  quotient overflows the result register; DX and AX will be unchanged.
Notes:	on an 8086/8088, the return address points to the following instruction
	on an 80286+, the return address points to the divide instruction
	an 8086/8088 will generate this interrupt if the result of a division
	  is 80h (byte) or 8000h (word)
 */
bool bios_00h(CPU* cpu) {
	printf("\nDivide overflow at %04X:%04X\n", CPU_CS, CPU_IP);
	printf("Last int_call %s\n", last_int_call);
    SET_CS ( 0xFFF0 ); // -> FFF74: INT FFh
    SET_IP ( 0x0074 );
    set_bios_callback(cpu, &params, false);
    return false;
}
