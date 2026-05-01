#ifndef I386_H
#define I386_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

typedef int32_t s32;
typedef int16_t s16;
typedef int8_t s8;

typedef u32 uword;
typedef s32 sword;

#define PREFETCH_ENABLED 1

/* Enable optimized register layout (union-based) */
#ifndef I386_OPT1
#define I386_OPT1
#endif

/* Forward declaration for FPU */
typedef struct FPU FPU;

/* CPU callback structure - must be defined before CPUI386 */
typedef struct {
	void *pic;
	int (*pic_read_irq)(void *);

	void *io;
	u8 (*io_read8)(void *, int);
	void (*io_write8)(void *, int, u8);
	u16 (*io_read16)(void *, int);
	void (*io_write16)(void *, int, u16);
	u32 (*io_read32)(void *, int);
	void (*io_write32)(void *, int, u32);
	int (*io_read_string)(void *, int, uint32_t, int, int);
	int (*io_write_string)(void *, int, uint32_t, int, int);

	void *iomem;
} CPU_CB;

/* TLB entry structure */
struct tlb_entry {
	uword lpgno;
	uword xaddr;
	int (*pte_lookup)[2];
	u8 *ppte;
};

/*
 * CPUI386 structure - main CPU state
 * Defined here so JIT compiler can access fields directly
 */
struct CPUI386 {
#ifdef I386_OPT1
	union {
		u32 r32;
		u16 r16;
		u8 r8[2];
	} gprx[8];
#else
	uword gpr[8];
#endif
	uword ip, next_ip;
	uword flags;
	uword flags_mask;
	int cpl;
	bool code16;
	uword sp_mask;
	bool halt;

	FPU *fpu;

	struct {
		uword sel;
		uword base;
		uword limit;
		uword flags;
	} seg[8];

	struct {
		uword base;
		uword limit;
	} idt, gdt;

	uword cr0, cr2, cr3;

	uword dr[8];

	struct {
		unsigned long laddr;
		uword xaddr;
	} ifetch;

	struct {
		int op;
		uword dst;
		uword dst2;
		uword src1;
		uword src2;
		uword mask;
	} cc;

	struct {
		int size;
		struct tlb_entry *tab;
	} tlb;
#if PREFETCH_ENABLED
/* Prefetch buffer: holds 4 bytes fetched as one 32-bit aligned read.
 * cpu->prefetch_base is the physical address of the aligned 4-byte slot currently
 * in the buffer (always a multiple of 4).  (u32)-1 means "invalid / empty".
 * Invalidated automatically when the physical address of next_ip falls outside
 * the current 4-byte slot */
	u32 prefetch_base;
	u8  prefetch[16] __attribute__((aligned(4)));
#endif
	long cycle;

	int excno;
	uword excerr;

	bool intr;
	CPU_CB cb;

	int gen;
	struct {
		uword cs, eip, esp;
	} sysenter;

	/* INT 2Fh network attached drive handler hook */
	bool (*int2f_handler)(struct CPUI386 *cpu, void *opaque);
	void *int2f_opaque;

	u32 a20_mask;  /* 0xFFFFFFFF = A20 on, 0xFFEFFFFF = A20 off */
};

typedef struct CPUI386 CPUI386;

CPUI386 *cpui386_new(int gen, CPU_CB **cb);
void cpui386_delete(CPUI386 *cpu);
void cpui386_enable_fpu(CPUI386 *cpu);
void cpui386_reset(CPUI386 *cpu);
void cpui386_reset_pm(CPUI386 *cpu, uint32_t start_addr);
void cpui386_step(CPUI386 *cpu, int stepcount);
void cpui386_raise_irq(CPUI386 *cpu);
void cpui386_set_gpr(CPUI386 *cpu, int i, u32 val);
long cpui386_get_cycle(CPUI386 *cpu);
void cpui386_get_state(CPUI386 *cpu, uint32_t *cs, uint32_t *ip, int *halt);

bool cpu_load8(CPUI386 *cpu, int seg, uword addr, u8 *res);
bool cpu_store8(CPUI386 *cpu, int seg, uword addr, u8 val);
bool cpu_load16(CPUI386 *cpu, int seg, uword addr, u16 *res);
bool cpu_store16(CPUI386 *cpu, int seg, uword addr, u16 val);
bool cpu_load32(CPUI386 *cpu, int seg, uword addr, u32 *res);
bool cpu_store32(CPUI386 *cpu, int seg, uword addr, u32 val);
void cpu_setax(CPUI386 *cpu, u16 ax);
u16 cpu_getax(CPUI386 *cpu);
void cpu_setexc(CPUI386 *cpu, int excno, uword excerr);
void cpu_setflags(CPUI386 *cpu, uword set_mask, uword clear_mask);
uword cpu_getflags(CPUI386 *cpu);
void cpu_abort(CPUI386 *cpu, int code);

// Register accessors for disk/BIOS emulation
// 8-bit registers
u8 cpu_get_al(CPUI386 *cpu);
u8 cpu_get_ah(CPUI386 *cpu);
u8 cpu_get_bl(CPUI386 *cpu);
u8 cpu_get_bh(CPUI386 *cpu);
u8 cpu_get_cl(CPUI386 *cpu);
u8 cpu_get_ch(CPUI386 *cpu);
u8 cpu_get_dl(CPUI386 *cpu);
u8 cpu_get_dh(CPUI386 *cpu);
void cpu_set_al(CPUI386 *cpu, u8 val);
void cpu_set_ah(CPUI386 *cpu, u8 val);
void cpu_set_bl(CPUI386 *cpu, u8 val);
void cpu_set_bh(CPUI386 *cpu, u8 val);
void cpu_set_cl(CPUI386 *cpu, u8 val);
void cpu_set_ch(CPUI386 *cpu, u8 val);
void cpu_set_dl(CPUI386 *cpu, u8 val);
void cpu_set_dh(CPUI386 *cpu, u8 val);
// 16-bit registers
u16 cpu_get_bx(CPUI386 *cpu);
u16 cpu_get_cx(CPUI386 *cpu);
u16 cpu_get_dx(CPUI386 *cpu);
u16 cpu_get_es(CPUI386 *cpu);
void cpu_set_bx(CPUI386 *cpu, u16 val);
void cpu_set_cx(CPUI386 *cpu, u16 val);
void cpu_set_dx(CPUI386 *cpu, u16 val);
// Carry flag
u16 cpu_get_bp(CPUI386 *cpu);
u16 cpu_get_si(CPUI386 *cpu);
u16 cpu_get_di(CPUI386 *cpu);
void cpu_set_si(CPUI386 *cpu, u16 val);
void cpu_set_di(CPUI386 *cpu, u16 val);
u16 cpu_get_ds(CPUI386 *cpu);
u16 cpu_get_ss(CPUI386 *cpu);
void cpu_set_cf(CPUI386 *cpu, int val);
int cpu_get_cf(CPUI386 *cpu);
// A20 gate control
void cpu_set_a20(CPUI386 *cpu, int enabled);
int cpu_get_a20(CPUI386 *cpu);

typedef bool (*int2f_handler_t)(CPUI386 *cpu, void *opaque);
void cpu_set_int2f_handler(CPUI386 *cpu, int2f_handler_t handler, void *opaque);

/* Profiling support (enable with -DI386_PROFILE) */
#ifdef I386_PROFILE
void i386_profile_dump(void);
void i386_profile_reset(void);
#endif

#endif /* I386_H */
