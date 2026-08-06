/**
 * frank-386 — guest code footprint measurement.
 *
 * SPDX-License-Identifier: MIT
 *
 * Sizing question for a JIT: how much *distinct guest code* does a real
 * workload execute? Translated ARM blocks have to live somewhere, and on
 * this board that somewhere is tight — the master has roughly 54 KB of
 * SRAM free, and running translated code out of PSRAM would reintroduce
 * exactly the fetch cost a JIT exists to remove.
 *
 * So the number that decides feasibility is the guest code working set,
 * times an x86 -> ARM expansion factor. This measures the first half.
 *
 * Hooked into prefetch_fill(), which the interpreter calls once per
 * 16-byte block of guest code it needs. That makes it a direct
 * observation of code actually executed, not of code merely resident.
 *
 * Two figures are kept:
 *
 *   cumulative — every distinct block since boot. Upper bound on a cache
 *                that never evicts.
 *   window     — distinct blocks since the last report. This is the one
 *                that matters, because a JIT cache holds what is being
 *                used now, not everything ever seen.
 *
 * Resolution is 64 bytes over the low 4 MB. DOS lives well below that
 * ceiling; anything above it is counted separately rather than being
 * silently dropped, so an over-4MB workload cannot masquerade as a small
 * one.
 */
#ifndef CODEPROFILE_H
#define CODEPROFILE_H

#include <stdint.h>

#if CODE_PROFILE

#define CP_GRAN_SHIFT 6                       /* 64-byte blocks */
#define CP_SPAN       (4u * 1024u * 1024u)    /* low 4 MB       */
#define CP_BLOCKS     (CP_SPAN >> CP_GRAN_SHIFT)
#define CP_WORDS      (CP_BLOCKS / 32u)

extern uint32_t cp_seen_cum[CP_WORDS];
extern uint32_t cp_seen_win[CP_WORDS];
extern uint32_t cp_over_span;   /* fetches above CP_SPAN */

/*
 * Event counters for costing the "interpreter on the slave" split.
 *
 * If the interpreter moves to the second chip, every VGA memory access
 * and every port I/O becomes a link round trip — 89 cycles, against a
 * 7-cycle SRAM store today. These rates say how much that would cost,
 * and they need measuring before any compiler gets written, because a
 * graphics-heavy workload could hand back everything a JIT wins.
 */
extern uint32_t cp_vga_w, cp_vga_r, cp_io_w, cp_io_r;

static inline void __attribute__((always_inline)) cp_vga_write(void) { cp_vga_w++; }
static inline void __attribute__((always_inline)) cp_vga_read(void)  { cp_vga_r++; }
static inline void __attribute__((always_inline)) cp_io_write(void)  { cp_io_w++; }
static inline void __attribute__((always_inline)) cp_io_read(void)   { cp_io_r++; }

/* Called from prefetch_fill() with the physical address of the block. */
static inline void __attribute__((always_inline)) cp_note(uint32_t paddr) {
    if (paddr >= CP_SPAN) { cp_over_span++; return; }
    const uint32_t b = paddr >> CP_GRAN_SHIFT;
    const uint32_t w = b >> 5, m = 1u << (b & 31u);
    cp_seen_cum[w] |= m;
    cp_seen_win[w] |= m;
}

/* Tally both bitmaps, publish to g_code[], clear the window. */
void cp_report(uint32_t window_ms);

#else

static inline void cp_note(uint32_t paddr) { (void)paddr; }
static inline void cp_report(uint32_t window_ms) { (void)window_ms; }
static inline void cp_vga_write(void) {}
static inline void cp_vga_read(void)  {}
static inline void cp_io_write(void)  {}
static inline void cp_io_read(void)   {}

#endif /* CODE_PROFILE */

#endif /* CODEPROFILE_H */
