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

/* CPU callback structure - must be defined before CPU */
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

#define CPU_INT_COUNT 256
struct CPU;
typedef bool (*cpu_int_handler_t)(struct CPU *cpu, void *opaque);
typedef struct cpu_int_hook {
    cpu_int_handler_t handler;
    void *opaque;
} cpu_int_hook_t;

typedef u8 (*get_reg8_t)(struct CPU* cpu, u8 regn);
typedef u16 (*get_reg16_t)(struct CPU* cpu, u8 regn);
typedef u32 (*get_reg32_t)(struct CPU* cpu, u8 regn);
typedef u32 (*get_flags_t)(struct CPU* cpu, u32 mask);

typedef void (*set_reg8_t)(struct CPU* cpu, u8 regn, u8 v);
typedef void (*set_reg16_t)(struct CPU* cpu, u8 regn, u16 v);
typedef void (*set_reg32_t)(struct CPU* cpu, u8 regn, u32 v);
typedef void (*set_flag_t)(struct CPU* cpu, u32 mask, bool v);
typedef void (*set_flags_t)(struct CPU* cpu, uword set_mask, uword clear_mask);

typedef void (*cpu_enable_fpu_t)(struct CPU* cpu);
typedef void (*cpu_reset_t)(struct CPU* cpu);
typedef void (*cpu_reset_pm_t)(struct CPU* cpu, uint32_t start_addr);
typedef void (*cpu_step_t)(struct CPU* cpu, int stepcount);
typedef void (*cpu_raise_irq_t)(struct CPU* cpu);
typedef void (*cpu_setexc_t)(struct CPU* cpu, int excno, uword excerr);
typedef void (*cpu_abort_t)(struct CPU* cpu, int code);

typedef struct CPU_ext_accessors {
	get_reg8_t get_reg8;
	get_reg16_t get_reg16;
	get_reg32_t get_reg32;
	set_reg8_t set_reg8;
	set_reg16_t set_reg16;
	set_reg32_t set_reg32;
	get_reg16_t get_seg16;
	set_reg16_t set_seg16;
	get_flags_t get_flags;
	set_flag_t set_flag;
	set_flags_t setflags;
	cpu_enable_fpu_t enable_fpu;
	cpu_reset_t reset;
	cpu_reset_pm_t reset_pm;
	cpu_step_t step;
	cpu_raise_irq_t raise_irq;
	cpu_setexc_t setexc;
	cpu_abort_t abort;
} CPU_ext_accessors_t;

struct CPU {
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

	int gen;
	cpu_int_hook_t* int_hooks[CPU_INT_COUNT];
	u32 a20_mask;  /* 0xFFFFFFFF = A20 on, 0xFFEFFFFF = A20 off */
	CPU_ext_accessors_t* ext_accessors;
}; // should be the same in all implementations

typedef struct CPU CPU;

CPU *cpui386_new(int gen, CPU_CB **cb);
inline static void cpui386_enable_fpu(CPU *cpu) {
	cpu->ext_accessors->enable_fpu(cpu);
}
inline static void cpui386_reset(CPU *cpu) {
	cpu->ext_accessors->reset(cpu);
}
inline static void cpui386_reset_pm(CPU *cpu, uint32_t start_addr) {
	cpu->ext_accessors->reset_pm(cpu, start_addr);
}
inline static void cpui386_step(CPU *cpu, int stepcount) {
	cpu->ext_accessors->step(cpu, stepcount);
}
inline static void cpui386_raise_irq(CPU *cpu) {
	cpu->ext_accessors->raise_irq(cpu);
}
inline static void cpu_setexc(CPU *cpu, int excno, uword excerr) {
	cpu->ext_accessors->setexc(cpu, excno, excerr);
}
inline static void cpu_setflags(CPU *cpu, uword set_mask, uword clear_mask) {
	cpu->ext_accessors->setflags(cpu, set_mask, clear_mask);
}
inline static uword cpu_getflags(CPU *cpu) {
	return cpu->ext_accessors->get_flags(cpu, ~0);
}
inline static void cpu_abort(CPU *cpu, int code) {
	cpu->ext_accessors->abort(cpu, code);
}

// Register accessors for disk/BIOS emulation
// 8-bit registers
#define AL_REG_IDX 0
#define CL_REG_IDX 1
#define DL_REG_IDX 2
#define BL_REG_IDX 3
#define AH_REG_IDX 4
#define CH_REG_IDX 5
#define DH_REG_IDX 6
#define BH_REG_IDX 7
// 16-bit registers
#define AX_REG_IDX 0
#define CX_REG_IDX 1
#define DX_REG_IDX 2
#define BX_REG_IDX 3
#define SP_REG_IDX 4
#define BP_REG_IDX 5
#define SI_REG_IDX 6
#define DI_REG_IDX 7

enum {
	SEG_ES = 0,
	SEG_CS,
	SEG_SS,
	SEG_DS,
	SEG_FS,
	SEG_GS,
	SEG_LDT,
	SEG_TR,
};

enum {
	CF = 0x1,
	/* 1 0x2 */
	PF = 0x4,
	/* 0 0x8 */
	AF = 0x10,
	/* 0 0x20 */
	ZF = 0x40,
	SF = 0x80,
	TF = 0x100,
	IF = 0x200,
	DF = 0x400,
	OF = 0x800,
	IOPL = 0x3000,
	NT = 0x4000,
	/* 0 0x8000 */
	RF = 0x10000,
	VM = 0x20000,
};

// A20 gate control
void cpu_set_a20(CPU *cpu, int enabled);
int cpu_get_a20(CPU *cpu);

cpu_int_hook_t* cpu_set_int_hook(CPU *cpu, u8 no, cpu_int_hook_t* hook);

/* Profiling support (enable with -DI386_PROFILE) */
#ifdef I386_PROFILE
void i386_profile_dump(void);
void i386_profile_reset(void);
void i386_profile_dump_sd_and_reset(const char *reason);
void i386_profile_install_bios_hooks(CPU *cpu);
#endif

#endif /* I386_H */
