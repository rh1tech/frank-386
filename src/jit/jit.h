/*
 * murm386 JIT Compiler - i386 to ARM Thumb-2 dynamic recompilation
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * MIT License
 */

#ifndef JIT_H
#define JIT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef RP2350_BUILD
#include "pico/stdlib.h"
#endif

/* Forward declarations */
struct CPUI386;
typedef struct CPUI386 CPUI386;

/*
 * JIT Configuration
 *
 * Note: Code cache is allocated in internal SRAM which is limited.
 * Keep cache size small - 16KB is a reasonable compromise.
 * Larger caches would require PSRAM execution support.
 */
#define JIT_CACHE_SIZE      (8 * 1024)    /* 8KB code cache */
#define JIT_BLOCK_MAX_SIZE  256           /* Max ARM code per block */
#define JIT_HASH_SIZE       256           /* Hash table entries (direct-mapped) */
#define JIT_MAX_BLOCK_INSNS 32            /* Max i386 instructions per block */

/*
 * ARM Cortex-M33 Register Mapping
 *
 * We map i386 registers to ARM registers for fast access:
 *   r4  = EAX    r8  = ESP
 *   r5  = ECX    r9  = EBP
 *   r6  = EDX    r10 = ESI
 *   r7  = EBX    r11 = EDI
 *   r12 = Pointer to CPUI386 struct
 *   r0-r3 = Scratch/temporaries
 *
 * Segment registers and flags are accessed through the CPU struct.
 */
#define ARM_REG_EAX     4
#define ARM_REG_ECX     5
#define ARM_REG_EDX     6
#define ARM_REG_EBX     7
#define ARM_REG_ESP     8
#define ARM_REG_EBP     9
#define ARM_REG_ESI     10
#define ARM_REG_EDI     11
#define ARM_REG_CPU     12    /* Pointer to CPUI386 */

#define ARM_REG_SCRATCH0 0
#define ARM_REG_SCRATCH1 1
#define ARM_REG_SCRATCH2 2
#define ARM_REG_SCRATCH3 3

/*
 * Block exit reasons - why a compiled block returns to dispatcher
 */
typedef enum {
    JIT_EXIT_NORMAL,          /* Reached end of block */
    JIT_EXIT_BRANCH,          /* Conditional branch taken */
    JIT_EXIT_CALL,            /* CALL instruction */
    JIT_EXIT_RET,             /* RET instruction */
    JIT_EXIT_INTERRUPT,       /* Software interrupt (INT) */
    JIT_EXIT_EXCEPTION,       /* CPU exception */
    JIT_EXIT_FALLBACK,        /* Needs interpreter fallback */
    JIT_EXIT_SELF_MODIFY,     /* Self-modifying code detected */
} jit_exit_reason_t;

/*
 * Compiled block header
 * Stored at the beginning of each compiled block in the code cache
 */
typedef struct jit_block {
    uint32_t i386_addr;       /* Linear address of i386 code */
    uint32_t i386_cs_base;    /* CS base when block was compiled */
    uint16_t i386_len;        /* Length of i386 code in bytes */
    uint16_t arm_len;         /* Length of ARM code in bytes */
    uint32_t exec_count;      /* Execution count for profiling */
    uint8_t  flags;           /* Block flags */
    uint8_t  reserved[3];
    /* ARM code follows immediately after this header */
} jit_block_t;

#define JIT_BLOCK_FLAG_VALID    0x01
#define JIT_BLOCK_FLAG_HOTSPOT  0x02  /* Frequently executed */

/*
 * Hash table entry for block lookup
 */
typedef struct jit_hash_entry {
    uint32_t i386_addr;       /* i386 linear address (0 = empty) */
    jit_block_t *block;       /* Pointer to compiled block */
} jit_hash_entry_t;

/*
 * JIT context - main state structure
 */
typedef struct jit_context {
    /* Code cache */
    uint8_t *cache_base;          /* Base of code cache memory */
    uint8_t *cache_ptr;           /* Current allocation pointer */
    uint8_t *cache_end;           /* End of code cache */

    /* Block lookup hash table */
    jit_hash_entry_t hash_table[JIT_HASH_SIZE];

    /* Statistics */
    uint32_t blocks_compiled;
    uint32_t blocks_executed;
    uint32_t cache_flushes;
    uint32_t fallback_count;

    /* CPU reference */
    CPUI386 *cpu;
    uint8_t *phys_mem;            /* Cached pointer to physical memory */

    /* Current block being compiled */
    jit_block_t *current_block;
    uint8_t *emit_ptr;            /* Current emit position */

    /* Translation state */
    uint32_t block_start_addr;    /* i386 linear address of block start */
    int insn_count;               /* Instructions in current block */
    bool block_terminated;        /* Block ends with control flow */

} jit_context_t;

/*
 * JIT Public API
 */

/* Initialize JIT compiler */
jit_context_t *jit_init(CPUI386 *cpu);

/* Destroy JIT compiler and free resources */
void jit_destroy(jit_context_t *jit);

/* Execute code at current IP using JIT */
/* Returns number of i386 instructions executed, or -1 on error */
int jit_execute(jit_context_t *jit);

/* Flush entire code cache (e.g., on memory write) */
void jit_flush_cache(jit_context_t *jit);

/* Invalidate blocks containing specific address range */
void jit_invalidate_range(jit_context_t *jit, uint32_t start, uint32_t len);

/* Get JIT statistics */
void jit_get_stats(jit_context_t *jit, uint32_t *compiled, uint32_t *executed,
                   uint32_t *flushes, uint32_t *fallbacks);

/* Check if JIT is enabled and working */
bool jit_is_enabled(jit_context_t *jit);

/*
 * Internal functions (used by jit_translate.c and jit_emit.c)
 */

/* Lookup compiled block for address */
jit_block_t *jit_lookup_block(jit_context_t *jit, uint32_t addr);

/* Compile a new block starting at address */
jit_block_t *jit_compile_block(jit_context_t *jit, uint32_t addr);

/* Add block to hash table */
void jit_hash_insert(jit_context_t *jit, jit_block_t *block);

/* Remove block from hash table */
void jit_hash_remove(jit_context_t *jit, uint32_t addr);

/*
 * ARM code emission helpers
 */

/* Emit 16-bit Thumb instruction */
void jit_emit16(jit_context_t *jit, uint16_t insn);

/* Emit 32-bit Thumb-2 instruction */
void jit_emit32(jit_context_t *jit, uint32_t insn);

/* Emit: MOV Rd, #imm8 (8-bit immediate) */
void jit_emit_mov_imm8(jit_context_t *jit, int rd, uint8_t imm);

/* Emit: MOV Rd, #imm32 (32-bit immediate using MOVW/MOVT) */
void jit_emit_mov_imm32(jit_context_t *jit, int rd, uint32_t imm);

/* Emit: MOV Rd, Rm (register to register) */
void jit_emit_mov_reg(jit_context_t *jit, int rd, int rm);

/* Emit: LDR Rd, [Rn, #offset] (load word) */
void jit_emit_ldr_offset(jit_context_t *jit, int rd, int rn, int offset);

/* Emit: STR Rd, [Rn, #offset] (store word) */
void jit_emit_str_offset(jit_context_t *jit, int rd, int rn, int offset);

/* Emit: ADD Rd, Rn, Rm */
void jit_emit_add_reg(jit_context_t *jit, int rd, int rn, int rm);

/* Emit: SUB Rd, Rn, Rm */
void jit_emit_sub_reg(jit_context_t *jit, int rd, int rn, int rm);

/* Emit: ADD Rd, Rn, #imm */
void jit_emit_add_imm(jit_context_t *jit, int rd, int rn, int imm);

/* Emit: SUB Rd, Rn, #imm */
void jit_emit_sub_imm(jit_context_t *jit, int rd, int rn, int imm);

/* Emit: BL <address> (branch with link - function call) */
void jit_emit_bl(jit_context_t *jit, void *target);

/* Emit: BX LR (return from function) */
void jit_emit_bx_lr(jit_context_t *jit);

/* Emit: PUSH {regs} */
void jit_emit_push(jit_context_t *jit, uint16_t reg_mask);

/* Emit: POP {regs} */
void jit_emit_pop(jit_context_t *jit, uint16_t reg_mask);

/* Emit prologue - save registers and set up CPU pointer */
void jit_emit_prologue(jit_context_t *jit);

/* Emit epilogue - restore registers and return */
void jit_emit_epilogue(jit_context_t *jit, jit_exit_reason_t reason);

/* Emit call to C helper function */
void jit_emit_call_helper(jit_context_t *jit, void *helper);

/* Emit memory read (through PSRAM) */
void jit_emit_mem_read8(jit_context_t *jit, int rd, int addr_reg);
void jit_emit_mem_read16(jit_context_t *jit, int rd, int addr_reg);
void jit_emit_mem_read32(jit_context_t *jit, int rd, int addr_reg);

/* Emit memory write (through PSRAM) */
void jit_emit_mem_write8(jit_context_t *jit, int val_reg, int addr_reg);
void jit_emit_mem_write16(jit_context_t *jit, int val_reg, int addr_reg);
void jit_emit_mem_write32(jit_context_t *jit, int val_reg, int addr_reg);

/*
 * i386 instruction translation
 */

/* Translate single i386 instruction to ARM code */
/* Returns number of i386 bytes consumed, or -1 for unsupported instruction */
int jit_translate_insn(jit_context_t *jit, uint8_t *code, int max_len);

/* Check if instruction can be JIT compiled */
bool jit_can_translate(uint8_t opcode, uint8_t modrm);

#endif /* JIT_H */
