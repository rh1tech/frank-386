/*
 * Disk management for RP2350 - adapted from pico-286
 * Uses FatFS for SD card access
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <hardware/gpio.h>
#include "i386.h"
#include "disk.h"
#include "ff.h"
#include "diskio.h"
#include "mem.h"
#include "ems.h"
#include "vga.h"

extern FATFS fs;

int hdcount = 0;

static uint8_t sectorbuffer[512];

static uint8_t raw_sd_hdd_enabled = 0;
static uint32_t raw_sd_hdd_sectors = 0;

struct struct_fdd {
    FIL fil;
    char* name;
    uint32_t usable_size;
    uint16_t cyls;
    uint16_t sects;
    uint16_t heads;
    uint8_t readonly;
    uint8_t drive_type;  /* BIOS/CMOS type: 1=360K 2=1.2M 3=720K 4=1.44M 5=2.88M */
} fdd[2] = {
    [0] = { .drive_type = 4 },  // A: floppy, default 1.44M
    [1] = { .drive_type = 4 },  // B: floppy, default 1.44M
};  // 0-1: floppy, 2-3: HDD, 4: CD-ROM

struct struct_ata {
    FIL fil;
    char* name;
    uint32_t usable_size;
    uint16_t cyls;
    uint16_t sects;
    uint16_t heads;
    uint8_t iscdrom;
    uint8_t drive_type;
} ata[4] = { 0 };


static CPU *disk_cpu = NULL;
static VGAState *disk_vga = NULL;
void disk_set_vga(VGAState *vga) { disk_vga = vga; }

static void (*disk_cmos_update_cb)(uint8_t type_a, uint8_t type_b) = NULL;
void disk_set_cmos_callback(void (*cb)(uint8_t, uint8_t)) { disk_cmos_update_cb = cb; }

static void (*disk_fdc_mediachange_cb)(int drive) = NULL;
void disk_set_fdc_mediachange_callback(void (*cb)(int drive)) { disk_fdc_mediachange_cb = cb; }

static void (*disk_cdrom_change_cb)(int drive, const char *filename, int was_present) = NULL;
void disk_set_cdrom_change_callback(void (*cb)(int drive, const char *filename, int was_present)) { disk_cdrom_change_cb = cb; }

/* Установить FDPT (Fixed Disk Parameter Table) и INT 41h/46h векторы.
 * Вызывается при каждом INT 13h для HDD — перезаписывает то что мог
 * поставить SeaBIOS во время boot.
 * Раскладка IBM AT BIOS: +00 word cyls, +02 byte heads,
 * +04..07 precomp (0xFFFF), +09 control, +0D word landing zone, +0F byte sects. */
static void install_fdpt(void) {
    /* Всегда перезаписываем — SeaBIOS может восстановить свои векторы */
#define FDPT(base, c, h, s) do { \
    uint16_t _c=(c); uint8_t _h=(h),_s=(s),_ctrl=(_h>8)?0x08:0x00; \
    pstore8((base)+0x00,_c&0xFF); pstore8((base)+0x01,_c>>8); \
    pstore8((base)+0x02,_h);      pstore8((base)+0x03,0); \
    pstore8((base)+0x04,0xFF);    pstore8((base)+0x05,0xFF); /* reduced write */ \
    pstore8((base)+0x06,0xFF);    pstore8((base)+0x07,0xFF); /* write precomp */ \
    pstore8((base)+0x08,0);       pstore8((base)+0x09,_ctrl); \
    pstore8((base)+0x0A,0);       pstore8((base)+0x0B,0); \
    pstore8((base)+0x0C,0); \
    pstore8((base)+0x0D,_c&0xFF); pstore8((base)+0x0E,_c>>8); /* landing zone */ \
    pstore8((base)+0x0F,_s); /* sectors per track */ \
} while(0)
    if (ata[0].name) {
        FDPT(0x522, ata[0].cyls, ata[0].heads, ata[0].sects);
        pstore8(0x104,0x22); pstore8(0x105,0x05);
        pstore8(0x106,0x00); pstore8(0x107,0x00);
    }
    if (ata[1].name) {
        FDPT(0x532, ata[1].cyls, ata[1].heads, ata[1].sects);
        pstore8(0x118,0x32); pstore8(0x119,0x05);
        pstore8(0x11A,0x00); pstore8(0x11B,0x00);
    }
    // TODO: ata[2,3]
#undef FDPT
}

static void update_floppy_cmos(void) {
    if (!disk_cmos_update_cb) return;
    uint8_t ta = fdd[0].drive_type;
    uint8_t tb = fdd[1].drive_type;
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

void disk_set_cpu(CPU *cpu) {
    disk_cpu = cpu;
}

void ejectdisk(uint8_t drivenum, bool is_fdd) {
    if (drivenum < 2 && is_fdd && fdd[drivenum].name) {
        f_close(&fdd[drivenum].fil);
        free(fdd[drivenum].name);
        fdd[drivenum].name = 0;
        /* Empty floppy drive must fall back to default 1.44M type */
        fdd[drivenum].drive_type = 4;
        fdd[drivenum].cyls = 80;
        fdd[drivenum].heads = 2;
        fdd[drivenum].sects = 18;
        update_floppy_cmos();
        if (disk_fdc_mediachange_cb)
            disk_fdc_mediachange_cb(drivenum);
        return;
    }
    if (drivenum < 4 && ata[drivenum].iscdrom && ata[drivenum].name) {
        /* ATA eject: CD (from GUI or insertdisk) */
        f_close(&ata[drivenum].fil);
        free(ata[drivenum].name);
        ata[drivenum].name = 0;
        ata[drivenum].iscdrom = 0;
        ata[drivenum].usable_size = 0;
        ata[drivenum].cyls = 0;
        ata[drivenum].heads = 0;
        ata[drivenum].sects = 0;
        ata[drivenum].drive_type = 0;
        if (disk_cdrom_change_cb)
            disk_cdrom_change_cb(drivenum, NULL, 1);
        return;
    }
    if (drivenum < 4 && ata[drivenum].name) {
        /* HDD eject (e.g. from GUI for HDD drives) */
        f_close(&ata[drivenum].fil);
        free(ata[drivenum].name);
        ata[drivenum].name = 0;
        ata[drivenum].iscdrom = 0;
        ata[drivenum].usable_size = 0;
        ata[drivenum].cyls = 0;
        ata[drivenum].heads = 0;
        ata[drivenum].sects = 0;
        ata[drivenum].drive_type = 0;
        if (hdcount > 0) hdcount--;
    }
}

uint8_t insertdisk(uint8_t drivenum, bool is_fdd, bool is_cd, const char *pathname) {
    if ((is_fdd && drivenum >= 2) || drivenum >= 4) return false;
    // Build full path (files are in the SD_DATA_DIR directory)
    char path[256];
    snprintf(path, sizeof(path), SD_DATA_DIR_SLASH "%s", pathname);

    /* CD-ROMs are read-only; regular disks need write access */
    BYTE fmode = is_cd ? FA_READ : (FA_READ | FA_WRITE);
    FIL* pf = is_fdd ? &fdd[drivenum].fil : &ata[drivenum].fil;
    const int cd_was_present = (!is_fdd && is_cd && pf->obj.fs) ? 1 : 0;
    if (pf->obj.fs) {
        /* Eject whatever is currently in the drive before inserting new image */
        if (is_fdd)
            ejectdisk(drivenum, true);
        else if (is_cd)
            ejectdisk(drivenum, false);
        else
            ejectdisk(drivenum, false);
    }
    FRESULT fres = f_open(pf, path, fmode);
    if (FR_OK != fres) {
        /* Fall back to read-only if write-open failed (e.g. write-protected card) */
        if (fmode != FA_READ)
            fres = f_open(pf, path, FA_READ);
    }
    if (FR_OK != fres) {
        return 0;
    }
    if(is_fdd) fdd[drivenum].name = strdup(pathname);
    else ata[drivenum].name = strdup(pathname);
    size_t size = f_size(pf);

    int is_vhd = detect_vhd(pf, size);
    size_t usable_size = size;

    if (is_vhd) {
        // Fixed VHD: subtract 512-byte footer
        usable_size -= 512;
        //printf("disk: '%s': VHD detected, data size %u bytes\n", pathname, (unsigned)usable_size);
    }

    /* CD-ROM images are read-only sector images (2048-byte sectors logically,
     * but the block layer always uses 512-byte sectors for IDE).
     * Skip geometry/size validation for CD-ROMs. */
    if (is_cd) {
        size_t iso_sectors = size / 512;  /* nb_sectors for block layer */
        ata[drivenum].iscdrom    = 1;
        ata[drivenum].cyls       = 0;
        ata[drivenum].heads      = 0;
        ata[drivenum].sects      = 0;
        if (disk_cdrom_change_cb)
            disk_cdrom_change_cb(drivenum, path, cd_was_present);
        return 1;
    }
    // Validate size constraints (non-CD-ROM only)
    if (usable_size < 360 * 1024 || usable_size > 0x1f782000UL || (usable_size & 511)) {
        f_close(pf);
        return 0;
    }
    // Determine geometry (cyls, heads, sects)
    uint16_t cyls = 0, heads = 0, sects = 0, drive_type = 47;
    if (!is_fdd) {  // Hard disk
        sects = 63;
        heads = 16;
        // Try to detect geometry from MBR partition table.
        // The end-CHS of the last partition entry encodes the heads
        // and sectors-per-track the image was created with.
        UINT br;
        f_lseek(pf, 0);
        if (FR_OK == f_read(pf, sectorbuffer, 512, &br) && br == 512
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
        ata[drivenum].drive_type = drive_type;
        ata[drivenum].usable_size = usable_size;
        ata[drivenum].cyls = cyls;
        ata[drivenum].heads = heads;
        ata[drivenum].sects = sects;
        hdcount++;
    } else {  // Floppy disk
        switch (size) {
            case 163840:  cyls=40; heads=1; sects=8;  drive_type=1; break; //160K
            case 184320:  cyls=40; heads=1; sects=9;  drive_type=1; break; //180K
            case 327680:  cyls=40; heads=2; sects=8;  drive_type=1; break; //320K
            case 368640:  cyls=40; heads=2; sects=9;  drive_type=1; break; //360K
            case 655360:  cyls=80; heads=2; sects=8;  drive_type=3; break; //640K
            case 737280:  cyls=80; heads=2; sects=9;  drive_type=3; break; //720K
            case 1228800: cyls=80; heads=2; sects=15; drive_type=2; break; //1.2M
            case 1474560: cyls=80; heads=2; sects=18; drive_type=4; break; //1.44M
            case 1556480: cyls=80; heads=2; sects=19; drive_type=4; break; //1.49M
            case 1638400: cyls=80; heads=2; sects=20; drive_type=4; break; //1.60M
            case 1720320: cyls=80; heads=2; sects=21; drive_type=4; break; //DMF
            case 1763328: cyls=82; heads=2; sects=21; drive_type=4; break; //tomsrtbt
            case 1802240: cyls=80; heads=2; sects=22; drive_type=4; break;
            case 1884160: cyls=80; heads=2; sects=23; drive_type=4; break;
            case 1966080: cyls=80; heads=2; sects=24; drive_type=4; break;
            case 2048000: cyls=80; heads=2; sects=25; drive_type=4; break;
            case 2129920: cyls=80; heads=2; sects=26; drive_type=4; break;
            case 2211840: cyls=80; heads=2; sects=27; drive_type=4; break;
            case 2293760: cyls=80; heads=2; sects=28; drive_type=4; break;
            case 2375680: cyls=80; heads=2; sects=29; drive_type=4; break;
            case 2457600: cyls=80; heads=2; sects=30; drive_type=4; break;
            case 2949120: cyls=80; heads=2; sects=36; drive_type=5; break; //2.88M
            default: cyls=80; heads=2; sects=18; drive_type=4;
        }
        fdd[drivenum].drive_type = drive_type;
        fdd[drivenum].usable_size = usable_size;
        fdd[drivenum].readonly = 0;
        fdd[drivenum].cyls = cyls;
        fdd[drivenum].heads = heads;
        fdd[drivenum].sects = sects;
        // Update CMOS floppy type if floppy
        update_floppy_cmos();
        if (disk_fdc_mediachange_cb)
            disk_fdc_mediachange_cb(drivenum);
    }
    return 1;
}

//=============================================================================
// Public API for Disk UI
//=============================================================================

// Check if disk is inserted in specified drive
uint8_t ata_is_inserted(uint8_t drivenum) {
    if (drivenum >= 4) return 0;
    return !!ata[drivenum].name;
}

uint8_t fdd_is_inserted(uint8_t drivenum) {
    if (drivenum >= 2) return 0;
    return !!fdd[drivenum].name;
}

// Set CD-ROM flag for a drive
void disk_set_cdrom(uint8_t drivenum, uint8_t iscdrom) {
    if (drivenum >= 4) return;
    ata[drivenum].iscdrom = iscdrom;
}

// Check if drive is CD-ROM
uint8_t ata_is_cdrom(uint8_t drivenum) {
    if (drivenum >= 4) return 0;
    return ata[drivenum].iscdrom;
}

int8_t ata_hdd_slot(uint8_t bios_index) {
    for (uint8_t slot = 0; slot < 4; slot++) {
        if (!ata[slot].name || ata[slot].iscdrom)
            continue;
        if (bios_index == 0)
            return (int8_t)slot;
        bios_index--;
    }
    return -1;
}

uint8_t ata_hdd_count(void) {
    uint8_t count = 0;
    for (uint8_t slot = 0; slot < 4; slot++) {
        if (ata[slot].name && !ata[slot].iscdrom)
            count++;
    }
    return count;
}

void disk_set_raw_sd_hdd(uint8_t enabled) {
    raw_sd_hdd_enabled = 0;
    raw_sd_hdd_sectors = 0;

    if (!enabled)
        return;

    DWORD sectors = 0;
    if (disk_ioctl(0, GET_SECTOR_COUNT, &sectors) == RES_OK && sectors) {
        raw_sd_hdd_sectors = (uint32_t)sectors;
        raw_sd_hdd_enabled = 1;
    }
}

uint8_t disk_raw_sd_hdd_enabled(void) {
    return raw_sd_hdd_enabled;
}

uint32_t disk_raw_sd_sectors(void) {
    return raw_sd_hdd_enabled ? raw_sd_hdd_sectors : 0u;
}

bool disk_raw_sd_readonly(void) {
    return (disk_status(0) & STA_PROTECT) != 0;
}

bool disk_raw_sd_read(uint32_t lba, void *buf, uint32_t count) {
    if (!raw_sd_hdd_enabled || !count) return false;
    if (lba >= raw_sd_hdd_sectors || count > raw_sd_hdd_sectors - lba) return false;
    return disk_read(0, (BYTE *)buf, (LBA_t)lba, count) == RES_OK;
}

bool disk_raw_sd_write(uint32_t lba, const void *buf, uint32_t count) {
    if (!raw_sd_hdd_enabled || !count) return false;
    if (disk_status(0) & STA_PROTECT) return false;
    if (lba >= raw_sd_hdd_sectors || count > raw_sd_hdd_sectors - lba) return false;
    return disk_write(0, (const BYTE *)buf, (LBA_t)lba, count) == RES_OK;
}

bool disk_raw_sd_sync(void) {
    if (!raw_sd_hdd_enabled) return false;
    return disk_ioctl(0, CTRL_SYNC, NULL) == RES_OK;
}

uint8_t bios_hdd_count(void) {
    return (uint8_t)(ata_hdd_count() + (raw_sd_hdd_enabled ? 1u : 0u));
}

bool bios_hdd_get_info(uint8_t bios_index, bios_hdd_info_t *info) {
    if (!info)
        return false;

    memset(info, 0, sizeof(*info));
    info->ata_slot = -1;

    uint8_t ata_count = ata_hdd_count();
    if (bios_index < ata_count) {
        int8_t slot = ata_hdd_slot(bios_index);
        if (slot < 0)
            return false;

        info->ata_slot = slot;
        info->cyls = ata[(uint8_t)slot].cyls;
        info->heads = ata[(uint8_t)slot].heads;
        info->sects = ata[(uint8_t)slot].sects;
        info->total_sectors = ata[(uint8_t)slot].usable_size / 512u;
        return info->cyls && info->heads && info->sects && info->total_sectors;
    }

    if (raw_sd_hdd_enabled && bios_index == ata_count) {
        const uint32_t track = 255u * 63u;
        uint32_t cyls = (raw_sd_hdd_sectors + track - 1u) / track;
        if (cyls == 0) cyls = 1;
        if (cyls > 0xFFFFu) cyls = 0xFFFFu;

        info->raw_sd = 1;
        info->cyls = (uint16_t)cyls;
        info->heads = 255;
        info->sects = 63;
        info->total_sectors = raw_sd_hdd_sectors;
        return true;
    }

    return false;
}

#ifdef FDOS_RAWSD_DIAG
#include <stdarg.h>
/* Mirrors the debug-trace pattern in i386.c / ide.c: a static FIL opened
 * once in append mode. Called only AFTER disk_read() has returned, so the
 * SD/SPI is idle and this is a plain sequential FatFS write, not nested
 * inside the driver. Writes to 386/rawsd.log; read it off the card on a PC. */
static void rawsd_diag_log(const char *fmt, ...)
{
    static FIL lf;
    static int state = 0;              /* 0=unopened 1=open 2=failed */
    if (state == 2) return;
    if (state == 0) {
        if (f_open(&lf, SD_DATA_DIR_SLASH "rawsd.log",
                   FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS) == FR_OK)
            state = 1;
        else if (f_open(&lf, "rawsd.log",
                        FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS) == FR_OK)
            state = 1;                 /* fallback: volume root */
        else
            state = 2;
    }
    if (state != 1) return;
    char line[128];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    UINT bw = 0;
    f_write(&lf, line, (UINT)n, &bw);
    f_sync(&lf);
}
#endif

bool bios_hdd_read(uint8_t bios_index, uint32_t lba, void *buf, uint16_t count) {
    bios_hdd_info_t info;
    if (!count || !bios_hdd_get_info(bios_index, &info)) return false;
    if (lba >= info.total_sectors || count > info.total_sectors - lba) {
#ifdef FDOS_RAWSD_DIAG
        if (info.raw_sd)
            rawsd_diag_log("RANGE REJECT lba=%lu cnt=%u tot=%lu\r\n",
                           (unsigned long)lba, (unsigned)count,
                           (unsigned long)info.total_sectors);
#endif
        return false;
    }

    if (info.raw_sd) {
        DRESULT r = disk_read(0, (BYTE *)buf, (LBA_t)lba, count);
#ifdef FDOS_RAWSD_DIAG
        {
            /* Unconditional trace of the first raw_sd reads, so the log
               ALWAYS appears once any raw_sd read happens (no failure
               needed). Shows cluster-1 LBAs, the FAT read (a low lba among
               high ones), and exactly where z=1 begins. */
            static unsigned seen = 0;
            static int hdr = 0;
            const uint8_t *b = (const uint8_t *)buf;
            unsigned nbytes = 512u * (unsigned)count, k;
            int allzero = 1;
            for (k = 0; k < nbytes; k++) if (b[k]) { allzero = 0; break; }
            if (!hdr) {
                hdr = 1;
                rawsd_diag_log("== rawsd trace tot=%lu ==\r\n",
                               (unsigned long)info.total_sectors);
            }
            if (seen < 48u) {
                seen++;
                rawsd_diag_log("%u lba=%lu cnt=%u rc=%d z=%d "
                               "b=%02x%02x%02x%02x\r\n",
                               seen, (unsigned long)lba, (unsigned)count,
                               (int)r, allzero, b[0], b[1], b[2], b[3]);
            }
        }
#endif
        return r == RES_OK;
    }

    FIL *f = &ata[(uint8_t)info.ata_slot].fil;
    UINT done = 0;
    UINT bytes = (UINT)count * 512u;
    if (f_lseek(f, (FSIZE_t)lba * 512u) != FR_OK) return false;
    return f_read(f, buf, bytes, &done) == FR_OK && done == bytes;
}

bool bios_hdd_write(uint8_t bios_index, uint32_t lba, const void *buf, uint16_t count) {
    bios_hdd_info_t info;
    if (!count || !bios_hdd_get_info(bios_index, &info)) return false;
    if (lba >= info.total_sectors || count > info.total_sectors - lba) return false;

    if (info.raw_sd)
        return disk_write(0, (const BYTE *)buf, (LBA_t)lba, count) == RES_OK;

    FIL *f = &ata[(uint8_t)info.ata_slot].fil;
    UINT done = 0;
    UINT bytes = (UINT)count * 512u;
    if (f_lseek(f, (FSIZE_t)lba * 512u) != FR_OK) return false;
    return f_write(f, buf, bytes, &done) == FR_OK && done == bytes;
}

bool bios_hdd_sync(uint8_t bios_index) {
    bios_hdd_info_t info;
    if (!bios_hdd_get_info(bios_index, &info)) return false;
    if (info.raw_sd) return disk_ioctl(0, CTRL_SYNC, NULL) == RES_OK;
    return f_sync(&ata[(uint8_t)info.ata_slot].fil) == FR_OK;
}

uint8_t *disk_sector_buffer(void) {
    return sectorbuffer;
}

const char* fdd_get_filename(int i) {
    if (i >= 2) return NULL;
    return fdd[i].name;
}

const char* ata_get_filename(int i) {
    if (i >= 4) return NULL;
    return ata[i].name;
}

FIL* fdd_get_file(uint8_t drivenum) {
    if (drivenum >= 2) return 0;
    return &fdd[drivenum].fil;
}

uint16_t fdd_get_cyls(uint8_t drivenum) {
    if (drivenum >= 2) return 0;
    return fdd[drivenum].cyls;
}
uint16_t fdd_get_heads(uint8_t drivenum) {
    if (drivenum >= 2) return 0;
    return fdd[drivenum].heads;
}
uint16_t fdd_get_sects(uint8_t drivenum) {
    if (drivenum >= 2) return 0;
    return fdd[drivenum].sects;
}
uint32_t fdds_types() { return ((fdd[1].drive_type & 0xF) << 4) | (fdd[0].drive_type & 0xF); }

FIL* ata_get_file(uint8_t drivenum) {
    if (drivenum >= 4) return NULL;
    return &ata[drivenum].fil;
}
uint16_t ata_get_cyls(uint8_t drivenum) {
    if (drivenum >= 4) return 0;
    return ata[drivenum].cyls;
}
uint16_t ata_get_heads(uint8_t drivenum) {
    if (drivenum >= 4) return 0;
    return ata[drivenum].heads;
}
uint16_t ata_get_sects(uint8_t drivenum) {
    if (drivenum >= 4) return 0;
    return ata[drivenum].sects;
}
