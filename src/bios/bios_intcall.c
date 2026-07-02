#include "286/cpu.h"
#include "bios/bios.h"

static bool intcall_waiter(CPU* cpu, bios_callback_params_t* params) {
    if (!params->done) {
        params->done = true;
    }
//    ifl = 1; // allow IRQ ?
    return false; // in a loop on the same CS:IP, no IRET required there
}

extern struct PC* pc;
void pc_step(struct PC* pc);

void bios_intcall(CPU* cpu, uint8_t intnum) {
    u16 cs = CPU_CS;
    u16 ip = CPU_IP;
    bios_callback_params_t params = {
        .callback = intcall_waiter,
        .expected_cs = 0xFFEF, // just default, may be changed
        .expected_ip = 0x000F, // by set_bios_callback reenter=true
        .done = false
    };
    set_bios_callback(cpu, &params, true);
    // to handle IRET by intcall_waiter:
    SET_CS ( params.expected_cs ); // -> FFEFF
    SET_IP ( params.expected_ip );
    // set CS:IP/flags, prep stack, and on IRET will recover
    cpu_intcall(cpu, intnum);
    while(!params.done) {
        pc_step(pc);
    }
    drop_bios_callback(cpu, &params);
    params.done = false;
    // restore initial CS:IP
    SET_CS (cs);
    SET_IP (ip);
}
