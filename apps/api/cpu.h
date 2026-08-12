#ifndef __CPU_H__
#define __CPU_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Public CPU ABI: field layout is fixed to 4-byte packing on both sides. */
#pragma pack(push, 4)

#define regax 0
#define regcx 1
#define regdx 2
#define regbx 3
#define regsp 4
#define regbp 5
#define regsi 6
#define regdi 7

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
typedef u16 (*get_reg16_t)(const struct CPU* cpu, u8 regn);
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

struct bios_callback_params;
typedef bool (*bios_callback_t)(struct CPU* cpu, struct bios_callback_params* params);
typedef struct bios_callback_params {
	bios_callback_t callback;
	void* data;
	u16 expected_cs;
	u16 expected_ip;
	struct bios_callback_params* chain;
	bool done;
	const char* owner;
} bios_callback_params_t;

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
        unsigned AC  : 1; // 18 (Alignment Check)
        unsigned VIF : 1; // 19 (Virtual Interrupt Flag)
        unsigned VIP : 1; // 20 (Virtual Interrupt Pending)
        unsigned ID  : 1; // 21 (ID Flag)
    } bits;
} x86_flags_t;

typedef	union {
	u32 r32;
	u16 r16;
	u8 r8[2];
} gprx_t;

struct CPU {
	gprx_t gprx[8];
	uword ip, next_ip;
	x86_flags_t flags;
	uword flags_mask;

	bool intr;

	int gen;
	u32 a20_mask;  /* A20 is modeled as permanently enabled */
	CPU_ext_accessors_t* ext_accessors;
	CPU_CB cb;

	int excno;
	uword excerr;

	const char *bios;
	bool native_done;

/* Prefetch buffer: holds 4 bytes fetched as one 32-bit aligned read.
 * cpu->prefetch_base is the physical address of the aligned 4-byte slot currently
 * in the buffer (always a multiple of 4).  (u32)-1 means "invalid / empty".
 * Invalidated automatically when the physical address of next_ip falls outside
 * the current 4-byte slot */
	u32 prefetch_base;
	u8  prefetch[16] __attribute__((aligned(4)));

	cpu_int_hook_t* int_hooks[CPU_INT_COUNT];

	FPU *fpu;
}; // should be the same in all implementations

typedef struct CPU CPU;

/* ARM32 ABI guards: fail the build instead of silently changing public layout. */
#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(CPU_CB) == 48, "CPU_CB ABI size");
_Static_assert(sizeof(struct tlb_entry) == 16, "tlb_entry ABI size");
_Static_assert(sizeof(cpu_int_hook_t) == 8, "cpu_int_hook_t ABI size");
_Static_assert(sizeof(bios_callback_params_t) == 24, "bios_callback_params_t ABI size");
_Static_assert(sizeof(CPU_ext_accessors_t) == 68, "CPU_ext_accessors_t ABI size");
_Static_assert(sizeof(x86_flags_t) == 4, "x86_flags_t ABI size");
_Static_assert(sizeof(gprx_t) == 4, "gprx_t ABI size");
_Static_assert(offsetof(struct CPU, intr) == 48, "CPU.intr ABI offset");
_Static_assert(offsetof(struct CPU, gen) == 52, "CPU.gen ABI offset");
_Static_assert(offsetof(struct CPU, cb) == 64, "CPU.cb ABI offset");
_Static_assert(offsetof(struct CPU, bios) == 120, "CPU.bios ABI offset");
_Static_assert(offsetof(struct CPU, prefetch_base) == 128, "CPU.prefetch_base ABI offset");
_Static_assert(offsetof(struct CPU, int_hooks) == 148, "CPU.int_hooks ABI offset");
_Static_assert(offsetof(struct CPU, fpu) == 1172, "CPU.fpu ABI offset");
_Static_assert(sizeof(struct CPU) == 1176, "CPU ABI size");
#endif

#pragma pack(pop)

#endif /* __CPU_H__ */
