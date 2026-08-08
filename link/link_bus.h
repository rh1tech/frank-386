/*
 * FRANK Core 2 — dual-RP2350 test firmware
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * link_bus.h — transport layer for the inter-processor link.
 *
 * One `link_t` owns both directions: a PIO transmit SM feeding the
 * outgoing 8-bit bus and a PIO receive SM draining the incoming one,
 * each with a dedicated DMA channel. Master and slave use identical
 * code and differ only in which pin bases they pass to link_init().
 *
 * Framing rules the two sides agree on:
 *
 *   - The receiver arms first (link_rx_arm restarts the SM, which
 *     resets the input shift counter). That is what keeps the 32-bit
 *     autopush words aligned with the sender's 32-bit autopull words.
 *   - The sender raises VALID, streams, then lowers VALID. VALID is a
 *     plain SIO output — it carries no per-byte timing, it just tells
 *     the peer "a frame is in flight on this bus".
 *   - Byte boundaries are inherent: one clock edge is exactly one byte.
 *     Word boundaries come from the arm-before-send ordering above.
 */
#ifndef LINK_BUS_H
#define LINK_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/pio.h"

typedef struct {
    PIO   pio;
    uint  sm_tx, sm_rx;
    uint  off_tx, off_rx;

    uint  tx_data_base, tx_clk, tx_valid;
    uint  rx_data_base, rx_clk, rx_valid;

    uint  db_out;        /* doorbell we drive   */
    uint  db_in;         /* doorbell we observe */
    uint  fs;            /* frame sync / phase strobe */
    bool  fs_is_output;  /* master drives FS, slave reads it */

    int   dma_tx, dma_rx;
    float bulk_clkdiv;      /* what the sweep is currently testing */
    float applied_clkdiv;   /* what the TX state machine is set to  */
} link_t;

/* Control frames always run at this divider regardless of what the sweep
 * is testing.
 *
 * Measured on the first assembled board: the wire is error-free at 1.50x
 * and starts corrupting at 1.25x. Running 128-byte control frames at the
 * rate under test means a marginal rate does not just produce a bad
 * throughput number, it corrupts the protocol that is trying to measure
 * it — a failed CRC desynchronises both sides and the run collapses into
 * "slave refused divider" and lost replies. Control traffic is a
 * rounding error in time (128 bytes is ~4 us even here), so it buys
 * robustness for nothing. */
#define LINK_CTRL_CLKDIV 2.0f

/* Claim two SMs on `pio`, load both programs, configure the pins and
 * grab two DMA channels. Starts at clkdiv 1.0 (maximum rate). */
void link_init(link_t *l, PIO pio,
               uint tx_data_base, uint rx_data_base,
               uint db_out, uint db_in, uint fs, bool fs_is_output);

/* Set the divider used for bulk transfers. Only the transmitter is
 * divided — the receiver is edge-driven and always runs flat out.
 * 1.0 = sys_clk/5 bytes per second, 2.0 = sys_clk/10, and so on. */
void link_set_bulk_clkdiv(link_t *l, float clkdiv);

/* Select which rate the next transmit uses. Cheap and idempotent: the
 * divider is only touched when it actually changes, and only ever
 * between transfers when the state machine is stalled on an empty FIFO. */
void link_use_ctrl_rate(link_t *l);
void link_use_bulk_rate(link_t *l);

/* Theoretical byte rate implied by the bulk divider and sys_clk. */
uint32_t link_byte_rate(const link_t *l);

/* ---- Transmit ---- */
void   link_tx_start(link_t *l, const void *buf, size_t bytes);
/* Stream `total_bytes` by cycling repeatedly over a 2^ring_bits sized,
 * equally aligned buffer. One uninterrupted DMA transfer, so the wire
 * never idles — this is what the throughput measurements use. */
void   link_tx_start_ring(link_t *l, const void *buf, uint32_t ring_bits,
                          size_t total_bytes);
bool   link_tx_busy(const link_t *l);
/* Blocks until the DMA, the TX FIFO and the output shift register have
 * all drained, then drops VALID. Returns false on timeout. */
bool   link_tx_finish(link_t *l, uint32_t timeout_us);

/* ---- Receive ---- */
void   link_rx_arm(link_t *l, void *buf, size_t bytes);
/* Receive `total_bytes` into a wrapping 2^ring_bits window. Later blocks
 * overwrite earlier ones — this is the throughput counterpart to
 * link_tx_start_ring(), not an integrity check. */
void   link_rx_arm_ring(link_t *l, void *buf, uint32_t ring_bits,
                        size_t total_bytes);
size_t link_rx_remaining(const link_t *l);
/* Returns the number of bytes still outstanding; 0 means complete. */
size_t link_rx_wait(link_t *l, uint32_t timeout_us);
void   link_rx_abort(link_t *l);

/* ---- Single-wire control signals ---- */
void link_db_set(link_t *l, bool level);
bool link_db_get(const link_t *l);
/* Wait for the peer's doorbell to reach `level`. False on timeout. */
bool link_db_wait(const link_t *l, bool level, uint32_t timeout_us);

void link_fs_set(link_t *l, bool level);   /* master only */
bool link_fs_get(const link_t *l);

/* True while the peer is actively driving our receive bus. */
bool link_rx_valid_asserted(const link_t *l);

#endif /* LINK_BUS_H */
