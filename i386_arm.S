/*
 * i386_arm.S - ARM Cortex-M33 (RP2350) assembly optimizations for i386 emulator
 *
 * These functions are called millions of times per second in the hot path.
 * Each cycle saved here has significant impact on overall performance.
 *
 * Register conventions:
 *   r0-r3: Arguments and scratch (caller-saved)
 *   r4-r11: Callee-saved (must preserve)
 *   r12: Scratch (ip)
 *   r13: Stack pointer (sp)
 *   r14: Link register (lr)
 *   r15: Program counter (pc)
 *
 * ARM Cortex-M33 specific features used:
 *   - Thumb-2 instruction set
 *   - IT (If-Then) blocks for conditional execution
 *   - LDRB/LDRH/LDR for unaligned access (supported on M33)
 *   - CLZ (Count Leading Zeros) for BSF/BSR emulation
 */

    .syntax unified
    .cpu cortex-m33
    .thumb

    .section .time_critical.i386_arm, "ax", %progbits

/*
 * CPUI386 structure offsets (must match i386.c)
 * These are used to access CPU state from assembly
 */
#define CPU_GPRX_OFFSET     0       /* gprx[8] - 32 bytes */
#define CPU_IP_OFFSET       32      /* ip */
#define CPU_NEXT_IP_OFFSET  36      /* next_ip */
#define CPU_FLAGS_OFFSET    40      /* flags */
#define CPU_CPL_OFFSET      48      /* cpl */
#define CPU_CODE16_OFFSET   52      /* code16 */
#define CPU_SEG_OFFSET      64      /* seg[8] - each is 16 bytes (sel, base, limit, flags) */
#define CPU_SEG_CS_BASE     (CPU_SEG_OFFSET + 16 + 4)  /* seg[1].base */
#define CPU_CR0_OFFSET      200     /* cr0 - approximate */
#define CPU_IFETCH_OFFSET   168     /* ifetch.laddr, ifetch.xaddr */
#define CPU_CC_OFFSET       184     /* cc structure */
#define CPU_TLB_OFFSET      208     /* tlb structure */
#define CPU_PHYS_MEM_OFFSET 224     /* phys_mem pointer */
#define CPU_PHYS_MEM_SIZE_OFFSET 228 /* phys_mem_size */

/* TLB entry structure offsets */
#define TLB_LPGNO_OFFSET    0
#define TLB_XADDR_OFFSET    4
#define TLB_PTE_LOOKUP_OFFSET 8
#define TLB_PPTE_OFFSET     12
#define TLB_ENTRY_SIZE      16

/* tlb_size constant */
#define TLB_SIZE            512
#define TLB_SIZE_MASK       (TLB_SIZE - 1)

/* Flag bits */
#define FLAG_CF             0x001
#define FLAG_PF             0x004
#define FLAG_AF             0x010
#define FLAG_ZF             0x040
#define FLAG_SF             0x080
#define FLAG_OF             0x800

/* CR0 bits */
#define CR0_PG              0x80000000

/*
 * ============================================================================
 * PHYSICAL MEMORY ACCESS - These bypass TLB and are used after address
 * translation is complete. Called millions of times per second.
 * ============================================================================
 */

/*
 * uint8_t pload8_asm(uint8_t *phys_mem, uint32_t addr)
 *
 * Load a single byte from physical memory.
 * Input:  r0 = phys_mem base pointer
 *         r1 = physical address
 * Output: r0 = loaded byte (zero-extended)
 */
    .global pload8_asm
    .type pload8_asm, %function
    .thumb_func
pload8_asm:
    ldrb    r0, [r0, r1]        /* Load byte from phys_mem + addr */
    bx      lr                   /* Return */
    .size pload8_asm, .-pload8_asm

/*
 * uint16_t pload16_asm(uint8_t *phys_mem, uint32_t addr)
 *
 * Load a 16-bit word from physical memory (unaligned ok on M33).
 * Input:  r0 = phys_mem base pointer
 *         r1 = physical address
 * Output: r0 = loaded halfword (zero-extended)
 */
    .global pload16_asm
    .type pload16_asm, %function
    .thumb_func
pload16_asm:
    ldrh    r0, [r0, r1]        /* Load halfword (unaligned ok) */
    bx      lr
    .size pload16_asm, .-pload16_asm

/*
 * uint32_t pload32_asm(uint8_t *phys_mem, uint32_t addr)
 *
 * Load a 32-bit word from physical memory (unaligned ok on M33).
 * Input:  r0 = phys_mem base pointer
 *         r1 = physical address
 * Output: r0 = loaded word
 */
    .global pload32_asm
    .type pload32_asm, %function
    .thumb_func
pload32_asm:
    ldr     r0, [r0, r1]        /* Load word (unaligned ok) */
    bx      lr
    .size pload32_asm, .-pload32_asm

/*
 * void pstore8_asm(uint8_t *phys_mem, uint32_t addr, uint8_t val)
 *
 * Store a single byte to physical memory.
 * Input:  r0 = phys_mem base pointer
 *         r1 = physical address
 *         r2 = value to store
 */
    .global pstore8_asm
    .type pstore8_asm, %function
    .thumb_func
pstore8_asm:
    strb    r2, [r0, r1]        /* Store byte to phys_mem + addr */
    bx      lr
    .size pstore8_asm, .-pstore8_asm

/*
 * void pstore16_asm(uint8_t *phys_mem, uint32_t addr, uint16_t val)
 *
 * Store a 16-bit word to physical memory.
 * Input:  r0 = phys_mem base pointer
 *         r1 = physical address
 *         r2 = value to store
 */
    .global pstore16_asm
    .type pstore16_asm, %function
    .thumb_func
pstore16_asm:
    strh    r2, [r0, r1]        /* Store halfword */
    bx      lr
    .size pstore16_asm, .-pstore16_asm

/*
 * void pstore32_asm(uint8_t *phys_mem, uint32_t addr, uint32_t val)
 *
 * Store a 32-bit word to physical memory.
 * Input:  r0 = phys_mem base pointer
 *         r1 = physical address
 *         r2 = value to store
 */
    .global pstore32_asm
    .type pstore32_asm, %function
    .thumb_func
pstore32_asm:
    str     r2, [r0, r1]        /* Store word */
    bx      lr
    .size pstore32_asm, .-pstore32_asm

/*
 * ============================================================================
 * BIT MANIPULATION - Using ARM CLZ for efficient BSF/BSR implementation
 * ============================================================================
 */

/*
 * int bsf32_asm(uint32_t val)
 *
 * Bit Scan Forward - find index of lowest set bit (like x86 BSF)
 * Input:  r0 = value to scan
 * Output: r0 = bit index (0-31), undefined if input is 0
 *
 * Algorithm: BSF(x) = 31 - CLZ(x & -x)
 *            x & -x isolates the lowest set bit
 */
    .global bsf32_asm
    .type bsf32_asm, %function
    .thumb_func
bsf32_asm:
    rsbs    r1, r0, #0          /* r1 = -r0 (negate) */
    ands    r0, r0, r1          /* r0 = r0 & -r0 (isolate lowest bit) */
    clz     r0, r0              /* r0 = count leading zeros */
    rsb     r0, r0, #31         /* r0 = 31 - clz = bit index */
    bx      lr
    .size bsf32_asm, .-bsf32_asm

/*
 * int bsr32_asm(uint32_t val)
 *
 * Bit Scan Reverse - find index of highest set bit (like x86 BSR)
 * Input:  r0 = value to scan
 * Output: r0 = bit index (0-31), undefined if input is 0
 *
 * Algorithm: BSR(x) = 31 - CLZ(x)
 */
    .global bsr32_asm
    .type bsr32_asm, %function
    .thumb_func
bsr32_asm:
    clz     r0, r0              /* r0 = count leading zeros */
    rsb     r0, r0, #31         /* r0 = 31 - clz = bit index */
    bx      lr
    .size bsr32_asm, .-bsr32_asm

/*
 * ============================================================================
 * SIGN EXTENSION - Used constantly in instruction decoding
 * ============================================================================
 */

/*
 * int32_t sext8_asm(uint8_t val)
 *
 * Sign-extend 8-bit value to 32-bit.
 * Input:  r0 = 8-bit value (in low byte)
 * Output: r0 = sign-extended 32-bit value
 */
    .global sext8_asm
    .type sext8_asm, %function
    .thumb_func
sext8_asm:
    sxtb    r0, r0              /* Sign-extend byte to word */
    bx      lr
    .size sext8_asm, .-sext8_asm

/*
 * int32_t sext16_asm(uint16_t val)
 *
 * Sign-extend 16-bit value to 32-bit.
 * Input:  r0 = 16-bit value (in low halfword)
 * Output: r0 = sign-extended 32-bit value
 */
    .global sext16_asm
    .type sext16_asm, %function
    .thumb_func
sext16_asm:
    sxth    r0, r0              /* Sign-extend halfword to word */
    bx      lr
    .size sext16_asm, .-sext16_asm

/*
 * ============================================================================
 * PARITY CALCULATION - Used for x86 PF flag
 * ARM doesn't have a direct parity instruction, but we can compute it
 * ============================================================================
 */

/*
 * int parity8_asm(uint8_t val)
 *
 * Calculate parity of low 8 bits (1 if even number of 1s, 0 if odd).
 * Input:  r0 = 8-bit value
 * Output: r0 = 1 if even parity, 0 if odd parity
 *
 * Algorithm: XOR all bits together, invert result
 * p = v ^ (v >> 4)
 * p = p ^ (p >> 2)
 * p = p ^ (p >> 1)
 * return ~p & 1
 */
    .global parity8_asm
    .type parity8_asm, %function
    .thumb_func
parity8_asm:
    eor     r1, r0, r0, lsr #4  /* r1 = v ^ (v >> 4) */
    eor     r1, r1, r1, lsr #2  /* r1 = p ^ (p >> 2) */
    eor     r1, r1, r1, lsr #1  /* r1 = p ^ (p >> 1) */
    mvn     r0, r1              /* r0 = ~p */
    and     r0, r0, #1          /* r0 = ~p & 1 */
    bx      lr
    .size parity8_asm, .-parity8_asm

/*
 * ============================================================================
 * TLB FAST PATH - The most critical function in the emulator
 * This is called for every memory access when paging is enabled.
 * ============================================================================
 */

/*
 * int tlb_lookup_fast(CPUI386 *cpu, uint32_t lpgno, uint32_t *paddr_out)
 *
 * Fast TLB lookup - returns 1 on hit, 0 on miss.
 * On hit, writes physical address to *paddr_out.
 *
 * Input:  r0 = cpu pointer
 *         r1 = logical page number (laddr >> 12)
 *         r2 = pointer to store physical address
 * Output: r0 = 1 on TLB hit, 0 on miss
 *
 * This function ONLY handles the fast path (TLB hit with valid permissions).
 * On miss or permission fault, caller must handle via C code.
 */
    .global tlb_lookup_fast
    .type tlb_lookup_fast, %function
    .thumb_func
tlb_lookup_fast:
    push    {r4, r5, lr}

    /* Calculate TLB entry address: &cpu->tlb.tab[lpgno % TLB_SIZE] */
    /* tlb.tab is at cpu->tlb + 4 (after size field) */
    ldr     r3, [r0, #CPU_TLB_OFFSET + 4]   /* r3 = tlb.tab pointer */
    /* lpgno % 512 = extract low 9 bits using UBFX */
    ubfx    r4, r1, #0, #9                  /* r4 = lpgno & 0x1ff (low 9 bits) */
    add     r3, r3, r4, lsl #4              /* r3 = &tab[index] (entry size = 16) */

    /* Check if TLB entry matches: ent->lpgno == lpgno */
    ldr     r4, [r3, #TLB_LPGNO_OFFSET]     /* r4 = ent->lpgno */
    cmp     r4, r1                           /* Compare with requested lpgno */
    bne     .Ltlb_miss                       /* Branch if miss */

    /* TLB hit - calculate physical address */
    /* paddr = ent->xaddr ^ (lpgno << 12) */
    ldr     r4, [r3, #TLB_XADDR_OFFSET]     /* r4 = ent->xaddr */
    lsl     r5, r1, #12                      /* r5 = lpgno << 12 */
    eor     r4, r4, r5                       /* r4 = physical address */
    str     r4, [r2]                         /* Store result */

    mov     r0, #1                           /* Return 1 (hit) */
    pop     {r4, r5, pc}

.Ltlb_miss:
    mov     r0, #0                           /* Return 0 (miss) */
    pop     {r4, r5, pc}
    .size tlb_lookup_fast, .-tlb_lookup_fast

/*
 * ============================================================================
 * INSTRUCTION FETCH FAST PATH
 * ============================================================================
 */

/*
 * int peek8_fast(CPUI386 *cpu, uint8_t *val_out)
 *
 * Fast path for instruction fetch when within same page.
 * Returns 1 on success (fast path hit), 0 if slow path needed.
 *
 * Input:  r0 = cpu pointer
 *         r1 = pointer to store fetched byte
 * Output: r0 = 1 on success, 0 if slow path needed
 *
 * Fast path condition: (laddr ^ ifetch.laddr) < 4096
 * This means we're in the same page as the cached ifetch.
 */
    .global peek8_fast
    .type peek8_fast, %function
    .thumb_func
peek8_fast:
    push    {r4, r5, r6, lr}

    /* Load seg[CS].base */
    ldr     r2, [r0, #CPU_SEG_CS_BASE]      /* r2 = seg[CS].base */

    /* Load next_ip */
    ldr     r3, [r0, #CPU_NEXT_IP_OFFSET]   /* r3 = next_ip */

    /* Calculate laddr = seg[CS].base + next_ip */
    add     r4, r2, r3                       /* r4 = laddr */

    /* Load ifetch.laddr */
    ldr     r5, [r0, #CPU_IFETCH_OFFSET]    /* r5 = ifetch.laddr */

    /* Check if same page: (laddr ^ ifetch.laddr) < 4096 */
    eor     r6, r4, r5                       /* r6 = laddr ^ ifetch.laddr */
    cmp     r6, #4096
    bhs     .Lpeek8_slow                     /* Branch if different page */

    /* Fast path - load from cached page */
    /* addr = ifetch.xaddr ^ laddr */
    ldr     r5, [r0, #CPU_IFETCH_OFFSET + 4] /* r5 = ifetch.xaddr */
    eor     r4, r5, r4                       /* r4 = physical addr */

    /* Load phys_mem pointer */
    ldr     r5, [r0, #CPU_PHYS_MEM_OFFSET]  /* r5 = phys_mem */

    /* Load the byte */
    ldrb    r4, [r5, r4]                     /* r4 = byte value */
    strb    r4, [r1]                         /* Store to output */

    mov     r0, #1                           /* Return 1 (success) */
    pop     {r4, r5, r6, pc}

.Lpeek8_slow:
    mov     r0, #0                           /* Return 0 (need slow path) */
    pop     {r4, r5, r6, pc}
    .size peek8_fast, .-peek8_fast

/*
 * ============================================================================
 * FLAG COMPUTATION FAST PATHS
 * ============================================================================
 */

/*
 * int get_ZF_fast(uint32_t flags, uint32_t cc_mask, uint32_t cc_dst)
 *
 * Fast ZF (Zero Flag) computation.
 * Input:  r0 = cpu->flags
 *         r1 = cpu->cc.mask
 *         r2 = cpu->cc.dst
 * Output: r0 = ZF value (0 or 1)
 *
 * If !(cc.mask & ZF): return !!(flags & ZF)
 * Else: return (cc.dst == 0)
 */
    .global get_ZF_fast
    .type get_ZF_fast, %function
    .thumb_func
get_ZF_fast:
    tst     r1, #FLAG_ZF            /* Test if ZF is lazy */
    bne     .Lzf_lazy               /* Branch if lazy */

    /* Not lazy - return from flags */
    tst     r0, #FLAG_ZF            /* Test ZF in flags */
    ite     ne                       /* If-Then-Else */
    movne   r0, #1                   /* ZF set */
    moveq   r0, #0                   /* ZF clear */
    bx      lr

.Lzf_lazy:
    /* Lazy - compute from dst */
    cmp     r2, #0                   /* Compare dst with 0 */
    ite     eq                       /* If-Then-Else */
    moveq   r0, #1                   /* dst == 0, ZF = 1 */
    movne   r0, #0                   /* dst != 0, ZF = 0 */
    bx      lr
    .size get_ZF_fast, .-get_ZF_fast

/*
 * int get_SF_fast(uint32_t flags, uint32_t cc_mask, uint32_t cc_dst)
 *
 * Fast SF (Sign Flag) computation.
 * Input:  r0 = cpu->flags
 *         r1 = cpu->cc.mask
 *         r2 = cpu->cc.dst
 * Output: r0 = SF value (0 or 1)
 *
 * If !(cc.mask & SF): return !!(flags & SF)
 * Else: return cc.dst >> 31
 */
    .global get_SF_fast
    .type get_SF_fast, %function
    .thumb_func
get_SF_fast:
    tst     r1, #FLAG_SF            /* Test if SF is lazy */
    bne     .Lsf_lazy               /* Branch if lazy */

    /* Not lazy - return from flags */
    tst     r0, #FLAG_SF            /* Test SF in flags */
    ite     ne                       /* If-Then-Else */
    movne   r0, #1                   /* SF set */
    moveq   r0, #0                   /* SF clear */
    bx      lr

.Lsf_lazy:
    /* Lazy - compute from dst (sign bit) */
    lsr     r0, r2, #31              /* r0 = dst >> 31 */
    bx      lr
    .size get_SF_fast, .-get_SF_fast

/*
 * int get_CF_simple(uint32_t flags, uint32_t cc_mask)
 *
 * Fast CF check for non-lazy case only.
 * Returns CF from flags, or -1 if lazy (caller must compute).
 *
 * Input:  r0 = cpu->flags
 *         r1 = cpu->cc.mask
 * Output: r0 = CF value (0 or 1), or -1 if lazy
 */
    .global get_CF_simple
    .type get_CF_simple, %function
    .thumb_func
get_CF_simple:
    tst     r1, #FLAG_CF            /* Test if CF is lazy */
    bne     .Lcf_lazy               /* Branch if lazy */

    /* Not lazy - return from flags */
    and     r0, r0, #FLAG_CF        /* Isolate CF bit */
    bx      lr

.Lcf_lazy:
    mov     r0, #-1                  /* Return -1 (need slow path) */
    bx      lr
    .size get_CF_simple, .-get_CF_simple

/*
 * ============================================================================
 * MEMORY RANGE CHECK
 * ============================================================================
 */

/*
 * int in_iomem_fast(uint32_t addr)
 *
 * Fast check if address is in I/O memory range.
 * Returns 1 if in I/O memory, 0 otherwise.
 *
 * I/O memory: 0xa0000-0xbffff (VGA) or >= 0xe0000000 (PCI)
 */
    .global in_iomem_fast
    .type in_iomem_fast, %function
    .thumb_func
in_iomem_fast:
    /* Check if addr >= 0xe0000000 (PCI region) */
    movw    r1, #0x0000
    movt    r1, #0xe000             /* r1 = 0xe0000000 */
    cmp     r0, r1
    bhs     .Lis_iomem              /* Branch if >= 0xe0000000 */

    /* Check if 0xa0000 <= addr < 0xc0000 (VGA region) */
    movw    r1, #0xa0000 & 0xffff
    movt    r1, #0xa0000 >> 16      /* r1 = 0xa0000 */
    cmp     r0, r1
    blo     .Lnot_iomem             /* Branch if < 0xa0000 */

    movw    r1, #0xc0000 & 0xffff
    movt    r1, #0xc0000 >> 16      /* r1 = 0xc0000 */
    cmp     r0, r1
    bhs     .Lnot_iomem             /* Branch if >= 0xc0000 */

.Lis_iomem:
    mov     r0, #1
    bx      lr

.Lnot_iomem:
    mov     r0, #0
    bx      lr
    .size in_iomem_fast, .-in_iomem_fast

/*
 * ============================================================================
 * SET_BIT MACRO IMPLEMENTATION
 * Branchless bit set/clear used extensively in flag updates
 * ============================================================================
 */

/*
 * uint32_t set_bit_asm(uint32_t word, int flag, uint32_t mask)
 *
 * Set or clear a bit in word based on flag value.
 * Equivalent to: word = (word & ~mask) | (-flag & mask)
 *
 * Input:  r0 = word
 *         r1 = flag (0 or non-zero)
 *         r2 = mask
 * Output: r0 = modified word
 */
    .global set_bit_asm
    .type set_bit_asm, %function
    .thumb_func
set_bit_asm:
    bic     r0, r0, r2              /* r0 = word & ~mask */
    rsbs    r1, r1, #0              /* r1 = -flag (0 or 0xffffffff) */
    and     r1, r1, r2              /* r1 = -flag & mask */
    orr     r0, r0, r1              /* r0 = (word & ~mask) | (-flag & mask) */
    bx      lr
    .size set_bit_asm, .-set_bit_asm

    .end
