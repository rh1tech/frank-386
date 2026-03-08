/*
 * Disk management for RP2350 - adapted from pico-286
 * Uses FatFS for SD card access
 */
#include <stdio.h>
#include <string.h>
#include <hardware/gpio.h>
#include "i386.h"
#include "ff.h"
#include "ems.h"
#include "vga.h"

extern FATFS fs;

int hdcount = 0, fdcount = 0;

static uint8_t sectorbuffer[512];

struct struct_drive {
    FIL diskfile;
    size_t filesize;
    size_t data_offset;   // 0 for raw, 0 for fixed VHD (footer at end only)
    uint16_t cyls;
    uint16_t sects;
    uint16_t heads;
    uint8_t inserted;
    uint8_t readonly;
    uint8_t iscdrom;
    uint8_t drive_type;  /* BIOS/CMOS type: 1=360K 2=1.2M 3=720K 4=1.44M 5=2.88M */  // CD-ROM flag for drive E:
} disk[5] = {
    [0] = { .drive_type = 4 },  // A: floppy, default 1.44M
    [1] = { .drive_type = 4 },  // B: floppy, default 1.44M
};  // 0-1: floppy, 2-3: HDD, 4: CD-ROM

static int led_state = 0;
static CPUI386 *disk_cpu = NULL;
static uint8_t *disk_mem = NULL;
static VGAState *disk_vga = NULL;
void disk_set_vga(VGAState *vga) { disk_vga = vga; }

/* Copy sector data to guest memory, routing VGA IOMEM through vga_mem_write */
static inline void disk_copy_to_guest(uint8_t *phys_mem, uint32_t guest,
                                      const uint8_t *buf, uint32_t len)
{
    if (disk_vga && guest >= 0xA0000 && guest + len <= 0xC0000) {
        for (uint32_t i = 0; i < len; i++)
            vga_mem_write(disk_vga, guest - 0xA0000 + i, buf[i]);
        return;
    }
    ems_copy_to_guest(phys_mem, guest, buf, len);
}

/* Copy from guest memory, routing VGA IOMEM through vga_mem_read */
static inline void disk_copy_from_guest(uint8_t *phys_mem, uint32_t guest,
                                        uint8_t *buf, uint32_t len)
{
    if (disk_vga && guest >= 0xA0000 && guest + len <= 0xC0000) {
        for (uint32_t i = 0; i < len; i++)
            buf[i] = vga_mem_read(disk_vga, guest - 0xA0000 + i);
        return;
    }
    ems_copy_from_guest(phys_mem, guest, buf, len);
}
static void (*disk_cmos_update_cb)(uint8_t type_a, uint8_t type_b) = NULL;
void disk_set_cmos_callback(void (*cb)(uint8_t, uint8_t)) { disk_cmos_update_cb = cb; }

static void (*disk_fdc_mediachange_cb)(int drive) = NULL;
void disk_set_fdc_mediachange_callback(void (*cb)(int drive)) { disk_fdc_mediachange_cb = cb; }

/* Установить FDPT (Fixed Disk Parameter Table) и INT 41h/46h векторы.
 * Вызывается при каждом INT 13h для HDD — перезаписывает то что мог
 * поставить SeaBIOS во время boot.
 * Раскладка IBM AT BIOS: +00 word cyls, +02 byte heads,
 * +04..07 precomp (0xFFFF), +09 control, +0D word landing zone, +0F byte sects. */
static void install_fdpt(void) {
    /* Всегда перезаписываем — SeaBIOS может восстановить свои векторы */
#define FDPT(base, c, h, s) do { \
    uint16_t _c=(c); uint8_t _h=(h),_s=(s),_ctrl=(_h>8)?0x08:0x00; \
    disk_mem[(base)+0x00]=_c&0xFF; disk_mem[(base)+0x01]=_c>>8; \
    disk_mem[(base)+0x02]=_h;      disk_mem[(base)+0x03]=0; \
    disk_mem[(base)+0x04]=0xFF;    disk_mem[(base)+0x05]=0xFF; /* reduced write */ \
    disk_mem[(base)+0x06]=0xFF;    disk_mem[(base)+0x07]=0xFF; /* write precomp */ \
    disk_mem[(base)+0x08]=0;       disk_mem[(base)+0x09]=_ctrl; \
    disk_mem[(base)+0x0A]=0;       disk_mem[(base)+0x0B]=0; \
    disk_mem[(base)+0x0C]=0; \
    disk_mem[(base)+0x0D]=_c&0xFF; disk_mem[(base)+0x0E]=_c>>8; /* landing zone */ \
    disk_mem[(base)+0x0F]=_s; /* sectors per track */ \
} while(0)
    if (disk[2].inserted) {
        FDPT(0x522, disk[2].cyls, disk[2].heads, disk[2].sects);
        disk_mem[0x104]=0x22; disk_mem[0x105]=0x05;
        disk_mem[0x106]=0x00; disk_mem[0x107]=0x00;
    }
    if (disk[3].inserted) {
        FDPT(0x532, disk[3].cyls, disk[3].heads, disk[3].sects);
        disk_mem[0x118]=0x32; disk_mem[0x119]=0x05;
        disk_mem[0x11A]=0x00; disk_mem[0x11B]=0x00;
    }
#undef FDPT
}

static void update_floppy_cmos(void) {
    if (!disk_cmos_update_cb) return;
    uint8_t ta = disk[0].drive_type;  // всегда 4 (1.44M) если не было mount
    uint8_t tb = disk[1].drive_type;
    disk_cmos_update_cb(ta, tb);
}

// Detect fixed VHD (footer at end)
static int detect_vhd(FIL *file, size_t size) {
    if (size < 512) return 0;
    UINT br;
    f_lseek(file, size - 512);
    if (FR_OK != f_read(file, sectorbuffer, 512, &br) || br != 512)
        return 0;
    // Footer starts with "conectix"
    if (memcmp(sectorbuffer, "conectix", 8) == 0) {
        return 1;
    }
    return 0;
}

void disk_set_cpu(CPUI386 *cpu) {
    disk_cpu = cpu;
    disk_mem = cpu_get_phys_mem(cpu);
}

// Forward declaration for filename tracking
void disk_set_filename(uint8_t drivenum, const char *filename);

static inline void ejectdisk(uint8_t drivenum) {
    if (drivenum & 0x80) drivenum = (uint8_t)(2u + (drivenum & 0x7Fu));

    if (disk[drivenum].inserted) {
        f_close(&disk[drivenum].diskfile);
        disk[drivenum].inserted = 0;
        disk_set_filename(drivenum, NULL);
        if (drivenum < 2) update_floppy_cmos();
        if (drivenum < 2 && disk_fdc_mediachange_cb)
            disk_fdc_mediachange_cb(drivenum);
        if (drivenum >= 2)
            hdcount--;
        else
            fdcount--;
    }
}

uint8_t insertdisk(uint8_t drivenum, const char *pathname) {
    FIL file;

    if (drivenum & 0x80) drivenum = (uint8_t)(2u + (drivenum & 0x7Fu));
    if (drivenum >= 5) return false;

    // Build full path (files are in 386/ directory)
    char path[256];
    snprintf(path, sizeof(path), "386/%s", pathname);

    FRESULT fres = f_open(&file, path, FA_READ | FA_WRITE);
    if (FR_OK != fres) {
        printf("disk: cannot open '%s' (FatFS error %d)\n", path, fres);
        return 0;
    }

    size_t size = f_size(&file);

    int is_vhd = detect_vhd(&file, size);
    size_t usable_size = size;

    if (is_vhd) {
        // Fixed VHD: subtract 512-byte footer
        usable_size -= 512;
        //printf("disk: '%s': VHD detected, data size %u bytes\n", pathname, (unsigned)usable_size);
    }

    // Validate size constraints
    if (usable_size < 360 * 1024 || usable_size > 0x1f782000UL || (usable_size & 511)) {
        printf("disk: '%s': bad size %u (need 360K..528M, 512-aligned)\n", pathname, (unsigned)usable_size);
        f_close(&file);
        return 0;
    }

    // Determine geometry (cyls, heads, sects)
    uint16_t cyls = 0, heads = 0, sects = 0;

    if (drivenum >= 2) {  // Hard disk
        sects = 63;
        heads = 16;

        // Try to detect geometry from MBR partition table.
        // The end-CHS of the last partition entry encodes the heads
        // and sectors-per-track the image was created with.
        UINT br;
        f_lseek(&file, 0);
        if (FR_OK == f_read(&file, sectorbuffer, 512, &br) && br == 512
            && sectorbuffer[510] == 0x55 && sectorbuffer[511] == 0xAA) {
            for (int p = 0; p < 4; p++) {
                uint8_t *pe = &sectorbuffer[0x1BE + p * 16];
                if (pe[4] == 0) continue;          // empty slot
                uint8_t end_h = pe[5];
                uint8_t end_s = pe[6] & 0x3F;
                if (end_s > 0 && end_h > 0) {
                    heads = end_h + 1;
                    sects = end_s;
                }
            }
        }

        cyls = usable_size / ((size_t)sects * heads * 512);
    } else {  // Floppy disk
        cyls = 80;
        sects = 18;
        heads = 2;

        /* Floppy geometry by image size:
         * 360K  = 40 cyl, 2 head,  9 sect (5.25" DD)
         * 720K  = 80 cyl, 2 head,  9 sect (3.5" DD)
         * 1.2M  = 80 cyl, 2 head, 15 sect (5.25" HD)
         * 1.44M = 80 cyl, 2 head, 18 sect (3.5" HD)  <-- default
         * 2.88M = 80 cyl, 2 head, 36 sect (3.5" ED) */
        if (size <= 368640) {        // 360K
            cyls = 40; sects = 9; heads = 2;
            disk[drivenum].drive_type = 1;
        } else if (size <= 737280) { // 720K
            cyls = 80; sects = 9; heads = 2;
            disk[drivenum].drive_type = 3;
        } else if (size <= 1228800) { // 1.2M
            cyls = 80; sects = 15; heads = 2;
            disk[drivenum].drive_type = 2;
        } else if (size <= 1474560) { // 1.44M
            cyls = 80; sects = 18; heads = 2;
            disk[drivenum].drive_type = 4;
        } else {                      // 2.88M
            cyls = 80; sects = 36; heads = 2;
            disk[drivenum].drive_type = 5;
        }
    }

    // Eject any existing disk and insert the new one
    ejectdisk(drivenum);

    disk[drivenum].diskfile = file;
    disk[drivenum].filesize = usable_size;
    disk[drivenum].data_offset = 0;   // fixed VHD has no header, only footer
    disk[drivenum].inserted = 1;
    disk[drivenum].readonly = 0;
    disk[drivenum].cyls = cyls;
    disk[drivenum].heads = heads;
    disk[drivenum].sects = sects;

    // Update drive counts
    if (drivenum >= 2) {
        hdcount++;
    } else {
        fdcount++;
    }

    // Update CMOS floppy type if floppy
    if (drivenum < 2) update_floppy_cmos();
    if (drivenum < 2 && disk_fdc_mediachange_cb)
        disk_fdc_mediachange_cb(drivenum);
    // Track filename for disk UI
    disk_set_filename(drivenum, pathname);
/*
    printf("disk: '%s' -> drive %d (%s, %uC/%uH/%uS, %u KB)\n",
           pathname, drivenum, drivenum >= 2 ? "HDD" : "FDD",
           cyls, heads, sects, (unsigned)(usable_size / 1024));
*/
    return 1;
}

// Call this ONLY if all parameters are valid! There is no check here!
static inline size_t chs2ofs(int drivenum, int cyl, int head, int sect) {
    return disk[drivenum].data_offset + (
                   ((size_t)cyl * (size_t)disk[drivenum].heads + (size_t)head) * (size_t)disk[drivenum].sects + (size_t) sect - 1
           ) * 512UL;
}


static void readdisk(uint8_t drivenum,
              uint16_t dstseg, uint16_t dstoff,
              uint16_t cyl, uint16_t sect, uint16_t head,
              uint16_t sectcount, int is_verify
) {
    uint32_t memdest = ((uint32_t) dstseg << 4) + (uint32_t) dstoff;
    uint32_t cursect = 0;

    // Check if disk is inserted
    if (!disk[drivenum].inserted) {
        cpu_set_ah(disk_cpu, 0x31);    // no media in drive
        cpu_set_al(disk_cpu, 0);
        cpu_set_cf(disk_cpu, 1);
        return;
    }

    // Check if CHS parameters are valid
    if (sect == 0 || sect > disk[drivenum].sects || cyl >= disk[drivenum].cyls || head >= disk[drivenum].heads) {
        cpu_set_ah(disk_cpu, 0x04);    // sector not found
        cpu_set_al(disk_cpu, 0);
        cpu_set_cf(disk_cpu, 1);
        return;
    }

    // Convert CHS to file offset
    size_t fileoffset = chs2ofs(drivenum, cyl, head, sect);

    // Check if fileoffset is valid
    if (fileoffset > disk[drivenum].filesize) {
        cpu_set_ah(disk_cpu, 0x04);    // sector not found
        cpu_set_al(disk_cpu, 0);
        cpu_set_cf(disk_cpu, 1);
        return;
    }

    // Set file position
    f_lseek(&disk[drivenum].diskfile, fileoffset);

    // Process sectors
    for (cursect = 0; cursect < sectcount; cursect++) {
        // Read the sector into buffer
        size_t br;
        f_read(&disk[drivenum].diskfile, &sectorbuffer[0], 512, &br);

        if (!br) {
            cpu_set_ah(disk_cpu, 0x04);    // sector not found
            cpu_set_al(disk_cpu, 0);
            cpu_set_cf(disk_cpu, 1);
            return;
        }

        if (is_verify) {
            if (!ems_verify_guest(disk_mem, memdest, sectorbuffer, 512)) {
                cpu_set_al(disk_cpu, cursect);
                cpu_set_cf(disk_cpu, 1);
                cpu_set_ah(disk_cpu, 0xBB);
                return;
            }
            memdest += 512;
        } else {
            // Copy sector data to guest memory (EMS-aware)
            disk_copy_to_guest(disk_mem, memdest, sectorbuffer, 512);
            memdest += 512;
        }

        gpio_put(PICO_DEFAULT_LED_PIN, led_state);
        led_state ^= 1;

        // Update file offset for next sector
        fileoffset += 512;
    }
    led_state = 0;
    gpio_put(PICO_DEFAULT_LED_PIN, led_state);

    // If no sectors could be read, handle the error
    if (cursect == 0) {
        cpu_set_ah(disk_cpu, 0x04);    // sector not found
        cpu_set_al(disk_cpu, 0);
        cpu_set_cf(disk_cpu, 1);
        return;
    }

    // Set success flags
    cpu_set_al(disk_cpu, cursect);
    cpu_set_cf(disk_cpu, 0);
    cpu_set_ah(disk_cpu, 0);
}

static void writedisk(uint8_t drivenum,
               uint16_t dstseg, uint16_t dstoff,
               uint16_t cyl, uint16_t sect, uint16_t head,
               uint16_t sectcount
) {
    uint32_t memdest = ((uint32_t) dstseg << 4) + (uint32_t) dstoff;
    uint32_t cursect;

    // Check if disk is inserted
    if (!disk[drivenum].inserted) {
        cpu_set_ah(disk_cpu, 0x31);    // no media in drive
        cpu_set_al(disk_cpu, 0);
        cpu_set_cf(disk_cpu, 1);
        return;
    }

    // Convert CHS to file offset
    size_t fileoffset = chs2ofs(drivenum, cyl, head, sect);

    // check if sector can be found
    if (
            ((sect == 0 || sect > disk[drivenum].sects || cyl >= disk[drivenum].cyls || head >= disk[drivenum].heads))
            || fileoffset > disk[drivenum].filesize
            || disk[drivenum].filesize < fileoffset
            ) {
        cpu_set_ah(disk_cpu, 0x04);    // sector not found
        cpu_set_al(disk_cpu, 0);
        cpu_set_cf(disk_cpu, 1);
        return;
    }

    // Check if drive is read-only
    if (disk[drivenum].readonly) {
        cpu_set_ah(disk_cpu, 0x03);    // drive is read-only
        cpu_set_al(disk_cpu, 0);
        cpu_set_cf(disk_cpu, 1);
        return;
    }

    // Set file position
    f_lseek(&disk[drivenum].diskfile, fileoffset);

    // Write each sector
    for (cursect = 0; cursect < sectcount; cursect++) {
        // Copy from guest memory to sector buffer (EMS-aware)
        disk_copy_from_guest(disk_mem, memdest, sectorbuffer, 512);
        memdest += 512;

        // Write the buffer to the file
        size_t bw;
        f_write(&disk[drivenum].diskfile, sectorbuffer, 512, &bw);
        gpio_put(PICO_DEFAULT_LED_PIN, led_state);
        led_state ^= 1;
    }
    led_state = 0;
    gpio_put(PICO_DEFAULT_LED_PIN, led_state);

    // Handle the case where no sectors were written
    if (sectcount && cursect == 0) {
        cpu_set_ah(disk_cpu, 0x04);    // sector not found
        cpu_set_al(disk_cpu, 0);
        cpu_set_cf(disk_cpu, 1);
        return;
    }

    // Set success flags
    cpu_set_al(disk_cpu, cursect);
    cpu_set_cf(disk_cpu, 0);
    cpu_set_ah(disk_cpu, 0);
}


void diskhandler(CPUI386 *cpu) {
    static uint8_t lastdiskah[5] = { 0 }, lastdiskcf[5] = { 0 };

    disk_cpu = cpu;
    disk_mem = cpu_get_phys_mem(cpu);

    // Keep BDA hard drive count in sync.  SeaBIOS discovers drives via
    // ATA port probing (which we don't implement), so BDA 0x475 stays 0.
    // Patching it here ensures DOS sees the correct count before it ever
    // tries to access drive C:.
    disk_mem[0x475] = hdcount;

    uint8_t dl_orig  = cpu_get_dl(cpu);
    uint8_t drivenum = (dl_orig & 0x80) ? (uint8_t)(2u + (dl_orig & 0x7Fu)) : dl_orig;
    uint8_t ah = cpu_get_ah(cpu);

// TODO:    if (dl_orig & 0x80) install_fdpt();
    // HDD drivenum 4+ не существует (4 = CD-ROM слот, 5+ = OOB)
    if ((dl_orig & 0x80) && drivenum >= 4) {
        cpu_set_ah(cpu, 0x01);  // invalid drive
        cpu_set_cf(cpu, 1);
        return;
    }

    // Handle the interrupt service based on the function requested in AH
    switch (ah) {
        case 0x00:  // Reset disk system
            if (disk[drivenum].inserted) {
                cpu_set_ah(cpu, 0);
                cpu_set_cf(cpu, 0);  // Successful reset (no-op in emulator)
            } else {
                cpu_set_cf(cpu, 1);  // Disk not inserted
            }
            break;

        case 0x01:  // Return last status
            cpu_set_ah(cpu, lastdiskah[drivenum]);
            cpu_set_cf(cpu, lastdiskcf[drivenum]);
            return;

        case 0x02:  // Read sector(s) into memory
            readdisk(drivenum, cpu_get_es(cpu), cpu_get_bx(cpu),
                     ((uint16_t)cpu_get_ch(cpu) | (((uint16_t)cpu_get_cl(cpu) & 0xC0u) << 2)),  // Cylinder
                     cpu_get_cl(cpu) & 63,                            // Sector
                     cpu_get_dh(cpu),                                 // Head
                     cpu_get_al(cpu),                                 // Sector count
                     0);                                              // Read operation
            break;

        case 0x03:  // Write sector(s) from memory
            writedisk(drivenum, cpu_get_es(cpu), cpu_get_bx(cpu),
                      ((uint16_t)cpu_get_ch(cpu) | (((uint16_t)cpu_get_cl(cpu) & 0xC0u) << 2)),  // Cylinder
                      cpu_get_cl(cpu) & 63,                            // Sector
                      cpu_get_dh(cpu),                                 // Head
                      cpu_get_al(cpu));                                // Sector count
            break;

        case 0x04:  // Verify sectors
            readdisk(drivenum, cpu_get_es(cpu), cpu_get_bx(cpu),
                     ((uint16_t)cpu_get_ch(cpu) | (((uint16_t)cpu_get_cl(cpu) & 0xC0u) << 2)),   // Cylinder
                     cpu_get_cl(cpu) & 63,                             // Sector
                     cpu_get_dh(cpu),                                  // Head
                     cpu_get_al(cpu),                                  // Sector count
                     1);                                               // Verify operation
            break;

        case 0x05:  // Format track
            cpu_set_cf(cpu, 0);  // Success (no-op for emulator)
            cpu_set_ah(cpu, 0);
            break;

        case 0x08: {  // Get drive parameters
            if (dl_orig & 0x80) {
                // HDD
                if (!disk[drivenum].inserted) {
                    cpu_set_cf(cpu, 1); cpu_set_ah(cpu, 0x07); break;
                }
                uint16_t mc = disk[drivenum].cyls - 1u;
                uint8_t  ch = (uint8_t)(mc & 0xFFu);
                uint8_t  cl = (uint8_t)((disk[drivenum].sects & 0x3Fu) | (((mc >> 8) & 0x03u) << 6));
                uint8_t  dh = disk[drivenum].heads - 1u;
                cpu_set_cf(cpu, 0); cpu_set_ah(cpu, 0);
                cpu_set_ch(cpu, ch); cpu_set_cl(cpu, cl);
                cpu_set_dh(cpu, dh); cpu_set_dl(cpu, hdcount);
            } else {
                // Флоппи — отвечаем всегда; drive_type инициализирован в 4 (1.44M)
                // и обновляется при mount, но не сбрасывается при unmount.
                static const uint8_t fd_geom[6][3] = {
                    {80,2,18},{40,2,9},{80,2,15},{80,2,9},{80,2,18},{80,2,36}
                };
                uint8_t dt = disk[drivenum].drive_type;
                if (dt > 5) dt = 4;
                uint16_t cyls  = disk[drivenum].inserted ? disk[drivenum].cyls  : fd_geom[dt][0];
                uint8_t  heads = disk[drivenum].inserted ? disk[drivenum].heads : fd_geom[dt][1];
                uint8_t  sects = disk[drivenum].inserted ? disk[drivenum].sects : fd_geom[dt][2];
                uint16_t mc = cyls - 1u;
                cpu_set_cf(cpu, 0); cpu_set_ah(cpu, 0);
                cpu_set_ch(cpu, (uint8_t)(mc & 0xFFu));
                cpu_set_cl(cpu, (uint8_t)((sects & 0x3Fu) | (((mc >> 8) & 0x03u) << 6)));
                cpu_set_dh(cpu, heads - 1);
                cpu_set_bl(cpu, dt);
                cpu_set_dl(cpu, fdcount);
            }
            break;
        }
/* TODO:
        case 0x41:  // Check Extensions Present
            if (drivenum >= 2) {
                cpu_set_ah(cpu, 0x30);  // version 3.0
                cpu_set_bx(cpu, 0xAA55);
                cpu_set_cx(cpu, 0x0007);  // поддерживаем функции 1,2,3
                cpu_set_cf(cpu, 0);
            } else {
                cpu_set_cf(cpu, 1);
            }
            break;
*/
        default:  // Unknown function requested
            cpu_set_cf(cpu, 1);  // Error
            break;
    }

    // Update last disk status
    lastdiskah[drivenum] = cpu_get_ah(cpu);
    lastdiskcf[drivenum] = cpu_get_cf(cpu);

    // Set the last status in BIOS Data Area (for hard drives)
    if (dl_orig & 0x80) {
        disk_mem[0x474] = cpu_get_ah(cpu);
    }
}

//=============================================================================
// Public API for Disk UI
//=============================================================================

// Eject disk from specified drive (0-4)
void disk_eject(uint8_t drivenum) {
    if (drivenum > 4) return;
    ejectdisk(drivenum);
}

// Insert disk image into specified drive (0-4)
// Returns 1 on success, 0 on failure
uint8_t disk_insert(uint8_t drivenum, const char *pathname) {
    if (drivenum > 4) return 0;
    return insertdisk(drivenum, pathname);
}

// Check if disk is inserted in specified drive
uint8_t disk_is_inserted(uint8_t drivenum) {
    if (drivenum > 4) return 0;
    return disk[drivenum].inserted;
}

// Get filename of inserted disk (returns empty string if none)
// Note: FatFS doesn't store filename in FIL, so we need separate tracking
static char disk_filenames[5][64] = {0};

void disk_set_filename(uint8_t drivenum, const char *filename) {
    if (drivenum > 4) return;
    if (filename) {
        strncpy(disk_filenames[drivenum], filename, 63);
        disk_filenames[drivenum][63] = 0;
    } else {
        disk_filenames[drivenum][0] = 0;
    }
}

const char* disk_get_filename(uint8_t drivenum) {
    if (drivenum > 4) return "";
    return disk_filenames[drivenum];
}

// Set CD-ROM flag for a drive
void disk_set_cdrom(uint8_t drivenum, uint8_t iscdrom) {
    if (drivenum > 4) return;
    disk[drivenum].iscdrom = iscdrom;
}

// Check if drive is CD-ROM
uint8_t disk_is_cdrom(uint8_t drivenum) {
    if (drivenum > 4) return 0;
    return disk[drivenum].iscdrom;
}

// Get disk geometry (для записи в CMOS)
uint16_t disk_get_cyls(uint8_t drivenum)  { return drivenum < 5 ? disk[drivenum].cyls  : 0; }
uint16_t disk_get_heads(uint8_t drivenum) { return drivenum < 5 ? disk[drivenum].heads : 0; }
uint16_t disk_get_sects(uint8_t drivenum) { return drivenum < 5 ? disk[drivenum].sects : 0; }

// Get FIL pointer for IDE backend (avoids double-open)
FIL* disk_get_fil(uint8_t drivenum)  { return drivenum < 5 && disk[drivenum].inserted ? &disk[drivenum].diskfile : NULL; }
UINT disk_get_filesize(uint8_t drivenum) { return drivenum < 5 ? disk[drivenum].filesize : 0; }
UINT disk_get_data_offset(uint8_t drivenum) { return drivenum < 5 ? disk[drivenum].data_offset : 0; }
