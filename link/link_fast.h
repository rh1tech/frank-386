/*
 * frank-386 — low-latency request/response path over the C2 link.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Distinct from link_session.c, which is built for throughput: doorbell
 * handshake, DMA, 128-byte control frames, ~3 us per round trip. That is
 * the right shape for shipping a frame of sound events and quite the
 * wrong shape for fetching four bytes of guest memory.
 *
 * This path is the opposite trade. Both state machines stay armed
 * forever; a request is one 32-bit FIFO write and a reply is one 32-bit
 * FIFO read, with no DMA, no doorbell and no framing beyond the PIO's
 * own 32-bit autopull/autopush threshold. Strict request/response is
 * what keeps the two sides word-aligned — every request produces exactly
 * one reply, so neither side can drift.
 *
 * Why it might be worth it (docs/C2_SPLIT_PLAN.md §6b): the master's own
 * PSRAM costs a measured 184 core cycles per random access, while the
 * budget for a round trip here is ~70-100. If that holds, the slave's
 * idle ~400 KB of SRAM is a *faster* tier than the master's own guest
 * RAM, reached through a branch (`addr >= phys_mem_size`) the
 * interpreter already pays for.
 *
 * Both halves must be built at the same CPU_SPEED. The receiving PIO
 * program has to complete its loop inside the transmitter's byte period
 * and each side derives that from its own clk_sys.
 */
#ifndef LINK_FAST_H
#define LINK_FAST_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"

/* ---- Wire format ----
 *
 * One 32-bit word each way. The request packs a 4-bit opcode into the
 * top nibble and a 28-bit word-address below it, which covers 256 MB of
 * remote space — far more than the slave will ever have.
 *
 * The reply is a bare 32-bit value: there is nothing to disambiguate
 * because a reply can only ever belong to the one outstanding request.
 */
#define LINKF_OP_SHIFT     28
#define LINKF_ADDR_MASK    0x0FFFFFFFu

#define LINKF_OP_READ32    0x1u   /* arg = word address; replies with data */
#define LINKF_OP_WRITE32   0x2u   /* arg = word address; data word follows,
                                   * posted — no reply */
#define LINKF_OP_PING      0x3u   /* replies with LINKF_PING_MAGIC         */
/*
 * Burst transfers. arg = first word address; a count word follows the
 * request, then the payload streams with no per-word handshake.
 *
 * Single-word access costs a full round trip (89 cycles) for four bytes
 * — about 22 MB/s, which is enough to beat the SD card but wastes most
 * of the wire. A burst pays the round trip once: after that the PIO
 * streams at one byte per five system clocks, ~100 MB/s at 504 MHz, so
 * a 4 KB disk block moves in ~41 us instead of ~180 us.
 *
 * The master can always drain faster than the wire delivers, so the RX
 * FIFO cannot overflow mid-burst.
 */
#define LINKF_OP_READ_BURST  0x4u
#define LINKF_OP_WRITE_BURST 0x5u

#define LINKF_PING_MAGIC   0xA5A5F00Du

static inline uint32_t linkf_req(uint32_t op, uint32_t word_addr) {
    return (op << LINKF_OP_SHIFT) | (word_addr & LINKF_ADDR_MASK);
}

/* ---- Setup ----
 *
 * `tx_data_base` / `rx_data_base` are the first data GPIO of each bus;
 * CLK is always base+8. The master passes bus A for TX and bus B for RX,
 * the slave the other way round.
 *
 * On the master the PIO instance needs pio_set_gpio_base(16) because bus
 * B reaches GPIO39; the caller does that, since it is per-instance and
 * the choice of instance belongs to the board.
 */
/* Returns false if the PIO rejects the pin configuration — which is what
 * happens when this instance's GPIO window cannot reach the link pins,
 * usually because the video path already owns it. */
bool linkf_init(PIO pio, uint tx_data_base, uint rx_data_base, float clkdiv);

/* One-time alignment handshake on the VALID lines. Must be called on
 * both halves after linkf_init() and before any exchange: the always-armed
 * receivers collect stray bits from the floating bus while the peer's PIO
 * is still coming up, and autopush then lands on the wrong 32-bit
 * boundary. Returns false if the peer never raised its VALID. */
bool linkf_sync(uint valid_out, uint valid_in, uint32_t timeout_us);

/* ---- Master side ---- */

/* Blocking 32-bit read. Returns false on timeout, in which case *out is
 * untouched — a slave that is not running must not look like zeroed
 * memory, or a missing link reads as a machine full of NOPs. */
bool linkf_read32(uint32_t word_addr, uint32_t *out, uint32_t timeout_loops);

/* Posted 32-bit write. Returns as soon as the words are in the TX FIFO;
 * there is no acknowledgement. Writes do not need a round trip, which is
 * the one place this path beats PSRAM outright. */
void linkf_write32(uint32_t word_addr, uint32_t value);

/* Liveness probe. */
bool linkf_ping(uint32_t timeout_loops);

/* Bulk transfer of `words` 32-bit words. Both return false on timeout.
 * `words` must be >= 1. */
bool linkf_read_burst(uint32_t word_addr, uint32_t *dst, uint32_t words,
                      uint32_t timeout_loops);
bool linkf_write_burst(uint32_t word_addr, const uint32_t *src, uint32_t words);

/* True once linkf_init() has run and the peer has answered. */
bool linkf_is_up(void);
void linkf_set_up(bool up);

/* Average round-trip latency over `iters` pings, in core cycles.
 * Returns 0 if the slave did not answer. This is the number the whole
 * remote-memory idea rests on. */
uint32_t linkf_measure_rtt(uint32_t iters);

/* ---- Slave side ----
 *
 * Serves requests forever against `base` (which must be 4-byte aligned)
 * of `words` 32-bit words. Never returns.
 */
void linkf_serve(uint32_t *base, uint32_t words);

#endif /* LINK_FAST_H */
