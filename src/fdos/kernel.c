#include <pico.h>
#include <pico/time.h>
#include <hardware/pio.h>
#include <ctype.h>
#include "286/cpu.h"
#include "bios/bios.h"
#include "fdos.h"
#include "i8254.h"

#define printf(...) dos_printf(__VA_ARGS__)

static bool waiter(CPU* cpu, bios_callback_params_t* any) {
    // actually do nothing, since reboot only is allowed in this case
    ifl = 1; // allow IRQ
    return false; // in a loop on the same CS:IP, no IRET required there
}

static bios_callback_params_t params = {
    .callback = waiter,
    .expected_cs = 0xF000,
    .expected_ip = 0xFEFF
};

static CPU* cpu;

int	vsnprintf (char *__restrict, size_t, const char *__restrict, __gnuc_va_list)
               _ATTRIBUTE ((__format__ (__printf__, 3, 0)));

void dos_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bios_puts(cpu, buf);
}

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
#include "hdr/network.h"
#include "init-mod.h"
#include "dyndata.h"

#define x86_para2far(seg) (MK_FP((seg), 0))
#define para2far(seg) ((mcb*)ARM_PTR(MK_FP((seg), 0)))

BYTE HaltCpuWhileIdle = 0;
UWORD ram_top = 0;
COUNT UmbState BSS_INIT(0);
STATIC seg base_seg BSS_INIT(0);
STATIC seg umb_base_seg BSS_INIT(0);
UWORD umb_start BSS_INIT(0), UMB_top BSS_INIT(0);
dos_far_ptr lpTop;
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

/*UBYTE DiskTransferBuffer[MAX_SEC_SIZE]*/ const dos_far_ptr DiskTransferBuffer = x86_BSS; // BSS
/*256*/const dos_far_ptr x86_szLine = MK_FP(DOS_PSP, 0x19F4 + MAX_SEC_SIZE); // _BSS + MAX_SEC_SIZE
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

const KernelConfig InitKernelConfig = {
    .CONFIG = {'C','O','N','F','I','G'},
    .ConfigSize = sizeof(KernelConfig) - 8, // без signature[6] и config_size
    .DLASortByDriveNo = 0,
    .InitDiskShowDriveAssignment = 1,
    .SkipConfigSeconds = -1, /// TODO: was 2
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
     * C_INPUT/C_OUTPUT/C_OUTVFY are serviced by blockio() (migrated from
     * dsk.c), which drives bios_13h() directly through LBA_Transfer().
     */
    switch (rq->r_command) {
    case C_INPUT:
    case C_OUTPUT:
    case C_OUTVFY:
        blockio(cpu, rq);
        break;
    case C_INIT:
        /* disk init is done, so this should never be called */
    default:
        /// TODO: C_MEDIACHK / C_BUILDBPB / C_IOCTLIN / C_IOCTLOUT / C_GENIOCTL
        /// are not implemented yet - not required for DosOpen() on a fixed,
        /// never-removed disk image.
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
    .firstsftt.sftt_next  = MK_FP(-1, -1),
    .firstsftt.sftt_count = 5,
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
    dpb->dpb_next = ADD_OFF(x86_dpb, Index * sizeof(struct dpb));  /* memory allocated as array, so next is just next element */
    dpb->dpb_unit = LoL->nblkdev;
    dpb->dpb_subunit = Index;
    dpb->dpb_device = x86_dhp;
    dpb->dpb_flags = M_CHANGED;
    // LoL->CDSp: Current Directory Structure
    if ((FP_SEG(LoL->CDSp) != 0) && (LoL->nblkdev < LoL->lastdrive))
    {
      struct cds* CDSp = (struct cds*)ARM_PTR(LoL->CDSp);
      CDSp[LoL->nblkdev].cdsDpb = x86_dpb;
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

void* fmemset(dos_far_ptr p, int v, unsigned int sz) {
    void* res = ARM_PTR(p);
    memset(res, v, sz);
    return res;
}

void fmemcpy(dos_far_ptr d, const dos_far_ptr s, size_t n) {
    memcpy(ARM_PTR(d), ARM_PTR(s), n);
}

/*
    fgetword/fgetlong/fputword/fputlong - explicit little-endian
    byte-at-a-time read/write of a 16/32-bit value at a native
    pointer. Migrated from syspack.c (the portable version, not the
    globals.h "#define fgetword(vp) (*(UWORD FAR *)(vp))" one-liner
    used on compilers that allow unaligned/far access directly): on
    ARM, an unaligned (UWORD*)/(UDWORD*) cast like that one would be
    undefined behaviour (and on real ARM cores can fault), and these
    values come straight from a disk-sector buffer (bp->b_buffer),
    which has no alignment guarantee relative to the field being
    read/written. Used for the on-disk FAT12/16/32 entries (see
    link_fat() below) and directory entry fields (dir_time/dir_date/
    dir_start/dir_size etc., see getdirent()/putdirent() - not yet
    migrated, they currently use plain memcpy() since the in-memory
    "struct dirent" representation matches the on-disk layout field
    for field and is itself accessed only through native pointers).
*/
UWORD fgetword(const void *vp)
{
  const UBYTE *p = (const UBYTE *)vp;
  return (p[0] & 0xff) + ((p[1] & 0xff) << 8);
}

ULONG fgetlong(const void *vp)
{
  const UBYTE *p = (const UBYTE *)vp;
  return (p[0] & 0xff) +
      ((p[1] & 0xff) << 8) +
      ((ULONG)(p[2] & 0xff) << 16) +
      ((ULONG)(p[3] & 0xff) << 24);
}

void fputword(void *vp, UWORD w)
{
  UBYTE *p = (UBYTE *)vp;
  p[0] = (UBYTE)(w & 0xff);
  p[1] = (UBYTE)((w >> 8) & 0xff);
}

void fputlong(void *vp, ULONG l)
{
  UBYTE *p = (UBYTE *)vp;
  p[0] = (UBYTE)(l & 0xff);
  p[1] = (UBYTE)((l >> 8) & 0xff);
  p[2] = (UBYTE)((l >> 16) & 0xff);
  p[3] = (UBYTE)((l >> 24) & 0xff);
}

/*
    linear_to_far(p) - turn a native ARM pointer into a guest seg:off
    pair, for code (like LBA_Transfer below) that needs to load a real
    CPU_ES/CPU_BX pair before calling bios_13h().

    Request packets in this codebase carry r_trans as a plain native
    pointer (FAR expands to nothing on this "linear architecture", see
    portab.h), so by the time a request reaches here the original DOS
    segment:offset the caller used is no longer available - only the
    resulting linear guest address is. We therefore normalize with
    offset = addr & 0xF, segment = addr >> 4, which always reproduces
    the same linear address and keeps the offset far from the 0xFFFF
    boundary. This is also an honest match for this platform: bios_13h
    services the transfer with a plain linear address (see int13_transfer_lba
    in bios_13h.c), it does not emulate the real-8086 same-segment offset
    wraparound that the original 64K DMA boundary check in dsk.c exists
    to avoid, so a tight, boundary-safe normalization here is sufficient.

    SAFETY: p must point inside the 1MB guest RAM window
    [X86_RAM_BASE, X86_RAM_BASE + 0x100000). If p is outside that
    range (e.g. a stray native/kernel pointer reached here instead of
    a guest buffer), (uint16_t) truncation below would silently wrap
    around and hand back a seg:off pair that points at some unrelated
    guest address - bios_13h would then read/write through it as if
    it were the caller's buffer, corrupting guest memory instead of
    failing loudly. We assert() in debug builds and, since assert()
    compiles to nothing in release builds (see debug.h), additionally
    panic-halt unconditionally so a bad pointer can never silently
    turn into "write somewhere in guest RAM" in any build.
*/
dos_far_ptr linear_to_far(const BYTE *p)
{
  uint32_t lin = (uint32_t)(p - (intptr_t)X86_RAM_BASE);
  if (lin > EFFECTIVE(MK_FP(-1, -1)))
  {
    printf("PANIC: linear_to_far out of x86 guest RAM range %p\n", (const void *)p);
    for (;;) ;
  }
  return MK_FP((UWORD)(lin >> 4), (UWORD)(lin & 0xF));
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

int read(int fd, dos_far_ptr dst, COUNT sz) {
    CPU_AH = 0x3F;
    CPU_BX = fd;
    CPU_DX = dst.offset;
    SET_DS ( dst.segment );
    fdos_21h(cpu);
    return cf ? -1 : CPU_AX;
}

int close(int fd) {
    CPU_AH = 0x3E;
    CPU_BX = fd;
    fdos_21h(cpu);
    return cf ? -1 : CPU_AX;
}

/*
    idx_to_sft(SftIndex) - same as idx_to_sft_(), but only for
    internal callers: returns a pointer to the SFT entry, and treats
    an entry with sft_count == 0 (not currently open) as not found,
    same as the original.

    NOTE: idx_to_sft_() above walks (and overwrites) LoL->sfthead as
    it follows sftt_next, mirroring how the original walks its local
    "sp" variable started from the *global* sfthead - in the original
    sfthead itself is never modified by this walk (only the local
    copy "sp" is advanced). Doing the same here without disturbing
    LoL->sfthead would require a separate cursor; since LoL->sfthead
    always points back to the first (built-in) SFT block before any
    call into this function and idx_to_sft_() does not persist its
    walk across calls, every walk restarts from the right place, but
    plays it safe with a save/restore around the walk so LoL->sfthead
    is never observably changed by callers.
*/
sft *idx_to_sft(int SftIndex)
{
  dos_far_ptr saved_head = LoL->sfthead;
  sft *result;

  SftIndex = idx_to_sft_(SftIndex);
  LoL->sfthead = saved_head;

  /* if not opened, the SFT is useless            */
  if (SftIndex == -1)
    return (sft *)-1;

  result = (sft *)ARM_PTR(internal_data->lpCurSft);
  if (result->sft_count == 0)
    return (sft *)-1;
  return result;
}

/*
    network_redirector_mx/network_redirector_fp - network redirector
    multiplex entry points (INT 2Fh AX=11xxh equivalents: QRemote_Fn/
    remote_rw/remote_getfree/etc, see proto.h for the macros built on
    top of these).

    /// TODO: there is no network redirector (no NETX/IFS-equivalent
    /// driver, no UNC path support) in this codebase. These always
    /// reporting "failed/not handled" is the honest, correct
    /// behavior for a system with no redirector loaded - exactly what
    /// real DOS does when called with none loaded - not a shortcut:
    /// truename() (below) relies on QRemote_Fn() failing here to fall
    /// through to its local (non-networked) path resolution, the same
    /// way it would on real DOS with no redirector present.
*/
long network_redirector_mx(unsigned cmd, void *s, void *arg)
{
  UNREFERENCED_PARAMETER(cmd);
  UNREFERENCED_PARAMETER(s);
  UNREFERENCED_PARAMETER(arg);
  return -1;
}

int network_redirector_fp(unsigned cmd, void *s)
{
  UNREFERENCED_PARAMETER(cmd);
  UNREFERENCED_PARAMETER(s);
  return -1;
}

/*
    cooked_read/cooked_write/read_line_handle/update_scr_pos - "cooked"
    (line-buffered, ^Z-EOF-aware, echo-as-you-type) character device
    I/O, used by rwblock()/DosRWSft() (below) when reading/writing a
    device opened without SFT_FBINARY (raw/binary mode) - typically
    CON.

    /// TODO: stubs for this iteration. The original (chario.c) is a
    /// substantial separate module (line editing, ^C/^Break checking,
    /// tab expansion, BIOS_inkey()/BIOS_putc() console interaction).
    /// None of it is migrated yet. This iteration's actual goal
    /// (reading/closing a fixed file like CONFIG.SYS) never reaches
    /// these - CONFIG.SYS is opened as a regular file, not a console
    /// device - so these panic loudly instead of silently behaving
    /// like an empty/closed console, in case that assumption ever
    /// stops holding (e.g. something tries to DosRead() from CON).

    Migrated from chario.c (signatures only; bodies replaced as above).
*/
long cooked_read(struct dhdr **pdev, size_t n, char *bp)
{
  UNREFERENCED_PARAMETER(pdev);
  UNREFERENCED_PARAMETER(n);
  UNREFERENCED_PARAMETER(bp);
  printf("PANIC: cooked_read() called but not implemented\n");
  for (;;) ;
}

long cooked_write(struct dhdr **pdev, size_t n, char *bp)
{
  UNREFERENCED_PARAMETER(pdev);
  UNREFERENCED_PARAMETER(n);
  UNREFERENCED_PARAMETER(bp);
  printf("PANIC: cooked_write() called but not implemented\n");
  for (;;) ;
}

size_t read_line_handle(int sft_idx, size_t n, char *bp)
{
  UNREFERENCED_PARAMETER(sft_idx);
  UNREFERENCED_PARAMETER(n);
  UNREFERENCED_PARAMETER(bp);
  printf("PANIC: read_line_handle() called but not implemented\n");
  for (;;) ;
}

void update_scr_pos(unsigned char c, unsigned char count)
{
  UNREFERENCED_PARAMETER(c);
  UNREFERENCED_PARAMETER(count);
  printf("PANIC: update_scr_pos() called but not implemented\n");
  for (;;) ;
}

/*
    -----------------------------------------------------------------
    DosUpChar/DosUpString/DosUpMem/DosUpFChar/DosUpFString/DosUpFMem
    -----------------------------------------------------------------

    /// TODO: the original (nls.c) routes every one of these through
    /// nlsInfo (struct nlsInfoBlock), a fully pluggable national
    /// language support layer: COUNTRY.SYS-style codepage tables,
    /// DBCS lead-byte awareness, and a "FUpMem" variant specifically
    /// for filenames that differs from plain DosUpMem by codepage-
    /// specific rules. None of that (nlsInfo, xUpMem(), nlsFUpMem(),
    /// muxUpMem(), codepage switching, DBCS) is implemented in this
    /// codebase. What's below is a plain US-ASCII 'a'-'z' -> 'A'-'Z'
    /// uppercase, nothing else - correct only for unaccented ASCII
    /// names. Any non-ASCII/extended/DBCS byte is passed through
    /// unchanged rather than miscased, but true codepage-aware
    /// upcasing (accented Latin-1 letters, etc, as a real COUNTRY.SYS
    /// would do) is simply not there. Needed now because truename()
    /// (below) uppercases every path component as part of canonicalizing
    /// a path, same as the original.
*/
STATIC unsigned char ascii_upchar(unsigned char ch)
{
  if (ch >= 'a' && ch <= 'z')
    return (unsigned char)(ch - 'a' + 'A');
  return ch;
}

VOID DosUpMem(VOID *str, unsigned len)
{
  unsigned char *p = (unsigned char *)str;
  while (len--)
  {
    *p = ascii_upchar(*p);
    p++;
  }
}

unsigned char DosUpChar(unsigned char ch)
{
  return ascii_upchar(ch);
}

VOID DosUpString(char *str)
{
  DosUpMem(str, strlen(str));
}

VOID DosUpFMem(VOID *str, unsigned len)
{
  DosUpMem(str, len);
}

unsigned char DosUpFChar(unsigned char ch)
{
  return ascii_upchar(ch);
}

VOID DosUpFString(char *str)
{
  DosUpFMem(str, strlen(str));
}

#define PATHLEN 128

#define drLetterToNr(dr) ((unsigned char)((dr) - 'A'))
/* Convert an uppercased drive letter into the drive index */
#define drNrToLetter(dr) ((dr) + 'A')
/* the other direction */

#define PATH_ERROR() \
      strchr(src, '/') == 0 && strchr(src, '\\') == 0 \
        ? DE_FILENOTFND \
        : DE_PATHNOTFND

#define PNE_WILDCARD 1
#define PNE_DOT 2

STATIC const char _DirChars[] = "\"[]:|<>+=;,";

#define DirChar(c)  (((unsigned char)(c)) >= ' ' && \
                     !strchr(_DirChars, (c)))

/* /// TODO: SFTMAX (128, the number of file handles) is what the
   original actually uses here too (see newstuff.c) - a path-length
   limit reusing an unrelated constant looks like a historical typo
   in the original, but per this project's porting policy, bugs in
   the original are preserved rather than silently "fixed". */
#define addChar(c) \
{ \
  if (p >= dest + SFTMAX) return PATH_ERROR(); /* path too long */	\
  *p++ = c; \
}

/* Map a logical path into a physical one.

	1) Uppercasing path.
	2) Flipping '/' -> '\\'.
	3) Removing empty directory components & ".".
	4) Processing ".." components.
	5) Convert path components into 8.3 convention.
	6) Make it fully-qualified.
	7) Map it to SUBST/UNC.
        8) Map to JOIN.

   Return:
   	*cdsItem will be point to the appropriate CDS entry. This will allow
   	the caller to aquire the DPB or the IFS informtion of this entry.
   	error number
   	Return value:
   		DE_FILENOTFND, or DE_PATHNOTFND (as described in RBIL)
   	If the output path pnfo->physPath exceeds the length MAX_PATH, the error
   	DE_FILENOTFND will be returned.

    Migrated from newstuff.c. Differences from the original:
      - src is a dos_far_ptr here (it comes straight from the guest
        program via DS:DX, just like the original), copied into a
        native PATHLEN-sized stack buffer up front via ARM_PTR() so
        the rest of the function (an exact port of the original's
        logic) can work with a plain native char* the same way
        split_path()/dos_open()/etc above already do. There is no
        adjust_far()-equivalent normalization step: adjust_far()
        exists to keep a real 16-bit DOS far pointer's offset away
        from the 0xFFFF wraparound boundary as the original indexes
        further and further into src - ARM_PTR()/EFFECTIVE() compute
        a plain linear address with no such 16-bit wraparound to
        guard against (the same reasoning as linear_to_far()'s
        comment above), so copying once up front and then doing
        ordinary pointer arithmetic on a native copy is sufficient.
      - UNC paths (the "\\\\server\\share" case), the network
        redirector (QRemote_Fn()), and SHARE/JOIN are all real, live
        code paths in the original - migrated as-is, but with
        IsShareInstalled()/network_redirector_mx() always reporting
        "not present" (see their definitions above), so they correctly
        fall through to local (non-networked, non-JOINed) path
        resolution on every call, exactly as real DOS would with no
        redirector loaded. The JOIN loop further down is similarly
        live code that simply never matches while LoL->njoined stays 0
        (nothing in this codebase implements the JOIN command yet).
      - get_cds() returns a dos_far_ptr in this codebase (not a
        directly-dereferenceable "struct cds FAR *"), so cdsEntry and
        current_ldt are handled as dos_far_ptr/native-pointer pairs:
        x86_cdsEntry holds the dos_far_ptr, cdsEntry is the ARM_PTR()
        of it; internal_data->current_ldt (a dos_far_ptr field, see
        lol.h) is what the original's bare "current_ldt = cdsEntry"
        assignments become.
      - media_check()/TempCDS.cdsDpb: cdsDpb is a dos_far_ptr (see
        cds.h), so media_check() (which takes a native struct dpb*)
        needs an ARM_PTR() first.
*/
COUNT truename(dos_far_ptr x86_src, char *dest, COUNT mode)
{
  COUNT i;
  struct dhdr *dhp;
  const char *froot;
  COUNT result;
  unsigned state;
  dos_far_ptr x86_cdsEntry;
  struct cds *cdsEntry;
  char *p = dest;	  /* dynamic pointer into dest */
  char *rootPos;
  char src0;
  char srcbuf[PATHLEN];
  char *src;
  struct cds TempCDS;

  /* copy the guest path into a native buffer up front - see the
     migration note above for why there is no adjust_far() step */
  {
    unsigned len;
    const char *guest_src = (const char *)ARM_PTR(x86_src);
    for (len = 0; len < sizeof(srcbuf) - 1; len++)
    {
      srcbuf[len] = guest_src[len];
      if (srcbuf[len] == '\0')
        break;
    }
    srcbuf[sizeof(srcbuf) - 1] = '\0';
  }
  src = srcbuf;

  /* In opposite of the TRUENAME shell command, an empty string is
     rejected by MS DOS 6 */
  src0 = src[0];
  if (src0 == '\0')
    return DE_FILENOTFND;

  if (src0 == '\\' && src[1] == '\\') {
    const char *unc_src = src;
    /* Flag UNC paths and short circuit processing.  Set current LDT   */
    /* to sentinel (offset 0xFFFF) for redirector processing.          */
    do {
      src0 = unc_src[0];
      addChar(src0);
      unc_src++;
    } while (src0);
    internal_data->current_ldt = MK_FP(0xFFFF, 0xFFFF);
    /* Flag as network - drive bits are empty but shouldn't get */
    /* referenced for network with empty current_ldt.           */
    return IS_NETWORK;
  }

  /* Do we have a drive?                                          */
  if (src[1] == ':')
    result = drLetterToNr(DosUpFChar(src0));
  else
    result = internal_data->default_drive;

  dhp = IsDevice(src);

  x86_cdsEntry = get_cds(result);
  cdsEntry = far_is_null(x86_cdsEntry) ? NULL : (struct cds *)ARM_PTR(x86_cdsEntry);
  if (cdsEntry == NULL)
  {
    /* If opening a character device, DOS allows device name
       to be prefixed by [invalid] drive letter and/or optionally
       \DEV\ directory prefix, however, any other directory
       including root (\) is an invalid path if drive is not
       valid and returns such.
       Whereas truename always fails for invalid drive.
    */
    if (dhp && (mode & CDS_MODE_CHECK_DEV_PATH) && (result >= LoL->lastdrive))
    {
      /* Note: check for (result >= lastdrive) means invalid drive
         was provided as otherwise we would have used default_drive
         so we know src in the form of X:?
         fail if anything other than no path or path is \DEV\
      */
      const char *s = src+2;
      char c = *s;

      if( c != '\\' && c != '/' ) c = '\0';
      /* could be 1 letter devicename, don't go scanning random memory */
      if (*(src+3) != '\0')
      {
        s = strchr(src+3, '\\'); /* ?is there \ or / other than immediately after drive: */
        if (s == NULL) s = strchr(src+3, '/');
      }
      else
      {
        s = NULL;
      }

      if (c == '\0')
      {
        /* either X:devicename or X:path\devicename */
        if (s != NULL) goto invalid_path;
      }
      else
      {
        /* either X:\devicename or X:\path\devicename 
           only X:\DEV\devicename is valid path
        */
        if (s == NULL) goto invalid_path;
        if (s != src+6) goto invalid_path;
        if (memcmp(src+3, "DEV", 3) != 0) goto invalid_path;
        s = strchr(src+7, '\\');
        if (s == NULL) s = strchr(src+7, '/');
        if (s != NULL) goto invalid_path;
      }

      /* use CDS of current drive (MS-DOS may return drive P: for invalid drive.) */
      result = internal_data->default_drive;
      x86_cdsEntry = get_cds(result);
      cdsEntry = far_is_null(x86_cdsEntry) ? NULL : (struct cds *)ARM_PTR(x86_cdsEntry);
      if (cdsEntry == NULL) goto invalid_path;
    }
    else
    {
invalid_path:
        return DE_PATHNOTFND;
    }
  }

  memcpy(&TempCDS, cdsEntry, sizeof(TempCDS));
  /* is the current_ldt thing necessary for compatibly??
     -- 2001/09/03 ska*/
  internal_data->current_ldt = x86_cdsEntry;
  if (TempCDS.cdsFlags & CDSNETWDRV)
    result |= IS_NETWORK;

  if (dhp)
    result |= IS_DEVICE;

  /* Try if the Network redirector wants to do it */
  /* via Qualify Remote Filename call & validate results */
  memset(dest, 0, 12);  /* enable can verify redirector set result value */
  /* MUX succeeded and really something */
  if (!(mode & CDS_MODE_SKIP_PHYSICAL) &&
      QRemote_Fn(dest, src) == SUCCESS && dest[0] != '\0')
  {
    /* don't flag devices such as Z:/NUL as NETWORK devices,
       where Z: is a network mapped drive, but do flag redirected
       devices such as LPT# for Lantastic
     */       
    if (dest[2] == '/' && (result & IS_DEVICE))
      result &= ~IS_NETWORK;
    else
      result |= IS_NETWORK;
    return result;
  }

  /* Redirector interface failed --> proceed with local mapper */
  dest[0] = drNrToLetter(result & 0x1f);
  dest[1] = ':';

  /* Do we have a drive? */
  if (src[1] == ':')
    src += 2;

/*
    Code repoff from dosfns.c
    MSD returns X:/CON for truename con. Not X:\CON
*/
  /* check for a device  */

  dest[2] = '\\';
  if (result & IS_DEVICE)
  {
    froot = get_root(src);
    if (froot == src || froot == src + 5)
    {
      if (froot == src + 5)
      {
        memcpy(dest + 3, src, 5);
        DosUpMem(dest + 3, 5);
        if (dest[3] == '/') dest[3] = '\\';
        if (dest[7] == '/') dest[7] = '\\';
      }
      if (froot == src || memcmp(dest + 3, "\\DEV\\", 5) == 0)
      {
        /* /// Bugfix: NUL.LST is the same as NUL.  This is true for all
           devices.  On a device name, the extension is irrelevant
           as long as the name matches.
           - Ron Cemer */
        dest[2] = '/';
        result &= ~IS_NETWORK;
        /* /// DOS will return C:/NUL.LST if you pass NUL.LST in.
           DOS will also return C:/NUL.??? if you pass NUL.* in.
           Code added here to support this.
           - Ron Cemer */
        src = (char *)froot;
      }
    }
  }

  /* Make fully-qualified logical path */
  /* register these two used characters and the \0 terminator byte */
  /* we always append the current dir to stat the drive;
     the only exceptions are devices without paths */
  rootPos = p = dest + 2;
  if (*p != '/') /* i.e., it's a backslash! */
  {
    BYTE *cp;

    cp = TempCDS.cdsCurrentPath;
    /* ensure termination of strcpy */
    cp[MAX_CDSPATH - 1] = '\0';
    if ((TempCDS.cdsFlags & CDSNETWDRV) == 0)
    {
      if (media_check((struct dpb *)ARM_PTR(TempCDS.cdsDpb)) < 0)
        return DE_PATHNOTFND;

      /* dos_cd ensures that the path exists; if not, we
         need to change to the root directory */
      if (dos_cd((char *)cp) != SUCCESS) {
        cp[TempCDS.cdsBackslashOffset + 1] =
          cdsEntry->cdsCurrentPath[TempCDS.cdsBackslashOffset + 1] = '\0';
        dos_cd((char *)cp);
      }
    }

    if (!(mode & CDS_MODE_SKIP_PHYSICAL))
    {
/* What to do now: the logical drive letter will be replaced by the hidden
   portion of the associated path. This is necessary for NETWORK and
   SUBST drives. For local drives it should not harm.
   This is actually the reverse mechanism of JOINED drives. */

      strcpy(dest, (char *)cp);
      if (TempCDS.cdsFlags & CDSSUBST)
      {
        /* The drive had been changed --> update the CDS pointer */
        if (dest[1] == ':')
        {  /* sanity check if this really is a local drive still */
          unsigned ii = drLetterToNr(dest[0]);

          /* truename returns the "real", not the "virtual" drive letter! */
          if (ii < LoL->lastdrive) /* sanity check #2 */
            result = (result & 0xffe0) | ii;
        }
      }
      rootPos = p = dest + TempCDS.cdsBackslashOffset;
    }
    else
    {
      cp += TempCDS.cdsBackslashOffset;
      /* truename must use the CuDir of the "virtual" drive letter! */
      strcpy(p, (char *)cp);
    }
    if (p[0] == '\0')
      p[1] = p[0];
    p[0] = '\\'; /* force backslash! */

    if (*src != '\\' && *src != '/')
      p += strlen(p);
    else /* skip the absolute path marker */
      src++;
    /* remove trailing separator */
    if (p[-1] == '\\') p--;
  }

  /* append the path specified in src */

  state = 0;
  while(*src)
  {
    /* New segment.  If any wildcards in previous
       segment(s), this is an invalid path. */
    if (state & PNE_WILDCARD)
      return DE_PATHNOTFND;

    /* append backslash if not already there.
       MS DOS preserves a trailing '\\', so an access to "C:\\DOS\\"
       or "CDS.C\\" fails; in that case the last new segment consists of just
       the \ */
    if (p[-1] != *rootPos)
      addChar(*rootPos);
    /* skip multiple separators (duplicated slashes) */
    while (*src == '/' || *src == '\\')
      src++;

    if(*src == '.')
    {
      int dots = 1;
      /* special directory component */
      ++src;
      if (*src == '.') /* skip the second dot */
      {
        ++src;
        dots++;
      }
      if (*src == '/' || *src == '\\' || *src == '\0')
      {
        --p; /* backup the backslash */
        if (dots == 2)
        {
          /* ".." entry */
          /* remove last path component */
          while(*--p != '\\')
            if (p <= rootPos) /* already on root */
              return DE_PATHNOTFND;
        }
        continue;	/* next char */
      }

      /* ill-formed .* or ..* entries => return error */
      /* The error is either PATHNOTFND or FILENOTFND
         depending on if it is not the last component */
      return PATH_ERROR();
    }

    /* normal component */
    /* append component in 8.3 convention */

    /* *** parse name and extension *** */
    i = FNAME_SIZE;
    state &= ~PNE_DOT;
    while(*src != '/' && *src  != '\\' && *src != '\0')
    {
      char c = *src++;
      if (c == '*')
      {
        /* register the wildcard, even if no '?' is appended */
        c = '?';
        while (i)
        {
          --i;
          addChar(c);
        }
      }
      if (c == '.')
      {
        if (state & PNE_DOT) /* multiple dots are ill-formed */
          return PATH_ERROR();
        /* strip trailing dot */
        if (*src == '/' || *src == '\\' || *src == '\0')
          break;
        /* we arrive here only when an extension-dot has been found */
        state |= PNE_DOT;
        i = FEXT_SIZE + 1;
      }
      else if (c == '?')
        state |= PNE_WILDCARD;
      if (i) {	/* name length in limits */
        --i;
        if (!DirChar(c)) return PATH_ERROR();
        addChar(c);
      }
    }
    /* *** end of parse name and extension *** */
  }
  if (state & PNE_WILDCARD && !(mode & CDS_MODE_ALLOW_WILDCARDS))
    return DE_PATHNOTFND;
  if (p == dest + 2)
  {
    /* we must always add a seperator if dest = "c:" */
    addChar('\\');
  }

  *p = '\0';				/* add the string terminator */
  DosUpFString(rootPos);	        /* upcase the file/path name */

/** Note:
    Only the portions passed in by the user are upcased, because it is
    assumed that the CDS is configured correctly and if it contains
    lower case letters, it is required so **/

  /* Now, all the steps 1) .. 7) are fullfilled. Join now */
  /* search, if this path is a joined drive */

  if (dest[2] != '/' && (!(mode & CDS_MODE_SKIP_PHYSICAL)) && LoL->njoined)
  {
    dos_far_ptr x86_cdsp = LoL->CDSp;
    struct cds *cdsp = (struct cds *)ARM_PTR(x86_cdsp);
    for(i = 0; i < LoL->lastdrive; ++i, ++cdsp)
    {
      /* How many bytes must match */
      size_t j = strlen((char *)cdsp->cdsCurrentPath);
      /* the last component must end before the backslash offset and */
      /* the path the drive is joined to leads the logical path */
      if ((cdsp->cdsFlags & CDSJOINED) && (dest[j] == '\\' || dest[j] == '\0')
         && memcmp(dest, cdsp->cdsCurrentPath, j) == 0)
      { /* JOINed drive found */
        dest[0] = drNrToLetter(i);	/* index is physical here */
        dest[1] = ':';
        if (dest[j] == '\0')
        {	/* Reduce to root direc */
          dest[2] = '\\';
          dest[3] = 0;
          /* move the relative path right behind the drive letter */
        }
        else if (j != 2)
        {
          strcpy(dest + 2, dest + j);
        }
        result = (result & 0xffe0) | i; /* tweak drive letter (JOIN) */
        internal_data->current_ldt = x86_FAR_PTR(FP_SEG(LoL->CDSp), cdsp);
        result &= ~IS_NETWORK;
        if (cdsp->cdsFlags & CDSNETWDRV)
          result |= IS_NETWORK;
        return result;
      }
    }
    /* nothing found => continue normally */
  }
  if ((mode & CDS_MODE_CHECK_DEV_PATH) &&
      ((result & (IS_DEVICE|IS_NETWORK)) == IS_DEVICE) &&
      dest[2] != '/' && !dir_exists(dest))
    return DE_PATHNOTFND;

  /* Note: Not reached on error or if JOIN or QRemote_Fn (2f.1123) matched */
  if (mode==CDS_MODE_ALLOW_WILDCARDS) /* DosTruename mode */
  {
    /* in other words: result & 0x60 = 0x20...: */
    /// TODO: os_major is not tracked in this codebase (no real
    /// "DOS version" concept yet) - always taking the "else" branch
    /// here (as if os_major were never 6) until that exists.
    result = 0; /* AL is 00, 2f, 5c, or last-of-TempCDS.cdsCurrentPath? */
  }
  return result;
}

/* FAT time notation in the form of hhhh hmmm mmmd dddd (d = double second)

    Migrated from fatfs.c verbatim - t->hour/minute/second become
    plain CPU register reads (see dos_gettime() below) instead of a
    "struct dostime *" parameter, since this codebase's DosGetTime()
    fills CPU registers (CPU_CH/CPU_CL/CPU_DH/CPU_DL), not a C struct
    (see fdos_21h.c) - so time_encode() is inlined directly into
    dos_gettime() rather than kept as a separate helper taking a
    struct dostime this codebase doesn't have.
*/

/*
    dos_getdate()/dos_gettime() - the current system date/time,
    DOS-directory-entry-encoded (ddate/dtime, see ddate.h/dtime.h),
    for stamping a file's creation/modification date and time.

    Migrated from fatfs.c. The original calls DosGetDate(&dd)/
    DosGetTime(&dt) (filling a "struct dosdate"/"struct dostime"); this
    codebase's DosGetDate()/DosGetTime() instead fill CPU registers
    (see fdos_21h.c) the same way every other DOS-API-shaped function
    here does, so the date/time fields are read from CPU_CX/CPU_DH/
    CPU_DL (date) and CPU_CH/CPU_CL/CPU_DH/CPU_DL (time) after calling
    them, rather than from a struct - same underlying ExecuteClockDriverRequest()/
    internal_data->ClkRecord data, just read back through the CPU
    register convention this codebase's DosGetDate()/DosGetTime()
    already use.
*/
ddate dos_getdate(void)
{
  DosGetDate(cpu);
  return DT_ENCODE(CPU_DH, CPU_DL, CPU_CX - EPOCH_YEAR);
}

dtime dos_gettime(void)
{
  DosGetTime(cpu);
  return (CPU_CH << 11) | (CPU_CL << 5) | (CPU_DH >> 1);
}

STATIC VOID FsConfig(VOID)
{
  dos_far_ptr x86_dpb = LoL->DPBp;
  struct dpb* dpb = (struct dpb*)ARM_PTR(x86_dpb);
  int i;

  /* Initialize the current directory structures    */
  for (i = 0; i < LoL->lastdrive; i++)
  {
    struct cds* pcds_table = (struct cds*)ARM_PTR(LoL->CDSp) + i;

    memcpy(pcds_table->cdsCurrentPath, "A:\\\0", 4);

    pcds_table->cdsCurrentPath[0] += i;

    if (i < LoL->nblkdev && dpb != (struct dpb*)ARM_PTR(MK_FP(-1, -1)))
    {
      pcds_table->cdsDpb = x86_dpb;
      pcds_table->cdsFlags = CDSPHYSDRV;
      x86_dpb = dpb->dpb_next;
      dpb = (struct dpb*)ARM_PTR(x86_dpb);
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

/*
       Initialize all printers
 
       this should work. IMHO, this might also be done on first use
       of printer, as I never liked the noise by a resetting printer, and
       I usually much more often reset my system, then I print :-)
 */

STATIC VOID InitPrinters(VOID)
{
  bios_11h(cpu);     /* get equipment list */
  int num_printers = (CPU_AX >> 14) & 3;     /* bits 15-14 */
  for (int i = 0; i < num_printers; i++)
  {
    CPU_AX = 0x0100;             /* initialize printer */
    CPU_DX = i;
    bios_17h(cpu);
  }
}

STATIC VOID InitSerialPorts(VOID)
{
  bios_11h(cpu);     /* get equipment list */
  int serial_ports = (CPU_AX >> 9) & 7;      /* bits 11-9 */
  for (int i = 0; i < serial_ports; i++)
  {
    CPU_AX = 0xA3;               /* initialize serial port to 2400,n,8,1 */
    CPU_DX = i;
    bios_14h(cpu);
  }
}

STATIC void init_kernel(CPU* cpu)
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

    InitPrinters();
    InitSerialPorts();

    init_PSPSet(cpu, DOS_PSP);
    set_DTA(MK_FP(DOS_PSP, 0x80));
    PSPInit();

    Init_clk_driver(cpu);

    /* Do first initialization of system variable buffers so that   */
    /* we can read config.sys later.  */

    /* use largest possible value for the initial CDS */
    LoL->lastdrive = 26;

    /*  init_device((struct dhdr FAR *)&blk_dev, NULL, 0, &ram_top); */
    /*  WARNING: dsk_init() must be called prior to update_dcb() to ensure
        _Dyn (start of Dynamic memory block) is the start of drive data table (see getddt() in dsk.c)
     */
    blk_dev->dh_name[0] = dsk_init(cpu);

    PreConfig();
    /* Number of units */
    if (blk_dev->dh_name[0] > 0) {
        update_dcb(x86_blk_dev);
    }

    /* Now config the temporary file system */
    FsConfig();

    /* Now process CONFIG.SYS     */
    DoConfig(0);
    DoConfig(1);

/// TODO:
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
    init_kernel(_cpu);

    /// TODO: next point to complete impl.

    /// debug-blink this point acived
    for (int i = 0; i < 6; i++) {
    /// TODO: for reboot    keyboard_tick();
        sleep_ms(23);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(23);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
    // allow it to wait for keyboard 
    SET_CS ( 0xF000 ); // -> FFEFFh (bios callback)
    SET_IP ( 0xFEFF );
    set_bios_callback(cpu, &params, false);
    printf("FreeDOS impl. is incomplete. Nothing to do for now...\n");
}
