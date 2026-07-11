#include <pico.h>
//#include <hardware/gpio.h>
#include "286/cpu.h"
#include "bios/bios.h"

const char* last_int_call = "NONE";

static bool intcall_waiter(CPU* cpu, bios_callback_params_t* params) {
    //gpio_put(PICO_DEFAULT_LED_PIN, 1);
    last_int_call = params->owner;
    if (!params->done) {
        ifl = 0; /// no more IRQ till return to normal flow
        params->done = true;
        cpu->native_done = true;
    } else {
        ifl = 1; // allow IRQ
    }
    return false; // in a loop on the same CS:IP, no IRET required there
}

extern struct PC* pc;
void pc_step(struct PC* pc, size_t max_ops);

void bios_intcall(CPU* cpu, uint8_t intnum, const char* owner) {
    u16 cs = CPU_CS;
    u16 ip = CPU_IP;
    bios_callback_params_t params = {
        .callback = intcall_waiter,
        .expected_cs = 0xFFEF, // just default, may be changed
        .expected_ip = 0x000F, // by set_bios_callback reenter=true
        .done = false,
        .owner = owner
    };
    set_bios_callback(cpu, &params, true);
#if BIOS_DEBUG
    if (intnum != 0x1C) {
        char buf[80];
        u16 new_cs = getmem16(0, (uint16_t) intnum * 4 + 2);
        u16 new_ip = getmem16(0, (uint16_t) intnum * 4);
        int snprintf(char *s, size_t n, const char *fmt, ...);
        snprintf(buf, 79, "INT %02Xh ARM? %04X:%04X->%04X:%04X AX:%04X  ", intnum, cs, ip, params.expected_cs, params.expected_ip, CPU_AX);
        print_line(buf, 0);
    }
#endif
    // to handle IRET by intcall_waiter:
    SET_CS ( params.expected_cs ); // -> FFEFF
    SET_IP ( params.expected_ip );
    bool old_ifl = ifl;
    // set CS:IP/flags, prep stack, and on IRET will recover
    cpu_intcall(cpu, intnum);
    cpu->native_done = false;
    while(!params.done) {
        pc_step(pc, 4096); /// TODO: a lot of?
    }
    cpu->native_done = false;
    drop_bios_callback(cpu, &params);
    ifl = old_ifl;
    params.done = false;
    // restore initial CS:IP
    SET_CS (cs);
    SET_IP (ip);
    //gpio_put(PICO_DEFAULT_LED_PIN, 0);
}
