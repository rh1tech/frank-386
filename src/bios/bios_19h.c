#include "286/cpu.h"
#include "bios.h"
#include "fdos/fdos.h"
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

static void boot_from(CPU* cpu, uint8_t dl, bool native)
{
    CPU_DL = dl;
    /* IBM PC compatible entry point: physical 0000:7C00.
     * Some BIOSes use 07C0:0000; 0000:7C00 is the usual safe form. */
    SET_CS ( 0x0000 );
    SET_IP ( BOOT_ADDR >> 4 );
    SET_SS ( 0x0000 );
    CPU_SP = BOOT_ADDR >> 4;
/// TODO: support to select native BIOS + guest DOS
///    if (native) {
        // Native FreeDOS kernel
        _boot(cpu);
        kernel(cpu);
        __unreachable();
//    }
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
    if (params->done) goto ex; // just wait bios_19h will continue
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
        print_line("3", 2);
        params->done = true;
    }
ex:
    ifl = 1; // allow IRQ
    return false; // in a loop on the same CS:IP, no IRET required there
}

static bios_callback_params_t params = {
    .callback = bios_19h_waiter,
    .expected_cs = 0xFFEF,
    .expected_ip = 0x000F,
    .done = false
};

extern struct PC* pc;
void pc_step(struct PC* pc);

bool bios_19h(CPU* cpu) {
    print_line("Press Win+F12 to enter Setup", 1);
    SET_CS ( 0xFFEF ); // -> FFEFF
    SET_IP ( 0x000F );
    set_bios_callback(cpu, &params, false);
    while(!params.done) {
        pc_step(pc);
    }
    print_line("                            ", 1);
    print_line(" ", 2);
    drop_bios_callback(cpu, &params);
    params.done = false;
    print_line(" ", 2);
    /* Classic boot order used here: floppy A:, then first fixed disk C:.
    * No POST is done here; INT 19h is only bootstrap. */
    if (fdd_is_inserted(0) && read_boot_sector(fdd_get_file(0))) {
        boot_from(cpu, 0x00, false);
        return false;
    }
    if (ata_is_inserted(0) && !ata_is_cdrom(0) && read_boot_sector(ata_get_file(0))) {
        boot_from(cpu, 0x80, false);
        return false;
    }
//    bios_18h(cpu); // ROM Basic, or System halted
    bios_printf(cpu, "No boot media, native DOS is selected\n");
    boot_from(cpu, 0x00, true);
    __unreachable();
}
