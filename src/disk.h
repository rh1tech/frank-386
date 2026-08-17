/*
 * Disk management - adapted from pico-286
 * Provides INT 13h disk handler for floppy and hard drives
 */
#ifndef DISK_H
#define DISK_H

#include "i386.h"

// External variables
extern int hdcount;

// Disk management functions
void ejectdisk(uint8_t drivenum, bool is_fdd);
uint8_t insertdisk(uint8_t drivenum, bool is_fdd, bool is_cd, const char *pathname);
void disk_set_cpu(CPU *cpu);
// Disk UI API functions
uint8_t ata_is_inserted(uint8_t drivenum);
uint8_t fdd_is_inserted(uint8_t drivenum);
void disk_set_cmos_callback(void (*cb)(uint8_t type_a, uint8_t type_b));
void disk_set_fdc_mediachange_callback(void (*cb)(int drive));
/* Optional callback: called when a floppy (drive 0 or 1) is inserted/ejected.
   Used by the FDC emulator to update the DIR disk-change bit. */
void disk_set_fdc_mediachange_callback(void (*cb)(int drive));
/* Callback: called when a CD-ROM (drive 4) is inserted or ejected.
   Used by the IDE emulator to signal UNIT_ATTENTION. */
void disk_set_cdrom_change_callback(void (*cb)(int drive, const char *filename, int was_present));

struct VGAState;
void disk_set_vga(struct VGAState *vga);
uint8_t ata_is_cdrom(uint8_t drivenum);
/* Map dense BIOS HDD index (0 = 80h) to physical ATA slot 0..3. */
int8_t ata_hdd_slot(uint8_t bios_index);
/* Number of actual HDD images in ATA slots (CD-ROMs excluded). */
uint8_t ata_hdd_count(void);
typedef struct {
    uint8_t raw_sd;       /* 1 = whole physical SD card, 0 = file-backed ATA image */
    int8_t ata_slot;       /* 0..3 for ATA image, -1 for raw SD */
    uint16_t cyls;
    uint16_t heads;
    uint16_t sects;
    uint32_t total_sectors;
} bios_hdd_info_t;

void disk_set_raw_sd_hdd(uint8_t enabled);
uint8_t disk_raw_sd_hdd_enabled(void);

/* Whole-SD-card raw sector access for the USB MSC fallback (used when no
   floppy/ATA image is attached). Only usable while the SD raw HDD option is
   enabled (disk_set_raw_sd_hdd). Sizes/LBA are in 512-byte sectors. */
uint32_t disk_raw_sd_sectors(void);
bool     disk_raw_sd_readonly(void);
bool     disk_raw_sd_read(uint32_t lba, void *buf, uint32_t count);
bool     disk_raw_sd_write(uint32_t lba, const void *buf, uint32_t count);
bool     disk_raw_sd_sync(void);
uint8_t bios_hdd_count(void);
bool bios_hdd_get_info(uint8_t bios_index, bios_hdd_info_t *info);
bool bios_hdd_read(uint8_t bios_index, uint32_t lba, void *buf, uint16_t count);
bool bios_hdd_write(uint8_t bios_index, uint32_t lba, const void *buf, uint16_t count);
bool bios_hdd_sync(uint8_t bios_index);
uint8_t *disk_sector_buffer(void);

uint16_t fdd_get_cyls(uint8_t drivenum);
uint16_t fdd_get_heads(uint8_t drivenum);
uint16_t fdd_get_sects(uint8_t drivenum);
uint32_t fdds_types();

const char* fdd_get_filename(int i);
const char* ata_get_filename(int i);

typedef struct FIL_s FIL;
FIL* fdd_get_file(uint8_t);
FIL* ata_get_file(uint8_t drivenum);
uint16_t ata_get_cyls(uint8_t drivenum);
uint16_t ata_get_heads(uint8_t drivenum);
uint16_t ata_get_sects(uint8_t drivenum);

#endif /* DISK_H */
