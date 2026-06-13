#include "../cpu.h"
#include "../bios.h"
#include "../fdos.h"

#ifndef PSRAM_BASE_ADDR
#define PSRAM_BASE_ADDR   0x11000000
#endif
#define X86_RAM_BASE ((uint8_t*)PSRAM_BASE_ADDR)

static CPU* cpu;

#include "hdr/kconfig.h"
#include "hdr/portab.h"
#include "hdr/lol.h"
#include "hdr/version.h"
#include "globals.h"

BYTE HaltCpuWhileIdle = 0;
UWORD ram_top = 0;
BYTE FAR *lpTop BSS_INIT(0);

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
        rq_done(rq);
        break;
    default:
        rq_error(rq, E_CMD);
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
    .dh_next = (struct dhdr far *)-1,
    .dh_attr = 0x08c2 | ATTR_NATIVE,
    .arm.dh_interrupt = BlkEntry,
    .dh_name = { 4, 0, 0, 0, 0, 0, 0, 0 }
};

static struct dhdr clk_dev = {
    .dh_next = &blk_dev,
    .dh_attr = 0x8008 | ATTR_NATIVE,
    .arm.dh_interrupt = ClkEntry,
    .dh_name = "CLOCK$  "
};

static struct dhdr com4_dev = {
    .dh_next = &clk_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com4Intr,
    .dh_name = "COM4    "
};

static struct dhdr com3_dev = {
    .dh_next = &com4_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com3Intr,
    .dh_name = "COM3    "
};

static struct dhdr com2_dev = {
    .dh_next = &com3_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com2Intr,
    .dh_name = "COM2    "
};

static struct dhdr com1_dev = {
    .dh_next = &com2_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = AuxIntr,
    .dh_name = "COM1    "
};

static struct dhdr lpt3_dev = {
    .dh_next = &com1_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt3Intr,
    .dh_name = "LPT3    "
};

static struct dhdr lpt2_dev = {
    .dh_next = &lpt3_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt2Intr,
    .dh_name = "LPT2    "
};

static struct dhdr lpt1_dev = {
    .dh_next = &lpt2_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt1Intr,
    .dh_name = "LPT1    "
};

static struct dhdr aux_dev = {
    .dh_next = &lpt1_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = AuxIntr,
    .dh_name = "AUX     "
};

static struct dhdr prn_dev = {
    .dh_next = &aux_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = PrnIntr,
    .dh_name = "PRN     "
};

static struct dhdr con_dev = {
    .dh_next = &prn_dev,
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
        .dh_next = &con_dev,
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
static struct lol* LoL = &lol;

void dos_puts(const char* str) {
    u16 ax = CPU_AH;
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
  return NULL;
}

/* check for a block device and update  device control block    */
STATIC VOID update_dcb(struct dhdr FAR * dhp) {
  /// TODO:
}


/* If cmdLine is NULL, this is an internal driver */

BOOL init_device(struct dhdr FAR * dhp, char *cmdLine, COUNT mode,
                 char FAR **r_top)
{
  request rq;
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
    if (rq.r_endaddr == (BYTE FAR *) dhp)
      return TRUE;

    /* Don't link in block device drivers which indicate no units */
    if (!(dhp->dh_attr & ATTR_CHAR) && !rq.r_nunits)
    {
      rq.r_endaddr = (BYTE FAR *) dhp;
      return TRUE;
    }


    /* Fix for multisegmented device drivers:                          */
    /*   If there are multiple device drivers in a single driver file, */
    /*   only the END ADDRESS returned by the last INIT call should be */
    /*   the used.  It is recommended that all the device drivers in   */
    /*   the file return the same address                              */

    if (FP_OFF(dhp->dh_next) == DHDR_END)
    {
      KernelAllocPara(FP_SEG(rq.r_endaddr) + (FP_OFF(rq.r_endaddr) + 15)/16
                      - FP_SEG(dhp), 'D', name, mode);
    }

    /* Another fix for multisegmented device drivers:                  */
    /*   To help emulate the functionallity experienced with other DOS */
    /*   operating systems when calling multiple device drivers in a   */
    /*   single driver file, save the end address returned from the    */
    /*   last INIT call which will then be passed as the end address   */
    /*   for the next INIT call.                                       */

    *r_top = (char FAR *)rq.r_endaddr;
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
    device = device->dh_next;
  }
  while (FP_OFF(device) != DHDR_END);
}

static void init_PSPSet(u16 psp) {
    CPU_AH = 0x50; // Set Current PSP
    CPU_BX = psp;
    fdos_21h(cpu);
}

STATIC void init_kernel()
{
    COUNT i;

    LoL->os_setver_major = LoL->os_major = MAJOR_RELEASE;
    LoL->os_setver_minor = LoL->os_minor = MINOR_RELEASE;

    /* Init oem hook - returns memory size in KB, just read BDA */
    ram_top = pload16(0x413);

    /* no resident DOS in guest RAM, so top is just below video RAM */
    lpTop = X86_RAM_BASE + ((uint32_t)ram_top << 10) - 16;

    /* Initialize IO subsystem                                      */
    InitIO();

    // no printers and COM-ports for now
//  InitPrinters();
//  InitSerialPorts();

    init_PSPSet(DOS_PSP);
/// TODO: next point to migrate:
/*
  set_DTA(MK_FP(DOS_PSP, 0x80));
  PSPInit();

  Init_clk_driver();
*/
  /* Do first initialization of system variable buffers so that   */
  /* we can read config.sys later.  */

  /* use largest possible value for the initial CDS */
  LoL->lastdrive = 26;

  /*  init_device((struct dhdr FAR *)&blk_dev, NULL, 0, &ram_top); */
  /*  WARNING: dsk_init() must be called prior to update_dcb() to ensure
      _Dyn (start of Dynamic memory block) is the start of drive data table (see getddt() in dsk.c)
   */
///  blk_dev.dh_name[0] = dsk_init();

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

    // stop it before complete impl.
    while(1);
}
