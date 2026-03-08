/*
 * fdd.c  –  Intel 8272A / 82077AA Floppy Disk Controller emulation
 *
 * Implements real hardware-level FDC via ports 0x3F0–0x3F7, DMA channel 2,
 * IRQ 6.  Sector data is transferred through the i8257 DMA controller
 * (i8257_dma_read_memory / i8257_dma_write_memory), exactly as real
 * hardware does.  Floppy image I/O uses the existing FatFS-backed disk[]
 * layer exposed by disk.h.
 *
 * Design notes:
 *   • Full MSR-based handshake (RQM/DIO/CB) so poll-mode drivers work.
 *   • FIFO buffer (16 bytes) for command/result bytes.
 *   • Delayed IRQ: raised one fdc_tick() after command completion so the
 *     guest BIOS has time to set up its ISR before we fire.
 *   • DMA transfer handler registered on channel 2; DREQ/DACK cycle
 *     triggered by SEEK→transfer commands.
 *   • media-change (DIR bit 7) tracked per-drive.
 *   • Thread-safe-enough for the RP2350 single-core emulator loop.
 *
 * References:
 *   Intel 82077AA Floppy Disk Controller Data Sheet (1991)
 *   IBM PC/AT Technical Reference – INT 13h / FDC chapter
 *   RBIL (Ralf Brown's Interrupt List) – FDC port descriptions
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "fdd.h"
#include "disk.h"       /* disk[], chs2ofs helpers, FatFS FIL         */

/* ------------------------------------------------------------------ */
/*  Compile-time tunables                                               */
/* ------------------------------------------------------------------ */
#define FDC_DMA_CHAN      2       /* ISA 8-bit DMA channel              */
#define FDC_IRQ           6       /* IRQ line                           */
#define FDC_MAX_DRIVES    2       /* A: (0) and B: (1) only             */
#define FIFO_DEPTH        16      /* bytes in command / result FIFO     */
#define FDC_SECTOR_SIZE   512     /* always 512 for FDD images          */
#define FDC_IRQ_DELAY_MS  1       /* ticks before IRQ fires             */

/* ------------------------------------------------------------------ */
/*  MSR bits (0x3F4, read)                                              */
/* ------------------------------------------------------------------ */
#define MSR_ACTA    0x01   /* drive A seeking                          */
#define MSR_ACTB    0x02   /* drive B seeking                          */
#define MSR_CB      0x10   /* FDC busy – command in progress           */
#define MSR_NDMA    0x20   /* non-DMA mode (we always use DMA)         */
#define MSR_DIO     0x40   /* 1 = FDC→CPU (result phase)               */
#define MSR_RQM     0x80   /* ready for master (CPU may access DATA)   */

/* ------------------------------------------------------------------ */
/*  DOR bits (0x3F2, read/write)                                        */
/* ------------------------------------------------------------------ */
#define DOR_DSEL    0x03   /* drive select mask                        */
#define DOR_NRESET  0x04   /* 0 = FDC in reset                         */
#define DOR_DMAEN   0x08   /* enable DMA + IRQ                         */
#define DOR_MOTA    0x10   /* motor A                                  */
#define DOR_MOTB    0x20   /* motor B                                  */

/* ------------------------------------------------------------------ */
/*  ST0 bits                                                            */
/* ------------------------------------------------------------------ */
#define ST0_DS      0x03   /* drive select                             */
#define ST0_HD      0x04   /* head number                              */
#define ST0_NR      0x08   /* not-ready                                */
#define ST0_EC      0x10   /* equipment check (recalibrate fail)       */
#define ST0_SE      0x20   /* seek end                                 */
#define ST0_IC_MASK 0xC0
#define ST0_IC_NORM 0x00   /* normal termination                       */
#define ST0_IC_ABNORM 0x40 /* abnormal termination                     */
#define ST0_IC_INVAL 0x80  /* invalid command                          */
#define ST0_IC_RDYCHG 0xC0 /* ready changed                           */

/* ------------------------------------------------------------------ */
/*  ST1 bits                                                            */
/* ------------------------------------------------------------------ */
#define ST1_MA      0x01   /* missing address mark                     */
#define ST1_NW      0x02   /* not-writeable (WP)                       */
#define ST1_ND      0x04   /* no data                                  */
#define ST1_OR      0x10   /* overrun                                  */
#define ST1_DE      0x20   /* data error (CRC)                         */
#define ST1_EN      0x80   /* end of cylinder                          */

/* ------------------------------------------------------------------ */
/*  ST2 bits                                                            */
/* ------------------------------------------------------------------ */
#define ST2_MD      0x01   /* missing data address mark                */
#define ST2_BC      0x02   /* bad cylinder                             */
#define ST2_SN      0x04   /* scan not satisfied                       */
#define ST2_SH      0x08   /* scan equal hit                           */
#define ST2_WC      0x10   /* wrong cylinder                           */
#define ST2_DD      0x20   /* data error in data field                 */
#define ST2_CM      0x40   /* control mark (deleted data)              */

/* ------------------------------------------------------------------ */
/*  ST3 bits                                                            */
/* ------------------------------------------------------------------ */
#define ST3_DS      0x03   /* drive select                             */
#define ST3_HD      0x04   /* head                                     */
#define ST3_TS      0x08   /* two-sided                                */
#define ST3_T0      0x10   /* track 0                                  */
#define ST3_RY      0x20   /* ready                                    */
#define ST3_WP      0x40   /* write protected                          */
#define ST3_FT      0x80   /* fault                                    */

/* ------------------------------------------------------------------ */
/*  FDC command phase states                                            */
/* ------------------------------------------------------------------ */
typedef enum {
    PHASE_CMD    = 0,   /* accepting command + parameter bytes         */
    PHASE_EXEC   = 1,   /* executing (DMA in progress)                 */
    PHASE_RESULT = 2,   /* result bytes ready for CPU to read          */
} FDCPhase;

/* ------------------------------------------------------------------ */
/*  Per-drive state                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  track;          /* current head position (cylinder)       */
    uint8_t  dir;            /* direction of last seek (+1 / -1)       */
    uint8_t  media_changed;  /* DIR bit: set on insert/eject           */
    uint8_t  seeking;        /* seek in progress flag                  */
} FDCDrive;

/* ------------------------------------------------------------------ */
/*  Main FDC state                                                      */
/* ------------------------------------------------------------------ */
struct FDCState {
    /* Hardware connections */
    PicState2  *pic;
    I8257State *dma;

    /* Registers */
    uint8_t  dor;            /* Digital Output Register                */
    uint8_t  tdr;            /* Tape Drive Register (mostly ignored)   */
    uint8_t  dsr;            /* Data-rate Select Register              */
    uint8_t  ccr;            /* Configuration Control Register         */

    /* FIFO / command handling */
    FDCPhase phase;
    uint8_t  fifo[FIFO_DEPTH];
    int      fifo_pos;       /* next byte to write / read              */
    int      fifo_len;       /* valid bytes in fifo[]                  */

    /* Command decode */
    uint8_t  cmd;            /* current command byte (low 5 bits used) */
    int      cmd_len;        /* total parameter bytes expected          */
    int      cmd_received;   /* parameter bytes received so far        */

    /* Result */
    int      result_len;     /* bytes in result phase                  */

    /* Per-drive state */
    FDCDrive drive[FDC_MAX_DRIVES];

    /* Interrupt bookkeeping */
    int      irq_pending;    /* IRQ scheduled but not yet raised        */
    int      irq_delay;      /* countdown ticks                         */
    uint8_t  pending_st0;    /* ST0 saved at seek/cmd end               */
    uint8_t  pending_pcn;    /* PCN saved at seek end                   */
    int      int_result_valid; /* sense-interrupt has data waiting      */

    /* DMA transfer state */
    int      dma_active;     /* DMA transfer in progress                */
    int      dma_drivenum;   /* disk[] index (0 or 1)                   */
    uint8_t  dma_write;      /* 1 = write to disk, 0 = read from disk   */
    uint32_t dma_file_off;   /* current byte offset in image file       */
    int      dma_sectors_todo; /* sectors remaining in multi-sector cmd */
    /* Current CHS tracking during multi-sector transfer */
    uint8_t  cur_cyl;
    uint8_t  cur_head;
    uint8_t  cur_sector;
    uint8_t  eot;            /* end-of-track sector from command        */

    /* CONFIGURE command state */
    uint8_t  config;         /* CONFIGURE byte 2                        */
    uint8_t  pretrk;         /* CONFIGURE byte 3                        */
    uint8_t  locked;         /* LOCK state                              */

    /* Sector buffer for DMA use */
    uint8_t  sector_buf[FDC_SECTOR_SIZE];
    int      sector_buf_pos; /* position within sector_buf             */
    int      sector_buf_len; /* valid bytes (always 512 when loaded)    */
};

/* ------------------------------------------------------------------ */
/*  Forward declarations                                                */
/* ------------------------------------------------------------------ */
static void fdc_raise_irq(FDCState *s);
static void fdc_lower_irq(FDCState *s);
static void fdc_set_result(FDCState *s, const uint8_t *buf, int len);
static void fdc_abort_command(FDCState *s);
static void fdc_execute_command(FDCState *s);
static int  fdc_dma_handler(void *opaque, int nchan, int dma_pos, int dma_len);

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline int fdc_selected_drive(FDCState *s)
{
    return s->dor & DOR_DSEL;   /* 0 or 1                               */
}

/* Map logical drive (0/1) to disk[] array index (always same here) */
static inline int fdc_drivenum(int drive)
{
    return drive & 1;
}

static inline int fdc_drive_ready(int drivenum)
{
    return disk_is_inserted((uint8_t)drivenum);
}

/* Compute byte offset in image file from CHS */
static uint32_t fdc_chs_to_offset(int drivenum, int cyl, int head, int sect)
{
    /* geometry is stored in disk[] – use public accessors */
    uint16_t heads = disk_get_heads((uint8_t)drivenum);
    uint16_t sects = disk_get_sects((uint8_t)drivenum);
    if (heads == 0 || sects == 0) return (uint32_t)-1;
    return (uint32_t)(((uint32_t)cyl * heads + (uint32_t)head) * sects
                      + ((uint32_t)sect - 1)) * FDC_SECTOR_SIZE;
}

/* Validate CHS against current geometry */
static int fdc_chs_valid(int drivenum, int cyl, int head, int sect)
{
    uint16_t cyls  = disk_get_cyls ((uint8_t)drivenum);
    uint16_t heads = disk_get_heads((uint8_t)drivenum);
    uint16_t sects = disk_get_sects((uint8_t)drivenum);
    return (sect >= 1 && sect <= sects &&
            (uint16_t)cyl  < cyls &&
            (uint16_t)head < heads);
}

/* ------------------------------------------------------------------ */
/*  IRQ                                                                 */
/* ------------------------------------------------------------------ */
static void fdc_raise_irq(FDCState *s)
{
    if (s->dor & DOR_DMAEN)
        i8259_set_irq(s->pic, FDC_IRQ, 1);
}

static void fdc_lower_irq(FDCState *s)
{
    i8259_set_irq(s->pic, FDC_IRQ, 0);
}

/* Schedule a deferred IRQ (fired by fdc_tick) */
static void fdc_schedule_irq(FDCState *s, uint8_t st0, uint8_t pcn)
{
    s->pending_st0       = st0;
    s->pending_pcn       = pcn;
    s->int_result_valid  = 1;
    s->irq_pending       = 1;
    s->irq_delay         = FDC_IRQ_DELAY_MS;
}

/* ------------------------------------------------------------------ */
/*  Result phase helpers                                                */
/* ------------------------------------------------------------------ */
static void fdc_set_result(FDCState *s, const uint8_t *buf, int len)
{
    if (len > FIFO_DEPTH) len = FIFO_DEPTH;
    memcpy(s->fifo, buf, len);
    s->fifo_pos   = 0;
    s->fifo_len   = len;
    s->result_len = len;
    s->phase      = PHASE_RESULT;
    /* MSR: RQM=1, DIO=1, CB=1 */
}

/* Build standard 7-byte READ/WRITE result */
static void fdc_finish_rw(FDCState *s, uint8_t st0, uint8_t st1, uint8_t st2,
                           uint8_t cyl, uint8_t head, uint8_t sect, uint8_t n)
{
    uint8_t res[7] = { st0, st1, st2, cyl, head, sect, n };
    fdc_set_result(s, res, 7);
    fdc_schedule_irq(s, st0, cyl);
}

/* ------------------------------------------------------------------ */
/*  Invalid command result                                              */
/* ------------------------------------------------------------------ */
static void fdc_invalid_command(FDCState *s)
{
    uint8_t res[1] = { ST0_IC_INVAL };
    fdc_set_result(s, res, 1);
    /* No IRQ for invalid command */
}

/* ------------------------------------------------------------------ */
/*  Reset handling                                                      */
/* ------------------------------------------------------------------ */
static void fdc_reset(FDCState *s, int software_reset)
{
    s->phase        = PHASE_CMD;
    s->fifo_pos     = 0;
    s->fifo_len     = 0;
    s->cmd          = 0;
    s->cmd_len      = 0;
    s->cmd_received = 0;
    s->result_len   = 0;
    s->irq_pending  = 0;
    s->int_result_valid = 0;

    /* Abort any in-progress DMA transfer cleanly */
    if (s->dma_active) {
        s->dma_active = 0;
        i8257_dma_release_DREQ((IsaDma *)s->dma, FDC_DMA_CHAN);
    }

    for (int i = 0; i < FDC_MAX_DRIVES; i++) {
        s->drive[i].seeking = 0;
        s->drive[i].track   = 0;
    }

    /* After a software reset (DOR bit2: 0->1), the FDC signals completion
     * via IRQ 6 so the BIOS knows it can issue SENSE INTERRUPT STATUS.
     * On power-on we do NOT fire IRQ: the BIOS has not set up its ISR yet,
     * and the PIC may not be initialised. */
    if (software_reset && (s->dor & DOR_DMAEN)) {
        s->int_result_valid = 1;
        s->pending_st0      = ST0_IC_RDYCHG;
        s->pending_pcn      = 0;
        fdc_schedule_irq(s, ST0_IC_RDYCHG, 0);
    }
}

/* ------------------------------------------------------------------ */
/*  DMA transfer handler                                                */
/*  Called by i8257 DMA engine.  dma_pos = bytes transferred so far.   */
/*  dma_len = total bytes for this DMA request.                         */
/*  Returns number of bytes consumed (== dma_len on success).           */
/* ------------------------------------------------------------------ */
static int fdc_dma_handler(void *opaque, int nchan, int dma_pos, int dma_len)
{
    FDCState *s = opaque;
    (void)nchan;

    if (!s->dma_active)
        return dma_len;   /* tell DMA engine: done, nothing to transfer */

    int drivenum = s->dma_drivenum;

    /* We transfer one sector at a time.  dma_pos tracks bytes done. */
    int transferred = 0;

    while (s->dma_sectors_todo > 0 && transferred < dma_len) {
        /* Load sector from image if buffer empty */
        if (s->sector_buf_pos >= s->sector_buf_len) {
            /* Read/write sector via FatFS */
            uint32_t off = fdc_chs_to_offset(drivenum,
                                              s->cur_cyl,
                                              s->cur_head,
                                              s->cur_sector);
            if (off == (uint32_t)-1) goto dma_error;

            if (!s->dma_write) {
                /* READ: load sector from image into buffer */
                FIL *fil = disk_get_fil((uint8_t)drivenum);
                if (!fil) goto dma_error;
                UINT br;
                FRESULT fr = f_lseek(fil, off);
                if (fr != FR_OK) goto dma_error;
                fr = f_read(fil, s->sector_buf, FDC_SECTOR_SIZE, &br);
                if (fr != FR_OK || br != FDC_SECTOR_SIZE) goto dma_error;
            } else {
                /* WRITE: buffer will be filled by DMA write, flush after */
                memset(s->sector_buf, 0, FDC_SECTOR_SIZE);
            }
            s->sector_buf_pos = 0;
            s->sector_buf_len = FDC_SECTOR_SIZE;
            s->dma_file_off   = off;
        }

        int chunk = s->sector_buf_len - s->sector_buf_pos;
        int remaining_dma = dma_len - transferred;
        if (chunk > remaining_dma) chunk = remaining_dma;

        if (!s->dma_write) {
            /* READ from disk → write to guest memory via DMA */
            i8257_dma_write_memory((IsaDma *)s->dma, FDC_DMA_CHAN,
                                   s->sector_buf + s->sector_buf_pos,
                                   dma_pos + transferred, chunk);
        } else {
            /* WRITE: read from guest memory via DMA → into buffer */
            i8257_dma_read_memory((IsaDma *)s->dma, FDC_DMA_CHAN,
                                  s->sector_buf + s->sector_buf_pos,
                                  dma_pos + transferred, chunk);
        }

        s->sector_buf_pos += chunk;
        transferred       += chunk;

        /* Sector complete? */
        if (s->sector_buf_pos >= FDC_SECTOR_SIZE) {
            if (s->dma_write) {
                /* Flush buffer to image */
                FIL *fil = disk_get_fil((uint8_t)drivenum);
                if (!fil) goto dma_error;
                UINT bw;
                FRESULT fr = f_lseek(fil, s->dma_file_off);
                if (fr != FR_OK) goto dma_error;
                fr = f_write(fil, s->sector_buf, FDC_SECTOR_SIZE, &bw);
                if (fr != FR_OK || bw != FDC_SECTOR_SIZE) goto dma_error;
            }

            s->dma_sectors_todo--;
            s->sector_buf_pos = s->sector_buf_len; /* mark empty */

            /* Advance CHS */
            s->cur_sector++;
            uint16_t spt = disk_get_sects((uint8_t)drivenum);
            if (s->cur_sector > spt || s->cur_sector > s->eot) {
                /* End of track or EOT reached */
                s->cur_sector = 1;
                s->cur_head ^= 1;
                if (s->cur_head == 0)
                    s->cur_cyl++;
            }
        }
    }

    if (s->dma_sectors_todo == 0) {
        /* Transfer complete */
        s->dma_active = 0;
        i8257_dma_release_DREQ((IsaDma *)s->dma, FDC_DMA_CHAN);

        uint8_t st0 = (s->cur_head ? ST0_HD : 0) | (uint8_t)(drivenum & ST0_DS);
        fdc_finish_rw(s, st0, 0, 0,
                      s->cur_cyl, s->cur_head, s->cur_sector, 2 /*N=512*/);
    }

    return dma_pos + transferred;

dma_error:
    s->dma_active = 0;
    i8257_dma_release_DREQ((IsaDma *)s->dma, FDC_DMA_CHAN);
    {
        uint8_t st0 = ST0_IC_ABNORM | (uint8_t)(drivenum & ST0_DS);
        uint8_t st1 = ST1_ND;
        fdc_finish_rw(s, st0, st1, 0,
                      s->cur_cyl, s->cur_head, s->cur_sector, 2);
    }
    return dma_pos + transferred;
}

/* ------------------------------------------------------------------ */
/*  Start a DMA-backed read/write transfer                              */
/* ------------------------------------------------------------------ */
static void fdc_start_dma(FDCState *s, int drivenum, int write_to_disk,
                           uint8_t cyl, uint8_t head, uint8_t sect,
                           uint8_t eot, int sector_count)
{
    s->dma_drivenum     = drivenum;
    s->dma_write        = write_to_disk;
    s->cur_cyl          = cyl;
    s->cur_head         = head;
    s->cur_sector       = sect;
    s->eot              = eot;
    s->dma_sectors_todo = sector_count;
    s->sector_buf_pos   = FDC_SECTOR_SIZE; /* empty */
    s->sector_buf_len   = FDC_SECTOR_SIZE;
    s->dma_active       = 1;
    s->phase            = PHASE_EXEC;

    /* Assert DREQ on DMA channel 2 – DMA engine will call fdc_dma_handler */
    i8257_dma_hold_DREQ((IsaDma *)s->dma, FDC_DMA_CHAN);
}

/* ------------------------------------------------------------------ */
/*  Decode command length table                                         */
/* ------------------------------------------------------------------ */
/*
 * Returns the total number of bytes for a command (command byte + params).
 * Command byte is fifo[0]; low 5 bits are the base command.
 */
static int fdc_cmd_param_count(uint8_t cmd_byte)
{
    /* Low 5 bits = opcode; upper bits = option flags (MT, MFM, SK) */
    switch (cmd_byte & 0x1F) {
    case 0x02: return 9;   /* READ TRACK             */
    case 0x03: return 3;   /* SPECIFY                */
    case 0x04: return 2;   /* SENSE DRIVE STATUS     */
    case 0x05: return 9;   /* WRITE DATA             */
    case 0x06: return 9;   /* READ DATA              */
    case 0x07: return 2;   /* RECALIBRATE            */
    case 0x08: return 1;   /* SENSE INTERRUPT STATUS */
    case 0x09: return 9;   /* WRITE DELETED DATA     */
    case 0x0A: return 2;   /* READ ID                */
    case 0x0C: return 9;   /* READ DELETED DATA      */
    case 0x0D: return 6;   /* FORMAT TRACK           */
    case 0x0E: return 1;   /* DUMPREG                */
    case 0x0F: return 3;   /* SEEK                   */
    case 0x10: return 1;   /* VERSION                */
    case 0x11: return 9;   /* SCAN EQUAL             */
    case 0x12: return 2;   /* PERPENDICULAR MODE     */
    case 0x13: return 4;   /* CONFIGURE              */
    case 0x14: return 1;   /* LOCK / UNLOCK          */
    case 0x18: return 1;   /* PART ID                */
    case 0x19: return 9;   /* SCAN LOW OR EQUAL      */
    case 0x1D: return 9;   /* SCAN HIGH OR EQUAL     */
    default:   return 1;   /* Unknown – 1 byte       */
    }
}

/* ------------------------------------------------------------------ */
/*  Execute fully received command                                       */
/* ------------------------------------------------------------------ */
static void fdc_execute_command(FDCState *s)
{
    uint8_t *f    = s->fifo;          /* shorthand                      */
    uint8_t  cmd  = f[0] & 0x1F;     /* opcode without option bits      */
    int      drive = f[1] & 0x03;    /* drive select in most commands   */
    int      head  = (f[1] >> 2) & 1;
    int      dn    = fdc_drivenum(drive);

    switch (cmd) {

    /* ---- SPECIFY (03h) ------------------------------------------ */
    case 0x03:
        /* SRT/HUT in f[1], HLT/ND in f[2] – we accept and ignore     */
        s->phase = PHASE_CMD;
        break;

    /* ---- SENSE DRIVE STATUS (04h) -------------------------------- */
    case 0x04: {
        uint8_t st3 = (uint8_t)(dn & ST3_DS)
                    | (head ? ST3_HD : 0)
                    | ST3_TS            /* always two-sided              */
                    | (fdc_drive_ready(dn) ? ST3_RY : 0)
                    | (s->drive[dn].track == 0 ? ST3_T0 : 0);
        uint8_t res[1] = { st3 };
        fdc_set_result(s, res, 1);
        break;
    }

    /* ---- RECALIBRATE (07h) --------------------------------------- */
    case 0x07:
        s->drive[dn].track   = 0;
        s->drive[dn].seeking = 0;
        {
            uint8_t st0 = ST0_SE | (uint8_t)(dn & ST0_DS);
            fdc_schedule_irq(s, st0, 0);
        }
        s->phase = PHASE_CMD;
        break;

    /* ---- SENSE INTERRUPT STATUS (08h) ---------------------------- */
    case 0x08:
        if (s->int_result_valid) {
            uint8_t res[2] = { s->pending_st0, s->pending_pcn };
            s->int_result_valid = 0;
            fdc_set_result(s, res, 2);
            fdc_lower_irq(s);
        } else {
            /* No interrupt pending */
            uint8_t res[2] = { ST0_IC_INVAL, 0 };
            fdc_set_result(s, res, 2);
        }
        break;

    /* ---- SEEK (0Fh) ---------------------------------------------- */
    case 0x0F: {
        uint8_t ncyl = f[2];
        s->drive[dn].track   = ncyl;
        s->drive[dn].seeking = 0;
        uint8_t st0 = ST0_SE | (head ? ST0_HD : 0) | (uint8_t)(dn & ST0_DS);
        fdc_schedule_irq(s, st0, ncyl);
        s->phase = PHASE_CMD;
        break;
    }

    /* ---- READ DATA (06h) / READ TRACK (02h) ---------------------- */
    case 0x06:
    case 0x02: {
        /* f[1]=HD/DS, f[2]=C, f[3]=H, f[4]=R, f[5]=N, f[6]=EOT,
           f[7]=GPL, f[8]=DTL                                           */
        uint8_t cyl  = f[2];
        uint8_t hd   = f[3];
        uint8_t sect = f[4];
        /* uint8_t n = f[5]; – sector size code, we always use 512     */
        uint8_t eot  = f[6];

        if (!fdc_drive_ready(dn)) {
            uint8_t st0 = ST0_IC_ABNORM | ST0_NR | (uint8_t)(dn);
            fdc_finish_rw(s, st0, ST1_MA, 0, cyl, hd, sect, 2);
            break;
        }
        if (!fdc_chs_valid(dn, cyl, hd, sect)) {
            uint8_t st0 = ST0_IC_ABNORM | (uint8_t)(dn);
            fdc_finish_rw(s, st0, ST1_ND, 0, cyl, hd, sect, 2);
            break;
        }
        /* Compute sector count from EOT */
        int nsects = (int)(eot - sect + 1);
        if (nsects <= 0) nsects = 1;

        /* Update drive head position */
        s->drive[dn].track = cyl;
        fdc_start_dma(s, dn, 0 /*read*/, cyl, hd, sect, eot, nsects);
        break;
    }

    /* ---- WRITE DATA (05h) ---------------------------------------- */
    case 0x05:
    case 0x09: /* WRITE DELETED DATA – treat same as WRITE DATA       */ {
        uint8_t cyl  = f[2];
        uint8_t hd   = f[3];
        uint8_t sect = f[4];
        uint8_t eot  = f[6];

        if (!fdc_drive_ready(dn)) {
            uint8_t st0 = ST0_IC_ABNORM | ST0_NR | (uint8_t)(dn);
            fdc_finish_rw(s, st0, ST1_MA, 0, cyl, hd, sect, 2);
            break;
        }
        if (!fdc_chs_valid(dn, cyl, hd, sect)) {
            uint8_t st0 = ST0_IC_ABNORM | (uint8_t)(dn);
            fdc_finish_rw(s, st0, ST1_ND, 0, cyl, hd, sect, 2);
            break;
        }
        int nsects = (int)(eot - sect + 1);
        if (nsects <= 0) nsects = 1;

        s->drive[dn].track = cyl;
        fdc_start_dma(s, dn, 1 /*write*/, cyl, hd, sect, eot, nsects);
        break;
    }

    /* ---- READ DELETED DATA (0Ch) --------------------------------- */
    case 0x0C: {
        /* Treat as READ DATA – deleted mark not tracked in flat images */
        uint8_t cyl  = f[2];
        uint8_t hd   = f[3];
        uint8_t sect = f[4];
        uint8_t eot  = f[6];
        if (!fdc_drive_ready(dn)) {
            uint8_t st0 = ST0_IC_ABNORM | ST0_NR | (uint8_t)(dn);
            fdc_finish_rw(s, st0, ST1_MA, 0, cyl, hd, sect, 2);
            break;
        }
        int nsects = (int)(eot - sect + 1);
        if (nsects <= 0) nsects = 1;
        s->drive[dn].track = cyl;
        fdc_start_dma(s, dn, 0 /*read*/, cyl, hd, sect, eot, nsects);
        break;
    }

    /* ---- FORMAT TRACK (0Dh) -------------------------------------- */
    case 0x0D: {
        /* f[1]=HD/DS, f[2]=N, f[3]=SC (sectors/track), f[4]=GPL, f[5]=D */
        uint8_t sc   = f[3];   /* sectors per track to format           */
        uint8_t fill = f[5];   /* fill byte                             */

        if (!fdc_drive_ready(dn)) {
            uint8_t st0 = ST0_IC_ABNORM | ST0_NR | (uint8_t)(dn);
            uint8_t res[7] = { st0, ST1_MA, 0, 0, 0, 0, 2 };
            fdc_set_result(s, res, 7);
            fdc_schedule_irq(s, st0, s->drive[dn].track);
            break;
        }

        /* Write fill bytes to all sectors on the current track.
           We format the track that was last seeked to.                */
        uint8_t cyl = s->drive[dn].track;
        FIL *fil = disk_get_fil((uint8_t)dn);
        if (fil) {
            memset(s->sector_buf, fill, FDC_SECTOR_SIZE);
            for (int sec = 1; sec <= (int)sc; sec++) {
                uint32_t off = fdc_chs_to_offset(dn, cyl, head, sec);
                if (off == (uint32_t)-1) continue;
                UINT bw;
                if (f_lseek(fil, off) == FR_OK)
                    f_write(fil, s->sector_buf, FDC_SECTOR_SIZE, &bw);
            }
        }
        {
            uint8_t st0 = (head ? ST0_HD : 0) | (uint8_t)(dn & ST0_DS);
            fdc_finish_rw(s, st0, 0, 0, cyl, (uint8_t)head, sc, 2);
        }
        break;
    }

    /* ---- READ ID (0Ah) ------------------------------------------- */
    case 0x0A: {
        if (!fdc_drive_ready(dn)) {
            uint8_t st0 = ST0_IC_ABNORM | ST0_NR | (uint8_t)(dn);
            fdc_finish_rw(s, st0, ST1_MA, 0,
                          s->drive[dn].track, (uint8_t)head, 1, 2);
            break;
        }
        uint8_t st0 = (head ? ST0_HD : 0) | (uint8_t)(dn & ST0_DS);
        fdc_finish_rw(s, st0, 0, 0,
                      s->drive[dn].track, (uint8_t)head, 1, 2);
        break;
    }

    /* ---- VERSION (10h) ------------------------------------------- */
    case 0x10: {
        uint8_t res[1] = { 0x90 };   /* 82077AA */
        fdc_set_result(s, res, 1);
        break;
    }

    /* ---- PERPENDICULAR MODE (12h) -------------------------------- */
    case 0x12:
        /* Accept and ignore – not relevant for flat image emulation   */
        s->phase = PHASE_CMD;
        break;

    /* ---- CONFIGURE (13h) ----------------------------------------- */
    case 0x13:
        /* f[1]=0, f[2]=config byte, f[3]=pretrk                      */
        s->config  = f[2];
        s->pretrk  = f[3];
        s->phase   = PHASE_CMD;
        break;

    /* ---- LOCK / UNLOCK (14h / 94h) ------------------------------- */
    case 0x14:
        s->locked = (f[0] >> 7) & 1;
        {
            uint8_t res[1] = { (uint8_t)(s->locked << 4) };
            fdc_set_result(s, res, 1);
        }
        break;

    /* ---- DUMPREG (0Eh) ------------------------------------------- */
    case 0x0E: {
        uint8_t res[10];
        res[0] = s->drive[0].track;
        res[1] = s->drive[1].track;
        res[2] = 0;   /* drive 2 (N/A) */
        res[3] = 0;   /* drive 3 (N/A) */
        res[4] = 0;   /* SRT/HUT (not tracked) */
        res[5] = 0;   /* HLT/ND  (not tracked) */
        res[6] = 0;   /* SC/EFIFO */
        res[7] = s->pretrk;
        res[8] = s->config;
        res[9] = (uint8_t)(s->locked << 7);
        fdc_set_result(s, res, 10);
        break;
    }

    /* ---- PART ID (18h) ------------------------------------------- */
    case 0x18: {
        uint8_t res[1] = { 0x41 };
        fdc_set_result(s, res, 1);
        break;
    }

    /* ---- Unknown / unsupported ----------------------------------- */
    default:
        fdc_invalid_command(s);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Abort – reset to command phase                                      */
/* ------------------------------------------------------------------ */
static void fdc_abort_command(FDCState *s)
{
    s->phase        = PHASE_CMD;
    s->fifo_pos     = 0;
    s->fifo_len     = 0;
    s->cmd_received = 0;
    s->dma_active   = 0;
}

/* ------------------------------------------------------------------ */
/*  Port I/O – READ                                                     */
/* ------------------------------------------------------------------ */
uint8_t fdc_ioport_read(FDCState *s, uint32_t addr)
{
    switch (addr) {

    case 0x3F0: /* SRA – Status Register A (PS/2) */
        /* Bit 7 = INT (interrupt pending), bit 6 = DMA, rest = drive status */
        return (uint8_t)((s->irq_pending ? 0x80 : 0x00) |
                         (s->dor & DOR_DMAEN ? 0x20 : 0x00) |
                         (fdc_drive_ready(0) ? 0x04 : 0x00));

    case 0x3F1: /* SRB – Status Register B (PS/2) */
        return 0x00;   /* all motor off, no activity */

    case 0x3F2: /* DOR – Digital Output Register */
        return s->dor;

    case 0x3F3: /* TDR – Tape Drive Register */
        return s->tdr;

    case 0x3F4: /* MSR – Main Status Register */
    {
        uint8_t msr = MSR_RQM;   /* default: ready for CPU              */

        if (s->phase == PHASE_EXEC) {
            /* DMA transfer in progress – not ready for CMD/RESULT     */
            msr = 0;             /* RQM=0, CB=1 implied                 */
            msr |= MSR_CB;
        } else if (s->phase == PHASE_RESULT) {
            /* Result bytes ready: RQM=1, DIO=1, CB=1                  */
            msr = MSR_RQM | MSR_DIO | MSR_CB;
        } else {
            /* PHASE_CMD: ready to accept command bytes                 */
            msr = MSR_RQM;
        }

        /* Drive busy flags */
        for (int i = 0; i < FDC_MAX_DRIVES; i++)
            if (s->drive[i].seeking)
                msr |= (MSR_ACTA << i);

        return msr;
    }

    case 0x3F5: /* DATA / FIFO */
        if (s->phase == PHASE_RESULT && s->fifo_pos < s->fifo_len) {
            uint8_t val = s->fifo[s->fifo_pos++];
            if (s->fifo_pos >= s->fifo_len) {
                /* All result bytes consumed – back to command phase   */
                s->phase    = PHASE_CMD;
                s->fifo_pos = 0;
                s->fifo_len = 0;
            }
            return val;
        }
        return 0xFF;

    case 0x3F7: /* DIR – Digital Input Register */
    {
        int    d   = fdc_selected_drive(s);
        uint8_t dir = 0x00;
        if (d < FDC_MAX_DRIVES && s->drive[d].media_changed)
            dir |= 0x80;
        /* Bit 0: high-density selected (1 = HD) */
        return dir | 0x7F;
    }

    default:
        return 0xFF;
    }
}

/* ------------------------------------------------------------------ */
/*  Port I/O – WRITE                                                    */
/* ------------------------------------------------------------------ */
void fdc_ioport_write(FDCState *s, uint32_t addr, uint8_t val)
{
    switch (addr) {

    case 0x3F2: /* DOR – Digital Output Register */
    {
        uint8_t prev = s->dor;
        s->dor = val;

        /* Software reset: DOR bit 2 transitions 0→1 */
        if (!(prev & DOR_NRESET) && (val & DOR_NRESET)) {
            /* NRESET: 0->1 = software reset complete, signal via IRQ */
            fdc_reset(s, 1);
        } else if (!(val & DOR_NRESET)) {
            /* NRESET asserted (0) = hold in reset, no IRQ */
            fdc_reset(s, 0);
            fdc_lower_irq(s);
        }
        break;
    }

    case 0x3F3: /* TDR – Tape Drive Register */
        s->tdr = val;
        break;

    case 0x3F4: /* DSR – Data-rate Select Register (write-only) */
        s->dsr = val;
        if (val & 0x80) {
            /* Software reset via DSR bit 7 */
            fdc_reset(s, 1);
        }
        break;

    case 0x3F5: /* DATA / FIFO */
        if (s->phase == PHASE_EXEC)
            break;   /* ignore writes during DMA execution             */

        if (s->phase == PHASE_RESULT) {
            /* Should not write during result phase; reset gracefully  */
            fdc_abort_command(s);
        }

        /* Accumulate command bytes in FIFO */
        if (s->phase == PHASE_CMD) {
            if (s->cmd_received == 0) {
                /* First byte = command */
                s->fifo[0]    = val;
                s->cmd        = val & 0x1F;
                s->cmd_len    = fdc_cmd_param_count(val);
                s->cmd_received = 1;
            } else if (s->cmd_received < FIFO_DEPTH) {
                s->fifo[s->cmd_received++] = val;
            }

            /* Execute when all bytes received */
            if (s->cmd_received >= s->cmd_len) {
                s->cmd_received = 0;
                fdc_execute_command(s);
            }
        }
        break;

    case 0x3F7: /* CCR – Configuration Control Register (write-only) */
        s->ccr = val & 0x03;   /* data-rate bits */
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Media change notification                                           */
/* ------------------------------------------------------------------ */
void fdc_media_changed(FDCState *s, int drive)
{
    if (drive >= 0 && drive < FDC_MAX_DRIVES) {
        s->drive[drive].media_changed = 1;
        /* NOTE: do NOT reset track here. The physical head position is
         * unchanged by a media swap; the next RECALIBRATE/SEEK will
         * reposition it. Resetting track mid-operation corrupts CHS math. */
    }
}

/* ------------------------------------------------------------------ */
/*  Periodic tick (call ~1 ms from pc_step)                             */
/* ------------------------------------------------------------------ */
void fdc_tick(FDCState *s)
{
    if (s->irq_pending) {
        if (s->irq_delay > 0) {
            s->irq_delay--;
        } else {
            s->irq_pending = 0;
            fdc_raise_irq(s);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Constructor / destructor                                            */
/* ------------------------------------------------------------------ */
FDCState *fdc_new(PicState2 *pic, I8257State *dma)
{
    FDCState *s = calloc(1, sizeof(FDCState));
    if (!s) return NULL;

    s->pic = pic;
    s->dma = dma;

    /* Initial DOR: reset asserted (bit 2 = 0), DMA disabled */
    s->dor = 0x00;

    /* Register DMA channel 2 handler */
    i8257_dma_register_channel((IsaDma *)dma, FDC_DMA_CHAN,
                                fdc_dma_handler, s);

    /* Perform initial reset */
    /* Power-on state: reset asserted, DMAEN on.
     * Do NOT fire IRQ here — BIOS/PIC not yet initialised. */
    s->dor = DOR_NRESET | DOR_DMAEN;
    fdc_reset(s, 0);  /* 0 = power-on, no IRQ */

    return s;
}

void fdc_free(FDCState *s)
{
    free(s);
}
