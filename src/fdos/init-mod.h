#include "i386.h"

#define MAX_HARD_DRIVE  4

#define GLOBAL extern
#define NAMEMAX         MAX_CDSPATH     /* Maximum path for CDS         */
#define NFILES          16      /* number of files in table     */
#define NFCBS           16      /* number of fcbs               */
#define NSTACKS         8       /* number of stacks             */
#define STACKSIZE       256     /* default stacksize            */
#define NLAST           5       /* last drive                   */
#define NUMBUFF         20      /* Number of track buffers at INIT time     */
                                        /* -- must be at least 3        */
///#define MAX_HARD_DRIVE  8
#define NDEV            26      /* up to Z:                     */

#define open        init_DosOpen

/*
 00000H 000FFH 00100H PSP                PSP
 00100H 004E1H 003E2H _TEXT              CODE
 004E2H 007A7H 002C6H _IO_TEXT           CODE
 007A8H 008E5H 0013EH _IO_FIXED_DATA     CODE
 008F0H 0139FH 00AB0H _FIXED_DATA        DATA
 013A0H 019F3H 00654H _DATA              DATA
 019F4H 0240DH 00A1AH _BSS               BSS
*/
/*
 * DOS_PSP:0000..00FF is the kernel's own PSP.  Its second half doubles as the
 * master environment - this is NOT an accident of the port, it is what
 * kernel.asm does:
 *
 *      beyond_entry:   times 256-(beyond_entry-entry) db 0
 *                                      ; scratch area for data (DOS_PSP)
 *      _master_env equ $ - 128
 *      global  _master_env
 *
 * i.e. master_env == DOS_PSP:0080, the same 128 bytes as the PSP command tail
 * and the default DTA (main.c: set_DTA(MK_FP(DOS_PSP, 0x80))).
 *
 * It works because of the init order, which this port keeps identical to
 * main.c:
 *      set_DTA(DOS_PSP:0080) -> PSPInit() -> PreConfig() -> DoConfig(0/1/2)
 *      -> PostConfig() -> "if (master_env[0] == 0) master_env = PATH=."
 *
 * PSPInit() writes the (empty) command tail at 0080h, and only afterwards do
 * the SET= lines of CONFIG.SYS start filling the environment from 0080h up.
 * The kernel's own DTA is never used for a FindFirst/FCB call - neither during
 * init nor later, since every process gets its own PSP and DTA - so nothing
 * ever writes 128 bytes of directory entry over the master environment.
 *
 * Do not "fix" this by moving one of them: any DOS program that walks the
 * master environment MCB expects it exactly here.
 */
#define x86_DTA           MK_FP(DOS_PSP, 0x0080) // Disk Transfer Area
#define x86_MASTER_ENV    MK_FP(DOS_PSP + 8, 0)  // == DOS_PSP:0080, see above

#define x86_IO_FIXED_DATA MK_FP(DOS_PSP, 0x07A8) // _IO_FIXED_DATA -> con_dev
#define x86_FIXED_DATA    MK_FP(DOS_PSP, 0x08F0) // _FIXED_DATA -> LoL

#define X86_INTERNAL_DATA_OFF   (0x08F0u + 0x01FBu /*+LoL*/)
#define x86_INTERNAL_DATA       MK_FP(DOS_PSP, X86_INTERNAL_DATA_OFF) // internal_data

/*
 * _DATA must begin AFTER the end of internal_data.
 *
 * In the original kernel the API stacks (error_tos/disk_api_tos/char_api_tos)
 * live in a separate ASM stack segment (hdr/stacks.inc); here they are members
 * of struct dos_data.  That makes sizeof(struct dos_data) == 0AE5h, while the
 * original map leaves only 08B5h between _FIXED_DATA+LoL (0AEBh) and _DATA
 * (13A0h).  With _DATA hardcoded at 13A0h, internal_data ran to 15D0h and
 * therefore *contained* nlsInfo (13A4h) and nlsPackageHardcoded (13B4h):
 *   - memset(internal_data, 0, sizeof(struct dos_data)) in kernel() zeroed
 *     them right after init_nls_hardcoded() had filled them in;
 *   - the API stacks (SDA+480h..900h -> 1090h..1510h) kept overwriting them
 *     on every INT 21h call.
 * Result: nlsInfo.actPkg/chain == 0 -> searchPackage() fails -> DosGetData()
 * falls through to MUX-14 -> DE_FILENOTFND, i.e. DosGetCountryInformation()
 * (INT 21h AH=38h) always returned an error.
 *
 * Derive the offset instead of hardcoding it, and assert the result.
 */
#define X86_DATA_OFF            (X86_INTERNAL_DATA_OFF + sizeof(struct dos_data))
#define x86_DATA                MK_FP(DOS_PSP, X86_DATA_OFF) // _DATA one byte in chario.c + 3 (adjust to 32-bit)

#define X86_NLS_INFO_OFF        (X86_DATA_OFF + 4u)
#define x86_nlsInfo             MK_FP(DOS_PSP, X86_NLS_INFO_OFF) // _DATA struct nlsInfoBlock
#define x86_nlsPackageHardcoded MK_FP(DOS_PSP, X86_NLS_INFO_OFF + sizeof(struct nlsInfoBlock)) // struct nlsPackage
/* hardcoded NLS tables: upcase+fupcase+fname+collate+dbcs (sizes in nls.h) */
#define X86_NLS_HC_TABLE_BYTES  (NLS_HC_TBL2_SIZE + NLS_HC_TBL4_SIZE \
                                 + NLS_HC_TBL5_SIZE + NLS_HC_TBL6_SIZE \
                                 + NLS_HC_TBL7_SIZE)
#define X86_NLS_SCRATCH_OFF     (X86_NLS_INFO_OFF + sizeof(struct nlsInfoBlock) + sizeof(struct nlsPackage) + X86_NLS_HC_TABLE_BYTES)
#define x86_nlsScratch          MK_FP(DOS_PSP, X86_NLS_SCRATCH_OFF)
#define x86_nlsEntries          ADD_OFF(x86_nlsScratch, 0) // UWORD
#define x86_nlsCount            ADD_OFF(x86_nlsScratch, sizeof(UWORD)) // UWORD
#define x86_subf_hdr            ADD_OFF(x86_nlsScratch, sizeof(UWORD) + sizeof(UWORD)) // struct subf_hdr * 9
#define x86_subf_data           ADD_OFF(x86_nlsScratch, sizeof(UWORD) + sizeof(UWORD) + sizeof(struct subf_hdr) * 9) // struct subf_data
#define x86_subf_data_buffer    ADD_OFF(x86_subf_data, offsetof(struct subf_data, buffer)) // internal in pref
#define X86_NLS_SCRATCH_SIZE    (sizeof(UWORD) + sizeof(UWORD) \
                                 + sizeof(struct subf_hdr) * 9 + sizeof(struct subf_data))
/* One guest-addressable byte. The original upcases a single character with
   xUpMem(nls, MK_FP(_SS, &ch), 1) - the address must be reachable by the MUX-14
   handler through ES:DI, so an ARM stack local will not do. */
#define x86_nlsUpChar           ADD_OFF(x86_nlsScratch, X86_NLS_SCRATCH_SIZE)
#define X86_NLS_END_OFF         (X86_NLS_SCRATCH_OFF + X86_NLS_SCRATCH_SIZE + 1u)

#define X86_BSS_OFF       0x19F4u
#define x86_BSS           MK_FP(DOS_PSP, X86_BSS_OFF) // _BSS -> DiskTransferBuffer[MAX_SEC_SIZE=512]
#define x86_SZ_LINE       MK_FP(DOS_PSP, X86_BSS_OFF + MAX_SEC_SIZE) // _BSS + MAX_SEC_SIZE = 0x1BF4
#define x86_DAP           MK_FP(DOS_PSP, X86_BSS_OFF + MAX_SEC_SIZE + 256) // = 0x1CF4 /* 16 */
// end = 0x1D04 /* 128 */

/* Конфиг-буфер szBuf. В оригинале szBuf лежит в DATA-сегменте ядра
   (гостевая память по определению), и именно ЕГО upstream отдаёт в
   open()/init_DosExec() для имён CONFIG.SYS и путей DEVICE=/INSTALL=
   (config.c: init_DosExec(3, &eb, szBuf)).Гостевой szBuf:
    у каждого имени свой буфер, SDA не участвует. */
#define X86_SZ_BUF_OFF    (X86_BSS_OFF + MAX_SEC_SIZE + 256u + 16u + 128u) /* = 0x1D84 */
#define x86_SZ_BUF        MK_FP(DOS_PSP, X86_SZ_BUF_OFF)
#define SZ_BUF_LEN        256u
_Static_assert(X86_SZ_BUF_OFF + SZ_BUF_LEN <= 0x240E /* DYN_BUFFER */,
               "szBuf overruns DYN_BUFFER");

/*
 * Фиксированные слоты на disk API-стеке SDA (internal_data->disk_stack,
 * область [disk_stack .. char_stack)). В оригинале это автоматики
 * kernel-кода: entry.asm переключает SS:SP на _disk_api_tos для
 * дисковых функций INT 21h, и TempCDS truename()/буфер хвоста EXEC
 * живут там как локальные переменные. Порт INT 21h стеки не
 * переключает, поэтому регион свободен и занят теми же объектами по
 * фиксированным адресам:
 *   - TempCDS: truename() не вкладывается сам в себя (вызов всегда
 *     завершается до передачи управления EXEC-ребёнку), одного
 *     экземпляра достаточно;
 *   - хвост EXEC: жив от сборки до fcom_create_process()/patchPSP(),
 *     в т.ч. поперёк ChildEnv() -> truename() - поэтому слоты не
 *     пересекаются; уровни EXEC не накладываются, т.к. хвост
 *     копируется в PSP ребёнка до его запуска.
 * Стек вызывающего процесса несёт тот же контракт INT 21h, что и в
 * MS-DOS: IRET-кадр + образ регистров, без kernel-автоматиков.
 */
#define SDA_DISK_TOS_OFF   ((UWORD)(X86_INTERNAL_DATA_OFF + \
                                    offsetof(struct dos_data, char_stack)))
#define SDA_CHAR_TOS_OFF   ((UWORD)(SDA_DISK_TOS_OFF + \
                                    sizeof(((struct dos_data *)0)->char_stack)))
#define SDA_TEMPCDS_OFF    ((UWORD)((SDA_DISK_TOS_OFF - sizeof(struct cds)) & ~3u))
#define SDA_EXEC_TAIL_LEN  132u
#define SDA_EXEC_TAIL_OFF  ((UWORD)((SDA_TEMPCDS_OFF - SDA_EXEC_TAIL_LEN) & ~3u))
_Static_assert(offsetof(struct dos_data, char_stack) -
               offsetof(struct dos_data, disk_stack)
               >= sizeof(struct cds) + SDA_EXEC_TAIL_LEN + 8,
               "disk_stack too small for TempCDS + EXEC tail slots");

#include "hdr/nls.h"

/* Layout guards: silently overlapping these areas costs days of debugging. */
_Static_assert(X86_DATA_OFF >= X86_INTERNAL_DATA_OFF + sizeof(struct dos_data),
               "_DATA/nlsInfo overlaps internal_data (struct dos_data grew)");
_Static_assert(X86_NLS_END_OFF <= X86_BSS_OFF,
               "NLS data + scratch overruns _BSS");

typedef struct {
  char  ThisIsAConstantOne;
  short TableSize;
  struct CountrySpecificInfo C;
} nlsCountryInfoHardcoded_t;
extern nlsCountryInfoHardcoded_t nlsCountryInfoHardcoded;

extern struct _KernelConfig InitKernelConfig;
extern UWORD HMAFree;            /* first byte in HMA not yet used      */
extern BYTE DosLoadedInHMA;      /* TRUE once DOS=HIGH put the kernel in HMA */
extern struct config Config;
extern BYTE DOSFAR ASM HaltCpuWhileIdle;
extern dos_far_ptr x86_PSP; // == MK_FP(DOS_PSP, 0x0000); // PSP ядра занимает 0060:0000–0060:00FF
extern const dos_far_ptr x86_dap;
extern const dos_far_ptr x86_master_env;
extern dos_far_ptr lpTop;
extern UWORD ram_top;
extern char singleStep;
extern char SkipAllConfig;
/* break_ena lives in guest SDA; access it through SDA guest accessors. */
extern unsigned char DOSTEXTFAR ASM kbdType;
extern const dos_far_ptr _nlsPackageHardcoded;

/*
    data shared between DSK.C and INITDISK.C
*/
extern UWORD DOSFAR LBA_WRITE_VERIFY;

void keycheck(void);
void init_PSPSet(CPU* cpu, u16 psp);
void Init_clk_driver(CPU* cpu);
COUNT dsk_init(CPU* cpu);
void PreConfig(void);
VOID PreConfig2(VOID);
VOID PostConfig(VOID);
VOID configDone(VOID);
VOID DoInstall(void);
void P_0(CPU* cpu, struct config*);
int MoveKernelToHMA(void);
dos_far_ptr HMAalloc(COUNT bytesToAllocate);
dos_far_ptr KernelAllocPara(size_t nPara, char type, char *name, int mode);
dos_far_ptr KernelAlloc(size_t nBytes, char type, int mode);
int init_DosOpen(dos_far_ptr pathname, int flags);
int read(int fd, dos_far_ptr dst, COUNT sz);
int close(int fd);
dos_far_ptr linear_to_far(const void *p);
VOID DoConfig(int nPass);
void BIOS_drive_reset(CPU* cpu, unsigned drive);
int ASMPASCAL init_switchar(int chr);
dos_far_ptr getvec(uint8_t intno);
void setvec(uint8_t intno, dos_far_ptr vec);
BOOL init_device(/*struct dhdr*/ dos_far_ptr x86_dhp, char *cmdLine, COUNT mode, dos_far_ptr * r_top);
COUNT DosExec(COUNT mode, exec_blk FAR * ep, BYTE FAR * lp);
int UMB_get_largest(dos_far_ptr driverAddress, UCOUNT *seg, UCOUNT *size);
ULONG ASMPASCAL lseek(int fd, long position);
dos_far_ptr DetectXMSDriver(void);
int find_fname(const char *path, int attr, f_node_ptr fnp);
COUNT delete_dir_entry(f_node_ptr fnp);

inline static void rq_done(request FAR *rq) {
    rq->r_status = S_DONE;
}
inline static void rq_busy_done(request FAR *rq) {
    rq->r_status = S_DONE | S_BUSY;
}
inline static void rq_error(request FAR *rq, UBYTE err) {
    rq->r_status = S_ERROR | S_DONE | err;
}

/*
    stk_lin(ss, sp, n) - linear address of the word at ss:(sp + n).

    Real-mode PUSH/POP wrap SP inside the stack segment: SP is a 16-bit
    register and 0xFFFE + 2 is 0x0000 of the SAME segment, not the first
    byte of the next one. Written straight out in C,

        ((uint32_t)ss << 4) + sp + n

    the addition promotes to int and does NOT wrap, so for a guest whose SP
    sits near the top of its stack segment DOS would read the wrong word, a
    whole segment away. Masking the offset back to 16 bits reproduces the
    hardware. (fdos_2fh.c already spells this out by hand as
    "(UWORD)(CPU_SP + 6)"; this is that idiom, named.)
*/
static inline uint32_t stk_lin(uint16_t ss, uint16_t sp, int n) {
    return ((uint32_t)ss << 4) + (uint16_t)(sp + n);
}

/*
    guest_stack_alloc(cpu, n) - carve n bytes of scratch off the guest stack
    and return a far pointer to it.

    Several CONFIG.SYS-time helpers borrow guest stack space instead of SRAM
    (which is scarce on this target) to stage data that must live in guest RAM
    - COUNTRY.SYS headers, a device driver's request packet, a synthesised
    command tail. They all did "CPU_SP -= n; ptr = MK_FP(CPU_SS, CPU_SP)" by
    hand, which is fine, but the follow-up "MK_FP(CPU_SS, CPU_SP + k)" to point
    at a sub-field is a raw 16-bit-unmasked add: if SP+k crosses 0xFFFF the
    offset handed to MK_FP is truncated and points somewhere other than the
    bytes just reserved.

    Centralising it: SP decrements as a genuine 16-bit register (wraps in
    hardware fashion, matching a real PUSH), and the returned pointer is built
    from the post-decrement SP so it is always consistent with it. Callers that
    need a sub-field should offset with stk_lin()/MK_FP off the RETURNED
    pointer's segment, or just call this once per field.

    Caller restores SP (the existing "CPU_SP = saved_sp" on the way out).
*/
static inline dos_far_ptr guest_stack_alloc(CPU *cpu, uint16_t n) {
    CPU_SP = (uint16_t)(CPU_SP - n);
    return MK_FP(CPU_SS, CPU_SP);
}

static inline bool far_is_null(dos_far_ptr p) {
    return FP_SEG(p) == 0 && FP_OFF(p) == 0;
}
static inline bool far_is_end(dos_far_ptr p) {
    return FP_SEG(p) == 0xffff && FP_OFF(p) == 0xffff;
}
#if PDB_DEBUG
void drv_watch_set_dpb_context(dos_far_ptr/*struct dpb*/ _dpb, UBYTE cmd, UBYTE unit, UBYTE subunit, const char *source);
void dpb_watch_check_chain(const char *tag);
void dpb_watch_check(const char *tag, dos_far_ptr _dpb);
#else
#define drv_watch_set_dpb_context(...)
#define dpb_watch_check_chain(tag)
#define dpb_watch_check(...)
#define drv_watch_panic_if_bad(...)
#define drv_watch_capture(...)
#define dpb_watch_capture_chain(tag)
#endif
