/*
 * frank-386 — low-latency request/response path over the C2 link.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See link_fast.h for why this exists alongside link_session.c.
 *
 * Everything on the hot path is in RAM (`__not_in_flash_func`) and
 * touches the PIO FIFOs through their raw registers. The whole point is
 * the round-trip latency, and an XIP miss on the way to the FIFO write
 * would be a self-inflicted PSRAM stall inside the code trying to avoid
 * one.
 */
#include "link_fast.h"

#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "link_bus.pio.h"

static PIO  lf_pio;
static uint lf_sm_tx, lf_sm_rx;
static uint lf_off_tx, lf_off_rx;

/*
 * Bring the two receivers into word alignment.
 *
 * Both RX state machines are armed continuously — that is what makes a
 * request one FIFO write instead of a DMA setup. The cost is that they
 * are also armed while the *other* chip's PIO has not yet taken its pins,
 * so the bus floats and stray edges clock random bits into the input
 * shift register. Autopush then fires on a 32-bit boundary that is
 * offset from the sender's, and every later word is shifted garbage. The
 * link looks dead rather than misaligned, which is a poor way to spend
 * an afternoon.
 *
 * link_session avoids this by restarting the SM before each transfer.
 * Here the fix is a one-time handshake on the VALID lines, which are
 * plain SIO and carry no PIO timing: each side raises its own VALID once
 * its PIO owns the pins, waits for the peer's, and only then restarts
 * its receiver. After that both transmitters park their clocks low until
 * a word is pushed, so nothing can drift again.
 */
bool __not_in_flash_func(linkf_sync)(uint valid_out, uint valid_in,
                                     uint32_t timeout_us) {
    gpio_init(valid_out);
    gpio_set_dir(valid_out, GPIO_OUT);
    gpio_put(valid_out, 1);

    gpio_init(valid_in);
    gpio_set_dir(valid_in, GPIO_IN);
    gpio_pull_down(valid_in);

    const absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (!gpio_get(valid_in)) {
        if (time_reached(deadline)) return false;
    }

    /* Peer's PIO is up and its clock is parked low. Reset our receiver's
     * shift counter and drop anything it collected from the floating
     * bus. */
    pio_sm_set_enabled(lf_pio, lf_sm_rx, false);
    pio_sm_clear_fifos(lf_pio, lf_sm_rx);
    pio_sm_restart(lf_pio, lf_sm_rx);
    pio_sm_exec(lf_pio, lf_sm_rx, pio_encode_jmp(lf_off_rx));
    pio_sm_set_enabled(lf_pio, lf_sm_rx, true);

    /* And make sure our transmitter starts from the top of its program
     * with an empty FIFO, so the first word out is a whole word. */
    pio_sm_clear_fifos(lf_pio, lf_sm_tx);
    return true;
}

bool linkf_init(PIO pio, uint tx_data_base, uint rx_data_base, float clkdiv) {
    lf_pio = pio;

    lf_off_tx = pio_add_program(pio, &link_tx_program);
    lf_off_rx = pio_add_program(pio, &link_rx_program);

    lf_sm_tx = pio_claim_unused_sm(pio, true);
    lf_sm_rx = pio_claim_unused_sm(pio, true);

    /* These return PICO_ERROR_BAD_ALIGNMENT when the pins fall outside
     * the instance's 32-GPIO window — the failure mode when the link is
     * pointed at a PIO the video path already owns. Silently ignoring it
     * produces a link that never answers, which reads as a dead slave. */
    if (link_tx_program_init(pio, lf_sm_tx, lf_off_tx, tx_data_base, clkdiv) != PICO_OK)
        return false;
    if (link_rx_program_init(pio, lf_sm_rx, lf_off_rx, rx_data_base, clkdiv) != PICO_OK)
        return false;

    /* Arm the receiver before the transmitter. The RX shift counter is
     * reset by the SM restart, and that is what keeps its 32-bit
     * autopush words aligned with the sender's 32-bit autopull words. */
    pio_sm_set_enabled(pio, lf_sm_rx, true);
    pio_sm_set_enabled(pio, lf_sm_tx, true);
    return true;
}

/* ------------------------------------------------------------------ */
/* Master side                                                         */
/* ------------------------------------------------------------------ */

static inline void __not_in_flash_func(lf_put)(uint32_t w) {
    while (pio_sm_is_tx_fifo_full(lf_pio, lf_sm_tx)) tight_loop_contents();
    lf_pio->txf[lf_sm_tx] = w;
}

bool __not_in_flash_func(linkf_read32)(uint32_t word_addr, uint32_t *out,
                                       uint32_t timeout_loops) {
    lf_put(linkf_req(LINKF_OP_READ32, word_addr));

    while (pio_sm_is_rx_fifo_empty(lf_pio, lf_sm_rx)) {
        if (--timeout_loops == 0) return false;
    }
    *out = lf_pio->rxf[lf_sm_rx];
    return true;
}

void __not_in_flash_func(linkf_write32)(uint32_t word_addr, uint32_t value) {
    lf_put(linkf_req(LINKF_OP_WRITE32, word_addr));
    lf_put(value);
}

bool __not_in_flash_func(linkf_ping)(uint32_t timeout_loops) {
    uint32_t v = 0;
    lf_put(linkf_req(LINKF_OP_PING, 0));

    while (pio_sm_is_rx_fifo_empty(lf_pio, lf_sm_rx)) {
        if (--timeout_loops == 0) return false;
    }
    v = lf_pio->rxf[lf_sm_rx];
    return v == LINKF_PING_MAGIC;
}

/* DWT cycle counter — the same one profile_subsys.h uses. Declared
 * locally so this file does not depend on the profiler being built. */
#define LF_DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define LF_DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define LF_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)

uint32_t __not_in_flash_func(linkf_measure_rtt)(uint32_t iters) {
    LF_DEMCR |= (1u << 24);
    LF_DWT_CTRL |= 1u;

    /* One warm-up exchange: the first request after a quiet period pays
     * for the transmitter spinning its clock back up, which is not what
     * a steady-state access costs. */
    if (!linkf_ping(1000000u)) return 0;

    const uint32_t t0 = LF_DWT_CYCCNT;
    for (uint32_t i = 0; i < iters; i++) {
        if (!linkf_ping(1000000u)) return 0;
    }
    const uint32_t dt = LF_DWT_CYCCNT - t0;
    return iters ? dt / iters : 0;
}

/* ------------------------------------------------------------------ */
/* Slave side                                                          */
/* ------------------------------------------------------------------ */

/* Slave-side diagnostics, read over SWD:
 *   [0] requests received   [1] last request word
 *   [2] replies sent        [3] serve-loop entered */
volatile uint32_t g_slave_diag[4] __attribute__((used));

void __not_in_flash_func(linkf_serve)(uint32_t *base, uint32_t words) {
    PIO  pio   = lf_pio;
    uint sm_rx = lf_sm_rx, sm_tx = lf_sm_tx;

    g_slave_diag[3] = 1;

    for (;;) {
        while (pio_sm_is_rx_fifo_empty(pio, sm_rx)) tight_loop_contents();
        const uint32_t req  = pio->rxf[sm_rx];
        g_slave_diag[0]++;
        g_slave_diag[1] = req;
        const uint32_t op   = req >> LINKF_OP_SHIFT;
        const uint32_t addr = req & LINKF_ADDR_MASK;

        switch (op) {
        case LINKF_OP_READ32:
            /* Out-of-range reads answer 0 rather than wedging: a bad
             * address must not cost the master a timeout, because the
             * master is spinning on this reply with the guest stopped. */
            pio->txf[sm_tx] = (addr < words) ? base[addr] : 0u;
            break;

        case LINKF_OP_WRITE32: {
            while (pio_sm_is_rx_fifo_empty(pio, sm_rx)) tight_loop_contents();
            const uint32_t val = pio->rxf[sm_rx];
            if (addr < words) base[addr] = val;
            break;  /* posted — no reply */
        }

        case LINKF_OP_PING:
            pio->txf[sm_tx] = LINKF_PING_MAGIC;
            g_slave_diag[2]++;
            break;

        default:
            /* Unknown opcode. Answering anyway keeps the strict
             * one-request-one-reply invariant that holds the two sides
             * in word alignment; staying silent would desynchronise
             * every later exchange. */
            pio->txf[sm_tx] = 0u;
            break;
        }
    }
}
