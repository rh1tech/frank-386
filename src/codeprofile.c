/**
 * frank-386 — guest code footprint measurement. See codeprofile.h.
 */
#include "codeprofile.h"

#if CODE_PROFILE

#include <stdio.h>
#include <string.h>

uint32_t cp_seen_cum[CP_WORDS];
uint32_t cp_seen_win[CP_WORDS];
uint32_t cp_over_span;
uint32_t cp_vga_w, cp_vga_r, cp_io_w, cp_io_r;

/* High-water marks. The user drives the machine, so rather than trying to
 * synchronise with a workload, keep the largest window ever seen. */
static uint32_t cp_peak_blocks;

/*
 * Published for SWD reading:
 *   [0] cumulative distinct 64-byte code blocks
 *   [1] window distinct blocks (since last report)
 *   [2] cumulative guest code bytes
 *   [3] window guest code bytes
 *   [4] fetches above CP_SPAN
 *   [5] estimated JIT cache bytes for the window at 5x expansion
 *   [6] peak window blocks ever seen
 *   [7] peak window guest code bytes
 * and g_events[]:
 *   [0] VGA writes  [1] VGA reads  [2] port out  [3] port in   (per window)
 *   [4] window milliseconds
 */
volatile uint32_t g_code[8] __attribute__((used));
volatile uint32_t g_events[8] __attribute__((used));

static uint32_t popcount_words(const uint32_t *w, uint32_t n) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++) c += __builtin_popcount(w[i]);
    return c;
}

void cp_report(uint32_t window_ms) {
    const uint32_t cum = popcount_words(cp_seen_cum, CP_WORDS);
    const uint32_t win = popcount_words(cp_seen_win, CP_WORDS);
    if (win > cp_peak_blocks) cp_peak_blocks = win;

    const uint32_t cum_bytes = cum << CP_GRAN_SHIFT;
    const uint32_t win_bytes = win << CP_GRAN_SHIFT;

    g_code[0] = cum;
    g_code[1] = win;
    g_code[2] = cum_bytes;
    g_code[3] = win_bytes;
    g_code[4] = cp_over_span;
    /* x86 -> ARM expansion. 5x is a working assumption for a simple
     * template JIT with no register allocation; a good one does better,
     * a naive one much worse. Recorded as an explicit multiplier so the
     * assumption is visible rather than buried in a conclusion. */
    g_code[5] = win_bytes * 5u;
    g_code[6] = cp_peak_blocks;
    g_code[7] = cp_peak_blocks << CP_GRAN_SHIFT;

    g_events[0] = cp_vga_w; g_events[1] = cp_vga_r;
    g_events[2] = cp_io_w;  g_events[3] = cp_io_r;
    g_events[4] = window_ms;
    cp_vga_w = cp_vga_r = cp_io_w = cp_io_r = 0;

    printf("code footprint: cum %lu blocks (%lu KB), window %lu blocks (%lu KB), "
           ">4MB %lu -> JIT cache ~%lu KB at 5x\n",
           (unsigned long)cum, (unsigned long)(cum_bytes / 1024u),
           (unsigned long)win, (unsigned long)(win_bytes / 1024u),
           (unsigned long)cp_over_span,
           (unsigned long)((win_bytes * 5u) / 1024u));

    memset(cp_seen_win, 0, sizeof(cp_seen_win));
}

#endif /* CODE_PROFILE */
