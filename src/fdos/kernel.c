#include <pico.h>
#include <pico/time.h>
#include <hardware/pio.h>
#include <ctype.h>
#include "286/cpu.h"
#include "bios/bios.h"
#include "fdos.h"
#include "i8254.h"
#include "core0_stack.h"

#define printf(...) dos_printf(__VA_ARGS__)
CPU* cpu;

int	vsnprintf (char *__restrict, size_t, const char *__restrict, __gnuc_va_list)
               _ATTRIBUTE ((__format__ (__printf__, 3, 0)));

/*
    put_console()/put_string()/put_unsigned() - the prf.c primitives.

    The original writes single characters through INT 29h (fast console
    output); here the console is driven natively, so bios_teletype() takes its
    place. Semantics are kept: put_console() expands '\n' to CR+LF, put_string()
    does NOT append a newline of its own - callers such as play_dj() and
    task.c's "Bad or missing Command Interpreter: " build one line out of
    several put_string() calls, and the old put_string(x) macro (which appended
    "\n" to every call) broke them apart.
*/
void put_console(int c)
{
    if (c == '\n')
        put_console('\r');
    bios_teletype(cpu, (uint8_t)c, 0);
}

void put_string(const char *s)
{
    while (*s != '\0')
        put_console((unsigned char)*s++);
}

void put_unsigned(unsigned n, int base, int width)
{
    char s[6];
    int i;

    for (i = 0; i < width && i < (int)sizeof(s); i++)
    {                             /* generate digits in reverse order */
        s[i] = "0123456789abcdef"[n % (unsigned)base];
        n /= (unsigned)base;
    }

    while (i != 0)
    {                             /* print digits in reverse order */
        put_console(s[--i]);
    }
}

void dos_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bios_puts(cpu, buf);
}

#include "hdrs.h"
#include "native_devices.h"
#include "kernel_guest_proxy.h"

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


static inline uint32_t kernel_guest_linear(dos_far_ptr p)
{
  return ((uint32_t)FP_SEG(p) << 4) + FP_OFF(p);
}

static void kernel_guest_write(uint32_t addr, const void *src, size_t len)
{
  guest_write_block(addr, src, len);
}

static void kernel_guest_read(uint32_t addr, void *dst, size_t len)
{
  guest_read_block(addr, dst, len);
}

static void kernel_guest_fill(uint32_t addr, UBYTE value, size_t len)
{
  guest_fill_block(addr, value, len);
}

#define KERNEL_LOL_LINEAR \
  (((uint32_t)DOS_PSP << 4) + 0x08F0u)
#define KERNEL_IDATA_LINEAR \
  (((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF)
static dos_far_ptr kernel_guest_read_far(uint32_t addr)
{
  dos_far_ptr p;
  p.offset = pload16(addr);
  p.segment = pload16(addr + 2u);
  return p;
}

static void kernel_guest_write_far(uint32_t addr, dos_far_ptr p)
{
  pstore16(addr, p.offset);
  pstore16(addr + 2u, p.segment);
}

static UBYTE kernel_lol_read8(size_t off)
{
  return pload8(KERNEL_LOL_LINEAR + (uint32_t)off);
}

static UWORD kernel_lol_read16(size_t off)
{
  return pload16(KERNEL_LOL_LINEAR + (uint32_t)off);
}

static dos_far_ptr kernel_lol_read_far(size_t off)
{
  return kernel_guest_read_far(KERNEL_LOL_LINEAR + (uint32_t)off);
}

static void kernel_lol_write8(size_t off, UBYTE v)
{
  pstore8(KERNEL_LOL_LINEAR + (uint32_t)off, v);
}

static void kernel_lol_write_far(size_t off, dos_far_ptr p)
{
  kernel_guest_write_far(KERNEL_LOL_LINEAR + (uint32_t)off, p);
}

/* break_ena: the original kernel's C global break_ena IS the SDA byte
   at internal_data+17h (an asm label alias). The port briefly had two
   diverging copies; the SDA field internal_data->break_ena is now the
   single source of truth (guest programs peek that byte directly). */
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
 
_Static_assert(sizeof(nls_upcase_hardcoded_init)     == NLS_HC_TBL2_SIZE,
               "hardcoded upcase table must match NLS_HC_TBL2_SIZE");
_Static_assert(sizeof(nls_fupcase_hardcoded_init)    == NLS_HC_TBL4_SIZE,
               "hardcoded file-upcase table must match NLS_HC_TBL4_SIZE");
_Static_assert(sizeof(nls_fname_term_hardcoded_init) == NLS_HC_TBL5_SIZE,
               "hardcoded fname-terminator table must match NLS_HC_TBL5_SIZE");
_Static_assert(sizeof(nls_coll_hardcoded_init)       == NLS_HC_TBL6_SIZE,
               "hardcoded collate table must match NLS_HC_TBL6_SIZE");
_Static_assert(sizeof(nls_dbcs_hardcoded_init)       == NLS_HC_TBL7_SIZE,
               "hardcoded DBCS table must match NLS_HC_TBL7_SIZE");

static dos_far_ptr nls_hc_ptr(UWORD off)
{
    return MK_FP(FP_SEG(x86_nlsPackageHardcoded), FP_OFF(x86_nlsPackageHardcoded) + off);
}

static void init_nls_hardcoded(void)
{
    struct nlsPackage pkg;
    struct nlsInfoBlock info;
    UWORD off_table2 = sizeof(struct nlsPackage);
    UWORD off_table4 = off_table2 + sizeof(nls_upcase_hardcoded_init);
    UWORD off_table5 = off_table4 + sizeof(nls_fupcase_hardcoded_init);
    UWORD off_table6 = off_table5 + sizeof(nls_fname_term_hardcoded_init);
    UWORD off_table7 = off_table6 + sizeof(nls_coll_hardcoded_init);

    memset(&pkg, 0, sizeof(pkg));
    pkg.nxt = MK_FP(0, 0);
    pkg.cntry = 1;
    pkg.cp = 437;
    pkg.flags = NLS_FLAG_HARDCODED;
    pkg.yeschar = 'Y';
    pkg.nochar = 'N';
    pkg.numSubfct = 6;

    pkg.nlsPointers[0].subfct = 2;
    pkg.nlsPointers[0].pointer = nls_hc_ptr(off_table2);
    pkg.nlsPointers[1].subfct = 4;
    pkg.nlsPointers[1].pointer = nls_hc_ptr(off_table4);
    pkg.nlsPointers[2].subfct = 5;
    pkg.nlsPointers[2].pointer = nls_hc_ptr(off_table5);
    pkg.nlsPointers[3].subfct = 6;
    pkg.nlsPointers[3].pointer = nls_hc_ptr(off_table6);
    pkg.nlsPointers[4].subfct = 7;
    pkg.nlsPointers[4].pointer = nls_hc_ptr(off_table7);
    pkg.nlsPointers[5].subfct = 1;
    pkg.nlsPointers[5].pointer = nls_hc_ptr(offsetof(struct nlsPackage, nlsExt));

    pkg.nlsExt.subfct = 1;
    pkg.nlsExt.size = 0x001c;
    pkg.nlsExt.countryCode = 1;
    pkg.nlsExt.codePage = 437;
    pkg.nlsExt.dateFmt = 0;
    memcpy(pkg.nlsExt.curr, "$", 2);
    memcpy(pkg.nlsExt.thSep, ",", 2);
    memcpy(pkg.nlsExt.point, ".", 2);
    memcpy(pkg.nlsExt.dateSep, "-", 2);
    memcpy(pkg.nlsExt.timeSep, ":", 2);
    pkg.nlsExt.currFmt = 0;
    pkg.nlsExt.prescision = 2;
    pkg.nlsExt.timeFmt = 0;
    pkg.nlsExt.upCaseFct = CharMapSrvc;
    memcpy(pkg.nlsExt.dataSep, ",", 2);

    kernel_guest_write(kernel_guest_linear(x86_nlsPackageHardcoded),
                       &pkg, sizeof(pkg));
    kernel_guest_write(kernel_guest_linear(nls_hc_ptr(off_table2)),
                       nls_upcase_hardcoded_init,
                       sizeof(nls_upcase_hardcoded_init));
    kernel_guest_write(kernel_guest_linear(nls_hc_ptr(off_table4)),
                       nls_fupcase_hardcoded_init,
                       sizeof(nls_fupcase_hardcoded_init));
    kernel_guest_write(kernel_guest_linear(nls_hc_ptr(off_table5)),
                       nls_fname_term_hardcoded_init,
                       sizeof(nls_fname_term_hardcoded_init));
    kernel_guest_write(kernel_guest_linear(nls_hc_ptr(off_table6)),
                       nls_coll_hardcoded_init,
                       sizeof(nls_coll_hardcoded_init));
    kernel_guest_write(kernel_guest_linear(nls_hc_ptr(off_table7)),
                       nls_dbcs_hardcoded_init,
                       sizeof(nls_dbcs_hardcoded_init));

    memset(&info, 0, sizeof(info));
    info.fname = MK_FP(0, 0);
    info.sysCodePage = 437;
    info.flags = NLS_CODE_REORDER_POINTERS;
    info.actPkg = x86_nlsPackageHardcoded;
    info.chain = x86_nlsPackageHardcoded;
    kernel_guest_write(kernel_guest_linear(x86_nlsInfo), &info, sizeof(info));
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
    CPU_regs saved;
    cpu_save_regs(cpu, &saved);
    CPU_BX = (UWORD)fd;
    CPU_CX = (UWORD)((ULONG)position >> 16);
    CPU_DX = (UWORD)((ULONG)position & 0xffff);
    CPU_AX = 0x4200;   /* origin = start of file */
    bios_intcall(cpu, 0x21, "LSEEK");
    ULONG res = cf ? (ULONG)-1 : ((ULONG)CPU_DX << 16) | CPU_AX;
    cpu_restore_regs(cpu, &saved);
    return res;
}

void keycheck(void)
{
    CPU_regs saved;
    cpu_save_regs(cpu, &saved);
    CPU_AH = 0x01;
    bios_intcall(cpu, 0x16, "KEYCHECK");
    cpu_restore_regs(cpu, &saved);
}

static const struct dhdr _blk_dev = {
    .dh_next = MK_FP(-1, -1),
    .dh_attr = 0x08c2 | ATTR_NATIVE,
    .arm.dh_interrupt = BlkEntry,
    .dh_name = { 4, 0, 0, 0, 0, 0, 0, 0 },
};

static const struct dhdr _clk_dev = {
    .dh_next = x86_blk_dev,
    .dh_attr = 0x8008 | ATTR_NATIVE,
    .arm.dh_interrupt = ClkEntry,
    .dh_name = "CLOCK$  "
};

static const struct dhdr _com4_dev = {
    .dh_next = x86_clk_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com4Intr,
    .dh_name = "COM4    "
};

static const struct dhdr _com3_dev = {
    .dh_next = x86_com4_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com3Intr,
    .dh_name = "COM3    "
};

static const struct dhdr _com2_dev = {
    .dh_next = x86_com3_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = Com2Intr,
    .dh_name = "COM2    "
};

static const struct dhdr _com1_dev = {
    .dh_next = x86_com2_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = AuxIntr,
    .dh_name = "COM1    "
};

static const struct dhdr _lpt3_dev = {
    .dh_next = x86_com1_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt3Intr,
    .dh_name = "LPT3    "
};

static const struct dhdr _lpt2_dev = {
    .dh_next = x86_lpt3_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt2Intr,
    .dh_name = "LPT2    "
};

static const struct dhdr _lpt1_dev = {
    .dh_next = x86_lpt2_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = Lpt1Intr,
    .dh_name = "LPT1    "
};

static const struct dhdr _aux_dev = {
    .dh_next = x86_lpt1_dev,
    .dh_attr = 0x8000 | ATTR_NATIVE,
    .arm.dh_interrupt = AuxIntr,
    .dh_name = "AUX     "
};

static const struct dhdr _prn_dev = {
    .dh_next = x86_aux_dev,
    .dh_attr = 0xA040 | ATTR_NATIVE,
    .arm.dh_interrupt = PrnIntr,
    .dh_name = "PRN     "
};

static const struct dhdr _con_dev = {
    .dh_next = x86_prn_dev,
    .dh_attr = 0x8013 | ATTR_NATIVE,
    .arm.dh_interrupt = ConIntr,
    .dh_name = "CON     "
};

static const struct lol lol = {
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
    .sfthead     = {0},
    .clock       = x86_clk_dev,
    .syscon      = x86_con_dev,
    .maxsecsize  = 512,
    .CDSp        = {0},
    .FCBp        = {0},
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
void pc_step(struct PC* pc, size_t max_ops);

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
        ifl = 0; // no IRQ alloweed this time
        params->done = true;
        cpu->native_done = true;
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
///  printf("cpu_far_call @ CS:IP=%04x:%04x SS:SP=%04x:%04x\n", CPU_CS, CPU_IP, CPU_SS, CPU_SP);
  /* См. bios_intcall(): native_done стекуется, внешнее состояние
     восстанавливается после вложенного цикла. */
  bool old_native_done = cpu->native_done;
  cpu->native_done = false;
  set_bios_callback(cpu, &params, true);
///  printf("cpu_far_call callback node=%p ret=%04x:%04x\n",  &params, params.expected_cs, params.expected_ip);
  /* Emulate exactly what "CALL FAR seg:off" pushes: CS, then IP (so
     that RETF - which pops IP, then CS - lands back here). */
  CPU_SP -= 2;
  putmem8(CPU_SS, CPU_SP, params.expected_cs & 0xff);
  putmem8(CPU_SS, (CPU_SP + 1) & 0xffff, params.expected_cs >> 8);
  CPU_SP -= 2;
  putmem8(CPU_SS, CPU_SP, params.expected_ip & 0xff);
  putmem8(CPU_SS, (CPU_SP + 1) & 0xffff, params.expected_ip >> 8);

  SET_CS(seg);
  SET_IP(off);
  while (!params.done) {
    /* See bios_intcall(): bail out when a terminate is pending so we
       do not spin with a latched native_done.  A guest reached through
       this far-call (e.g. a driver entry issuing LMSW PE=1) that gets
       aborted by request_terminate() never RETFs back to the trap
       address, so params.done would never be set.  terminate_flag is
       left armed for the enclosing exec_run_process() to consume. */
    if (terminate_requested())
      break;
    pc_step(pc, 4096); /// ???
    /*
    if (CPU_CS == params.expected_cs && CPU_IP == params.expected_ip) {
        printf("cpu_far_call reached return %04x:%04x SP=%04x\n",
               CPU_CS, CPU_IP, CPU_SP);
    }
    u8 op = pload8((((u32)(CPU_CS)) << 4) + CPU_IP);
    printf("cpu_far_call waits x86 on CS:IP=%04x:%04x SS:SP=%04x:%04x opcode=%02x\n", CPU_CS, CPU_IP, CPU_SS, CPU_SP, op);
    sleep_ms(1000);
    */
  }
  cpu->native_done = old_native_done;
  drop_bios_callback(cpu, &params);
  SET_CS(save_cs);
  SET_IP(save_ip);
}

#if PDB_DEBUG
typedef struct drv_watch_s {
  dos_far_ptr dhp;
  dos_far_ptr next;
  UWORD attr, strat, intr;
  UBYTE first16[16];
  UWORD init_cmd;
  UWORD init_status;
  dos_far_ptr init_endaddr;
  char name[9];
  const char *tag;
} drv_watch_t;

#define DRV_WATCH_MAX 64
static drv_watch_t drv_watch[DRV_WATCH_MAX];
static unsigned drv_watch_count;
static dos_far_ptr drv_watch_last_dpb = MK_FP(0, 0);
static UBYTE drv_watch_last_cmd = 0xff;
static UBYTE drv_watch_last_unit = 0xff;
static UBYTE drv_watch_last_subunit = 0xff;
static const char *drv_watch_last_source = "?";

typedef struct dpb_watch_s {
  dos_far_ptr dpb;
  UBYTE bytes[64];
  const char *tag;
} dpb_watch_t;

#define DPB_WATCH_MAX 32
static dpb_watch_t dpb_watch[DPB_WATCH_MAX];
static unsigned dpb_watch_count;

static void dpb_watch_capture(const char *tag, dos_far_ptr _dpb)
{
  if (far_is_null(_dpb) || far_is_end(_dpb))
    return;
  if (dpb_watch_count >= DPB_WATCH_MAX)
    return;

  dpb_watch_t *w = &dpb_watch[dpb_watch_count++];
  w->dpb = _dpb;
  w->tag = tag;
  kernel_guest_read(kernel_guest_linear(_dpb), w->bytes, sizeof(w->bytes));
}

static void dpb_watch_capture_chain(const char *tag)
{
  dos_far_ptr _dpb = fdos_lol_dpb();
  unsigned guard = 0;
  const unsigned limit = (unsigned)fdos_lol_nblkdev() + 4u;

  while (!far_is_null(_dpb) && !far_is_end(_dpb) && guard++ < limit) {
    dpb_watch_capture(tag, _dpb);
    _dpb = fdos_dpb_next(_dpb);
  }
}

void dpb_watch_check(const char *tag, dos_far_ptr _dpb)
{
  for (unsigned i = 0; i < dpb_watch_count; i++) {
    if (FP_SEG(dpb_watch[i].dpb) != FP_SEG(_dpb) ||
        FP_OFF(dpb_watch[i].dpb) != FP_OFF(_dpb))
      continue;

    UBYTE now[sizeof(dpb_watch[i].bytes)];
    kernel_guest_read(kernel_guest_linear(_dpb), now, sizeof(now));
    if (memcmp(now, dpb_watch[i].bytes, sizeof(dpb_watch[i].bytes)) == 0) {
      dpb_watch[i].tag = tag;
      return;
    }

    printf("DPBWATCH PANIC[%s]: dpb=%04x:%04x saved_tag=%s\n",
           tag, FP_SEG(_dpb), FP_OFF(_dpb), dpb_watch[i].tag);
    printf("DPBWATCH saved: %02x %02x %02x %02x %02x %02x %02x %02x "
           "%02x %02x %02x %02x %02x %02x %02x %02x\n",
           dpb_watch[i].bytes[0], dpb_watch[i].bytes[1], dpb_watch[i].bytes[2], dpb_watch[i].bytes[3],
           dpb_watch[i].bytes[4], dpb_watch[i].bytes[5], dpb_watch[i].bytes[6], dpb_watch[i].bytes[7],
           dpb_watch[i].bytes[8], dpb_watch[i].bytes[9], dpb_watch[i].bytes[10], dpb_watch[i].bytes[11],
           dpb_watch[i].bytes[12], dpb_watch[i].bytes[13], dpb_watch[i].bytes[14], dpb_watch[i].bytes[15]);
    printf("DPBWATCH now:   %02x %02x %02x %02x %02x %02x %02x %02x "
           "%02x %02x %02x %02x %02x %02x %02x %02x\n",
           now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7],
           now[8], now[9], now[10], now[11], now[12], now[13], now[14], now[15]);
    for (;;) ;
  }
}
void dpb_watch_check_chain(const char *tag)
{
  for (unsigned i = 0; i < dpb_watch_count; i++)
    dpb_watch_check(tag, dpb_watch[i].dpb);
}
void drv_watch_set_dpb_context(dos_far_ptr/*struct dpb*/ _dpb, UBYTE cmd, UBYTE unit, UBYTE subunit, const char *source)
{
  dpb_watch_check("drv_watch_set_dpb_context", _dpb);
  drv_watch_last_dpb = _dpb;
  drv_watch_last_cmd = cmd;
  drv_watch_last_unit = unit;
  drv_watch_last_subunit = subunit;
  drv_watch_last_source = source;
}

static int in_e000_block(dos_far_ptr p)
{
  return FP_SEG(p) >= 0xE000 && FP_SEG(p) < 0xF000;
}

static void drv_watch_capture(const char *tag, dos_far_ptr dhp,
                              request *rq)
{
  if (drv_watch_count >= DRV_WATCH_MAX)
    return;

  const UWORD attr = fdos_dhdr_attr(dhp);
  if (attr & ATTR_NATIVE)
    return;
  drv_watch_t *w = &drv_watch[drv_watch_count++];

  w->dhp = dhp;
  w->next = fdos_dhdr_next(dhp);
  w->attr = attr;
  w->strat = fdos_dhdr_strategy(dhp);
  w->intr = fdos_dhdr_interrupt(dhp);
  kernel_guest_read(kernel_guest_linear(dhp), w->first16, sizeof(w->first16));
  fdos_dhdr_read_name(dhp, (BYTE *)w->name);
  w->name[8] = 0;
  w->tag = tag;
  w->init_cmd = 0xff;
  w->init_status = 0xffff;
  w->init_endaddr = MK_FP(0, 0);

  if (rq) {
    w->init_cmd = rq->r_command;
    w->init_status = rq->r_status;
    w->init_endaddr = rq->r_endaddr;
  }
}

static const drv_watch_t *drv_watch_find(dos_far_ptr dhp)
{
  for (unsigned i = 0; i < drv_watch_count; i++) {
    if (FP_SEG(drv_watch[i].dhp) == FP_SEG(dhp))
      return &drv_watch[i];
  }
  return NULL;
}

static int drv_watch_same_umb_macroblock(dos_far_ptr a, dos_far_ptr b)
{
  return (FP_SEG(a) & 0xF000) == (FP_SEG(b) & 0xF000);
}

static void drv_watch_print_one(const drv_watch_t *w)
{
  if (!w)
    return;

  printf("DRVWATCH: tag=%s dhp=%04x:%04x name='%.8s' next=%04x:%04x attr=%04x strat=%04x intr=%04x init_cmd=%02x init_status=%04x end=%04x:%04x "
         "first16=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
         w->tag,
         FP_SEG(w->dhp), FP_OFF(w->dhp), w->name,
         FP_SEG(w->next), FP_OFF(w->next),
         w->attr, w->strat, w->intr,
         w->init_cmd, w->init_status,
         FP_SEG(w->init_endaddr), FP_OFF(w->init_endaddr),
         w->first16[0], w->first16[1], w->first16[2], w->first16[3],
         w->first16[4], w->first16[5], w->first16[6], w->first16[7],
         w->first16[8], w->first16[9], w->first16[10], w->first16[11],
         w->first16[12], w->first16[13], w->first16[14], w->first16[15]);
}

static void drv_watch_print_all(dos_far_ptr dhp)
{
  unsigned found = 0;

  printf("DRVWATCH ALL: requested=%04x:%04x count=%u\n",
         FP_SEG(dhp), FP_OFF(dhp), drv_watch_count);

  for (unsigned i = 0; i < drv_watch_count; i++) {
    if (!drv_watch_same_umb_macroblock(drv_watch[i].dhp, dhp))
      continue;
    found = 1;
    drv_watch_print_one(&drv_watch[i]);
  }

   if (!found)
  {
    printf("DRVWATCH ALL: no saved records for macroblock %04x\n", FP_SEG(dhp) & 0xF000);
    for (unsigned i = 0; i < drv_watch_count; i++)
      drv_watch_print_one(&drv_watch[i]);
  }
}

static int drv_watch_dhdr_looks_text(const struct dhdr *d)
{
  const unsigned char *p = (const unsigned char *)d;
  unsigned printable = 0;

  for (unsigned i = 0; i < 16; i++) {
    if ((p[i] >= 0x20 && p[i] <= 0x7e) || p[i] == 0)
      printable++;
  }

  return printable >= 14;
}

static void drv_watch_print_current(dos_far_ptr dhp, const struct dhdr *d)
{
  const unsigned char *p = (const unsigned char *)d;

  printf("DRVWATCH CURRENT: dhp=%04x:%04x "
         "next=%04x:%04x attr=%04x strat=%04x intr=%04x "
         "bytes=%02x %02x %02x %02x %02x %02x %02x %02x "
         "%02x %02x %02x %02x %02x %02x %02x %02x "
         "ascii='%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c'\n",
         FP_SEG(dhp), FP_OFF(dhp),
         FP_SEG(d->dh_next), FP_OFF(d->dh_next),
         d->dh_attr, d->x86.dh_strategy, d->x86.dh_interrupt,
         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
         p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15],
         (p[0] >= 0x20 && p[0] <= 0x7e) ? p[0] : '.',
         (p[1] >= 0x20 && p[1] <= 0x7e) ? p[1] : '.',
         (p[2] >= 0x20 && p[2] <= 0x7e) ? p[2] : '.',
         (p[3] >= 0x20 && p[3] <= 0x7e) ? p[3] : '.',
         (p[4] >= 0x20 && p[4] <= 0x7e) ? p[4] : '.',
         (p[5] >= 0x20 && p[5] <= 0x7e) ? p[5] : '.',
         (p[6] >= 0x20 && p[6] <= 0x7e) ? p[6] : '.',
         (p[7] >= 0x20 && p[7] <= 0x7e) ? p[7] : '.',
         (p[8] >= 0x20 && p[8] <= 0x7e) ? p[8] : '.',
         (p[9] >= 0x20 && p[9] <= 0x7e) ? p[9] : '.',
         (p[10] >= 0x20 && p[10] <= 0x7e) ? p[10] : '.',
         (p[11] >= 0x20 && p[11] <= 0x7e) ? p[11] : '.',
         (p[12] >= 0x20 && p[12] <= 0x7e) ? p[12] : '.',
         (p[13] >= 0x20 && p[13] <= 0x7e) ? p[13] : '.',
         (p[14] >= 0x20 && p[14] <= 0x7e) ? p[14] : '.',
         (p[15] >= 0x20 && p[15] <= 0x7e) ? p[15] : '.');
}

static void drv_watch_print_dpb_context(void)
{
  printf("DRVWATCH DPBCTX: source=%s dpb=%04x:%04x cmd=%02x unit=%u sub=%u\n",
         drv_watch_last_source, FP_SEG(drv_watch_last_dpb), FP_OFF(drv_watch_last_dpb),
         drv_watch_last_cmd, drv_watch_last_unit, drv_watch_last_subunit);

  if (!far_is_null(drv_watch_last_dpb) && !far_is_end(drv_watch_last_dpb)) {
    const dos_far_ptr next = fdos_dpb_next(drv_watch_last_dpb);
    const dos_far_ptr device = fdos_dpb_device(drv_watch_last_dpb);
    printf("DRVWATCH DPBCTX: dpb_next=%04x:%04x dpb_unit=%u dpb_subunit=%u dpb_device=%04x:%04x flags=%04x mdb=%02x\n",
           FP_SEG(next), FP_OFF(next),
           fdos_dpb_unit(drv_watch_last_dpb), fdos_dpb_subunit(drv_watch_last_dpb),
           FP_SEG(device), FP_OFF(device),
           (UWORD)(UBYTE)fdos_dpb_flags(drv_watch_last_dpb), fdos_dpb_mdb(drv_watch_last_dpb));
  }
}

static void drv_watch_panic_if_bad(dos_far_ptr dhp, const struct dhdr *d)
{
  if (!in_e000_block(dhp))
    return;

  if (!drv_watch_dhdr_looks_text(d))
    return;

  printf("\nDRVWATCH PANIC: suspicious device header before x86_execrh\n");
  drv_watch_print_dpb_context();
  drv_watch_print_current(dhp, d);
  drv_watch_print_all(dhp);

  for (;;) ;
}
#endif

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
static void x86_execrh(/*request*/ dos_far_ptr x86_rq, dos_far_ptr x86_dhp, UWORD dh_strategy, UWORD dh_interrupt) {
#if PDB_DEBUG
  struct dhdr debug_dh;
  kernel_guest_read(kernel_guest_linear(x86_dhp), &debug_dh, sizeof(debug_dh));
  drv_watch_panic_if_bad(x86_dhp, &debug_dh);
#endif
  bool ifl_old = ifl;
//  ifl = 1;
//  df = 0;
  /*
   * C analogue of FreeDOS kernel/execrh.asm.
   *
   * Original flow:
   *   push si
   *   push ds
   *   lds  si,[dhp]     ; DS:SI = device header
   *   les  bx,[rhp]     ; ES:BX = request header
   *   mov  ax,[si+6]    ; strategy offset
   *   mov  [dhp],ax     ; far ptr keeps original segment
   *   push si
   *   push di
   *   call far [dhp]
   *   pop  di
   *   pop  si
   *   mov  ax,[si+8]    ; interrupt offset
   *   mov  [dhp],ax
   *   call far [dhp]
   *   sti
   *   cld
   *   pop  ds
   *   pop  si
   */

  UWORD hdr_seg = FP_SEG(x86_dhp);
  UWORD hdr_off = FP_OFF(x86_dhp);

  /* push bp; mov bp,sp; push si; push ds
   *
   * Match FreeDOS execrh.asm prologue. Some real drivers do not
   * preserve BP; execrh must preserve the caller's BP across both
   * driver entry points.
   */
  CPU_SP -= 2;
  writew86(((uint32_t)CPU_SS << 4) + CPU_SP, CPU_BP);
  CPU_BP = CPU_SP;
  CPU_SP -= 2;
  writew86(((uint32_t)CPU_SS << 4) + CPU_SP, CPU_SI);
  CPU_SP -= 2;
  writew86(((uint32_t)CPU_SS << 4) + CPU_SP, CPU_DS);

  /* lds si,[dhp] */
  SET_DS ( hdr_seg );
  CPU_SI = hdr_off;

  /* les bx,[rhp] */
  SET_ES ( FP_SEG(x86_rq) );
  CPU_BX = FP_OFF(x86_rq);
  #if EXEC_DEBUG
  const uint32_t dh_lin = kernel_guest_linear(x86_dhp);
  const uint32_t dh_next_raw = pload32(dh_lin + offsetof(struct dhdr, dh_next));
  const dos_far_ptr dh_next = MK_FP((UWORD)(dh_next_raw >> 16), (UWORD)dh_next_raw);
  const UWORD dh_attr = pload16(dh_lin + offsetof(struct dhdr, dh_attr));
  printf("x86_execrh: dh_strategy @ %04x:%04x stack: %04x:%04x\n", hdr_seg, dh_strategy, CPU_SS, CPU_SP);
  printf("x86_execrh: DS=%04x ES=%04x BX=%04x rq=%04x:%04x\n", CPU_DS, CPU_ES, CPU_BX, FP_SEG(x86_rq), FP_OFF(x86_rq));
  printf("x86_execrh: hdr next=%04x:%04x attr=%04x strat=%04x intr=%04x "
         "rq len=%02x unit=%02x cmd=%02x status=%04x "
         "strat-op=%02x %02x %02x %02x %02x\n",
         FP_SEG(dh_next), FP_OFF(dh_next),
         dh_attr, dh_strategy, dh_interrupt,
         getmem8(FP_SEG(x86_rq), FP_OFF(x86_rq) + 0),
         getmem8(FP_SEG(x86_rq), FP_OFF(x86_rq) + 1),
         getmem8(FP_SEG(x86_rq), FP_OFF(x86_rq) + 2),
         getmem16(FP_SEG(x86_rq), FP_OFF(x86_rq) + 3),
         getmem8(hdr_seg, dh_strategy + 0), getmem8(hdr_seg, dh_strategy + 1), getmem8(hdr_seg, dh_strategy + 2), getmem8(hdr_seg, dh_strategy + 3), getmem8(hdr_seg, dh_strategy + 4));
  #endif
  /* push si; push di */
  CPU_SP -= 2;
  writew86(((uint32_t)CPU_SS << 4) + CPU_SP, CPU_SI);
  CPU_SP -= 2;
  writew86(((uint32_t)CPU_SS << 4) + CPU_SP, CPU_DI);

  cpu_far_call(cpu, hdr_seg, dh_strategy);
  ifl = ifl_old;

  /* pop di; pop si */
  CPU_DI = readw86(((uint32_t)CPU_SS << 4) + CPU_SP);
  CPU_SP += 2;
  CPU_SI = readw86(((uint32_t)CPU_SS << 4) + CPU_SP);
  CPU_SP += 2;

  #if EXEC_DEBUG
  printf("x86_execrh: dh_interrupt @ %04x:%04x\n", hdr_seg, dh_interrupt);
  #endif
  cpu_far_call(cpu, hdr_seg, dh_interrupt);

  /* sti; cld */
  ifl = 1;
  df = 0;

  /* pop ds; pop si */
  SET_DS(readw86(((uint32_t)CPU_SS << 4) + CPU_SP));
  CPU_SP += 2;
  CPU_SI = readw86(((uint32_t)CPU_SS << 4) + CPU_SP);
  CPU_SP += 2;
  CPU_BP = readw86(((uint32_t)CPU_SS << 4) + CPU_SP);
  CPU_SP += 2;
  #if EXEC_DEBUG
  printf("x86_execrh: done\n");
  #endif
}

WORD execrh(/*request*/ dos_far_ptr _rq, /*struct dhdr*/ dos_far_ptr _dhp) {
  UWORD status;
  UWORD strategy;
  UWORD interrupt;

  /* Native device callbacks consume the guest request through request_ref;
     no dhdr/request snapshot or copyback is required. */
  if (fdos_native_execrh(_dhp, _rq, &status))
    return status;

  /* Real x86 drivers need only the two entry offsets from their guest header. */
  fdos_x86_dhdr_entries(_dhp, &strategy, &interrupt);
  x86_execrh(_rq, _dhp, strategy, interrupt);
  return pload16(kernel_guest_linear(_rq) + offsetof(request, r_status));
}

/* check for a block device and update  device control block    */
STATIC VOID update_dcb(/*struct dhdr*/ dos_far_ptr x86_dhp)
{
  struct dhdr dh;
  REG COUNT Index;
  COUNT nunits;
  dos_far_ptr x86_dpb;
  UBYTE nblkdev;

  kernel_guest_read(kernel_guest_linear(x86_dhp), &dh, sizeof(dh));
  nunits = (UBYTE)dh.dh_name[0];
  if (nunits == 0)
    return;

  if (kernel_lol_read16(offsetof(struct lol, first_mcb)) != 0) {
    x86_dpb = KernelAlloc(nunits * sizeof(struct dpb), 'E', Config.cfgDosDataUmb);
  } else {
    x86_dpb = DynAlloc("DPBp", (UBYTE)dh.dh_name[0], sizeof(struct dpb));
  }

  nblkdev = kernel_lol_read8(offsetof(struct lol, nblkdev));
  if (nblkdev == 0) {
    kernel_lol_write_far(offsetof(struct lol, DPBp), x86_dpb);
  } else {
    dos_far_ptr cur = kernel_lol_read_far(offsetof(struct lol, DPBp));
    struct dpb d;

    for (;;) {
      kernel_guest_read(kernel_guest_linear(cur), &d, sizeof(d));
      if (far_is_end(d.dpb_next))
        break;
      cur = d.dpb_next;
    }
    d.dpb_next = x86_dpb;
    kernel_guest_write(kernel_guest_linear(cur), &d, sizeof(d));
  }

  for (Index = 0; Index < nunits; Index++)
  {
    dos_far_ptr this_dpb = ADD_OFF(x86_dpb, Index * sizeof(struct dpb));
    struct dpb d;

    kernel_guest_read(kernel_guest_linear(this_dpb), &d, sizeof(d));
    d.dpb_next = (Index + 1 < nunits)
               ? ADD_OFF(x86_dpb, (Index + 1) * sizeof(struct dpb))
               : MK_FP((UWORD)-1, (UWORD)-1);
    d.dpb_unit = nblkdev;
    d.dpb_subunit = Index;
    d.dpb_device = x86_dhp;
    d.dpb_flags = M_CHANGED;
    kernel_guest_write(kernel_guest_linear(this_dpb), &d, sizeof(d));

    {
      dos_far_ptr cds_base = kernel_lol_read_far(offsetof(struct lol, CDSp));
      UBYTE lastdrive = kernel_lol_read8(offsetof(struct lol, lastdrive));
      if (!far_is_null(cds_base) && nblkdev < lastdrive) {
        uint32_t cds_lin = kernel_guest_linear(cds_base) +
                           (uint32_t)nblkdev * sizeof(struct cds);
        struct cds c;
        kernel_guest_read(cds_lin, &c, sizeof(c));
        c.cdsDpb = this_dpb;
        c.cdsFlags = CDSPHYSDRV;
        kernel_guest_write(cds_lin, &c, sizeof(c));
      }
    }

    ++nblkdev;
    kernel_lol_write8(offsetof(struct lol, nblkdev), nblkdev);
  }
}

/* If cmdLine is NULL, this is an internal driver */

BOOL init_device(/*struct dhdr*/ dos_far_ptr x86_dhp, char *cmdLine, COUNT mode, dos_far_ptr* r_top)
{
  struct dhdr dh;
  const char *cmdstr = cmdLine ? cmdLine : "\n";
  size_t cmdlen = strlen(cmdstr) + 1;
  dos_far_ptr x86_cmdline;
  dos_far_ptr x86_rq;
  request rq;
  char name[8];

  kernel_guest_read(kernel_guest_linear(x86_dhp), &dh, sizeof(dh));

  x86_cmdline = guest_stack_alloc(cpu, (uint16_t)(sizeof(request) + cmdlen));
  x86_rq = MK_FP(FP_SEG(x86_cmdline),
                 (uint16_t)(FP_OFF(x86_cmdline) + cmdlen));

  memset(&rq, 0, sizeof(rq));
  guest_strcpy(x86_cmdline, cmdstr);

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
        q = p;
    }
    for (i = 0; i < 8; i++) {
      ch = '\0';
      if (p != q && *q != '.')
        ch = *q++;
      name[i] = ch;
    }
  }

  rq.r_unit = 0;
  rq.r_status = 0;
  rq.r_command = C_INIT;
  rq.r_length = sizeof(request);
  rq.r_endaddr = *r_top;
  rq.r_bpbptr = x86_cmdline;
  rq.r_firstunit = kernel_lol_read8(offsetof(struct lol, nblkdev));
  kernel_guest_write(kernel_guest_linear(x86_rq), &rq, sizeof(rq));

  execrh(x86_rq, x86_dhp);
  kernel_guest_read(kernel_guest_linear(x86_rq), &rq, sizeof(rq));

  if ((rq.r_status & (S_ERROR | S_DONE)) == S_ERROR)
      goto ok;

  kernel_guest_read(kernel_guest_linear(x86_dhp), &dh, sizeof(dh));

  if (cmdLine)
  {
    if (kernel_guest_linear(rq.r_endaddr) == kernel_guest_linear(x86_dhp))
      goto ok;

    if (!(dh.dh_attr & ATTR_CHAR) && !rq.r_nunits)
    {
      rq.r_endaddr = x86_dhp;
      kernel_guest_write(kernel_guest_linear(x86_rq), &rq, sizeof(rq));
      goto ok;
    }

    if (FP_OFF(dh.dh_next) == 0xffff) {
        drv_watch_capture("before-KernelAllocPara", x86_dhp, &rq);
        KernelAllocPara(FP_SEG(rq.r_endaddr) +
                        (FP_OFF(rq.r_endaddr) + 15)/16 -
                        FP_SEG(x86_dhp), 'D', name, mode);
        drv_watch_capture("after-KernelAllocPara", x86_dhp, &rq);
    }

    *r_top = rq.r_endaddr;
  }

  if (!(dh.dh_attr & ATTR_CHAR) && (rq.r_nunits != 0))
  {
    drv_watch_capture("after-C_INIT-before-update_dcb", x86_dhp, &rq);
    pstore8(kernel_guest_linear(x86_dhp) + offsetof(struct dhdr, dh_name),
            rq.r_nunits);
    update_dcb(x86_dhp);
    drv_watch_capture("after-update_dcb", x86_dhp, &rq);
  }

  if (dh.dh_attr & ATTR_CONIN)
    kernel_lol_write_far(offsetof(struct lol, syscon), x86_dhp);
  else if (dh.dh_attr & ATTR_CLOCK)
    kernel_lol_write_far(offsetof(struct lol, clock), x86_dhp);

  CPU_SP += sizeof(request) + cmdlen;
  return FALSE;
ok:
  CPU_SP += sizeof(request) + cmdlen;
  return TRUE;
}

STATIC void InitIO()
{
    dos_far_ptr x86_device =
        ADD_OFF(x86_FIXED_DATA, offsetof(struct lol, nul_dev));

    do {
        struct dhdr dh;
        init_device(x86_device, NULL, 0, &lpTop);
        kernel_guest_read(kernel_guest_linear(x86_device), &dh, sizeof(dh));
        x86_device = dh.dh_next;
    }
    while (FP_OFF(x86_device) != 0xffff);
}

static void set_DTA(dos_far_ptr p) {
    CPU_AH = 0x1A; // Set Current DTA
    SET_DS (FP_SEG(p));
    CPU_DX = p.offset;
    bios_intcall(cpu, 0x21, "DTA");
}

dos_far_ptr /* -> interrupt handler entry */ getvec(uint8_t intno) {
    uint32_t res = pload32(4ul * intno);
    /* An IVT slot is <offset word><segment word>, little-endian, which is
       byte-for-byte the layout of dos_far_ptr - but reading it back as
       *(dos_far_ptr *)&res is a strict-aliasing violation (GCC flags it
       under -Wstrict-aliasing), and this file is compiled at -O3, where the
       optimiser is entitled to act on that. Build the pair explicitly
       instead: same code, no aliasing assumption, and the IVT layout is
       now stated rather than implied. */
    return MK_FP((uint16_t)(res >> 16), (uint16_t)(res & 0xFFFFu));
}

void setvec(uint8_t intno, dos_far_ptr vec) {
    pstore16(4ul * intno,     FP_OFF(vec));
    pstore16(4ul * intno + 2, FP_SEG(vec));
}

STATIC void PSPInit(void)
{
  psp p;
  UBYTE os_major = kernel_lol_read8(offsetof(struct lol, os_setver_major));
  UBYTE os_minor = kernel_lol_read8(offsetof(struct lol, os_setver_minor));

  memset(&p, 0, sizeof(p));

  p.ps_exit = 0x20cd;
  p.ps_farcall = 0x9a;
  p.ps_reentry = MK_FP(0, 0x30 * 4);

  write86(0x00c0, 0xea);
  writew86(0x00c1, 0x0030);
  writew86(0x00c3, 0xffe0);

  p.ps_unix[0] = 0xcd;
  p.ps_unix[1] = 0x21;
  p.ps_unix[2] = 0xcb;

  p.ps_parent = FP_SEG(x86_PSP);
  p.ps_prevpsp = MK_FP((UWORD)-1, (UWORD)-1);
  p.ps_environ = DOS_PSP + 8;
  p.ps_isv22 = getvec(0x22);
  p.ps_isv23 = getvec(0x23);
  p.ps_isv24 = getvec(0x24);

  p.ps_maxfiles = 20;
  memset(p.ps_files, 0xff, 20);
  p.ps_filetab = MK_FP(FP_SEG(x86_PSP), offsetof(psp, ps_files));
  p.ps_retdosver = ((UWORD)os_minor << 8) + os_major;

  memset(p.ps_fcb1.fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);
  memset(p.ps_fcb2.fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);

  kernel_guest_write(kernel_guest_linear(x86_PSP), &p, sizeof(p));
}

/*
    Guest memory primitives that honour 16-bit offset wrap.
    ==========================================================

    A real-mode string move wraps the OFFSET inside the segment: writing past
    seg:FFFF continues at seg:0000 of the SAME segment. It does not spill into
    the next one. Upstream gets this for free - fmemcpy()/fmemset() are
    rep movsb/stosb over ES:DI, and DI is a 16-bit register.

    ARM_PTR() + memcpy() does not wrap. It just keeps walking, which is worse
    than the guest's own bug in two distinct ways:

      1. A guest that overruns its own buffer corrupts SOMEONE ELSE's segment
         instead of its own. On real DOS the damage is confined to the
         offending program; here it is not.
      2. Near the top of guest RAM it walks straight out of the mapped window
         (a 512-byte transfer to FFFF:FFE0 reaches 0x1101DF, past the
         0x10FFEF the guest can even name).

    Splitting each copy at the segment boundary fixes both at once, and the
    result is provably in range: the highest byte any of these can touch is
    (seg << 4) + 0xFFFF, which for seg == 0xFFFF is exactly X86_MAX_LINEAR.

    Cost is a compare and a branch on the common (non-wrapping) path, and
    zero bytes of SRAM - the split is pointer arithmetic, not a bounce buffer.
*/

/* Bytes from off to the end of its 64K segment. off == 0 gives a full 64K. */
static inline size_t seg_room(uint16_t off) { return (size_t)0x10000u - off; }

/*
    Линейные копиры guest_lin_read()/guest_lin_write()/guest_lin_set() -
    нижний этаж всех far-примитивов: каждый непрерывный (в смысле 16-бит
    оффсета) кусок дополнительно дробится guest_span_ptr()'ом по гранулам
    гостевой физической карты, чтобы копия попадала в АКТИВНУЮ страницу
    окна EMS, а не в сырую линейную память под ним (см. mem.h). Экспорт в
    proto.h - для путей данных, держащих гостевой линейный курсор напрямую
    (rwblock() в fatfs.c).
*/
void guest_lin_write(uint32_t lin, const void *src, size_t n) {
    const uint8_t *p = (const uint8_t *)src;
    while (n) {
        uint32_t span;
        uint8_t *h = guest_span_ptr(lin, &span);
        size_t chunk = (span < n) ? span : n;
        dos_api_memcpy(h, p, chunk);
        p += chunk; lin += chunk; n -= chunk;
    }
}

void guest_lin_read(void *dst, uint32_t lin, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n) {
        uint32_t span;
        const uint8_t *h = guest_span_ptr(lin, &span);
        size_t chunk = (span < n) ? span : n;
        dos_api_memcpy(p, h, chunk);
        p += chunk; lin += chunk; n -= chunk;
    }
}

static void guest_lin_set(uint32_t lin, int v, size_t n) {
    while (n) {
        uint32_t span;
        uint8_t *h = guest_span_ptr(lin, &span);
        size_t chunk = (span < n) ? span : n;
        nf_memset(h, v, chunk);
        lin += chunk; n -= chunk;
    }
}

void fmemset(dos_far_ptr p, int v, unsigned int sz) {
    uint16_t seg = FP_SEG(p), off = FP_OFF(p);
    size_t n = sz;

    while (n) {
        size_t chunk = n, room = seg_room(off);
        if (chunk > room) chunk = room;
        guest_lin_set(EFFECTIVE(MK_FP(seg, off)), v, chunk);
        n -= chunk;
        off = (uint16_t)(off + chunk);   /* wraps to 0 at the segment end */
    }
}

void fmemcpy(dos_far_ptr d, const dos_far_ptr s, size_t n) {
    uint16_t dseg = FP_SEG(d), doff = FP_OFF(d);
    uint16_t sseg = FP_SEG(s), soff = FP_OFF(s);

    /* Source and destination wrap independently, so each pass is limited by
       whichever of the two hits its segment end first. */
    while (n) {
        size_t chunk = n;
        size_t droom = seg_room(doff);
        size_t sroom = seg_room(soff);

        if (chunk > droom) chunk = droom;
        if (chunk > sroom) chunk = sroom;

        {
            uint32_t dlin = EFFECTIVE(MK_FP(dseg, doff));
            uint32_t slin = EFFECTIVE(MK_FP(sseg, soff));
            size_t   left = chunk;
            while (left) {
                uint32_t dspan, sspan;
                uint8_t       *dh = guest_span_ptr(dlin, &dspan);
                const uint8_t *sh = guest_span_ptr(slin, &sspan);
                size_t m = left;
                if (m > dspan) m = dspan;
                if (m > sspan) m = sspan;
                dos_api_memcpy(dh, sh, m);
                dlin += m; slin += m; left -= m;
            }
        }
        n -= chunk;
        doff = (uint16_t)(doff + chunk);
        soff = (uint16_t)(soff + chunk);
    }
}

/* Native buffer -> guest, wrapping. For the many sites that hold a native
   pointer on one side and a guest far pointer on the other. */
void guest_write(dos_far_ptr d, const void *src, size_t n) {
    const UBYTE *p = (const UBYTE *)src;
    uint16_t seg = FP_SEG(d), off = FP_OFF(d);

    while (n) {
        size_t chunk = n, room = seg_room(off);
        if (chunk > room) chunk = room;
        guest_lin_write(EFFECTIVE(MK_FP(seg, off)), p, chunk);
        p += chunk;
        n -= chunk;
        off = (uint16_t)(off + chunk);
    }
}

/* Guest -> native buffer, wrapping. */
void guest_read(void *dst, dos_far_ptr s, size_t n) {
    UBYTE *p = (UBYTE *)dst;
    uint16_t seg = FP_SEG(s), off = FP_OFF(s);

    while (n) {
        size_t chunk = n, room = seg_room(off);
        if (chunk > room) chunk = room;
        guest_lin_read(p, EFFECTIVE(MK_FP(seg, off)), chunk);
        p += chunk;
        n -= chunk;
        off = (uint16_t)(off + chunk);
    }
}

/* strcpy() into guest memory, NUL included, wrapping. */
void guest_strcpy(dos_far_ptr d, const char *s) {
    guest_write(d, s, strlen(s) + 1);
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
    pair

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
  uint32_t lin = fdos_arm_linear(p);
  /* is_guest_ptr() above has already established lin <= X86_MAX_LINEAR
     (0x10FFEF), so the HMA branch's offset (lin - 0xFFFF0) is <= 0xFFFF
     and the cast below cannot truncate. See is_guest_ptr() in portab.h
     for why the window stops exactly there. */
  if (lin >= 0x100000)
    return MK_FP(0xFFFF, (UWORD)(lin - 0xFFFF0));
  return MK_FP((UWORD)(lin >> 4), (UWORD)(lin & 0xF));
}

int init_setdrive(int drive) {
    CPU_regs saved;
    cpu_save_regs(cpu, &saved);
    CPU_AH = 0x0e;
    CPU_DX = drive;
    bios_intcall(cpu, 0x21, "SET DRIVE");
    int res = CPU_AL;          /* number of potentially valid drives */
    cpu_restore_regs(cpu, &saved);
    return res;
}

int init_DosOpen(dos_far_ptr pathname, int flags) {
    CPU_regs saved;
    cpu_save_regs(cpu, &saved);
    SET_DS (FP_SEG(pathname));
    CPU_DX = FP_OFF(pathname);
    CPU_AL = flags & 0xff;
    CPU_AH = 0x3d;          /* DOS open */
    bios_intcall(cpu, 0x21, "I_OPEN");
    int res = cf ? -1 : CPU_AX;
    cpu_restore_regs(cpu, &saved);
    return res;
}

int dup2(int oldfd, int newfd)
{
    CPU_regs saved;
    cpu_save_regs(cpu, &saved);
    CPU_AH = 0x46;      /* Force duplicate file handle */
    CPU_BX = oldfd;
    CPU_CX = newfd;
    bios_intcall(cpu, 0x21, "DUP2");
    int res = cf ? -1 : CPU_AX;
    cpu_restore_regs(cpu, &saved);
    return res;
}

int read(int fd, dos_far_ptr dst, COUNT sz) {
    CPU_regs saved;
    cpu_save_regs(cpu, &saved);
    CPU_AH = 0x3F;
    CPU_BX = fd;
    CPU_CX = sz;
    CPU_DX = dst.offset;
    SET_DS ( dst.segment );
    bios_intcall(cpu, 0x21, "READ");
    int res = cf ? -1 : CPU_AX;
    cpu_restore_regs(cpu, &saved);
    return res;
}

int close(int fd) {
    CPU_regs saved;
    cpu_save_regs(cpu, &saved);
    CPU_AH = 0x3E;
    CPU_BX = fd;
    bios_intcall(cpu, 0x21, "CLOSE");
    int res = cf ? -1 : CPU_AX;
    cpu_restore_regs(cpu, &saved);
    return res;
}

/*
    idx_to_sft(SftIndex) - translate a system file number into the
    corresponding open SFT entry.

    idx_to_sft_() performs the complete chain walk and leaves
    internal_data->lpCurSft pointing at the exact guest SFT entry.
    An unused entry (sft_count == 0) is reported as not found.
*/
dos_far_ptr /*sft*/ idx_to_sft(int SftIndex)
{
  dos_far_ptr result;

  SftIndex = idx_to_sft_(SftIndex);

  /* if not opened, the SFT is useless            */
  if (SftIndex == -1)
    return MK_FP(-1,-1);

  result = fdos_dos_lp_cur_sft();
  if (fdos_sft_count(result) == 0)
    return MK_FP(-1,-1);
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
/*
    The guest-visible contract of "no redirector loaded":

      INT 2Fh AX=1100h  -> AL = 00h  (fdos_2fh.c: installation check)
      INT 2Fh AX=1000h  -> AL = 00h  (SHARE not installed)
      INT 21h AH=5Eh    -> CF=1, AX=0001h (invalid function)
      INT 21h AH=5Fh    -> CF=1, AX=0001h (invalid function)

    which is exactly what MS-DOS answers with no redirector present. It comes
    out of the -DE_INVLDFUNC below: fdos_21h's error_exit does AX = -rc.

    truename() also depends on QRemote_Fn() failing here, so that path
    resolution falls through to the local (non-networked) code, the same way
    it does on real DOS without a redirector.
*/
long network_redirector_mx(unsigned cmd, void *s, void *arg)
{
  UNREFERENCED_PARAMETER(cmd);
  UNREFERENCED_PARAMETER(s);
  UNREFERENCED_PARAMETER(arg);
  return -DE_INVLDFUNC;
}

int network_redirector_fp(unsigned cmd, void *s)
{
  UNREFERENCED_PARAMETER(cmd);
  UNREFERENCED_PARAMETER(s);
  return -DE_INVLDFUNC;
}

/* Declared in proto.h but never defined until now: the ///-disabled call
   sites in dosfns.c (REM_GETATTRZ, REM_MKDIR/REM_RMDIR, REM_RENAME, ...)
   reference it, so they can be re-enabled without a link error. */
int network_redirector(unsigned cmd)
{
  UNREFERENCED_PARAMETER(cmd);
  return -DE_INVLDFUNC;
}

/*
    -----------------------------------------------------------------
    DosUpChar/DosUpString/DosUpMem/DosUpFChar/DosUpFString/DosUpFMem
    are implemented in nls.c, as in the original kernel: they honour
    NLS_FLAG_DIRECT_UPCASE / NLS_FLAG_DIRECT_FUPCASE and fall back to
    MUX-14 (NLSFUNC_UPMEM / NLSFUNC_FILE_UPMEM) otherwise.
    -----------------------------------------------------------------
*/


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

#define addChar(c) \
{ \
  /*if (p >= dest + SFTMAX) return PATH_ERROR(); */	\
  if (p >= dest + FDOS_PATHLEN - 1) return PATH_ERROR(); /* path too long */	\
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
        program via DS:DX, just like the original). ARM_PTR() exposes
        the guest string as one linear read-only view, so the port can
        walk it directly instead of copying PATHLEN bytes to the native
        ARM stack. There is no adjust_far()-equivalent normalization
        step: adjust_far() only keeps a real 16-bit far offset away from
        the 0xFFFF wrap boundary while the original advances through
        src; EFFECTIVE()/ARM_PTR() already form the corresponding linear
        address in this emulator.
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
      - media_check()/CDS.dpb: cdsDpb is a dos_far_ptr (see
        cds.h), so media_check() (which takes a native struct dpb*)
        needs an ARM_PTR() first.
*/
#define TNDBG(fmt, ...) 
///printf("[truename] " fmt "\n", ##__VA_ARGS__)

#define TNPTR(p)  ((unsigned)((const char *)(p) - src))
#define TNDPTR(p) ((unsigned)((const char *)(p) - dest))

static int dpb_chain_contains(dos_far_ptr needle)
{
  dos_far_ptr p = fdos_lol_dpb();
  unsigned guard = 0;
  const unsigned limit = (unsigned)fdos_lol_nblkdev() + 4u;

  while (!far_is_null(p) && !far_is_end(p) && guard++ < limit)
  {
    if (FP_SEG(p) == FP_SEG(needle) && FP_OFF(p) == FP_OFF(needle))
      return 1;
    p = fdos_dpb_next(p);
  }
  return 0;
}

static void panic_bad_cds_dpb(const char *tag, dos_far_ptr x86_cds,
                              UWORD flags, const char *path, dos_far_ptr cds_dpb)
{
  if (far_is_null(cds_dpb) || dpb_chain_contains(cds_dpb))
    return;

  {
    const dos_far_ptr root_dpb = fdos_lol_dpb();
    printf("BAD CDS DPB[%s]: cds=%04x:%04x flags=%04x path='%s' "
           "cdsDpb=%04x:%04x root_dpb=%04x:%04x nblk=%u lastdrv=%u\n",
           tag,
           FP_SEG(x86_cds), FP_OFF(x86_cds),
           flags, path,
           FP_SEG(cds_dpb), FP_OFF(cds_dpb),
           FP_SEG(root_dpb), FP_OFF(root_dpb),
           fdos_lol_nblkdev(), fdos_dos_lastdrive());

    {
      dos_far_ptr p = root_dpb;
      unsigned guard = 0;
      const unsigned limit = (unsigned)fdos_lol_nblkdev() + 4u;
      while (!far_is_null(p) && !far_is_end(p) && guard++ < limit)
      {
        const dos_far_ptr next = fdos_dpb_next(p);
        const dos_far_ptr device = fdos_dpb_device(p);
        printf("BAD CDS DPB[%s]: chain dpb=%04x:%04x next=%04x:%04x "
               "unit=%u sub=%u dev=%04x:%04x flags=%04x\n",
               tag,
               FP_SEG(p), FP_OFF(p),
               FP_SEG(next), FP_OFF(next),
               fdos_dpb_unit(p), fdos_dpb_subunit(p),
               FP_SEG(device), FP_OFF(device),
               (UWORD)(UBYTE)fdos_dpb_flags(p));
        p = next;
      }
    }
  }

  for (;;) ;
}

static COUNT truename_worker(dos_far_ptr x86_src, const char *src_snapshot,
                             char *dest, COUNT mode)
{
  COUNT i;
  const char *froot;
  COUNT result;
  unsigned state;
  dos_far_ptr x86_cdsEntry;
  char *p = dest;	  /* dynamic pointer into dest */
  char *rootPos;
  char src0;
  const char *src = src_snapshot;
  char cds_path[MAX_CDSPATH];
  UWORD cds_flags;
  dos_far_ptr cds_dpb;
  UWORD cds_backslash_offset;

  TNDBG("TN00 enter x86_src=%04X:%04X mode=%04X snapshot='%s'",
        FP_SEG(x86_src), FP_OFF(x86_src), mode, src);

  TNDBG("TN01 stable guest snapshot src='%s'", src);

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

    fdos_dos_set_current_ldt(MK_FP(0xFFFF, 0xFFFF));
    TNDBG("TN04 UNC return dest='%s'", dest);
    return IS_NETWORK;
  }

  if (src[1] == ':')
    result = drLetterToNr(DosUpFChar(src0));
  else
    result = fdos_dos_default_drive();

  TNDBG("TN05 drive result=%u default=%u src='%s'",
        result, fdos_dos_default_drive(), src);

  dos_far_ptr x86_dhp = IsDevice(src);
  TNDBG("TN06 IsDevice=%p src='%s'", EFFECTIVE(x86_dhp), src);

  x86_cdsEntry = get_cds(result);

  TNDBG("TN07 get_cds(%u)=%04X:%04X lastdrive=%u",
        result, FP_SEG(x86_cdsEntry), FP_OFF(x86_cdsEntry),
        fdos_dos_lastdrive());

  if (far_is_null(x86_cdsEntry))
  {
    if (EFFECTIVE(x86_dhp) && (mode & CDS_MODE_CHECK_DEV_PATH) &&
        (result >= fdos_dos_lastdrive()))
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

      result = fdos_dos_default_drive();
      x86_cdsEntry = get_cds(result);

      TNDBG("TN10 fallback default drive result=%u cds=%04X:%04X",
            result, FP_SEG(x86_cdsEntry), FP_OFF(x86_cdsEntry));

      if (far_is_null(x86_cdsEntry))
        goto invalid_path;
    }
    else
    {
invalid_path:
      TNDBG("TN11 invalid path result=%u src='%s'", result, src);
      return DE_PATHNOTFND;
    }
  }

  cds_flags = fdos_cds_flags(x86_cdsEntry);
  cds_dpb = fdos_cds_dpb(x86_cdsEntry);
  cds_backslash_offset = (UWORD)fdos_cds_backslash_offset(x86_cdsEntry);
  fdos_cds_copy_current_path(x86_cdsEntry, cds_path, sizeof(cds_path));
  panic_bad_cds_dpb("after-load", x86_cdsEntry, cds_flags, cds_path, cds_dpb);
  TNDBG("TN12 CDS path='%s' flags=%04X dpb=%04X:%04X backslash=%u",
        cds_path, cds_flags,
        FP_SEG(cds_dpb), FP_OFF(cds_dpb),
        cds_backslash_offset);

  fdos_dos_set_current_ldt(x86_cdsEntry);

  if (cds_flags & CDSNETWDRV)
    result |= IS_NETWORK;

  if (EFFECTIVE(x86_dhp))
    result |= IS_DEVICE;

  TNDBG("TN13 before QRemote mode=%04X result=%04X src='%s'",  mode, result, src);

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

    cp = cds_path;
    cp[MAX_CDSPATH - 1] = '\0';

    TNDBG("TN23 CDS current cp='%s' flags=%04X", cp, cds_flags);

    if ((cds_flags & CDSNETWDRV) == 0)
    {
      int mc = media_check_tagged(cds_dpb, "truename/CDS.dpb");
      TNDBG("TN25 after media_check rc=%d", mc);

      if (mc < 0) {
        TNDBG("TN26 media_check failed -> DE_PATHNOTFND");
        return DE_PATHNOTFND;
      }
      TNDBG("TN27 before dos_cd cp='%s'", cp);

      if (dos_cd((char *)cp) != SUCCESS) {
        TNDBG("TN28 dos_cd failed cp='%s' backslash=%u",
              cp, cds_backslash_offset);

        cp[cds_backslash_offset + 1] = '\0';
        fdos_cds_current_path_byte(
            x86_cdsEntry, cds_backslash_offset + 1u, 0);

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

      if (cds_flags & CDSSUBST)
      {
        TNDBG("TN33 CDSSUBST dest='%s'", dest);
        if (dest[1] == ':')
        {
          unsigned ii = drLetterToNr(dest[0]);

          TNDBG("TN34 subst real drive ii=%u lastdrive=%u",
                ii, fdos_dos_lastdrive());
          if (ii < fdos_dos_lastdrive())
            result = (result & 0xffe0) | ii;
        }
      }

      rootPos = p = dest + cds_backslash_offset;

      TNDBG("TN35 after root setup p=%u root=%u backslash=%u dest='%s'",
            TNDPTR(p), TNDPTR(rootPos), cds_backslash_offset, dest);
    }
    else
    {
      cp += cds_backslash_offset;

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
    TNDBG("TN42 after sep skip src_ofs=%u src='%s' p=%u", TNPTR(src), src, TNDPTR(p));
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

  if (dest[2] != '/' && (!(mode & CDS_MODE_SKIP_PHYSICAL)) &&
      kernel_lol_read8(offsetof(struct lol, njoined)) != 0)
  {
    const dos_far_ptr cds_base = kernel_lol_read_far(offsetof(struct lol, CDSp));
    const UBYTE lastdrive = fdos_dos_lastdrive();
    char join_path[MAX_CDSPATH];

    TNDBG("TN59 JOIN scan njoined=%u cdsp=%04X:%04X",
          kernel_lol_read8(offsetof(struct lol, njoined)),
          FP_SEG(cds_base), FP_OFF(cds_base));

    for (i = 0; i < lastdrive; ++i)
    {
      const dos_far_ptr entry =
          MK_FP(FP_SEG(cds_base),
                (UWORD)(FP_OFF(cds_base) + (UWORD)i * sizeof(struct cds)));
      const UWORD flags = fdos_cds_flags(entry);
      size_t j;

      fdos_cds_copy_current_path(entry, join_path, sizeof(join_path));
      join_path[MAX_CDSPATH - 1] = '\0';
      j = strlen(join_path);

      TNDBG("TN60 JOIN i=%u j=%u flags=%04X path='%s'",
            i, (unsigned)j, flags, join_path);
      if ((flags & CDSJOINED) &&
          (dest[j] == '\\' || dest[j] == '\0') &&
          memcmp(dest, join_path, j) == 0)
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
        fdos_dos_set_current_ldt(entry);
        result &= ~IS_NETWORK;

        if (flags & CDSNETWDRV)
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


/* truename() is core0-synchronous and never recursively invokes itself.
 * Keep its legacy pointer-oriented parser on two native scratch strings,
 * while guest source/destination buffers are transferred only through the
 * paging-aware bulk primitives.  This removes the old 128-byte stack
 * snapshot and, crucially, never exposes PriPathBuffer/SecPathBuffer as a
 * host pointer. */
static char truename_src_scratch[FDOS_PATHLEN];
static char truename_dst_scratch[FDOS_PATHLEN];

COUNT truename(dos_far_ptr x86_src, char *dest, COUNT mode)
{
  const uint32_t src_linear = ((uint32_t)FP_SEG(x86_src) << 4) + FP_OFF(x86_src);
  size_t n = guest_strnlen_block(src_linear, FDOS_PATHLEN - 1u);
  if (n >= FDOS_PATHLEN - 1u && pload8(src_linear + (uint32_t)n) != 0)
    n = FDOS_PATHLEN - 1u;
  guest_read_block(src_linear, truename_src_scratch, n);
  truename_src_scratch[n] = '\0';
  return truename_worker(x86_src, truename_src_scratch, dest, mode);
}

COUNT truename_guest(dos_far_ptr x86_src, dos_far_ptr x86_dest, COUNT mode)
{
  COUNT rc = truename(x86_src, truename_dst_scratch, mode);
  if (rc >= SUCCESS)
  {
    const size_t n = strnlen(truename_dst_scratch, FDOS_PATHLEN - 1u);
    guest_write(x86_dest, truename_dst_scratch, n + 1u);
  }
  return rc;
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
  dos_far_ptr x86_dpb = kernel_lol_read_far(offsetof(struct lol, DPBp));
  dos_far_ptr cds_base = kernel_lol_read_far(offsetof(struct lol, CDSp));
  UBYTE lastdrive = kernel_lol_read8(offsetof(struct lol, lastdrive));
  UBYTE nblkdev = kernel_lol_read8(offsetof(struct lol, nblkdev));
  int i;

  for (i = 0; i < lastdrive; i++)
  {
    uint32_t cds_lin = kernel_guest_linear(cds_base) +
                       (uint32_t)i * sizeof(struct cds);
    struct cds c;

    kernel_guest_read(cds_lin, &c, sizeof(c));
    memcpy(c.cdsCurrentPath, "A:\\\0", 4);
    c.cdsCurrentPath[0] += i;

    if (i < nblkdev && !far_is_end(x86_dpb))
    {
      struct dpb d;
      c.cdsDpb = x86_dpb;
      c.cdsFlags = CDSPHYSDRV;
      kernel_guest_read(kernel_guest_linear(x86_dpb), &d, sizeof(d));
      x86_dpb = d.dpb_next;
    }
    else
    {
      c.cdsFlags = 0;
    }

    c.cdsStrtClst = 0xffff;
    c.cdsParam = 0xffff;
    c.cdsStoreUData = 0xffff;
    c.cdsJoinOffset = 2;
    kernel_guest_write(cds_lin, &c, sizeof(c));
  }

  init_setdrive(kernel_lol_read8(offsetof(struct lol, BootDrive)) - 1);

  open(ADD_OFF(x86_FIXED_DATA, offsetof(struct lol, aux_str)), O_RDWR);
  open(ADD_OFF(x86_FIXED_DATA, offsetof(struct lol, con_str)), O_RDWR);

  dup2(STDIN, STDAUX);
  dup2(STDOUT, STDIN);
  dup2(STDOUT, STDERR);

  open(ADD_OFF(x86_FIXED_DATA, offsetof(struct lol, prn_str)), O_WRONLY);
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
  char path[] = "A:-@JUNK@-.TMP";
  UBYTE nblkdev = kernel_lol_read8(offsetof(struct lol, nblkdev));

  for (drive = 'C'; drive < 'A' + nblkdev; drive++)
  {
    path[0] = (char)drive;
    kernel_guest_write(kernel_guest_linear(x86_szLine), path, sizeof(path));
    if ((fileno = open(x86_szLine, O_RDONLY)) >= 0)
      close(fileno);
  }
}

STATIC void init_kernel(CPU* cpu)
{
    COUNT i;

    pstore8(KERNEL_LOL_LINEAR + offsetof(struct lol, os_major), MAJOR_RELEASE);
    pstore8(KERNEL_LOL_LINEAR + offsetof(struct lol, os_setver_major), MAJOR_RELEASE);
    pstore8(KERNEL_LOL_LINEAR + offsetof(struct lol, os_minor), MINOR_RELEASE);
    pstore8(KERNEL_LOL_LINEAR + offsetof(struct lol, os_setver_minor), MINOR_RELEASE);

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
    pstore8(KERNEL_LOL_LINEAR + offsetof(struct lol, lastdrive), 26);

    /*  init_device((struct dhdr FAR *)&blk_dev, NULL, 0, &ram_top); */
    /*  WARNING: dsk_init() must be called prior to update_dcb() to ensure
        _Dyn (start of Dynamic memory block) is the start of drive data table (see getddt() in dsk.c)
     */
    pstore8(kernel_guest_linear(x86_blk_dev) + offsetof(struct dhdr, dh_name),
            dsk_init(cpu));

    PreConfig();
/* Historical debug output dereferenced LoL through a persistent host
   pointer.  LoL is guest-resident now; diagnostics must use lol_ref or
   explicit guest accessors instead. */
    /* Number of units */
    if (pload8(kernel_guest_linear(x86_blk_dev) + offsetof(struct dhdr, dh_name)) > 0) {
        update_dcb(x86_blk_dev);
    }
    /* Now config the temporary file system */
    FsConfig();

    /* Scratch fnodes live in guest RAM (DynAlloc), so they must exist before
       the first file is opened: DoConfig() below reads CONFIG.SYS, and any
       open goes INT 21h/AH=3Dh -> DosOpenSft -> dos_open -> sft_to_fnode(),
       which calls fnode_slot(). (FsConfig()'s CON/AUX/PRN opens above are
       fine either way - DosOpenSft() short-circuits character devices at
       IsDevice() and never reaches dos_open().)

       Timing is safe in both directions:
         - EARLY ENOUGH: DynAlloc() is a fixed bump allocator in DYN_BUFFER
           (DOS_PSP:240Eh). It never consults UmbState/HMAState, and
           MoveKernelToHMA() does not relocate DOS_PSP data (the kernel is
           native ARM here - there is no code image to move), so DOS=HIGH,UMB
           cannot invalidate it. dsk_init()/update_dcb() above already
           DynAlloc() the ddt and DPB arrays, even earlier than this.
         - LATE ENOUGH: it must precede PreConfig2(), which sets
           first_mcb = AlignParagraph(DynLast() + 0Fh) - i.e. the MCB arena
           starts immediately above the end of the Dyn area. Any DynAlloc()
           after that point would hand out memory the MCB arena already owns. */
    fnode_init();

    /* Now process CONFIG.SYS     */
    DoConfig(0);
    DoConfig(1);

#ifdef WITHLFNAPI
    /* Persistent LFN helper fnodes belong to resident guest DOS data.
       (The old comment here implied DoConfig() had to run first to "bring the
       dynamic area up". It does not - Dyn is live from file scope. The only
       real constraint is the PreConfig2() one described above, which this
       still satisfies.) */
    lfnapi_init();
#endif    
    /* initialize near data and MCBs */
    PreConfig2();

    /* and process CONFIG.SYS one last time for device drivers */
    DoConfig(2);

    {
      dos_far_ptr _dpb = kernel_lol_read_far(offsetof(struct lol, DPBp));
      unsigned guard = 0;

      while (!far_is_end(_dpb) && !far_is_null(_dpb) &&
             guard++ < kernel_lol_read8(offsetof(struct lol, nblkdev)) + 4) {
        struct dpb d;
        kernel_guest_read(kernel_guest_linear(_dpb), &d, sizeof(d));
        drv_watch_capture("after-DoConfig2-dpb-device", d.dpb_device, NULL);
        _dpb = d.dpb_next;
      }
    }

    /* Close all (device) files */
    for (i = 0; i < 20; i++)
      close(i);

    /* and do final buffer allocation. */
    PostConfig();

    /* Init the file system one more time     */
    FsConfig();
  
    configDone();

    InitializeAllBPBs();
    dpb_watch_capture_chain("after-InitializeAllBPBs-check");
}

/*
 * Порт main.c init_vectors() (upstream FreeDOS):
 *
 *   for (i = 0x23; i <= 0x3f; i++) setvec(i, empty_handler);
 *   ... затем vectors[] переустанавливает настоящие обработчики
 *   (0x20,21,22,24,25,26,27,28,2a,2f) и int0/1/3/6, 0x1b, 0x29.
 *
 * В этом порту "настоящие обработчики" - это трап-страница FFE0:NN,
 * уже прописанная bios_post()/cpu_install_dos_handlers(), поэтому здесь
 * остаётся ровно недостающая часть: дефолтные ПУСТЫЕ обработчики для
 * векторов, которые ядро обязано обслужить IRET'ом, пока их не
 * перехватят программы. Без этого Ctrl-C уводил INT 23h в
 * no_handler-трап ("no_handled FFE0:0023"), а любой вызов INT 2Ah/2Eh
 * и т.п. печатал ту же диагностику.
 *
 * Пустой обработчик = FFF0:0006 (reusable IRET, pc.c).
 * INT 24h = F000:FF44 "mov al,FAIL; iret" - байты кладёт pc.c, ставит
 * вектор ядро, как в оригинале (kernel.asm _int24_handler в образе ядра).
 */
STATIC void setup_int_vectors(void)
{
  int i;

  for (i = 0x23; i <= 0x3f; i++)
  {
    switch (i)
    {
    case 0x24:                  /* critical error: AL=FAIL; IRET */
      setvec(0x24, MK_FP(0xF000, 0xFF44));
      break;
    case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2f:
    case 0x33:
      /* нативные обработчики порта (FFE0-страница) - эквивалент
         upstream vectors[] / резидентного драйвера мыши: не трогать */
      break;
    case 0x30: case 0x31:
      /* слоты заняты байтами CP/M CALL-5 gateway (PSPInit) */
      break;
    default:
      setvec((UBYTE)i, MK_FP(0xFFF0, 0x0006));  /* empty_handler: IRET */
      break;
    }
  }
  /* upstream vectors[]: int22_handler - тот же пустой IRET */
  setvec(0x22, MK_FP(0xFFF0, 0x0006));
  /* INT 0/1/3/6, 0x1b, 0x29: эквиваленты уже установлены bios_post();
     INT 1/3 указывают на IRET (point2iret), как при debugger_present==0 */
}

STATIC void prep_shell(CPU* cpu)
{
  CommandTail Cmd;
  dpb_watch_check_chain("prep_shell-entry");
  if (pload8(kernel_guest_linear(x86_master_env)) == 0) {
    static const char default_env[] = "PATH=.\0\0\0\0";
    kernel_guest_write(kernel_guest_linear(x86_master_env),
                       default_env, sizeof(default_env));
  }

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
  dpb_watch_check_chain("prep_shell-before-P_0");
  cpu_set_a20(cpu, 1);
  SET_DS ( DOS_PSP );
  P_0(cpu, &Config);
  dpb_watch_check_chain("prep_shell-after-P_0");
  __unreachable();
}

void kernel(CPU* _cpu) {
    cpu = _cpu;
    /* Fixed guest-resident objects can cross paging pages.  Initialize them
       through guest writes rather than treating the first mapped page as a
       contiguous native buffer. */
    kernel_guest_write(kernel_guest_linear(x86_con_dev), &_con_dev, sizeof(struct dhdr));
    kernel_guest_write(kernel_guest_linear(x86_prn_dev), &_prn_dev, sizeof(struct dhdr));
    kernel_guest_write(kernel_guest_linear(x86_aux_dev), &_aux_dev, sizeof(struct dhdr));
    kernel_guest_write(kernel_guest_linear(x86_lpt1_dev), &_lpt1_dev, sizeof(struct dhdr));
    kernel_guest_write(kernel_guest_linear(x86_lpt2_dev), &_lpt2_dev, sizeof(struct dhdr));
    kernel_guest_write(kernel_guest_linear(x86_lpt3_dev), &_lpt3_dev, sizeof(struct dhdr));
    kernel_guest_write(kernel_guest_linear(x86_com1_dev), &_com1_dev, sizeof(struct dhdr));
    kernel_guest_write(kernel_guest_linear(x86_com2_dev), &_com2_dev, sizeof(struct dhdr));
    kernel_guest_write(kernel_guest_linear(x86_com3_dev), &_com3_dev, sizeof(struct dhdr));
    kernel_guest_write(kernel_guest_linear(x86_com4_dev), &_com4_dev, sizeof(struct dhdr));
    kernel_guest_write(kernel_guest_linear(x86_clk_dev), &_clk_dev, sizeof(struct dhdr));
    kernel_guest_write(kernel_guest_linear(x86_blk_dev), &_blk_dev, sizeof(struct dhdr));

    init_nls_hardcoded();

    kernel_guest_write(kernel_guest_linear(x86_FIXED_DATA), &lol, sizeof(struct lol));
    kernel_guest_fill(kernel_guest_linear(x86_INTERNAL_DATA), 0,
                      sizeof(struct dos_data));

    /* LoL/SDA are guest-resident only.  There is deliberately no persistent
       host-pointer alias: every access goes through guest refs/primitives. */

    pstore8(KERNEL_IDATA_LINEAR + offsetof(struct dos_data, switchar), '/');
    pstore8(KERNEL_IDATA_LINEAR + offsetof(struct dos_data, net_set_count), 1);
    pstore8(KERNEL_IDATA_LINEAR + offsetof(struct dos_data, CritPatchPad), 0x90);
    pstore8(KERNEL_IDATA_LINEAR + offsetof(struct dos_data, break_ena), 1);
    pstore8(KERNEL_IDATA_LINEAR + offsetof(struct dos_data, DayOfMonth), 1);
    pstore8(KERNEL_IDATA_LINEAR + offsetof(struct dos_data, Month), 1);
    pstore16(KERNEL_IDATA_LINEAR + offsetof(struct dos_data, daysSince1980), 0xffff);
    pstore8(KERNEL_IDATA_LINEAR + offsetof(struct dos_data, DayOfWeek), 2);
    pstore8(KERNEL_IDATA_LINEAR + offsetof(struct dos_data, dosidle_flag), 1);
    pstore16(KERNEL_IDATA_LINEAR + offsetof(struct dos_data, last_component), 0xffff);

    kernel_guest_fill(
        KERNEL_IDATA_LINEAR + offsetof(struct dos_data, sda_tmp_dm_ren),
        0x90,
        sizeof(((struct dos_data *)0)->sda_tmp_dm_ren) +
        sizeof(((struct dos_data *)0)->SearchDir_ren) +
        sizeof(((struct dos_data *)0)->error_stack) +
        sizeof(((struct dos_data *)0)->disk_stack) +
        sizeof(((struct dos_data *)0)->char_stack));

    // adjust boot drive to DOS format
    {
      UBYTE boot_drive = (UBYTE)(CPU_BL + 1);
      if (boot_drive > 0x80)
        boot_drive = 3;
      pstore8(KERNEL_LOL_LINEAR + offsetof(struct lol, BootDrive), boot_drive);
      pstore8(KERNEL_LOL_LINEAR + offsetof(struct lol, cpu), cpu->gen);
    }

    /*
     * INIT guest stack: SDA apistk_top (верх char_stack, DOS_PSP-регион
     * 300h..780h). Ставится до первого сдвига CPU_SP: нативный init-путь
     * (FsConfig()/DoConfig() -> DosOpenSft(), execrh() драйверов,
     * INSTALL=) исполняется раньше первой гостевой инструкции, и SS:SP
     * иначе держали бы reset-значение 0:0000 - любая резервация на
     * гостевом стеке заворачивалась бы в 0000:FFxx поверх Dyn-арены.
     *
     * Почему именно apistk, а не другое место:
     *   - регион принадлежит ядру навсегда и в оригинале несёт ровно
     *     эту нагрузку: entry.asm исполняет INT 21h на этих стеках,
     *     т.е. глубина драйверной инициализации бюджетируется upstream
     *     теми же областями (90h-заливка позволяет измерить фактический
     *     минимум по остатку заполнителя);
     *   - Dyn-арена запрещена: dsk.c/getddt() адресует ddt-массив как
     *     ПЕРВУЮ аллокацию (base = DYN_BUFFER + sizeof(DynS)), любая
     *     вставка раньше dsk_init() сдвигает ddt и лишает диски
     *     геометрии;
     *   - верх conventional запрещён: umb_init() кладёт мостовой MCB в
     *     uppermem_root (ram_top-1, обычно 9FFF) - стек, растущий от
     *     A0000 вниз, затирает мост, после чего seam-guard DosUmbLink()
     *     навсегда отказывает в линковке и UMB-цепь недостижима.
     *
     * Фиксированные слоты SDA_TEMPCDS_OFF/SDA_EXEC_TAIL_OFF занимают
     * вершину disk-региона (см. init-mod.h); init-стек достигает их
     * только при глубине > sizeof(char_stack). fcom process 0 и каждый
     * EXEC-ребёнок ставят собственные SS:SP, так что это значение живёт
     * только в init-фазе.
     */
    SET_SS(DOS_PSP);
    CPU_SP = (UWORD)(X86_INTERNAL_DATA_OFF +
                     offsetof(struct dos_data, char_stack) +
                     sizeof(((struct dos_data *)0)->char_stack));

    /* install DOS API and other interrupt service routines, basic kernel functionality works */
    setup_int_vectors();
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
    printf("KERNEL: Boot drive = %c\n", 'A' + pload8(KERNEL_LOL_LINEAR + offsetof(struct lol, BootDrive)) - 1);
#endif

    dpb_watch_check_chain("kernel-before-DoInstall");
    DoInstall();
    dpb_watch_check_chain("kernel-after-DoInstall");

    /* CONFIG.SYS-only CORE0_STACK_EXT data is dead now. If core0 already
       moved into GFX_BUFFER, grow the FatFs cache from 4 KiB to 8 KiB. */
    core0_expand_relocated_stack_services();

    prep_shell(_cpu);

    __unreachable();
}
