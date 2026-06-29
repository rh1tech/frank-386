#include "i386.h"

#define MAX_HARD_DRIVE  4

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
#define x86_FIXED_DATA MK_FP(DOS_PSP, 0x08F0) // _FIXED_DATA -> LoL
#define x86_DTA MK_FP(DOS_PSP, 0x0080) // Disk Transfer Area
#define x86_IO_FIXED_DATA MK_FP(DOS_PSP, 0x07A8) // _IO_FIXED_DATA -> con_dev
#define x86_INTERNAL_DATA MK_FP(DOS_PSP, 0x08F0 + 0x01FB) // internal_data
#define x86_DATA MK_FP(DOS_PSP, 0x13A0) // _DATA
#define x86_BSS MK_FP(DOS_PSP, 0x19F4) // _BSS

extern const struct _KernelConfig InitKernelConfig;
extern UWORD HMAFree;            /* first byte in HMA not yet used      */
extern struct config Config;
extern BYTE DOSFAR ASM HaltCpuWhileIdle;
extern dos_far_ptr x86_dap;
extern dos_far_ptr lpTop;
/*
    data shared between DSK.C and INITDISK.C
*/
extern UWORD DOSFAR LBA_WRITE_VERIFY;

void init_PSPSet(CPU* cpu, u16 psp);
void Init_clk_driver(CPU* cpu);
COUNT dsk_init(CPU* cpu);
void PreConfig(void);
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
