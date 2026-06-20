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
typedef void (*cpu_step_t)(struct CPU* cpu, int stepcount);
typedef void (*cpu_raise_irq_t)(struct CPU* cpu);
typedef void (*cpu_setexc_t)(struct CPU* cpu, int excno, uword excerr);
typedef void (*cpu_abort_t)(struct CPU* cpu, int code);

typedef bool (*bios_callback_t)(struct CPU* cpu, void* any);

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
	set_flags_t set_flags;
	cpu_enable_fpu_t enable_fpu;
	cpu_reset_t reset;
	cpu_step_t step;
	cpu_raise_irq_t raise_irq;
	cpu_setexc_t setexc;
	cpu_abort_t abort;
    bios_callback_t bios_callback;
	void* bios_callback_data;
} CPU_ext_accessors_t;

typedef union {
    uint32_t value;
    struct {
        unsigned CF : 1;  // 0 bit of value
        unsigned _1 : 1;  // 1
        unsigned PF : 1;  // 2
        unsigned _3 : 1;  // 3
        unsigned AF : 1;  // 4
        unsigned _5 : 1;  // 5
        unsigned ZF : 1;  // 6
        unsigned SF : 1;  // 7
        unsigned TF : 1;  // 8
        unsigned IF : 1;  // 9
        unsigned DF : 1;  // 10
        unsigned OF : 1;  // 11
        unsigned _12 : 1;
        unsigned _13 : 1;
        unsigned _14 : 1;
        unsigned _15 : 1;
        unsigned _16 : 1;
        unsigned _17 : 1;
        unsigned AC : 1; // 18 (Alignment Check)	Проверка выравнивания (включается в CPL=3 при CR0.AM=1)
                         // (Alignment Check Exception) — INT 17 (11h)
        unsigned VIF : 1; // 19 (Virtual Interrupt Flag)	Виртуальный IF для виртуализации (введён в 486, но зарезервирован с 386)
        unsigned VIP : 1; // 20 (Virtual Interrupt Pending)	Виртуальное прерывание ожидает (аналогично — введён в 486)
        unsigned ID : 1; // 21 (ID Flag)	Позволяет проверить поддержку CPUID инструкцией
    } bits;
} x86_flags_t;

struct CPU {
	union {
		u32 r32;
		u16 r16;
		u8 r8[2];
	} gprx[8];
	uword ip, next_ip;
	x86_flags_t flags;
	uword flags_mask;

	bool intr;

	int gen;
	u32 a20_mask;  /* 0xFFFFFFFF = A20 on, 0xFFEFFFFF = A20 off */
	CPU_ext_accessors_t* ext_accessors;
	CPU_CB cb;

	int excno;
	uword excerr;

	cpu_int_hook_t* int_hooks[CPU_INT_COUNT];

	FPU *fpu;
}; // should be the same in all implementations

typedef struct CPU CPU;

CPU *cpu_new(int gen, CPU_CB **cb);
inline static void enable_fpu(CPU *cpu) {
	cpu->ext_accessors->enable_fpu(cpu);
}
inline static void cpu_reset(CPU *cpu) {
	cpu->ext_accessors->reset(cpu);
}
inline static void cpu_step(CPU *cpu, int stepcount) {
	cpu->ext_accessors->step(cpu, stepcount);
}
inline static void cpu_raise_irq(CPU *cpu) {
	cpu->ext_accessors->raise_irq(cpu);
}
inline static void cpu_setexc(CPU *cpu, int excno, uword excerr) {
	cpu->ext_accessors->setexc(cpu, excno, excerr);
}
inline static void cpu_setflags(CPU *cpu, uword set_mask, uword clear_mask) {
	cpu->ext_accessors->set_flags(cpu, set_mask, clear_mask);
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

#define CPU_IP    (*(uint16_t*)&(cpu->ip))
#define StepIP(x) CPU_IP += x

#define CPU_AX    cpu->gprx[regax].r16
#define CPU_BX    cpu->gprx[regbx].r16
#define CPU_CX    cpu->gprx[regcx].r16
#define CPU_DX    cpu->gprx[regdx].r16
#define CPU_SI    cpu->gprx[regsi].r16
#define CPU_DI    cpu->gprx[regdi].r16
#define CPU_BP    cpu->gprx[regbp].r16
#define CPU_SP    cpu->gprx[regsp].r16

#define CPU_AL    cpu->gprx[regax].r8[0]
#define CPU_AH    cpu->gprx[regax].r8[1]
#define CPU_BL    cpu->gprx[regbx].r8[0]
#define CPU_BH    cpu->gprx[regbx].r8[1]
#define CPU_CL    cpu->gprx[regcx].r8[0]
#define CPU_CH    cpu->gprx[regcx].r8[1]
#define CPU_DL    cpu->gprx[regdx].r8[0]
#define CPU_DH    cpu->gprx[regdx].r8[1]

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

void cpu_init_286(CPU* cpu);

/* Read one CMOS register via I/O ports (matches what real BIOS does). */
static inline uint8_t cmos_read(CPU* cpu, uint8_t reg)
{
	
	cpu->cb.io_write8(cpu->cb.io, 0x70, reg);
    return cpu->cb.io_read8(cpu->cb.io, 0x71);
}

/* Write one CMOS register via I/O ports. */
static inline void cmos_write(CPU* cpu, uint8_t reg, uint8_t val)
{
	cpu->cb.io_write8(cpu->cb.io, 0x70, reg);
	cpu->cb.io_write8(cpu->cb.io, 0x70, val);
}


#endif /* I386_H */
