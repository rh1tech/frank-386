/*
 * i386_arm.h - ARM assembly optimizations for i386 emulator
 *
 * Declarations for ARM Cortex-M33 optimized functions.
 * These are implemented in i386_arm.S for RP2350 builds.
 */

#ifndef I386_ARM_H
#define I386_ARM_H

#include <stdint.h>

#ifdef RP2350_BUILD

/* Physical memory access - bypasses TLB */
extern uint8_t pload8_asm(uint8_t *phys_mem, uint32_t addr);
extern uint16_t pload16_asm(uint8_t *phys_mem, uint32_t addr);
extern uint32_t pload32_asm(uint8_t *phys_mem, uint32_t addr);
extern void pstore8_asm(uint8_t *phys_mem, uint32_t addr, uint8_t val);
extern void pstore16_asm(uint8_t *phys_mem, uint32_t addr, uint16_t val);
extern void pstore32_asm(uint8_t *phys_mem, uint32_t addr, uint32_t val);

/* Bit manipulation - BSF/BSR using ARM CLZ */
extern int bsf32_asm(uint32_t val);
extern int bsr32_asm(uint32_t val);

/* Sign extension */
extern int32_t sext8_asm(uint8_t val);
extern int32_t sext16_asm(uint16_t val);

/* Parity calculation for PF flag */
extern int parity8_asm(uint8_t val);

/* TLB fast path - returns 1 on hit, 0 on miss */
extern int tlb_lookup_fast(void *cpu, uint32_t lpgno, uint32_t *paddr_out);

/* Instruction fetch fast path - returns 1 on success */
extern int peek8_fast(void *cpu, uint8_t *val_out);

/* Flag computation fast paths */
extern int get_ZF_fast(uint32_t flags, uint32_t cc_mask, uint32_t cc_dst);
extern int get_SF_fast(uint32_t flags, uint32_t cc_mask, uint32_t cc_dst);
extern int get_CF_simple(uint32_t flags, uint32_t cc_mask);

/* Memory range check */
extern int in_iomem_fast(uint32_t addr);

/* Branchless bit set */
extern uint32_t set_bit_asm(uint32_t word, int flag, uint32_t mask);

/*
 * Inline assembly macros for hot paths that should be inlined
 * rather than called as functions.
 */

/* Fast byte load - single ARM instruction */
#define PLOAD8_INLINE(mem, addr) ({ \
    uint8_t __result; \
    __asm__ __volatile__( \
        "ldrb %0, [%1, %2]" \
        : "=r" (__result) \
        : "r" (mem), "r" (addr) \
    ); \
    __result; \
})

/* Fast halfword load - single ARM instruction */
#define PLOAD16_INLINE(mem, addr) ({ \
    uint16_t __result; \
    __asm__ __volatile__( \
        "ldrh %0, [%1, %2]" \
        : "=r" (__result) \
        : "r" (mem), "r" (addr) \
    ); \
    __result; \
})

/* Fast word load - single ARM instruction */
#define PLOAD32_INLINE(mem, addr) ({ \
    uint32_t __result; \
    __asm__ __volatile__( \
        "ldr %0, [%1, %2]" \
        : "=r" (__result) \
        : "r" (mem), "r" (addr) \
    ); \
    __result; \
})

/* Fast byte store - single ARM instruction */
#define PSTORE8_INLINE(mem, addr, val) do { \
    __asm__ __volatile__( \
        "strb %2, [%0, %1]" \
        : \
        : "r" (mem), "r" (addr), "r" ((uint8_t)(val)) \
        : "memory" \
    ); \
} while(0)

/* Fast halfword store - single ARM instruction */
#define PSTORE16_INLINE(mem, addr, val) do { \
    __asm__ __volatile__( \
        "strh %2, [%0, %1]" \
        : \
        : "r" (mem), "r" (addr), "r" ((uint16_t)(val)) \
        : "memory" \
    ); \
} while(0)

/* Fast word store - single ARM instruction */
#define PSTORE32_INLINE(mem, addr, val) do { \
    __asm__ __volatile__( \
        "str %2, [%0, %1]" \
        : \
        : "r" (mem), "r" (addr), "r" ((uint32_t)(val)) \
        : "memory" \
    ); \
} while(0)

/* BSF inline using CLZ - bit scan forward */
#define BSF32_INLINE(val) ({ \
    uint32_t __v = (val); \
    uint32_t __result; \
    __asm__ __volatile__( \
        "rbit %0, %1\n\t" \
        "clz %0, %0" \
        : "=r" (__result) \
        : "r" (__v) \
    ); \
    __result; \
})

/* BSR inline using CLZ - bit scan reverse */
#define BSR32_INLINE(val) ({ \
    uint32_t __v = (val); \
    uint32_t __result; \
    __asm__ __volatile__( \
        "clz %0, %1\n\t" \
        "rsb %0, %0, #31" \
        : "=r" (__result) \
        : "r" (__v) \
    ); \
    __result; \
})

/* Sign extend byte to word inline */
#define SEXT8_INLINE(val) ({ \
    int32_t __result; \
    __asm__ __volatile__( \
        "sxtb %0, %1" \
        : "=r" (__result) \
        : "r" ((uint32_t)(val)) \
    ); \
    __result; \
})

/* Sign extend halfword to word inline */
#define SEXT16_INLINE(val) ({ \
    int32_t __result; \
    __asm__ __volatile__( \
        "sxth %0, %1" \
        : "=r" (__result) \
        : "r" ((uint32_t)(val)) \
    ); \
    __result; \
})

/* Branchless SET_BIT using ARM conditional execution */
#define SET_BIT_INLINE(word, flag, mask) ({ \
    uint32_t __w = (word); \
    uint32_t __f = (flag) ? 0xffffffff : 0; \
    uint32_t __m = (mask); \
    __asm__ __volatile__( \
        "bic %0, %0, %2\n\t" \
        "and %1, %1, %2\n\t" \
        "orr %0, %0, %1" \
        : "+r" (__w), "+r" (__f) \
        : "r" (__m) \
    ); \
    __w; \
})

/*
 * Parity calculation using XOR cascade
 * More efficient than table lookup for single values
 */
#define PARITY8_INLINE(val) ({ \
    uint32_t __v = (val) & 0xff; \
    uint32_t __result; \
    __asm__ __volatile__( \
        "eor %0, %1, %1, lsr #4\n\t" \
        "eor %0, %0, %0, lsr #2\n\t" \
        "eor %0, %0, %0, lsr #1\n\t" \
        "mvn %0, %0\n\t" \
        "and %0, %0, #1" \
        : "=r" (__result) \
        : "r" (__v) \
    ); \
    __result; \
})

#else /* !RP2350_BUILD - fallback to C implementations */

/* Provide inline C implementations for non-RP2350 builds */
#define PLOAD8_INLINE(mem, addr) ((mem)[(addr)])
#define PLOAD16_INLINE(mem, addr) (*(uint16_t*)&((mem)[(addr)]))
#define PLOAD32_INLINE(mem, addr) (*(uint32_t*)&((mem)[(addr)]))
#define PSTORE8_INLINE(mem, addr, val) ((mem)[(addr)] = (val))
#define PSTORE16_INLINE(mem, addr, val) (*(uint16_t*)&((mem)[(addr)]) = (val))
#define PSTORE32_INLINE(mem, addr, val) (*(uint32_t*)&((mem)[(addr)]) = (val))

static inline int bsf32_fallback(uint32_t val) {
    int i;
    for (i = 0; i < 32 && !(val & (1u << i)); i++);
    return i;
}
static inline int bsr32_fallback(uint32_t val) {
    int i;
    for (i = 31; i >= 0 && !(val & (1u << i)); i--);
    return i;
}
#define BSF32_INLINE(val) bsf32_fallback(val)
#define BSR32_INLINE(val) bsr32_fallback(val)

#define SEXT8_INLINE(val) ((int32_t)(int8_t)(val))
#define SEXT16_INLINE(val) ((int32_t)(int16_t)(val))

#define SET_BIT_INLINE(word, flag, mask) \
    (((word) & ~((uint32_t)(mask))) | ((-((uint32_t)!!(flag))) & (mask)))

/* Parity table fallback */
extern const uint8_t parity_tab[256];
#define PARITY8_INLINE(val) parity_tab[(val) & 0xff]

#endif /* RP2350_BUILD */

#endif /* I386_ARM_H */
