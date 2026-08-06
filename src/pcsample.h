/**
 * frank-386 — statistical PC sampling profiler for core 0. See pcsample.c.
 */
#ifndef PCSAMPLE_H
#define PCSAMPLE_H

#include <stdint.h>

#if PC_SAMPLE

/* RAM text window. cpu_exec1 sits at ~0x20002c5c and spans 119 KB. */
#define PS_BASE    0x20000000u
#define PS_SPAN    0x00040000u          /* 256 KB */
#define PS_SHIFT   7                    /* 128-byte buckets = 8 KB histogram.
                                        * 64-byte buckets (16 KB) push RAM
                                        * near the point where pc_new() can no
                                        * longer allocate. */
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
