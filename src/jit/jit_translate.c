/*
 * murm386 JIT Compiler - i386 to ARM Translation
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * MIT License
 *
 * This file translates individual i386 instructions to ARM Thumb-2 code.
 * Complex instructions fall back to the interpreter.
 */

#include "jit.h"
#include "../i386.h"
#include <string.h>

/*
 * i386 to ARM register mapping
 */
static const int i386_to_arm_reg[8] = {
    ARM_REG_EAX,  /* 0: EAX */
    ARM_REG_ECX,  /* 1: ECX */
    ARM_REG_EDX,  /* 2: EDX */
    ARM_REG_EBX,  /* 3: EBX */
    ARM_REG_ESP,  /* 4: ESP */
    ARM_REG_EBP,  /* 5: EBP */
    ARM_REG_ESI,  /* 6: ESI */
    ARM_REG_EDI,  /* 7: EDI */
};

/*
 * Decode ModR/M byte
 * Returns: mod (bits 7-6), reg (bits 5-3), rm (bits 2-0)
 */
static void decode_modrm(uint8_t modrm, int *mod, int *reg, int *rm)
{
    *mod = (modrm >> 6) & 3;
    *reg = (modrm >> 3) & 7;
    *rm = modrm & 7;
}

/*
 * Check if an instruction can be JIT compiled
 * Returns true if the instruction is supported
 */
bool jit_can_translate(uint8_t opcode, uint8_t modrm)
{
    (void)modrm;

    switch (opcode) {
    /* Simple ALU operations - reg, reg */
    case 0x00: case 0x01: case 0x02: case 0x03: /* ADD */
    case 0x08: case 0x09: case 0x0A: case 0x0B: /* OR */
    case 0x10: case 0x11: case 0x12: case 0x13: /* ADC - needs flags */
    case 0x18: case 0x19: case 0x1A: case 0x1B: /* SBB - needs flags */
    case 0x20: case 0x21: case 0x22: case 0x23: /* AND */
    case 0x28: case 0x29: case 0x2A: case 0x2B: /* SUB */
    case 0x30: case 0x31: case 0x32: case 0x33: /* XOR */
    case 0x38: case 0x39: case 0x3A: case 0x3B: /* CMP */

    /* ALU with immediate to AL/AX/EAX */
    case 0x04: case 0x05: /* ADD AL/AX, imm */
    case 0x0C: case 0x0D: /* OR AL/AX, imm */
    case 0x24: case 0x25: /* AND AL/AX, imm */
    case 0x2C: case 0x2D: /* SUB AL/AX, imm */
    case 0x34: case 0x35: /* XOR AL/AX, imm */
    case 0x3C: case 0x3D: /* CMP AL/AX, imm */

    /* INC/DEC register */
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47: /* INC r32 */
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: /* DEC r32 */

    /* PUSH/POP register */
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: /* PUSH r32 */
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: /* POP r32 */

    /* MOV reg, imm */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: /* MOV r8, imm8 */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: /* MOV r32, imm32 */

    /* MOV r/m, r and MOV r, r/m */
    case 0x88: case 0x89: case 0x8A: case 0x8B:

    /* XCHG EAX, r32 */
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:

    /* NOP */
    /* 0x90 is XCHG EAX, EAX which is NOP */

    /* LEA - useful for address calculations */
    case 0x8D:
        return true;

    default:
        return false;
    }
}

/*
 * Translate a single i386 instruction to ARM code
 * Returns: number of i386 bytes consumed, or -1 for unsupported
 */
int jit_translate_insn(jit_context_t *jit, uint8_t *code, int max_len)
{
    if (max_len < 1) return -1;

    uint8_t opcode = code[0];
    int consumed = 1;

    /* Handle prefixes */
    bool opsz16 = false;    /* Operand size prefix (0x66) */
    bool adsz16 = false;    /* Address size prefix (0x67) */
    int seg_override = -1;  /* Segment override */

    /* Scan prefixes */
    while (consumed < max_len) {
        switch (opcode) {
        case 0x26: seg_override = 0; break; /* ES */
        case 0x2E: seg_override = 1; break; /* CS */
        case 0x36: seg_override = 2; break; /* SS */
        case 0x3E: seg_override = 3; break; /* DS */
        case 0x64: seg_override = 4; break; /* FS */
        case 0x65: seg_override = 5; break; /* GS */
        case 0x66: opsz16 = true; break;
        case 0x67: adsz16 = true; break;
        case 0xF0: break; /* LOCK - ignore */
        case 0xF2: /* REPNE - fall back */
        case 0xF3: /* REP - fall back */
            return -1;
        default:
            goto done_prefixes;
        }
        opcode = code[consumed++];
    }
done_prefixes:

    /* Ignore segment overrides and address size for now (simplified) */
    (void)seg_override;
    (void)adsz16;

    /* Decode and translate based on opcode */
    switch (opcode) {

    /*
     * NOP (0x90 = XCHG EAX, EAX)
     */
    case 0x90:
        /* No ARM code needed */
        return consumed;

    /*
     * MOV r32, imm32 (0xB8-0xBF)
     */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
    {
        int reg = opcode & 7;
        int arm_reg = i386_to_arm_reg[reg];

        if (consumed + (opsz16 ? 2 : 4) > max_len) return -1;

        if (opsz16) {
            uint16_t imm = code[consumed] | (code[consumed + 1] << 8);
            consumed += 2;
            /* Zero-extend to 32-bit but preserve high bits */
            /* For 16-bit ops, we should mask the register */
            jit_emit_mov_imm32(jit, arm_reg,
                               (jit->cpu->gprx[reg].r32 & 0xFFFF0000) | imm);
        } else {
            uint32_t imm = code[consumed] | (code[consumed + 1] << 8) |
                           (code[consumed + 2] << 16) | (code[consumed + 3] << 24);
            consumed += 4;
            jit_emit_mov_imm32(jit, arm_reg, imm);
        }
        return consumed;
    }

    /*
     * MOV r8, imm8 (0xB0-0xB7)
     */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
    {
        /* 8-bit register move is complex due to register encoding */
        /* For simplicity, fall back to interpreter */
        return -1;
    }

    /*
     * INC r32 (0x40-0x47)
     */
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
    {
        int reg = opcode & 7;
        int arm_reg = i386_to_arm_reg[reg];
        jit_emit_add_imm(jit, arm_reg, arm_reg, 1);
        /* TODO: Update flags (OF, SF, ZF, AF, PF but not CF) */
        return consumed;
    }

    /*
     * DEC r32 (0x48-0x4F)
     */
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
    {
        int reg = opcode & 7;
        int arm_reg = i386_to_arm_reg[reg];
        jit_emit_sub_imm(jit, arm_reg, arm_reg, 1);
        /* TODO: Update flags */
        return consumed;
    }

    /*
     * PUSH r32 (0x50-0x57)
     */
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
    {
        int reg = opcode & 7;
        int arm_reg = i386_to_arm_reg[reg];
        int size = opsz16 ? 2 : 4;

        /* ESP -= size */
        jit_emit_sub_imm(jit, ARM_REG_ESP, ARM_REG_ESP, size);

        /* Write to stack [ESP] */
        if (opsz16) {
            jit_emit_mem_write16(jit, arm_reg, ARM_REG_ESP);
        } else {
            jit_emit_mem_write32(jit, arm_reg, ARM_REG_ESP);
        }
        return consumed;
    }

    /*
     * POP r32 (0x58-0x5F)
     */
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    {
        int reg = opcode & 7;
        int arm_reg = i386_to_arm_reg[reg];
        int size = opsz16 ? 2 : 4;

        /* Read from stack [ESP] */
        if (opsz16) {
            jit_emit_mem_read16(jit, arm_reg, ARM_REG_ESP);
            /* TODO: Zero-extend for 16-bit */
        } else {
            jit_emit_mem_read32(jit, arm_reg, ARM_REG_ESP);
        }

        /* ESP += size */
        jit_emit_add_imm(jit, ARM_REG_ESP, ARM_REG_ESP, size);
        return consumed;
    }

    /*
     * XCHG EAX, r32 (0x91-0x97, 0x90 is NOP)
     */
    case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:
    {
        int reg = opcode & 7;
        int arm_reg = i386_to_arm_reg[reg];

        /* XCHG using scratch register */
        jit_emit_mov_reg(jit, ARM_REG_SCRATCH0, ARM_REG_EAX);
        jit_emit_mov_reg(jit, ARM_REG_EAX, arm_reg);
        jit_emit_mov_reg(jit, arm_reg, ARM_REG_SCRATCH0);
        return consumed;
    }

    /*
     * ADD EAX, imm32 (0x05)
     */
    case 0x05:
    {
        if (consumed + (opsz16 ? 2 : 4) > max_len) return -1;

        if (opsz16) {
            uint16_t imm = code[consumed] | (code[consumed + 1] << 8);
            consumed += 2;
            jit_emit_add_imm(jit, ARM_REG_EAX, ARM_REG_EAX, imm);
        } else {
            uint32_t imm = code[consumed] | (code[consumed + 1] << 8) |
                           (code[consumed + 2] << 16) | (code[consumed + 3] << 24);
            consumed += 4;
            if (imm <= 255) {
                jit_emit_add_imm(jit, ARM_REG_EAX, ARM_REG_EAX, imm);
            } else {
                jit_emit_mov_imm32(jit, ARM_REG_SCRATCH0, imm);
                jit_emit_add_reg(jit, ARM_REG_EAX, ARM_REG_EAX, ARM_REG_SCRATCH0);
            }
        }
        return consumed;
    }

    /*
     * SUB EAX, imm32 (0x2D)
     */
    case 0x2D:
    {
        if (consumed + (opsz16 ? 2 : 4) > max_len) return -1;

        if (opsz16) {
            uint16_t imm = code[consumed] | (code[consumed + 1] << 8);
            consumed += 2;
            jit_emit_sub_imm(jit, ARM_REG_EAX, ARM_REG_EAX, imm);
        } else {
            uint32_t imm = code[consumed] | (code[consumed + 1] << 8) |
                           (code[consumed + 2] << 16) | (code[consumed + 3] << 24);
            consumed += 4;
            if (imm <= 255) {
                jit_emit_sub_imm(jit, ARM_REG_EAX, ARM_REG_EAX, imm);
            } else {
                jit_emit_mov_imm32(jit, ARM_REG_SCRATCH0, imm);
                jit_emit_sub_reg(jit, ARM_REG_EAX, ARM_REG_EAX, ARM_REG_SCRATCH0);
            }
        }
        return consumed;
    }

    /*
     * ADD r/m32, r32 (0x01) and ADD r32, r/m32 (0x03)
     * SUB r/m32, r32 (0x29) and SUB r32, r/m32 (0x2B)
     * XOR r/m32, r32 (0x31) and XOR r32, r/m32 (0x33)
     * AND r/m32, r32 (0x21) and AND r32, r/m32 (0x23)
     * OR  r/m32, r32 (0x09) and OR  r32, r/m32 (0x0B)
     * MOV r/m32, r32 (0x89) and MOV r32, r/m32 (0x8B)
     */
    case 0x01: case 0x03: /* ADD */
    case 0x09: case 0x0B: /* OR */
    case 0x21: case 0x23: /* AND */
    case 0x29: case 0x2B: /* SUB */
    case 0x31: case 0x33: /* XOR */
    case 0x89: case 0x8B: /* MOV */
    {
        if (consumed >= max_len) return -1;
        uint8_t modrm = code[consumed++];

        int mod, reg_field, rm;
        decode_modrm(modrm, &mod, &reg_field, &rm);

        /* Only handle register-to-register operations (mod == 3) */
        if (mod != 3) {
            /* Memory operand - complex, fall back */
            return -1;
        }

        int arm_reg = i386_to_arm_reg[reg_field];
        int arm_rm = i386_to_arm_reg[rm];

        bool dir = (opcode & 2) != 0; /* 1 = r, r/m; 0 = r/m, r */
        int dst = dir ? arm_reg : arm_rm;
        int src = dir ? arm_rm : arm_reg;

        switch (opcode & 0xFE) {
        case 0x00: /* ADD */
            jit_emit_add_reg(jit, dst, dst, src);
            break;
        case 0x08: /* OR */
            /* ORR Rd, Rn, Rm */
            if (dst <= 7 && src <= 7) {
                uint16_t insn = 0x4300 | (src << 3) | dst;
                jit_emit16(jit, insn);
            } else {
                uint32_t insn = 0xEA400000;
                insn |= (dst & 0xF) << 16;
                insn |= (dst & 0xF) << 8;
                insn |= (src & 0xF);
                jit_emit32(jit, insn);
            }
            break;
        case 0x20: /* AND */
            /* AND Rd, Rn, Rm */
            if (dst <= 7 && src <= 7) {
                uint16_t insn = 0x4000 | (src << 3) | dst;
                jit_emit16(jit, insn);
            } else {
                uint32_t insn = 0xEA000000;
                insn |= (dst & 0xF) << 16;
                insn |= (dst & 0xF) << 8;
                insn |= (src & 0xF);
                jit_emit32(jit, insn);
            }
            break;
        case 0x28: /* SUB */
            jit_emit_sub_reg(jit, dst, dst, src);
            break;
        case 0x30: /* XOR */
            /* EOR Rd, Rn, Rm */
            if (dst <= 7 && src <= 7) {
                uint16_t insn = 0x4040 | (src << 3) | dst;
                jit_emit16(jit, insn);
            } else {
                uint32_t insn = 0xEA800000;
                insn |= (dst & 0xF) << 16;
                insn |= (dst & 0xF) << 8;
                insn |= (src & 0xF);
                jit_emit32(jit, insn);
            }
            break;
        case 0x88: /* MOV */
            jit_emit_mov_reg(jit, dst, src);
            break;
        }
        return consumed;
    }

    /*
     * Control flow - terminate block
     */
    case 0xEB: /* JMP rel8 */
    case 0xE9: /* JMP rel32 */
    case 0xC3: /* RET */
    case 0xC2: /* RET imm16 */
    case 0xE8: /* CALL rel32 */
    case 0xFF: /* JMP/CALL r/m (indirect) */
    case 0xCC: /* INT 3 */
    case 0xCD: /* INT imm8 */
    case 0xCF: /* IRET */
        /* Control flow changes - terminate block and let dispatcher handle */
        jit->block_terminated = true;
        jit_emit_epilogue(jit, JIT_EXIT_BRANCH);
        return consumed;

    /* Conditional jumps - terminate block */
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        jit->block_terminated = true;
        jit_emit_epilogue(jit, JIT_EXIT_BRANCH);
        return consumed;

    default:
        /* Unsupported instruction */
        return -1;
    }
}
