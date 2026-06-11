#include <stdio.h>
#include "i286.h"
#include "bios.h"

static bool no_handler() {
    print_line("SERIAL ERROR: no handler defined", 1);
    char buf[10];
    snprintf(buf, 10, "AX: %04xh", CPU_AX); print_line(buf, 2);
    snprintf(buf, 10, "BX: %04xh", CPU_BX); print_line(buf, 3);
    snprintf(buf, 10, "CX: %04xh", CPU_CX); print_line(buf, 4);
    snprintf(buf, 10, "DX: %04xh", CPU_DX); print_line(buf, 5);
    snprintf(buf, 10, "SI: %04xh", CPU_SI); print_line(buf, 5);
    snprintf(buf, 10, "DI: %04xh", CPU_DI); print_line(buf, 6);
    snprintf(buf, 10, "BP: %04xh", CPU_BP); print_line(buf, 7);
    snprintf(buf, 10, "DS: %04xh", CPU_DS); print_line(buf, 8);
    snprintf(buf, 10, "SS: %04xh", CPU_SS); print_line(buf, 9);
    snprintf(buf, 10, "FS: %04xh", CPU_FS); print_line(buf, 10);
    snprintf(buf, 10, "GS: %04xh", CPU_GS); print_line(buf, 11);
    snprintf(buf, 10, "ES: %04xh", CPU_ES); print_line(buf, 12);
while(1); // remove it
    return true;
}

/*
SERIAL - INITIALIZE PORT
AH = 00h
AL = port parameters (see #00300)
DX = port number (00h-03h) (04h-43h for Digiboard XAPCM232.SYS)

Return:
AH = line status (see #00304)
FFh if error on Digiboard XAPCM232.SYS
AL = modem status (see #00305)

Bitfields for serial port parameters:

Bit(s)  Description     (Table 00300)
7-5    data rate (110,150,300,600,1200,2400,4800,9600 bps)
4-3    parity (00 or 10 = none, 01 = odd, 11 = even)
2      stop bits (set = 2, clear = 1)
1-0    data bits (00 = 5, 01 = 6, 10 = 7, 11 = 8)
*/
bool bios_14h(void)
{
    uint8_t fn = CPU_AH;
    uint8_t al = CPU_AL;
    uint16_t port_no = CPU_DX & 0xFFFF;

    if (fn != 0x00)
        return no_handler();

    if (port_no >= 4) {
        CPU_AH = 0x80; // timeout/error, условно
        CPU_AL = 0x00;
        return true;
    }

    uint16_t base = pload16(0x400 + port_no * 2); // COM1..COM4 base

    if (base == 0) {
        CPU_AH = 0x80; // no port / timeout
        CPU_AL = 0x00;
        return true;
    }

    uint8_t rate   = (al >> 5) & 7;
    uint8_t parity = (al >> 3) & 3;
    uint8_t stop   = (al >> 2) & 1;
    uint8_t bits   = al & 3;

    static const uint16_t divisors[8] = {
        1047, // 110
        768,  // 150
        384,  // 300
        192,  // 600
        96,   // 1200
        48,   // 2400
        24,   // 4800
        12    // 9600
    };

    uint16_t div = divisors[rate];

    uint8_t lcr = 0;

    // data bits
    lcr |= bits;             // 00=5, 01=6, 10=7, 11=8

    // stop bits
    if (stop)
        lcr |= 0x04;

    // parity
    switch (parity) {
        case 0: // none
        case 2: // none
            break;
        case 1: // odd
            lcr |= 0x08;
            break;
        case 3: // even
            lcr |= 0x18;
            break;
    }

    // set divisor latch
    cpu_portout8(base + 3, lcr | 0x80);
    cpu_portout8(base + 0, div & 0xFF);
    cpu_portout8(base + 1, div >> 8);

    // restore line control
    cpu_portout8(base + 3, lcr);

    // disable UART interrupts for BIOS polled mode
    cpu_portout8(base + 1, 0x00);

    // minimal modem control: DTR + RTS + OUT2
    cpu_portout8(base + 4, 0x0B);

    // return status
    CPU_AH = cpu_portin8(base + 5); // Line Status Register
    CPU_AL = cpu_portin8(base + 6); // Modem Status Register

    return true;
}
