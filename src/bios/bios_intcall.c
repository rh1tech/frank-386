#include "286/cpu.h"
#include "bios/bios.h"

static bool intcall_waiter(CPU* cpu, bios_callback_params_t* params) {
    if (!params->done) {
        params->done = true;
        drop_bios_callback(cpu, params);
    }
//    ifl = 1; // allow IRQ ?
    return false; // in a loop on the same CS:IP, no IRET required there
}

static bios_callback_params_t params = {
    .callback = intcall_waiter,
    .expected_cs = 0xFFEF,
    .expected_ip = 0x000F,
    .done = false
};

extern struct PC* pc;
void pc_step(struct PC* pc);

void bios_intcall(CPU* cpu, uint8_t intnum) {
    u16 cs = CPU_CS;
    u16 ip = CPU_IP;
    // to handle INT 10h IRET by dos_29h_waiter:
    SET_CS ( 0xFFEF ); // -> FFEFF
    SET_IP ( 0x000F );
    set_bios_callback(cpu, &params);
    // set CS:IP/flags, prep stack, and on IRET will recover
    cpu_intcall(cpu, 0x10);
    while(!params.done) {
        pc_step(pc);
    }
    params.done = false;
    // restore initial CS:IP
    SET_CS (cs);
    SET_IP (ip);
}
