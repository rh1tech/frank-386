/**
 * frank-386 — coarse subsystem cycle accounting for core 0.
 *
 * See profile_subsys.h. Compiled only when SUBSYS_PROFILE is set.
 */
#include "profile_subsys.h"

#if SUBSYS_PROFILE

#include <stdio.h>
#include <string.h>
#include "hardware/clocks.h"

prof_subsys_t g_prof;

/* Percentage of `total`, in tenths, without dragging in floating point
 * or overflowing: totals reach ~1e9 cycles between reports. */
static uint32_t permille(uint64_t part, uint64_t total) {
    if (!total) return 0;
    return (uint32_t)((part * 1000u) / total);
}

/*
 * One-shot memory latency benchmark, printed at startup.
 *
 * Sets the bar for any scheme that serves part of the guest address
 * space from the second RP2350: extending memory over the link is only
 * sane if a link round trip is comparable to a local PSRAM miss. That
 * comparison needs a real number for the local side, not an estimate
 * from the QSPI datasheet.
 *
 * The random walk strides by a large odd multiple of the 8 KiB XIP
 * cache so essentially every access misses; the sequential pass is the
 * cached/prefetched best case for contrast. `acc` is returned so the
 * compiler cannot delete the loads.
 */
#define BENCH_ITERS 4096u

static uint32_t bench_random(volatile uint8_t *base, uint32_t span, uint32_t *out_acc) {
    uint32_t acc = 0, idx = 1;
    const uint32_t t0 = prof_now();
    for (uint32_t i = 0; i < BENCH_ITERS; i++) {
        /* LCG, then spread across the whole span to defeat the cache. */
        idx = idx * 1664525u + 1013904223u;
        acc += base[idx % span];
    }
    const uint32_t dt = prof_now() - t0;
    *out_acc = acc;
    return dt;
}

static uint32_t bench_sequential(volatile uint8_t *base, uint32_t span, uint32_t *out_acc) {
    uint32_t acc = 0;
    const uint32_t t0 = prof_now();
    for (uint32_t i = 0; i < BENCH_ITERS; i++) {
        acc += base[(i * 64u) % span];
    }
    const uint32_t dt = prof_now() - t0;
    *out_acc = acc;
    return dt;
}

/* Results also land here so they can be read over SWD with mdw. The
 * console has a gap around the clock reconfiguration on this board, and
 * a number this decision rests on should not depend on a UART. */
volatile uint32_t g_bench[8] __attribute__((used));

void prof_mem_bench(void) {
    static uint8_t sram_buf[16384] __attribute__((aligned(4)));
    uint32_t acc = 0;
    const uint32_t mhz = clock_get_hz(clk_sys) / 1000000u;

    /* 8 MB of PSRAM through the cached XIP window — the same path the
     * interpreter's pload8() takes. */
    volatile uint8_t *psram = (volatile uint8_t *)0x11000000u;
    const uint32_t psram_rnd = bench_random(psram, 8u * 1024u * 1024u, &acc);
    const uint32_t psram_seq = bench_sequential(psram, 8u * 1024u * 1024u, &acc);
    const uint32_t sram_rnd  = bench_random(sram_buf, sizeof(sram_buf), &acc);

    printf("\n--- memory latency (%lu MHz, %u iters, acc=%lu) ---\n",
           (unsigned long)mhz, (unsigned)BENCH_ITERS, (unsigned long)acc);
    printf("  PSRAM random  %4lu cyc/access  (~%lu ns)\n",
           (unsigned long)(psram_rnd / BENCH_ITERS),
           (unsigned long)((psram_rnd / BENCH_ITERS) * 1000u / (mhz ? mhz : 1)));
    printf("  PSRAM seq/64B %4lu cyc/access  (~%lu ns)\n",
           (unsigned long)(psram_seq / BENCH_ITERS),
           (unsigned long)((psram_seq / BENCH_ITERS) * 1000u / (mhz ? mhz : 1)));
    printf("  SRAM  random  %4lu cyc/access\n",
           (unsigned long)(sram_rnd / BENCH_ITERS));

    g_bench[0] = psram_rnd / BENCH_ITERS;
    g_bench[1] = psram_seq / BENCH_ITERS;
    g_bench[2] = sram_rnd  / BENCH_ITERS;
    g_bench[3] = mhz;
}

#ifdef BOARD_C2
#include "link_fast.h"
#include "board_config.h"   /* M_LINK_*, LINK_PIO_MASTER, LINK_PIO_GPIO_BASE */
#include "hardware/pio.h"

/*
 * Bring the link up and measure a round trip against the slave.
 *
 * This is the number the remote-memory tier stands or falls on: the
 * master's own PSRAM is 184 cycles per random access, so a round trip
 * meaningfully below that makes the slave's idle SRAM a faster tier than
 * local guest RAM. Results also land in g_bench[] for reading over SWD.
 */
void prof_link_bench(void) {
    /* Bus B reaches GPIO39, so this PIO instance needs the upper GPIO
     * window. Per-instance, which is why the link owns PIO0 alone. */
    const int gb_rc = pio_set_gpio_base(LINK_PIO_MASTER, LINK_PIO_GPIO_BASE);
    g_bench[6] = (uint32_t)gb_rc;
    g_bench[7] = LINK_PIO_MASTER->gpiobase;

    /* Master: TX on bus A (master -> slave), RX on bus B. */
    if (!linkf_init(LINK_PIO_MASTER, M_LINK_A_DATA_BASE, M_LINK_B_DATA_BASE, 1.0f)) {
        printf("\n--- link: PIO init REJECTED (instance/window clash) ---\n");
        g_bench[4] = 0xFFFFFFFEu;
        return;
    }

    if (!linkf_sync(M_LINK_A_VALID, M_LINK_B_VALID, 3000000u)) {
        printf("\n--- link: slave never raised VALID ---\n");
        g_bench[4] = 0xFFFFFFFDu;
        return;
    }

    if (!linkf_ping(2000000u)) {
        printf("\n--- link: NO RESPONSE from slave ---\n");
        g_bench[4] = 0xFFFFFFFFu;
        /* Snapshot the PIO so the failure can be told apart from a dead
         * slave: a stalled transmitter means our own side never got the
         * bytes out, an empty receiver means they never came back. */
        return;
    }

    const uint32_t rtt = linkf_measure_rtt(1024u);
    const uint32_t mhz = clock_get_hz(clk_sys) / 1000000u;

    printf("\n--- link round trip: %lu cycles (~%lu ns) at %lu MHz ---\n",
           (unsigned long)rtt,
           (unsigned long)(rtt * 1000u / (mhz ? mhz : 1)),
           (unsigned long)mhz);

    /* Read a few served words back. The slave fills its block with
     * i ^ 0x5A5A0000, so a dropped byte or a swapped lane is obvious
     * rather than merely plausible. */
    uint32_t bad = 0, v = 0;
    for (uint32_t i = 0; i < 4096u; i++) {
        if (!linkf_read32(i, &v, 1000000u) || v != (i ^ 0x5A5A0000u)) bad++;
    }
    printf("  data check: %lu/4096 words wrong\n", (unsigned long)bad);

    g_bench[4] = rtt;
    g_bench[5] = bad;
}
#endif /* BOARD_C2 */

void prof_report(void) {
    const uint64_t total = g_prof.total;
    if (!total) { memset(&g_prof, 0, sizeof(g_prof)); return; }

    /* `cpu` includes the disk time nested inside it, and `total`
     * includes everything. "other" is whatever pc_step() spends outside
     * the buckets below — loop overhead and anything not instrumented. */
    const uint64_t buckets = g_prof.cpu + g_prof.adlib + g_prof.devices +
                             g_prof.poll + g_prof.refresh;
    const uint64_t other = total > buckets ? total - buckets : 0;

    const uint32_t mhz = clock_get_hz(clk_sys) / 1000000u;
    const uint32_t ms  = mhz ? (uint32_t)(total / (mhz * 1000u)) : 0;

    /* Throughput. Each pc_step() runs a fixed instruction budget (4096,
     * or 409 x 10 with AdLib on), so steps x budget / time is a directly
     * comparable figure across builds even though the budget is not an
     * exact count. This is the number an A/B of any interpreter change
     * has to move. */
    const uint32_t budget = 4096u;
    const uint32_t kips = ms ? (uint32_t)(((uint64_t)g_prof.steps * budget) / ms) : 0;
    g_bench[7] = kips;

    printf("\n--- core0 profile: %lu steps, %lu ms wall, %lu MHz, %lu.%03lu MIPS ---\n",
           (unsigned long)g_prof.steps, (unsigned long)ms, (unsigned long)mhz,
           (unsigned long)(kips / 1000u), (unsigned long)(kips % 1000u));
    printf("  cpu      %4lu.%lu%%   (i386 interpreter)\n",
           (unsigned long)(permille(g_prof.cpu, total) / 10),
           (unsigned long)(permille(g_prof.cpu, total) % 10));
    printf("    of which disk %3lu.%lu%%  (%lu SD ops)\n",
           (unsigned long)(permille(g_prof.disk, total) / 10),
           (unsigned long)(permille(g_prof.disk, total) % 10),
           (unsigned long)g_prof.disk_ops);
    printf("  adlib    %4lu.%lu%%   (OPL2 on core 0)\n",
           (unsigned long)(permille(g_prof.adlib, total) / 10),
           (unsigned long)(permille(g_prof.adlib, total) % 10));
    printf("  devices  %4lu.%lu%%   (PIT/CMOS/8042/DMA/FDC/vga_step)\n",
           (unsigned long)(permille(g_prof.devices, total) / 10),
           (unsigned long)(permille(g_prof.devices, total) % 10));
    printf("  poll     %4lu.%lu%%   (USB host + input)\n",
           (unsigned long)(permille(g_prof.poll, total) / 10),
           (unsigned long)(permille(g_prof.poll, total) % 10));
    printf("  refresh  %4lu.%lu%%   (vga_refresh)\n",
           (unsigned long)(permille(g_prof.refresh, total) / 10),
           (unsigned long)(permille(g_prof.refresh, total) % 10));
    printf("  other    %4lu.%lu%%\n",
           (unsigned long)(permille(other, total) / 10),
           (unsigned long)(permille(other, total) % 10));

    memset(&g_prof, 0, sizeof(g_prof));
}

#endif /* SUBSYS_PROFILE */
