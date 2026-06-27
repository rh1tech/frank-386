#include "286/cpu.h"
#include "bios.h"

#define LSR_DATA_READY      0x01
#define LSR_OVERRUN_ERROR   0x02
#define LSR_PARITY_ERROR    0x04
#define LSR_FRAMING_ERROR   0x08
#define LSR_BREAK_INTERRUPT 0x10
#define LSR_THR_EMPTY       0x20
#define LSR_TSR_EMPTY       0x40
#define LSR_TIMEOUT         0x80

static uint16_t serial_base(uint16_t port_no)
{
    if (port_no >= 4)
        return 0;
    return pload16(0x400 + port_no * 2); /* COM1..COM4 */
}

static void serial_error(CPU* cpu)
{
    CPU_AH = LSR_TIMEOUT;
    CPU_AL = 0x00;
}

static void serial_return_status(CPU* cpu, uint16_t base)
{
    CPU_AH = cpu_portin8(base + 5); /* Line Status Register */
    CPU_AL = cpu_portin8(base + 6); /* Modem Status Register */
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
static bool serial_init(CPU* cpu, uint16_t base)
{
    uint8_t al = CPU_AL;

    static const uint16_t divisors[8] = {
        1047, 768, 384, 192, 96, 48, 24, 12
    };

    uint8_t rate   = (al >> 5) & 7;
    uint8_t parity = (al >> 3) & 3;
    uint8_t stop   = (al >> 2) & 1;
    uint8_t bits   = al & 3;

    uint16_t div = divisors[rate];
    uint8_t lcr = bits;

    if (stop)
        lcr |= 0x04;

    switch (parity) {
    case 0:
    case 2:
        break;          /* none */
    case 1:
        lcr |= 0x08;    /* odd */
        break;
    case 3:
        lcr |= 0x18;    /* even */
        break;
    }

    cpu_portout8(base + 3, lcr | 0x80);      /* DLAB */
    cpu_portout8(base + 0, div & 0xFF);
    cpu_portout8(base + 1, div >> 8);
    cpu_portout8(base + 3, lcr);

    cpu_portout8(base + 1, 0x00);            /* disable IRQ */
    cpu_portout8(base + 4, 0x0B);            /* DTR + RTS + OUT2 */

    serial_return_status(cpu, base);
    return true;
}

static bool serial_send(CPU* cpu, uint16_t base)
{
    uint8_t timeout = 0xFF;

    while (timeout--) {
        uint8_t lsr = cpu_portin8(base + 5);

        if (lsr & LSR_THR_EMPTY) {
            cpu_portout8(base + 0, CPU_AL);
            CPU_AH = cpu_portin8(base + 5);
            return true;
        }
    }

    CPU_AH = cpu_portin8(base + 5) | LSR_TIMEOUT;
    return true;
}

static bool serial_recv(CPU* cpu, uint16_t base)
{
    uint8_t timeout = 0xFF;

    while (timeout--) {
        uint8_t lsr = cpu_portin8(base + 5);

        if (lsr & LSR_DATA_READY) {
            CPU_AL = cpu_portin8(base + 0);
            CPU_AH = cpu_portin8(base + 5);
            return true;
        }
    }

    CPU_AH = cpu_portin8(base + 5) | LSR_TIMEOUT;
    return true;
}

static bool serial_status(CPU* cpu, uint16_t base)
{
    serial_return_status(cpu, base);
    return true;
}

/*
 * INT 14h - SERIAL
 *
 * AH=00h initialize port
 * AH=01h transmit character, AL=char
 * AH=02h receive character
 * AH=03h get port status
 */
bool bios_14h(CPU* cpu)
{
    uint8_t fn = CPU_AH;
    uint16_t port_no = CPU_DX & 0xFFFF;
    uint16_t base = serial_base(port_no);

    if (!base) {
        serial_error(cpu);
        return true;
    }

    switch (fn) {
    case 0x00:
        return serial_init(cpu, base);

    case 0x01:
        return serial_send(cpu, base);

    case 0x02:
        return serial_recv(cpu, base);

    case 0x03:
        return serial_status(cpu, base);

    default:
        serial_error(cpu);
        return true;
    }
}
