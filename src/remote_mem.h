/**
 * frank-386 — guest memory served by the second RP2350.
 *
 * SPDX-License-Identifier: MIT
 *
 * Measured on FRANK Core 2 at 504 MHz (docs/C2_SPLIT_PLAN.md §6b):
 *
 *     master SRAM          7 cycles
 *     slave SRAM via link  89 cycles   <-- this
 *     master PSRAM        182 cycles
 *
 * So a window of guest memory served out of the slave's otherwise-idle
 * SRAM is *twice as fast* as the same window in the master's own PSRAM.
 * Writes are better still: they are posted, with no round trip at all.
 *
 * WHERE THE CHECK LIVES
 *
 * In pload/pstore, not in load/store. That is deliberate. load8()
 * already range-checks every access, so hooking there would have been
 * free — but two paths reach memory without going through it:
 *
 *   - page-table walks (i386.c: pload32 of the PDE and PTE)
 *   - instruction prefetch (i386.c: four pload32 calls)
 *
 * A window that silently mishandled page tables or code fetches would
 * fail in ways that look nothing like a memory bug. Paying one
 * predicted-not-taken compare in pload8() is the cheaper mistake.
 *
 * On every board except C2 REMOTE_MEM is undefined and all of this
 * compiles to nothing.
 */
#ifndef REMOTE_MEM_H
#define REMOTE_MEM_H

#include <stdbool.h>
#include <stdint.h>

/* How much of the slave's SRAM to claim. Must not exceed what
 * slave/src/main.c serves (SERVED_BYTES). The slave has 520 KB total and
 * a tiny firmware, so this can grow once the sound subsystem's eventual
 * footprint there is known. */
#ifndef REMOTE_MEM_BYTES
#define REMOTE_MEM_BYTES (256u * 1024u)
#endif

#if REMOTE_MEM

/* Byte range of the remote window. remote_span == 0 disables it, and
 * the unsigned-subtract test below then fails for every address —
 * the same trick in_iomem() uses to fold two comparisons into one. */
extern uint32_t remote_base;
extern uint32_t remote_span;

static inline bool __attribute__((always_inline)) is_remote(uint32_t addr) {
    return (addr - remote_base) < remote_span;
}

/* Bring up the link and claim `bytes` of the slave's SRAM starting at
 * guest physical `base`. Returns the number of bytes actually available
 * — 0 if the slave did not answer, in which case the window stays shut
 * and every access carries on hitting PSRAM. */
uint32_t remote_mem_init(uint32_t base, uint32_t bytes);

/* True once init has succeeded. */
bool remote_mem_ready(void);

/* Round-trip latency in core cycles, for the profile report. */
uint32_t remote_mem_rtt(void);

/* Read back and verify the whole window through these same accessors.
 * Returns the number of mismatching words; 0 is a pass. */
uint32_t remote_mem_selftest(void);

uint8_t  remote_read8(uint32_t addr);
uint16_t remote_read16(uint32_t addr);
uint32_t remote_read32(uint32_t addr);
void     remote_write8(uint32_t addr, uint8_t v);
void     remote_write16(uint32_t addr, uint16_t v);
void     remote_write32(uint32_t addr, uint32_t v);

#else /* !REMOTE_MEM */

static inline bool is_remote(uint32_t addr) { (void)addr; return false; }
static inline uint32_t remote_mem_init(uint32_t base, uint32_t bytes) {
    (void)base; (void)bytes; return 0;
}
static inline bool remote_mem_ready(void) { return false; }
static inline uint32_t remote_mem_rtt(void) { return 0; }
static inline uint32_t remote_mem_selftest(void) { return 0; }

#endif /* REMOTE_MEM */

#endif /* REMOTE_MEM_H */
