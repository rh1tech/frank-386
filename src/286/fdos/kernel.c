#include <pico.h>
#include <pico/time.h>
#include <hardware/pio.h>
#include "../cpu.h"
#include "../bios.h"
#include "../fdos.h"
#include "i8254.h"

#define printf(...) bios_printf(cpu, __VA_ARGS__)

static CPU* cpu;

#define MAX_HARD_DRIVE  4
#define NDEV            26      /* up to Z:                     */

#include "hdr/kconfig.h"
#include "hdr/portab.h"

#include "hdr/ddate.h"
#include "hdr/dtime.h"
#include "hdr/error.h"
#include "hdr/clock.h"
#include "hdr/device.h"
#include "hdr/sft.h"
#include "hdr/kbd.h"
#include "hdr/fcb.h"
#include "hdr/fat.h"
#include "hdr/pcb.h"
#include "hdr/dirmatch.h"
#include "hdr/fnode.h"
#include "hdr/mcb.h"
#include "hdr/lol.h"
#include "hdr/dcb.h"
#include "hdr/cds.h"
#include "hdr/tail.h"
#include "hdr/process.h"
#include "hdr/version.h"
#include "proto.h"
#include "globals.h"
#include "hdr/debug.h"
#include "hdr/buffer.h"
#include "hdr/file.h"
#include "config.h"

#define x86_para2far(seg) (MK_FP((seg), 0))
#define para2far(seg) ((mcb*)ARM_PTR(MK_FP((seg), 0)))

#define open        init_DosOpen

struct config Config = { 0 };
BYTE HaltCpuWhileIdle = 0;
UWORD ram_top = 0;
COUNT UmbState BSS_INIT(0);
STATIC seg base_seg BSS_INIT(0);
STATIC seg umb_base_seg BSS_INIT(0);
UWORD umb_start BSS_INIT(0), UMB_top BSS_INIT(0);
dos_far_ptr lpTop;
COUNT nUnits BSS_INIT(0);
dos_far_ptr InitDiskTransferBuffer = MK_FP(0x8000, 0x0000); // 512b
dos_far_ptr x86_dap = MK_FP(0x8000, 0x0200); // + 512
/*
 00000H 000FFH 00100H PSP                PSP
 00100H 004E1H 003E2H _TEXT              CODE
 004E2H 007A7H 002C6H _IO_TEXT           CODE
 007A8H 008E5H 0013EH _IO_FIXED_DATA     CODE
 008F0H 0139FH 00AB0H _FIXED_DATA        DATA
 013A0H 019F3H 00654H _DATA              DATA
 019F4H 0240DH 00A1AH _BSS               BSS
*/
dos_far_ptr x86_PSP = MK_FP(DOS_PSP, 0x0000); // PSP ядра занимает 0060:0000–0060:00FF
#define x86_DTA MK_FP(DOS_PSP, 0x0080) // Disk Transfer Area
#define x86_IO_FIXED_DATA MK_FP(DOS_PSP, 0x07A8) // _IO_FIXED_DATA -> con_dev
#define x86_FIXED_DATA MK_FP(DOS_PSP, 0x08F0) // _FIXED_DATA -> LoL
#define x86_INTERNAL_DATA MK_FP(DOS_PSP, 0x08F0 + 0x01FB) // internal_data
#define x86_DATA MK_FP(DOS_PSP, 0x13A0) // _DATA
#define x86_BSS MK_FP(DOS_PSP, 0x19F4) // _BSS

_Static_assert(sizeof(struct lol) <= 0x01FB, "LoL overlaps internal_data");

/*UBYTE DiskTransferBuffer[MAX_SEC_SIZE]*/ const dos_far_ptr DiskTransferBuffer = x86_BSS; // BSS
/*struct buffer*/dos_far_ptr x86_firstAvailableBuf;
const dos_far_ptr x86_con_dev = MK_FP(DOS_PSP, 0x07A8); // _IO_FIXED_DATA -> con_dev
const dos_far_ptr x86_prn_dev = MK_FP(DOS_PSP, 0x07A8 + sizeof(struct dhdr));
const dos_far_ptr x86_aux_dev = MK_FP(DOS_PSP, 0x07A8 + sizeof(struct dhdr) * 2);
const dos_far_ptr x86_lpt1_dev= MK_FP(DOS_PSP, 0x07A8 + sizeof(struct dhdr) * 3);
const dos_far_ptr x86_lpt2_dev= MK_FP(DOS_PSP, 0x07A8 + sizeof(struct dhdr) * 4);
const dos_far_ptr x86_lpt3_dev= MK_FP(DOS_PSP, 0x07A8 + sizeof(struct dhdr) * 5);
const dos_far_ptr x86_com1_dev= MK_FP(DOS_PSP, 0x07A8 + sizeof(struct dhdr) * 6);
const dos_far_ptr x86_com2_dev= MK_FP(DOS_PSP, 0x07A8 + sizeof(struct dhdr) * 7);
const dos_far_ptr x86_com3_dev= MK_FP(DOS_PSP, 0x07A8 + sizeof(struct dhdr) * 8);
const dos_far_ptr x86_com4_dev= MK_FP(DOS_PSP, 0x07A8 + sizeof(struct dhdr) * 9);
const dos_far_ptr x86_clk_dev = MK_FP(DOS_PSP, 0x07A8 + sizeof(struct dhdr) * 10);
const dos_far_ptr x86_blk_dev = MK_FP(DOS_PSP, 0x07A8 + sizeof(struct dhdr) * 11);
struct dhdr* con_dev;// = (struct dhdr*)ARM_PTR(x86_con_dev);
struct dhdr* prn_dev;// = (struct dhdr*)ARM_PTR(x86_prn_dev);
struct dhdr* aux_dev;//...
struct dhdr* lpt1_dev;
struct dhdr* lpt2_dev;
struct dhdr* lpt3_dev;
struct dhdr* com1_dev;
struct dhdr* com2_dev;
struct dhdr* com3_dev;
struct dhdr* com4_dev;
struct dhdr* clk_dev;
struct dhdr* blk_dev;
struct lol* LoL;// = (struct lol*)ARM_PTR(x86_FIXED_DATA);
struct dos_data* internal_data;// (struct dos_data*)ARM_PTR(x86_INTERNAL_DATA);

#define NENTRY          26      /* total size of dispatch table */

#define LBA_READ         0x4200
#define LBA_WRITE        0x4300
UWORD LBA_WRITE_VERIFY = 0x4302;
#define LBA_VERIFY       0x4400
#define LBA_FORMAT       0xffff /* fake number for FORMAT track
                                   (only for NON-LBA floppies now!) */

static KernelConfig InitKernelConfig = {
    .CONFIG = {'C','O','N','F','I','G'},
    .ConfigSize = sizeof(KernelConfig) - 8, // без signature[6] и config_size

    .DLASortByDriveNo = 0,
    .InitDiskShowDriveAssignment = 1,
    .SkipConfigSeconds = 2,
    .ForceLBA = 0,
    .GlobalEnableLBAsupport = 1,
    .BootHarddiskSeconds = 0,

    .Version_OemID = 0xFD,
    .Version_Major = 2,
    .Version_Revision = 43,
    .Version_Release = 1,

    .CheckDebugger = 0,
    .Verbose = 0,

    .PartitionMode = 0x1F
};

static void rq_done(request FAR *rq)
{
    rq->r_status = S_DONE;
}

static void rq_busy_done(request FAR *rq)
{
    rq->r_status = S_DONE | S_BUSY;
}

static void rq_error(request FAR *rq, UBYTE err)
{
    rq->r_status = S_ERROR | S_DONE | err;
}

static void ConIntr(request FAR *rq) {
    /*
     * CON device interrupt entry.
     *
     * Handles DOS character-device requests for console input/output:
     * init, input status, input, non-destructive input, output, flush,
     * ioctl where supported.
     *
     * Native implementation should bridge these requests to the emulator's
     * keyboard and screen/TTY backend, then update request status/count
     * exactly like a DOS character driver.
     */
    switch (rq->r_command) {
    case C_INIT:
        rq_done(rq);
        break;

    default:
        /*
         * CON command table will be filled later.
         * For now only INIT is required by InitIO().
         */
        rq_error(rq, E_CMD);
        break;
    }
}

static void PrnIntr(request FAR *rq) {
    /*
     * PRN pseudo-device interrupt entry.
     *
     * In FreeDOS this is a generic printer device that can be redirected
     * to selected LPT/COM targets by MODE. Native implementation can begin
     * as "not ready" or route output to the configured printer/log backend.
     */
    switch (rq->r_command) {
    case C_INIT:
        rq_done(rq);
        break;
    default:
        rq_error(rq, E_CMD);
        break;
    }
}

static void AuxIntr(request FAR *rq) {
    /*
     * AUX / COM1-style serial pseudo-device interrupt entry.
     *
     * Used for AUX and COM1 in io.asm. Native implementation should map
     * character read/write/status requests to the configured serial backend,
     * or return a DOS device error if serial is not available.
     */
    switch (rq->r_command) {
    case C_INIT:
        rq_done(rq);
        break;
    default:
        rq_error(rq, E_CMD);
        break;
    }
}

static void Lpt1Intr(request FAR *rq) {
    /*
     * LPT1 printer device interrupt entry.
     *
     * Native implementation should handle printer output/status for LPT1.
     * Minimal safe behaviour: report not ready / command error for unsupported
     * operations while still completing init/status requests consistently.
     */
    PrnIntr(rq);
}

static void Lpt2Intr(request FAR *rq) {
    /*
     * LPT2 printer device interrupt entry.
     *
     * Same semantics as LPT1, but for logical printer unit 2.
     */
    PrnIntr(rq);
}

static void Lpt3Intr(request FAR *rq) {
    /*
     * LPT3 printer device interrupt entry.
     *
     * Same semantics as LPT1, but for logical printer unit 3.
     */
    PrnIntr(rq);
}

static void Com2Intr(request FAR *rq) {
    /*
     * COM2 serial device interrupt entry.
     *
     * Same character-device request model as AUX/COM1, mapped to logical
     * serial unit 2.
     */
    AuxIntr(rq);
}

static void Com3Intr(request FAR *rq) {
    /*
     * COM3 serial device interrupt entry.
     *
     * Same character-device request model as AUX/COM1, mapped to logical
     * serial unit 3.
     */
    AuxIntr(rq);
}

static void Com4Intr(request FAR *rq) {
    /*
     * COM4 serial device interrupt entry.
     *
     * Same character-device request model as AUX/COM1, mapped to logical
     * serial unit 4.
     */
    AuxIntr(rq);
}

UWORD ASM DaysSinceEpoch = 0;
typedef UDWORD ticks_t;

static void ClkEntry(request FAR *rq) {
    /*
     * CLOCK$ device interrupt entry.
     *
     * Handles DOS clock-device requests. Native implementation should bridge
     * date/time operations to the emulator BIOS time source / RTC layer and
     * fill the request packet using DOS CLOCK$ transfer format.
     */
    switch (rq->r_command) {
    case C_INIT:
        rq->r_nunits = 0;
        rq_done(rq);
        break;
    case C_OFLUSH:
    case C_IFLUSH:
        rq_done(rq);
        break;
    case C_INPUT:
      {
        struct ClockRecord clk;
        uint32_t bios_ticks;
        uint32_t total_hundredths;

        if (sizeof(struct ClockRecord) != rq->r_count) {
            rq_error(rq, E_LENGTH);
            break;
        }

        /*
         * BDA 0040:006C contains BIOS timer ticks since midnight.
         * Standard PC tick rate is PIT_FREQ / 65536 ~= 18.2065 Hz.
         *
         * Convert BIOS ticks to hundredths of second:
         *
         *   hundredths = ticks * 100 * 65536 / PIT_FREQ
         *
         * Use 64-bit intermediate to avoid overflow.
         */
        bios_ticks = pload32(0x46C);
        total_hundredths =
            (uint32_t)(((uint64_t)bios_ticks * 100u * 65536u) / PIT_FREQ);

        total_hundredths %= 24u * 60u * 60u * 100u;

        clk.clkHours = total_hundredths / (60u * 60u * 100u);
        total_hundredths %= 60u * 60u * 100u;

        clk.clkMinutes = total_hundredths / (60u * 100u);
        total_hundredths %= 60u * 100u;

        clk.clkSeconds = total_hundredths / 100u;
        clk.clkHundredths = total_hundredths % 100u;

        clk.clkDays = DaysSinceEpoch;

        memcpy(rq->r_trans, &clk, sizeof(struct ClockRecord));
      }
        rq_done(rq);
        break;
    case C_OUTPUT:
      {
        struct ClockRecord clk;
        uint32_t total_hundredths;
        uint32_t bios_ticks;

        if (sizeof(struct ClockRecord) != rq->r_count) {
            rq_error(rq, E_LENGTH);
            break;
        }

        memcpy(&clk, rq->r_trans, sizeof(struct ClockRecord));

        /*
         * Store DOS date counter.
         * clkDays is days since 1980-01-01.
         */
        DaysSinceEpoch = clk.clkDays;

        /*
         * Convert CLOCK$ time to BIOS ticks since midnight.
         *
         * BDA 0040:006C stores ticks at PIT_FREQ / 65536 Hz.
         *
         *   ticks = hundredths * PIT_FREQ / (100 * 65536)
         *
         * Use 64-bit intermediate to avoid overflow.
         */
        total_hundredths =
            ((uint32_t)clk.clkHours * 60u * 60u * 100u) +
            ((uint32_t)clk.clkMinutes * 60u * 100u) +
            ((uint32_t)clk.clkSeconds * 100u) +
            (uint32_t)clk.clkHundredths;

        total_hundredths %= 24u * 60u * 60u * 100u;

        bios_ticks =
            (uint32_t)(((uint64_t)total_hundredths * PIT_FREQ) /
                       (100u * 65536u));

        pstore32(0x46C, bios_ticks);
        pstore8(0x470, 0);   /* midnight rollover flag */        
      }
        rq_done(rq);
        break;
    default:
        rq_error(rq, E_FAILURE);
        break;
    }
}

static void BlkEntry(request FAR *rq) {
    /*
     * Internal block-device interrupt entry.
     *
     * Handles block reads/writes/ioctl/media checks for DOS logical drives.
     * Native implementation should call the RP2350-side FAT/disk backend
     * instead of executing x86 INT 13h-heavy FreeDOS block driver code.
     */
    switch (rq->r_command) {
    case C_INIT:
        /*
         * Internal block device: dh_name[0] is unit count, not ASCII name.
         * dsk_init() later should replace this with the real detected count.
         */
        rq->r_nunits = blk_dev->dh_name[0];
        rq_done(rq);
        break;

    default:
        rq_error(rq, E_CMD);
        break;
    }
}

static void NulIntr(request FAR *rq) {
    /*
     * NUL device interrupt entry.
     *
     * Original kernel.asm behaviour:
     * - for read request, set transferred count to 0;
     * - mark request as done.
     *
     * Native implementation should complete all supported NUL requests
     * successfully, discard writes, and return EOF/zero bytes for reads.
     */
    switch (rq->r_command) {
    case C_INIT:
        rq_done(rq);
        break;

    default:
        /*
         * Safe minimal NUL behavior:
         * - reads return zero bytes;
         * - writes are discarded;
         * - unsupported control commands can be tightened later.
         */
        rq->r_count = 0;
        rq_done(rq);
        break;
    }
}

const static struct dhdr _blk_dev = {
    .dh_next = MK_FP(-1, -1),
    .dh_attr = 0x08c2 | ATTR_NATIVE,
    .arm.dh_interrupt = BlkEntry,
    .dh_name = { 4, 0, 0, 0, 0, 0, 0, 0 },
};

const static struct dhdr _clk_dev = {
    .dh_next = x86_blk_dev,
    .dh_attr = 0x8008 | ATTR_NATIVE,
    .arm.dh_interrupt = ClkEntry,
    .dh_name = "CLOCK$  "
};

const static struct dhdr _com4_dev = {
    .dh_next = x86_clk_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com4Intr,
    .dh_name = "COM4    "
};

const static struct dhdr _com3_dev = {
    .dh_next = x86_com4_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com3Intr,
    .dh_name = "COM3    "
};

const static struct dhdr _com2_dev = {
    .dh_next = x86_com3_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com2Intr,
    .dh_name = "COM2    "
};

const static struct dhdr _com1_dev = {
    .dh_next = x86_com2_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = AuxIntr,
    .dh_name = "COM1    "
};

const static struct dhdr _lpt3_dev = {
    .dh_next = x86_com1_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt3Intr,
    .dh_name = "LPT3    "
};

const static struct dhdr _lpt2_dev = {
    .dh_next = x86_lpt3_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt2Intr,
    .dh_name = "LPT2    "
};

const static struct dhdr _lpt1_dev = {
    .dh_next = x86_lpt2_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt1Intr,
    .dh_name = "LPT1    "
};

const static struct dhdr _aux_dev = {
    .dh_next = x86_lpt1_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = AuxIntr,
    .dh_name = "AUX     "
};

const static struct dhdr _prn_dev = {
    .dh_next = x86_aux_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = PrnIntr,
    .dh_name = "PRN     "
};

const static struct dhdr _con_dev = {
    .dh_next = x86_prn_dev,
    .dh_attr = 0x8013 | ATTR_NATIVE,
    .arm.dh_interrupt = ConIntr,
    .dh_name = "CON     "
};

const static struct lol lol = {
    .dos_data = 0,              /* 0x00  abs / -0x26 rel: SDA format byte (0=DOS3.x, 1=DOS4+) */
    .kernel_start_off = 0,      /* 0x01  abs / -0x25 rel: offset of kernel_start, may be 0x0100?, in case somebody reads it*/
    ._pad0 = 0,                 /* 0x03  abs / -0x23 rel: padding */
    .version_style = 1,         /* 0x04  abs / -0x22 rel: 1 = MS-DOS 4.0+ style */
//    ._pad1[8] = {0,0,0,0,0,0,0,0},/* 0x06  abs / -0x20 rel: padding to NetBios */
    .NetBios = 0,               /* 0x0E  abs / -0x18 rel: NetBios Number */
//    ._pad2[10] = { 0 },       /* 0x10  abs / -0x16 rel: padding to NetRetry */
    .NetRetry = 3,              /* 0x1A  abs / -0x0C rel: network retry count */
    .NetDelay = 1,              /* 0x1C  abs / -0x0A rel: network delay count */
    .DskBuffer = MK_FP(-1,-1),  /* 0x1E  abs / -0x08 rel: current dos disk buffer */
    .inputptr = 0,              /* 0x22  abs / -0x04 rel: unread CON input */
    .first_mcb = 0,             /* 0x24  abs / -0x02 rel: start of user memory */
    /* === MARK0026H, offset 0x26 = offsetof(struct lol, DPBp) */
    .DPBp        = MK_FP(-1,-1),
    .sfthead     = 0,
    .clock       = x86_clk_dev,
    .syscon      = x86_con_dev,
    .maxsecsize  = 512,
    .CDSp        = 0,
    .FCBp        = 0,
    .nprotfcb    = 0,
    .nblkdev     = 0,
    .lastdrive   = 0,

    .nul_dev = {
        .dh_next = x86_con_dev,
        .dh_attr = 0x8004 | ATTR_NATIVE,
        .arm.dh_interrupt = NulIntr,
        .dh_name = "NUL     "
    },

    .njoined     = 0,
    .setverPtr   = 0,
    .nbuffers    = 1,
    .nlookahead  = 1,
    .BootDrive   = 1,
    .cpu         = 0,
    .xmssize     = 0,
    .firstbuf    = 0,
    .dirtybuf    = 0,
    .lookahead   = 0,
    .slookahead  = 0,
    .bufloc      = 0,
    .deblock_buf = 0,

    .uppermem_root = 0xffff,
    .last_para = 0,

    .os_setver_minor = 0,
    .os_setver_major = 5,
    .os_minor        = 0,
    .os_major        = 5,
    .rev_number      = 0,
    .version_flags   = 0,
    .os_release      = offsetof(struct lol, os_release_str), /* near ptr на строку os_release */
    .os_release_str  = KERNEL_VERSION,
    .aux_str = "AUX",
    .con_str = "CON",
    .prn_str = "PRN",
};

static void x86_execrh() {
  /// TODO: see execrh.asm
        print_line("KERNEL INIT TODO (x86_execrh)", 1);
        while(1);
}

WORD ASMPASCAL execrh(request FAR * rq, /*struct dhdr*/ dos_far_ptr _dhp) {
  struct dhdr* dhp = (struct dhdr*)ARM_PTR(_dhp);
  if (dhp->dh_attr & ATTR_NATIVE) {
      dhp->arm.dh_interrupt(rq);
  } else {
      x86_execrh();
  }
  return rq->r_status;
}

STATIC VOID mcb_init_copy(UCOUNT seg, UWORD size, mcb *near_mcb)
{
  near_mcb->m_size = size;
  memcpy(ARM_PTR(MK_FP(seg, 0)), near_mcb, sizeof(mcb));
}

STATIC VOID mcb_init(UCOUNT seg, UWORD size, BYTE type)
{
  static mcb near_mcb BSS_INIT({0}); /// TODO: _BSS
  near_mcb.m_type = type;
  mcb_init_copy(seg, size, &near_mcb);
}

STATIC VOID mumcb_init(UCOUNT seg, UWORD size)
{
  static mcb near_mcb = {
    MCB_NORMAL,
    8, 0,
    {0,0,0},
    {"SC"}
  };
  mcb_init_copy(seg, size, &near_mcb);
}

#pragma pack(push, 1)
struct submcb
{
  char type;
  unsigned short start;
  unsigned short size;
  char unused[3];
  char name[8];
};
#pragma pack(pop)

dos_far_ptr KernelAllocPara(size_t nPara, char type, char *name, int mode)
{
  seg base, start;

  /* if no umb available force low allocation */
  if (UmbState != 1)
    mode = 0;

  if (mode)
  {
    base = umb_base_seg;
    start = umb_start;
  }
  else
  {
    base = base_seg;
    start = LoL->first_mcb;
  }

  /* create the special DOS data MCB if it doesn't exist yet */
  CfgDbgPrintf(("kernelallocpara: %x %x %x %c %d\n", start, base, nPara, type, mode));

  if (base == start)
  {
    /*mcb*/ dos_far_ptr x86_p = x86_para2far(base);
    mcb* p = (mcb*)ARM_PTR(x86_p);
    base++;
    mcb_init(base, p->m_size - 1, p->m_type);
    mumcb_init(FP_SEG(x86_p), 0);
    p->m_name[1] = 'D';
  }

  nPara++;
  mcb_init(base + nPara, para2far(base)->m_size - nPara, para2far(base)->m_type);
  para2far(start)->m_size += nPara;

  struct submcb* p = (struct submcb*)para2far(base);
  p->type = type;
  p->start = base + 1;
  p->size = nPara-1;
  if (name)
    memcpy(p->name, name, 8);
  base += nPara;
  if (mode)
    umb_base_seg = base;
  else
    base_seg = base;

  return MK_FP(base+1, 0);
}

STATIC dos_far_ptr AlignParagraph(dos_far_ptr lpPtr)
{
  UWORD uSegVal;

  /* First, convert the segmented pointer to linear address       */
  uSegVal = FP_SEG(lpPtr);
  uSegVal += (FP_OFF(lpPtr) + 0xf) >> 4;
  if (FP_OFF(lpPtr) > 0xfff0)
    uSegVal += 0x1000;          /* handle overflow */

  /* and return an adddress adjusted to the nearest paragraph     */
  /* boundary.                                                    */
  return MK_FP(uSegVal, 0);
}

dos_far_ptr KernelAlloc(size_t nBytes, char type, int mode)
{
  dos_far_ptr p;
  size_t nPara = (nBytes + 15)/16;

  if (LoL->first_mcb == 0)
  {
    /* prealloc */
    lpTop = MK_FP(FP_SEG(lpTop) - nPara, FP_OFF(lpTop));
    p = AlignParagraph(lpTop);
  }
  else
  {
    p = KernelAllocPara(nPara, type, NULL, mode);
  }
  fmemset(p, 0, nBytes);
  return p;
}
dos_far_ptr DynAlloc(char *what, unsigned num, unsigned size);

/* check for a block device and update  device control block    */
STATIC VOID update_dcb(/*struct dhdr*/ dos_far_ptr x86_dhp)
{
  struct dhdr* dhp = (struct dhdr*)ARM_PTR(x86_dhp);
  REG COUNT Index;
  COUNT nunits = dhp->dh_name[0];
   // Drive Parameter Block: описание одного DOS-диска/логического drive unit.
   // Через DPB DOS связывает букву диска с конкретным block-device driver и его subunit.
  dos_far_ptr x86_dpb;

  /* printf("nblkdev = %i\n", LoL->nblkdev); */
  
  /* if no units, nothing to do, ensure at least 1 unit for rest of logic */
  if (nunits == 0) return;

  /* allocate memory for new device control blocks, insert into chain [at end], and update our pointer to new end */
  if ( LoL->first_mcb ) { // MCB exist, use KernelAlloc
    x86_dpb = KernelAlloc(nunits * sizeof(struct dpb), 'E', Config.cfgDosDataUmb);
  }
  else { // no MCB, use temporaty flow (TODO: ensure)
    x86_dpb = DynAlloc("DPBp", blk_dev->dh_name[0], sizeof(struct dpb));
  }
  struct dpb FAR *dpb = (struct dpb*)ARM_PTR(x86_dpb);

  /* find end of dpb chain or initialize root if needed */
  if (LoL->nblkdev == 0)
  {
    /* update root pointer to new end (our just allocated block) */
    LoL->DPBp = x86_dpb;
  }  
  else
  {
    struct dpb FAR *tmp_dpb;
    /* find current end of dpb chain by following next pointers to end */
    for (
        tmp_dpb = (struct dpb*)ARM_PTR(LoL->DPBp);
        FP_SEG(tmp_dpb->dpb_next) != 0xFFFF && FP_OFF(tmp_dpb->dpb_next) != 0xFFFF;
        tmp_dpb = (struct dpb*)ARM_PTR(tmp_dpb->dpb_next)
    )
      ;
    /* insert into chain [at end] */
    tmp_dpb->dpb_next = x86_dpb;
  }
  /* dpb points to last block, one just allocated */

  for (Index = 0; Index < nunits; Index++)
  {		
    /* printf("processing unit %i of %i nunits\n", Index, nunits); */
    dpb->dpb_next = MK_FP(FP_SEG(x86_dpb), FP_OFF(x86_dpb) + sizeof(struct dpb));  /* memory allocated as array, so next is just next element */
    dpb->dpb_unit = LoL->nblkdev;
    dpb->dpb_subunit = Index;
    dpb->dpb_device = x86_dhp;
    dpb->dpb_flags = M_CHANGED;
    // LoL->CDSp: Current Directory Structure
    if ((FP_SEG(LoL->CDSp) != 0) && (LoL->nblkdev < LoL->lastdrive))
    {
      struct cds* CDSp = (struct cds*)ARM_PTR(LoL->CDSp);
      CDSp[LoL->nblkdev].cdsDpb = dpb;
      CDSp[LoL->nblkdev].cdsFlags = CDSPHYSDRV;
    }
	
    ++dpb;  /* dbp = dbp->dpb_next; */
    ++LoL->nblkdev;
  }
  /* note that always at least 1 valid dpb due to above early exit if nunits==0 */
  (dpb - 1)->dpb_next = MK_FP(-1, -1);

  /* printf("processed %i nunits\n", nunits); */
}

/* If cmdLine is NULL, this is an internal driver */

BOOL init_device(/*struct dhdr*/ dos_far_ptr x86_dhp, char *cmdLine, COUNT mode,
                 dos_far_ptr * r_top)
{
  struct dhdr* dhp = (struct dhdr*)ARM_PTR(x86_dhp);
  request rq = { 0 };
  char name[8];

  if (cmdLine) {
    char *p, *q, ch;
    int i;

    p = q = cmdLine;
    for (;;)
    {
      ch = *p;
      if (ch == '\0' || ch == ' ' || ch == '\t')
        break;
      p++;
      if (ch == '\\' || ch == '/' || ch == ':')
        q = p; /* remember position after path */
    }
    for (i = 0; i < 8; i++) {
      ch = '\0';
      if (p != q && *q != '.')
        ch = *q++;
      /* copy name, without extension */
      name[i] = ch;
    }
  }

  rq.r_unit = 0;
  rq.r_status = 0;
  rq.r_command = C_INIT;
  rq.r_length = sizeof(request);
  rq.r_endaddr = *r_top;
  rq.r_bpbptr = (void FAR *)(cmdLine ? cmdLine : "\n");
  rq.r_firstunit = LoL->nblkdev;

  execrh((request FAR *) & rq, x86_dhp);

/*
 *  Added needed Error handle
 */
  if ((rq.r_status & (S_ERROR | S_DONE)) == S_ERROR)
    return TRUE;

  if (cmdLine)
  {
    /* Don't link in device drivers which do not take up memory */
    if ((struct dhdr*)ARM_PTR(rq.r_endaddr) == dhp)
      return TRUE;

    /* Don't link in block device drivers which indicate no units */
    if (!(dhp->dh_attr & ATTR_CHAR) && !rq.r_nunits)
    {
      rq.r_endaddr = x86_dhp;
      return TRUE;
    }


    /* Fix for multisegmented device drivers:                          */
    /*   If there are multiple device drivers in a single driver file, */
    /*   only the END ADDRESS returned by the last INIT call should be */
    /*   the used.  It is recommended that all the device drivers in   */
    /*   the file return the same address                              */
    if (FP_OFF(dhp->dh_next) == 0xffff) {
        KernelAllocPara(FP_SEG(rq.r_endaddr) + (FP_OFF(rq.r_endaddr) + 15)/16 - FP_SEG(x86_dhp), 'D', name, mode);
    }

    /* Another fix for multisegmented device drivers:                  */
    /*   To help emulate the functionallity experienced with other DOS */
    /*   operating systems when calling multiple device drivers in a   */
    /*   single driver file, save the end address returned from the    */
    /*   last INIT call which will then be passed as the end address   */
    /*   for the next INIT call.                                       */
    *r_top = rq.r_endaddr;
  }

  if (!(dhp->dh_attr & ATTR_CHAR) && (rq.r_nunits != 0))
  {
    dhp->dh_name[0] = rq.r_nunits;
    update_dcb(x86_dhp);
  }

  if (dhp->dh_attr & ATTR_CONIN)
    LoL->syscon = x86_dhp;
  else if (dhp->dh_attr & ATTR_CLOCK)
    LoL->clock = x86_dhp;

  return FALSE;
}

STATIC void InitIO()
{
    dos_far_ptr x86_device = x86_FAR_PTR(DOS_PSP, &LoL->nul_dev);
    struct dhdr* device = (struct dhdr*)ARM_PTR(x86_device);
    /* Initialize driver chain                                      */
    do {
        init_device(x86_device, NULL, 0, &lpTop);
        x86_device = device->dh_next;
        device = (struct dhdr*)ARM_PTR(x86_device);
    }
    while (FP_OFF(x86_device) != 0xffff);
}

static void init_PSPSet(u16 psp) {
    CPU_AH = 0x50; // Set Current PSP
    CPU_BX = psp;
    fdos_21h(cpu);
}

static void set_DTA(dos_far_ptr p) {
    CPU_AH = 0x1A; // Set Current DTA
    SET_DS (FP_SEG(p));
    CPU_DX = p.offset;
    fdos_21h(cpu);
}

static dos_far_ptr getvec(uint8_t intno) {
    uint32_t res = pload32(4ul * intno);
    return *(dos_far_ptr*)&res;
}

STATIC void PSPInit(void)
{
  psp far *p = (psp far *) ARM_PTR(x86_PSP);

  /* Clear out new psp first                              */
  memset(p, 0, sizeof(psp));
  /* high half is used as environment */

  /* initialize all entries and exits                     */
  /* CP/M-like exit point                                 */
  p->ps_exit = 0x20cd;

  /* CP/M-like entry point - call far to special entry    */
  p->ps_farcall = 0x9a;
  p->ps_reentry = MK_FP(0, 0x30 * 4);
  /* unix style call - 0xcd 0x21 0xcb (int 21, retf)      */
  p->ps_unix[0] = 0xcd;
  p->ps_unix[1] = 0x21;
  p->ps_unix[2] = 0xcb;

  /* Now for parent-child relationships                   */
  /* parent psp segment                                   */
  p->ps_parent = FP_SEG(x86_PSP);
  /* previous psp pointer                                 */
  p->ps_prevpsp = MK_FP(0xffff,0xffff);

  /* Environment and memory useage parameters             */
  /* memory size in paragraphs                            */
  /*  p->ps_size = 0; clear from above                    */
  /* environment paragraph                                */
  p->ps_environ = DOS_PSP + 8;
  /* terminate address                                    */
  p->ps_isv22 = getvec(0x22);
  /* break address                                        */
  p->ps_isv23 = getvec(0x23);
  /* critical error address                               */
  p->ps_isv24 = getvec(0x24);

  /* user stack pointer - int 21                          */
  /* p->ps_stack = NULL; clear from above                 */

  /* File System parameters                               */
  /* maximum open files                                   */
  p->ps_maxfiles = 20;
  memset(p->ps_files, 0xff, 20);

  /* open file table pointer                              */
  p->ps_filetab = p->ps_files;

  /* default system version for int21/ah=30               */
  p->ps_retdosver = (LoL->os_setver_minor << 8) + LoL->os_setver_major;

  /* first command line argument                          */
  /* p->ps_fcb1.fcb_drive = 0; already set                */
  memset(p->ps_fcb1.fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);
  /* second command line argument                         */
  /* p->ps_fcb2.fcb_drive = 0; already set                */
  memset(p->ps_fcb2.fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);

  /* do not modify command line tail, used as environment */
}

STATIC int InitBcdToByte(int x)
{
  return ((x >> 4) & 0xf) * 10 + (x & 0xf);
}

void Init_clk_driver(void)
{
  /* get BIOS time */
  CPU_AH = 2;
  bios_1Ah(cpu);

  /* DosSetTime */
  CPU_AH = 0x2d;
  CPU_CL = InitBcdToByte(CPU_CL);   /* minutes */
  CPU_CH = InitBcdToByte(CPU_CH);   /* hours   */
  CPU_DH = InitBcdToByte(CPU_DH);   /*seconds */
  CPU_DL = 0;
  fdos_21h(cpu);

  /* get BIOS date */
  CPU_AH = 4;
  bios_1Ah(cpu);

  /* DosSetDate */
  CPU_AH = 0x2b;
  CPU_CX = 100 * InitBcdToByte(CPU_CH) /* century */
               + InitBcdToByte(CPU_CL);/* year */
  /* A BIOS with y2k (year 2000) bug will always report year 19nn */
  if ((CPU_CX >= 1900) && (CPU_CX < 1980)) CPU_CX += 100;
  CPU_DH = InitBcdToByte(CPU_DH);   /* month */
  CPU_DL = InitBcdToByte(CPU_DL);   /* day   */
  fdos_21h(cpu);

}

void BIOS_drive_reset(unsigned drive)
{
    CPU_DL = drive | 0x80;
    CPU_AH = 0;
    bios_13h(cpu);
}

/*
    internal global data
*/

BOOL ExtLBAForce = FALSE;

COUNT init_readdasd(UBYTE drive)
{
  CPU_AH = 0x15;
  CPU_DL = drive;
  bios_13h(cpu); // GET DISK TYPE
  if (!cf)
    switch (CPU_AH)
    {
      case 2:
        return DF_CHANGELINE;
      case 3:
        return DF_FIXED;
    }
  return 0;
}

typedef struct {
  UWORD bpb_nbyte;              /* Bytes per Sector             */
  UBYTE bpb_nsector;            /* Sectors per Allocation Unit  */
  UWORD bpb_nreserved;          /* # Reserved Sectors           */
  UBYTE bpb_nfat;               /* # FATs                       */
  UWORD bpb_ndirent;            /* # Root Directory entries     */
  UWORD bpb_nsize;              /* Size in sectors              */
  UBYTE bpb_mdesc;              /* MEDIA Descriptor Byte        */
  UWORD bpb_nfsect;             /* FAT size in sectors          */
  UWORD bpb_nsecs;              /* Sectors per track            */
  UWORD bpb_nheads;             /* Number of heads              */
} floppy_bpb;

#define FLOPPY_SEC_SIZE 512u  /* common sector size */

floppy_bpb floppy_bpbs[5] = {
/* copied from Brian Reifsnyder's FORMAT, bpb.h */
  {FLOPPY_SEC_SIZE, 2, 1, 2, 112, 720, 0xfd, 2, 9, 2}, /* FD360  5.25 DS   */
  {FLOPPY_SEC_SIZE, 1, 1, 2, 224, 2400, 0xf9, 7, 15, 2},       /* FD1200 5.25 HD   */
  {FLOPPY_SEC_SIZE, 2, 1, 2, 112, 1440, 0xf9, 3, 9, 2},        /* FD720  3.5  LD   */
  {FLOPPY_SEC_SIZE, 1, 1, 2, 224, 2880, 0xf0, 9, 18, 2},       /* FD1440 3.5  HD   */
  {FLOPPY_SEC_SIZE, 2, 1, 2, 240, 5760, 0xf0, 9, 36, 2}        /* FD2880 3.5  ED   */
};

COUNT init_getdriveparm(UBYTE drive, bpb * pbpbarray)
{
  REG UBYTE type;

  if (drive & 0x80)
    return 5;
  CPU_AH = 0x08;
  CPU_DL = drive;
  bios_13h(cpu); // GET DRIVE PARAMETERS
  type = CPU_BL - 1;
  if (cf)
    type = 0;                   /* return 320-360 for XTs */
  else if (type > 6)
    type = 8;                   /* any odd ball drives get 8&7=0: the 320-360 table */
  else if (type == 5)
    type = 4;                   /* 5 and 4 are both 2.88 MB */

  memcpy(pbpbarray, &floppy_bpbs[type & 7], sizeof(floppy_bpb));
  ((bpb *)pbpbarray)->bpb_hidden = 0;  /* very important to init to 0, see bug#1789 */
  ((bpb *)pbpbarray)->bpb_huge = 0;

  if (type == 3)
    return 7;                   /* 1.44 MB */

  if (type == 4)
    return 9;                   /* 2.88 almost forgot this one */

  /* 0=320-360kB, 1=1.2MB, 2=720kB, 8=any odd ball drives */
  return type;
}

#pragma pack(push, 1)
struct DynS {
  UWORD Allocated;
};
#pragma pack(pop)

void* fmemset(dos_far_ptr p, int v, unsigned int sz) {
    void* res = ARM_PTR(p);
    memset(res, v, sz);
    return res;
}

void fmemcpy(dos_far_ptr d, const dos_far_ptr s, size_t n) {
    memcpy(ARM_PTR(d), ARM_PTR(s), n);
}

dos_far_ptr DynAlloc(char *what, unsigned num, unsigned size)
{
  unsigned total = num * size;
  static dos_far_ptr Dyn = MK_FP(0x9000, 0); // 64k from 0x9000:0000 to 0x1A00:0000
  struct DynS far *Dynp = (struct DynS far *)ARM_PTR(Dyn);

#ifndef DEBUG
  UNREFERENCED_PARAMETER(what);
#endif

  if ((ULONG) total + Dynp->Allocated > 0xffff)
  {
    printf("PANIC:Dyn %lu\n", (ULONG) total + Dynp->Allocated);
    for (;;) ;
  }

  DebugPrintf(("DYNDATA:allocating %s - %u * %u bytes, total %u, %u..%u\n",
               what, num, size, total, Dynp->Allocated,
               Dynp->Allocated + total));

  dos_far_ptr now = MK_FP(FP_SEG(Dyn), Dynp->Allocated + sizeof(struct DynS));
  fmemset(now, 0, total);

  Dynp->Allocated += total;

  return now;
}

STATIC void push_ddt(ddt *pddt)
{
  dos_far_ptr fddt = DynAlloc("ddt", 1, sizeof(ddt));
  memcpy(ARM_PTR(fddt), pddt, sizeof(ddt));
  if (pddt->ddt_logdriveno != 0) {
    ((ddt*)ARM_PTR(fddt) - 1)->ddt_next = fddt;
    if (pddt->ddt_driveno == 0 && pddt->ddt_logdriveno == 1)
      ((ddt*)ARM_PTR(fddt) - 1)->ddt_descflags |= DF_CURLOG | DF_MULTLOG;
  }
}

STATIC void make_ddt (ddt *pddt, int Unit, int driveno, int flags)
{
  pddt->ddt_next = MK_FP(0, 0xffff);
  pddt->ddt_logdriveno = Unit;
  pddt->ddt_driveno = driveno;
  pddt->ddt_type = init_getdriveparm(driveno, &pddt->ddt_defbpb);
  pddt->ddt_ncyl = (pddt->ddt_type & 7) ? 80 : 40;
  pddt->ddt_descflags = init_readdasd(driveno) | flags;

  pddt->ddt_offset = 0;
  pddt->ddt_serialno = 0x12345678l;
  memcpy(&pddt->ddt_bpb, &pddt->ddt_defbpb, sizeof(bpb));
  push_ddt(pddt);
}

int BIOS_nrdrives(void)
{
  CPU_AH = 0x08;
  CPU_DL = 0x80;
  bios_13h(cpu); // GET DRIVE PARAMETERS
  if (cf)
  {
    printf("no hard disks detected\n");
    return 0;
  }
  return CPU_DL;
}

#define SCAN_PRIMARYBOOT 0x00
#define SCAN_PRIMARY     0x01
#define SCAN_EXTENDED    0x02
#define SCAN_PRIMARY2    0x03

#define FAT12           0x01
#define FAT16SMALL      0x04
#define EXTENDED        0x05
#define FAT16LARGE      0x06
#define FAT32           0x0b    /* FAT32 partition that ends before the 8.4  */
                              /* GB boundary                               */
#define FAT32_LBA       0x0c    /* FAT32 partition that ends after the 8.4GB */
                              /* boundary.  LBA is needed to access this.  */
#define FAT16_LBA       0x0e    /* like 0x06, but it is supposed to end past */
                              /* the 8.4GB boundary                        */
#define FAT12_LBA       0xff    /* fake FAT12 LBA entry for internal use     */
#define EXTENDED_LBA    0x0f    /* like 0x05, but it is supposed to end past */

/* Let's play it safe and do not allow partitions with clusters above  *
 * or equal to 0xff0/0xfff0/0xffffff0 to be created                    *
 * the problem with fff0-fff6 is that they might be interpreted as BAD *
 * even though the standard BAD value is ...ff7                        */

#define FAT12MAX        (FAT_MAGIC-6)
#define FAT16MAX        (FAT_MAGIC16-6)
#define FAT32MAX        (FAT_MAGIC32-6)

#define IsExtPartition(parttyp) ((parttyp) == EXTENDED || \
                                 (parttyp) == EXTENDED_LBA )

#define IsLBAPartition(parttyp) ((parttyp) == FAT12_LBA  || \
                                 (parttyp) == FAT16_LBA  || \
                                 (parttyp) == FAT32_LBA)

#ifdef WITHFAT32
#define IsFATPartition(parttyp) ((parttyp) == FAT12      || \
                                 (parttyp) == FAT16SMALL || \
                                 (parttyp) == FAT16LARGE || \
                                 (parttyp) == FAT16_LBA  || \
                                 (parttyp) == FAT32      || \
                                 (parttyp) == FAT32_LBA)
#else
#define IsFATPartition(parttyp) ((parttyp) == FAT12      || \
                                 (parttyp) == FAT16SMALL || \
                                 (parttyp) == FAT16LARGE || \
                                 (parttyp) == FAT16_LBA)
#endif

#define MSDOS_EXT_SIGN 0x29     /* extended boot sector signature */
#define MSDOS_FAT12_SIGN "FAT12   "     /* FAT12 filesystem signature */
#define MSDOS_FAT16_SIGN "FAT16   "     /* FAT16 filesystem signature */
#define MSDOS_FAT32_SIGN "FAT32   "     /* FAT32 filesystem signature */

/* local - returned and used for BIOS interface INT 13, AH=48*/
struct _bios_LBA_disk_parameterS {
  UWORD size;
  UWORD information;
  ULONG cylinders;
  ULONG heads;
  ULONG sectors;

  ULONG totalSect;
  ULONG totalSectHigh;
  UWORD BytesPerSector;

  ULONG eddparameters;
};

struct PartTableEntry           /* INTERNAL representation of partition table entry */
{
  UBYTE Bootable;
  UBYTE FileSystem;
  struct CHS Begin;
  struct CHS End;
  ULONG RelSect;
  ULONG NumSect;
};

struct DriveParamS {
  UBYTE driveno;                /* = 0x8x                           */
  UWORD descflags;
  ULONG total_sectors;

  struct CHS chs;               /* for normal   INT 13 */
};

/* Get the parameters of the hard disk */
STATIC int LBA_Get_Drive_Parameters(int drive, struct DriveParamS *driveParam, int firstPass)
{
  if (firstPass && (InitKernelConfig.Verbose >= 1))
    printf("Checking for LBA support in BIOS for drive %02x\n", drive);

  memset(driveParam, 0, sizeof *driveParam);
  drive |= 0x80;

  /* use CHS if LBA support is not enabled by kernel configuration */
  if (!InitKernelConfig.GlobalEnableLBAsupport)
  {
    if (firstPass && (InitKernelConfig.Verbose >= 1)) printf("LBA support disabled.\n");
    goto StandardBios;
  }
  /* check for LBA support */
  CPU_BX = 0x55aa;
  CPU_AH = 0x41;
  CPU_DL = drive;
  SET_DS ( 0x40 );
  /* ds = 40h is to work around a Xi8088 ROM-BIOS bug,
      refer to https://github.com/FDOS/kernel/issues/156
      and https://www.bttr-software.de/forum/forum_entry.php?id=21275 */
  cf = 1;  /* ensure carry is set to force error if unsupported */

  bios_13h(cpu);

  if ((cf) || CPU_BX != 0xaa55 || !(CPU_CX & 0x01))
  {
    /* error conditions:
        carry set or BX != 0xaa55 => no EDD spec compatible BIOS (LBA extensions not supported)
        CX bit 1 is set if BIOS supports fixed disk subset (Disk Address Packet [DAP] subset),
        or clear if fixed disk access subset not supported by LBA extensions
    */
    goto StandardBios;
  }

  /* version 1.0, 2.0 have different verify */
  if (CPU_AH < 0x21)
    LBA_WRITE_VERIFY = 0x4301;  /* may be problematic if INT13 is hooked by
                                   different controllers / drivers */

  // put it on x86 stack RAM, do not move SP, since it is temporary
  dos_far_ptr lba_bios_parameters = MK_FP(CPU_SS, CPU_SP - sizeof(struct _bios_LBA_disk_parameterS));
  struct _bios_LBA_disk_parameterS* plba_bios_parameters = (struct _bios_LBA_disk_parameterS*)ARM_PTR(lba_bios_parameters);
  /* query disk size and DMA handling, geometry is queried later by INT13,08 */
  memset(plba_bios_parameters, 0, sizeof(struct _bios_LBA_disk_parameterS));
  plba_bios_parameters->size = sizeof(struct _bios_LBA_disk_parameterS);

  CPU_SI = FP_OFF(lba_bios_parameters);
  SET_DS (FP_SEG(lba_bios_parameters));
  CPU_AH = 0x48;
  CPU_DL = drive;
  bios_13h(cpu);

  if (cf)
  {
    /* carry flag set indicates failed LBA disk parameter query */
    goto StandardBios;
  }

  if (plba_bios_parameters->heads > 0xffff ||
      plba_bios_parameters->sectors > 0xffff ||
      (plba_bios_parameters->totalSect == 0 &&
       plba_bios_parameters->totalSectHigh == 0))
  {
    if (firstPass) 
    {
      printf("Suspicious LBA disk parameters, reverting to CHS access:\n");
      printf("  drive %02x, heads=%lu, sectors=%lu, total=0x%lx-%08lx\n",
           drive,
           (ULONG) plba_bios_parameters->heads,
           (ULONG) plba_bios_parameters->sectors,
           (ULONG) plba_bios_parameters->totalSect,
           (ULONG) plba_bios_parameters->totalSectHigh);
    }

    goto StandardBios;
  }

  /* restrict disk size to 2TB, because we can not handle more */
  if (plba_bios_parameters->totalSectHigh == 0)
  {
    driveParam->total_sectors = plba_bios_parameters->totalSect;
  }
  else
  {
    if (firstPass) printf("Drive %02x is too large to handle, restricted to 2TB\n", drive);
    driveParam->total_sectors = 0xffffffffUL;
  }

  /* if we arrive here, mark drive as LBA capable */
  driveParam->descflags = DF_LBA;
  if (plba_bios_parameters->information & 8)
    driveParam->descflags |= DF_WRTVERIFY;

  if (plba_bios_parameters->information & 1)
  {
    /* DMA boundary errors are handled transparently */
    driveParam->descflags |= DF_DMA_TRANSPARENT;
  }
  
StandardBios:   /* get disk geometry, and if LBA is not enabled, also size */
  if (firstPass && (InitKernelConfig.Verbose >= 1))
    printf("Retrieving CHS values for drive\n");

  CPU_AH = 0x08;
  CPU_DL = drive;

  bios_13h(cpu);

  if (cf) 
  {
    goto ErrorReturn;
  }

  /* int13h call returns max value, store as count (#) i.e. +1 for 0 based heads & cylinders */
  driveParam->chs.Head = (CPU_DX >> 8) + 1; /* DH = max head value = # of heads - 1 (0-255) */
  driveParam->chs.Sector = (CPU_CX & 0x3f); /* CL bits 0-5 = max sector value = # (sectors/track) - 1 (1-63) */
  /* max cylinder value = # cylinders - 1 (0-1023) = [high two bits]CL7:6=cyls9:8, [low byte]CH=cyls7:0 */
  driveParam->chs.Cylinder = (CPU_CX >> 8) | ((CPU_CX & 0xc0) << 2) + 1; 
  
  if (driveParam->chs.Sector == 0) {
    /* happens e.g. with Bochs 1.x if no harddisk defined */
    driveParam->chs.Sector = 63; /* avoid division by zero...! */
    if (firstPass && (InitKernelConfig.Verbose >= 0)) 
      printf("BIOS reported 0 sectors/track, assuming 63!\n");
  }

  if (!(driveParam->descflags & DF_LBA))
  {
    driveParam->total_sectors =
        (ULONG)driveParam->chs.Cylinder
        * driveParam->chs.Head * driveParam->chs.Sector;
  }

  driveParam->driveno = drive;

  DebugPrintf(("drive %02Xh total: C = %u, H = %u, S = %u,",
               drive,
               driveParam->chs.Cylinder,
               driveParam->chs.Head, driveParam->chs.Sector));
  DebugPrintf((" total size %luMB\n\n", driveParam->total_sectors / 2048));

  return driveParam->driveno;


ErrorReturn:
  /* to avoid division by zero later, use some sane defaults */
  driveParam->total_sectors = 0;
  driveParam->chs.Head = 16;
  driveParam->chs.Sector = 63;
  return 0;
}

#define LBA_to_CHS   init_LBA_to_CHS

/*
    translate LBA sectors into CHS addressing
    initially copied and pasted from dsk.c!

    LBA to/from CHS conversion - see http://www.ata-atapi.com/ How It Works section on CHSxlat - CHS Translation
    LBA (logical block address) simple 0 to N-1 used internally and with extended int 13h (BIOS)
    L-CHS (logical CHS) is the CHS view when using int 13h (BIOS)
    P-CHS (physical CHS) is the CHS view when directly accessing disk, should not, but could be used in BS or MBR

    LBA = ( (cylinder * heads_per_cylinder + heads ) * sectors_per_track ) + sector - 1

    cylinder = LBA / (heads_per_cylinder * sectors_per_track)
        temp = LBA % (heads_per_cylinder * sectors_per_track)
        head = temp / sectors_per_track
      sector = temp % sectors_per_track + 1

    where heads_per_cylinder and sectors_per_track are the current translation mode values.
    cyclinder and heads are 0 to N-1 based, sector is 1 to N based
*/

void init_LBA_to_CHS(struct CHS *chs, ULONG LBA_address,
                     struct DriveParamS *driveparam)
{
  unsigned hs = driveparam->chs.Sector * driveparam->chs.Head;
  unsigned hsrem = (unsigned)(LBA_address % hs);
  
  LBA_address /= hs;

  chs->Cylinder = LBA_address >= 0x10000ul ? 0xffffu : (unsigned)LBA_address;
  chs->Head = hsrem / driveparam->chs.Sector;
  chs->Sector = hsrem % driveparam->chs.Sector + 1;
}

int Read1LBASector(struct DriveParamS *driveParam, unsigned drive,
                   ULONG LBA_address, dos_far_ptr buffer)
{
  struct _bios_LBA_address_packet* pdap = (struct _bios_LBA_address_packet*)ARM_PTR(x86_dap);
  pdap->packet_size = sizeof(struct _bios_LBA_address_packet);

  struct CHS chs;
  int num_retries;

/* disabled because this should not happen and if it happens the BIOS
   should complain; also there are weird disks around with
   CMOS geometry < real geometry */
#if 0
  if (LBA_address >= driveParam->total_sectors)
  {
    printf("LBA-Transfer error : address overflow = %lu, > %lu total sectors\n",
           LBA_address, driveParam->total_sectors);
    return 1;
  }
#endif

  for (num_retries = 0; num_retries < N_RETRY; num_retries++)
  {
    if (InitKernelConfig.Verbose >= 1)
    {
        printf("retry# %i sector %lu\n", num_retries, LBA_address);
    }

    CPU_DL = drive | 0x80;
    LBA_to_CHS(&chs, LBA_address, driveParam);
    /* Some old "security" software (PROT) traps int13 and assumes non
       LBA accesses. This statement causes partition tables to be read
       using CHS methods even if LBA is available unless CHS can't reach
       them. This can be overridden using kernel config parameters and
       the extended LBA partition type indicator.
    */
    if ((driveParam->descflags & DF_LBA) &&
        (InitKernelConfig.ForceLBA || ExtLBAForce || (chs.Cylinder > 1023)))
    {
      if (InitKernelConfig.Verbose >= 1) printf("LBA mode\n");
      pdap->number_of_blocks = 1;
      pdap->buffer_address = buffer;
      pdap->block_address_high = 0;       /* clear high part */
      pdap->block_address = LBA_address;  /* clear high part */

      /* Load the registers and call the interrupt. */
      CPU_AX = LBA_READ;
      CPU_SI = FP_OFF(x86_dap);
      SET_DS ( FP_SEG(x86_dap));
    }
    else
    {                           /* transfer data, using old bios functions */
      if (InitKernelConfig.Verbose >= 1) printf("CHS mode\n");
      /* avoid overflow at end of track */

      if (chs.Cylinder > 1023)
      {
        printf("LBA-Transfer error : address = %lu, cylinder %u > 1023\n", LBA_address, chs.Cylinder);
        return 1;
      }

      CPU_AX = 0x0201;
      CPU_BX = FP_OFF(buffer);
      CPU_CX =
          ((chs.Cylinder & 0xff) << 8) + ((chs.Cylinder & 0x300) >> 2) +
          chs.Sector;
      CPU_DH = chs.Head;
      SET_ES ( FP_SEG(buffer));
    }                           /* end of retries */
    bios_13h(cpu);
    if (cf == 0)
      break;
    BIOS_drive_reset(driveParam->driveno);
  }

  return cf;
}

/*
    converts physical into logical representation of partition entry
*/

STATIC void ConvCHSToIntern(struct CHS *chs, UBYTE * pDisk)
{
  chs->Head = pDisk[0];
  chs->Sector = pDisk[1] & 0x3f;
  chs->Cylinder = pDisk[2] + ((pDisk[1] & 0xc0) << 2);
}

BOOL ConvPartTableEntryToIntern(struct PartTableEntry * pEntry,
                                UBYTE * pDisk)
{
  int i;

  if (pDisk[0x1fe] != 0x55 || pDisk[0x1ff] != 0xaa)
  {
    memset(pEntry, 0, 4 * sizeof(struct PartTableEntry));

    return FALSE;
  }

  pDisk += 0x1be;

  for (i = 0; i < 4; i++, pDisk += 16, pEntry++)
  {

    pEntry->Bootable = pDisk[0];
    pEntry->FileSystem = pDisk[4];

    ConvCHSToIntern(&pEntry->Begin, pDisk+1);
    ConvCHSToIntern(&pEntry->End, pDisk+5);

    pEntry->RelSect = *(ULONG *) (pDisk + 8);
    pEntry->NumSect = *(ULONG *) (pDisk + 12);
  }
  return TRUE;
}

BOOL is_suspect(struct CHS *chs, struct CHS *pEntry_chs)
{
  /* Valid entry:
     entry == chs ||           // partition entry equal to computed values
     (chs->Cylinder > 1023 &&  // or LBA partition
      (entry->Cylinder == 1023 ||
       entry->Cylinder == (0x3FF & chs->Cylinder)))
  */
  return !((pEntry_chs->Cylinder == chs->Cylinder &&
            pEntry_chs->Head     == chs->Head     &&
            pEntry_chs->Sector   == chs->Sector)        ||
           chs->Cylinder > 1023u &&
           (pEntry_chs->Cylinder == 1023 ||
            pEntry_chs->Cylinder == (0x3ff & chs->Cylinder)));
}

void printCHS(char *title, struct CHS *chs)
{
  /* has no fixed size for head/sect: is often 1/1 in our context */
  if (InitKernelConfig.Verbose >= 0) printf("%s%4u-%u-%u", title, chs->Cylinder, chs->Head, chs->Sector);
}

/*
    reason for this modules existence:
    
    we have found a partition, and add them to the global 
    partition structure.

*/

/* Compute ceil(a/b) */
#define cdiv(a, b) (((a) + (b) - 1) / (b))

/* calculates FAT data:
   code adapted by Bart Oldeman from mkdosfs from the Linux dosfstools:
      Author:       Dave Hudson
      Updated by:   Roman Hodek
      Portions copyright 1992, 1993 Remy Card
      and 1991 Linus Torvalds
*/
/* defaults: */
#define MAXCLUSTSIZE 128
#define NSECTORFAT12 8
#define NFAT 2

VOID CalculateFATData(ddt * pddt, ULONG NumSectors, UBYTE FileSystem)
{
  ULONG fatdata;

  bpb *defbpb = &pddt->ddt_defbpb;

  /* FAT related items */
  defbpb->bpb_nfat = NFAT;
  /* normal value of number of entries in root dir */
  defbpb->bpb_ndirent = 512;
  defbpb->bpb_nreserved = 1;
  /* SEC_SIZE * DIRENT_SIZE / defbpb->bpb_ndirent + defbpb->bpb_nreserved */
  fatdata = NumSectors - (DIRENT_SIZE + 1);
  if (FileSystem == FAT12 || FileSystem == FAT12_LBA)
  {
    unsigned fatdat;
    /* in DOS, FAT12 defaults to 4096kb (8 sector) - clusters. */
    defbpb->bpb_nsector = NSECTORFAT12;
    /* Force maximal fatdata=32696 sectors since with our only possible sector
       size (512 bytes) this is the maximum for 4k clusters.
       #clus*secperclus+#fats*fatlength= 4077 * 8 + 2 * 12 = 32640.
       max FAT12 size for FreeDOS = 16,728,064 bytes */
    fatdat = (unsigned)fatdata;
    if (fatdata > 32640)
      fatdat = 32640;
    /* The "+2*NSECTORFAT12" is for the reserved first two FAT entries */
    defbpb->bpb_nfsect = (UWORD)cdiv((fatdat + 2 * NSECTORFAT12) * 3UL,
                                     FLOPPY_SEC_SIZE * 2 * NSECTORFAT12 + NFAT*3);
#ifdef DEBUG
    /* Need to calculate number of clusters, since the unused parts of the
     * FATS and data area together could make up space for an additional,
     * not really present cluster.
     * (This is really done in fatfs.c, bpbtodpb) */
    {
      unsigned clust = (fatdat - 2 * defbpb->bpb_nfsect) / NSECTORFAT12;
      unsigned maxclust = (defbpb->bpb_nfsect * 2 * FLOPPY_SEC_SIZE) / 3;
      if (maxclust > FAT12MAX)
        maxclust = FAT12MAX;
      printf("FAT12: #clu=%u, fatlength=%u, maxclu=%u, limit=%u\n",
             clust, defbpb->bpb_nfsect, maxclust, FAT12MAX);
      if (clust > maxclust - 2)
      {
        clust = maxclust - 2;
        printf("FAT12: too many clusters: setting to maxclu-2\n");
      }
    }
#endif
    memcpy(pddt->ddt_fstype, MSDOS_FAT12_SIGN, 8);
  }
  else
  { /* FAT16/FAT32 */
    CLUSTER fatlength, maxcl;
    unsigned long clust, maxclust, rest;
    unsigned fatentpersec;
    unsigned divisor;

#ifdef WITHFAT32
    if (FileSystem == FAT32 || FileSystem == FAT32_LBA)
    {
      /* For FAT32, use the cluster size table described in the FAT spec:
       * http://www.microsoft.com/hwdev/download/hardware/fatgen103.pdf
       */
      unsigned sz_gb = (unsigned)(NumSectors / 2097152UL);
      unsigned char nsector = 64; /* disks greater than 32 GB, 32K cluster */
      if (sz_gb <= 32)            /* disks up to 32 GB, 16K cluster */
        nsector = 32;
      if (sz_gb <= 16)            /* disks up to 16 GB, 8K cluster */
        nsector = 16;
      if (sz_gb <= 8)             /* disks up to 8 GB, 4K cluster */
        nsector = 8;
      if (NumSectors <= 532480UL)   /* disks up to 260 MB, 0.5K cluster */
        nsector = 1;
      defbpb->bpb_nsector = nsector;
      defbpb->bpb_ndirent = 0;
      defbpb->bpb_nreserved = 0x20;
      fatdata = NumSectors - 0x20;
      fatentpersec = FLOPPY_SEC_SIZE/4;  /* how many 32bit FAT values fit in a default 512 byte sector */
      maxcl = FAT32MAX;
    }
    else
#endif
    {
      /* FAT16: start at 4 sectors per cluster */
      defbpb->bpb_nsector = 4;
      /* Force maximal fatdata=8387584 sectors (NumSectors=8387617)
         since with our only possible sectorsize (512 bytes) this is the
         maximum we can address with 64k clusters
         #clus*secperclus+#fats*fatlength=65517 * 128 + 2 * 256=8386688.
         max FAT16 size for FreeDOS = 4,293,984,256 bytes = 4GiB-983,040 */
      if (fatdata > 8386688ul)
        fatdata = 8386688ul;
      fatentpersec = FLOPPY_SEC_SIZE/2; /* how many 16bit FAT values fit in a default 512 byte sector */
      maxcl = FAT16MAX;
    }

    DebugPrintf(("%lu sectors for FAT+data, starting with %u sectors/cluster\n", fatdata, defbpb->bpb_nsector));
    do
    {
      DebugPrintf(("Trying with %u sectors/cluster:\n", defbpb->bpb_nsector));
      divisor = fatentpersec * defbpb->bpb_nsector + NFAT; /* # of fat entries per cluster + 2 */
      rest = (unsigned)(fatdata % divisor);
      fatlength  = (CLUSTER)(fatdata / divisor);
      fatlength += (CLUSTER)((2 * defbpb->bpb_nsector + divisor + rest - 1) / divisor);

      /* Need to calculate number of clusters, since the unused parts of the
       * FATS and data area together could make up space for an additional,
       * not really present cluster. */
      clust = (fatdata - NFAT * fatlength) / defbpb->bpb_nsector;
      maxclust = fatlength * fatentpersec;
      if (maxclust > maxcl)
        maxclust = maxcl;
      DebugPrintf(("FAT: #clu=%lu, fatlen=%lu, maxclu=%lu, limit=%lu\n",
                   clust, (ULONG)fatlength, maxclust, (ULONG)maxcl));
      if (clust > maxclust - 2)
      {
        clust = 0;
        DebugPrintf(("FAT: too many clusters\n"));
      }
      else if (clust <= FAT_MAGIC)
      {
        /* The <= 4086 avoids that the filesystem will be misdetected as having a
         * 12 bit FAT. */
        DebugPrintf(("FAT: would be misdetected as FAT12\n"));
        clust = 0;
      }
      if (clust)
        break;
      defbpb->bpb_nsector <<= 1;
    }
    while (defbpb->bpb_nsector && defbpb->bpb_nsector <= MAXCLUSTSIZE);
#ifdef WITHFAT32
    if (FileSystem == FAT32 || FileSystem == FAT32_LBA)
    {
      defbpb->bpb_nfsect = 0;
      defbpb->bpb_xnfsect = fatlength;
      /* set up additional FAT32 fields */
      defbpb->bpb_xflags = 0;
      defbpb->bpb_xfsversion = 0;
      defbpb->bpb_xrootclst = 2;
      defbpb->bpb_xfsinfosec = 1;
      defbpb->bpb_xbackupsec = 6;
      memcpy(pddt->ddt_fstype, MSDOS_FAT32_SIGN, 8);
    }
    else
#endif
    {
      defbpb->bpb_nfsect = (UWORD)fatlength;
      memcpy(pddt->ddt_fstype, MSDOS_FAT16_SIGN, 8);
    }
  }
  pddt->ddt_fstype[8] = '\0';
}

void print_warning_suspect(char *partitionName, UBYTE fs, struct CHS *chs,
                           struct CHS *pEntry_chs)
{
  if (!InitKernelConfig.ForceLBA)
  {
    if (InitKernelConfig.Verbose >= 0) 
    {
      printf("WARNING: using suspect partition %s FS %02x:", partitionName, fs);
      printCHS(" with calculated values ", chs);
      printCHS(" instead of ", pEntry_chs);
      printf("\n");
    }
  }
  memcpy(pEntry_chs, chs, sizeof(struct CHS));
}

void DosDefinePartition(struct DriveParamS *driveParam,
                        ULONG StartSector, struct PartTableEntry *pEntry,
                        int extendedPartNo, int PrimaryNum)
{
  ddt nddt;
  ddt *pddt = &nddt;
  struct CHS chs;

  if (nUnits >= NDEV)
  {
    printf("more Partitions detected then possible, max = %d\n", NDEV);
    return;                     /* we are done */
  }

  pddt->ddt_next = MK_FP(0, 0xffff);
  pddt->ddt_driveno = driveParam->driveno;
  pddt->ddt_logdriveno = nUnits;
  pddt->ddt_descflags = driveParam->descflags;
  /* Turn off LBA if not forced and the partition is within 1023 cyls and of the right type */
  /* the FileSystem type was internally converted to LBA_xxxx if a non-LBA partition
     above cylinder 1023 was found */
  if (!(InitKernelConfig.ForceLBA || IsLBAPartition(pEntry->FileSystem) || ExtLBAForce))
    pddt->ddt_descflags &= ~DF_LBA;
  pddt->ddt_ncyl = driveParam->chs.Cylinder;

  DebugPrintf(("LBA %senabled for drive %c:\n", (pddt->ddt_descflags & DF_LBA)?"":"not ", 'A' + nUnits));

  pddt->ddt_offset = StartSector;

  pddt->ddt_defbpb.bpb_nbyte = FLOPPY_SEC_SIZE;
  pddt->ddt_defbpb.bpb_mdesc = 0xf8;
  pddt->ddt_defbpb.bpb_nheads = driveParam->chs.Head;
  pddt->ddt_defbpb.bpb_nsecs = driveParam->chs.Sector;
  pddt->ddt_defbpb.bpb_hidden = pEntry->RelSect;

  pddt->ddt_defbpb.bpb_nsize = 0;
  pddt->ddt_defbpb.bpb_huge = pEntry->NumSect;
  if (pEntry->NumSect <= 0xffff)
  {
    pddt->ddt_defbpb.bpb_nsize = (UWORD) (pEntry->NumSect);
    pddt->ddt_defbpb.bpb_huge = 0;  /* may still be set on Win95 */
  }

  /* sectors per cluster, sectors per FAT etc. */
  CalculateFATData(pddt, pEntry->NumSect, pEntry->FileSystem);

  pddt->ddt_serialno = 0x12345678l;
  /* drive inaccessible until bldbpb successful */
  pddt->ddt_descflags |= init_readdasd(pddt->ddt_driveno) | DF_NOACCESS;
  pddt->ddt_type = 5;
  memcpy(&pddt->ddt_bpb, &pddt->ddt_defbpb, sizeof(bpb));

  push_ddt(pddt);

  /* Alain whishes to keep this in later versions, too 
     Tom likes this too, so he made it configurable by SYS CONFIG ...
   */

  if (InitKernelConfig.InitDiskShowDriveAssignment)
  {
    char *ExtPri;
    int num;

    LBA_to_CHS(&chs, StartSector, driveParam);

    ExtPri = "Pri";
    num = PrimaryNum + 1;
    if (extendedPartNo)
    {
      ExtPri = "Ext";
      num = extendedPartNo;
    }
    printf("%c: HD%d, %s[%2d]", 'A' + nUnits,
           (driveParam->driveno & 0x7f) + 1, ExtPri, num);

    printCHS(", CHS= ", &chs);

    printf(", start=%6lu MB, size=%6lu MB\n",
           StartSector / 2048, pEntry->NumSect / 2048);
  }

  nUnits++;
}

BOOL ScanForPrimaryPartitions(struct DriveParamS * driveParam, int scan_type,
                         struct PartTableEntry * pEntry, ULONG startSector,
                         int partitionsToIgnore, int extendedPartNo)
{
  int i;
  struct CHS chs, end;
  ULONG partitionStart;
  char partitionName[12];

  for (i = 0; i < 4; i++, pEntry++)
  {
    if (pEntry->FileSystem == 0)
      continue;

    if (partitionsToIgnore & (1 << i))
      continue;

    if (IsExtPartition(pEntry->FileSystem))
      continue;

    if (scan_type == SCAN_PRIMARYBOOT && !pEntry->Bootable)
      continue;

    partitionStart = startSector + pEntry->RelSect;

    if (!IsFATPartition(pEntry->FileSystem))
    {
      continue;
    }

    if (extendedPartNo)
      sprintf(partitionName, "Ext:%d", extendedPartNo);
    else
      sprintf(partitionName, "Pri:%d", i + 1);

    /*
       some sanity checks, that partition
       structure is OK
     */
    LBA_to_CHS(&chs, partitionStart, driveParam);
    LBA_to_CHS(&end, partitionStart + pEntry->NumSect - 1, driveParam);

    /* some FDISKs enter for partitions 
       > 8 GB cyl = 1023, other (cyl&1023)
     */

    if (is_suspect(&chs, &pEntry->Begin))
    {
      print_warning_suspect(partitionName, pEntry->FileSystem, &chs,
                            &pEntry->Begin);
    }

    if (is_suspect(&end, &pEntry->End))
    {
      if (pEntry->NumSect == 0)
      {
        printf("Not using partition %s with 0 sectors\n", partitionName);
        continue;
      }
      print_warning_suspect(partitionName, pEntry->FileSystem, &end,
                            &pEntry->End);
    }

    if (chs.Cylinder > 1023 || end.Cylinder > 1023)
    {

      /* if partition exceeds bounds of CHS addressing but LBA is not supported then skip partition */
      if (!(driveParam->descflags & DF_LBA))
      {
        printf
            ("can't use LBA partition without LBA support - part %s FS %02x",
             partitionName, pEntry->FileSystem);

        printCHS(" start ", &chs);
        printCHS(", end ", &end);
        printf("\n");

        continue;
      }

      /* if partition exceeds bounds of CHS addressing and we can use LBA 
	     but partition type indicates to use CHS then print warning 
         and force internal filesystem indicator to enable LBA
      */
      if (!(InitKernelConfig.ForceLBA || IsLBAPartition(pEntry->FileSystem) || ExtLBAForce))
      {
        printf
            ("WARNING: Partition ID does not suggest LBA - part %s FS %02x.\n"
             "Please run FDISK to correct this - using LBA to access partition.\n",
             partitionName, pEntry->FileSystem);

        printCHS(" start ", &chs);
        printCHS(", end ", &end);
        printf("\n");
        pEntry->FileSystem = (pEntry->FileSystem == FAT12 ? FAT12_LBA :
                              pEntry->FileSystem == FAT32 ? FAT32_LBA :
                              /*  pEntry->FileSystem == FAT16 ? */
                              FAT16_LBA);
      }

      /* else its a diagnostic message only */
#ifdef DEBUG
      printf("found and using LBA partition %s FS %02x",
             partitionName, pEntry->FileSystem);
      printCHS(" start ", &chs);
      printCHS(", end ", &end);
      printf("\n");
#endif
    }

    /*
       here we have a partition table in our hand !!
     */

    partitionsToIgnore |= 1 << i;

    DosDefinePartition(driveParam, partitionStart, pEntry,
                       extendedPartNo, i);

    if (scan_type == SCAN_PRIMARYBOOT || scan_type == SCAN_PRIMARY)
    {
      return partitionsToIgnore;
    }
  }

  return partitionsToIgnore;
}

/* Load the Partition Tables and get information on all drives */
int ProcessDisk(int scanType, unsigned drive, int PartitionsToIgnore)
{
  /* note: error messages are only printed on first call, where (scanType==SCAN_PRIMARYBOOT) */

  struct PartTableEntry PTable[4];
  ULONG RelSectorOffset;
  ULONG ExtendedPartitionOffset;
  int iPart;
  int strangeHardwareLoop;

  int num_extended_found = 0;

  struct DriveParamS driveParam;

  /* Get the hard drive parameters and ensure that the drive exists. */
  /* If there was an error accessing the drive, skip that drive. */

  if (!LBA_Get_Drive_Parameters(drive, &driveParam,(scanType==SCAN_PRIMARYBOOT)))
  {
    printf("can't get drive parameters for drive %02x\n", drive);
    return PartitionsToIgnore;
  }

  RelSectorOffset = 0;          /* boot sector */
  ExtendedPartitionOffset = 0;  /* not found yet */
  ExtLBAForce = 0;      /* initially we are not dealing with partitions
                           within a type 0x0E LBA extended partition,
                           so we do not enforce LBA access by now  */

    /* Read the Primary Partition Table. */
ReadNextPartitionTable:
    strangeHardwareLoop = 0;
    strange_restart:

  if (Read1LBASector(&driveParam, drive, RelSectorOffset, InitDiskTransferBuffer))
  {
    printf("Error reading partition table drive %02Xh sector %lu", drive,
           RelSectorOffset);
    return PartitionsToIgnore;
  }

  if (!ConvPartTableEntryToIntern(PTable, ARM_PTR(InitDiskTransferBuffer)))
  {
    /* there is some strange hardware out in the world,
       which returns OK on first read, but the data are
       rubbish. simply retrying works fine.
       there is no logic behind this, but it works TE */

    if (++strangeHardwareLoop < 3)
      goto strange_restart;

    if (scanType==SCAN_PRIMARYBOOT) printf("illegal partition table - drive %02x sector %lu\n", drive,
           RelSectorOffset);
    return PartitionsToIgnore;
  }

  if (scanType == SCAN_PRIMARYBOOT ||
      scanType == SCAN_PRIMARY ||
      scanType == SCAN_PRIMARY2 || num_extended_found != 0)
  {

    PartitionsToIgnore = ScanForPrimaryPartitions(&driveParam, scanType,
                                                  PTable, RelSectorOffset,
                                                  PartitionsToIgnore,
                                                  num_extended_found);
  }

  if (scanType != SCAN_EXTENDED)
  {
    return PartitionsToIgnore;
  }

  /* scan for extended partitions now */
  PartitionsToIgnore = 0;

  for (iPart = 0; iPart < 4; iPart++)
  {
    if (IsExtPartition(PTable[iPart].FileSystem))
    {
      RelSectorOffset = ExtendedPartitionOffset + PTable[iPart].RelSect;

      if (ExtendedPartitionOffset == 0) /* first extended in chain? */
      {
        ExtendedPartitionOffset = PTable[iPart].RelSect;
        /* grand parent LBA -> all children and grandchildren LBA */
        ExtLBAForce = (PTable[iPart].FileSystem == EXTENDED_LBA);
      }

      num_extended_found++;

      if (num_extended_found > 30)
      {
        printf("found more then 30 extended partitions, terminated\n");
        return 0;
      }

      goto ReadNextPartitionTable;
    }
  }

  return PartitionsToIgnore;
}

void ReadAllPartitionTables(void)
{
    UBYTE foundPartitions[MAX_HARD_DRIVE];
    int HardDrive;
    int nHardDisk;
    ddt nddt;
    /* Setup media info and BPBs arrays for floppies */
    make_ddt(&nddt, 0, 0, 0);

    /*
     this is a quick patch - see if B: exists
     test for A: also, need not exist
    */
    bios_11h(cpu);  /* get equipment list */
    /*if ((regs.AL & 1)==0)*//* no floppy drives installed  */
    if ((CPU_AL & 1) && (CPU_AL & 0xc0))
    {
        /* floppy drives installed and a B: drive */
        make_ddt(&nddt, 1, 1, 0);
    }
    else
    {
        /* set up the DJ method : multiple logical drives */
        make_ddt(&nddt, 1, 0, DF_MULTLOG);
    }

    /* Initial number of disk units                                 */
    nUnits = 2;

    nHardDisk = BIOS_nrdrives();
    if (nHardDisk > LENGTH(foundPartitions))
        nHardDisk = LENGTH(foundPartitions);

    DebugPrintf(("DSK init: found %d disk drives\n", nHardDisk));

    /* Reset the drives                                             */
    for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
    {
        BIOS_drive_reset(HardDrive);
        foundPartitions[HardDrive] = 0;
    }

    if (InitKernelConfig.DLASortByDriveNo == 0)
    {
        if (InitKernelConfig.Verbose >= 1) printf("Drive Letter Assignment - DOS order\n");

        /* Process primary partition table   1 partition only      */
        for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
        {
            foundPartitions[HardDrive] = ProcessDisk(SCAN_PRIMARYBOOT, HardDrive, 0);

            if (foundPartitions[HardDrive] == 0)
                foundPartitions[HardDrive] = ProcessDisk(SCAN_PRIMARY, HardDrive, 0);
        }

        /* Process extended partition table                      */
        for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
        {
            ProcessDisk(SCAN_EXTENDED, HardDrive, 0);
        }

    /* Process primary a 2nd time */
    for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
    {
      ProcessDisk(SCAN_PRIMARY2, HardDrive, foundPartitions[HardDrive]);
    }
  }
  else
  {
    UBYTE bootdrv = peekb(0,0x5e0);

    if (InitKernelConfig.Verbose >= 1) printf("Drive Letter Assignment - sorted by drive\n");

    /* Process primary partition table   1 partition only      */
    for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
    {
      struct DriveParamS driveParam;
      if (LBA_Get_Drive_Parameters(HardDrive, &driveParam, 0) &&
          driveParam.driveno == bootdrv)
      {
        foundPartitions[HardDrive] =
          ProcessDisk(SCAN_PRIMARYBOOT, HardDrive, 0);
        break;
      }
    }

    for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
    {
      if (foundPartitions[HardDrive] == 0)
      {
        foundPartitions[HardDrive] =
          ProcessDisk(SCAN_PRIMARYBOOT, HardDrive, 0);

        if (foundPartitions[HardDrive] == 0)
          foundPartitions[HardDrive] =
            ProcessDisk(SCAN_PRIMARY, HardDrive, 0);
      }

      /* Process extended partition table                      */
      ProcessDisk(SCAN_EXTENDED, HardDrive, 0);

      /* Process primary a 2nd time */
      ProcessDisk(SCAN_PRIMARY2, HardDrive, foundPartitions[HardDrive]);
    }
  }
  
  if (InitKernelConfig.Verbose >= 0)
  {
    unsigned foundPartitionsCount = 0;
    /* Tell user if no valid partitions found on any hard drive     */
    for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
    {
      foundPartitionsCount += foundPartitions[HardDrive];
    }
    /* printf("Found %i partitions\n", foundPartitionsCount); */
    if (!foundPartitionsCount) printf("No supported partitions found.\n");
  }
}

/* disk initialization: returns number of units */
COUNT dsk_init()
{
  if (InitKernelConfig.Verbose >= 1) printf("\nInitDisk\n");

  /* Reset the drives                                             */
  BIOS_drive_reset(0);

  ReadAllPartitionTables();

  return nUnits;
}

BYTE HMAState BSS_INIT(0);
UWORD HMAFree BSS_INIT(0);            /* first byte in HMA not yet used      */
BYTE DosLoadedInHMA BSS_INIT(FALSE);  /* set to TRUE if loaded HIGH          */

#define HMA_NONE 0              /* do nothing */
#define HMA_REQ 1               /* DOS = HIGH detected */
#define HMA_DONE 2              /* Moved kernel to HMA */
#define HMA_LOW 3               /* Definitely LOW */

/*
    this allocates some bytes from the HMA area
    only available if DOS=HIGH was successful
*/
dos_far_ptr HMAalloc(COUNT bytesToAllocate)
{
  if (!DosLoadedInHMA)
    return MK_FP(0, 0);

  if (HMAFree > 0xfff0 - bytesToAllocate)
    return MK_FP(0, 0);

  dos_far_ptr HMAptr = MK_FP(0xffff, HMAFree);

  /* align on 16 byte boundary */
  HMAFree = (HMAFree + bytesToAllocate + 0xf) & 0xfff0;

  /*printf("HMA allocated %d byte at %x\n", bytesToAllocate, HMAptr); */

  fmemset(HMAptr, 0, bytesToAllocate);

  return HMAptr;
}

STATIC void config_init_buffers(int wantedbuffers)
{
  unsigned buffers = 0;

  /* fill HMA with buffers if BUFFERS count >=0 and DOS in HMA        */
  if (wantedbuffers < 0)
    wantedbuffers = -wantedbuffers;
  else if (HMAState == HMA_DONE)
    buffers = (0xfff0 - HMAFree) / sizeof(struct buffer);

  if (wantedbuffers < 6)         /* min 6 buffers                     */
    wantedbuffers = 6;
  if (wantedbuffers > 99)        /* max 99 buffers                    */
  {
    printf("BUFFERS=%u not supported, reducing to 99\n", wantedbuffers);
    wantedbuffers = 99;
  }
  if (wantedbuffers > buffers)   /* more specified than available -> get em */
    buffers = wantedbuffers;

  LoL->nbuffers = buffers;
  LoL->inforecptr = LoL->firstbuf;
  struct buffer *pbuffer;
  dos_far_ptr x86_buffer;
  {
    size_t bytes = sizeof(struct buffer) * buffers;
    x86_buffer = HMAalloc(bytes);

    if (!FP_SEG(x86_buffer) && !FP_OFF(x86_buffer))
    {
      x86_buffer = KernelAlloc(bytes, 'B', 0);
      if (HMAState == HMA_DONE)
        x86_firstAvailableBuf = MK_FP(0xffff, HMAFree);
    }
    else
    {
      LoL->bufloc = LOC_HMA;
      /* space in HMA beyond requested buffers available as user space */
      x86_firstAvailableBuf = MK_FP(FP_SEG(x86_buffer), FP_OFF(x86_buffer) + wantedbuffers);
    }
    pbuffer = (struct buffer*)ARM_PTR(x86_buffer);
  }
  LoL->deblock_buf = DiskTransferBuffer;
  LoL->firstbuf = x86_buffer;

  CfgDbgPrintf(("init_buffers (size %u) at", sizeof(struct buffer)));
  CfgDbgPrintf((" (%p)", LoL->firstbuf));

  buffers--;
  pbuffer->b_prev = FP_OFF(x86_buffer) + buffers * sizeof(struct buffer);
  {
    int i = buffers;
    do
    {
      pbuffer->b_next = FP_OFF(x86_buffer) + sizeof(struct buffer);
      pbuffer++;
      pbuffer->b_prev = FP_OFF(x86_buffer) - sizeof(struct buffer);
    }
    while (--i);
  }
  pbuffer->b_next = FP_OFF(x86_buffer) - buffers * sizeof(struct buffer);

    /* now, we can have quite some buffers in HMA
       -- up to 50 for KE38616.
       so we fill the HMA with buffers
       but not if the BUFFERS count is negative ;-)
     */

  CfgDbgPrintf((" done\n"));

  if (FP_SEG(x86_buffer) == 0xffff)
  {
    buffers++;
    if (InitKernelConfig.Verbose >= 0) 
    {
      printf("Kernel: allocated %d Diskbuffers = %u Bytes in HMA\n",
           buffers, buffers * sizeof(struct buffer));
    }
  }
}

/* Do first time initialization.  Store last so that we can reset it    */
/* later.                                                               */
void PreConfig(void)
{
  /* Initialize the base memory pointers                          */

  CfgDbgPrintf(("SDA located at 0x%p\n", internal_data));
  /* Begin by initializing our system buffers                     */
  /* DebugPrintf(("Preliminary %d buffers allocated at 0x%p\n", Config.cfgBuffers, buffers));*/
  LoL->sfthead = MK_FP(FP_SEG(x86_FIXED_DATA), 0xcc); /* &(LoL->firstsftt) */
  /* LoL->FCBp = (sfttbl FAR *)&FcbSft; */
  /* LoL->FCBp = (sfttbl FAR *)
     KernelAlloc(sizeof(sftheader)
     + Config.cfgFiles * sizeof(sft)); */

  config_init_buffers(Config.cfgBuffers);

  LoL->CDSp = KernelAlloc(sizeof(struct cds) * LoL->lastdrive, 'L', 0);

/*  CfgDbgPrintf((" FCB table 0x%p\n",LoL->FCBp));*/
  CfgDbgPrintf((" sft table 0x%p\n", LoL->sfthead));
  CfgDbgPrintf((" CDS table 0x%p\n", LoL->CDSp));
  CfgDbgPrintf((" DPB table 0x%p\n", LoL->DPBp));

  /* Done.  Now initialize the MCB structure                      */
  /* This next line is 8086 and 80x86 real mode specific          */
  CfgDbgPrintf(("Preliminary  allocation completed: top at %p\n", lpTop));
}

int init_setdrive(int drive) {
    CPU_AH = 0x0e;
    CPU_DX = drive;
    fdos_21h(cpu);
    return CPU_AL;          /* number of potentially valid drives */
}

int init_DosOpen(dos_far_ptr pathname, int flags) {
    SET_DS (FP_SEG(pathname));
    CPU_DX = FP_OFF(pathname);
    CPU_AL = flags & 0xff;
    CPU_AH = 0x3d;          /* DOS open */
    fdos_21h(cpu);
    return cf ? -1 : CPU_AX;          /* file handle */
}

int dup2(int oldfd, int newfd)
{
    CPU_AH = 0x46;      /* Force duplicate file handle */
    CPU_BX = oldfd;
    CPU_CX = newfd;
    fdos_21h(cpu);
    return cf ? -1 : CPU_AX;
}

STATIC VOID FsConfig(VOID)
{
  struct dpb* dpb = (struct dpb*)ARM_PTR(LoL->DPBp);
  int i;

  /* Initialize the current directory structures    */
  for (i = 0; i < LoL->lastdrive; i++)
  {
    struct cds* pcds_table = (struct cds*)ARM_PTR(LoL->CDSp) + i;

    memcpy(pcds_table->cdsCurrentPath, "A:\\\0", 4);

    pcds_table->cdsCurrentPath[0] += i;

    if (i < LoL->nblkdev && dpb != (struct dpb*)ARM_PTR(MK_FP(-1, -1)))
    {
      pcds_table->cdsDpb = dpb;
      pcds_table->cdsFlags = CDSPHYSDRV;
      dpb = (struct dpb*)ARM_PTR(dpb->dpb_next);
    }
    else
    {
      pcds_table->cdsFlags = 0;
    }
    pcds_table->cdsStrtClst = 0xffff;
    pcds_table->cdsParam = 0xffff;
    pcds_table->cdsStoreUData = 0xffff;
    pcds_table->cdsJoinOffset = 2;
  }

  /* Log-in the default drive. */
  init_setdrive(LoL->BootDrive - 1);

  /* The system file tables need special handling and are "hand   */
  /* built. Included is the stdin, stdout, stdaux and stdprn. */
  /* a little bit of shuffling is necessary for compatibility */

  /* sft_idx=0 is /dev/aux                                        */
  open(x86_FAR_PTR(DOS_PSP, LoL->aux_str), O_RDWR);

  /* handle 1, sft_idx=1 is /dev/con (stdout) */
  open(x86_FAR_PTR(DOS_PSP, LoL->con_str), O_RDWR);

  /* 3 is /dev/aux                */
  dup2(STDIN, STDAUX);

  /* 0 is /dev/con (stdin)        */
  dup2(STDOUT, STDIN);

  /* 2 is /dev/con (stdin)        */
  dup2(STDOUT, STDERR);

  /* 4 is /dev/prn                                                */
  open(x86_FAR_PTR(DOS_PSP, LoL->prn_str), O_WRONLY);

  /* Initialize the disk buffer management functions */
  /* init_call_init_buffers(); done from CONFIG.C   */
}

STATIC void init_kernel()
{
    COUNT i;

    LoL->os_setver_major = LoL->os_major = MAJOR_RELEASE;
    LoL->os_setver_minor = LoL->os_minor = MINOR_RELEASE;

    /* Init oem hook - returns memory size in KB, just read BDA */
    ram_top = pload16(0x413);

    /* no resident DOS in guest RAM, so top is just below video RAM */
    lpTop = MK_FP((ram_top << 6) - 1, 0);

    /* Initialize IO subsystem                                      */
    InitIO();

    // no printers and COM-ports for now
//  InitPrinters();
//  InitSerialPorts();

    init_PSPSet(DOS_PSP);
    set_DTA(MK_FP(DOS_PSP, 0x80));
    PSPInit();

    Init_clk_driver();

    /* Do first initialization of system variable buffers so that   */
    /* we can read config.sys later.  */

    /* use largest possible value for the initial CDS */
    LoL->lastdrive = 26;

    /*  init_device((struct dhdr FAR *)&blk_dev, NULL, 0, &ram_top); */
    /*  WARNING: dsk_init() must be called prior to update_dcb() to ensure
        _Dyn (start of Dynamic memory block) is the start of drive data table (see getddt() in dsk.c)
     */
    blk_dev->dh_name[0] = dsk_init();

    PreConfig();
    /* Number of units */
    if (blk_dev->dh_name[0] > 0)
        update_dcb(x86_blk_dev);

    /* Now config the temporary file system */
    FsConfig();

/// TODO:
  /* Now process CONFIG.SYS     * /
  DoConfig(0);
  DoConfig(1);
*/
  /* initialize near data and MCBs */
///  PreConfig2();
  /* and process CONFIG.SYS one last time for device drivers */
//  DoConfig(2);


  /* Close all (device) files * /
  for (i = 0; i < 20; i++)
    close(i);
*/
  /* and do final buffer allocation. * /
  PostConfig();
*/
  /* Init the file system one more time     */
///  FsConfig();
  
///  configDone();

///  InitializeAllBPBs();
}

void kernel(CPU* _cpu) {
    cpu = _cpu;
    con_dev = (struct dhdr*)ARM_PTR(x86_con_dev);
    memcpy(con_dev, &_con_dev, sizeof(struct dhdr));
    prn_dev = (struct dhdr*)ARM_PTR(x86_prn_dev);
    memcpy(prn_dev, &_prn_dev, sizeof(struct dhdr));
    aux_dev = (struct dhdr*)ARM_PTR(x86_aux_dev);
    memcpy(aux_dev, &_aux_dev, sizeof(struct dhdr));
    lpt1_dev = (struct dhdr*)ARM_PTR(x86_lpt1_dev);
    memcpy(lpt1_dev, &_lpt1_dev, sizeof(struct dhdr));
    lpt2_dev = (struct dhdr*)ARM_PTR(x86_lpt2_dev);
    memcpy(lpt2_dev, &_lpt2_dev, sizeof(struct dhdr));
    lpt3_dev = (struct dhdr*)ARM_PTR(x86_lpt3_dev);
    memcpy(lpt3_dev, &_lpt3_dev, sizeof(struct dhdr));
    com1_dev = (struct dhdr*)ARM_PTR(x86_com1_dev);
    memcpy(com1_dev, &_com1_dev, sizeof(struct dhdr));
    com2_dev = (struct dhdr*)ARM_PTR(x86_com2_dev);
    memcpy(com2_dev, &_com2_dev, sizeof(struct dhdr));
    com3_dev = (struct dhdr*)ARM_PTR(x86_com3_dev);
    memcpy(com3_dev, &_com3_dev, sizeof(struct dhdr));
    com4_dev = (struct dhdr*)ARM_PTR(x86_com4_dev);
    memcpy(com4_dev, &_com4_dev, sizeof(struct dhdr));
    clk_dev = (struct dhdr*)ARM_PTR(x86_clk_dev);
    memcpy(clk_dev, &_clk_dev, sizeof(struct dhdr));
    blk_dev = (struct dhdr*)ARM_PTR(x86_blk_dev);
    memcpy(blk_dev, &_blk_dev, sizeof(struct dhdr));

    LoL = (struct lol*)ARM_PTR(x86_FIXED_DATA);
    memcpy(LoL, &lol, sizeof(struct lol));

    internal_data = (struct dos_data*)ARM_PTR(x86_INTERNAL_DATA);
    memset(internal_data, 0, sizeof(struct dos_data));
    internal_data->switchar = '/';
    internal_data->net_set_count = 1;
    internal_data->CritPatchPad = 0x90; // NOP pad byte
    internal_data->break_ena = 1;
    internal_data->DayOfMonth = 1;
    internal_data->Month = 1;
    internal_data->daysSince1980 = 0xFFFF;
    internal_data->DayOfWeek = 2;
    internal_data->dosidle_flag = 1;
    internal_data->last_component = 0xffff; // 296 - 0xffff or offset of last component in filename
    memset(
        internal_data->sda_tmp_dm_ren,
        0x90,
        sizeof(internal_data->sda_tmp_dm_ren) + sizeof(internal_data->SearchDir_ren) + sizeof(internal_data->api_stacks) +
        sizeof(internal_data->error_tos) + sizeof(internal_data->disk_api_tos) + sizeof(internal_data->char_api_tos)
    );

    // adjust boot drive to DOS format
    LoL->BootDrive  = CPU_BL + 1;
    if (LoL->BootDrive > 0x80) {
        LoL->BootDrive = 3;
    }

    LoL->cpu = cpu->gen;

    /* install DOS API and other interrupt service routines, basic kernel functionality works */
//    setup_int_vectors();
    HaltCpuWhileIdle = 0;
    // TODO: INT 30h как far jump на CP/M entry
// CheckContinueBootFromHarddisk
    /* display copyright info and kernel emulation status */
//    signon();
    if (InitKernelConfig.Verbose) {
        dos_puts(KERNEL_VERSION_STRING "\n");
    } else {
        dos_puts(KERNEL_VERSION_SHORT_STRING "\n");
    }

    /* initialize all internal variables, process CONFIG.SYS, load drivers, etc */
    init_kernel();

    /// TODO: next point to complete impl.

    /// debug-blink this point acived
    for (int i = 0; i < 6; i++) {
    /// TODO: for reboot    keyboard_tick();
        sleep_ms(23);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(23);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
    // stop it before complete impl.
    while(1);
}
