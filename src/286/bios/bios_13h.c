#include <ff.h>
#include "debug.h"
#include "i286.h"
#include "bios.h"
#include "disk.h"

/* INT 13h status codes used by the minimal BIOS layer below.
 * These are the conventional BIOS status values returned in AH.
 */
#define INT13_ST_OK             0x00
#define INT13_ST_BAD_COMMAND    0x01
#define INT13_ST_SECTOR_NF      0x04
#define INT13_ST_ECHANGED       0x06  /* disk changed (SeaBIOS DISK_RET_ECHANGED) */
#define INT13_ST_NOT_REMOVABLE  0x0B  /* volume not removable (SeaBIOS DISK_RET_ENOTREMOVABLE) */
#define INT13_ST_CONTROLLER     0x20
#define INT13_ST_SEEK_FAILED    0x40
#define INT13_ST_TIMEOUT        0x80
#define INT13_ST_ELOCKED        0xB1  /* drive locked (SeaBIOS 0xB1) */
#define INT13_ST_ENOTLOCKED     0xB0  /* drive not locked (SeaBIOS 0xB0) */
#define INT13_ST_ETOOMANYLOCKS  0xB4  /* too many locks (SeaBIOS 0xB4) */

/* BDA disk status fields used by IBM-compatible BIOS services.
 * 0040:0041 - last diskette operation status
 * 0040:0074 - last fixed-disk operation status
 * 0040:0075 - number of fixed disks
 */
#define BDA_FDD_LAST_STATUS     0x0441u
#define BDA_HDD_LAST_STATUS     0x0474u
#define BDA_HDD_COUNT           0x0475u

/* Minimal EDD/INT 13h Extensions support.
 * AH=41h reports the services implemented below:
 *   CX bit 0: AH=42h/43h extended read/write are available.
 *   CX bit 2: AH=48h extended drive parameter table is available.
 * AH=48h returns a 26-byte v2.x-compatible parameter table.
 */
#define INT13_EDD_VERSION    0x30u
#define INT13_EDD_FEATURES   0x0007u
#define INT13_EDD_PARAM_SIZE 0x42u

typedef struct BiosDisk_s {
    FIL *f;
    uint8_t bios_drive;     /* Original BIOS DL value: 00h/01h for FDD, 80h..83h for HDD. */
    uint8_t hdd;            /* Non-zero for fixed disks. */
    uint16_t cyls;          /* Geometry supplied by disk.c, not guessed from file size. */
    uint16_t heads;
    uint16_t sects;
} BiosDisk;


/* Store the INT 13h completion status in both AH/CF and the matching BDA field.
 * FDD and HDD have separate "last status" locations, so AH=01h can report
 * the status for the same class of drive that the caller asks about in DL.
 */
static void int13_set_status(uint8_t drive, uint8_t status)
{
    CPU_AH = status;
    cf = status ? 1 : 0;

    if (drive & 0x80)
        pstore8(BDA_HDD_LAST_STATUS, status);
    else
        pstore8(BDA_FDD_LAST_STATUS, status);
}

/* Fetch the BDA-backed last status for AH=01h.
 * This deliberately does not keep a separate C global: the BDA is the state.
 */
static uint8_t int13_get_last_status(uint8_t drive)
{
    return (drive & 0x80) ? pload8(BDA_HDD_LAST_STATUS) : pload8(BDA_FDD_LAST_STATUS);
}

/* Resolve BIOS DL to the existing disk.c FDD/ATA image and geometry.
 * No size-based geometry guessing is done here: insertdisk() already chose
 * cyl/head/sector values and exposes them via fdd_get_*()/ata_get_*().
 */
static bool int13_get_disk(uint8_t drive, BiosDisk *d)
{
    d->f = NULL;
    d->bios_drive = drive;
    d->hdd = (drive & 0x80) ? 1 : 0;
    d->cyls = 0;
    d->heads = 0;
    d->sects = 0;

    if (!d->hdd) { /* FDD: DL=00h..01h */
        if (drive >= 2) return false;
        d->cyls = fdd_get_cyls(drive);
        d->heads = fdd_get_heads(drive);
        d->sects = fdd_get_sects(drive);
        d->f = fdd_is_inserted(drive) ? fdd_get_file(drive) : NULL;
        return true;
    }
    /* HDD: DL=80h..83h, CD-ROM images are not INT 13h CHS disks. */
    uint8_t hdd = drive & 0x7F;
    if (hdd >= 4 || !ata_is_inserted(hdd) || ata_is_cdrom(hdd))
        return false;
    d->cyls = ata_get_cyls(hdd);
    d->heads = ata_get_heads(hdd);
    d->sects = ata_get_sects(hdd);
    d->f = ata_get_file(hdd);
    return d->f && d->cyls && d->heads && d->sects;
}

/* Decode classic CHS registers into an LBA offset inside the backing image.
 * CH = cylinder low 8 bits
 * CL bits 0-5 = sector number, 1-based
 * CL bits 6-7 = cylinder bits 8-9
 * DH = head
 */
static bool int13_chs_to_lba(const BiosDisk *d, uint32_t *lba)
{
    uint16_t cyl = ((uint16_t)(CPU_CL & 0xC0) << 2) | (uint16_t)CPU_CH;
    uint8_t sect = CPU_CL & 0x3F;
    uint8_t head = CPU_DH;

    if (sect == 0 || sect > d->sects || head >= d->heads || cyl >= d->cyls)
        return false;

    *lba = ((uint32_t)cyl * d->heads + head) * d->sects + (uint32_t)(sect - 1);
    return true;
}

/* Verify that the whole AL-sector request stays inside the configured geometry.
 * This intentionally allows multi-sector transfers across track/head boundaries,
 * matching the common BIOS behaviour documented in the original read comment.
 */
static bool int13_check_range(const BiosDisk *d, uint32_t lba, uint16_t count)
{
    uint32_t total = (uint32_t)d->cyls * d->heads * d->sects;
    return count && lba < total && count <= (total - lba);
}

/* Total number of 512-byte sectors according to disk.c geometry.
 * This is intentionally geometry-backed, so CHS and EDD agree with each other.
 */
static uint32_t int13_total_sectors(const BiosDisk *d)
{
    return (uint32_t)d->cyls * d->heads * d->sects;
}

/* Common sector transfer path used by both CHS calls and EDD packet calls.
 * The caller supplies an already-decoded LBA, sector count and linear guest RAM
 * address.  For verify requests data is read from the image but not stored.
 */
static bool int13_transfer_lba(const BiosDisk *d, uint32_t lba, uint16_t count,
                               uint32_t addr, uint8_t write, uint8_t verify)
{
    uint8_t drive = d->bios_drive;

    if (!int13_check_range(d, lba, count)) {
        int13_set_status(drive, INT13_ST_SECTOR_NF);
        return true;
    }

    if (f_lseek(d->f, lba * 512u) != FR_OK) {
        int13_set_status(drive, INT13_ST_SEEK_FAILED);
        return true;
    }
debug_write("int13_transfer_lba DL=%02X CHS/LBA=%lu CNT=%u ES:BX=%04X:%04X PHYS=%05lX\n",
            drive, lba, count, CPU_ES, CPU_BX,
            ((uint32_t)CPU_ES << 4) + CPU_BX);

    uint8_t buf[512];

    for (uint16_t i = 0; i < count; i++) {
        UINT done = 0;
        uint32_t mem = addr + (uint32_t)i * 512u;

        if (write) {
            for (uint32_t j = 0; j < 512; j++)
                buf[j] = read86(mem + j);

            if (f_write(d->f, buf, 512, &done) != FR_OK || done != 512) {
                CPU_AL = (uint8_t)i;
                int13_set_status(drive, INT13_ST_CONTROLLER);
                return true;
            }
        } else {
            if (f_read(d->f, buf, 512, &done) != FR_OK || done != 512) {
                CPU_AL = (uint8_t)i;
                int13_set_status(drive, INT13_ST_CONTROLLER);
                return true;
            }

            if (!verify) {
                for (uint32_t j = 0; j < 512; j++)
                    write86(mem + j, buf[j]);
            }
        }
    }

    if (write)
        f_sync(d->f);

debug_write("INT13 DATA %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X\n",
            buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
            buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);

    CPU_AL = (uint8_t)count;
    int13_set_status(drive, INT13_ST_OK);
    return true;
}

/* Common CHS transfer path for AH=02h/03h/04h.
 * write=0, verify=0: read sectors to ES:BX
 * write=1, verify=0: write sectors from ES:BX
 * write=0, verify=1: read sectors from image but do not copy them to guest RAM
 */
static bool int13_rw_chs(uint8_t write, uint8_t verify)
{
    BiosDisk d;
    uint32_t lba;
    uint8_t count = CPU_AL;
    uint8_t drive = CPU_DL;

    if (!count) { // || count > 128) { TODO: -- 128 in SeaBIOS. Why?
        int13_set_status(drive, INT13_ST_BAD_COMMAND);
        return true;
    }

    if (!int13_get_disk(drive, &d)) {
        int13_set_status(drive, INT13_ST_TIMEOUT);
        return true;
    }
    if (!d.f) {
        int13_set_status(drive, INT13_ST_TIMEOUT);
        return true;
    }
    if (!int13_chs_to_lba(&d, &lba)) {
        int13_set_status(drive, INT13_ST_SECTOR_NF);
        return true;
    }

    uint32_t addr = ((uint32_t)CPU_ES << 4) + CPU_BX;
    return int13_transfer_lba(&d, lba, count, addr, write, verify);
}

/*
DISK - RESET DISK SYSTEM
AH = 00h
DL = drive number.  If bit 7 is set, reset hard disk system; otherwise reset diskette system.

Return:
CF clear if successful
AH = status

This file-backed implementation has no real controller state to recalibrate.
The meaningful effect is clearing the appropriate BDA last-status byte.
*/
static bool bios_13h_00h()
{
    if (!(CPU_DL & 0x80)) {
        /* FDD reset: сбросить BDA-состояние контроллера (SeaBIOS floppy_reset()) */
        pstore8(0x43E, 0x00); /* floppy_recalibration_status */
        pstore8(0x490, 0x00); /* floppy_media_state[0] */
        pstore8(0x491, 0x00); /* floppy_media_state[1] */
        pstore8(0x494, 0x00); /* floppy_track[0] */
        pstore8(0x495, 0x00); /* floppy_track[1] */
        pstore8(0x48B, 0x00); /* floppy_last_data_rate */
    }
    int13_set_status(CPU_DL, INT13_ST_OK);
    return true;
}

/*
DISK - GET STATUS OF LAST OPERATION
AH = 01h
DL = drive number

Return:
AH = status of previous INT 13h operation for diskette/fixed-disk class
CF clear if AH=00h, set otherwise
*/
static bool bios_13h_01h()
{
    uint8_t st = int13_get_last_status(CPU_DL);
    CPU_AH = st;
    cf = st ? 1 : 0;
    return true;
}

/*
DISK - READ SECTOR(S) INTO MEMORY
AH = 02h
AL = number of sectors to read (must be nonzero)
CH = low eight bits of cylinder number
CL = sector number 1-63 (bits 0-5)
high two bits of cylinder (bits 6-7, hard disk only)
DH = head number
DL = drive number (bit 7 set for hard disk)
ES:BX -> data buffer

Return:
CF set on error
if AH = 11h (corrected ECC error), AL = burst length
CF clear if successful
AH = status (see #00234)
AL = number of sectors transferred (only valid if CF set for some
BIOSes)

Notes: Errors on a floppy may be due to the motor failing to spin up quickly enough; the read should be retried at least three times,
 resetting the disk with AH=00h between attempts. Most BIOSes support "multitrack" reads, where the value in AL exceeds the number of
 sectors remaining on the track, in which case any additional sectors are read beginning at sector 1 on the following head in the
 same cylinder; the MSDOS CONFIG.SYS command MULTITRACK (or the Novell DOS DEBLOCK=) can be used to force DOS to split disk accesses
 which would wrap across a track boundary into two separate calls. The IBM AT BIOS and many other BIOSes use only the low four bits
 of DH (head number) since the WD-1003 controller which is the standard AT controller (and the controller that IDE emulates) only 
 supports 16 heads. AWARD AT BIOS and AMI 386sx BIOS have been extended to handle more than 1024 cylinders by placing bits 10 and 11
 of the cylinder number into bits 6 and 7 of DH. Under Windows95, a volume must be locked (see INT 21/AX=440Dh/CX=084Bh) in order to 
 perform direct accesses such as INT 13h reads and writes. All versions of MS-DOS (including MS-DOS 7 [Windows 95]) have a bug which 
 prevents booting on hard disks with 256 heads (FFh), so many modern BIOSes provide mappings with at most 255 (FEh) heads. Some cache 
 drivers flush their buffers when detecting that DOS is bypassed by directly issuing INT 13h from applications.
 A dummy read can be used as one of several methods to force cache flushing for unknown caches (e.g. before rebooting).
*/
static bool bios_13h_02h()
{
    return int13_rw_chs(0, 0);
}

/*
DISK - WRITE DISK SECTOR(S)
AH = 03h
AL = number of sectors to write (must be nonzero)
CH/CL/DH/DL = CHS drive address, same encoding as AH=02h
ES:BX -> data buffer

Return:
CF clear if successful, set on error
AH = status
AL = number of sectors transferred on some BIOSes when CF set
*/
static bool bios_13h_03h()
{
    return int13_rw_chs(1, 0);
}

/*
DISK - VERIFY DISK SECTOR(S)
AH = 04h
AL = number of sectors to verify (must be nonzero)
CH/CL/DH/DL = CHS drive address, same encoding as AH=02h

Return:
CF clear if the range can be read from the backing image
AH = status

For this emulator, "verify" means the backing sectors are readable and inside
configured geometry.  Data is not copied to guest memory.
*/
static bool bios_13h_04h()
{
    return int13_rw_chs(0, 1);
}

/*
FLOPPY - FORMAT TRACK
AH = 05h
AL = number of sectors to format
CH = track number
DH = head number
DL = drive number
ES:BX -> address field buffer (see #00235)

Return:
CF set on error
CF clear if successful
AH = status (see #00234)

Notes: On AT or higher, call AH=17h first. The number of sectors per track is read from the diskette parameter table pointed at by INT 1E

Format of floppy format address field buffer entry (one per sector in track):

Offset  Size    Description     (Table 00235)
00h    BYTE    track number
01h    BYTE    head number (0-based)
02h    BYTE    sector number
03h    BYTE    sector size (00h=128 bytes, 01h=256 bytes, 02h=512, 03h=1024)
*/
static bool bios_13h_05h()
{
    BiosDisk d;
    uint8_t drive = CPU_DL;

    if (!int13_get_disk(drive, &d)) {
        int13_set_status(drive, INT13_ST_TIMEOUT);
        return true;
    }
    if (!d.f) {
        int13_set_status(drive, INT13_ST_TIMEOUT);
        return true;
    }
    uint8_t count    = CPU_AL;
    uint8_t cylinder = CPU_CH;
    uint8_t head     = CPU_DH;

    if (cylinder >= d.cyls || head >= d.heads || count == 0 || count > d.sects) {
        int13_set_status(drive, INT13_ST_BAD_COMMAND);
        return true;
    }

    /* Заполнить каждый сектор дорожки fill byte 0xF6 (DPT offset+8, IBM standard) */
    uint8_t buf[512];
    memset(buf, 0xF6, sizeof(buf));

    for (uint8_t s = 0; s < count; s++) {
        uint32_t lba = ((uint32_t)cylinder * d.heads + head) * d.sects + s;
        if (f_lseek(d.f, lba * 512u) != FR_OK) {
            int13_set_status(drive, INT13_ST_SEEK_FAILED);
            return true;
        }
        UINT done = 0;
        if (f_write(d.f, buf, 512, &done) != FR_OK || done != 512) {
            int13_set_status(drive, INT13_ST_CONTROLLER);
            return true;
        }
    }
    f_sync(d.f);

    int13_set_status(drive, INT13_ST_OK);
    return true;
}

// В load_bios_and_reset() — один раз разместить таблицу в ROM-области:
#define FLOPPY_DPT_ADDR  0xFFF20u   // свободное место после config table (0xFFF10+10)

void install_floppy_dpt(void) {
    // Diskette Parameter Table — INT 1Eh standard format
    pstore8(FLOPPY_DPT_ADDR + 0, 0xDF); // specify 1: SRT=0xD, HUT=0xF
    pstore8(FLOPPY_DPT_ADDR + 1, 0x02); // specify 2: HLT=1, DMA=0
    pstore8(FLOPPY_DPT_ADDR + 2, 0x25); // motor off delay (ticks)
    pstore8(FLOPPY_DPT_ADDR + 3, 0x02); // bytes per sector: 2 = 512
    pstore8(FLOPPY_DPT_ADDR + 4, 0x12); // sectors per track: 18 (1.44M)
    pstore8(FLOPPY_DPT_ADDR + 5, 0x1B); // gap length
    pstore8(FLOPPY_DPT_ADDR + 6, 0xFF); // data length
    pstore8(FLOPPY_DPT_ADDR + 7, 0x6C); // gap length for format
    pstore8(FLOPPY_DPT_ADDR + 8, 0xF6); // fill byte for format
    pstore8(FLOPPY_DPT_ADDR + 9, 0x0F); // head settle time (ms)
    pstore8(FLOPPY_DPT_ADDR +10, 0x01); // motor start time (1=128 ms)

    // Вектор INT 1Eh должен указывать на эту таблицу
    pstore16(0x1E * 4,     FLOPPY_DPT_ADDR & 0x000F);        // IP/offset
    pstore16(0x1E * 4 + 2, (FLOPPY_DPT_ADDR >> 4) & 0xFFFF); // CS/segment
}

/*
DISK - GET DRIVE PARAMETERS
AH = 08h
DL = drive number

Return:
CF clear if successful
AH = 00h
CH = cylinder low 8 bits of maximum cylinder number
CL bits 0-5 = sectors per track
CL bits 6-7 = cylinder bits 8-9
DH = maximum head number
DL = number of drives of this type
BL = diskette drive type for floppy drives, if known
ES:DI -> diskette parameter table for floppy drives
*/
static bool bios_13h_08h()
{
    BiosDisk d;
    uint8_t drive = CPU_DL;

    if (!int13_get_disk(drive, &d)) {
        int13_set_status(drive, INT13_ST_TIMEOUT);
        return true;
    }

    uint16_t max_cyl = d.cyls - 1;
//    if (drive & 0x80 && max_cyl > 0)
//        max_cyl--;          /* TODO: ?? SeaBIOS disk_1308: last cylinder reserved for diagnostics */
    if (max_cyl > 1023)
        max_cyl = 1023;

    CPU_AX = 0x0000;
    CPU_CH = (uint8_t)(max_cyl & 0xFF);
    CPU_CL = (uint8_t)((d.sects & 0x3F) | ((max_cyl >> 2) & 0xC0));
    CPU_DH = (uint8_t)(d.heads - 1);

    if (drive & 0x80) {
        CPU_DL = pload8(BDA_HDD_COUNT);
        CPU_ES = 0x0000;
        CPU_DI = 0x0000;
    } else {
        CPU_DL = 2;       /* BDA equipment word currently advertises two floppy drives. */
        CPU_BL = fdds_types() >> (drive ? 4 : 0);
        CPU_ES = (FLOPPY_DPT_ADDR >> 4) & 0xFFFF;
        CPU_DI = FLOPPY_DPT_ADDR & 0x000F;
    }

    int13_set_status(drive, INT13_ST_OK);
    return true;
}

/*
DISK - GET DISK TYPE
AH = 15h
DL = drive number

Return:
AH = 00h no such drive
AH = 01h diskette, no change-line support
AH = 03h fixed disk present, CX:DX = number of 512-byte sectors
CF clear when a drive is present, set when absent
*/
static bool bios_13h_15h()
{
    BiosDisk d;
    uint8_t drive = CPU_DL;

    if (!int13_get_disk(drive, &d)) {
        CPU_AH = 0x00;
        cf = 1;
        return true;
    }

    if (drive & 0x80) {
        uint32_t sectors = (uint32_t)d.cyls * d.heads * d.sects;
        // TODO: SeaBIOS: why?
        //uint32_t sectors = (uint32_t)(d.cyls - 1) * d.heads * d.sects;
        CPU_AH = 0x03;
        CPU_CX = (uint16_t)(sectors >> 16);
        CPU_DX = (uint16_t)(sectors & 0xFFFF);
    } else {
        CPU_AH = 0x01;
    }

    cf = 0;
    return true;
}


/*
DISK - IBM/MS INT 13 EXTENSIONS - INSTALLATION CHECK
AH = 41h
BX = 55AAh
DL = drive number, normally 80h..FFh

Return if supported:
CF clear
BX = AA55h
AH = extension version
CX = feature bitmap

This is intentionally HDD-only.  Classic floppy drives continue to use CHS
functions; reporting EDD for DL=00h would be misleading.
*/
static bool bios_13h_41h()
{
    BiosDisk d;
    uint8_t drive = CPU_DL;

    if (!(drive & 0x80) || CPU_BX != 0x55AA || !int13_get_disk(drive, &d)) {
        CPU_AH = INT13_ST_BAD_COMMAND;
        cf = 1;
        return true;
    }

    CPU_BX = 0xAA55;
    CPU_AH = INT13_EDD_VERSION;
    CPU_CX = INT13_EDD_FEATURES;
    cf = 0;
    pstore8(BDA_HDD_LAST_STATUS, INT13_ST_OK);
    return true;
}

/* Decode an EDD Disk Address Packet pointed to by DS:SI.
 * Packet layout for the 16-byte form:
 *   +00 byte  size, must be at least 10h
 *   +01 byte  reserved, ignored here
 *   +02 word  sector count
 *   +04 word  buffer offset
 *   +06 word  buffer segment
 *   +08 qword starting LBA
 * This emulator stores image offsets in 32-bit LBA.  Non-zero high dword is
 * rejected instead of silently wrapping.
 */
static bool int13_decode_dap(uint32_t *lba, uint16_t *count, uint32_t *addr)
{
    uint32_t dap = ((uint32_t)CPU_DS << 4) + CPU_SI;
    uint8_t size = read86(dap + 0);

    if (size < 0x10)
        return false;

    *count = readw86(dap + 2);

    uint16_t off = readw86(dap + 4);
    uint16_t seg = readw86(dap + 6);
    *addr = ((uint32_t)seg << 4) + off;

    *lba = readdw86(dap + 8);
    if (readdw86(dap + 12) != 0)
        return false;

    return true;  /* count=0 валиден: SeaBIOS возвращает SUCCESS для пустого запроса */
}

/*
DISK - EXTENDED READ
AH = 42h
DL = drive number, normally 80h..FFh
DS:SI -> Disk Address Packet

Return:
CF clear if successful
AH = status

This is the EDD/LBA equivalent of AH=02h.  It uses the same image file and the
same geometry-backed range limit as the CHS path, but the sector address comes
from the packet LBA instead of CH/CL/DH.
*/
static bool bios_13h_42h()
{
    BiosDisk d;
    uint32_t lba, addr;
    uint16_t count;
    uint8_t drive = CPU_DL;

    if (!(drive & 0x80) || !int13_get_disk(drive, &d)) {
        int13_set_status(drive, INT13_ST_TIMEOUT);
        return true;
    }

    if (!int13_decode_dap(&lba, &count, &addr)) {
        int13_set_status(drive, INT13_ST_BAD_COMMAND);
        return true;
    }

    if (count == 0) {                              /* SeaBIOS: empty request → SUCCESS */
        int13_set_status(drive, INT13_ST_OK);
        return true;
    }

    uint32_t total = (uint32_t)d.cyls * d.heads * d.sects;
    if (lba >= total) {                            /* SeaBIOS: explicit range check */
        int13_set_status(drive, INT13_ST_SECTOR_NF);
        return true;
    }
    
    return int13_transfer_lba(&d, lba, count, addr, 0, 0);
}

/*
DISK - EXTENDED WRITE
AH = 43h
AL bit 0 = verify flag after write, ignored by many BIOSes
DL = drive number, normally 80h..FFh
DS:SI -> Disk Address Packet

Return:
CF clear if successful
AH = status

The write path writes guest memory to the backing image and f_sync() is called
by the common transfer helper.  AL verify-after-write is accepted but not given
extra handling, matching a minimal file-backed BIOS service.
*/
static bool bios_13h_43h()
{
    BiosDisk d;
    uint32_t lba, addr;
    uint16_t count;
    uint8_t drive = CPU_DL;

    if (!(drive & 0x80) || !int13_get_disk(drive, &d)) {
        int13_set_status(drive, INT13_ST_TIMEOUT);
        return true;
    }

    if (!int13_decode_dap(&lba, &count, &addr)) {
        int13_set_status(drive, INT13_ST_BAD_COMMAND);
        return true;
    }

    if (count == 0) {                              /* SeaBIOS: empty request → SUCCESS */
        int13_set_status(drive, INT13_ST_OK);
        return true;
    }

    uint32_t total = (uint32_t)d.cyls * d.heads * d.sects;
    if (lba >= total) {                            /* SeaBIOS: explicit range check */
        int13_set_status(drive, INT13_ST_SECTOR_NF);
        return true;
    }
    
    return int13_transfer_lba(&d, lba, count, addr, 1, 0);
}

/*
IBM/MS INT 13 Extensions - VERIFY SECTORS
AH = 44h
DL = drive number
DS:SI -> disk address packet (see #00272)

Return:
CF clear if successful
AH = 00h
CF set on error
AH = error code (see #00234)
disk address packet's block count field set to number of blocks
successfully verified
*/
static bool bios_13h_44h()
{
    BiosDisk d;
    uint32_t lba, addr;
    uint16_t count;
    uint8_t drive = CPU_DL;

    if (!(drive & 0x80) || !int13_get_disk(drive, &d)) {
        int13_set_status(drive, INT13_ST_TIMEOUT);
        return true;
    }

    if (!int13_decode_dap(&lba, &count, &addr)) {
        int13_set_status(drive, INT13_ST_BAD_COMMAND);
        return true;
    }

    if (count == 0) {
        int13_set_status(drive, INT13_ST_OK);
        return true;
    }

    uint32_t total = (uint32_t)d.cyls * d.heads * d.sects;
    if (lba >= total) {
        int13_set_status(drive, INT13_ST_SECTOR_NF);
        return true;
    }

    return int13_transfer_lba(&d, lba, count, addr, 0, 1);
}

/*
DISK - GET EXTENDED DRIVE PARAMETERS
AH = 48h
DL = drive number, normally 80h..FFh
DS:SI -> result buffer; word at +00 contains caller-provided buffer size

Return:
CF clear if successful
AH = status
DS:SI buffer filled with at least the 1Ah-byte EDD parameter table:
  +00 word table size returned
  +02 word information flags
  +04 dword physical cylinders
  +08 dword physical heads
  +0C dword physical sectors per track
  +10 qword total sectors
  +18 word bytes per sector

No DPTE pointer is returned because the emulator is not exposing a real BIOS
IDE parameter table here.
*/
static bool bios_13h_48h()
{
    BiosDisk d;
    uint8_t  drive = CPU_DL;
    uint32_t p     = ((uint32_t)CPU_DS << 4) + CPU_SI;
    uint16_t caller_size = readw86(p);

    if (!(drive & 0x80) || !int13_get_disk(drive, &d)) {
        int13_set_status(drive, INT13_ST_TIMEOUT);
        return true;
    }
    if ((drive & 0x7F) > 1) {
        /* DPTE только для 0x80 и 0x81; остальные не поддерживаются */
        int13_set_status(drive, INT13_ST_BAD_COMMAND);
        return true;
    }
    if (caller_size < 0x1A) {
        int13_set_status(drive, INT13_ST_BAD_COMMAND);
        return true;
    }

    uint32_t total      = int13_total_sectors(&d);
    uint16_t info_flags = 0x0002;               // geometry valid
    if (d.cyls > 1023) info_flags |= 0x0004;    // may be truncated

    // --- v1.x / v2.x часть (0x1A байт) ---
    uint16_t ret_size = 0x1A;
    writew86(p + 0x00, 0x1A);          // временно, обновим в конце
    writew86(p + 0x02, info_flags);
    writedw86(p + 0x04, d.cyls);
    writedw86(p + 0x08, d.heads);
    writedw86(p + 0x0C, d.sects);
    writedw86(p + 0x10, total);         // total sectors low
    writedw86(p + 0x14, 0x00000000);    // total sectors high
    writew86(p + 0x18, 512);           // bytes per sector

    // --- v2.x DPTE pointer (0x1E байт) ---
    if (caller_size >= 0x1E) {
        ret_size = 0x1E;
        uint32_t dpte_addr = ((drive & 0x7F) == 0) ? DPTE_ADDR_0 : DPTE_ADDR_1;
        writew86(p + 0x1A, (uint16_t)(dpte_addr & 0x000F));          // offset
        writew86(p + 0x1C, (uint16_t)((dpte_addr >> 4) & 0xFFFF));   // segment
    }

    // --- v3.0 Device Path (0x42 байт) ---
    if (caller_size >= 0x42) {
        ret_size = 0x42;

        writew86(p + 0x1E, 0xBEDD);    // key
        write86 (p + 0x20, 0x22);      // dpi_length: 34 байт (от +20h до +41h включительно, SeaBIOS Phoenix: 36-2=34)
        write86 (p + 0x21, 0x00);      // reserved1
        writew86(p + 0x22, 0x0000);    // reserved2

        // host_bus[4]: "ISA " (SeaBIOS: 4 байта, не 8)
        const char *bus = "ISA ";
        for (int i = 0; i < 4; i++)
            write86(p + 0x24 + i, (uint8_t)bus[i]);

        // iface_type[8]: "ATA     "
        const char *iface = "ATA     ";
        for (int i = 0; i < 8; i++) {
            write86(p + 0x28 + i, (uint8_t)iface[i]);
        }

        // iface_path: u64 (8 байт) начиная с +30h (SeaBIOS: offset of iface_path in int13dpt_s)
        uint16_t iobase = ((drive & 0x7F) < 2) ? 0x01F0 : 0x0170;
        writew86(p + 0x30, iobase);
        writew86(p + 0x32, 0x0000);
        writedw86(p + 0x34, 0x00000000);

        // device_path: u64 (8 байт) начиная с +38h (SeaBIOS: phoenix.device_path)
        uint8_t devnum = (drive & 1) ? 1 : 0;
        write86 (p + 0x38, devnum);
        write86 (p + 0x39, 0x00);
        writew86(p + 0x3A, 0x0000);
        writedw86(p + 0x3C, 0x00000000);

        // reserved3: +40h
        write86 (p + 0x40, 0x00);

        // checksum: +41h (SeaBIOS phoenix.checksum)
        // считается по байтам +1Eh..+40h (SeaBIOS: checksum_far(seg, param_far+30, 35) = от struct offset 0x1E, 35 байт)
        // записывается в +41h — последний байт буфера 0x42
        uint8_t sum = 0;
        for (uint32_t i = 0x1E; i < 0x41; i++)
            sum += read86(p + i);
        write86(p + 0x41, (uint8_t)((-sum) & 0xFF));  // +41h = последний байт
    }

    writew86(p + 0x00, ret_size);  // финальный размер
    int13_set_status(drive, INT13_ST_OK);
    return true;
}

bool bios_13h() {
    bool res = true;
    switch(CPU_AH) {
        case 0x00:
            res = bios_13h_00h(); // RESET DISK SYSTEM
            break;
        case 0x01:
            res = bios_13h_01h(); // GET STATUS OF LAST OPERATION
            break;
        case 0x02:
            res = bios_13h_02h(); // READ SECTOR(S) INTO MEMORY
            break;
        case 0x03:
            res = bios_13h_03h(); // WRITE DISK SECTOR(S)
            break;
        case 0x04:
            res = bios_13h_04h(); // VERIFY DISK SECTOR(S)
            break;
        case 0x05:
            res = bios_13h_05h(); // FORMAT
            break;
        case 0x08:
            res = bios_13h_08h(); // GET DRIVE PARAMETERS
            break;
        case 0x09:  /* INITIALIZE DRIVE PARAMETERS — no-op for file-backed emulator */
        case 0x0C:  /* SEEK TO CYLINDER — no-op for file-backed emulator */
        case 0x0D:  /* ALTERNATE DISK RESET — no-op for file-backed emulator */
        case 0x11:  /* RECALIBRATE DRIVE — no-op for file-backed emulator */
        case 0x14:  /* CONTROLLER INTERNAL DIAGNOSTIC — no-op for file-backed emulator */
            int13_set_status(CPU_DL, INT13_ST_OK);
            break;
        case 0x10: { /* CHECK DRIVE READY */
            BiosDisk d;
            int13_set_status(CPU_DL, int13_get_disk(CPU_DL, &d)
                            ? INT13_ST_OK : INT13_ST_TIMEOUT);
            break;
        }
        case 0x15:
            res = bios_13h_15h(); // GET DISK TYPE
            break;
        case 0x41:
            res = bios_13h_41h(); // EXTENSIONS INSTALLATION CHECK
            break;
        case 0x42:
            res = bios_13h_42h(); // EXTENDED READ
            break;
        case 0x43:
            res = bios_13h_43h(); // EXTENDED WRITE
            break;
        case 0x44:
            res = bios_13h_44h(); // IBM/MS INT 13 Extensions - VERIFY SECTORS
            break;
        case 0x45:  /* LOCK/UNLOCK DRIVE — no removable media support TODO: */
            int13_set_status(CPU_DL, INT13_ST_OK);
            break;
        case 0x46: {  /* EJECT MEDIA — not supported TODO: ensure */
            BiosDisk d;
            if (!int13_get_disk(CPU_DL, &d)) {
                int13_set_status(CPU_DL, INT13_ST_TIMEOUT);
            } else if (d.hdd && ata_is_cdrom(CPU_DL & 0x7F)) {
                /* CD-ROM: нет реального эжекта — сообщаем locked */
                int13_set_status(CPU_DL, INT13_ST_ELOCKED);
            } else {
                /* HDD/FDD: SeaBIOS ENOTREMOVABLE */
                int13_set_status(CPU_DL, INT13_ST_NOT_REMOVABLE);
            }
            break;
        }
        case 0x47: { /* EXTENDED SEEK — no-op for file-backed emulator */
            uint32_t lba, addr;
            uint16_t count;
            if (!int13_decode_dap(&lba, &count, &addr)) {
                int13_set_status(CPU_DL, INT13_ST_BAD_COMMAND);
            } else {
                int13_set_status(CPU_DL, INT13_ST_OK);
            }
            break;
        }
        case 0x48:
            res = bios_13h_48h(); // GET EXTENDED DRIVE PARAMETERS
            break;
        case 0x49: { /* EXTENDED MEDIA CHANGE */
            BiosDisk d;
            if (!int13_get_disk(CPU_DL, &d)) {
                int13_set_status(CPU_DL, INT13_ST_TIMEOUT);
            } else if (d.hdd && ata_is_cdrom((CPU_DL & 0x7F))) {
                /* CD-ROM: SeaBIOS всегда возвращает ECHANGED (0x06) */
                CPU_AH = INT13_ST_ECHANGED;
                cf = 1;
            } else {
                /* HDD/FDD: SeaBIOS всегда SUCCESS */
                int13_set_status(CPU_DL, INT13_ST_OK);
            }
            break;
        }
        case 0x4E: /* SET HARDWARE CONFIGURATION (SeaBIOS disk_134e) */
            switch (CPU_AL) {
            case 0x01: case 0x03: case 0x04: case 0x06:
                int13_set_status(CPU_DL, INT13_ST_OK);
                break;
            default:
                int13_set_status(CPU_DL, INT13_ST_BAD_COMMAND);
                break;
            }
            break;
        case 0xE3: // TODO: what is this ???            
            break;
        default:
// BIOS INT 13h is covered, all other - not related
            int13_set_status(CPU_DL, INT13_ST_BAD_COMMAND);
    }
    return res;
}
