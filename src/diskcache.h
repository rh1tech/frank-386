/**
 * frank-386 — SD sector cache in the slave RP2350's PSRAM.
 *
 * SPDX-License-Identifier: MIT
 *
 * Under a real workload (Wolf3D loading) spi_write_read_blocking is 4.4%
 * of core 0 — the interpreter waits while the guest blocks on SPI. At an
 * idle DOS prompt the same measurement reads 0.0%, which is why this was
 * left unbuilt until there were numbers from something doing work.
 *
 * Granularity is one 512-byte sector, not a larger block. The first
 * attempt cached 4 KB blocks and required whole-block alignment, and it
 * never engaged once: FatFS issues single-sector reads at arbitrary LBAs,
 * so all 196 requests during boot fell through as "partial". Sector
 * granularity has no alignment requirement and catches the repetitive
 * FAT and directory reads, which is where the win is.
 *
 * Size is dictated by master SRAM, which is the scarce resource on this
 * board: 1024 tags x 4 B = 4 KB, caching 512 KB of the slave's 8 MB.
 * An 8 KB tag array took the image to 91.25% and it stopped booting —
 * pc_new() could no longer allocate. 90.5% boots.
 *
 * One sector over the link is a ~1 us round trip plus 128 words at
 * ~100 MB/s, call it 6 us, against roughly 200-400 us for the same
 * sector over SPI.
 *
 * Writes are write-through and invalidate. An SD card can be pulled and
 * the settings UI can reset the machine, so nothing may live only here.
 *
 * STATUS: EXPERIMENTAL, off by default, and NOT yet correct.
 *
 * It boots and caches — 132 fills observed, burst transfers working —
 * and then the link desynchronises. The bug is in this file's error
 * handling, not in the link: when lf_put() times out part-way through a
 * write burst the master abandons the transfer while the slave is still
 * waiting for the remaining words, so every subsequent word arrives
 * shifted. The slave was seen parsing payload as a request header
 * (last_req = 0x5700bf56, opcode 5 with a 117M-word address).
 *
 * Two things must change before this is usable:
 *
 *  1. A burst must never be abandoned half-sent. Either reserve FIFO
 *     space up front, or force a linkf_sync() resynchronisation before
 *     the link is used again after any aborted transfer.
 *  2. The master must pace writes to what the slave's PSRAM can absorb.
 *     The wire delivers ~100 MB/s; PSRAM writes measure ~13 MB/s, so the
 *     master will always outrun the slave on a fill and the only
 *     question is whether that is handled or merely survived.
 */
#ifndef DISKCACHE_H
#define DISKCACHE_H

#include <stdbool.h>
#include <stdint.h>

#if DISK_CACHE

#define DC_SECTORS   1024u                 /* 512 KB cached */
#define DC_SEC_WORDS (512u / 4u)

void dc_init(void);
bool dc_enabled(void);

/* True only if every sector of the request was served from the cache. */
bool dc_read(uint32_t sector, uint32_t count, uint8_t *dst);
/* Cache the sectors after a successful read from the card. */
void dc_fill(uint32_t sector, uint32_t count, const uint8_t *src);
/* Drop the affected sectors after a write reaches the card. */
void dc_invalidate(uint32_t sector, uint32_t count);

#else

static inline void dc_init(void) {}
static inline bool dc_enabled(void) { return false; }
static inline bool dc_read(uint32_t s, uint32_t c, uint8_t *d) { (void)s;(void)c;(void)d; return false; }
static inline void dc_fill(uint32_t s, uint32_t c, const uint8_t *d) { (void)s;(void)c;(void)d; }
static inline void dc_invalidate(uint32_t s, uint32_t c) { (void)s;(void)c; }

#endif /* DISK_CACHE */

#endif /* DISKCACHE_H */
