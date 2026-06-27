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
