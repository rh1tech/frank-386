#include <pico.h>
#include <pico/time.h>
#include <hardware/pio.h>
#include <ctype.h>
#include "286/cpu.h"
#include "bios/bios.h"
#include "fdos.h"
#include "i8254.h"

#define printf(...) dos_printf(__VA_ARGS__)
CPU* cpu;

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

#include "hdrs.h"

BYTE HaltCpuWhileIdle = 0;
UWORD ram_top = 0;
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
/*256*/const dos_far_ptr x86_szLine = x86_SZ_LINE; // _BSS + MAX_SEC_SIZE
/* 16*/const dos_far_ptr x86_dap = x86_DAP;
/*128*/const dos_far_ptr x86_master_env = x86_MASTER_ENV;

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
BYTE DOSFAR ASM break_ena = 1;  /* break enabled flag                   */
unsigned char DOSTEXTFAR ASM kbdType = 0x10; // 00 for 84key, 10h for 102key
const dos_far_ptr _nlsPackageHardcoded = x86_nlsPackageHardcoded;

static const UBYTE nls_upcase_hardcoded_init[] = {
    0x80, 0x00, 0x80, 0x9a, 0x45, 0x41, 0x8e, 0x41, 0x8f, 0x80, 0x45, 0x45,
    0x45, 0x49, 0x49, 0x49, 0x8e, 0x8f, 0x90, 0x92, 0x92, 0x4f, 0x99, 0x4f,
    0x55, 0x55, 0x59, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0x41, 0x49,
    0x4f, 0x55, 0xa5, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad,
    0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9,
    0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd,
    0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5,
    0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

static const UBYTE nls_fupcase_hardcoded_init[] = {
    0x80, 0x00, 0x80, 0x9a, 0x45, 0x41, 0x8e, 0x41, 0x8f, 0x80, 0x45, 0x45,
    0x45, 0x49, 0x49, 0x49, 0x8e, 0x8f, 0x90, 0x92, 0x92, 0x4f, 0x99, 0x4f,
    0x55, 0x55, 0x59, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0x41, 0x49,
    0x4f, 0x55, 0xa5, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad,
    0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9,
    0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd,
    0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5,
    0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

static const UBYTE nls_fname_term_hardcoded_init[] = {
    0x16, 0x00, 0x8e, 0x00, 0xff, 0x41, 0x00, 0x20, 0xee, 0x0e, 0x2e, 0x22,
    0x2f, 0x5c, 0x5b, 0x5d, 0x3a, 0x7c, 0x3c, 0x3e, 0x2b, 0x3d, 0x3b, 0x2c,
};

static const UBYTE nls_coll_hardcoded_init[] = {
    0x00, 0x01, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21,
    0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
    0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51,
    0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d,
    0x5e, 0x5f, 0x60, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x5a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x43, 0x55,
    0x45, 0x41, 0x41, 0x41, 0x41, 0x43, 0x45, 0x45, 0x45, 0x49, 0x49, 0x49,
    0x41, 0x41, 0x45, 0x41, 0x41, 0x4f, 0x4f, 0x4f, 0x55, 0x55, 0x59, 0x4f,
    0x55, 0x24, 0x24, 0x24, 0x24, 0x24, 0x41, 0x49, 0x4f, 0x55, 0x4e, 0x4e,
    0xa6, 0xa7, 0x3f, 0xa9, 0xaa, 0xab, 0xac, 0x21, 0x22, 0x22, 0xb0, 0xb1,
    0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd,
    0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5,
    0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0x53,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed,
    0xee, 0xef, 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

static const UBYTE nls_dbcs_hardcoded_init[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
 
static dos_far_ptr nls_hc_ptr(UWORD off)
{
    return MK_FP(FP_SEG(x86_nlsPackageHardcoded), FP_OFF(x86_nlsPackageHardcoded) + off);
}

static void init_nls_hardcoded(void)
{
    struct nlsPackage *pkg = (struct nlsPackage *)ARM_PTR(x86_nlsPackageHardcoded);
    struct nlsInfoBlock *info = (struct nlsInfoBlock *)ARM_PTR(x86_nlsInfo);
    UWORD off_table2 = sizeof(struct nlsPackage);
    UWORD off_table4 = off_table2 + sizeof(nls_upcase_hardcoded_init);
    UWORD off_table5 = off_table4 + sizeof(nls_fupcase_hardcoded_init);
    UWORD off_table6 = off_table5 + sizeof(nls_fname_term_hardcoded_init);
    UWORD off_table7 = off_table6 + sizeof(nls_coll_hardcoded_init);

    memset(pkg, 0, sizeof(*pkg));

    pkg->nxt = NULL;
    pkg->cntry = 1;
    pkg->cp = 437;
    pkg->flags = NLS_FLAG_HARDCODED;
    pkg->yeschar = 'Y';
    pkg->nochar = 'N';
    pkg->numSubfct = 6;

    struct nlsPointer *ptrs = pkg->nlsPointers;
    ptrs[0].subfct = 2;
    ptrs[0].pointer = ARM_PTR(nls_hc_ptr(off_table2));
    ptrs[1].subfct = 4;
    ptrs[1].pointer = ARM_PTR(nls_hc_ptr(off_table4));
    ptrs[2].subfct = 5;
    ptrs[2].pointer = ARM_PTR(nls_hc_ptr(off_table5));
    ptrs[3].subfct = 6;
    ptrs[3].pointer = ARM_PTR(nls_hc_ptr(off_table6));
    ptrs[4].subfct = 7;
    ptrs[4].pointer = ARM_PTR(nls_hc_ptr(off_table7));
    ptrs[5].subfct = 1;
    ptrs[5].pointer = &pkg->nlsExt;

    pkg->nlsExt.subfct = 1;
    pkg->nlsExt.size = 0x001c;
    pkg->nlsExt.countryCode = 1;
    pkg->nlsExt.codePage = 437;
    pkg->nlsExt.dateFmt = 0;
    memcpy(pkg->nlsExt.curr, "$", 2);
    memcpy(pkg->nlsExt.thSep, ",", 2);
    memcpy(pkg->nlsExt.point, ".", 2);
    memcpy(pkg->nlsExt.dateSep, "-", 2);
    memcpy(pkg->nlsExt.timeSep, ":", 2);
    pkg->nlsExt.currFmt = 0;
    pkg->nlsExt.prescision = 2;
    pkg->nlsExt.timeFmt = 0;
    pkg->nlsExt.upCaseFct = CharMapSrvc;
    memcpy(pkg->nlsExt.dataSep, ",", 2);

    memcpy(ARM_PTR(nls_hc_ptr(off_table2)), nls_upcase_hardcoded_init, sizeof(nls_upcase_hardcoded_init));
    memcpy(ARM_PTR(nls_hc_ptr(off_table4)), nls_fupcase_hardcoded_init, sizeof(nls_fupcase_hardcoded_init));
    memcpy(ARM_PTR(nls_hc_ptr(off_table5)), nls_fname_term_hardcoded_init, sizeof(nls_fname_term_hardcoded_init));
    memcpy(ARM_PTR(nls_hc_ptr(off_table6)), nls_coll_hardcoded_init, sizeof(nls_coll_hardcoded_init));
    memcpy(ARM_PTR(nls_hc_ptr(off_table7)), nls_dbcs_hardcoded_init, sizeof(nls_dbcs_hardcoded_init));

    memset(info, 0, sizeof(*info));
    info->fname = NULL;
    info->sysCodePage = 437;
    info->flags = NLS_CODE_REORDER_POINTERS;
    info->actPkg = pkg;
    info->chain = pkg;
}

KernelConfig InitKernelConfig = {
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

ULONG lseek(int fd, long position)
{
    CPU_BX = (UWORD)fd;
    CPU_CX = (UWORD)((ULONG)position >> 16);
    CPU_DX = (UWORD)((ULONG)position & 0xffff);
    CPU_AX = 0x4200;   /* origin = start of file */
    bios_intcall(cpu, 0x21);
    if (cf)
        return (ULONG)-1;
    return ((ULONG)CPU_DX << 16) | CPU_AX;
}

void keycheck(void)
{
    CPU_AH = 0x01;
    bios_intcall(cpu, 0x16);
}

static void ConIntr(request FAR *rq) {
    CPU_regs saved;
    switch (rq->r_command) {
    case C_INIT:
        rq_done(rq);
        break;
    case C_IFLUSH:
        /* drain the BIOS keyboard buffer */
        cpu_save_regs(cpu, &saved);
        while (1) {
            CPU_AH = 0x01;          /* INT 16h: check keystroke */
            bios_intcall(cpu, 0x16);
            if (zf) break;          /* ZF=1: buffer empty */
            CPU_AH = 0x00;          /* INT 16h: read and discard */
            bios_intcall(cpu, 0x16);
        }
        cpu_restore_regs(cpu, &saved);
        rq_done(rq);
        break;

    case C_NDREAD:
        /* non-destructive peek: S_BUSY if no key, else set r_ndbyte */
        cpu_save_regs(cpu, &saved);
        CPU_AH = 0x01;              /* INT 16h AH=01h: check keystroke */
        bios_intcall(cpu, 0x16);
        if (zf) {
            /* no key in buffer */
            rq->r_status = S_DONE | S_BUSY;
        } else {
            rq->r_ndbyte = CPU_AL;
            rq_done(rq);
        }
        cpu_restore_regs(cpu, &saved);
        break;

    case C_ISTAT:
        /* input status: S_BUSY if no key waiting */
        cpu_save_regs(cpu, &saved);
        CPU_AH = 0x01;
        bios_intcall(cpu, 0x16);
        rq->r_status = zf ? (S_DONE | S_BUSY) : S_DONE;
        cpu_restore_regs(cpu, &saved);
        break;

    case C_INPUT:
        /* blocking read: wait until a key is available */
        cpu_save_regs(cpu, &saved);
        CPU_AH = 0x00;              /* INT 16h AH=00h: read keystroke */
        bios_intcall(cpu, 0x16);    /* returns false (re-enters) until key ready */
        if (rq->r_count > 0 && rq->r_trans) {
            BYTE FAR *p = rq->r_trans;
            *p = CPU_AL;
            rq->r_count = 1;
        } else {
            rq->r_count = 0;
        }
        cpu_restore_regs(cpu, &saved);
        rq_done(rq);
        break;

    case C_OUTPUT:
    case C_OUTVFY:
        /* teletype output via INT 10h AH=0Eh */
        cpu_save_regs(cpu, &saved);
        {
            BYTE FAR *p   = rq->r_trans;
            UWORD     cnt = rq->r_count;
            while (cnt--) {
                CPU_AH = 0x0E;
                CPU_AL = *p++;
                CPU_BX = 0x0007;    /* page 0, attribute 7 */
                bios_intcall(cpu, 0x10);
            }
        }
        cpu_restore_regs(cpu, &saved);
        rq_done(rq);
        break;

    default:
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
    case C_MEDIACHK:
    case C_BLDBPB:
        blockio(cpu, rq);
        break;
    default:
        printf("WARN: BlkEntry unimplemented cmd=%02X unit=%u status_before=%04X\n",  rq->r_command, rq->r_unit, rq->r_status);
        /// TODO: C_IOCTLIN / C_IOCTLOUT / C_GENIOCTL
        /// are not implemented yet - not required for DosOpen() on a fixed,
        /// never-removed disk image.
    case C_INIT:
        /* disk init is done, so this should never be called */
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

/* pc_step()/pc are declared the same way bios/bios_intcall.c declares
   them (there is no shared header for it) - see that file for the
   INT-call counterpart of the mechanism used below. */
extern struct PC* pc;
void pc_step(struct PC* pc);

/*
   cpu_far_call_waiter() - bios_callback_params_t callback fired when
   the CPU reaches back the synthetic return address pushed by
   cpu_far_call() below (i.e. the driver did RETF). Just like
   bios_intcall.c's own waiter, it marks the wait done and drops the
   callback; unlike an INT/IRET pair, a CALL/RETF pair never expects a
   flags-restoring IRET at the trap address, so this always returns
   false ("do not synthesize an IRET here").
*/
static bool cpu_far_call_waiter(CPU* cpu, bios_callback_params_t* params) {
    if (!params->done) {
        params->done = true;
    }
    return false;
}

/*
   cpu_far_call(cpu, seg, off) - synchronously CALL FAR seg:off in
   guest (x86) code and wait for it to RETF back, by hand: push a
   return CS:IP that lands inside the emulator's fake-BIOS trap page
   (see bios/bios_FFh.c), set CS:IP to the target, and single-step the
   CPU until that trap fires and reports the RETF happened.

   This is the CALL/RETF analogue of bios_intcall()'s INT/IRET
   mechanism (bios/bios_intcall.c) - reused here because DOS device
   driver entry points (dh_strategy/dh_interrupt) are invoked with a
   plain far call, not a software interrupt: no flags are pushed by
   the caller, and none are expected to be popped by the callee.
*/
void cpu_far_call(CPU* cpu, UWORD seg, UWORD off)
{
  UWORD save_cs = CPU_CS, save_ip = CPU_IP;
  bios_callback_params_t params = {
    .callback = cpu_far_call_waiter,
    .expected_cs = 0xFFEF,
    .expected_ip = 0x000F,
    .done = false,
  };

  set_bios_callback(cpu, &params, true);

  /* Emulate exactly what "CALL FAR seg:off" pushes: CS, then IP (so
     that RETF - which pops IP, then CS - lands back here). */
  CPU_SP -= 2;
  writew86(((uint32_t)CPU_SS << 4) + CPU_SP, params.expected_cs);
  CPU_SP -= 2;
  writew86(((uint32_t)CPU_SS << 4) + CPU_SP, params.expected_ip);

  SET_CS(seg);
  SET_IP(off);

  while (!params.done)
    pc_step(pc);

  drop_bios_callback(cpu, &params);
  SET_CS(save_cs);
  SET_IP(save_ip);
}

/*
   x86_execrh() - drive the standard MS-DOS/FreeDOS device driver
   protocol for a loaded (real x86 machine code) driver:
     1. ES:BX = far pointer to the request header, CALL FAR
        dh_strategy - the driver stashes the pointer and returns.
     2. CALL FAR dh_interrupt (no arguments) - the driver re-fetches
        the pointer it stashed in step 1 and actually processes
        rq->r_command.

   dh_strategy/dh_interrupt are word *offsets* within the driver
   header's own segment - see the comment on struct dhdr's x86 member
   in hdr/device.h for why (that's also what keeps this layout
   byte-for-byte compatible with real, unmodified .SYS driver files).
*/
static void x86_execrh(/*request*/ dos_far_ptr x86_rq, struct dhdr *dhp, dos_far_ptr x86_dhp) {
  UWORD hdr_seg = FP_SEG(x86_dhp);
  SET_ES ( FP_SEG(x86_rq) );
  CPU_BX = FP_OFF(x86_rq);
  cpu_far_call(cpu, hdr_seg, dhp->x86.dh_strategy);
  cpu_far_call(cpu, hdr_seg, dhp->x86.dh_interrupt);
}

/// TODO: dos_far_ptr rq
WORD ASMPASCAL execrh(request* rq, /*struct dhdr*/ dos_far_ptr _dhp) {
  struct dhdr* dhp = (struct dhdr*)ARM_PTR(_dhp);
  if (dhp->dh_attr & ATTR_NATIVE) {
      dhp->arm.dh_interrupt(rq);
  } else {
    x86_execrh(linear_to_far(rq), dhp, _dhp);
  }
  return rq->r_status;
}

/* check for a block device and update  device control block    */
STATIC VOID update_dcb(/*struct dhdr*/ dos_far_ptr x86_dhp)
{
  /*
  printf("before update_dcb: first_mcb=%04X nblkdev=%u DPBp=%04X:%04X\n",
       LoL->first_mcb, LoL->nblkdev,
       FP_SEG(LoL->DPBp), FP_OFF(LoL->DPBp));  
  */
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
///printf("DBG update_dcb alloc nunits=%u x86_dpb=%04X:%04X native=%p\n", nunits, FP_SEG(x86_dpb), FP_OFF(x86_dpb), ARM_PTR(x86_dpb));

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
        !far_is_end(tmp_dpb->dpb_next);
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
    /* memory allocated as array, so next is just next element */
    dpb->dpb_next = (Index + 1 < nunits)
                  ? ADD_OFF(x86_dpb, (Index + 1) * sizeof(struct dpb))
                  : MK_FP(-1, -1);
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
    /*
	  printf("DBG update_dcb unit=%u dpb=%04X:%04X native=%p next=%04X:%04X dev=%04X:%04X flags=%04X\n",
       LoL->nblkdev,
       FP_SEG(ADD_OFF(x86_dpb, Index * sizeof(struct dpb))),
       FP_OFF(ADD_OFF(x86_dpb, Index * sizeof(struct dpb))),
       dpb,
       FP_SEG(dpb->dpb_next), FP_OFF(dpb->dpb_next),
       FP_SEG(dpb->dpb_device), FP_OFF(dpb->dpb_device),
       dpb->dpb_flags);
    */
    ++dpb;  /* dbp = dbp->dpb_next; */
    ++LoL->nblkdev;
  }

  /* printf("processed %i nunits\n", nunits); */
}

/* If cmdLine is NULL, this is an internal driver */

BOOL init_device(/*struct dhdr*/ dos_far_ptr x86_dhp, char *cmdLine, COUNT mode,
                 dos_far_ptr * r_top)
{
  struct dhdr* dhp = (struct dhdr*)ARM_PTR(x86_dhp);
  CPU_SP -= sizeof(request);
  dos_far_ptr x86_rq = MK_FP(CPU_SS, CPU_SP);
  request* rq = (request*)ARM_PTR(x86_rq);
  memset(rq, 0, sizeof(request));  
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

  rq->r_unit = 0;
  rq->r_status = 0;
  rq->r_command = C_INIT;
  rq->r_length = sizeof(request);
  rq->r_endaddr = *r_top;
  rq->r_bpbptr = (void FAR *)(cmdLine ? cmdLine : "\n");
  rq->r_firstunit = LoL->nblkdev;

  execrh(rq, x86_dhp);

/*
 *  Added needed Error handle
 */
  if ((rq->r_status & (S_ERROR | S_DONE)) == S_ERROR)
      goto ok;

  if (cmdLine)
  {
    /* Don't link in device drivers which do not take up memory */
    if ((struct dhdr*)ARM_PTR(rq->r_endaddr) == dhp)
      goto ok;

    /* Don't link in block device drivers which indicate no units */
    if (!(dhp->dh_attr & ATTR_CHAR) && !rq->r_nunits)
    {
      rq->r_endaddr = x86_dhp;
      goto ok;
    }


    /* Fix for multisegmented device drivers:                          */
    /*   If there are multiple device drivers in a single driver file, */
    /*   only the END ADDRESS returned by the last INIT call should be */
    /*   the used.  It is recommended that all the device drivers in   */
    /*   the file return the same address                              */
    if (FP_OFF(dhp->dh_next) == 0xffff) {
        KernelAllocPara(FP_SEG(rq->r_endaddr) + (FP_OFF(rq->r_endaddr) + 15)/16 - FP_SEG(x86_dhp), 'D', name, mode);
    }

    /* Another fix for multisegmented device drivers:                  */
    /*   To help emulate the functionallity experienced with other DOS */
    /*   operating systems when calling multiple device drivers in a   */
    /*   single driver file, save the end address returned from the    */
    /*   last INIT call which will then be passed as the end address   */
    /*   for the next INIT call.                                       */
    *r_top = rq->r_endaddr;
  }

  if (!(dhp->dh_attr & ATTR_CHAR) && (rq->r_nunits != 0))
  {
    dhp->dh_name[0] = rq->r_nunits;
    update_dcb(x86_dhp);
  }

  if (dhp->dh_attr & ATTR_CONIN)
    LoL->syscon = x86_dhp;
  else if (dhp->dh_attr & ATTR_CLOCK)
    LoL->clock = x86_dhp;

  CPU_SP += sizeof(request);
  return FALSE;
ok:
  CPU_SP += sizeof(request);
  return TRUE;
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
    bios_intcall(cpu, 0x21);
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
dos_far_ptr linear_to_far(const void *p)
{
  if (!is_guest_ptr(p))
  {
    printf("PANIC: linear_to_far out of x86 guest RAM range %p\n", (const void *)p);
    for (;;) ;
  }
  uint32_t lin = (uint32_t)(p - (intptr_t)X86_RAM_BASE);
  return MK_FP((UWORD)(lin >> 4), (UWORD)(lin & 0xF));
}

int init_setdrive(int drive) {
    CPU_AH = 0x0e;
    CPU_DX = drive;
    bios_intcall(cpu, 0x21);
    return CPU_AL;          /* number of potentially valid drives */
}

int init_DosOpen(dos_far_ptr pathname, int flags) {
    SET_DS (FP_SEG(pathname));
    CPU_DX = FP_OFF(pathname);
    CPU_AL = flags & 0xff;
    CPU_AH = 0x3d;          /* DOS open */
    bios_intcall(cpu, 0x21);
    return cf ? -1 : CPU_AX;          /* file handle */
}

int dup2(int oldfd, int newfd)
{
    CPU_AH = 0x46;      /* Force duplicate file handle */
    CPU_BX = oldfd;
    CPU_CX = newfd;
    bios_intcall(cpu, 0x21);
    return cf ? -1 : CPU_AX;
}

int read(int fd, dos_far_ptr dst, COUNT sz) {
    CPU_AH = 0x3F;
    CPU_BX = fd;
    CPU_CX = sz;
    CPU_DX = dst.offset;
    SET_DS ( dst.segment );
    bios_intcall(cpu, 0x21);
    return cf ? -1 : CPU_AX;
}

int close(int fd) {
    CPU_AH = 0x3E;
    CPU_BX = fd;
    bios_intcall(cpu, 0x21);
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
#define TNDBG(fmt, ...) 
///printf("[truename] " fmt "\n", ##__VA_ARGS__)

#define TNPTR(p)  ((unsigned)((const char *)(p) - srcbuf))
#define TNDPTR(p) ((unsigned)((const char *)(p) - dest))

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

  TNDBG("TN00 enter x86_src=%04X:%04X guest=%p mode=%04X raw='%s'",
        FP_SEG(x86_src), FP_OFF(x86_src), ARM_PTR(x86_src), mode,
        (const char *)ARM_PTR(x86_src));

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
  TNDBG("TN01 copied srcbuf='%s'", srcbuf);

  src0 = src[0];
  if (src0 == '\0') {
    TNDBG("TN02 empty -> DE_FILENOTFND");
    return DE_FILENOTFND;
  }

  if (src0 == '\\' && src[1] == '\\') {
    const char *unc_src = src;
    TNDBG("TN03 UNC start src='%s'", src);

    do {
      src0 = unc_src[0];
      addChar(src0);
      unc_src++;
    } while (src0);

    internal_data->current_ldt = MK_FP(0xFFFF, 0xFFFF);
    TNDBG("TN04 UNC return dest='%s'", dest);
    return IS_NETWORK;
  }

  if (src[1] == ':')
    result = drLetterToNr(DosUpFChar(src0));
  else
    result = internal_data->default_drive;

  TNDBG("TN05 drive result=%u default=%u src='%s'",
        result, internal_data->default_drive, src);

  dhp = IsDevice(src);
  TNDBG("TN06 IsDevice=%p src='%s'", dhp, src);

  x86_cdsEntry = get_cds(result);
  cdsEntry = far_is_null(x86_cdsEntry) ? NULL : (struct cds *)ARM_PTR(x86_cdsEntry);

  TNDBG("TN07 get_cds(%u)=%04X:%04X native=%p lastdrive=%u",
        result, FP_SEG(x86_cdsEntry), FP_OFF(x86_cdsEntry),
        cdsEntry, LoL->lastdrive);

  if (cdsEntry == NULL)
  {
    if (dhp && (mode & CDS_MODE_CHECK_DEV_PATH) && (result >= LoL->lastdrive))
    {
      const char *s = src + 2;
      char c = *s;

      TNDBG("TN08 invalid drive but device path candidate src='%s'", src);

      if (c != '\\' && c != '/')
        c = '\0';

      if (*(src + 3) != '\0')
      {
        s = strchr(src + 3, '\\');
        if (s == NULL)
          s = strchr(src + 3, '/');
      }
      else
      {
        s = NULL;
      }

      TNDBG("TN09 dev invalid-drive check c=%02X s_ofs=%d",
            (unsigned char)c, s ? (int)(s - src) : -1);

      if (c == '\0')
      {
        if (s != NULL)
          goto invalid_path;
      }
      else
      {
        if (s == NULL) goto invalid_path;
        if (s != src + 6) goto invalid_path;
        if (memcmp(src + 3, "DEV", 3) != 0) goto invalid_path;
        s = strchr(src + 7, '\\');
        if (s == NULL)
          s = strchr(src + 7, '/');
        if (s != NULL)
          goto invalid_path;
      }

      result = internal_data->default_drive;
      x86_cdsEntry = get_cds(result);
      cdsEntry = far_is_null(x86_cdsEntry) ? NULL : (struct cds *)ARM_PTR(x86_cdsEntry);

      TNDBG("TN10 fallback default drive result=%u cds=%04X:%04X native=%p",
            result, FP_SEG(x86_cdsEntry), FP_OFF(x86_cdsEntry), cdsEntry);

      if (cdsEntry == NULL)
        goto invalid_path;
    }
    else
    {
invalid_path:
      TNDBG("TN11 invalid path result=%u src='%s'", result, src);
      return DE_PATHNOTFND;
    }
  }

  memcpy(&TempCDS, cdsEntry, sizeof(TempCDS));
  TNDBG("TN12 CDS path='%s' flags=%04X dpb=%04X:%04X backslash=%u join=%u",
        TempCDS.cdsCurrentPath, TempCDS.cdsFlags,
        FP_SEG(TempCDS.cdsDpb), FP_OFF(TempCDS.cdsDpb),
        TempCDS.cdsBackslashOffset, TempCDS.cdsJoinOffset);

  internal_data->current_ldt = x86_cdsEntry;

  if (TempCDS.cdsFlags & CDSNETWDRV)
    result |= IS_NETWORK;

  if (dhp)
    result |= IS_DEVICE;

  TNDBG("TN13 before QRemote mode=%04X result=%04X src='%s'",
        mode, result, src);

  memset(dest, 0, 12);
/* /// TODO:
  if (!(mode & CDS_MODE_SKIP_PHYSICAL) &&
      QRemote_Fn(dest, src) == SUCCESS && dest[0] != '\0')
  {
    TNDBG("TN14 QRemote success dest='%s' result=%04X", dest, result);

    if (dest[2] == '/' && (result & IS_DEVICE))
      result &= ~IS_NETWORK;
    else
      result |= IS_NETWORK;

    TNDBG("TN15 QRemote return result=%04X dest='%s'", result, dest);
    return result;
  }
*/
  dest[0] = drNrToLetter(result & 0x1f);
  dest[1] = ':';

  TNDBG("TN16 local mapper dest='%c%c' result=%04X",
        dest[0], dest[1], result);

  if (src[1] == ':')
    src += 2;

  TNDBG("TN17 after drive skip src_ofs=%u src='%s'", TNPTR(src), src);

  dest[2] = '\\';

  if (result & IS_DEVICE)
  {
    TNDBG("TN18 device path src='%s'", src);

    froot = get_root(src);
    TNDBG("TN19 get_root froot_ofs=%d",
          froot ? (int)(froot - src) : -1);

    if (froot == src || froot == src + 5)
    {
      if (froot == src + 5)
      {
        memcpy(dest + 3, src, 5);
        DosUpMem(dest + 3, 5);
        if (dest[3] == '/') dest[3] = '\\';
        if (dest[7] == '/') dest[7] = '\\';

        TNDBG("TN20 device copied prefix dest='%s'", dest);
      }

      if (froot == src || memcmp(dest + 3, "\\DEV\\", 5) == 0)
      {
        dest[2] = '/';
        result &= ~IS_NETWORK;
        src = (char *)froot;

        TNDBG("TN21 device direct dest='%s' src_ofs=%u result=%04X",
              dest, TNPTR(src), result);
      }
    }
  }

  rootPos = p = dest + 2;
  TNDBG("TN22 fullqual start dest0='%c%c%c' p=%u root=%u",
        dest[0], dest[1], dest[2], TNDPTR(p), TNDPTR(rootPos));

  if (*p != '/')
  {
    BYTE *cp;

    cp = TempCDS.cdsCurrentPath;
    cp[MAX_CDSPATH - 1] = '\0';

    TNDBG("TN23 CDS current cp='%s' flags=%04X", cp, TempCDS.cdsFlags);

    if ((TempCDS.cdsFlags & CDSNETWDRV) == 0)
    {
      struct dpb *native_dpb = (struct dpb *)ARM_PTR(TempCDS.cdsDpb);

      TNDBG("TN24 before media_check dpb=%04X:%04X native=%p cp='%s'",
            FP_SEG(TempCDS.cdsDpb), FP_OFF(TempCDS.cdsDpb),
            native_dpb, cp);

      int mc = media_check(native_dpb);

      TNDBG("TN25 after media_check rc=%d", mc);

      if (mc < 0) {
        TNDBG("TN26 media_check failed -> DE_PATHNOTFND");
        return DE_PATHNOTFND;
      }

      TNDBG("TN27 before dos_cd cp='%s'", cp);

      if (dos_cd((char *)cp) != SUCCESS) {
        TNDBG("TN28 dos_cd failed cp='%s' backslash=%u",
              cp, TempCDS.cdsBackslashOffset);

        cp[TempCDS.cdsBackslashOffset + 1] =
          cdsEntry->cdsCurrentPath[TempCDS.cdsBackslashOffset + 1] = '\0';

        TNDBG("TN29 retry dos_cd cp='%s'", cp);
        dos_cd((char *)cp);
      }

      TNDBG("TN30 after dos_cd cp='%s'", cp);
    }

    if (!(mode & CDS_MODE_SKIP_PHYSICAL))
    {
      TNDBG("TN31 before strcpy dest <- cp='%s'", cp);

      strcpy(dest, (char *)cp);

      TNDBG("TN32 after strcpy dest='%s'", dest);

      if (TempCDS.cdsFlags & CDSSUBST)
      {
        TNDBG("TN33 CDSSUBST dest='%s'", dest);

        if (dest[1] == ':')
        {
          unsigned ii = drLetterToNr(dest[0]);

          TNDBG("TN34 subst real drive ii=%u lastdrive=%u",
                ii, LoL->lastdrive);

          if (ii < LoL->lastdrive)
            result = (result & 0xffe0) | ii;
        }
      }

      rootPos = p = dest + TempCDS.cdsBackslashOffset;

      TNDBG("TN35 after root setup p=%u root=%u backslash=%u dest='%s'",
            TNDPTR(p), TNDPTR(rootPos), TempCDS.cdsBackslashOffset, dest);
    }
    else
    {
      cp += TempCDS.cdsBackslashOffset;

      TNDBG("TN36 skip physical cp='%s'", cp);

      strcpy(p, (char *)cp);

      TNDBG("TN37 after skip physical strcpy dest='%s'", dest);
    }

    if (p[0] == '\0')
      p[1] = p[0];

    p[0] = '\\';

    TNDBG("TN38 after force slash p=%u root=%u dest='%s'",
          TNDPTR(p), TNDPTR(rootPos), dest);

    if (*src != '\\' && *src != '/')
      p += strlen(p);
    else
      src++;

    if (p[-1] == '\\')
      p--;

    TNDBG("TN39 before append src_ofs=%u src='%s' p=%u root_char='%c' dest='%s'",
          TNPTR(src), src, TNDPTR(p), *rootPos, dest);
  }

  state = 0;

  while (*src)
  {
    TNDBG("TN40 loop start src_ofs=%u src='%s' p=%u state=%u dest='%s'",
          TNPTR(src), src, TNDPTR(p), state, dest);

    if (state & PNE_WILDCARD) {
      TNDBG("TN41 wildcard before end -> DE_PATHNOTFND");
      return DE_PATHNOTFND;
    }

    if (p[-1] != *rootPos)
      addChar(*rootPos);

    while (*src == '/' || *src == '\\')
      src++;

    TNDBG("TN42 after sep skip src_ofs=%u src='%s' p=%u",
          TNPTR(src), src, TNDPTR(p));

    if (*src == '.')
    {
      int dots = 1;

      ++src;
      if (*src == '.')
      {
        ++src;
        dots++;
      }

      TNDBG("TN43 dot candidate dots=%d src_ofs=%u next=%02X",
            dots, TNPTR(src), (unsigned char)*src);

      if (*src == '/' || *src == '\\' || *src == '\0')
      {
        --p;

        if (dots == 2)
        {
          TNDBG("TN44 dotdot before strip p=%u root=%u dest='%s'",
                TNDPTR(p), TNDPTR(rootPos), dest);

          while (*--p != '\\')
          {
            if (p <= rootPos) {
              TNDBG("TN45 dotdot beyond root -> DE_PATHNOTFND");
              return DE_PATHNOTFND;
            }
          }

          TNDBG("TN46 dotdot after strip p=%u", TNDPTR(p));
        }

        continue;
      }

      TNDBG("TN47 malformed dot component");
      return PATH_ERROR();
    }

    i = FNAME_SIZE;
    state &= ~PNE_DOT;

    TNDBG("TN48 parse component start src_ofs=%u p=%u",
          TNPTR(src), TNDPTR(p));

    while (*src != '/' && *src != '\\' && *src != '\0')
    {
      char c = *src++;

      TNDBG("TN49 char c=%02X '%c' src_ofs=%u p=%u i=%d state=%u",
            (unsigned char)c,
            (c >= 32 && c < 127) ? c : '.',
            TNPTR(src), TNDPTR(p), i, state);

      if (c == '*')
      {
        c = '?';
        while (i)
        {
          --i;
          addChar(c);
        }

        TNDBG("TN50 wildcard star expanded p=%u i=%d", TNDPTR(p), i);
      }

      if (c == '.')
      {
        if (state & PNE_DOT) {
          TNDBG("TN51 multiple dot");
          return PATH_ERROR();
        }

        if (*src == '/' || *src == '\\' || *src == '\0')
          break;

        state |= PNE_DOT;
        i = FEXT_SIZE + 1;

        TNDBG("TN52 extension dot i=%d state=%u", i, state);
      }
      else if (c == '?')
      {
        state |= PNE_WILDCARD;
      }

      if (i) {
        --i;
        if (!DirChar(c)) {
          TNDBG("TN53 bad dir char c=%02X", (unsigned char)c);
          return PATH_ERROR();
        }
        addChar(c);
      }
    }

    TNDBG("TN54 component done src_ofs=%u p=%u state=%u dest='%s'",
          TNPTR(src), TNDPTR(p), state, dest);
  }

  if (state & PNE_WILDCARD && !(mode & CDS_MODE_ALLOW_WILDCARDS)) {
    TNDBG("TN55 wildcard final not allowed");
    return DE_PATHNOTFND;
  }

  if (p == dest + 2)
  {
    TNDBG("TN56 dest only drive, add slash");
    addChar('\\');
  }

  *p = '\0';

  TNDBG("TN57 before DosUpFString root=%u dest='%s'",
        TNDPTR(rootPos), dest);

  DosUpFString(rootPos);

  TNDBG("TN58 after DosUpFString dest='%s'", dest);

  if (dest[2] != '/' && (!(mode & CDS_MODE_SKIP_PHYSICAL)) && LoL->njoined)
  {
    dos_far_ptr x86_cdsp = LoL->CDSp;
    struct cds *cdsp = (struct cds *)ARM_PTR(x86_cdsp);

    TNDBG("TN59 JOIN scan njoined=%u cdsp=%04X:%04X native=%p",
          LoL->njoined, FP_SEG(x86_cdsp), FP_OFF(x86_cdsp), cdsp);

    for (i = 0; i < LoL->lastdrive; ++i, ++cdsp)
    {
      size_t j = strlen((char *)cdsp->cdsCurrentPath);

      TNDBG("TN60 JOIN i=%u j=%u flags=%04X path='%s'",
            i, (unsigned)j, cdsp->cdsFlags, cdsp->cdsCurrentPath);

      if ((cdsp->cdsFlags & CDSJOINED) &&
          (dest[j] == '\\' || dest[j] == '\0') &&
          memcmp(dest, cdsp->cdsCurrentPath, j) == 0)
      {
        dest[0] = drNrToLetter(i);
        dest[1] = ':';

        if (dest[j] == '\0')
        {
          dest[2] = '\\';
          dest[3] = 0;
        }
        else if (j != 2)
        {
          strcpy(dest + 2, dest + j);
        }

        result = (result & 0xffe0) | i;
        internal_data->current_ldt = x86_FAR_PTR(FP_SEG(LoL->CDSp), cdsp);
        result &= ~IS_NETWORK;

        if (cdsp->cdsFlags & CDSNETWDRV)
          result |= IS_NETWORK;

        TNDBG("TN61 JOIN return result=%04X dest='%s'", result, dest);
        return result;
      }
    }

    TNDBG("TN62 JOIN no match");
  }

  if ((mode & CDS_MODE_CHECK_DEV_PATH) &&
      ((result & (IS_DEVICE | IS_NETWORK)) == IS_DEVICE) &&
      dest[2] != '/')
  {
    TNDBG("TN63 before dir_exists dest='%s'", dest);

    int de = dir_exists(dest);

    TNDBG("TN64 after dir_exists rc=%d", de);

    if (!de)
      return DE_PATHNOTFND;
  }

  if (mode == CDS_MODE_ALLOW_WILDCARDS)
  {
    TNDBG("TN65 allow wildcards final result forced 0");
    result = 0;
  }

  TNDBG("TN66 return result=%04X dest='%s'", result, dest);
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

    if (i < LoL->nblkdev && !far_is_end(x86_dpb))
    {
      pcds_table->cdsDpb = x86_dpb;
      /*
printf("DBG FsConfig cds[%d] path='%s' flags=%04X cdsDpb=%04X:%04X x86_dpb=%04X:%04X dpb_next=%04X:%04X\n",
       i,
       pcds_table->cdsCurrentPath,
       pcds_table->cdsFlags,
       FP_SEG(pcds_table->cdsDpb), FP_OFF(pcds_table->cdsDpb),
       FP_SEG(x86_dpb), FP_OFF(x86_dpb),
       FP_SEG(dpb->dpb_next), FP_OFF(dpb->dpb_next));
*/
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
/*
printf("DBG FsConfig cds[%d] path='%s' flags=%04X dpb=%04X:%04X\n",
       i, pcds_table->cdsCurrentPath, pcds_table->cdsFlags,
       FP_SEG(pcds_table->cdsDpb), FP_OFF(pcds_table->cdsDpb));
*/     
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

/*
    InitializeAllBPBs()
    
    or MakeNortonDiskEditorHappy()

    it has been determined, that FDOS's BPB tables are initialized,
    only when used (like DIR H:).
    at least one known utility (norton DE) seems to access them directly.
    ok, so we access for all drives, that the stuff gets build
*/
static void InitializeAllBPBs(VOID)
{
  int drive, fileno;
  char *path = ARM_PTR(x86_szLine);
  dos_far_ptr x86_path = x86_szLine;
  strcpy(path, "A:-@JUNK@-.TMP");
  for (drive = 'C'; drive < 'A' + LoL->nblkdev; drive++)
  {
    path[0] = drive;
    if ((fileno = open( x86_path, O_RDONLY)) >= 0)
      close(fileno);
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
/*
printf("DBG after PreConfig CDSp=%04X:%04X native=%p lastdrive=%u nblkdev=%u DPBp=%04X:%04X\n",
       FP_SEG(LoL->CDSp), FP_OFF(LoL->CDSp), ARM_PTR(LoL->CDSp),
       LoL->lastdrive, LoL->nblkdev, FP_SEG(LoL->DPBp), FP_OFF(LoL->DPBp));
*/
    /* Number of units */
    if (blk_dev->dh_name[0] > 0) {
        update_dcb(x86_blk_dev);
    }
    /* Now config the temporary file system */
    FsConfig();

    /* Now process CONFIG.SYS     */
    DoConfig(0);
    DoConfig(1);

    /* initialize near data and MCBs */
    PreConfig2();

    /* and process CONFIG.SYS one last time for device drivers */
    DoConfig(2);

    /* Close all (device) files */
    for (i = 0; i < 20; i++)
      close(i);

    /* and do final buffer allocation. */
    PostConfig();

    /* Init the file system one more time     */
    FsConfig();
  
    configDone();

    InitializeAllBPBs();
}

STATIC void prep_shell(CPU* cpu)
{
  CommandTail Cmd;
  char* master_env  = ((char *)ARM_PTR(x86_master_env));
  if (master_env[0] == '\0')   /* some shells panic on empty master env. */
    memcpy(master_env, "PATH=.\0\0\0\0", sizeof("PATH=.\0\0\0\0"));

  /* process 0       */
  /* Execute command.com from the drive we just booted from    */
  memset(Cmd.ctBuffer, 0, sizeof(Cmd.ctBuffer));
  strcpy(Cmd.ctBuffer, Config.cfgInitTail);

  for (Cmd.ctCount = 0; Cmd.ctCount < sizeof(Cmd.ctBuffer); Cmd.ctCount++)
    if (Cmd.ctBuffer[Cmd.ctCount] == '\r')
      break;

  /* if stepping CONFIG.SYS (F5/F8), tell COMMAND.COM about it */

  /* 3 for string + 2 for "\r\n" */
  if (Cmd.ctCount < sizeof(Cmd.ctBuffer) - 5)
  {
    char *insertString = NULL;

    if (singleStep)
      insertString = " /Y";     /* single step AUTOEXEC */

    if (SkipAllConfig)
      insertString = " /D";     /* disable AUTOEXEC */

    if (insertString)
    {

      /* insert /D, /Y as first argument */
      char *p, *q;

      for (p = Cmd.ctBuffer; p < &Cmd.ctBuffer[Cmd.ctCount]; p++)
      {
        if (*p == ' ' || *p == '\t' || *p == '\r')
        {
          for (q = &Cmd.ctBuffer[Cmd.ctCount + 1]; q >= p; q--)
            q[3] = q[0];
          memcpy(p, insertString, 3);
          break;
        }
      }
      /* save buffer -- on the stack it's fine here */
      Config.cfgInitTail = Cmd.ctBuffer;
    }
  }
  /* go execute process 0 (the shell) */
  cpu_set_a20(cpu, 1);
  SET_DS ( DOS_PSP );
  P_0(cpu, &Config);
  __unreachable();
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

    init_nls_hardcoded();

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

#ifdef DEBUG
    /* Non-portable message kludge alert!   */
    printf("KERNEL: Boot drive = %c\n", 'A' + LoL->BootDrive - 1);
#endif

    DoInstall();

    prep_shell(_cpu);

    __unreachable();
}
