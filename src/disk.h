/*
 * Disk management - adapted from pico-286
 * Provides INT 13h disk handler for floppy and hard drives
 */
#ifndef DISK_H
#define DISK_H

#include "i386.h"

// External variables
extern int hdcount, fdcount;

// Disk management functions
uint8_t insertdisk(uint8_t drivenum, const char *pathname);
void disk_set_cpu(CPUI386 *cpu);
void diskhandler(CPUI386 *cpu);

// Disk UI API functions
void disk_eject(uint8_t drivenum);
uint8_t disk_insert(uint8_t drivenum, const char *pathname);
uint8_t disk_is_inserted(uint8_t drivenum);
void disk_set_filename(uint8_t drivenum, const char *filename);
const char* disk_get_filename(uint8_t drivenum);
void disk_set_cdrom(uint8_t drivenum, uint8_t iscdrom);
void disk_set_cmos_callback(void (*cb)(uint8_t type_a, uint8_t type_b));
struct VGAState;
void disk_set_vga(struct VGAState *vga);
uint8_t disk_is_cdrom(uint8_t drivenum);

// INT 13h handler wrapper for CPU hook (matches int13_handler_t signature)
static inline void diskhandler_wrapper(CPUI386 *cpu, void *opaque)
{
	(void)opaque;
	diskhandler(cpu);
}

uint16_t disk_get_cyls(uint8_t drivenum);
uint16_t disk_get_heads(uint8_t drivenum);
uint16_t disk_get_sects(uint8_t drivenum);

// For IDE backend - get already-open FIL* to avoid double-open
#ifdef RP2350_BUILD
#include "ff.h"
FIL* disk_get_fil(uint8_t drivenum);
UINT disk_get_filesize(uint8_t drivenum);
UINT disk_get_data_offset(uint8_t drivenum);
#endif

#endif /* DISK_H */
