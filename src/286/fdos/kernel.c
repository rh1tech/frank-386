#include "../cpu.h"
#include "../bios.h"
#include "../fdos.h"

#define FAR
#define far
#define GLOBAL extern
#define ASM
#define WITHFAT32 1
#define WIN31SUPPORT 1
#include "hdr/kconfig.h"
#include "hdr/portab.h"
#include "hdr/lol.h"

BYTE HaltCpuWhileIdle = 0;

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

static void GenStrategy(void) {
    /*
     * DOS device-driver strategy entry.
     *
     * Original FreeDOS io.asm receives ES:BX -> request header and stores it
     * into global _ReqPktPtr. The following interrupt entry then reads the
     * same request packet and executes the requested command.
     *
     * Native RP2350 variant should preserve the same semantic object:
     * "current DOS device request packet". It does not have to be a real
     * x86 far pointer, but all device interrupt handlers below must be able
     * to access the request prepared here.
     */
}

static void ConIntr(void) {
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
}

static void PrnIntr(void) {
    /*
     * PRN pseudo-device interrupt entry.
     *
     * In FreeDOS this is a generic printer device that can be redirected
     * to selected LPT/COM targets by MODE. Native implementation can begin
     * as "not ready" or route output to the configured printer/log backend.
     */
}

static void AuxIntr(void) {
    /*
     * AUX / COM1-style serial pseudo-device interrupt entry.
     *
     * Used for AUX and COM1 in io.asm. Native implementation should map
     * character read/write/status requests to the configured serial backend,
     * or return a DOS device error if serial is not available.
     */
}

static void Lpt1Intr(void) {
    /*
     * LPT1 printer device interrupt entry.
     *
     * Native implementation should handle printer output/status for LPT1.
     * Minimal safe behaviour: report not ready / command error for unsupported
     * operations while still completing init/status requests consistently.
     */
}

static void Lpt2Intr(void) {
    /*
     * LPT2 printer device interrupt entry.
     *
     * Same semantics as LPT1, but for logical printer unit 2.
     */
}

static void Lpt3Intr(void) {
    /*
     * LPT3 printer device interrupt entry.
     *
     * Same semantics as LPT1, but for logical printer unit 3.
     */
}

static void Com2Intr(void) {
    /*
     * COM2 serial device interrupt entry.
     *
     * Same character-device request model as AUX/COM1, mapped to logical
     * serial unit 2.
     */
}

static void Com3Intr(void) {
    /*
     * COM3 serial device interrupt entry.
     *
     * Same character-device request model as AUX/COM1, mapped to logical
     * serial unit 3.
     */
}

static void Com4Intr(void) {
    /*
     * COM4 serial device interrupt entry.
     *
     * Same character-device request model as AUX/COM1, mapped to logical
     * serial unit 4.
     */
}

static void ClkEntry(void) {
    /*
     * CLOCK$ device interrupt entry.
     *
     * Handles DOS clock-device requests. Native implementation should bridge
     * date/time operations to the emulator BIOS time source / RTC layer and
     * fill the request packet using DOS CLOCK$ transfer format.
     */
}

static void BlkEntry(void) {
    /*
     * Internal block-device interrupt entry.
     *
     * Handles block reads/writes/ioctl/media checks for DOS logical drives.
     * Native implementation should call the RP2350-side FAT/disk backend
     * instead of executing x86 INT 13h-heavy FreeDOS block driver code.
     */
}

static void NulIntr(void) {
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
}

static struct dhdr blk_dev = {
    .dh_next = (struct dhdr far *)-1,
    .dh_attr = 0x08c2,
    .dh_strategy = GenStrategy,
    .dh_interrupt = BlkEntry,
    .dh_name = { 4, 0, 0, 0, 0, 0, 0, 0 }
};

static struct dhdr clk_dev = {
    .dh_next = &blk_dev,
    .dh_attr = 0x8008,
    .dh_strategy = GenStrategy,
    .dh_interrupt = ClkEntry,
    .dh_name = "CLOCK$  "
};

static struct dhdr com4_dev = {
    .dh_next = &clk_dev,
    .dh_attr = 0x8000,
    .dh_strategy = GenStrategy,
    .dh_interrupt = Com4Intr,
    .dh_name = "COM4    "
};

static struct dhdr com3_dev = {
    .dh_next = &com4_dev,
    .dh_attr = 0x8000,
    .dh_strategy = GenStrategy,
    .dh_interrupt = Com3Intr,
    .dh_name = "COM3    "
};

static struct dhdr com2_dev = {
    .dh_next = &com3_dev,
    .dh_attr = 0x8000,
    .dh_strategy = GenStrategy,
    .dh_interrupt = Com2Intr,
    .dh_name = "COM2    "
};

static struct dhdr com1_dev = {
    .dh_next = &com2_dev,
    .dh_attr = 0x8000,
    .dh_strategy = GenStrategy,
    .dh_interrupt = AuxIntr,
    .dh_name = "COM1    "
};

static struct dhdr lpt3_dev = {
    .dh_next = &com1_dev,
    .dh_attr = 0xA040,
    .dh_strategy = GenStrategy,
    .dh_interrupt = Lpt3Intr,
    .dh_name = "LPT3    "
};

static struct dhdr lpt2_dev = {
    .dh_next = &lpt3_dev,
    .dh_attr = 0xA040,
    .dh_strategy = GenStrategy,
    .dh_interrupt = Lpt2Intr,
    .dh_name = "LPT2    "
};

static struct dhdr lpt1_dev = {
    .dh_next = &lpt2_dev,
    .dh_attr = 0xA040,
    .dh_strategy = GenStrategy,
    .dh_interrupt = Lpt1Intr,
    .dh_name = "LPT1    "
};

static struct dhdr aux_dev = {
    .dh_next = &lpt1_dev,
    .dh_attr = 0x8000,
    .dh_strategy = GenStrategy,
    .dh_interrupt = AuxIntr,
    .dh_name = "AUX     "
};

static struct dhdr prn_dev = {
    .dh_next = &aux_dev,
    .dh_attr = 0xA040,
    .dh_strategy = GenStrategy,
    .dh_interrupt = PrnIntr,
    .dh_name = "PRN     "
};

static struct dhdr con_dev = {
    .dh_next = &prn_dev,
    .dh_attr = 0x8013,
    .dh_strategy = GenStrategy,
    .dh_interrupt = ConIntr,
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
        .dh_attr = 0x8004,
        .dh_strategy = GenStrategy,
        .dh_interrupt = NulIntr,
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
    .os_release      = "os_release" /* near ptr на строку os_release */
};

void dos_puts(CPU* cpu, const char* str) {
    while(*str) {
        CPU_AL = *str;
        fdos_29h(cpu);
        str++;
    }
}

void kernel(CPU* cpu) {
    struct lol* LoL = &lol;
    // adjust boot drive to DOS format
    LoL->BootDrive  = CPU_BL + 1;
    if (LoL->BootDrive > 0x80) {
        LoL->BootDrive = 3;
    }

#ifndef QUIET
    CPU_AX = 0x0e31; // '1' Tracecode - kernel entered
    CPU_BX = 0x00f0;
    bios_10h(cpu);
#endif

    LoL->cpu = cpu->gen;

    /* install DOS API and other interrupt service routines, basic kernel functionality works */
//    setup_int_vectors();
    HaltCpuWhileIdle = 0;
    // TODO: INT 30h как far jump на CP/M entry
// CheckContinueBootFromHarddisk
    /* display copyright info and kernel emulation status */
//    signon();
    dos_puts(cpu, "\nRP2350 FreeDOS kernel\n\n");

    // stop it before complete impl.
    while(1);
}
