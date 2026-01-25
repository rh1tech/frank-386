/*
 * murm386 JIT Compiler - ARM Thumb-2 Code Emission
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * MIT License
 *
 * This file contains helpers to emit ARM Thumb-2 instructions.
 * All generated code is position-independent where possible.
 *
 * Reference: ARM Architecture Reference Manual (ARMv8-M)
 * Thumb-2 encoding for Cortex-M33
 */

#include "jit.h"
#include "../i386.h"
#include <string.h>
#include <stddef.h>

/*
 * Emit raw 16-bit Thumb instruction
 */
void jit_emit16(jit_context_t *jit, uint16_t insn)
{
    if (jit->emit_ptr + 2 <= jit->cache_end) {
        *(uint16_t *)jit->emit_ptr = insn;
        jit->emit_ptr += 2;
    }
}

/*
 * Emit raw 32-bit Thumb-2 instruction
 * Note: Thumb-2 32-bit instructions are stored as two 16-bit halfwords
 * with the first halfword containing bits [31:16] and second [15:0]
 */
void jit_emit32(jit_context_t *jit, uint32_t insn)
{
    if (jit->emit_ptr + 4 <= jit->cache_end) {
        /* First halfword is upper 16 bits */
        *(uint16_t *)jit->emit_ptr = (insn >> 16) & 0xFFFF;
        jit->emit_ptr += 2;
        /* Second halfword is lower 16 bits */
        *(uint16_t *)jit->emit_ptr = insn & 0xFFFF;
        jit->emit_ptr += 2;
    }
}

/*
 * Emit: MOVS Rd, #imm8 (8-bit immediate, sets flags)
 * Encoding: 0010 0ddd iiii iiii (Thumb 16-bit)
 * Only for low registers (r0-r7)
 */
void jit_emit_mov_imm8(jit_context_t *jit, int rd, uint8_t imm)
{
    if (rd <= 7) {
        uint16_t insn = 0x2000 | (rd << 8) | imm;
        jit_emit16(jit, insn);
    } else {
        /* For high registers, use MOVW */
        jit_emit_mov_imm32(jit, rd, imm);
    }
}

/*
 * Emit: MOV Rd, #imm32 using MOVW/MOVT pair
 * MOVW: 1111 0x10 0100 iiii 0iii dddd iiii iiii (32-bit Thumb-2)
 * MOVT: 1111 0x10 1100 iiii 0iii dddd iiii iiii (32-bit Thumb-2)
 */
void jit_emit_mov_imm32(jit_context_t *jit, int rd, uint32_t imm)
{
    uint16_t imm_lo = imm & 0xFFFF;
    uint16_t imm_hi = (imm >> 16) & 0xFFFF;

    /* MOVW Rd, #imm16 (low half) */
    /* Encoding: 11110 i 10 0100 imm4 | 0 imm3 rd imm8 */
    uint32_t movw = 0xF2400000;
    movw |= ((imm_lo >> 12) & 0xF) << 16;  /* imm4 in bits [19:16] */
    movw |= ((imm_lo >> 11) & 0x1) << 26;  /* i in bit [26] */
    movw |= ((imm_lo >> 8) & 0x7) << 12;   /* imm3 in bits [14:12] */
    movw |= (imm_lo & 0xFF);               /* imm8 in bits [7:0] */
    movw |= (rd & 0xF) << 8;               /* rd in bits [11:8] */
    jit_emit32(jit, movw);

    /* MOVT Rd, #imm16 (high half) - only if needed */
    if (imm_hi != 0) {
        uint32_t movt = 0xF2C00000;
        movt |= ((imm_hi >> 12) & 0xF) << 16;
        movt |= ((imm_hi >> 11) & 0x1) << 26;
        movt |= ((imm_hi >> 8) & 0x7) << 12;
        movt |= (imm_hi & 0xFF);
        movt |= (rd & 0xF) << 8;
        jit_emit32(jit, movt);
    }
}

/*
 * Emit: MOV Rd, Rm (register to register)
 * For low registers: MOVS Rd, Rm (0000 0000 00mm mddd)
 * For any registers: MOV Rd, Rm (0100 0110 Dmm mddd)
 */
void jit_emit_mov_reg(jit_context_t *jit, int rd, int rm)
{
    if (rd <= 7 && rm <= 7) {
        /* MOVS Rd, Rm - 16-bit */
        uint16_t insn = 0x0000 | (rm << 3) | rd;
        jit_emit16(jit, insn);
    } else {
        /* MOV Rd, Rm - 16-bit, handles high registers */
        uint16_t insn = 0x4600;
        insn |= (rd & 0x7);
        insn |= ((rd >> 3) & 0x1) << 7;  /* D bit */
        insn |= (rm & 0xF) << 3;
        jit_emit16(jit, insn);
    }
}

/*
 * Emit: LDR Rd, [Rn, #offset] (load word with immediate offset)
 * Thumb-2 encoding for full offset range
 */
void jit_emit_ldr_offset(jit_context_t *jit, int rd, int rn, int offset)
{
    if (offset >= 0 && offset < 128 && (offset & 3) == 0 &&
        rd <= 7 && rn <= 7) {
        /* Use compact 16-bit encoding: LDR Rd, [Rn, #imm5*4] */
        /* 0110 1iii iinn nddd */
        uint16_t insn = 0x6800 | ((offset >> 2) << 6) | (rn << 3) | rd;
        jit_emit16(jit, insn);
    } else {
        /* Use 32-bit encoding: LDR.W Rd, [Rn, #imm12] */
        /* 1111 1000 1101 nnnn | tttt iiii iiii iiii */
        if (offset >= 0 && offset < 4096) {
            uint32_t insn = 0xF8D00000;
            insn |= (rn & 0xF) << 16;
            insn |= (rd & 0xF) << 12;
            insn |= (offset & 0xFFF);
            jit_emit32(jit, insn);
        } else if (offset < 0 && offset >= -255) {
            /* LDR.W Rd, [Rn, #-imm8] */
            /* 1111 1000 0101 nnnn | tttt 1100 iiii iiii */
            uint32_t insn = 0xF8500C00;
            insn |= (rn & 0xF) << 16;
            insn |= (rd & 0xF) << 12;
            insn |= ((-offset) & 0xFF);
            jit_emit32(jit, insn);
        } else {
            /* For larger offsets, compute address in scratch register */
            jit_emit_mov_imm32(jit, ARM_REG_SCRATCH0, offset);
            jit_emit_add_reg(jit, ARM_REG_SCRATCH0, rn, ARM_REG_SCRATCH0);
            /* LDR Rd, [r0, #0] */
            uint16_t insn = 0x6800 | rd;
            jit_emit16(jit, insn);
        }
    }
}

/*
 * Emit: STR Rd, [Rn, #offset] (store word with immediate offset)
 */
void jit_emit_str_offset(jit_context_t *jit, int rd, int rn, int offset)
{
    if (offset >= 0 && offset < 128 && (offset & 3) == 0 &&
        rd <= 7 && rn <= 7) {
        /* Use compact 16-bit encoding: STR Rd, [Rn, #imm5*4] */
        /* 0110 0iii iinn nddd */
        uint16_t insn = 0x6000 | ((offset >> 2) << 6) | (rn << 3) | rd;
        jit_emit16(jit, insn);
    } else {
        /* Use 32-bit encoding: STR.W Rd, [Rn, #imm12] */
        /* 1111 1000 1100 nnnn | tttt iiii iiii iiii */
        if (offset >= 0 && offset < 4096) {
            uint32_t insn = 0xF8C00000;
            insn |= (rn & 0xF) << 16;
            insn |= (rd & 0xF) << 12;
            insn |= (offset & 0xFFF);
            jit_emit32(jit, insn);
        } else {
            /* For larger offsets, compute address */
            jit_emit_mov_imm32(jit, ARM_REG_SCRATCH0, offset);
            jit_emit_add_reg(jit, ARM_REG_SCRATCH0, rn, ARM_REG_SCRATCH0);
            uint16_t insn = 0x6000 | rd;
            jit_emit16(jit, insn);
        }
    }
}

/*
 * Emit: ADD Rd, Rn, Rm
 * Thumb encoding: ADD Rd, Rn, Rm (0001 100m mmnn nddd)
 */
void jit_emit_add_reg(jit_context_t *jit, int rd, int rn, int rm)
{
    if (rd <= 7 && rn <= 7 && rm <= 7) {
        /* 16-bit encoding */
        uint16_t insn = 0x1800 | (rm << 6) | (rn << 3) | rd;
        jit_emit16(jit, insn);
    } else {
        /* 32-bit encoding: ADD.W Rd, Rn, Rm */
        /* 1110 1011 0000 nnnn | 0iii dddd ii00 mmmm */
        uint32_t insn = 0xEB000000;
        insn |= (rn & 0xF) << 16;
        insn |= (rd & 0xF) << 8;
        insn |= (rm & 0xF);
        jit_emit32(jit, insn);
    }
}

/*
 * Emit: SUB Rd, Rn, Rm
 * Thumb encoding: SUB Rd, Rn, Rm (0001 101m mmnn nddd)
 */
void jit_emit_sub_reg(jit_context_t *jit, int rd, int rn, int rm)
{
    if (rd <= 7 && rn <= 7 && rm <= 7) {
        /* 16-bit encoding */
        uint16_t insn = 0x1A00 | (rm << 6) | (rn << 3) | rd;
        jit_emit16(jit, insn);
    } else {
        /* 32-bit encoding: SUB.W Rd, Rn, Rm */
        /* 1110 1011 1010 nnnn | 0iii dddd ii00 mmmm */
        uint32_t insn = 0xEBA00000;
        insn |= (rn & 0xF) << 16;
        insn |= (rd & 0xF) << 8;
        insn |= (rm & 0xF);
        jit_emit32(jit, insn);
    }
}

/*
 * Emit: ADD Rd, Rn, #imm
 */
void jit_emit_add_imm(jit_context_t *jit, int rd, int rn, int imm)
{
    if (rd <= 7 && rn <= 7 && imm >= 0 && imm <= 7) {
        /* ADDS Rd, Rn, #imm3: 0001 110i iinn nddd */
        uint16_t insn = 0x1C00 | (imm << 6) | (rn << 3) | rd;
        jit_emit16(jit, insn);
    } else if (rd <= 7 && rd == rn && imm >= 0 && imm <= 255) {
        /* ADDS Rd, #imm8: 0011 0ddd iiii iiii */
        uint16_t insn = 0x3000 | (rd << 8) | imm;
        jit_emit16(jit, insn);
    } else {
        /* ADD.W Rd, Rn, #imm (32-bit encoding) */
        /* Use modified immediate encoding or fall back to MOV + ADD */
        jit_emit_mov_imm32(jit, ARM_REG_SCRATCH0, imm);
        jit_emit_add_reg(jit, rd, rn, ARM_REG_SCRATCH0);
    }
}

/*
 * Emit: SUB Rd, Rn, #imm
 */
void jit_emit_sub_imm(jit_context_t *jit, int rd, int rn, int imm)
{
    if (rd <= 7 && rn <= 7 && imm >= 0 && imm <= 7) {
        /* SUBS Rd, Rn, #imm3: 0001 111i iinn nddd */
        uint16_t insn = 0x1E00 | (imm << 6) | (rn << 3) | rd;
        jit_emit16(jit, insn);
    } else if (rd <= 7 && rd == rn && imm >= 0 && imm <= 255) {
        /* SUBS Rd, #imm8: 0011 1ddd iiii iiii */
        uint16_t insn = 0x3800 | (rd << 8) | imm;
        jit_emit16(jit, insn);
    } else {
        jit_emit_mov_imm32(jit, ARM_REG_SCRATCH0, imm);
        jit_emit_sub_reg(jit, rd, rn, ARM_REG_SCRATCH0);
    }
}

/*
 * Emit: PUSH {regs}
 * reg_mask: bit 0=r0, bit 1=r1, ..., bit 14=LR
 */
void jit_emit_push(jit_context_t *jit, uint16_t reg_mask)
{
    /* Check if we can use 16-bit PUSH */
    bool lr = (reg_mask & (1 << 14)) != 0;
    uint8_t low_regs = reg_mask & 0xFF;

    if ((reg_mask & ~0x40FF) == 0) {
        /* PUSH {reglist, LR}: 1011 010M rrrr rrrr */
        uint16_t insn = 0xB400 | (lr ? 0x100 : 0) | low_regs;
        jit_emit16(jit, insn);
    } else {
        /* Use 32-bit PUSH.W */
        /* 1110 1001 0010 1101 | rrrrrrrr rrrrrrrr */
        uint32_t insn = 0xE92D0000 | (reg_mask & 0x5FFF);
        jit_emit32(jit, insn);
    }
}

/*
 * Emit: POP {regs}
 * reg_mask: bit 0=r0, bit 1=r1, ..., bit 15=PC
 */
void jit_emit_pop(jit_context_t *jit, uint16_t reg_mask)
{
    bool pc = (reg_mask & (1 << 15)) != 0;
    uint8_t low_regs = reg_mask & 0xFF;

    if ((reg_mask & ~0x80FF) == 0) {
        /* POP {reglist, PC}: 1011 110P rrrr rrrr */
        uint16_t insn = 0xBC00 | (pc ? 0x100 : 0) | low_regs;
        jit_emit16(jit, insn);
    } else {
        /* Use 32-bit POP.W */
        uint32_t insn = 0xE8BD0000 | (reg_mask & 0xDFFF);
        jit_emit32(jit, insn);
    }
}

/*
 * Emit: BL <offset> (branch with link)
 * Target must be within range of BL instruction
 */
void jit_emit_bl(jit_context_t *jit, void *target)
{
    /* Calculate offset from current position */
    intptr_t current = (intptr_t)jit->emit_ptr;
    intptr_t offset = (intptr_t)target - current - 4; /* -4 because PC is ahead */

    /* BL encoding: 11110 S imm10 | 11 J1 1 J2 imm11 */
    int32_t imm = offset >> 1;  /* Divide by 2 (halfword aligned) */

    uint32_t S = (imm < 0) ? 1 : 0;
    uint32_t imm10 = (imm >> 11) & 0x3FF;
    uint32_t imm11 = imm & 0x7FF;
    uint32_t J1 = ((~(imm >> 22)) ^ S) & 1;
    uint32_t J2 = ((~(imm >> 21)) ^ S) & 1;

    uint32_t insn = 0xF0009000;
    insn |= (S << 26);
    insn |= (imm10 << 16);
    insn |= (J1 << 13);
    insn |= (J2 << 11);
    insn |= imm11;

    jit_emit32(jit, insn);
}

/*
 * Emit: BX LR (return from function)
 */
void jit_emit_bx_lr(jit_context_t *jit)
{
    /* BX LR: 0100 0111 0111 0000 */
    jit_emit16(jit, 0x4770);
}

/*
 * Offset of various CPU struct fields
 * Note: gprx array is at offset 0, each entry is 4 bytes
 */
#define CPU_OFFSET_GPR      0       /* gprx array */

/*
 * ARM_REG_SCRATCH3 (r3) is reserved as phys_mem pointer
 * It's passed as second argument to JIT block function
 */
#define ARM_REG_PHYS_MEM    3

/*
 * Emit block prologue
 * - Save callee-saved registers
 * - Set up r12 with CPU pointer, r3 with phys_mem pointer
 * - Load i386 registers into ARM registers
 *
 * JIT block function signature: int jit_block(CPUI386 *cpu, uint8_t *phys_mem)
 */
void jit_emit_prologue(jit_context_t *jit)
{
    /* PUSH {r3-r11, lr} - save callee-saved registers (r3 included for phys_mem) */
    jit_emit_push(jit, (1<<3)|(1<<4)|(1<<5)|(1<<6)|(1<<7)|
                       (1<<8)|(1<<9)|(1<<10)|(1<<11)|(1<<14));

    /* MOV r12, r0 - save CPU pointer (passed in r0) */
    jit_emit_mov_reg(jit, ARM_REG_CPU, 0);

    /* r1 already contains phys_mem pointer, move to r3 for safekeeping */
    /* r3 is callee-saved, so it persists across helper calls */
    jit_emit_mov_reg(jit, ARM_REG_PHYS_MEM, 1);

    /* Load i386 registers from CPU struct into ARM registers */
    /* EAX = cpu->gprx[0].r32 */
    jit_emit_ldr_offset(jit, ARM_REG_EAX, ARM_REG_CPU, 0*4);
    /* ECX = cpu->gprx[1].r32 */
    jit_emit_ldr_offset(jit, ARM_REG_ECX, ARM_REG_CPU, 1*4);
    /* EDX = cpu->gprx[2].r32 */
    jit_emit_ldr_offset(jit, ARM_REG_EDX, ARM_REG_CPU, 2*4);
    /* EBX = cpu->gprx[3].r32 */
    jit_emit_ldr_offset(jit, ARM_REG_EBX, ARM_REG_CPU, 3*4);
    /* ESP = cpu->gprx[4].r32 */
    jit_emit_ldr_offset(jit, ARM_REG_ESP, ARM_REG_CPU, 4*4);
    /* EBP = cpu->gprx[5].r32 */
    jit_emit_ldr_offset(jit, ARM_REG_EBP, ARM_REG_CPU, 5*4);
    /* ESI = cpu->gprx[6].r32 */
    jit_emit_ldr_offset(jit, ARM_REG_ESI, ARM_REG_CPU, 6*4);
    /* EDI = cpu->gprx[7].r32 */
    jit_emit_ldr_offset(jit, ARM_REG_EDI, ARM_REG_CPU, 7*4);
}

/*
 * Emit block epilogue
 * - Store i386 registers back to CPU struct
 * - Set return value (exit reason)
 * - Restore callee-saved registers
 * - Return
 */
void jit_emit_epilogue(jit_context_t *jit, jit_exit_reason_t reason)
{
    /* Store i386 registers back to CPU struct */
    jit_emit_str_offset(jit, ARM_REG_EAX, ARM_REG_CPU, 0*4);
    jit_emit_str_offset(jit, ARM_REG_ECX, ARM_REG_CPU, 1*4);
    jit_emit_str_offset(jit, ARM_REG_EDX, ARM_REG_CPU, 2*4);
    jit_emit_str_offset(jit, ARM_REG_EBX, ARM_REG_CPU, 3*4);
    jit_emit_str_offset(jit, ARM_REG_ESP, ARM_REG_CPU, 4*4);
    jit_emit_str_offset(jit, ARM_REG_EBP, ARM_REG_CPU, 5*4);
    jit_emit_str_offset(jit, ARM_REG_ESI, ARM_REG_CPU, 6*4);
    jit_emit_str_offset(jit, ARM_REG_EDI, ARM_REG_CPU, 7*4);

    /* MOV r0, #exit_reason - return value */
    jit_emit_mov_imm8(jit, 0, reason);

    /* POP {r3-r11, pc} - restore and return */
    jit_emit_pop(jit, (1<<3)|(1<<4)|(1<<5)|(1<<6)|(1<<7)|
                      (1<<8)|(1<<9)|(1<<10)|(1<<11)|(1<<15));
}

/*
 * Emit call to a C helper function
 * Assumes function takes CPUI386* as first argument
 */
void jit_emit_call_helper(jit_context_t *jit, void *helper)
{
    /* First, sync registers back to CPU struct */
    jit_emit_str_offset(jit, ARM_REG_EAX, ARM_REG_CPU, 0*4);
    jit_emit_str_offset(jit, ARM_REG_ECX, ARM_REG_CPU, 1*4);
    jit_emit_str_offset(jit, ARM_REG_EDX, ARM_REG_CPU, 2*4);
    jit_emit_str_offset(jit, ARM_REG_EBX, ARM_REG_CPU, 3*4);
    jit_emit_str_offset(jit, ARM_REG_ESP, ARM_REG_CPU, 4*4);
    jit_emit_str_offset(jit, ARM_REG_EBP, ARM_REG_CPU, 5*4);
    jit_emit_str_offset(jit, ARM_REG_ESI, ARM_REG_CPU, 6*4);
    jit_emit_str_offset(jit, ARM_REG_EDI, ARM_REG_CPU, 7*4);

    /* MOV r0, r12 - pass CPU pointer as argument */
    jit_emit_mov_reg(jit, 0, ARM_REG_CPU);

    /* Load helper address into scratch and call */
    jit_emit_mov_imm32(jit, ARM_REG_SCRATCH1, (uint32_t)(uintptr_t)helper);
    /* BLX r1 - branch with link and exchange */
    jit_emit16(jit, 0x4788 | (ARM_REG_SCRATCH1 << 3));

    /* Reload i386 registers from CPU struct */
    jit_emit_ldr_offset(jit, ARM_REG_EAX, ARM_REG_CPU, 0*4);
    jit_emit_ldr_offset(jit, ARM_REG_ECX, ARM_REG_CPU, 1*4);
    jit_emit_ldr_offset(jit, ARM_REG_EDX, ARM_REG_CPU, 2*4);
    jit_emit_ldr_offset(jit, ARM_REG_EBX, ARM_REG_CPU, 3*4);
    jit_emit_ldr_offset(jit, ARM_REG_ESP, ARM_REG_CPU, 4*4);
    jit_emit_ldr_offset(jit, ARM_REG_EBP, ARM_REG_CPU, 5*4);
    jit_emit_ldr_offset(jit, ARM_REG_ESI, ARM_REG_CPU, 6*4);
    jit_emit_ldr_offset(jit, ARM_REG_EDI, ARM_REG_CPU, 7*4);
}

/*
 * Memory access helpers
 * These emit code to read/write emulated memory through PSRAM
 * ARM_REG_PHYS_MEM (r3) holds the phys_mem base pointer
 */

/*
 * Emit: Load byte from memory at address in addr_reg into rd
 * Uses r3 (ARM_REG_PHYS_MEM) as base pointer
 */
void jit_emit_mem_read8(jit_context_t *jit, int rd, int addr_reg)
{
    /* Add address to phys_mem base to get final pointer */
    jit_emit_add_reg(jit, ARM_REG_SCRATCH2, ARM_REG_PHYS_MEM, addr_reg);
    /* LDRB Rd, [scratch] */
    /* 0101 110m mmnn nddd */
    if (rd <= 7 && ARM_REG_SCRATCH2 <= 7) {
        uint16_t insn = 0x5C00 | (rd & 7);
        insn |= (ARM_REG_SCRATCH2 & 7) << 3;
        jit_emit16(jit, insn);
    } else {
        /* LDRB.W Rd, [Rn] */
        uint32_t insn = 0xF8900000;
        insn |= (ARM_REG_SCRATCH2 & 0xF) << 16;
        insn |= (rd & 0xF) << 12;
        jit_emit32(jit, insn);
    }
}

void jit_emit_mem_read16(jit_context_t *jit, int rd, int addr_reg)
{
    /* Add address to phys_mem base to get final pointer */
    jit_emit_add_reg(jit, ARM_REG_SCRATCH2, ARM_REG_PHYS_MEM, addr_reg);
    /* LDRH.W Rd, [Rn] */
    uint32_t insn = 0xF8B00000;
    insn |= (ARM_REG_SCRATCH2 & 0xF) << 16;
    insn |= (rd & 0xF) << 12;
    jit_emit32(jit, insn);
}

void jit_emit_mem_read32(jit_context_t *jit, int rd, int addr_reg)
{
    /* Add address to phys_mem base to get final pointer */
    jit_emit_add_reg(jit, ARM_REG_SCRATCH2, ARM_REG_PHYS_MEM, addr_reg);
    /* LDR Rd, [Rn] */
    if (rd <= 7 && ARM_REG_SCRATCH2 <= 7) {
        uint16_t insn = 0x6800 | (rd & 7);
        insn |= (ARM_REG_SCRATCH2 & 7) << 3;
        jit_emit16(jit, insn);
    } else {
        uint32_t insn = 0xF8D00000;
        insn |= (ARM_REG_SCRATCH2 & 0xF) << 16;
        insn |= (rd & 0xF) << 12;
        jit_emit32(jit, insn);
    }
}

void jit_emit_mem_write8(jit_context_t *jit, int val_reg, int addr_reg)
{
    /* Add address to phys_mem base to get final pointer */
    jit_emit_add_reg(jit, ARM_REG_SCRATCH2, ARM_REG_PHYS_MEM, addr_reg);
    /* STRB val_reg, [scratch] */
    if (val_reg <= 7 && ARM_REG_SCRATCH2 <= 7) {
        uint16_t insn = 0x5400 | (val_reg & 7);
        insn |= (ARM_REG_SCRATCH2 & 7) << 3;
        jit_emit16(jit, insn);
    } else {
        uint32_t insn = 0xF8800000;
        insn |= (ARM_REG_SCRATCH2 & 0xF) << 16;
        insn |= (val_reg & 0xF) << 12;
        jit_emit32(jit, insn);
    }
}

void jit_emit_mem_write16(jit_context_t *jit, int val_reg, int addr_reg)
{
    /* Add address to phys_mem base to get final pointer */
    jit_emit_add_reg(jit, ARM_REG_SCRATCH2, ARM_REG_PHYS_MEM, addr_reg);
    /* STRH.W val_reg, [scratch] */
    uint32_t insn = 0xF8A00000;
    insn |= (ARM_REG_SCRATCH2 & 0xF) << 16;
    insn |= (val_reg & 0xF) << 12;
    jit_emit32(jit, insn);
}

void jit_emit_mem_write32(jit_context_t *jit, int val_reg, int addr_reg)
{
    /* Add address to phys_mem base to get final pointer */
    jit_emit_add_reg(jit, ARM_REG_SCRATCH2, ARM_REG_PHYS_MEM, addr_reg);
    /* STR val_reg, [scratch] */
    if (val_reg <= 7 && ARM_REG_SCRATCH2 <= 7) {
        uint16_t insn = 0x6000 | (val_reg & 7);
        insn |= (ARM_REG_SCRATCH2 & 7) << 3;
        jit_emit16(jit, insn);
    } else {
        uint32_t insn = 0xF8C00000;
        insn |= (ARM_REG_SCRATCH2 & 0xF) << 16;
        insn |= (val_reg & 0xF) << 12;
        jit_emit32(jit, insn);
    }
}
