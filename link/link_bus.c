/*
 * FRANK Core 2 — dual-RP2350 test firmware
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "link_bus.h"
#include "link_bus.pio.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#include <string.h>

/* Once the TX FIFO reads empty, at most one 32-bit word is left in the
 * output shift register. Waiting for that to clock out is what stops
 * VALID from dropping mid-byte, but the wait has to be derived from the
 * current link rate rather than fixed: a constant large enough for the
 * slowest divider would dwarf the actual transfer on a 128-byte control
 * frame and turn the round-trip figure into a measurement of our own
 * padding. Four bytes plus a generous margin. */
static inline uint32_t tx_drain_us(const link_t *l) {
    float cycles = 4.0f * (float)LINK_PIO_CYCLES_PER_BYTE * l->applied_clkdiv;
    float us = (cycles * 1000000.0f) / (float)clock_get_hz(clk_sys);
    return (uint32_t)us + 2u;
}

void link_init(link_t *l, PIO pio,
               uint tx_data_base, uint rx_data_base,
               uint db_out, uint db_in, uint fs, bool fs_is_output) {
    memset(l, 0, sizeof(*l));

    l->pio = pio;
    l->tx_data_base = tx_data_base;
    l->tx_clk       = tx_data_base + 8;
    l->tx_valid     = tx_data_base + 9;
    l->rx_data_base = rx_data_base;
    l->rx_clk       = rx_data_base + 8;
    l->rx_valid     = rx_data_base + 9;
    l->db_out       = db_out;
    l->db_in        = db_in;
    l->fs           = fs;
    l->fs_is_output = fs_is_output;
    l->bulk_clkdiv    = 1.0f;
    l->applied_clkdiv = LINK_CTRL_CLKDIV;

#if PICO_PIO_USE_GPIO_BASE
    /* An RP2350B PIO instance can only see 32 consecutive GPIOs, chosen
     * by GPIOBASE as either 0..31 or 16..47. The master's two buses span
     * GPIO20 (bus A data) to GPIO38 (bus B clock), which only fits in
     * the upper window — without this the SDK would reject the config
     * (or, with assertions off, quietly alias GPIO32..38 onto 0..6).
     * The slave's pins are all below 24 and the macro compiles out
     * there, so this is master-only in practice. */
    uint gpio_hi = (tx_data_base > rx_data_base ? tx_data_base : rx_data_base) + 8;
    if (gpio_hi > 31) pio_set_gpio_base(pio, 16);
#endif

    l->sm_tx  = pio_claim_unused_sm(pio, true);
    l->sm_rx  = pio_claim_unused_sm(pio, true);
    l->off_tx = pio_add_program(pio, &link_tx_program);
    l->off_rx = pio_add_program(pio, &link_rx_program);

    /* Both of these fail rather than misbehave if the pins fall outside
     * the PIO's GPIO window, which is the one configuration mistake that
     * would otherwise produce a link that looks wired but reads garbage
     * off whichever low GPIOs the high pins aliased onto. */
    hard_assert(link_tx_program_init(pio, l->sm_tx, l->off_tx,
                                     tx_data_base, l->applied_clkdiv) == PICO_OK);
    /* The receiver is edge-driven: it must always run as fast as it can,
     * because its three-instruction loop has to complete inside the
     * peer's byte period. Dividing it would eat the very margin the
     * sweep is trying to measure. */
    hard_assert(link_rx_program_init(pio, l->sm_rx, l->off_rx,
                                     rx_data_base, 1.0f) == PICO_OK);

    /* VALID we drive, VALID the peer drives. Both are plain SIO. */
    gpio_init(l->tx_valid);
    gpio_set_dir(l->tx_valid, GPIO_OUT);
    gpio_put(l->tx_valid, 0);

    gpio_init(l->rx_valid);
    gpio_set_dir(l->rx_valid, GPIO_IN);
    gpio_pull_down(l->rx_valid);

    /* Doorbells. Pull the input down so a powered-off / unprogrammed
     * peer reads as "no doorbell" rather than floating. */
    gpio_init(db_out);
    gpio_set_dir(db_out, GPIO_OUT);
    gpio_put(db_out, 0);

    gpio_init(db_in);
    gpio_set_dir(db_in, GPIO_IN);
    gpio_pull_down(db_in);

    gpio_init(fs);
    if (fs_is_output) {
        gpio_set_dir(fs, GPIO_OUT);
        gpio_put(fs, 0);
    } else {
        gpio_set_dir(fs, GPIO_IN);
        gpio_pull_down(fs);
    }

    l->dma_tx = dma_claim_unused_channel(true);
    l->dma_rx = dma_claim_unused_channel(true);

    /* The transmitter idles enabled: with an empty FIFO it stalls inside
     * `out pins, 8` holding the clock low, so nothing appears on the wire
     * until the DMA feeds it. The receiver stays disabled until armed. */
    pio_sm_set_enabled(pio, l->sm_tx, true);
    pio_sm_set_enabled(pio, l->sm_rx, false);
}

void link_set_bulk_clkdiv(link_t *l, float clkdiv) {
    if (clkdiv < 1.0f) clkdiv = 1.0f;
    l->bulk_clkdiv = clkdiv;
}

/* Program the transmit divider. Only touched when it changes, and only
 * ever between transfers — the SM is then stalled inside `out` with an
 * empty FIFO, so a clkdiv restart cannot land mid-byte. */
static void apply_clkdiv(link_t *l, float clkdiv) {
    if (clkdiv == l->applied_clkdiv) return;
    l->applied_clkdiv = clkdiv;
    pio_sm_set_clkdiv(l->pio, l->sm_tx, clkdiv);
    pio_sm_clkdiv_restart(l->pio, l->sm_tx);
}

void link_use_ctrl_rate(link_t *l) { apply_clkdiv(l, LINK_CTRL_CLKDIV); }
void link_use_bulk_rate(link_t *l) { apply_clkdiv(l, l->bulk_clkdiv);   }

uint32_t link_byte_rate(const link_t *l) {
    float hz = (float)clock_get_hz(clk_sys) / l->bulk_clkdiv;
    return (uint32_t)(hz / (float)LINK_PIO_CYCLES_PER_BYTE);
}

/* ------------------------------------------------------------------ */
/* Transmit                                                           */
/* ------------------------------------------------------------------ */

void link_tx_start(link_t *l, const void *buf, size_t bytes) {
    /* The PIO FIFO is 32 bits wide and autopull is 32-bit, so frames
     * must be a whole number of words. Callers use word-sized frames. */
    size_t words = bytes / 4;

    gpio_put(l->tx_valid, 1);

    dma_channel_config c = dma_channel_get_default_config(l->dma_tx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(l->pio, l->sm_tx, true));

    dma_channel_configure(l->dma_tx, &c,
                          &l->pio->txf[l->sm_tx], buf, words, true);
}

void link_tx_start_ring(link_t *l, const void *buf, uint32_t ring_bits,
                        size_t total_bytes) {
    size_t words = total_bytes / 4;

    gpio_put(l->tx_valid, 1);

    dma_channel_config c = dma_channel_get_default_config(l->dma_tx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    /* Wrap the read address every 2^ring_bits bytes so one DMA transfer
     * can replay the same pattern block indefinitely with no gap. */
    channel_config_set_ring(&c, false /* wrap read address */, ring_bits);
    channel_config_set_dreq(&c, pio_get_dreq(l->pio, l->sm_tx, true));

    dma_channel_configure(l->dma_tx, &c,
                          &l->pio->txf[l->sm_tx], buf, words, true);
}

bool link_tx_busy(const link_t *l) {
    return dma_channel_is_busy(l->dma_tx);
}

bool link_tx_finish(link_t *l, uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);

    while (dma_channel_is_busy(l->dma_tx)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            dma_channel_abort(l->dma_tx);
            gpio_put(l->tx_valid, 0);
            return false;
        }
    }

    /* DMA done only means the FIFO is fed. Wait for the FIFO to empty,
     * then give the OSR its last few bytes on the wire before dropping
     * VALID — otherwise the peer sees the frame end early. */
    while (!pio_sm_is_tx_fifo_empty(l->pio, l->sm_tx)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            gpio_put(l->tx_valid, 0);
            return false;
        }
    }
    busy_wait_us(tx_drain_us(l));

    gpio_put(l->tx_valid, 0);
    return true;
}

/* ------------------------------------------------------------------ */
/* Receive                                                            */
/* ------------------------------------------------------------------ */

void link_rx_arm(link_t *l, void *buf, size_t bytes) {
    size_t words = bytes / 4;

    /* Only abort a channel that is actually running. dma_channel_abort()
     * spins on the abort bit and carries an RP2350 erratum workaround;
     * paying that on every arm, when the previous transfer has normally
     * already finished, is part of what the peer waits for. Measured on
     * the master: the wait for "peer armed" drops from 40 us to 36 us per
     * control send, and Ultimate Mortal Kombat 3 loses about a sixth of
     * its audio underruns. */
    if (dma_channel_is_busy(l->dma_rx)) dma_channel_abort(l->dma_rx);

    /* Restarting the SM clears the ISR and, crucially, the input shift
     * counter — that is what re-aligns autopush words with the sender's
     * autopull words at the start of every frame. */
    pio_sm_set_enabled(l->pio, l->sm_rx, false);
    pio_sm_clear_fifos(l->pio, l->sm_rx);
    pio_sm_restart(l->pio, l->sm_rx);
    pio_sm_clkdiv_restart(l->pio, l->sm_rx);
    pio_sm_exec(l->pio, l->sm_rx, pio_encode_jmp(l->off_rx));

    dma_channel_config c = dma_channel_get_default_config(l->dma_rx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(l->pio, l->sm_rx, false));

    dma_channel_configure(l->dma_rx, &c,
                          buf, &l->pio->rxf[l->sm_rx], words, true);

    pio_sm_set_enabled(l->pio, l->sm_rx, true);
}

void link_rx_arm_ring(link_t *l, void *buf, uint32_t ring_bits,
                      size_t total_bytes) {
    size_t words = total_bytes / 4;

    dma_channel_abort(l->dma_rx);

    pio_sm_set_enabled(l->pio, l->sm_rx, false);
    pio_sm_clear_fifos(l->pio, l->sm_rx);
    pio_sm_restart(l->pio, l->sm_rx);
    pio_sm_clkdiv_restart(l->pio, l->sm_rx);
    pio_sm_exec(l->pio, l->sm_rx, pio_encode_jmp(l->off_rx));

    dma_channel_config c = dma_channel_get_default_config(l->dma_rx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true /* wrap write address */, ring_bits);
    channel_config_set_dreq(&c, pio_get_dreq(l->pio, l->sm_rx, false));

    dma_channel_configure(l->dma_rx, &c,
                          buf, &l->pio->rxf[l->sm_rx], words, true);

    pio_sm_set_enabled(l->pio, l->sm_rx, true);
}

size_t link_rx_remaining(const link_t *l) {
    return (size_t)dma_channel_hw_addr(l->dma_rx)->transfer_count * 4;
}

size_t link_rx_wait(link_t *l, uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);

    while (dma_channel_is_busy(l->dma_rx)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            /* Re-check before declaring failure: the transfer may have
             * completed between the busy test and the clock read. */
            if (!dma_channel_is_busy(l->dma_rx)) return 0;

            size_t left = link_rx_remaining(l);
            link_rx_abort(l);
            return left ? left : 4;   /* never report success on timeout */
        }
    }
    return 0;
}

void link_rx_abort(link_t *l) {
    dma_channel_abort(l->dma_rx);
    pio_sm_set_enabled(l->pio, l->sm_rx, false);
    pio_sm_clear_fifos(l->pio, l->sm_rx);
}

/* ------------------------------------------------------------------ */
/* Control signals                                                    */
/* ------------------------------------------------------------------ */

void link_db_set(link_t *l, bool level)  { gpio_put(l->db_out, level); }
bool link_db_get(const link_t *l)        { return gpio_get(l->db_in);  }

bool link_db_wait(const link_t *l, bool level, uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (gpio_get(l->db_in) != level) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return false;
    }
    return true;
}

void link_fs_set(link_t *l, bool level) {
    if (l->fs_is_output) gpio_put(l->fs, level);
}

bool link_fs_get(const link_t *l) { return gpio_get(l->fs); }

bool link_rx_valid_asserted(const link_t *l) { return gpio_get(l->rx_valid); }
