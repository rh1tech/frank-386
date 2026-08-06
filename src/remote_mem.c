/**
 * frank-386 — guest memory served by the second RP2350.
 *
 * SPDX-License-Identifier: MIT
 *
 * See remote_mem.h.
 */
#include "remote_mem.h"

#if REMOTE_MEM

#include <stdio.h>

#include "board_config.h"
#include "link_fast.h"
#include "hardware/pio.h"

uint32_t remote_base;
uint32_t remote_span;   /* 0 until init succeeds — window shut */

static bool     rm_ready;
static uint32_t rm_rtt;

/* Guest byte address -> slave word index. The slave serves a flat array
 * of 32-bit words starting at remote_base. */
static inline uint32_t rm_word(uint32_t addr) {
    return (addr - remote_base) >> 2;
}

uint32_t remote_mem_init(uint32_t base, uint32_t bytes) {
    remote_base = base;
    remote_span = 0;
    rm_ready = false;

    /* Bus B reaches GPIO39, so the instance needs the upper GPIO window.
     * This fails with PICO_ERROR_INVALID_STATE if anything already
     * loaded a program here — which is how two earlier PIO clashes
     * presented as a dead slave. Do not ignore it. */
    if (pio_set_gpio_base(LINK_PIO_MASTER, LINK_PIO_GPIO_BASE) != PICO_OK) {
        printf("remote_mem: PIO %p already in use, window stays shut\n",
               (void *)LINK_PIO_MASTER);
        return 0;
    }

    if (!linkf_init(LINK_PIO_MASTER, M_LINK_A_DATA_BASE,
                    M_LINK_B_DATA_BASE, 1.0f)) {
        printf("remote_mem: PIO rejected the link pins\n");
        return 0;
    }

    /* The always-armed receivers collect stray bits from the floating
     * bus while the peer's PIO is coming up; without this the framing is
     * permanently offset and the link looks dead. */
    if (!linkf_sync(M_LINK_A_VALID, M_LINK_B_VALID, 3000000u)) {
        printf("remote_mem: slave never raised VALID\n");
        return 0;
    }

    if (!linkf_ping(2000000u)) {
        printf("remote_mem: slave did not answer a ping\n");
        return 0;
    }

    rm_rtt = linkf_measure_rtt(256u);
    remote_span = bytes;
    rm_ready = true;

    printf("remote_mem: %lu KB at 0x%08lx, %lu cycles round trip\n",
           (unsigned long)(bytes / 1024u), (unsigned long)base,
           (unsigned long)rm_rtt);
    return bytes;
}

bool remote_mem_ready(void) { return rm_ready; }
uint32_t remote_mem_rtt(void) { return rm_rtt; }

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/*                                                                     */
/* The slave serves whole 32-bit words, so sub-word access is a         */
/* read-modify-write. That makes a byte store two round trips where     */
/* PSRAM needs one access — the one case where remote is worse, and     */
/* the reason a byte-granular opcode is the obvious next optimisation   */
/* if the profile says stores dominate.                                 */
/* ------------------------------------------------------------------ */

uint32_t __not_in_flash_func(remote_read32)(uint32_t addr) {
    uint32_t v = 0;
    if (!(addr & 3u)) {
        linkf_read32(rm_word(addr), &v, 1000000u);
        return v;
    }
    /* Unaligned: straddles two words. */
    const uint32_t w = rm_word(addr);
    const uint32_t sh = (addr & 3u) * 8u;
    uint32_t lo = 0, hi = 0;
    linkf_read32(w, &lo, 1000000u);
    linkf_read32(w + 1, &hi, 1000000u);
    return (lo >> sh) | (hi << (32u - sh));
}

uint8_t __not_in_flash_func(remote_read8)(uint32_t addr) {
    uint32_t v = 0;
    linkf_read32(rm_word(addr), &v, 1000000u);
    return (uint8_t)(v >> ((addr & 3u) * 8u));
}

uint16_t __not_in_flash_func(remote_read16)(uint32_t addr) {
    if ((addr & 3u) <= 2u) {
        uint32_t v = 0;
        linkf_read32(rm_word(addr), &v, 1000000u);
        return (uint16_t)(v >> ((addr & 3u) * 8u));
    }
    return (uint16_t)remote_read32(addr);
}

void __not_in_flash_func(remote_write32)(uint32_t addr, uint32_t val) {
    if (!(addr & 3u)) {
        linkf_write32(rm_word(addr), val);   /* posted, no round trip */
        return;
    }
    const uint32_t w = rm_word(addr);
    const uint32_t sh = (addr & 3u) * 8u;
    uint32_t lo = 0, hi = 0;
    linkf_read32(w, &lo, 1000000u);
    linkf_read32(w + 1, &hi, 1000000u);
    const uint32_t lo_mask = (1u << sh) - 1u;
    linkf_write32(w,     (lo & lo_mask) | (val << sh));
    linkf_write32(w + 1, (hi & ~((1u << (32u - sh)) - 1u)) | (val >> (32u - sh)));
}

void __not_in_flash_func(remote_write8)(uint32_t addr, uint8_t val) {
    const uint32_t w = rm_word(addr);
    const uint32_t sh = (addr & 3u) * 8u;
    uint32_t v = 0;
    linkf_read32(w, &v, 1000000u);
    v = (v & ~(0xFFu << sh)) | ((uint32_t)val << sh);
    linkf_write32(w, v);
}

void __not_in_flash_func(remote_write16)(uint32_t addr, uint16_t val) {
    if ((addr & 3u) <= 2u) {
        const uint32_t w = rm_word(addr);
        const uint32_t sh = (addr & 3u) * 8u;
        uint32_t v = 0;
        linkf_read32(w, &v, 1000000u);
        v = (v & ~(0xFFFFu << sh)) | ((uint32_t)val << sh);
        linkf_write32(w, v);
        return;
    }
    remote_write8(addr, (uint8_t)val);
    remote_write8(addr + 1, (uint8_t)(val >> 8));
}

/* ------------------------------------------------------------------ */

uint32_t remote_mem_selftest(void) {
    if (!rm_ready) return 0xFFFFFFFFu;

    const uint32_t words = remote_span / 4u;
    uint32_t bad = 0;

    /* Write through the same accessors the interpreter uses, so the
     * test exercises the real path rather than a private one. */
    for (uint32_t i = 0; i < words; i++) {
        remote_write32(remote_base + i * 4u, i * 2654435761u);
    }
    for (uint32_t i = 0; i < words; i++) {
        if (remote_read32(remote_base + i * 4u) != i * 2654435761u) bad++;
    }

    /* Byte lanes: a swapped or dropped lane survives a word-only test. */
    for (uint32_t i = 0; i < 1024u && i < remote_span; i++) {
        remote_write8(remote_base + i, (uint8_t)(i * 7u + 3u));
    }
    for (uint32_t i = 0; i < 1024u && i < remote_span; i++) {
        if (remote_read8(remote_base + i) != (uint8_t)(i * 7u + 3u)) bad++;
    }

    printf("remote_mem: selftest %lu mismatches over %lu KB\n",
           (unsigned long)bad, (unsigned long)(remote_span / 1024u));
    return bad;
}

#endif /* REMOTE_MEM */
