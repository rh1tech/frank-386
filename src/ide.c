/*
 * IDE emulation
 * 
 * Copyright (c) 2003-2016 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

//#include "cutils.h"
#include "ide.h"
#include "mem.h"

#include <stdarg.h>
#include <stdio.h>
#include <hardware/timer.h>

//#define DEBUG_IDE_ATAPI
#ifdef DEBUG_IDE_ATAPI
/* ---- ATAPI trace ---- */
static FIL _atapi_tf;
static int _atapi_tf_open = 0;

void atapi_tlog(const char *fmt, ...)
{
    if (!_atapi_tf_open) {
        _atapi_tf_open = (f_open(&_atapi_tf, "386/atapi2.txt",
                                 FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS) == FR_OK);
    }
    if (!_atapi_tf_open)
        return;

    char buf[256];

    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len < 0)
        return;

    if (len > (int)sizeof(buf))
        len = sizeof(buf);

    UINT bw;
    f_write(&_atapi_tf, buf, len, &bw);
    f_sync(&_atapi_tf);
}
#else
#define atapi_tlog(...) (void)0
#endif

#define MAX_MULT_SECTORS 4

/* Bits of HD_STATUS */
#define ERR_STAT		0x01
#define INDEX_STAT		0x02
#define ECC_STAT		0x04	/* Corrected error */
#define DRQ_STAT		0x08
#define SEEK_STAT		0x10
#define SRV_STAT		0x10
#define WRERR_STAT		0x20
#define READY_STAT		0x40
#define BUSY_STAT		0x80

/* Bits for HD_ERROR */
#define MARK_ERR		0x01	/* Bad address mark */
#define TRK0_ERR		0x02	/* couldn't find track 0 */
#define ABRT_ERR		0x04	/* Command aborted */
#define MCR_ERR			0x08	/* media change request */
#define ID_ERR			0x10	/* ID field not found */
#define MC_ERR			0x20	/* media changed */
#define ECC_ERR			0x40	/* Uncorrectable ECC error */
#define BBD_ERR			0x80	/* pre-EIDE meaning:  block marked bad */
#define ICRC_ERR		0x80	/* new meaning:  CRC error during transfer */

/* Bits of HD_NSECTOR */
#define CD			0x01
#define IO			0x02
#define REL			0x04
#define TAG_MASK		0xf8

#define IDE_CMD_RESET           0x04
#define IDE_CMD_DISABLE_IRQ     0x02

/* ATA/ATAPI Commands pre T13 Spec */
#define WIN_NOP				0x00
/*
 *	0x01->0x02 Reserved
 */
#define CFA_REQ_EXT_ERROR_CODE		0x03 /* CFA Request Extended Error Code */
/*
 *	0x04->0x07 Reserved
 */
#define WIN_SRST			0x08 /* ATAPI soft reset command */
#define WIN_DEVICE_RESET		0x08
/*
 *	0x09->0x0F Reserved
 */
#define WIN_RECAL			0x10
#define WIN_RESTORE			WIN_RECAL
/*
 *	0x10->0x1F Reserved
 */
#define WIN_READ			0x20 /* 28-Bit */
#define WIN_READ_ONCE			0x21 /* 28-Bit without retries */
#define WIN_READ_LONG			0x22 /* 28-Bit */
#define WIN_READ_LONG_ONCE		0x23 /* 28-Bit without retries */
#define WIN_READ_EXT			0x24 /* 48-Bit */
#define WIN_READDMA_EXT			0x25 /* 48-Bit */
#define WIN_READDMA_QUEUED_EXT		0x26 /* 48-Bit */
#define WIN_READ_NATIVE_MAX_EXT		0x27 /* 48-Bit */
/*
 *	0x28
 */
#define WIN_MULTREAD_EXT		0x29 /* 48-Bit */
/*
 *	0x2A->0x2F Reserved
 */
#define WIN_WRITE			0x30 /* 28-Bit */
#define WIN_WRITE_ONCE			0x31 /* 28-Bit without retries */
#define WIN_WRITE_LONG			0x32 /* 28-Bit */
#define WIN_WRITE_LONG_ONCE		0x33 /* 28-Bit without retries */
#define WIN_WRITE_EXT			0x34 /* 48-Bit */
#define WIN_WRITEDMA_EXT		0x35 /* 48-Bit */
#define WIN_WRITEDMA_QUEUED_EXT		0x36 /* 48-Bit */
#define WIN_SET_MAX_EXT			0x37 /* 48-Bit */
#define CFA_WRITE_SECT_WO_ERASE		0x38 /* CFA Write Sectors without erase */
#define WIN_MULTWRITE_EXT		0x39 /* 48-Bit */
/*
 *	0x3A->0x3B Reserved
 */
#define WIN_WRITE_VERIFY		0x3C /* 28-Bit */
/*
 *	0x3D->0x3F Reserved
 */
#define WIN_VERIFY			0x40 /* 28-Bit - Read Verify Sectors */
#define WIN_VERIFY_ONCE			0x41 /* 28-Bit - without retries */
#define WIN_VERIFY_EXT			0x42 /* 48-Bit */
/*
 *	0x43->0x4F Reserved
 */
#define WIN_FORMAT			0x50
/*
 *	0x51->0x5F Reserved
 */
#define WIN_INIT			0x60
/*
 *	0x61->0x5F Reserved
 */
#define WIN_SEEK			0x70 /* 0x70-0x7F Reserved */
#define CFA_TRANSLATE_SECTOR		0x87 /* CFA Translate Sector */
#define WIN_DIAGNOSE			0x90
#define WIN_SPECIFY			0x91 /* set drive geometry translation */
#define WIN_DOWNLOAD_MICROCODE		0x92
#define WIN_STANDBYNOW2			0x94
#define WIN_STANDBY2			0x96
#define WIN_SETIDLE2			0x97
#define WIN_CHECKPOWERMODE2		0x98
#define WIN_SLEEPNOW2			0x99
/*
 *	0x9A VENDOR
 */
#define WIN_PACKETCMD			0xA0 /* Send a packet command. */
#define WIN_PIDENTIFY			0xA1 /* identify ATAPI device	*/
#define WIN_QUEUED_SERVICE		0xA2
#define WIN_SMART			0xB0 /* self-monitoring and reporting */
#define CFA_ERASE_SECTORS       	0xC0
#define WIN_MULTREAD			0xC4 /* read sectors using multiple mode*/
#define WIN_MULTWRITE			0xC5 /* write sectors using multiple mode */
#define WIN_SETMULT			0xC6 /* enable/disable multiple mode */
#define WIN_READDMA_QUEUED		0xC7 /* read sectors using Queued DMA transfers */
#define WIN_READDMA			0xC8 /* read sectors using DMA transfers */
#define WIN_READDMA_ONCE		0xC9 /* 28-Bit - without retries */
#define WIN_WRITEDMA			0xCA /* write sectors using DMA transfers */
#define WIN_WRITEDMA_ONCE		0xCB /* 28-Bit - without retries */
#define WIN_WRITEDMA_QUEUED		0xCC /* write sectors using Queued DMA transfers */
#define CFA_WRITE_MULTI_WO_ERASE	0xCD /* CFA Write multiple without erase */
#define WIN_GETMEDIASTATUS		0xDA	
#define WIN_ACKMEDIACHANGE		0xDB /* ATA-1, ATA-2 vendor */
#define WIN_POSTBOOT			0xDC
#define WIN_PREBOOT			0xDD
#define WIN_DOORLOCK			0xDE /* lock door on removable drives */
#define WIN_DOORUNLOCK			0xDF /* unlock door on removable drives */
#define WIN_STANDBYNOW1			0xE0
#define WIN_IDLEIMMEDIATE		0xE1 /* force drive to become "ready" */
#define WIN_STANDBY             	0xE2 /* Set device in Standby Mode */
#define WIN_SETIDLE1			0xE3
#define WIN_READ_BUFFER			0xE4 /* force read only 1 sector */
#define WIN_CHECKPOWERMODE1		0xE5
#define WIN_SLEEPNOW1			0xE6
#define WIN_FLUSH_CACHE			0xE7
#define WIN_WRITE_BUFFER		0xE8 /* force write only 1 sector */
#define WIN_WRITE_SAME			0xE9 /* read ata-2 to use */
	/* SET_FEATURES 0x22 or 0xDD */
#define WIN_FLUSH_CACHE_EXT		0xEA /* 48-Bit */
#define WIN_IDENTIFY			0xEC /* ask drive to identify itself	*/
#define WIN_MEDIAEJECT			0xED
#define WIN_IDENTIFY_DMA		0xEE /* same as WIN_IDENTIFY, but DMA */
#define WIN_SETFEATURES			0xEF /* set special drive features */
#define EXABYTE_ENABLE_NEST		0xF0
#define WIN_SECURITY_SET_PASS		0xF1
#define WIN_SECURITY_UNLOCK		0xF2
#define WIN_SECURITY_ERASE_PREPARE	0xF3
#define WIN_SECURITY_ERASE_UNIT		0xF4
#define WIN_SECURITY_FREEZE_LOCK	0xF5
#define WIN_SECURITY_DISABLE		0xF6
#define WIN_READ_NATIVE_MAX		0xF8 /* return the native maximum address */
#define WIN_SET_MAX			0xF9
#define DISABLE_SEAGATE			0xFB

/* ATAPI defines */

#define ATAPI_PACKET_SIZE 12

/* The generic packet command opcodes for CD/DVD Logical Units,
 * From Table 57 of the SFF8090 Ver. 3 (Mt. Fuji) draft standard. */
#define GPCMD_BLANK			    0xa1
#define GPCMD_CLOSE_TRACK		    0x5b
#define GPCMD_FLUSH_CACHE		    0x35
#define GPCMD_FORMAT_UNIT		    0x04
#define GPCMD_GET_CONFIGURATION		    0x46
#define GPCMD_GET_EVENT_STATUS_NOTIFICATION 0x4a
#define GPCMD_GET_PERFORMANCE		    0xac
#define GPCMD_INQUIRY			    0x12
#define GPCMD_LOAD_UNLOAD		    0xa6
#define GPCMD_MECHANISM_STATUS		    0xbd
#define GPCMD_MODE_SELECT_10		    0x55
#define GPCMD_MODE_SENSE_10		    0x5a
#define GPCMD_PAUSE_RESUME		    0x4b
#define GPCMD_PLAY_AUDIO_10		    0x45
#define GPCMD_PLAY_AUDIO_MSF		    0x47
#define GPCMD_PLAY_AUDIO_TI		    0x48
#define GPCMD_PLAY_CD			    0xbc
#define GPCMD_PREVENT_ALLOW_MEDIUM_REMOVAL  0x1e
#define GPCMD_READ_10			    0x28
#define GPCMD_READ_12			    0xa8
#define GPCMD_READ_CDVD_CAPACITY	    0x25
#define GPCMD_READ_CD			    0xbe
#define GPCMD_READ_CD_MSF		    0xb9
#define GPCMD_READ_DISC_INFO		    0x51
#define GPCMD_READ_DVD_STRUCTURE	    0xad
#define GPCMD_READ_FORMAT_CAPACITIES	    0x23
#define GPCMD_READ_HEADER		    0x44
#define GPCMD_READ_TRACK_RZONE_INFO	    0x52
#define GPCMD_READ_SUBCHANNEL		    0x42
#define GPCMD_READ_TOC_PMA_ATIP		    0x43
#define GPCMD_REPAIR_RZONE_TRACK	    0x58
#define GPCMD_REPORT_KEY		    0xa4
#define GPCMD_REQUEST_SENSE		    0x03
#define GPCMD_RESERVE_RZONE_TRACK	    0x53
#define GPCMD_SCAN			    0xba
#define GPCMD_SEEK			    0x2b
#define GPCMD_SEND_DVD_STRUCTURE	    0xad
#define GPCMD_SEND_EVENT		    0xa2
#define GPCMD_SEND_KEY			    0xa3
#define GPCMD_SEND_OPC			    0x54
#define GPCMD_SET_READ_AHEAD		    0xa7
#define GPCMD_SET_STREAMING		    0xb6
#define GPCMD_START_STOP_UNIT		    0x1b
#define GPCMD_STOP_PLAY_SCAN		    0x4e
#define GPCMD_TEST_UNIT_READY		    0x00
#define GPCMD_VERIFY_10			    0x2f
#define GPCMD_WRITE_10			    0x2a
#define GPCMD_WRITE_AND_VERIFY_10	    0x2e
/* This is listed as optional in ATAPI 2.6, but is (curiously)
 * missing from Mt. Fuji, Table 57.  It _is_ mentioned in Mt. Fuji
 * Table 377 as an MMC command for SCSi devices though...  Most ATAPI
 * drives support it. */
#define GPCMD_SET_SPEED			    0xbb
/* This seems to be a SCSI specific CD-ROM opcode
 * to play data at track/index */
#define GPCMD_PLAYAUDIO_TI		    0x48
/*
 * From MS Media Status Notification Support Specification. For
 * older drives only.
 */
#define GPCMD_GET_MEDIA_STATUS		    0xda
#define GPCMD_MODE_SENSE_6		    0x1a

/* Mode page codes for mode sense/set */
#define GPMODE_R_W_ERROR_PAGE		0x01
#define GPMODE_WRITE_PARMS_PAGE		0x05
#define GPMODE_AUDIO_CTL_PAGE		0x0e
#define GPMODE_POWER_PAGE		0x1a
#define GPMODE_FAULT_FAIL_PAGE		0x1c
#define GPMODE_TO_PROTECT_PAGE		0x1d
#define GPMODE_CAPABILITIES_PAGE	0x2a
#define GPMODE_ALL_PAGES		0x3f
/* Not in Mt. Fuji, but in ATAPI 2.6 -- depricated now in favor
 * of MODE_SENSE_POWER_PAGE */
#define GPMODE_CDROM_PAGE		0x0d

/* Some generally useful CD-ROM information */
#define CD_MINS                       80 /* max. minutes per CD */
#define CD_SECS                       60 /* seconds per minute */
#define CD_FRAMES                     75 /* frames per second */
#define CD_FRAMESIZE                2048 /* bytes per frame, "cooked" mode */
#define CD_MAX_BYTES       (CD_MINS * CD_SECS * CD_FRAMES * CD_FRAMESIZE)
#define CD_MAX_SECTORS     (CD_MAX_BYTES / 512)

/* Profile list from MMC-6 revision 1 table 91 */
#define MMC_PROFILE_NONE                0x0000
#define MMC_PROFILE_CD_ROM              0x0008
#define MMC_PROFILE_DVD_ROM             0x0010

#define ATAPI_INT_REASON_CD             0x01 /* 0 = data transfer */
#define ATAPI_INT_REASON_IO             0x02 /* 1 = transfer to the host */
#define ATAPI_INT_REASON_REL            0x04
#define ATAPI_INT_REASON_TAG            0xf8
#define ASC_ILLEGAL_OPCODE                   0x20
#define ASC_LOGICAL_BLOCK_OOR                0x21
#define ASC_INV_FIELD_IN_CMD_PACKET          0x24
#define ASC_MEDIUM_MAY_HAVE_CHANGED          0x28
#define ASC_INCOMPATIBLE_FORMAT              0x30
#define ASC_MEDIUM_NOT_PRESENT               0x3a
#define ASC_SAVING_PARAMETERS_NOT_SUPPORTED  0x39
#define ASC_MEDIA_REMOVAL_PREVENTED          0x53
#define SENSE_NONE            0
#define SENSE_NOT_READY       2
#define SENSE_ILLEGAL_REQUEST 5
#define SENSE_UNIT_ATTENTION  6


#include "ff.h"

#define SECTOR_SIZE      512
#define CD_SECTOR_SIZE   2048

/* Maximum size of ATAPI command packet or small reply (IDENTIFY etc.) */
#define ATAPI_BUF_SIZE   512

typedef struct IDEState IDEState;
typedef void EndTransferFunc(IDEState *);

struct IDEState {
    struct IDEIFState *ide_if;
    enum { IDE_HD, IDE_CD } drive_kind;

    /* geometry (HDD only) */
    int cylinders, heads, sectors;
    int mult_sectors;
    int64_t nb_sectors;     /* in 512-byte units */

    /* IDE registers */
    uint8_t  feature;
    uint8_t  error;
    uint16_t nsector;
    uint8_t  sector;
    uint8_t  lcyl;
    uint8_t  hcyl;
    uint8_t  select;
    uint8_t  status;

    /* ATAPI sense */
    uint8_t  sense_key;
    uint8_t  asc;
    uint8_t  cdrom_changed;

    /* underlying file (FatFS) */
    FIL*     fp;
    int      start_offset;  /* byte offset of data in file (for headered images) */

    /* active data transfer to/from CPU data port */
    int      xfer_left;         /* bytes remaining */
    int      xfer_sectors;      /* sectors in current batch (for accounting in done callback */
    int      xfer_is_write;     /* 1 = CPU is writing (HDD write / ATAPI packet) */
    EndTransferFunc *xfer_done; /* called when xfer_left reaches 0 */

    /* CD-ROM streaming state (set by ide_atapi_cmd_read_pio) */
    int32_t  cd_lba;            /* next LBA to read */
    int32_t  cd_lba_end;        /* LBA past last sector */
    int      cd_sector_off;     /* byte position inside current CD sector */
    int      cd_sector_size;

    /* small ATAPI buffer: receives 12-byte command packet, holds small replies */
    uint8_t  atapi_buf[ATAPI_BUF_SIZE];
    int      atapi_buf_len;     /* valid bytes in atapi_buf (reply mode) */
    int      atapi_buf_pos;     /* CPU read/write position in atapi_buf */
};

struct IDEIFState {
    int irq;
    void *pic;
    void (*set_irq)(void *pic, int irq, int level);
    IDEState *cur_drive;
    IDEState *drives[2];
    uint8_t  cmd;
};

/* -------------------------------------------------------------------------
 * forward declarations
 * ---------------------------------------------------------------------- */
static void ide_atapi_cmd(IDEState *s);
static void ide_sector_read(IDEState *s);
static void ide_sector_write_flush(IDEState *s);
static void ide_sector_read_next(IDEState *s);

/* -------------------------------------------------------------------------
 * helpers
 * ---------------------------------------------------------------------- */
static void padstr(char *str, const char *src, int len)
{
    for (int i = 0; i < len; i++) {
        int v = *src ? *src++ : ' ';
        *(char *)((uintptr_t)str ^ 1) = v;
        str++;
    }
}

static void padstr8(uint8_t *buf, int buf_size, const char *src)
{
    for (int i = 0; i < buf_size; i++)
        buf[i] = *src ? *src++ : ' ';
}

static void stw(uint16_t *buf, int v) { *buf = v; }

static void ide_set_irq(IDEState *s)
{
    struct IDEIFState *ide_if = s->ide_if;
    if (!(ide_if->cmd & IDE_CMD_DISABLE_IRQ)) {
        atapi_tlog("[%d]  irq: st=%02x ns=%02x er=%02x\r\n",
            time_us_32(), s->status, s->nsector, s->error);
        ide_if->set_irq(ide_if->pic, ide_if->irq, 1);
    } else {
        atapi_tlog("[%d]  irq SUPPRESSED (nIEN): st=%02x ns=%02x\r\n",
            time_us_32(), s->status, s->nsector);
    }
}

static void ide_abort_command(IDEState *s)
{
    s->status = READY_STAT | ERR_STAT;
    s->error  = ABRT_ERR;
}

static void ide_set_signature(IDEState *s)
{
    s->select &= 0xf0;
    s->nsector = 1;
    s->sector  = 1;
    if (s->drive_kind == IDE_CD) {
        s->lcyl = 0x14;
        s->hcyl = 0xeb;
    } else {
        s->lcyl = 0;
        s->hcyl = 0;
    }
}

static void ide_transfer_stop(IDEState *s)
{
    int was_write = s->xfer_is_write;
    s->xfer_left     = 0;
    s->xfer_is_write = 0;
    s->xfer_done     = ide_transfer_stop;
    s->status       &= ~DRQ_STAT;
    /* ATAPI: send "command complete" IRQ (IO=1, CoD=1, DRQ=0).
     * Not for packet writes — those go straight into ide_atapi_cmd. */
    if (s->drive_kind == IDE_CD && !was_write) {
        s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO | ATAPI_INT_REASON_CD;
        ide_set_irq(s);
    }
}

/* -------------------------------------------------------------------------
 * ATAPI helpers
 * ---------------------------------------------------------------------- */
static void ide_atapi_cmd_ok(IDEState *s)
{
    s->error     = 0;
    s->status    = READY_STAT | SEEK_STAT;
    s->nsector   = (s->nsector & ~7) | ATAPI_INT_REASON_IO | ATAPI_INT_REASON_CD;
    atapi_tlog("[%d]  -> OK\r\n", time_us_32());
    ide_set_irq(s);
}

static void ide_atapi_cmd_error(IDEState *s, int sense_key, int asc)
{
    s->error     = sense_key << 4;
    s->status    = READY_STAT | ERR_STAT;
    s->nsector   = (s->nsector & ~7) | ATAPI_INT_REASON_IO | ATAPI_INT_REASON_CD;
    s->sense_key = sense_key;
    s->asc       = asc;
    atapi_tlog("[%d]  -> ERR sk=%d asc=%02x\r\n", time_us_32(), sense_key, (unsigned)asc);
    ide_set_irq(s);
}

static void ide_atapi_cmd_check_status(IDEState *s)
{
    atapi_tlog("[%d]  -> CHECK_STATUS (UA blocked)\r\n", time_us_32());
    s->error   = MC_ERR | (SENSE_UNIT_ATTENTION << 4);
    s->status  = ERR_STAT;
    s->nsector = 0;
    ide_set_irq(s);
}

static int64_t ide_nb_sectors(IDEState *s)
{
    if (!s->fp) return 0;
    return (int64_t)(f_size(s->fp) - s->start_offset) / SECTOR_SIZE;
}

/* CD: nb_sectors in 512-byte units → CD-sector count */
static int64_t ide_cd_total_sectors(IDEState *s)
{
    return ide_nb_sectors(s) >> 2;  /* 512→2048 */
}

static int media_present(IDEState *s) { return ide_cd_total_sectors(s) > 0; }
static int media_is_dvd(IDEState *s)  { return media_present(s) && ide_cd_total_sectors(s) > CD_MAX_SECTORS; }
static int media_is_cd(IDEState *s)   { return media_present(s) && ide_cd_total_sectors(s) <= CD_MAX_SECTORS; }

/* -------------------------------------------------------------------------
 * ATAPI reply: fill atapi_buf and arm transfer to CPU
 * ---------------------------------------------------------------------- */
static void ide_atapi_reply_start(IDEState *s, int size, int max_size)
{
    atapi_tlog("[%d]  -> ide_atapi_reply_start(%d, %d)\r\n", time_us_32(), size, max_size);
    if (size > max_size) size = max_size;
    if (size > ATAPI_BUF_SIZE) size = ATAPI_BUF_SIZE;

    /* dump first 20 bytes of buf to verify TOC/reply contents */
    {
        char tb[64]; int i, off=0, tl=size<20?size:20;
        off += snprintf(tb+off,sizeof(tb)-off,"  buf[%d]: ",size);
        for(i=0;i<tl&&off<(int)sizeof(tb)-3;i++)
            off+=snprintf(tb+off,sizeof(tb)-off,"%02x",s->atapi_buf[i]);
        tb[off++]='\r';tb[off++]='\n';tb[off]=0;
        atapi_tlog(tb);
    }

    int byte_count = s->lcyl | (s->hcyl << 8);
    if (byte_count == 0xffff) byte_count--;
    if (byte_count == 0 || byte_count > size) byte_count = size;
    /* make even */
    if (byte_count & 1) byte_count--;

    s->lcyl    = byte_count & 0xff;
    s->hcyl    = byte_count >> 8;
    s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO;
    s->status  = READY_STAT | SEEK_STAT | DRQ_STAT;

    s->atapi_buf_len = size;
    s->atapi_buf_pos = 0;
    s->xfer_left     = size;
    s->xfer_is_write = 0;
    s->xfer_done     = ide_transfer_stop;

    ide_set_irq(s);
}

/* -------------------------------------------------------------------------
 * CD-ROM data read: arm streaming transfer
 * ---------------------------------------------------------------------- */
static void ide_atapi_cmd_read_pio(IDEState *s, int lba, int nb_sectors, int sector_size)
{
    FSIZE_t pos = (FSIZE_t)s->start_offset + (FSIZE_t)lba * sector_size;
    atapi_tlog("[%d]  -> read_pio lba=%d n=%d ss=%d pos=%lu fp=%s\r\n",
        time_us_32(), lba, nb_sectors, sector_size,
        (unsigned long)pos, s->fp ? "ok" : "NULL");
    f_lseek(s->fp, pos);

    s->cd_lba        = lba;
    s->cd_lba_end    = lba + nb_sectors;
    s->cd_sector_off = 0;
    s->cd_sector_size = sector_size;

    s->atapi_buf_len = 0;
    s->atapi_buf_pos = 0;

    int byte_count_limit = s->lcyl | (s->hcyl << 8);
    if (byte_count_limit == 0xffff) byte_count_limit--;
    if (byte_count_limit == 0) byte_count_limit = sector_size;
    if (byte_count_limit & 1) byte_count_limit--;
    if (byte_count_limit > sector_size) byte_count_limit = sector_size;

    s->lcyl    = byte_count_limit & 0xff;
    s->hcyl    = byte_count_limit >> 8;
    s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO;
    s->status  = READY_STAT | SEEK_STAT | DRQ_STAT;

    s->xfer_left     = nb_sectors * sector_size;
    s->xfer_is_write = 0;
    s->xfer_done     = ide_transfer_stop; /* handled inline in readw */

    atapi_tlog("[%d]    bcl=%d xfer_left=%d\r\n",
        time_us_32(), byte_count_limit, s->xfer_left);
    ide_set_irq(s);
}

static void ide_atapi_cmd_read(IDEState *s, int lba, int nb_sectors, int sector_size)
{
    ide_atapi_cmd_read_pio(s, lba, nb_sectors, sector_size);
}

/* -------------------------------------------------------------------------
 * byte helpers
 * ---------------------------------------------------------------------- */
static inline void cpu_to_ube16(uint8_t *buf, int val)
{
    buf[0] = val >> 8;
    buf[1] = val;
}
static inline void cpu_to_ube32(uint8_t *buf, unsigned int val)
{
    buf[0] = val >> 24; buf[1] = val >> 16; buf[2] = val >> 8; buf[3] = val;
}
static inline int ube16_to_cpu(const uint8_t *buf) { return (buf[0] << 8) | buf[1]; }
static inline int ube32_to_cpu(const uint8_t *buf) { return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]; }

static void cpu_to_be32wu(uint32_t *p, uint32_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0]=v>>24; b[1]=v>>16; b[2]=v>>8; b[3]=v;
}
static void cpu_to_be16wu(uint16_t *p, uint16_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0]=v>>8; b[1]=v;
}
static void lba_to_msf(uint8_t *buf, int lba)
{
    lba += 150;
    buf[0] = (lba / 75) / 60;
    buf[1] = (lba / 75) % 60;
    buf[2] =  lba % 75;
}


/* -------------------------------------------------------------------------
 * TOC helpers (unchanged logic, operate on local buf)
 * ---------------------------------------------------------------------- */
static int cdrom_read_toc(int nb_sectors, uint8_t *buf, int msf, int start_track)
{
    atapi_tlog("[%d]  -> cdrom_read_toc(%d, %d, %d)\r\n", time_us_32(), nb_sectors, msf, start_track);
    uint8_t *q;
    int len;

    if (start_track > 1 && start_track != 0xaa) return -1;
    q = buf + 2;
    *q++ = 1; *q++ = 1;
    if (start_track <= 1) {
        *q++ = 0; *q++ = 0x14; *q++ = 1; *q++ = 0;
        if (msf) { *q++ = 0; lba_to_msf(q, 0); q += 3; }
        else {
            atapi_tlog("  toc_lba0 q-buf=%d\r\n", (int)(q-buf));
            *q++=0; *q++=0; *q++=0; *q++=0;
        }
    }
    atapi_tlog("  toc_step1 q-buf=%d [4..11]=%02x%02x%02x%02x %02x%02x%02x%02x\r\n",
        (int)(q-buf),buf[4],buf[5],buf[6],buf[7],buf[8],buf[9],buf[10],buf[11]);
    *q++ = 0; *q++ = 0x16; *q++ = 0xaa; *q++ = 0;
    if (msf) { *q++ = 0; lba_to_msf(q, nb_sectors); q += 3; }
    else { *q++=(nb_sectors>>24)&0xff; *q++=(nb_sectors>>16)&0xff; *q++=(nb_sectors>>8)&0xff; *q++=nb_sectors&0xff; }
    len = q - buf;
    atapi_tlog("  toc_step2 q-buf=%d [12..19]=%02x%02x%02x%02x %02x%02x%02x%02x\r\n",
        (int)(q-buf),buf[12],buf[13],buf[14],buf[15],buf[16],buf[17],buf[18],buf[19]);
    buf[0]=(len-2)>>8; buf[1]=(len-2)&0xff;
    {
        char tb[80]; int i, off=0, tl=len<24?len:24;
        off += snprintf(tb+off,sizeof(tb)-off,"  toc[%d]: ",len);
        for(i=0;i<tl&&off<(int)sizeof(tb)-3;i++)
            off+=snprintf(tb+off,sizeof(tb)-off,"%02x",buf[i]);
        tb[off++]='\r';tb[off++]='\n';tb[off]=0;
        atapi_tlog(tb);
    }
    return len;
}

static int cdrom_read_toc_raw(int nb_sectors, uint8_t *buf, int msf, int session_num)
{
    uint8_t *q = buf + 2;
    int len;
    *q++=1; *q++=1;

    *q++=1; *q++=0x14; *q++=0; *q++=0xa0;
    *q++=0; *q++=0; *q++=0; *q++=0; *q++=1; *q++=0x00; *q++=0x00;

    *q++=1; *q++=0x14; *q++=0; *q++=0xa1;
    *q++=0; *q++=0; *q++=0; *q++=0; *q++=1; *q++=0x00; *q++=0x00;

    *q++=1; *q++=0x14; *q++=0; *q++=0xa2;
    *q++=0; *q++=0; *q++=0;
    if (msf) { *q++=0; lba_to_msf(q, nb_sectors); q+=3; }
    else { *q++=(nb_sectors>>24)&0xff; *q++=(nb_sectors>>16)&0xff; *q++=(nb_sectors>>8)&0xff; *q++=nb_sectors&0xff; }

    *q++=1; *q++=0x14; *q++=0; *q++=1;
    *q++=0; *q++=0; *q++=0;
    if (msf) { *q++=0; lba_to_msf(q,0); q+=3; }
    else { *q++=0; *q++=0; *q++=0; *q++=0; }

    len = q - buf;
    buf[0] = (len-2) >> 8; buf[1] = (len-2) & 0xff;
    return len;
}

static inline uint8_t ide_atapi_set_profile(uint8_t *buf, uint8_t *index, uint16_t profile)
{
    atapi_tlog("[%d]  -> ide_atapi_set_profile(%d)\r\n", time_us_32(), profile);
    uint8_t *p = buf + 12 + (*index) * 4;
    cpu_to_ube16(p, profile);
    p[2] = ((p[0] == buf[6]) && (p[1] == buf[7]));
    (*index)++;
    buf[11] += 4;
    return 4;
}

/* -------------------------------------------------------------------------
 * IDENTIFY buffers
 * ---------------------------------------------------------------------- */
static void ide_identify(IDEState *s)
{
    uint16_t *tab = (uint16_t *)s->atapi_buf;
    memset(tab, 0, 512);
    stw(tab+0,  0x0040);
    stw(tab+1,  s->cylinders);
    stw(tab+3,  s->heads);
    stw(tab+4,  512 * s->sectors);
    stw(tab+5,  512);
    stw(tab+6,  s->sectors);
    stw(tab+20, 3);
    stw(tab+21, 512);
    stw(tab+22, 4);
    padstr((char *)(tab+27), "TINY386 HARDDISK", 40);
    stw(tab+47, 0x8000 | MAX_MULT_SECTORS);
    stw(tab+48, 1);
    stw(tab+49, 1<<9);
    stw(tab+51, 0x200);
    stw(tab+52, 0x200);
    stw(tab+54, s->cylinders);
    stw(tab+55, s->heads);
    stw(tab+56, s->sectors);
    uint32_t oldsize = s->cylinders * s->heads * s->sectors;
    stw(tab+57, oldsize);
    stw(tab+58, oldsize >> 16);
    if (s->mult_sectors) stw(tab+59, 0x100 | s->mult_sectors);
    stw(tab+60, s->nb_sectors);
    stw(tab+61, s->nb_sectors >> 16);
    stw(tab+80, (1<<1)|(1<<2));
    stw(tab+82, 1<<14);
    stw(tab+83, 1<<14);
    stw(tab+84, 1<<14);
    stw(tab+85, 1<<14);
    stw(tab+86, 0);
    stw(tab+87, 1<<14);
}

static void ide_atapi_identify(IDEState *s)
{
    uint16_t *tab = (uint16_t *)s->atapi_buf;
    memset(tab, 0, 512);
    stw(tab+0,  (2<<14)|(5<<8)|(1<<7)|(2<<5)|(0<<0));
    stw(tab+20, 3);
    stw(tab+21, 512);
    stw(tab+22, 4);
    padstr((char *)(tab+27), "TINY386 CD-ROM", 40);
    stw(tab+48, 1);
    stw(tab+49, 1<<9);
    stw(tab+53, 3);
    stw(tab+63, 0x103);
    stw(tab+64, 1);
    stw(tab+65, 0xb4);
    stw(tab+66, 0xb4);
    stw(tab+67, 0x12c);
    stw(tab+68, 0xb4);
    stw(tab+71, 30);
    stw(tab+72, 30);
    stw(tab+80, 0x1e);
}

/* -------------------------------------------------------------------------
 * HDD sector read / write
 * ---------------------------------------------------------------------- */
static int64_t ide_get_sector(IDEState *s)
{
    int64_t sector_num;
    if (s->select & 0x40) {
        sector_num = ((s->select & 0x0f) << 24) | (s->hcyl << 16) | (s->lcyl << 8) | s->sector;
    } else {
        sector_num = ((s->hcyl << 8) | s->lcyl) * s->heads * s->sectors
                   + (s->select & 0x0f) * s->sectors
                   + (s->sector - 1);
    }
    return sector_num;
}

static void ide_set_sector(IDEState *s, int64_t sector_num)
{
    if (s->select & 0x40) {
        s->select = (s->select & 0xf0) | (sector_num >> 24);
        s->hcyl   = (sector_num >> 16) & 0xff;
        s->lcyl   = (sector_num >> 8)  & 0xff;
        s->sector =  sector_num        & 0xff;
    } else {
        uint32_t cyl = sector_num / (s->heads * s->sectors);
        uint32_t r   = sector_num % (s->heads * s->sectors);
        s->hcyl   = cyl >> 8;
        s->lcyl   = cyl & 0xff;
        s->select = (s->select & 0xf0) | ((r / s->sectors) & 0x0f);
        s->sector = (r % s->sectors) + 1;
    }
}

/* Called when CPU finishes reading a batch; continue or done */
static void ide_sector_read_next(IDEState *s)
{
    int n = s->xfer_sectors;
    ide_set_sector(s, ide_get_sector(s) + n);
    s->nsector = (s->nsector - n) & 0xff;
    if (s->nsector == 0) {
        s->status = READY_STAT | SEEK_STAT;
        ide_transfer_stop(s);
    } else {
        ide_sector_read(s);
    }
}

static void ide_sector_read(IDEState *s)
{
    int64_t sector_num = ide_get_sector(s);
    int n = s->nsector ? s->nsector : 256;
    int max = s->mult_sectors ? s->mult_sectors : 1;
    if (n > max) n = max;

    FSIZE_t pos = (FSIZE_t)s->start_offset + (FSIZE_t)sector_num * SECTOR_SIZE;
    f_lseek(s->fp, pos);

    s->xfer_sectors  = n;
    s->xfer_left     = n * SECTOR_SIZE;
    s->xfer_is_write = 0;
    s->xfer_done     = ide_sector_read_next;
    s->status        = READY_STAT | SEEK_STAT | DRQ_STAT;
    ide_set_irq(s);
}

/* Called when CPU finishes writing a batch of sectors; sync and continue or done */
static void ide_sector_write_flush(IDEState *s)
{
    /* advance position by the batch we just received */
    int n = s->xfer_sectors;
    f_sync(s->fp);
    ide_set_sector(s, ide_get_sector(s) + n);
    s->nsector = (s->nsector - n) & 0xff;

    if (s->nsector == 0) {
        s->status = READY_STAT | SEEK_STAT;
        ide_transfer_stop(s);
        ide_set_irq(s);
    } else {
        /* more sectors to write: CPU writes next batch */
        int m = s->nsector ? s->nsector : 256;
        int max = s->mult_sectors ? s->mult_sectors : 1;
        if (m > max) m = max;
        s->xfer_sectors  = m;
        s->xfer_left     = m * SECTOR_SIZE;
        s->xfer_is_write = 1;
        s->xfer_done     = ide_sector_write_flush;
        s->status        = READY_STAT | SEEK_STAT | DRQ_STAT;
        ide_set_irq(s);
    }
}

static void ide_sector_write(IDEState *s)
{
    int64_t sector_num = ide_get_sector(s);
    int n = s->nsector ? s->nsector : 256;
    int max = s->mult_sectors ? s->mult_sectors : 1;
    if (n > max) n = max;

    FSIZE_t pos = (FSIZE_t)s->start_offset + (FSIZE_t)sector_num * SECTOR_SIZE;
    f_lseek(s->fp, pos);

    s->xfer_sectors  = n;
    s->xfer_left     = n * SECTOR_SIZE;
    s->xfer_is_write = 1;
    s->xfer_done     = ide_sector_write_flush;
    s->status        = READY_STAT | SEEK_STAT | DRQ_STAT;
}

static void ide_identify_cb(IDEState *s)
{
    ide_transfer_stop(s);
    s->status = READY_STAT;
}

/* -------------------------------------------------------------------------
 * ATAPI command handler
 * ---------------------------------------------------------------------- */
static inline void atapi_tlog_cmd(const uint8_t *pkt, int sk, int fp_ok, long secs) {
    atapi_tlog("[%d] CMD %02x [%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x] sk=%d fp=%d sec=%ld\r\n",
        time_us_32(),
        pkt[0],
        pkt[1],pkt[2],pkt[3],pkt[4],
        pkt[5],pkt[6],pkt[7],pkt[8],
        pkt[9],pkt[10],pkt[11],
        sk, fp_ok, secs);
}
/* ---- end trace ---- */

static void ide_atapi_cmd(IDEState *s)
{
    const uint8_t *packet = s->atapi_buf;
    uint8_t *buf = s->atapi_buf;   /* reply goes into same buffer */
    int max_len;

    atapi_tlog_cmd(packet, s->sense_key, s->fp ? 1 : 0,
                   (long)ide_cd_total_sectors(s));

    switch (packet[0]) {
    case GPCMD_TEST_UNIT_READY:
        if (!s->cdrom_changed) {
            if (s->fp && ide_cd_total_sectors(s) > 0)
                ide_atapi_cmd_ok(s);
            else
                ide_atapi_cmd_error(s, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT);
        } else {
            s->cdrom_changed = 0;
            if (s->fp && ide_cd_total_sectors(s) > 0)
                ide_atapi_cmd_error(s, SENSE_UNIT_ATTENTION, ASC_MEDIUM_MAY_HAVE_CHANGED);
            else
                ide_atapi_cmd_error(s, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT);
        }
    atapi_tlog("[%d]  -> b GPCMD_TEST_UNIT_READY\r\n", time_us_32());
        break;

    case GPCMD_MODE_SENSE_6:
    case GPCMD_MODE_SENSE_10:
        {
            int action, code;
            if (packet[0] == GPCMD_MODE_SENSE_10)
                max_len = ube16_to_cpu(packet + 7);
            else
                max_len = packet[4];
            action = packet[2] >> 6;
            code   = packet[2] & 0x3f;
            switch (action) {
            case 0: /* current values */
                switch (code) {
                case 0x01: /* error recovery */
                    cpu_to_ube16(buf, 16 + 6);
                    buf[2] = 0x70; buf[3] = 0; buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 0;
                    buf[8] = 0x01; buf[9] = 0x06;
                    buf[10] = 0x00; buf[11] = 0x05;
                    buf[12] = 0x00; buf[13] = 0x00; buf[14] = 0x00; buf[15] = 0x00;
                    ide_atapi_reply_start(s, 16, max_len);
    atapi_tlog("[%d]  -> b00 GPCMD_MODE_SENSE_6/10\r\n", time_us_32());
                    break;
                case 0x2a: /* CD capabilities */
                    cpu_to_ube16(buf, 28 + 6);
                    buf[2] = 0x70; buf[3] = 0; buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 0;
                    buf[8] = 0x2a; buf[9] = 0x12;
                    buf[10] = 0x00; buf[11] = 0x00;
                    buf[12] = 0x71; buf[13] = 3 << 5;
                    buf[14] = 0x29; buf[15] = 0x00;
                    cpu_to_ube16(buf+16, 706);
                    buf[18] = 0; buf[19] = 2;
                    cpu_to_ube16(buf+20, 512);
                    cpu_to_ube16(buf+22, 706);
                    buf[24] = 0; buf[25] = 0; buf[26] = 0; buf[27] = 0;
                    ide_atapi_reply_start(s, 28, max_len);
    atapi_tlog("[%d]  -> b01 GPCMD_MODE_SENSE_6/10\r\n", time_us_32());
                    break;
                default:
    atapi_tlog("[%d]  -> e1 GPCMD_MODE_SENSE_6/10\r\n", time_us_32());
                    goto error_cmd;
                }
    atapi_tlog("[%d]  -> b1 GPCMD_MODE_SENSE_6/10\r\n", time_us_32());
                break;
            default:
    atapi_tlog("[%d]  -> e2 GPCMD_MODE_SENSE_6/10\r\n", time_us_32());
                goto error_cmd;
            }
        }
    atapi_tlog("[%d]  -> b2 GPCMD_MODE_SENSE_6/10\r\n", time_us_32());
        break;

    case GPCMD_REQUEST_SENSE:
        max_len = packet[4];
        memset(buf, 0, 18);
        buf[0] = 0x70 | (1 << 7);
        buf[2] = s->sense_key;
        buf[7] = 10;
        buf[12] = s->asc;
        atapi_tlog("[%d]  -> b GPCMD_REQUEST_SENSE (sk=%d asc=%02x)\r\n",
            time_us_32(), buf[2], buf[12]);
        /* only consume UA — other sense codes (e.g. ILLEGAL_REQUEST) persist */
        if (s->sense_key == SENSE_UNIT_ATTENTION) {
            s->sense_key     = SENSE_NONE;
            s->asc           = 0;
            s->cdrom_changed = 0;
        }
        ide_atapi_reply_start(s, 18, max_len);
        break;

    case GPCMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        ide_atapi_cmd_ok(s);
    atapi_tlog("[%d]  -> b GPCMD_PREVENT_ALLOW_MEDIUM_REMOVAL\r\n", time_us_32());
        break;

    case GPCMD_READ_10:
    case GPCMD_READ_12:
        {
            int lba, nb_sectors;
            if (!s->fp || ide_cd_total_sectors(s) == 0) {
                ide_atapi_cmd_error(s, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT);
    atapi_tlog("[%d]  -> b0 GPCMD_READ_10/12\r\n", time_us_32());
                break;
            }
            if (packet[0] == GPCMD_READ_10)
                nb_sectors = ube16_to_cpu(packet + 7);
            else
                nb_sectors = ube32_to_cpu(packet + 6);
            lba = ube32_to_cpu(packet + 2);
            atapi_tlog("[%d]   READ lba=%d n=%d\r\n", time_us_32(), lba, nb_sectors);
            if (nb_sectors == 0) {
                ide_atapi_cmd_ok(s);
    atapi_tlog("[%d]  -> b! GPCMD_READ_10/12 (nb_sectors == 0)\r\n", time_us_32());
                break;
            }
            ide_atapi_cmd_read(s, lba, nb_sectors, CD_SECTOR_SIZE);
        }
    atapi_tlog("[%d]  -> b1 GPCMD_READ_10/12\r\n", time_us_32());
        break;

    case GPCMD_READ_CD:
        {
            int lba, nb_sectors, transfer_request;
            if (!s->fp || ide_cd_total_sectors(s) == 0) {
                ide_atapi_cmd_error(s, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT);
    atapi_tlog("[%d]  -> b0 GPCMD_READ_CD\r\n", time_us_32());
                break;
            }
            lba              = ube32_to_cpu(packet + 2);
            nb_sectors       = (packet[6]<<16)|(packet[7]<<8)|packet[8];
            transfer_request = packet[9];
            switch (transfer_request & 0xf8) {
            case 0x00:
                ide_atapi_cmd_ok(s);
    atapi_tlog("[%d]  -> b10 GPCMD_READ_CD\r\n", time_us_32());
                break;
            case 0x10: /* user data only */
                ide_atapi_cmd_read(s, lba, nb_sectors, CD_SECTOR_SIZE);
    atapi_tlog("[%d]  -> b11 GPCMD_READ_CD\r\n", time_us_32());
                break;
            default:
                ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
    atapi_tlog("[%d]  -> b12 GPCMD_READ_CD\r\n", time_us_32());
                break;
            }
        }
    atapi_tlog("[%d]  -> b2 GPCMD_READ_CD\r\n", time_us_32());
        break;

    case GPCMD_SEEK:
        ide_atapi_cmd_ok(s);
    atapi_tlog("[%d]  -> b GPCMD_SEEK\r\n", time_us_32());
        break;

    case GPCMD_START_STOP_UNIT:
        ide_atapi_cmd_ok(s);
    atapi_tlog("[%d]  -> b GPCMD_START_STOP_UNIT\r\n", time_us_32());
        break;

    case GPCMD_MECHANISM_STATUS:
        max_len = ube16_to_cpu(packet + 8);
        memset(buf, 0, 8);
        cpu_to_ube16(buf, 0);
        buf[5] = 1;
        cpu_to_ube16(buf + 6, 0);
        ide_atapi_reply_start(s, 8, max_len);
    atapi_tlog("[%d]  -> b GPCMD_MECHANISM_STATUS\r\n", time_us_32());
        break;

    case GPCMD_READ_TOC_PMA_ATIP:
        {
            int format, msf, start_track, len;
            int64_t total_sectors = ide_cd_total_sectors(s);
            if (total_sectors == 0) {
                ide_atapi_cmd_error(s, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT);
    atapi_tlog("[%d]  -> b0 GPCMD_READ_TOC_PMA_ATIP\r\n", time_us_32());
                break;
            }
            max_len     = ube16_to_cpu(packet + 7);
            format      = packet[9] >> 6;
            msf         = (packet[1] >> 1) & 1;
            start_track = packet[6];
            switch (format) {
            case 0:
                len = cdrom_read_toc(total_sectors, buf, msf, start_track);
                if (len < 0) goto error_cmd;
                /* clamp length field to what we actually send */
                if (len > max_len) {
                    buf[0] = ((max_len-2) >> 8) & 0xff;
                    buf[1] =  (max_len-2) & 0xff;
                }
                ide_atapi_reply_start(s, len, max_len);
    atapi_tlog("[%d]  -> b10 GPCMD_READ_TOC_PMA_ATIP\r\n", time_us_32());
                break;
            case 1:
                memset(buf, 0, 12);
                buf[1]=0x0a; buf[2]=0x01; buf[3]=0x01;
                ide_atapi_reply_start(s, 12, max_len);
    atapi_tlog("[%d]  -> b11 GPCMD_READ_TOC_PMA_ATIP\r\n", time_us_32());
                break;
            case 2:
                len = cdrom_read_toc_raw(total_sectors, buf, msf, start_track);
                if (len < 0) goto error_cmd;
                ide_atapi_reply_start(s, len, max_len);
    atapi_tlog("[%d]  -> b12 GPCMD_READ_TOC_PMA_ATIP\r\n", time_us_32());
                break;
            default:
    atapi_tlog("[%d]  -> e GPCMD_READ_TOC_PMA_ATIP\r\n", time_us_32());
                goto error_cmd;
            }
        }
    atapi_tlog("[%d]  -> b2 GPCMD_READ_TOC_PMA_ATIP\r\n", time_us_32());
        break;

    case GPCMD_READ_SUBCHANNEL:
        ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST,
                            ASC_ILLEGAL_OPCODE);
        atapi_tlog("[%d]  -> UNKNOWN CMD 42\r\n", time_us_32());
        break;
    case GPCMD_READ_CDVD_CAPACITY:
        {
            int64_t total_sectors = ide_cd_total_sectors(s);
            if (total_sectors == 0) {
                ide_atapi_cmd_error(s, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT);
    atapi_tlog("[%d]  -> b0 GPCMD_READ_CDVD_CAPACITY\r\n", time_us_32());
                break;
            }
            cpu_to_ube32(buf,     total_sectors - 1);
            cpu_to_ube32(buf + 4, CD_SECTOR_SIZE);
            ide_atapi_reply_start(s, 8, 8);
        }
    atapi_tlog("[%d]  -> b1 GPCMD_READ_CDVD_CAPACITY\r\n", time_us_32());
        break;

    case GPCMD_SET_SPEED:
        ide_atapi_cmd_ok(s);
    atapi_tlog("[%d]  -> b GPCMD_SET_SPEED\r\n", time_us_32());
        break;

    case GPCMD_INQUIRY:
        max_len = packet[4];
        buf[0] = 0x05; buf[1] = 0x80; buf[2] = 0x00; buf[3] = 0x21;
        buf[4] = 31;   buf[5] = 0;    buf[6] = 0;    buf[7] = 0;
        padstr8(buf+8,  8,  "TINY386");
        padstr8(buf+16, 16, "TINY386 CD-ROM");
        padstr8(buf+32, 4,  "0.1");
        ide_atapi_reply_start(s, 36, max_len);
    atapi_tlog("[%d]  -> b GPCMD_INQUIRY\r\n", time_us_32());
        break;

    case GPCMD_GET_CONFIGURATION:
        {
            uint8_t index = 0;
            max_len = ube16_to_cpu(packet + 7);
            if (packet[2] != 0 || packet[3] != 0) {
                ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
    atapi_tlog("[%d]  -> b0 GPCMD_GET_CONFIGURATION\r\n", time_us_32());
                break;
            }
            if (max_len > ATAPI_BUF_SIZE) max_len = ATAPI_BUF_SIZE;
            memset(buf, 0, max_len);
            if (media_is_dvd(s))     cpu_to_ube16(buf+6, MMC_PROFILE_DVD_ROM);
            else if (media_is_cd(s)) cpu_to_ube16(buf+6, MMC_PROFILE_CD_ROM);
            buf[10] = 0x02 | 0x01;
            uint32_t len = 12;
            len += ide_atapi_set_profile(buf, &index, MMC_PROFILE_DVD_ROM);
            len += ide_atapi_set_profile(buf, &index, MMC_PROFILE_CD_ROM);
            cpu_to_ube32(buf, len - 4);
            ide_atapi_reply_start(s, len, max_len);
        }
    atapi_tlog("[%d]  -> b1 GPCMD_GET_CONFIGURATION\r\n", time_us_32());
        break;

    case GPCMD_GET_EVENT_STATUS_NOTIFICATION:
        max_len = ube16_to_cpu(packet + 7);
        if (packet[1] & 1) { /* polled */
            buf[0] = 0x00; buf[1] = 0x06; buf[2] = 0x00; buf[3] = 0x10;
            buf[4] = 0x00; buf[5] = 0x00; buf[6] = 0x00; buf[7] = 0x00;
            ide_atapi_reply_start(s, 8, max_len);
        } else {
            ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
        }
    atapi_tlog("[%d]  -> b GPCMD_GET_EVENT_STATUS_NOTIFICATION\r\n", time_us_32());
        break;

    case GPCMD_READ_DISC_INFO:
        ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_ILLEGAL_OPCODE);
    atapi_tlog("[%d]  -> b GPCMD_READ_DISC_INFO\r\n", time_us_32());
        break;

    case GPCMD_MODE_SELECT_10:
        ide_atapi_cmd_ok(s);
    atapi_tlog("[%d]  -> b GPCMD_MODE_SELECT_10\r\n", time_us_32());
        break;

    default:
    error_cmd:
        ide_atapi_cmd_error(s, SENSE_ILLEGAL_REQUEST, ASC_ILLEGAL_OPCODE);
    atapi_tlog("[%d]  -> b d e\r\n", time_us_32());
        break;
    }
}


/* -------------------------------------------------------------------------
 * IDE exec command dispatch
 * ---------------------------------------------------------------------- */
static void ide_exec_cmd(IDEState *s, int val)
{
    switch (val) {
    case WIN_IDENTIFY:
        ide_identify(s);
        s->status = READY_STAT | SEEK_STAT | DRQ_STAT;
        s->atapi_buf_len = 512;
        s->atapi_buf_pos = 0;
        s->xfer_left     = 512;
        s->xfer_is_write = 0;
        s->xfer_done     = ide_identify_cb;
        ide_set_irq(s);
        break;
    case WIN_SPECIFY:
    case WIN_RECAL:
        s->error  = 0;
        s->status = READY_STAT | SEEK_STAT;
        ide_set_irq(s);
        break;
    case WIN_SETMULT:
        if (s->nsector > MAX_MULT_SECTORS || (s->nsector & (s->nsector - 1)) != 0)
            ide_abort_command(s);
        else
            s->mult_sectors = s->nsector;
        s->status = READY_STAT;
        ide_set_irq(s);
        break;
    case WIN_READ:
    case WIN_READ_ONCE:
        s->mult_sectors = 1;
        ide_sector_read(s);
        break;
    case WIN_WRITE:
    case WIN_WRITE_ONCE:
        s->mult_sectors = 1;
        ide_sector_write(s);
        break;
    case WIN_MULTREAD:
        if (!s->mult_sectors) { ide_abort_command(s); ide_set_irq(s); break; }
        ide_sector_read(s);
        break;
    case WIN_MULTWRITE:
        if (!s->mult_sectors) { ide_abort_command(s); ide_set_irq(s); break; }
        ide_sector_write(s);
        break;
    case WIN_READ_NATIVE_MAX:
        ide_set_sector(s, s->nb_sectors - 1);
        s->status = READY_STAT | SEEK_STAT;
        ide_set_irq(s);
        break;
    case WIN_CHECKPOWERMODE1:
    case WIN_CHECKPOWERMODE2:
        s->nsector = 0xff;
        s->status  = READY_STAT | SEEK_STAT;
        ide_set_irq(s);
        break;
    case WIN_SETFEATURES:
        s->status = READY_STAT | SEEK_STAT;
        ide_set_irq(s);
        break;
    default:
        ide_abort_command(s);
        ide_set_irq(s);
        break;
    }
}

static void idecd_exec_cmd(IDEState *s, int val)
{
    atapi_tlog("[%d] IDECD_CMD 0x%02x\r\n", time_us_32(), val);
    switch (val) {
    case WIN_DEVICE_RESET:
        ide_set_signature(s);
        s->status = READY_STAT | SEEK_STAT;
        s->error  = 0x01;  /* diagnostic code: drive OK */
        ide_set_irq(s);
        break;
    case WIN_PACKETCMD:
        /* arm transfer: CPU writes 12-byte ATAPI packet into atapi_buf */
        s->atapi_buf_pos = 0;
        s->xfer_left     = ATAPI_PACKET_SIZE;
        s->xfer_is_write = 1;
        s->xfer_done     = ide_atapi_cmd;
        s->status        = READY_STAT | SEEK_STAT | DRQ_STAT;
        s->nsector        = 1;
        ide_set_irq(s);
        break;
    case WIN_PIDENTIFY:
        ide_atapi_identify(s);
        s->status        = READY_STAT | SEEK_STAT | DRQ_STAT;
        s->atapi_buf_len = 512;
        s->atapi_buf_pos = 0;
        s->xfer_left     = 512;
        s->xfer_is_write = 0;
        s->xfer_done     = ide_transfer_stop;
        ide_set_irq(s);
        break;
    case WIN_IDENTIFY:
    case WIN_READ:
        ide_set_signature(s);
        /* fall through */
    default:
        ide_abort_command(s);
        ide_set_irq(s);
        break;
    }
}

/* -------------------------------------------------------------------------
 * I/O port handlers
 * ---------------------------------------------------------------------- */
void ide_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    IDEIFState *s1 = opaque;
    IDEState *s = s1->cur_drive;

    switch (addr) {
    case 0: break;
    case 1:
        if (s) s->feature = val;
        break;
    case 2:
        if (s) s->nsector = val;
        break;
    case 3:
        if (s) s->sector = val;
        break;
    case 4:
        if (s) s->lcyl = val;
        break;
    case 5:
        if (s) s->hcyl = val;
        break;
    case 6:
        s1->cur_drive = s1->drives[(val >> 4) & 1];
        if (s1->cur_drive)
            s1->cur_drive->select = val | 0xa0;
        break;
    default:
    case 7: /* command */
        if (!s) break;
        if (s->drive_kind == IDE_CD)
            idecd_exec_cmd(s, val);
        else
            ide_exec_cmd(s, val);
        break;
    }
}

uint32_t ide_ioport_read(void *opaque, uint32_t addr)
{
    IDEIFState *s1 = opaque;
    IDEState *s = s1->cur_drive;
    int ret;

    if (!s) { ret = 0x00; }
    else switch (addr) {
    case 0:  ret = 0xff; break;
    case 1:  ret = s->error; break;
    case 2:  ret = s->nsector; break;
    case 3:  ret = s->sector; break;
    case 4:  ret = s->lcyl; break;
    case 5:  ret = s->hcyl; break;
    case 6:  ret = s->select; break;
    default:
    case 7:
        ret = s->status;
        if (s->drive_kind == IDE_CD)
            atapi_tlog("[%d]  rd_st=%02x ns=%02x er=%02x\r\n",
                time_us_32(), ret, s->nsector, s->error);
        s1->set_irq(s1->pic, s1->irq, 0);
        break;
    }
    return ret;
}

uint32_t ide_status_read(void *opaque)
{
    IDEIFState *s1 = opaque;
    IDEState *s = s1->cur_drive;
    return s ? s->status : 0;
}

void ide_cmd_write(void *opaque, uint32_t val)
{
    IDEIFState *s1 = opaque;
    atapi_tlog("[%d]  devctrl=0x%02x (nIEN=%d SRST=%d)\r\n",
        time_us_32(), (unsigned)val, (val>>1)&1, (val>>2)&1);
    if (!(s1->cmd & IDE_CMD_RESET) && (val & IDE_CMD_RESET)) {
        for (int i = 0; i < 2; i++) {
            IDEState *s = s1->drives[i];
            if (s) { s->status = BUSY_STAT | SEEK_STAT; s->error = 0x01; }
        }
    } else if ((s1->cmd & IDE_CMD_RESET) && !(val & IDE_CMD_RESET)) {
        for (int i = 0; i < 2; i++) {
            IDEState *s = s1->drives[i];
            if (s) { s->status = READY_STAT | SEEK_STAT; ide_set_signature(s); }
        }
    }
    s1->cmd = val;
}

/* -------------------------------------------------------------------------
 * Data port: CPU reads/writes 2 or 4 bytes at a time.
 *
 * Three active modes, selected by what armed the transfer:
 *   1. ATAPI small buffer (atapi_buf_pos / atapi_buf_len): reply or packet.
 *   2. CD streaming: f_read directly from open FIL.
 *   3. HDD: f_read / f_write directly.
 *
 * All three share xfer_left/xfer_done.
 * ---------------------------------------------------------------------- */

/* read 2 bytes from current transfer source */
/* Read n bytes from current transfer: file (HD/CD stream) or atapi_buf (reply/packet) */
static inline int xfer_from_file(IDEState *s)
{
    /* Read from file only during actual sector transfers (xfer_done points to
     * sector-read continuation).  IDENTIFY and other small replies live in
     * atapi_buf regardless of drive_kind. */
    if (s->drive_kind == IDE_CD)
        return s->cd_lba < s->cd_lba_end && s->atapi_buf_len == 0;
    /* IDE_HD: file transfer only when xfer_done is the sector-read callback,
     * not when it's ide_identify_cb or ide_transfer_stop (small buffer). */
    return s->xfer_done == ide_sector_read_next;
}

static uint16_t xfer_read16(IDEState *s)
{
    uint8_t buf[2]; UINT br;
    if (xfer_from_file(s)) {
        f_read(s->fp, buf, 2, &br);
        /* log first word of each sector (xfer_left is multiple of sector_size at start) */
        if (s->drive_kind == IDE_CD && s->xfer_left % s->cd_sector_size == s->cd_sector_size - 2)
            atapi_tlog("[%d]  xfer16 lba=%d w0=%02x%02x br=%d\r\n",
                time_us_32(), s->cd_lba, buf[0], buf[1], br);
        return buf[0] | (buf[1] << 8);
    } else {
        if (s->atapi_buf_pos + 1 >= s->atapi_buf_len) return 0;
        uint16_t v = s->atapi_buf[s->atapi_buf_pos] | (s->atapi_buf[s->atapi_buf_pos+1] << 8);
        s->atapi_buf_pos += 2;
        return v;
    }
}

static uint32_t xfer_read32(IDEState *s)
{
    uint8_t buf[4]; UINT br;
    if (xfer_from_file(s)) {
        f_read(s->fp, buf, 4, &br);
        return buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24);
    } else {
        if (s->atapi_buf_pos + 3 >= s->atapi_buf_len) return 0;
        uint32_t v = s->atapi_buf[s->atapi_buf_pos]
                   | (s->atapi_buf[s->atapi_buf_pos+1] << 8)
                   | (s->atapi_buf[s->atapi_buf_pos+2] << 16)
                   | (s->atapi_buf[s->atapi_buf_pos+3] << 24);
        s->atapi_buf_pos += 4;
        return v;
    }
}

static void xfer_advance(IDEState *s, int n)
{
    s->xfer_left -= n;
    if (s->xfer_left <= 0) {
        if (s->drive_kind == IDE_CD && !s->xfer_is_write)
            atapi_tlog("[%d]  xfer_done lba=%d..%d was_write=%d\r\n",
                time_us_32(), s->cd_lba, s->cd_lba_end, s->xfer_is_write);
        if (!s->xfer_is_write && s->atapi_buf_len > 0) {
            s->atapi_buf_len = 0;
            s->atapi_buf_pos = 0;
        }
        s->xfer_left = 0;
        EndTransferFunc *fn = s->xfer_done;
        ide_transfer_stop(s);
        if (fn && fn != ide_transfer_stop) fn(s);
    } else if (s->drive_kind == IDE_CD && !s->xfer_is_write
               && s->cd_sector_size > 0
               && (s->xfer_left % s->cd_sector_size) == 0) {
        /* Host has consumed exactly one CD sector.
         * Advance the logical LBA counter and re-arm DRQ so the host
         * knows the next sector is ready to be read.
         * Without this, xfer_left silently counts down but no new IRQ is
         * ever raised, causing the host to time out on any n > 1 read. */
        s->cd_lba++;
        int byte_count_limit = s->lcyl | (s->hcyl << 8);
        if (byte_count_limit == 0 || byte_count_limit > s->cd_sector_size)
            byte_count_limit = s->cd_sector_size;
        if (byte_count_limit & 1) byte_count_limit--;
        s->lcyl    = byte_count_limit & 0xff;
        s->hcyl    = byte_count_limit >> 8;
        s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO; /* Data-In, DRQ */
        s->status  = READY_STAT | SEEK_STAT | DRQ_STAT;
        atapi_tlog("[%d]  xfer_sector_done lba=%d remaining=%d\r\n",
            time_us_32(), s->cd_lba, s->xfer_left);
        ide_set_irq(s);
    }
}

void ide_data_writew(void *opaque, uint32_t val)
{
    IDEIFState *s1 = opaque;
    IDEState *s = s1->cur_drive;
    if (!s || s->xfer_left < 2 || !s->xfer_is_write) return;

    if (s->drive_kind == IDE_HD) {
        uint8_t buf[2] = { val & 0xff, (val >> 8) & 0xff };
        UINT bw; f_write(s->fp, buf, 2, &bw);
    } else {
        /* ATAPI packet receive */
        s->atapi_buf[s->atapi_buf_pos++] = val & 0xff;
        s->atapi_buf[s->atapi_buf_pos++] = (val >> 8) & 0xff;
    }
    xfer_advance(s, 2);
}

uint32_t ide_data_readw(void *opaque)
{
    IDEIFState *s1 = opaque;
    IDEState *s = s1->cur_drive;
    if (!s || s->xfer_left < 2 || s->xfer_is_write) return 0;
    uint16_t v = xfer_read16(s);
    xfer_advance(s, 2);
    return v;
}

void ide_data_writel(void *opaque, uint32_t val)
{
    IDEIFState *s1 = opaque;
    IDEState *s = s1->cur_drive;
    if (!s || s->xfer_left < 4 || !s->xfer_is_write) return;

    if (s->drive_kind == IDE_HD) {
        uint8_t buf[4] = { val, val>>8, val>>16, val>>24 };
        UINT bw; f_write(s->fp, buf, 4, &bw);
    } else {
        s->atapi_buf[s->atapi_buf_pos++] = val;
        s->atapi_buf[s->atapi_buf_pos++] = val >> 8;
        s->atapi_buf[s->atapi_buf_pos++] = val >> 16;
        s->atapi_buf[s->atapi_buf_pos++] = val >> 24;
    }
    xfer_advance(s, 4);
}

uint32_t ide_data_readl(void *opaque)
{
    IDEIFState *s1 = opaque;
    IDEState *s = s1->cur_drive;
    if (!s || s->xfer_left < 4 || s->xfer_is_write) return 0;
    uint32_t v = xfer_read32(s);
    xfer_advance(s, 4);
    return v;
}

int ide_data_write_string(void *opaque, uint32_t addr, int size, int count)
{
    IDEIFState *s1 = opaque;
    IDEState *s = s1->cur_drive;
    if (!s || !s->xfer_is_write) return 0;
    int len = size * count;
    if (len > s->xfer_left) len = s->xfer_left;
    len -= len % size;
    if (s->drive_kind == IDE_HD) {
        UINT bw;
        uint8_t buf[512];
        for (int i = 0; i < len; i += 512) {
            UINT l = len - i;
            if (l > 512) l = 512;
            for (int j = 0; j < l; ++j) {
                buf[j] = pload8(addr + i + j);
            }
            f_write(s->fp, buf, l, &bw);
        }
    } else {
        for (int i = 0; i < len; ++i) {
            s->atapi_buf[s->atapi_buf_pos + i] = pload8(addr + i);
        }
        s->atapi_buf_pos += len;
    }
    xfer_advance(s, len);
    return len / size;
}

int ide_data_read_string(void *opaque, uint32_t addr, int size, int count)
{
    IDEIFState *s1 = opaque;
    IDEState *s = s1->cur_drive;
    if (!s || s->xfer_is_write) return 0;
    int len = size * count;
    if (len > s->xfer_left) len = s->xfer_left;
    len -= len % size;
    if (xfer_from_file(s)) {
        UINT br;
        uint8_t buf[512];
        for (int i = 0; i < len; i += 512) {
            UINT l = len - i;
            if (l > 512) l = 512;
            f_read(s->fp, buf, l, &br);
            for (int j = 0; j < l; ++j) {
                pstore8(addr + i + j, buf[j]);
            }
        }
    } else {
        for (int i = 0; i < len; ++i) {
            pstore8(addr + i, s->atapi_buf[s->atapi_buf_pos + i]);
        }
        s->atapi_buf_pos += len;
    }
    xfer_advance(s, len);
    return len / size;
}


/* -------------------------------------------------------------------------
 * Drive init helpers
 * ---------------------------------------------------------------------- */
static IDEState *ide_hddrive_init(IDEIFState *ide_if, FIL *f, int f_open,
                                   int64_t nb_sectors, int start_offset,
                                   int cylinders, int heads, int sectors)
{
    IDEState *s = malloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));

    s->ide_if     = ide_if;
    s->drive_kind = IDE_HD;
    s->nb_sectors = nb_sectors;
    s->start_offset = start_offset;

    if (f_open) { s->fp = f; }

    if (cylinders && heads && sectors) {
        s->cylinders = cylinders;
        s->heads     = heads;
        s->sectors   = sectors;
    } else {
        uint32_t cyls = nb_sectors / (16 * 63);
        if (cyls > 16383) cyls = 16383;
        if (cyls < 2)     cyls = 2;
        s->cylinders = cyls;
        s->heads     = 16;
        s->sectors   = 63;
    }

    s->mult_sectors = MAX_MULT_SECTORS;
    s->feature = s->error = s->nsector = 0;
    s->sector = s->lcyl = s->hcyl = 0;
    s->select = 0xa0;
    s->status = READY_STAT | SEEK_STAT;
    s->xfer_done = ide_transfer_stop;
    return s;
}

static IDEState *ide_cddrive_init(IDEIFState *ide_if)
{
    IDEState *s = malloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));

    s->ide_if     = ide_if;
    s->drive_kind = IDE_CD;
    s->feature = s->error = s->nsector = 0;
    s->sector = s->lcyl = s->hcyl = 0;
    s->select = 0xa0;
    s->status = READY_STAT | SEEK_STAT;
    s->xfer_done = ide_transfer_stop;
    ide_set_signature(s);
    return s;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
IDEIFState *ide_allocate(int irq, void *pic, void (*set_irq)(void *pic, int irq, int level))
{
    IDEIFState *s = malloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->irq     = irq;
    s->pic     = pic;
    s->set_irq = set_irq;
    s->cur_drive = NULL;  /* set on first drive-select (ioport addr=6) */
    return s;
}

const uint8_t ide_magic[8] = { '1','D','E','D','1','5','C','0' };

/* Attach HDD: open file, detect geometry from optional header */
int ide_attach(IDEIFState *s, int drive, const char *filename)
{
    FIL f;
    if (f_open(&f, filename, FA_READ | FA_WRITE) != FR_OK) return -1;

    int start_offset = 0;
    int cylinders = 0, heads = 0, sectors = 0;
    uint8_t buf[8]; UINT br;
    f_read(&f, buf, 8, &br);
    if (br == 8 && memcmp(buf, ide_magic, 8) == 0) {
        start_offset = 1024;
        uint8_t chsbuf[14];
        f_lseek(&f, 512);
        f_read(&f, chsbuf, sizeof(chsbuf), &br);
        cylinders = chsbuf[2] | (chsbuf[3] << 8);
        heads     = chsbuf[6] | (chsbuf[7] << 8);
        sectors   = chsbuf[12]| (chsbuf[13]<< 8);
    }

    int64_t nb_sectors = (int64_t)(f_size(&f) - start_offset) / SECTOR_SIZE;
    s->drives[drive] = ide_hddrive_init(s, &f, 1, nb_sectors, start_offset,
                                         cylinders, heads, sectors);
    if (!s->drives[drive]) { f_close(&f); return -1; }
    return 0;
}

/* Attach HDD using already-open FIL (avoids double-open on FatFS).
 * Reads file size directly from FIL; geometry is pre-detected by disk layer. */
int ide_attach_ata(IDEIFState *s, int drive, FIL *f,
                   int cylinders, int heads, int sectors)
{
    FSIZE_t sz = f ? f_size(f) : 0;
    int64_t nb_sectors = (int64_t)sz / SECTOR_SIZE;

    s->drives[drive] = ide_hddrive_init(s, f, f != NULL, nb_sectors, 0,
                                         cylinders, heads, sectors);
    /* Master drive (0) becomes default cur_drive so status reads work
     * even before the first explicit drive-select from BIOS */
    if (drive == 0 && s->drives[0])
        s->cur_drive = s->drives[0];
    return s->drives[drive] ? 0 : -1;
}

/* Attach empty ATAPI CD-ROM device (no image loaded yet) */
int ide_attach_cd(IDEIFState *s, int drive)
{
    s->drives[drive] = ide_cddrive_init(s);
    if (drive == 0 && s->drives[0])
        s->cur_drive = s->drives[0];
    return s->drives[drive] ? 0 : -1;
}

int ide_has_drive(IDEIFState *s, int drive)
{
    return s && drive >= 0 && drive < 2 && s->drives[drive] != NULL;
}

/* Hot-swap CD image: pass open FIL* on insert, NULL to eject.
 * was_present: 1 if a disc was loaded before this call (for correct UA signalling
 * when eject+insert happen back-to-back without the guest seeing the empty tray). */
void ide_change_cd(IDEIFState *sif, int drive, FIL *f, int was_present)
{
    IDEState *s = sif->drives[drive];
    atapi_tlog("ide_change_cd: drive=%d s=%s f=%s was_present=%d\r\n",
               drive, s?"ok":"NULL", f?"ok":"NULL", was_present);
    if (!s || s->drive_kind != IDE_CD) return;

    s->fp = f;

    if (f) {
        atapi_tlog("  f_size=%lu nb_512=%ld cd_sec=%ld\r\n",
            (unsigned long)f_size(f),
            (long)(f_size(f) / 512),
            (long)(f_size(f) / 2048));
        if (was_present) {
            /* disc swap: signal UA so driver re-reads TOC */
            s->sense_key     = SENSE_UNIT_ATTENTION;
            s->asc           = ASC_MEDIUM_MAY_HAVE_CHANGED;
            s->cdrom_changed = 1;
            atapi_tlog("  swap -> UA\r\n");
        } else {
            /* boot/initial insert: drive was empty, no UA needed.
             * SeaBIOS does not issue TUR for ATAPI before loading DOS,
             * so skip UA to avoid confusing OAKCDROM into polling mode. */
            s->sense_key     = SENSE_NONE;
            s->asc           = 0;
            s->cdrom_changed = 0;
            atapi_tlog("  initial insert -> no UA\r\n");
        }
    } else {
        /* eject */
        s->sense_key     = SENSE_UNIT_ATTENTION;
        s->asc           = ASC_MEDIUM_MAY_HAVE_CHANGED;
        s->cdrom_changed = 1;
        atapi_tlog("  eject -> UA\r\n");
    }
    ide_set_irq(s);
}

PCIDevice *piix3_ide_init(PCIBus *pci_bus, int devfn)
{
    PCIDevice *d;
    d = pci_register_device(pci_bus, "PIIX3 IDE", devfn, 0x8086, 0x7010, 0x00, 0x0101);
    pci_device_set_config8(d, 0x09, 0x00);
    return d;
}

void ide_fill_cmos(IDEIFState *s, void *cmos,
                   uint8_t (*set)(void *cmos, int addr, uint8_t val))
{
    uint8_t d_0x12 = 0;
    if (s->drives[0]) {
        d_0x12 |= 0xf0;
        set(cmos, 0x19, 47);
        set(cmos, 0x1b, set(cmos, 0x21, s->drives[0]->cylinders));
        set(cmos, 0x1c, set(cmos, 0x22, s->drives[0]->cylinders >> 8));
        set(cmos, 0x1d, s->drives[0]->heads);
        set(cmos, 0x1e, 0xff);
        set(cmos, 0x1f, 0xff);
        set(cmos, 0x20, 0xc0 | ((s->drives[0]->heads > 8) << 3));
        set(cmos, 0x23, s->drives[0]->sectors);
    }
    if (s->drives[1]) {
        d_0x12 |= 0x0f;
        set(cmos, 0x1a, 47);
        set(cmos, 0x24, set(cmos, 0x2a, s->drives[1]->cylinders));
        set(cmos, 0x25, set(cmos, 0x2b, s->drives[1]->cylinders >> 8));
        set(cmos, 0x26, s->drives[1]->heads);
        set(cmos, 0x27, 0xff);
        set(cmos, 0x28, 0xff);
        set(cmos, 0x29, 0xc0 | ((s->drives[1]->heads > 8) << 3));
        set(cmos, 0x2c, s->drives[1]->sectors);
    }
    set(cmos, 0x12, d_0x12);
}
