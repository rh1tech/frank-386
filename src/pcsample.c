/**
 * frank-386 — statistical PC sampling profiler for core 0.
 *
 * SPDX-License-Identifier: MIT
 *
 * The interpreter costs a measured 273 core cycles per guest instruction
 * (docs/C2_SPLIT_PLAN.md §6c) and it is compute-bound, so the useful
 * question is which code inside cpu_exec1's 119 KB actually burns them.
 *
 * Why sampling rather than instrumentation: adding DWT reads to the hot
 * loop measurably slows the machine — that mistake has already been made
 * twice here — and any figure taken from an instrumented build is
 * suspect. A SysTick interrupt that records the interrupted PC costs
 * ~0.1% at 10 kHz and does not distort what it measures.
 *
 * Why not halt over SWD and read PC: halting an RP2350 under OpenOCD
 * clears CPACR, and doing it repeatedly already left this board
 * unreachable once, needing a power cycle. Not something to do
 * unattended.
 *
 * SysTick is per-core and core 1 uses the alarm pool, so core 0's
 * SysTick is free.
 */
#include "pcsample.h"

#if PC_SAMPLE

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/structs/systick.h"

/* Histogram over the RAM-resident text region. cpu_exec1 lives at
 * ~0x20002c5c and runs 119 KB; 0x20000000..0x20040000 at 64-byte
 * granularity covers it with room to spare for 16 KB of counters. */
uint32_t ps_hist[PS_BUCKETS];
volatile uint32_t ps_total __attribute__((used));
volatile uint32_t ps_outside __attribute__((used));

/* Called from the naked handler with the interrupted PC. */
void __not_in_flash_func(ps_record)(uint32_t pc) {
    ps_total++;
    const uint32_t off = pc - PS_BASE;
    if (off >= PS_SPAN) { ps_outside++; return; }
    ps_hist[off >> PS_SHIFT]++;
}

/*
 * Naked so the exception frame is where the hardware left it. A normal C
 * handler's prologue would push registers first and the stacked PC would
 * no longer be at a known offset.
 *
 * EXC_RETURN bit 2 selects which stack the frame is on; the PC sits at
 * offset 24 of the eight-word hardware frame. r0-r3/r12 are already
 * stacked by the exception, so ps_record may clobber them freely.
 */
__attribute__((naked)) void __not_in_flash_func(isr_systick)(void) {
    __asm volatile(
        "mov   r0, lr            \n"
        "tst   r0, #4            \n"
        "ite   eq                \n"
        "mrseq r0, msp           \n"
        "mrsne r0, psp           \n"
        "ldr   r0, [r0, #24]     \n"   /* stacked PC */
        "push  {r4, lr}          \n"   /* keep 8-byte alignment */
        "bl    ps_record         \n"
        "pop   {r4, lr}          \n"
        "bx    lr                \n"
    );
}

void ps_init(uint32_t sys_hz, uint32_t sample_hz) {
    memset(ps_hist, 0, sizeof(ps_hist));
    ps_total = 0;
    ps_outside = 0;

    uint32_t reload = sys_hz / sample_hz;
    if (reload > 0x00FFFFFFu) reload = 0x00FFFFFFu;   /* 24-bit */
    if (reload < 2u) reload = 2u;

    systick_hw->csr = 0;
    systick_hw->rvr = reload - 1u;
    systick_hw->cvr = 0;
    /* CLKSOURCE=processor | TICKINT | ENABLE */
    systick_hw->csr = (1u << 2) | (1u << 1) | (1u << 0);
}

void ps_stop(void) { systick_hw->csr = 0; }

#endif /* PC_SAMPLE */
