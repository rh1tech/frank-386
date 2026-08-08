/**
 * frank-386 — coarse subsystem cycle accounting for core 0.
 *
 * Answers one question: of the time core 0 spends, how much is the i386
 * interpreter and how much is everything else? That is the number the
 * C2 offload plan is built on (docs/C2_SPLIT_PLAN.md), and guessing at
 * it would be a poor way to decide what to move to the second RP2350.
 *
 * Distinct from I386_PROFILE, which profiles *instructions*. This
 * profiles *subsystems*.
 *
 * Enable with -DSUBSYS_PROFILE=1 (./build.sh --subsys-profile). When it
 * is off every macro here compiles to nothing.
 *
 * Timing comes from the Cortex-M33 DWT cycle counter rather than
 * time_us_32(): a pc_step() is on the order of 100 us but the device
 * ticks inside it are well under a microsecond, and quantising those to
 * 1 us would bias exactly the small-but-frequent costs worth finding.
 *
 * CYCCNT is per-core and this only ever runs on core 0. It wraps every
 * 2^32 cycles (~11 s at 378 MHz); differences are taken in uint32 so a
 * wrap inside one interval is still correct, and the accumulators are
 * 64-bit so the totals are not.
 */
#ifndef PROFILE_SUBSYS_H
#define PROFILE_SUBSYS_H

#include <stdint.h>

#if SUBSYS_PROFILE

/* DWT / DEMCR, ARMv8-M debug block. */
#define PROF_DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define PROF_DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define PROF_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)

#define PROF_DEMCR_TRCENA    (1u << 24)
#define PROF_DWT_CYCCNTENA   (1u << 0)

typedef struct {
    /* Everything below is in core-0 cycles. */
    uint64_t cpu;      /* cpui386_step()  — the interpreter itself      */
    uint64_t adlib;    /* adlib_core0()   — OPL2 synthesis on core 0    */
    uint64_t devices;  /* PIT/CMOS/8042/8257x2/FDC/vga_step per step    */
    uint64_t poll;     /* pc->poll()      — USB host + input polling    */
    uint64_t refresh;  /* vga_refresh()   — a no-op on RP2350, verify   */
    uint64_t disk;     /* disk_read/disk_write — nested inside `cpu`    */
    uint64_t total;    /* whole pc_step()                               */
    uint32_t steps;
    uint32_t disk_ops;
} prof_subsys_t;

extern prof_subsys_t g_prof;

static inline void prof_init(void) {
    PROF_DEMCR |= PROF_DEMCR_TRCENA;
    PROF_DWT_CYCCNT = 0;
    PROF_DWT_CTRL |= PROF_DWT_CYCCNTENA;
}

static inline uint32_t prof_now(void) { return PROF_DWT_CYCCNT; }

#define PROF_T(v)          uint32_t v = prof_now()
#define PROF_ADD(v, field) do { g_prof.field += (uint32_t)(prof_now() - (v)); } while (0)

/* Dump and reset. Called from pc_step() every PROF_REPORT_STEPS steps. */
void prof_report(void);

/* One-shot memory latency benchmark, printed once at startup. */
void prof_mem_bench(void);

/* Results, also readable over SWD when the console is unavailable:
 *   [0] PSRAM random cyc/access   [3] clk_sys MHz
 *   [1] PSRAM seq    cyc/access   [4] link round trip cycles (0xFFFFFFFF = no slave)
 *   [2] SRAM  random cyc/access   [5] link data-check failures out of 4096 */
extern volatile uint32_t g_bench[8];

#ifdef BOARD_C2
/* Bring up the fast link and measure a round trip against the slave. */
void prof_link_bench(void);
#endif

#ifndef PROF_REPORT_STEPS
#define PROF_REPORT_STEPS 2000u
#endif

#else /* !SUBSYS_PROFILE */

static inline void prof_init(void) {}
static inline void prof_mem_bench(void) {}
static inline void prof_link_bench(void) {}
#define PROF_T(v)          ((void)0)
#define PROF_ADD(v, field) ((void)0)

#endif /* SUBSYS_PROFILE */

#endif /* PROFILE_SUBSYS_H */
