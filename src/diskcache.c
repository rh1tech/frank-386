/**
 * frank-386 — SD block cache in the slave RP2350's PSRAM. See diskcache.h.
 */
#include "diskcache.h"

#if DISK_CACHE

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "link_fast.h"
#include "hardware/pio.h"

/* Tag per cached sector: the LBA held there, or DC_EMPTY. */
#define DC_EMPTY 0xFFFFFFFFu
static uint32_t dc_tag[DC_SECTORS];
static bool dc_on;

/* Counters, readable over SWD. */
volatile uint32_t g_dc[8] __attribute__((used));
enum { DC_HIT, DC_MISS, DC_FILL, DC_INVAL, DC_SECS, DC_FAIL };

bool dc_enabled(void) { return dc_on; }

void dc_init(void) {
    for (uint32_t i = 0; i < DC_SECTORS; i++) dc_tag[i] = DC_EMPTY;
    dc_on = false;

    if (pio_set_gpio_base(LINK_PIO_MASTER, LINK_PIO_GPIO_BASE) != PICO_OK) {
        printf("diskcache: PIO already in use, cache disabled\n");
        return;
    }
    if (!linkf_init(LINK_PIO_MASTER, M_LINK_A_DATA_BASE, M_LINK_B_DATA_BASE, 1.0f)) {
        printf("diskcache: PIO rejected the link pins\n");
        return;
    }
    /* The always-armed receivers pick up stray bits from the floating bus
     * while the peer's PIO comes up; without this the 32-bit framing is
     * permanently offset and the link looks dead rather than misaligned. */
    if (!linkf_sync(M_LINK_A_VALID, M_LINK_B_VALID, 3000000u)) {
        printf("diskcache: slave never raised VALID, cache disabled\n");
        return;
    }
    if (!linkf_ping(2000000u)) {
        printf("diskcache: slave did not answer, cache disabled\n");
        return;
    }

    linkf_set_up(true);
    dc_on = true;
    printf("diskcache: %lu sectors (%lu KB) in slave PSRAM\n",
           (unsigned long)DC_SECTORS, (unsigned long)(DC_SECTORS / 2u));
}

static inline uint32_t dc_slot(uint32_t lba)  { return lba % DC_SECTORS; }
static inline uint32_t dc_word(uint32_t slot) { return slot * DC_SEC_WORDS; }

bool __not_in_flash_func(dc_read)(uint32_t sector, uint32_t count, uint8_t *dst) {
    if (!dc_on || count == 0) return false;

    /* All or nothing: a partial copy would still leave the caller going
     * to the card for the rest, so the SPI cost is paid either way. */
    for (uint32_t i = 0; i < count; i++) {
        if (dc_tag[dc_slot(sector + i)] != sector + i) { g_dc[DC_MISS]++; return false; }
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!linkf_read_burst(dc_word(dc_slot(sector + i)),
                              (uint32_t *)(dst + i * 512u),
                              DC_SEC_WORDS, 2000000u)) {
            /* A timeout mid-burst leaves the link's word framing in an
             * unknown state. Stop trusting it rather than risk serving
             * shifted bytes as disk contents. */
            g_dc[DC_FAIL]++;
            dc_on = false;
            return false;
        }
    }
    g_dc[DC_HIT]++;
    g_dc[DC_SECS] += count;
    return true;
}

void __not_in_flash_func(dc_fill)(uint32_t sector, uint32_t count, const uint8_t *src) {
    if (!dc_on || count == 0) return;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t lba = sector + i;
        const uint32_t slot = dc_slot(lba);
        if (!linkf_write_burst(dc_word(slot), (const uint32_t *)(src + i * 512u),
                               DC_SEC_WORDS)) {
            g_dc[DC_FAIL]++;
            g_dc[6] = lba;
            g_dc[7] = (count << 16) | i;
            dc_on = false;
            return;
        }
        dc_tag[slot] = lba;
    }
    g_dc[DC_FILL]++;
}

void dc_invalidate(uint32_t sector, uint32_t count) {
    if (!dc_on) return;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t lba = sector + i;
        const uint32_t slot = dc_slot(lba);
        if (dc_tag[slot] == lba) dc_tag[slot] = DC_EMPTY;
    }
    g_dc[DC_INVAL]++;
}

#endif /* DISK_CACHE */
