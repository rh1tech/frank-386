/*
 * fdd.h  –  Intel 8272A / 82077AA Floppy Disk Controller emulation
 *
 * Provides hardware-level FDC emulation via I/O ports 0x3F0–0x3F7,
 * DMA channel 2, IRQ 6.  Floppy image I/O is delegated to the existing
 * disk[] / FatFS layer through the disk.h public API.
 *
 * Port map (primary FDC, base 0x3F0):
 *   0x3F0  SRA  – Status Register A        (read-only,  PS/2)
 *   0x3F1  SRB  – Status Register B        (read-only,  PS/2)
 *   0x3F2  DOR  – Digital Output Register  (read/write)
 *   0x3F3  TDR  – Tape Drive Register      (read/write, mostly ignored)
 *   0x3F4  MSR  – Main Status Register     (read-only)
 *          DSR  – Data-rate Select Reg     (write-only)
 *   0x3F5  DATA – Data / FIFO             (read/write)
 *   0x3F7  DIR  – Digital Input Register   (read-only)
 *          CCR  – Config Control Reg       (write-only)
 *
 * Supported commands (sufficient for DOS / Windows 3.x / 9x):
 *   READ DATA (06/E6/C6), WRITE DATA (05/C5), READ TRACK (02),
 *   FORMAT TRACK (0D), VERIFY (06 with SK), RECALIBRATE (07),
 *   SENSE INTERRUPT STATUS (08), SPECIFY (03), SEEK (0F),
 *   SENSE DRIVE STATUS (04), VERSION (10→0x90), CONFIGURE (13),
 *   LOCK (14/94), PART ID (18→0x41), PERPENDICULAR MODE (12),
 *   DUMPREG (0E), READ ID (0A/4A).
 */
#ifndef FDC_H
#define FDC_H

#include <stdint.h>
#include "i8259.h"
#include "i8257.h"

typedef struct FDCState FDCState;

/* Create / destroy */
FDCState *fdc_new(PicState2 *pic, I8257State *dma);
void      fdc_free(FDCState *s);

/* Port I/O – called from pc_io_read / pc_io_write */
uint8_t fdc_ioport_read (FDCState *s, uint32_t addr);
void    fdc_ioport_write(FDCState *s, uint32_t addr, uint8_t val);

/* Called when a floppy image is inserted or ejected in drive 0 or 1 */
void fdc_media_changed(FDCState *s, int drive);

/* Periodic service – call from pc_step() once per emulated ms */
void fdc_tick(FDCState *s);

#endif /* FDC_H */
