#include "../cpu.h"
#include "../bios.h"

static bool bios_18h_waiter(CPU* cpu, bios_callback_params_t* any) {
    // actually do nothing, since reboot only is allowed in this case
    ifl = 1; // allow IRQ
    return false; // in a loop on the same CS:IP, no IRET required there
}

static bios_callback_params_t params = {
    .callback = bios_18h_waiter,
    .expected_cs = 0xFFE0,
    .expected_ip = 0x00FF
};

bool bios_18h(CPU* cpu) {
    print_line("No Basic ROM                           ", 1);
    print_line("System halted                          ", 2);
    SET_CS ( 0xFFF0 ); // -> FFF74: INT FFh
    SET_IP ( 0x0074 );
    set_bios_callback(cpu, &params);
    return false;
}
