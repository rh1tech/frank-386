#include <stdio.h>
#include "i286.h"
#include "bios.h"

/* INT 08h  –  IRQ0 System Timer Tick (18.2065 Hz)
 *
 * IBM PC/AT BIOS behaviour:
 *   1. Increment 32-bit tick counter at BDA 0x046C.
 *   2. If counter reached 0x1800B0 (ticks per 24 h), reset to 0
 *      and set midnight-rollover flag at BDA 0x0470.
 *   3. Chain to INT 1Ch (user timer tick hook, no-op by default).
 *   4. Send Non-Specific EOI (OCW2 = 0x20) to master PIC (port 0x20).
 *
 * Returns true → main loop performs IRET.
 */

/* Ticks per 24 h at 18.20648 Hz (IBM BIOS value). */
#define TICKS_PER_DAY  0x1800B0u

bool bios_08h(void)
{
    /* 1 & 2: tick counter */
    uint32_t ticks = pload32(0x046C);
    ticks++;
    if (ticks >= TICKS_PER_DAY) {
        ticks = 0;
        pstore8(0x0470, 1);   /* midnight rollover flag */
    }
    pstore32(0x046C, ticks);

    /* floppy_tick: декрементировать motor timeout counter */
    uint8_t motor_ctr = pload8(0x440);
    if (motor_ctr) {
        motor_ctr--;
        pstore8(0x440, motor_ctr);
        if (motor_ctr == 0) {
            /* выключить моторы: сбросить BDA motor status */
            pstore8(0x43F, 0x00);
        }
    }
    /* 3: chain to INT 1Ch (user tick hook)
    Moved to: void load_bios_and_reset(PC *pc)
    int 1Ch
    out 20h, 20h
    iret    
    */
//    { char buf[32]; snprintf(buf, sizeof(buf), "08h SS=%04x SP=%04x", CPU_SS, CPU_SP); print_line(buf, 13); }
 
    CPU_CS = 0xFFF0;
    CPU_IP = 0x0000;

    return false;
}
