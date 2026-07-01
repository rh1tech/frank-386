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
#define x86_DTA           MK_FP(DOS_PSP, 0x0080) // Disk Transfer Area

#define x86_IO_FIXED_DATA MK_FP(DOS_PSP, 0x07A8) // _IO_FIXED_DATA -> con_dev
#define x86_FIXED_DATA    MK_FP(DOS_PSP, 0x08F0) // _FIXED_DATA -> LoL
#define x86_INTERNAL_DATA MK_FP(DOS_PSP, 0x08F0 + 0x01FB /*+LoL*/) // internal_data

#define x86_DATA          MK_FP(DOS_PSP, 0x13A0) // _DATA - not used? (one byte in chario.c)

#define x86_BSS           MK_FP(DOS_PSP, 0x19F4) // _BSS -> DiskTransferBuffer[MAX_SEC_SIZE=512]
#define x86_SZ_LINE       MK_FP(DOS_PSP, 0x19F4 + MAX_SEC_SIZE) // _BSS + MAX_SEC_SIZE = 0x1BF4
#define x86_DAP           MK_FP(DOS_PSP, 0x19F4 + MAX_SEC_SIZE + 256) // = 0x1CF4 /* 16 */
#define x86_MASTER_ENV    MK_FP(DOS_PSP, 0x19F4 + MAX_SEC_SIZE + 256 + 16) // = 0x1D04 /* 128 */

extern const struct _KernelConfig InitKernelConfig;
extern UWORD HMAFree;            /* first byte in HMA not yet used      */
extern struct config Config;
extern BYTE DOSFAR ASM HaltCpuWhileIdle;
extern const dos_far_ptr x86_dap;
extern const dos_far_ptr x86_master_env;
extern dos_far_ptr lpTop;
extern UWORD ram_top;
extern char singleStep;
extern char SkipAllConfig;

/*
    data shared between DSK.C and INITDISK.C
*/
extern UWORD DOSFAR LBA_WRITE_VERIFY;

void init_PSPSet(CPU* cpu, u16 psp);
void Init_clk_driver(CPU* cpu);
COUNT dsk_init(CPU* cpu);
void PreConfig(void);
VOID PreConfig2(VOID);
VOID PostConfig(VOID);
VOID configDone(VOID);
VOID DoInstall(void);
void P_0(CPU* cpu, struct config*);
/// TODO:
inline static int MoveKernelToHMA() { return 0; }
dos_far_ptr HMAalloc(COUNT bytesToAllocate);
dos_far_ptr KernelAllocPara(size_t nPara, char type, char *name, int mode);
dos_far_ptr KernelAlloc(size_t nBytes, char type, int mode);
int init_DosOpen(dos_far_ptr pathname, int flags);
int read(int fd, dos_far_ptr dst, COUNT sz);
int close(int fd);
dos_far_ptr linear_to_far(const BYTE *p);
VOID DoConfig(int nPass);
void BIOS_drive_reset(CPU* cpu, unsigned drive);
void blockio(CPU* cpu, request FAR *rq);

inline static void rq_done(request FAR *rq) {
    rq->r_status = S_DONE;
}
inline static void rq_busy_done(request FAR *rq) {
    rq->r_status = S_DONE | S_BUSY;
}
inline static void rq_error(request FAR *rq, UBYTE err) {
    rq->r_status = S_ERROR | S_DONE | err;
}

static inline bool far_is_null(dos_far_ptr p) {
    return FP_SEG(p) == 0 && FP_OFF(p) == 0;
}
static inline bool far_is_end(dos_far_ptr p) {
    return FP_SEG(p) == 0xffff && FP_OFF(p) == 0xffff;
}
