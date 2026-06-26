#include "../cpu.h"
#include "../bios.h"
#include "../fdos.h"
#include "disk.h"
#include <ff.h>

#define BOOT_ADDR 0x07C00u

static int read_boot_sector(FIL *f)
{
    UINT br = 0;
    if (!f || !f->obj.fs)
        return 0;
    if (f_lseek(f, 0) != FR_OK)
        return 0;
    if (f_read(f, PC_RAM + BOOT_ADDR, 512, &br) != FR_OK || br != 512)
        return 0;
    return readw86(BOOT_ADDR + 510) == 0xAA55;
}

void boot_from(CPU* cpu, uint8_t dl)
{
    CPU_DL = dl;
    /* IBM PC compatible entry point: physical 0000:7C00.
     * Some BIOSes use 07C0:0000; 0000:7C00 is the usual safe form. */
    SET_CS ( 0x0000 );
    SET_IP ( 0x7C00 );
// like after POST (bios-less solution):
    SET_SS ( 0x0000 );
    CPU_SP = 0x7C00;

// FreeDOS kernel
	_boot(cpu);
	kernel(cpu);
}

/* TODO:
| Клавиша | Назначение                    |
| ------- | ----------------------------- |
| DEL     | вход в Setup                  |
| F2      | вход в Setup (часто ноутбуки) |
| F12     | Boot Menu                     |
| F11     | Boot Menu (часто MSI)         |
| F8      | Boot Menu (часто ASUS)        |
| ESC     | Boot Menu (часто HP)          |
*/

static bool bios_19h_waiter(CPU* cpu, bios_callback_params_t* params) {
    uint32_t ticks = pload32(0x046C);
    if (params->data == (void*)ticks) {
        goto ex;
    }
    params->data = (void*)ticks;
    if (ticks < 18) {
        print_line("1", 2);
    } else if (ticks < 36) {
        print_line("2", 2);
    } else {
        print_line(" ", 2);

        /* Classic boot order used here: floppy A:, then first fixed disk C:.
        * No POST is done here; INT 19h is only bootstrap. */
        if (fdd_is_inserted(0) && read_boot_sector(fdd_get_file(0))) {
            drop_bios_callback(cpu, params);
            boot_from(cpu, 0x00);
            return false;
        }
        if (ata_is_inserted(0) && !ata_is_cdrom(0) && read_boot_sector(ata_get_file(0))) {
            drop_bios_callback(cpu, params);
            boot_from(cpu, 0x80);
            return false;
        }
        return bios_18h(cpu); // ROM Basic, or System halted
    }
ex:
    ifl = 1; // allow IRQ
    return false; // in a loop on the same CS:IP, no IRET required there
}

static bios_callback_params_t params = {
    .callback = bios_19h_waiter,
    .expected_cs = 0xFFEF,
    .expected_ip = 0x000F
};

bool bios_19h(CPU* cpu) {
    print_line("Press Win+F12 to enter Setup", 1);
    SET_CS ( 0xFFEF ); // -> FFEFF
    SET_IP ( 0x000F );
    set_bios_callback(cpu, &params);
    return false; // exact CS:IP, no IRET required there
}
