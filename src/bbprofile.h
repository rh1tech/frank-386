/**
 * frank-386 — basic-block discovery and reuse profiling.
 *
 * SPDX-License-Identifier: MIT
 *
 * Stage 1 of the JIT. Before writing any code generator, three numbers
 * have to be known, and none of them can be guessed:
 *
 *   1. Average block length. A block cache amortises fetch and dispatch
 *      over a block, so two-instruction blocks would make the whole
 *      exercise pointless. Instruction fetch is 29% of core-0 time
 *      (docs/C2_SPLIT_PLAN.md §6e), and that is what a cache removes.
 *
 *   2. Reuse. If blocks are executed once and never revisited,
 *      compilation cost is never repaid. Loops are the case that pays.
 *
 *   3. Hot-block concentration. If 90% of execution sits in a few
 *      hundred blocks, a small SRAM cache is enough and the master can
 *      host it; if it is spread over thousands, the cache has to live in
 *      the slave's 450 KB and the architecture changes.
 *
 * Block detection is deliberately approximate. A start is any ip that
 * is not 1..15 bytes after the previous one — that catches every
 * backward branch and every long forward jump, so it finds loops, which
 * is what a code cache lives on. It misses short forward jumps within
 * 15 bytes, which merges two blocks into one and slightly overstates
 * average block length. Costing one subtract and one compare in the
 * interpreter's hot loop is what buys that approximation.
 */
#ifndef BBPROFILE_H
#define BBPROFILE_H

#include <stdint.h>

#if BB_PROFILE

/* Direct-mapped, 2048 entries x 8 bytes = 16 KB.
 *
 * 4096 slots (32 KB) pushed .bss to 93.6% and left pc_new()'s heap
 * allocations with nothing — the emulator reached vga_initialized and
 * then never finished init. The master's free SRAM is the real budget
 * here, and it is small; that is itself a data point for where a JIT
 * code cache can live. */
#define BB_BITS    11
#define BB_SLOTS   (1u << BB_BITS)

typedef struct { uint32_t tag; uint32_t hits; } bb_slot_t;
extern bb_slot_t bb_tab[BB_SLOTS];

extern uint32_t bb_entries;    /* block starts observed        */
extern uint32_t bb_collisions; /* tag mismatches (evictions)   */

/* Called from cpu_exec1's loop head with the linear address of the
 * instruction about to run, and the previous instruction's address. */
static inline void __attribute__((always_inline))
bb_note(uint32_t ip, uint32_t prev_ip) {
    const uint32_t delta = ip - prev_ip;
    if (delta - 1u < 15u) return;          /* ordinary sequential flow */

    bb_entries++;
    /* Fibonacci hash: cheap and spreads low-entropy code addresses. */
    const uint32_t h = (ip * 2654435761u) >> (32 - BB_BITS);
    bb_slot_t *s = &bb_tab[h];
    if (s->tag != ip) {
        if (s->tag) bb_collisions++;
        s->tag = ip;
        s->hits = 1;
    } else {
        s->hits++;
    }
}

void bb_report(void);

#else

static inline void bb_note(uint32_t ip, uint32_t prev_ip) { (void)ip; (void)prev_ip; }
static inline void bb_report(void) {}

#endif /* BB_PROFILE */

#endif /* BBPROFILE_H */
