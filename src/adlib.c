/*
 * QEMU Proxy for OPL2/3 emulation by MAME team
 *
 * Copyright (c) 2004-2005 Vassili Karpov (malc)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <pico.h>
#include <pico/time.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "adlib.h"
#include "audiodiag.h"
#include "emu8950/emu8950.h"
#if defined(RP2350_BUILD)
#include "board_config.h"
#endif

/* __dmb() is a CMSIS intrinsic; pico.h should pull it in transitively,
 * but include cmsis_compiler.h explicitly as a fallback. */
#if defined(__has_include) && __has_include("cmsis_compiler.h")
#  include "cmsis_compiler.h"
#elif !defined(__dmb)
#  define __dmb()  __asm volatile ("dmb" ::: "memory")
#endif

#define ADLIB_DESC "Yamaha YM3812 (OPL2)"

/*
 * Double-buffer:
 *   buf[2][ADLIB_BATCH_SIZE] — two batches.
 *   ready[2]  — Core 0 sets ready[i]=1 after filling buf[i],
 *               Core 1 sets ready[i]=0 after consuming buf[i].
 *   play_buf  — which buffer Core 1 is currently reading (0 or 1).
 *   read_pos  — sample index within buf[play_buf].
 *
 * Core 0 fills whichever buffer is NOT ready (i.e. already consumed).
 * Core 1 reads from play_buf; when exhausted, switches to the other one
 * if it's ready, otherwise returns silence.
 * Each ready[i] is written by one core at a time — no contention.
 */

struct AdlibState {
    uint32_t freq;
    uint16_t adlibregmem[5], adlib_register;
    uint8_t  adlibstatus;
    OPL     *opl;

    /* Single-producer/single-consumer sample ring. wpos and rpos are
     * free-running counters, masked on use, so wpos - rpos is the fill level
     * and no empty/full ambiguity arises. Core 0 only ever advances wpos and
     * core 1 only rpos, both with a single aligned 32-bit store, so no lock
     * is needed - only the barrier that orders the samples before the index. */
    int16_t  ring[ADLIB_RING_SAMPLES];
    volatile uint32_t wpos;     /* Core 0: samples rendered */
    volatile uint32_t rpos;     /* Core 1: samples played */
    int16_t  last;              /* Core 1: last sample, held across underruns */

    /* 49716 Hz -> 44100 Hz linear resampler, Core 0. s0/s1 bracket the output
     * position in the chip's own sample stream and frac is where between them
     * it sits, in Q16; b0/b1 are the same for the OPL3 second bank, which is
     * clocked identically and therefore shares frac. */
    int32_t  rs_s0, rs_s1;
    int32_t  rs_b0, rs_b1;
    uint32_t rs_frac;

    uint32_t underrun_count;
};

/*
 * Does the guest actually drive OPL3's second register bank?
 *
 * Aliasing the Sound Blaster's FM ports to this OPL2 turned silence into
 * music, but a game set to "Sound Blaster" reportedly sounds detuned next to
 * the same game set to "AdLib". The obvious explanation is that SB Pro/SB16
 * means OPL3 to the game - two banks, four-operator voices - while this is a
 * YM3812 with one bank, so anything written to 0x222/0x223 is lost and the
 * notes come out wrong.
 *
 * That is a guess until counted. If these stay at zero the game is using
 * plain OPL2 through the SB ports and the detuning has another cause; if they
 * climb, OPL3 is genuinely being driven and only real OPL3 emulation fixes it.
 */
volatile uint32_t g_opl3_bank1_writes __attribute__((section(".scratch_x.opl3diag"), used));
volatile uint32_t g_opl3_newbit_writes __attribute__((section(".scratch_x.opl3diag"), used));
static OPL *g_opl3_bank1 __attribute__((section(".scratch_x.opl3diag"), used));
/*
 * Microseconds core 0 spends generating OPL samples, accumulated.
 *
 * The guest is now known to drive OPL3's second bank (measured: 3242 writes in
 * 5 s, plus the 0x105 enable), so half the music is being discarded by this
 * one-bank YM3812. Covering it means a second OPL instance for bank 1, which
 * roughly doubles this cost - and that is only affordable if the current cost
 * is small. Reading this against wall time answers that before any of it is
 * written.
 */
volatile uint32_t g_adlib_us_total __attribute__((section(".scratch_x.opl3diag"), used));

/* adlibregmem[] stores byte-sized timer registers in uint16_t slots.  Keep
 * the OPL3 enable bit in the otherwise-unused high byte of slot zero so the
 * second bank costs no bytes in the already heap-sensitive AdlibState. */
#define ADLIB_OPL3_ENABLED 0x0100u
#define ADLIB_OPL3_STORAGE_BYTES 0x0800u

_Static_assert(sizeof(OPL) <= ADLIB_OPL3_STORAGE_BYTES,
               "OPL3 bank-1 state no longer fits its PSRAM shadow");

/* Linear interpolation between two chip samples. The 32x32 multiply is one
 * SMULL on this core; the alternative - shifting frac down to 8 bits to stay
 * in 32-bit arithmetic - puts audible quantisation on slow envelopes. */
static inline int32_t adlib_lerp(int32_t a, int32_t b, uint32_t frac)
{
    return a + (int32_t)((((int64_t)(b - a)) * (int32_t)frac) >> 16);
}

/* emu8950's per-call scratch buffers are sized SAMPLE_BUF_SIZE (64); the
 * renderer below never asks for more than ADLIB_RS_MAX in one call. */
_Static_assert(ADLIB_RS_MAX <= 64,
               "ADLIB_RS_MAX exceeds emu8950's SAMPLE_BUF_SIZE");

static inline int16_t adlib_sat16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void adlib_write(void *opaque, uint32_t nport, uint32_t val)
{
    AdlibState *s = opaque;

    /* Close the gap up to now before the new value takes effect, so this
     * write applies from the next sample on rather than retroactively to
     * everything core 1 has not played yet.  One sample of backlog is worth
     * rendering here - this is the whole point of the exercise. */
    adlib_produce(s, 1);
    /*
     * The OPL is reachable at three port pairs, not one:
     *
     *   0x388/0x389  the AdLib card
     *   0x228/0x229  the Sound Blaster's AdLib-compatible FM
     *   0x220/0x221  SB Pro / SB16 FM, primary bank
     *
     * pc.c already routes all of them here, but this switch only knew the
     * first pair, so everything a game wrote to the Sound Blaster's FM ports
     * was silently dropped. That is exactly the reported symptom: with the
     * game set to Sound Blaster the digital effects play - those go through
     * the DSP ports to sb16.c - while the music is silent, and setting the
     * same game to AdLib makes the music work.
     *
     * 0x222/0x223 are OPL3's second register bank and are deliberately not
     * aliased here: this is a YM3812 (OPL2) with one bank, and folding the
     * second bank onto the first would corrupt the first rather than add
     * anything.
     */
    switch (nport) {
        case 0x222:                       /* OPL3 bank 1 register select */
            g_opl3_bank1_writes++;
            if (val == 0x05) g_opl3_newbit_writes++;   /* 0x105 = OPL3 enable */
            s->adlib_register = (uint16_t)((s->adlib_register & 0x00ffu) |
                                           ((val & 0xffu) << 8));
            return;
        case 0x223:                       /* OPL3 bank 1 data */
            g_opl3_bank1_writes++;
            {
                const uint8_t reg = (uint8_t)(s->adlib_register >> 8);
                if (reg == 0x05) {        /* OPL3 NEW bit, register 0x105 */
                    if (val & 1u)
                        s->adlibregmem[0] |= ADLIB_OPL3_ENABLED;
                    else
                        s->adlibregmem[0] &= (uint16_t)~ADLIB_OPL3_ENABLED;
                } else if (reg != 0x04 && g_opl3_bank1) {
                    /* 0x104 selects four-operator pairs, which two OPL2
                     * instances cannot represent.  Every ordinary bank-1
                     * voice/operator register maps directly to the second
                     * nine-channel OPL2. */
                    OPL_writeReg(g_opl3_bank1, reg, val);
                }
            }
            return;
        case 0x388: case 0x228: case 0x220:
            frank_diag_opl_write(nport, (uint8_t)val, (uint8_t)val, 0);
            s->adlib_register = (uint16_t)((s->adlib_register & 0xff00u) |
                                           (val & 0xffu));
            break;
        case 0x389: case 0x229: case 0x221:
            {
                const uint8_t reg = (uint8_t)s->adlib_register;
                frank_diag_opl_write(nport, reg, (uint8_t)val, 1);
                if (reg <= 4) {
                    if (reg == 0)
                        s->adlibregmem[0] = (uint16_t)((s->adlibregmem[0] &
                                                       ADLIB_OPL3_ENABLED) |
                                                      (val & 0xffu));
                    else
                        s->adlibregmem[reg] = val;
                }
                if (reg == 4 && (val & 0x80)) {
                    s->adlibstatus = 0;
                    s->adlibregmem[4] = 0;
                }
                OPL_writeReg(s->opl, reg, val);
            }
    }
}

uint32_t adlib_read(void *opaque, uint32_t nport)
{
    AdlibState *s = opaque;
    switch (nport) {
        case 0x388: case 0x389:
        case 0x228: case 0x229:
        case 0x220: case 0x221:
            FRANK_DIAG_COUNT(opl_status);
            if (!s->adlibregmem[4])
                s->adlibstatus = 0;
            else
                s->adlibstatus = 0x80;
            s->adlibstatus = s->adlibstatus
                           + (s->adlibregmem[4] & 1) * 0x40
                           + (s->adlibregmem[4] & 2) * 0x10;
            return s->adlibstatus;
    }
    return 0xFF;
}

AdlibState *adlib_new()
{
    AdlibState *s = malloc(sizeof(AdlibState));
    if (!s) return NULL;
    memset(s, 0, sizeof(AdlibState));
    s->freq = SOUND_FREQUENCY;
    s->wpos = 0;
    s->rpos = 0;
    s->opl = OPL_new(3579552, s->freq);
    if (!s->opl) {
        free(s);
        return NULL;
    }

    /* A second malloc of sizeof(OPL)==1964 bytes is not viable here: this
     * firmware has repeatedly stopped inside pc_new() after much smaller
     * heap/layout changes.  On RP2350 the guest's physical 0xA0000 VGA
     * aperture is redirected to gfx_buffer, leaving its PSRAM backing store
     * unused.  Use 2 KiB of that shadow as caller-owned bank-1 state.
     *
     * This is deliberately an OPL3 approximation: it restores the second
     * bank's nine two-operator voices, but cannot implement OPL3 four-op
     * pairing or stereo panning. */
#if defined(RP2350_BUILD)
    g_opl3_bank1 = OPL_init_inplace((void *)(PSRAM_BASE_ADDR + 0x000a0000u),
                                    ADLIB_OPL3_STORAGE_BYTES,
                                    3579552, s->freq);
#else
    g_opl3_bank1 = OPL_new(3579552, s->freq);
#endif
    return s;
}

// call it 44100 times per sec from timer on core1 (ISR, so should be fast)
int16_t __not_in_flash_func(adlib_getsample)(AdlibState *s) {
    if (!s->opl) return 0;

    uint32_t r = s->rpos;
    if (r == s->wpos) {
        /* Hold the last sample rather than returning to zero. An underrun in
         * the middle of a sampled stream carries a large DC offset, and
         * snapping that to silence and back is a click far louder than the
         * gap it fills. */
        s->underrun_count++;
        return s->last;
    }

    int16_t sample = s->ring[r & (ADLIB_RING_SAMPLES - 1u)];
    s->last = sample;
    __dmb();
    s->rpos = r + 1u;
    return sample;
}

// call it from main cycle on core0
/*
 * Producer-gap instrumentation.
 *
 * Deepening the ring from 2.9 ms to 5.8 ms only cut silence by 5-10%, and the
 * captures showed total silence running at 2.5x total disk time - so core 0
 * is absent from the producer far longer than disk reads explain, and buffer
 * depth is not the binding constraint. This measures the absence directly.
 *
 * gap_lost_us is the part of each over-long gap that no buffer of the current
 * depth could have covered. If it sums to roughly the silence the underrun
 * counter reports, the gaps are the whole story and the question becomes what
 * core 0 is doing. If it comes back far smaller, the fault is on the consumer
 * side instead and this whole line of attack is wrong.
 */
#define ADLIB_DEPTH_US (ADLIB_NBUF * ADLIB_BATCH_SIZE * 1000000u / 44100u)

uint32_t g_adlib_calls;
uint32_t g_adlib_gap_max_us;
uint32_t g_adlib_gap_over;
uint32_t g_adlib_gap_lost_us;
static uint32_t adlib_last_call_us;

void adlib_gap_snapshot(uint32_t *calls, uint32_t *max_us,
                        uint32_t *over, uint32_t *lost_us)
{
    adlib_last_call_us = time_us_32();
    *calls   = g_adlib_calls;        g_adlib_calls = 0;
    *max_us  = g_adlib_gap_max_us;   g_adlib_gap_max_us = 0;
    *over    = g_adlib_gap_over;     g_adlib_gap_over = 0;
    *lost_us = g_adlib_gap_lost_us;  g_adlib_gap_lost_us = 0;
}

/*
 * Render forward until the producer sits ADLIB_LEAD_SAMPLES ahead of core 1.
 *
 * The pacing comes from the consumer's own index, not from a wall clock, and
 * that is deliberate.  Core 1 drains the ring on the I2S bit clock, which is
 * a divided system clock and not exactly 44100 Hz; anything paced off
 * time_us_32() would drift against it and eventually underrun or overflow on
 * a schedule of its own.  Chasing rpos cannot drift, because rpos *is* the
 * output clock.
 *
 * What that buys beyond stability: this is called from adlib_write() as well
 * as from the interpreter loop, so a guest that writes the OPL every 130 us
 * gets its writes separated by the five or six samples that actually elapsed
 * between them.  That is the whole fix for register-0x40 sample playback -
 * whole-batch rendering gave every sample in 1.45 ms of audio the same
 * register snapshot, which decimated an 8.5 kHz stream to about 690 Hz.
 */
void __not_in_flash_func(adlib_produce)(AdlibState *s, uint32_t min_samples) {
    if (!s->opl) return;

    int32_t n = (int32_t)(s->rpos + ADLIB_LEAD_SAMPLES - s->wpos);
    if (n < (int32_t)min_samples) return;

    const int use_bank1 = g_opl3_bank1 &&
                          (s->adlibregmem[0] & ADLIB_OPL3_ENABLED);

    while (n > 0) {
        const uint32_t w = s->wpos;
        const uint32_t off = w & (ADLIB_RING_SAMPLES - 1u);
        uint32_t chunk = (uint32_t)n;
        if (chunk > ADLIB_RENDER_CHUNK) chunk = ADLIB_RENDER_CHUNK;
        /* Never straddle the wrap: two shorter renders are cheaper than
         * carrying a split copy loop. */
        if (chunk > ADLIB_RING_SAMPLES - off) chunk = ADLIB_RING_SAMPLES - off;

        /* Chip samples this pass consumes. The interpolator crosses one input
         * boundary per output sample and occasionally a second, and this is
         * exactly how many times the loop below will step. */
        uint32_t frac = s->rs_frac;
        const uint32_t need = (frac + chunk * ADLIB_RS_STEP) >> 16;

        /* OPL renders int32, the ring stores int16.  Locals rather than
         * statics: .bss and the heap come out of the same pool, and that pool
         * has under a kilobyte to spare. */
        int32_t in[ADLIB_RS_MAX];
        OPL_calc_buffer_linear(s->opl, in, need);

        int32_t s0 = s->rs_s0, s1 = s->rs_s1;
        uint32_t k = 0;

        if (use_bank1) {
            /* Average the two nine-channel banks.  Summing at full scale
             * clips heavily once both banks play; averaging matches one OPL's
             * existing output range and leaves the final device mixer in
             * charge of overall volume.  Both banks run off the same crystal,
             * so one frac drives both. */
            int32_t in2[ADLIB_RS_MAX];
            OPL_calc_buffer_linear(g_opl3_bank1, in2, need);
            int32_t b0 = s->rs_b0, b1 = s->rs_b1;
            for (uint32_t j = 0; j < chunk; j++) {
                s->ring[off + j] = adlib_sat16((adlib_lerp(s0, s1, frac) +
                                                adlib_lerp(b0, b1, frac)) / 2);
                frac += ADLIB_RS_STEP;
                while (frac >= 0x10000u) {
                    s0 = s1; s1 = in[k];
                    b0 = b1; b1 = in2[k];
                    k++;
                    frac -= 0x10000u;
                }
            }
            s->rs_b0 = b0;
            s->rs_b1 = b1;
        } else {
            for (uint32_t j = 0; j < chunk; j++) {
                s->ring[off + j] = adlib_sat16(adlib_lerp(s0, s1, frac));
                frac += ADLIB_RS_STEP;
                while (frac >= 0x10000u) {
                    s0 = s1; s1 = in[k++];
                    frac -= 0x10000u;
                }
            }
        }

        s->rs_s0 = s0;
        s->rs_s1 = s1;
        s->rs_frac = frac;

        __dmb();
        s->wpos = w + chunk;
        n -= (int32_t)chunk;
    }
}

void __not_in_flash_func(adlib_core0)(AdlibState *s) {
    const uint32_t t_enter = time_us_32();
    const uint32_t now = t_enter;
    const uint32_t gap = now - adlib_last_call_us;
    adlib_last_call_us = now;
    g_adlib_calls++;
    if (gap > g_adlib_gap_max_us) g_adlib_gap_max_us = gap;
    if (gap > ADLIB_DEPTH_US) {
        g_adlib_gap_over++;
        g_adlib_gap_lost_us += gap - ADLIB_DEPTH_US;
    }

    adlib_produce(s, ADLIB_PERIODIC_MIN);
    g_adlib_us_total += time_us_32() - t_enter;
}

uint32_t adlib_underruns(AdlibState *s) {
    uint32_t u = s->underrun_count;
    s->underrun_count = 0;
    return u;
}
