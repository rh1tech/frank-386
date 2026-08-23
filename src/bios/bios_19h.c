#include "286/cpu.h"
#include "bios.h"
#include "fdos/fdos.h"
#include "disk.h"
#include "bulk_bounce.h"
#include <ff.h>

#define BOOT_ADDR 0x07C00u

static int read_boot_sector(FIL *f)
{
    UINT br = 0;
    if (!f || !f->obj.fs)
        return 0;
    if (f_lseek(f, 0) != FR_OK)
        return 0;
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (ega128_paging_active()) {
        if (f_read(f, guest_bulk_buf, 512, &br) != FR_OK || br != 512) return 0;
        for (uint32_t i = 0; i < 512; ++i) pstore8(BOOT_ADDR + i, guest_bulk_buf[i]);
    } else
#endif
    if (f_read(f, PC_RAM + BOOT_ADDR, 512, &br) != FR_OK || br != 512)
        return 0;
    return readw86(BOOT_ADDR + 510) == 0xAA55;
}

static int read_bios_hdd_boot_sector(uint8_t bios_index)
{
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (ega128_paging_active()) {
        if (!bios_hdd_read(bios_index, 0, guest_bulk_buf, 1)) return 0;
        for (uint32_t i = 0; i < 512; ++i) pstore8(BOOT_ADDR + i, guest_bulk_buf[i]);
    } else
#endif
    if (!bios_hdd_read(bios_index, 0, PC_RAM + BOOT_ADDR, 1))
        return 0;
    return readw86(BOOT_ADDR + 510) == 0xAA55;
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_exact_at(FIL *f, FSIZE_t offset, void *buf, UINT size)
{
    UINT br = 0;

    if (f_lseek(f, offset) != FR_OK)
        return 0;
    return f_read(f, buf, size, &br) == FR_OK && br == size;
}

/*
 * Read an El Torito no-emulation boot image from an ISO-9660 image.
 *
 * ISO-9660 uses 2048-byte logical sectors.  The Boot Record Volume
 * Descriptor is at sector 17 and contains the absolute LBA of the El Torito
 * boot catalog at offset 0x47.  The initial/default catalog entry at +0x20
 * points to the boot image.
 *
 * Only no-emulation boot entries are accepted here.  Floppy/HDD emulation
 * would require installing a CD-emulation INT 13h mapping, not merely loading
 * a sector at 0000:7C00.
 */
static int read_iso_boot_sector(FIL *f)
{
    uint8_t buf[16];
    uint32_t catalog_lba = 0;

    if (!f || !f->obj.fs)
        return 0;

    for (uint32_t vd_lba = 16; vd_lba < 64; vd_lba++) {
        FSIZE_t vd_offset = (FSIZE_t)vd_lba * 2048u;

        if (!read_exact_at(f, vd_offset, buf, 7))
            return 0;
        if (memcmp(buf + 1, "CD001", 5) != 0 || buf[6] != 0x01)
            return 0;

        if (buf[0] == 0x00) {
            if (!read_exact_at(f, vd_offset + 7, buf, 16) ||
                memcmp(buf, "EL TORITO SPECIF", 16) != 0)
                continue;
            if (!read_exact_at(f, vd_offset + 23, buf, 7) ||
                memcmp(buf, "ICATION", 7) != 0)
                continue;
            if (!read_exact_at(f, vd_offset + 0x47, buf, 4))
                return 0;
            catalog_lba = read_le32(buf);
            break;
        }

        if (buf[0] == 0xFF)
            return 0;
    }

    if (catalog_lba == 0)
        return 0;

    FSIZE_t catalog_offset = (FSIZE_t)catalog_lba * 2048u;

    /* Validation entry: header ID and trailing signature. */
    if (!read_exact_at(f, catalog_offset, buf, 1) || buf[0] != 0x01)
        return 0;
    if (!read_exact_at(f, catalog_offset + 0x1E, buf, 2) ||
        buf[0] != 0x55 || buf[1] != 0xAA)
        return 0;

    /* Initial/default entry.  Only bytes 0..11 are used. */
    if (!read_exact_at(f, catalog_offset + 0x20, buf, 12))
        return 0;
    if (buf[0] != 0x88)
        return 0;

    uint8_t media_type = buf[1] & 0x0F;
    if (media_type > 4)
        return 0;

    uint16_t load_seg = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    uint16_t sector_count = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
    uint32_t image_lba = read_le32(buf + 8);

    if (load_seg != 0 && load_seg != 0x07C0)
        return 0;
    if (sector_count == 0)
        sector_count = 4;

    uint32_t bytes = (uint32_t)sector_count * 512u;
    if (bytes < 512u)
        return 0;
    if (bytes > 2048u)
        bytes = 2048u;

    UINT br = 0;
    if (f_lseek(f, (FSIZE_t)image_lba * 2048u) != FR_OK)
        return 0;
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (ega128_paging_active()) {
        if (f_read(f, guest_bulk_buf, bytes, &br) != FR_OK || br < 512) return 0;
        for (uint32_t i = 0; i < br; ++i) pstore8(BOOT_ADDR + i, guest_bulk_buf[i]);
    } else
#endif
    if (f_read(f, PC_RAM + BOOT_ADDR, bytes, &br) != FR_OK || br < 512)
        return 0;
    /*
     * El Torito no-emulation images are not necessarily PC boot sectors.
     * ISOLINUX/GRUB-style CD boot images are validated by the boot catalog
     * entry and may not contain the 55AA signature at offset 510.  Floppy
     * and HDD emulation images, however, are disk boot sectors and should
     * keep the traditional signature check.
     */
    if (media_type == 0x00)
        return 1;
    return readw86(BOOT_ADDR + 510) == 0xAA55;
}

static void boot_from(CPU* cpu, uint8_t dl, bool native)
{
    CPU_DL = dl;
    /* IBM PC compatible entry point: physical 0000:7C00.
     * Some BIOSes use 07C0:0000; 0000:7C00 is the usual safe form. */
    SET_CS ( 0x0000 );
    SET_IP ( BOOT_ADDR );
    SET_SS ( 0x0000 );
    CPU_SP = BOOT_ADDR;
/// TODO: support to select native BIOS + guest DOS
//    if (native) {
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
    if (ticks >= 36) {
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
    .done = false,
    .owner = "INT 19H"
};

extern struct PC* pc;
void pc_step(struct PC* pc, size_t max_ops);

bool bios_19h(CPU* cpu) {
    SET_CS ( 0xFFEF ); // -> FFEFF
    SET_IP ( 0x000F );
    set_bios_callback(cpu, &params, false);
    while(!params.done) {
        pc_step(pc, 4096);
    }
    drop_bios_callback(cpu, &params);
    params.done = false;
    /* Classic boot order used here: floppy A:, then first fixed disk C:.
    * No POST is done here; INT 19h is only bootstrap. */
    if (fdd_is_inserted(0) && read_boot_sector(fdd_get_file(0))) {
        boot_from(cpu, 0x00, false);
        return false;
    }
    if (bios_hdd_count() && read_bios_hdd_boot_sector(0)) {
        boot_from(cpu, 0x80, false);
        return false;
    }
    if (ata_is_inserted(0) && ata_is_cdrom(0) && read_iso_boot_sector(ata_get_file(0))) {
        boot_from(cpu, 0x80, false);
        return false;
    }
//    bios_18h(cpu); // ROM Basic, or System halted
    bios_printf(cpu, "No boot media, native DOS is selected\n");
    boot_from(cpu, 0x00, true);
    __unreachable();
}
