#include <pico.h>
#include <pico/time.h>
#include <hardware/pio.h>
#include "../cpu.h"
#include "../bios.h"
#include "../fdos.h"
#include "i8254.h"

#define printf(...) bios_printf(cpu, __VA_ARGS__)

#ifndef PSRAM_BASE_ADDR
#define PSRAM_BASE_ADDR   0x11000000
#endif
#define X86_RAM_BASE ((uint8_t*)PSRAM_BASE_ADDR)

static CPU* cpu;

#define MAX_HARD_DRIVE  4

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

BYTE HaltCpuWhileIdle = 0;
UWORD ram_top = 0;
dos_far_ptr lpTop;
COUNT nUnits BSS_INIT(0);
extern unsigned char DOSTEXTFAR ASM int1e_table[0xe];

COUNT ASM CritErrCode = 0;          // 04 - DOS format error Code
request ASM CharReqHdr;             // 3A - Device driver request header
struct ClockRecord ASM ClkRecord;   // 96 - CLOCK$ transfer record

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

static struct dhdr blk_dev; // forward declaration
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
        rq->r_nunits = blk_dev.dh_name[0];
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

static struct dhdr blk_dev = {
    .dh_next = MK_FP(-1, -1),
    .dh_attr = 0x08c2 | ATTR_NATIVE,
    .arm.dh_interrupt = BlkEntry,
    .dh_name = { 4, 0, 0, 0, 0, 0, 0, 0 },
    .next = NULL
};

static struct dhdr clk_dev = {
    .dh_next = MK_FP(-1, -1),
    .dh_attr = 0x8008 | ATTR_NATIVE,
    .arm.dh_interrupt = ClkEntry,
    .dh_name = "CLOCK$  ",
    .next = &blk_dev,
};

static struct dhdr com4_dev = {
    .dh_next = MK_FP(-1, -1),
    .next = &clk_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com4Intr,
    .dh_name = "COM4    "
};

static struct dhdr com3_dev = {
    .dh_next = MK_FP(-1, -1),
    .next = &com4_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com3Intr,
    .dh_name = "COM3    "
};

static struct dhdr com2_dev = {
    .dh_next = MK_FP(-1, -1),
    .next = &com3_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com2Intr,
    .dh_name = "COM2    "
};

static struct dhdr com1_dev = {
    .dh_next = MK_FP(-1, -1),
    .next = &com2_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = AuxIntr,
    .dh_name = "COM1    "
};

static struct dhdr lpt3_dev = {
    .dh_next = MK_FP(-1, -1),
    .next = &com1_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt3Intr,
    .dh_name = "LPT3    "
};

static struct dhdr lpt2_dev = {
    .dh_next = MK_FP(-1, -1),
    .next = &lpt3_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt2Intr,
    .dh_name = "LPT2    "
};

static struct dhdr lpt1_dev = {
    .dh_next = MK_FP(-1, -1),
    .next = &lpt2_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt1Intr,
    .dh_name = "LPT1    "
};

static struct dhdr aux_dev = {
    .dh_next = MK_FP(-1, -1),
    .next = &lpt1_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = AuxIntr,
    .dh_name = "AUX     "
};

static struct dhdr prn_dev = {
    .dh_next = MK_FP(-1, -1),
    .next = &aux_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = PrnIntr,
    .dh_name = "PRN     "
};

static struct dhdr con_dev = {
    .dh_next = MK_FP(-1, -1),
    .next = &prn_dev,
    .dh_attr = 0x8013 | ATTR_NATIVE,
    .arm.dh_interrupt = ConIntr,
    .dh_name = "CON     "
};

static struct lol lol = {
    .DPBp        = (struct dpb far *)-1,
    .sfthead     = 0,
    .clock       = &clk_dev,
    .syscon      = &con_dev,
    .maxsecsize  = 512,
    .CDSp        = 0,
    .FCBp        = 0,
    .nprotfcb    = 0,
    .nblkdev     = 0,
    .lastdrive   = 0,

    .nul_dev = {
        .dh_next = MK_FP(-1, -1),
        .next = &con_dev,
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

    .os_setver_minor = 0,
    .os_setver_major = 5,
    .os_minor        = 0,
    .os_major        = 5,
    .rev_number      = 0,
    .version_flags   = 0,
    .os_release      = KERNEL_VERSION /* near ptr на строку os_release */
};
struct lol* LoL = &lol;

void dos_puts(const char* str) {
    u16 ax = CPU_AX;
    u16 bx = CPU_BX;
    CPU_AH = 0x0e;
    CPU_BX = 0x0007;
    while(*str) {
        CPU_AL = *str;
        bios_10h(cpu);
        str++;
    }
    CPU_AX = ax;
    CPU_BX = bx;
}

static void x86_execrh() {
  /// TODO: see execrh.asm
        print_line("KERNEL INIT TODO (x86_execrh)", 1);
        while(1);
}

WORD ASMPASCAL execrh(request FAR * rq, struct dhdr FAR * dhp) {
  if (dhp->dh_attr & ATTR_NATIVE) {
      dhp->arm.dh_interrupt(rq);
  } else {
      x86_execrh();
  }
  return rq->r_status;
}

void FAR * KernelAllocPara(size_t nPara, char type, char *name, int mode) {
  /// TODO:
        print_line("KERNEL INIT TODO (KernelAllocPara)", 1);
        while(1);

  return NULL;
}

/* check for a block device and update  device control block    */
STATIC VOID update_dcb(struct dhdr FAR * dhp)
{
  REG COUNT Index;
  COUNT nunits = dhp->dh_name[0];
   // Drive Parameter Block: описание одного DOS-диска/логического drive unit.
   // Через DPB DOS связывает букву диска с конкретным block-device driver и его subunit.
  struct dpb FAR *dpb;

  /* printf("nblkdev = %i\n", LoL->nblkdev); */
  
  /* if no units, nothing to do, ensure at least 1 unit for rest of logic */
  if (nunits == 0) return;

  /* allocate memory for new device control blocks, insert into chain [at end], and update our pointer to new end */
  dpb = (struct dpb FAR *)calloc(nunits, sizeof(struct dpb));
  /* it was:
  if ( LoL->first_mcb ) {
    dpb = (struct dpb FAR *)KernelAlloc(nunits * sizeof(struct dpb), 'E', Config.cfgDosDataUmb);
  }
  else {
    dpb = DynAlloc("DPBp", blk_dev.dh_name[0], sizeof(struct dpb));
  }
  */

  /* find end of dpb chain or initialize root if needed */
  if (LoL->nblkdev == 0)
  {
    /* update root pointer to new end (our just allocated block) */
    LoL->DPBp = dpb;
  }  
  else
  {
    struct dpb FAR *tmp_dpb;
    /* find current end of dpb chain by following next pointers to end */
    for (tmp_dpb = LoL->DPBp; (ULONG) tmp_dpb->dpb_next != 0xFFFFFFFFl; tmp_dpb = tmp_dpb->dpb_next)
      ;
    /* insert into chain [at end] */
    tmp_dpb->dpb_next = dpb;
  }
  /* dpb points to last block, one just allocated */

  for (Index = 0; Index < nunits; Index++)
  {		
    /* printf("processing unit %i of %i nunits\n", Index, nunits); */
    dpb->dpb_next = dpb + 1;  /* memory allocated as array, so next is just next element */
    dpb->dpb_unit = LoL->nblkdev;
    dpb->dpb_subunit = Index;
    dpb->dpb_device = dhp;
    dpb->dpb_flags = M_CHANGED;
    // LoL->CDSp: Current Directory Structure
    if ((LoL->CDSp != 0) && (LoL->nblkdev < LoL->lastdrive))
    {
      LoL->CDSp[LoL->nblkdev].cdsDpb = dpb;
      LoL->CDSp[LoL->nblkdev].cdsFlags = CDSPHYSDRV;
    }
	
    ++dpb;  /* dbp = dbp->dpb_next; */
    ++LoL->nblkdev;
  }
  /* note that always at least 1 valid dpb due to above early exit if nunits==0 */
  (dpb - 1)->dpb_next = (void FAR *)0xFFFFFFFFl;

  /* printf("processed %i nunits\n", nunits); */
}

/* If cmdLine is NULL, this is an internal driver */

BOOL init_device(struct dhdr FAR * dhp, char *cmdLine, COUNT mode,
                 dos_far_ptr * r_top)
{
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

  execrh((request FAR *) & rq, dhp);

/*
 *  Added needed Error handle
 */
  if ((rq.r_status & (S_ERROR | S_DONE)) == S_ERROR)
    return TRUE;

  if (cmdLine)
  {
    /* Don't link in device drivers which do not take up memory */
    if (EFFECTIVE(rq.r_endaddr) == EFFECTIVE(dhp->this))
      return TRUE;

    /* Don't link in block device drivers which indicate no units */
    if (!(dhp->dh_attr & ATTR_CHAR) && !rq.r_nunits)
    {
      rq.r_endaddr = dhp->this;
      return TRUE;
    }


    /* Fix for multisegmented device drivers:                          */
    /*   If there are multiple device drivers in a single driver file, */
    /*   only the END ADDRESS returned by the last INIT call should be */
    /*   the used.  It is recommended that all the device drivers in   */
    /*   the file return the same address                              */

    if (FP_OFF(dhp->dh_next) == 0xffff)
    {
        print_line("KERNEL INIT TODO (KernelAllocPara)", 1);
        while(1);
 /// TODO:     KernelAllocPara(FP_SEG(rq.r_endaddr) + (FP_OFF(rq.r_endaddr) + 15)/16
 ///                     - FP_SEG(dhp), 'D', name, mode);
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
    update_dcb(dhp);
  }

  if (dhp->dh_attr & ATTR_CONIN)
    LoL->syscon = dhp;
  else if (dhp->dh_attr & ATTR_CLOCK)
    LoL->clock = dhp;

  return FALSE;
}

STATIC void InitIO()
{
  struct dhdr far *device = &LoL->nul_dev;

  /* Initialize driver chain                                      */
  do {
    init_device(device, NULL, 0, &lpTop);
    device = device->next;
  }
  while (device != NULL);
}

static void init_PSPSet(u16 psp) {
    CPU_AH = 0x50; // Set Current PSP
    CPU_BX = psp;
    fdos_21h(cpu);
}

static void set_DTA(dos_far_ptr p) {
    CPU_AH = 0x1A; // Set Current DTA
    cpu->ext_accessors->set_seg16(cpu, SEG_DS, FP_SEG(p));
    CPU_DX = p.offset;
    fdos_21h(cpu);
}

static dos_far_ptr getvec(uint8_t intno) {
    uint32_t res = pload32(4ul * intno);
    return *(dos_far_ptr*)&res;
}

STATIC void PSPInit(void)
{
  dos_far_ptr p_x86 = MK_FP(DOS_PSP, 0);
  psp far *p = (psp far *) ARM_PTR(p_x86);

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
  p->ps_parent = FP_SEG(p_x86);
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

/// TODO:
#if 0 
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
      foundPartitions[HardDrive] =
          ProcessDisk(SCAN_PRIMARYBOOT, HardDrive, 0);

      if (foundPartitions[HardDrive] == 0)
        foundPartitions[HardDrive] =
            ProcessDisk(SCAN_PRIMARY, HardDrive, 0);
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
#endif
}

/* disk initialization: returns number of units */
COUNT dsk_init()
{
///  if (InitKernelConfig.Verbose >= 1) printf("\nInitDisk\n");

#if defined(DEBUG) && !defined(DOSEMU) && !defined(DEBUG_PRINT_COMPORT)
  {
    iregs regs;
    regs.a.x = 0x1112;          /* select 43 line mode - more space for partinfo */
    regs.b.x = 0;
    init_call_intr(0x10, &regs);
  }
#endif

  /* Reset the drives                                             */
  BIOS_drive_reset(0);

  ReadAllPartitionTables();

  return nUnits;
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
    blk_dev.dh_name[0] = dsk_init();

///  PreConfig();

  /* Number of units * /
  if (blk_dev.dh_name[0] > 0)
    update_dcb(&blk_dev);
*/
  /* Now config the temporary file system */
///  FsConfig();

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
