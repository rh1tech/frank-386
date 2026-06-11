#include "i286.h"
#include "bios.h"
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

static void boot_from(uint8_t dl)
{
    CPU_DL = dl;
    /* IBM PC compatible entry point: physical 0000:7C00.
     * Some BIOSes use 07C0:0000; 0000:7C00 is the usual safe form. */
    CPU_CS = 0x0000;
    CPU_IP = 0x7C00;
}

bool bios_19h() {
    /* Classic boot order used here: floppy A:, then first fixed disk C:.
     * No POST is done here; INT 19h is only bootstrap. */
    if (fdd_is_inserted(0) && read_boot_sector(fdd_get_file(0))) {
        boot_from(0x00);
        return false;
    }
    if (ata_is_inserted(0) && !ata_is_cdrom(0) && read_boot_sector(ata_get_file(0))) {
        boot_from(0x80);
        return false;
    }

    /*
INT 19h:
    try_read A: sector 0/0/1 to 0000:7C00
    if success and signature 55AA:
        DL = 00h
        JMP 0000:7C00

    try_read C: sector 0/0/1 to 0000:7C00
    if success and signature 55AA:
        DL = 80h
        JMP 0000:7C00

    print error
    wait/reboot/halt
     */
    // загрузчик не найден / boot sector невалиден / чтение не удалось
    return bios_18h(); // ROM Basic, or System halted
}

/*
void bios_int19h(void)
{
    // 1. Обычно прерывания запрещаются на время критической части
    cli();

    // 2. BIOS выбирает boot device
    // На старых PC/XT/AT это обычно:
    //   A: floppy first
    //   затем fixed disk
    //
    // В более поздних BIOS порядок задаётся CMOS / setup / boot menu.

    for (device in boot_order) {

        if (!device_present(device))
            continue;

        // 3. Сброс устройства перед чтением
        bios_disk_reset(device);

        // 4. Попытка прочитать первый сектор
        //
        // floppy:
        //   cylinder = 0
        //   head     = 0
        //   sector   = 1
        //
        // HDD:
        //   CHS 0/0/1 или соответствующая BIOS-логика
        //
        // destination:
        //   0000:7C00
        //
        ok = bios_int13_read_sector(
            device,
            cylinder = 0,
            head     = 0,
            sector   = 1,
            count    = 1,
            dest_seg = 0x0000,
            dest_off = 0x7C00
        );

        if (!ok)
            continue;

        // 5. Проверка сигнатуры boot sector
        if (mem16[0x7C00 + 510] != 0xAA55)
            continue;

        // 6. Передача управления boot sector
        //
        // Классически:
        //   CS:IP = 0000:7C00
        //
        // Некоторые BIOS передают как:
        //   07C0:0000
        //
        // Физический адрес одинаковый: 0x7C00.
        //
        // DL обычно содержит номер boot drive:
        //   00h = A:
        //   80h = first HDD

        jump_to_boot_sector(
            cs = 0x0000,
            ip = 0x7C00,
            dl = device.bios_drive_number
        );

        // сюда управление не возвращается
    }

    // 7. Если загрузиться не удалось
    print("No bootable device");
    halt_or_retry();
}

*/