/**
 * frank-386 — basic-block discovery and reuse profiling. See bbprofile.h.
 */
#include "bbprofile.h"

#if BB_PROFILE

#include <stdio.h>
#include <string.h>

bb_slot_t bb_tab[BB_SLOTS];
uint32_t bb_entries;
uint32_t bb_collisions;

/*
 * Published for SWD reading:
 *   [0] block entries      [1] collisions
 *   [2] live slots         [3] hits in the top 64 slots
 *   [4] total hits in table
 */
volatile uint32_t g_bb[8] __attribute__((used));

void bb_report(void) {
    uint32_t live = 0;
    uint64_t total = 0;
    /* Top-64 by hits, found with a running minimum rather than a sort —
     * this runs on the emulator's own core and must stay cheap. */
    uint32_t top[64]; memset(top, 0, sizeof(top));
    uint32_t topmin = 0, topmin_i = 0;

    for (uint32_t i = 0; i < BB_SLOTS; i++) {
        const uint32_t h = bb_tab[i].hits;
        if (!bb_tab[i].tag) continue;
        live++; total += h;
        if (h > topmin) {
            top[topmin_i] = h;
            topmin = top[0]; topmin_i = 0;
            for (uint32_t k = 1; k < 64; k++)
                if (top[k] < topmin) { topmin = top[k]; topmin_i = k; }
        }
    }
    uint64_t tophits = 0;
    for (uint32_t k = 0; k < 64; k++) tophits += top[k];

    g_bb[0] = bb_entries;
    g_bb[1] = bb_collisions;
    g_bb[2] = live;
    g_bb[3] = (uint32_t)tophits;
    g_bb[4] = (uint32_t)total;
}

#endif /* BB_PROFILE */
