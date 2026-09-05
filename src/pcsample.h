/**
 * frank-386 — statistical PC sampling profiler for core 0. See pcsample.c.
 */
#ifndef PCSAMPLE_H
#define PCSAMPLE_H

#include <stdint.h>

#if PC_SAMPLE

/*
 * Which window the 512 buckets cover.
 *
 * The default is the RAM text window, where cpu_exec1 and the rest of the
 * interpreter live.  But a sample outside the window is only counted, not
 * placed, and on this board 28% of them landed outside - all of it code
 * executing from flash through the XIP cache, which is exactly the traffic
 * that competes with the guest's own PSRAM lines.  Building with
 * -DPS_WINDOW_FLASH=1 aims the same histogram at the flash text instead, at
 * 1 KB granularity, so that 28% can be named rather than guessed at.
 */
#ifndef PS_WINDOW_FLASH
#define PS_WINDOW_FLASH 0
#endif

#if PS_WINDOW_FLASH
#define PS_BASE    0x10000000u
#define PS_SPAN    0x00080000u          /* 512 KB of flash text */
#else
/* RAM text window. cpu_exec1 sits at ~0x20002c5c and spans 119 KB. */
#define PS_BASE    0x20000000u
#define PS_SPAN    0x00040000u          /* 256 KB */
#endif
#if PS_WINDOW_FLASH
#define PS_SHIFT   10                   /* 1 KB buckets over 512 KB = same 2 KB */
#else
#ifndef PC_SAMPLE_SHIFT
#define PC_SAMPLE_SHIFT 11
#endif
#define PS_SHIFT   PC_SAMPLE_SHIFT      /* Default: 2 KB buckets = 512-byte histogram.
                                        *
                                        * Was 7 (128-byte, 8 KB).  The note
                                        * below this line already warned that
                                        * 16 KB pushes RAM past the point where
                                        * pc_new() can allocate; 8 KB was
                                        * already inside that margin and the
                                        * v8.10.2 profiler build would not boot.
                                        *
                                        * 512-byte buckets (2 KB total) also
                                        * leave only 42120 bytes before pc_new()
                                        * on the optimized Z2 build, below its
                                        * requirement. 1 KB buckets still left
                                        * only 43144 bytes and also stalled in
                                        * pc_new(). A 512-byte histogram leaves
                                        * 43656 bytes, slightly more than the
                                        * validated release, and 2 KB buckets
                                        * remain sufficient to rank the large
                                        * interpreter functions targeted here.
                                        * A JIT-disabled diagnostic build can
                                        * set PC_SAMPLE_SHIFT=7 for 128-byte
                                        * buckets without exhausting SRAM. */
#endif
#define PS_BUCKETS (PS_SPAN >> PS_SHIFT)

extern uint32_t ps_hist[PS_BUCKETS];
extern volatile uint32_t ps_total;
extern volatile uint32_t ps_outside;

void ps_init(uint32_t sys_hz, uint32_t sample_hz);
void ps_stop(void);

#else

static inline void ps_init(uint32_t a, uint32_t b) { (void)a; (void)b; }
static inline void ps_stop(void) {}

#endif /* PC_SAMPLE */

#endif /* PCSAMPLE_H */
