/*
 * frank-386 — C2 slave firmware (RP2350A, U6)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase 1: a memory server, and nothing else.
 *
 * The point is to answer one question with a measurement rather than
 * arithmetic — how many core cycles does a link round trip actually
 * cost? The master's own PSRAM is 184 cycles per random access
 * (docs/C2_SPLIT_PLAN.md §6b). If a round trip here is meaningfully
 * below that, the slave's otherwise-idle SRAM is a *faster* tier than
 * the master's guest RAM and the whole shape of the split changes.
 *
 * So this firmware does the least that can produce that number: bring up
 * the link, hand a block of SRAM to linkf_serve(), and spin. No PSRAM,
 * no sound, no PSRAM-backed tier yet.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"

#include "link_fast.h"
#include "link_pins.h"
#include "psram_init.h"

#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 252
#endif
#ifndef CPU_VOLTAGE
#define CPU_VOLTAGE VREG_VOLTAGE_1_50
#endif

/*
 * Flash QMI timing for overclocked operation.
 *
 * Not optional, and not obvious: the XIP flash divider is derived from
 * the system clock, so raising the core without widening it runs the
 * QSPI far out of spec and the chip faults before reaching main().
 * murmgenesis lost a debugging session to exactly this — the symptom is
 * a slave that appears completely dead, which reads as a link fault.
 */
#define FLASH_MAX_FREQ_MHZ 88

static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = FLASH_MAX_FREQ_MHZ * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) divisor = 2;

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) rxdelay += 1;

    qmi_hw->m[0].timing = 0x60007000 |
                          rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                          divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

/*
 * The served region: the slave's own 8 MB PSRAM, XIP-mapped at
 * 0x11000000.
 *
 * SRAM would be faster to reach locally, but the master is going to
 * spend its round trips on the wire, not on the slave's memory type —
 * and the disk cache needs capacity, not latency. 8 MB holds 2048 blocks
 * of 4 KB, which is what the master's tag array can index.
 */
#define SERVED_BYTES (8u * 1024u * 1024u)
static uint32_t *served = (uint32_t *)0x11000000u;

int main(void) {
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    sleep_ms(50);
    set_flash_timings(CPU_CLOCK_MHZ);
    set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false);

    stdio_init_all();

    gpio_init(S_LED_PIN);
    gpio_set_dir(S_LED_PIN, GPIO_OUT);
    gpio_put(S_LED_PIN, 1);

    printf("\nfrank-386 C2 slave — memory server\n");
    printf("  clk_sys %lu MHz, serving %u KB of SRAM\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000u),
           (unsigned)(SERVED_BYTES / 1024u));

    /* PSRAM must be up before anything is served out of it. */
    psram_init(S_PSRAM_CS_PIN);
    if (!psram_test()) {
        printf("  PSRAM test FAILED - cache will return garbage\n");
    } else {
        printf("  PSRAM 8 MB OK\n");
    }

    /* A recognisable pattern in the first pages so the master can tell a
     * real reply from a floating bus or a zeroed FIFO. */
    for (uint32_t i = 0; i < 4096u; i++) {
        served[i] = i ^ 0x5A5A0000u;
    }

    /* Slave: RX on bus A (master -> slave), TX on bus B (slave ->
     * master). All slave link pins are below 32, so no gpio base needed. */
    linkf_init(LINK_PIO_SLAVE, S_LINK_B_DATA_BASE, S_LINK_A_DATA_BASE, 1.0f);

    /* Wait for the master's PIO to come up, then align the receivers.
     * No timeout worth having here — the slave has nothing else to do,
     * and the master may be flashed or reset long after this one. */
    printf("  link init done, waiting for master VALID...\n");
    while (!linkf_sync(S_LINK_B_VALID, S_LINK_A_VALID, 1000000u)) {
        gpio_xor_mask(1u << S_LED_PIN);
    }
    gpio_put(S_LED_PIN, 1);

    printf("  link synced, serving\n");
    linkf_serve(served, SERVED_BYTES / 4);

    __builtin_unreachable();
}
