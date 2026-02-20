/**
 * murm386 - SN76489 (Tandy 3-Voice Sound) emulator
 *
 * Based on emu76489.c by Mitsutaka Okazaki 2001-2016
 * Adapted for murm386 by Mikhail Matveev, 2026
 *
 * Clock:  3 579 545 Hz (NTSC), divided by 16 internally => ~223 722 ticks/s
 * Output: stereo int16 at AUDIO_SAMPLE_RATE (44 100 Hz)
 *
 * SPDX-License-Identifier: MIT
 */

#pragma GCC optimize("O3")

#include "sn76489.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── tunables ────────────────────────────────────────────────────────────── */

#define SN_CLOCK          3579545u   /* chip input clock, Hz                 */
#define SN_DIVIDER        16u        /* chip internal pre-scaler              */

#ifndef AUDIO_SAMPLE_RATE
#  define AUDIO_SAMPLE_RATE 44100u
#endif

/*
 * Fixed-point accumulator: 16 integer + 16 fractional bits.
 * SN internal clock = SN_CLOCK / SN_DIVIDER = 223721.5625 Hz
 * Ticks per output sample = 223721.5625 / 44100 = 5.0731...
 *
 * FRAC_BITS=16 gives increment = 5.0731 * 65536 = 332427.
 * clks = acc >> 16  ->  5 most of the time, 6 occasionally.  Accurate.
 */
#define FRAC_BITS       16u
#define FRAC_ONE        (1u << FRAC_BITS)
#define FRAC_MASK       (FRAC_ONE - 1u)
#define CLOCK_INC  ((uint32_t)(((double)SN_CLOCK / SN_DIVIDER) \
                     * FRAC_ONE / AUDIO_SAMPLE_RATE + 0.5))

/* ── volume table ────────────────────────────────────────────────────────── */
/*
 * 2 dB per step, 16 entries (15 = silence).
 * Max value 16384: four channels summed -> max acc = 65536,
 * divided by >>2 in sn76489_tick -> max output = 16384 (50% of int16).
 * This matches the ~50% level of SB16/Adlib (both attenuated >>1 in mixer),
 * giving equal perceived loudness when mixed.
 */
static const uint16_t vol_table[16] = {
    16384, 13014, 10338,  8211,
     6523,  5181,  4115,  3269,
     2597,  2063,  1638,  1301,
     1034,   821,   652,     0
};

/* ── noise LFSR parity (feedback taps: bit0 XOR bit3) ───────────────────── */
static inline uint8_t parity_tap09(uint16_t v) {
    v ^= v >> 3;
    v ^= v >> 6;
    v ^= v >> 12;
    return v & 1u;
}

/* ── chip state ──────────────────────────────────────────────────────────── */

struct SN76489State {
    /* tone channels 0-2 */
    uint16_t tone_freq[3];   /* 10-bit reload period (internal clock ticks)  */
    uint16_t tone_vol[3];    /* 4-bit attenuation index                       */
    uint16_t tone_ctr[3];    /* down-counter                                  */
    uint8_t  tone_out[3];    /* current output polarity (0 or 1)             */

    /* noise channel */
    uint16_t noise_lfsr;     /* 15-bit LFSR                                   */
    uint16_t noise_period;   /* reload period in internal clock ticks         */
    uint16_t noise_vol;      /* 4-bit attenuation index                       */
    uint16_t noise_ctr;      /* down-counter                                  */
    uint8_t  noise_white;    /* 0 = periodic, 1 = white                       */
    uint8_t  noise_tone2;    /* 1 = track tone2 period                        */

    /* register latch */
    uint8_t  reg_addr;       /* last addressed register (0-7)                 */

    /* sample-rate converter */
    uint32_t frac_acc;       /* fractional tick accumulator                   */
};

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Decode the two-bit NF field to a noise base period in internal ticks.
 * NF=0 -> 16, NF=1 -> 32, NF=2 -> 64, NF=3 -> use tone2 */
static uint16_t noise_base_period(uint8_t nf) {
    return (uint16_t)(16u << nf);   /* 16, 32, 64 */
}

/* ── public API ──────────────────────────────────────────────────────────── */

SN76489State *sn76489_new(void) {
    SN76489State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    for (int i = 0; i < 3; i++) {
        s->tone_vol[i]  = 0x0Fu;   /* max attenuation = silent               */
        s->tone_freq[i] = 0x3FFu;  /* reset value per datasheet              */
        s->tone_ctr[i]  = 0x3FFu;
    }
    s->noise_vol    = 0x0Fu;
    s->noise_period = 16u;         /* NF0 default                            */
    s->noise_ctr    = 16u;
    s->noise_lfsr   = 0x8000u;
    return s;
}

void sn76489_free(SN76489State *s) {
    free(s);
}

void sn76489_write(SN76489State *s, uint8_t val) {
    if (val & 0x80u) {
        /* LATCH/DATA byte: bit6..4 = register address, bit3..0 = low data  */
        s->reg_addr = (val >> 4) & 0x07u;

        switch (s->reg_addr) {
        case 0: case 2: case 4:   /* tone N frequency – low 4 bits          */
            s->tone_freq[s->reg_addr >> 1] =
                (s->tone_freq[s->reg_addr >> 1] & 0x3F0u) | (val & 0x0Fu);
            break;

        case 1: case 3: case 5:   /* tone N volume                          */
            s->tone_vol[(s->reg_addr - 1u) >> 1] = val & 0x0Fu;
            break;

        case 6: {                  /* noise: mode + frequency                */
            uint8_t nf = val & 0x03u;
            s->noise_white = (val >> 2) & 1u;
            if (nf == 0x03u) {
                s->noise_period = s->tone_freq[2];
                s->noise_tone2  = 1u;
            } else {
                s->noise_period = noise_base_period(nf);
                s->noise_tone2  = 0u;
            }
            if (!s->noise_period) s->noise_period = 1u;
            s->noise_ctr  = s->noise_period;
            s->noise_lfsr = 0x8000u;             /* reset LFSR on noise write */
            break;
        }

        case 7:                    /* noise volume                           */
            s->noise_vol = val & 0x0Fu;
            break;
        }
    } else {
        /* DATA byte: bits5..0 are the high 6 bits of the current freq reg  */
        /* Only valid if the latched register is a frequency register        */
        if ((s->reg_addr & 1u) == 0u) {
            s->tone_freq[s->reg_addr >> 1] =
                ((val & 0x3Fu) << 4) | (s->tone_freq[s->reg_addr >> 1] & 0x0Fu);
        }
    }
}

/* ── inner tick: advance chip by `clks` internal clock cycles ───────────── */

static int16_t sn76489_tick(SN76489State *s, uint32_t clks) {
    int32_t acc = 0;

    /* ── tone channels ───────────────────────────────────────────────────── */
    for (int ch = 0; ch < 3; ch++) {
        uint16_t freq = s->tone_freq[ch];
        if (freq <= 1u) {
            /* Freq=0 or 1 -> output held HIGH (DC) per SN76489 datasheet    */
            s->tone_out[ch] = 1u;
        } else {
            uint32_t ctr = s->tone_ctr[ch];
            uint32_t rem = clks;
            while (rem >= ctr) {
                rem -= ctr;
                s->tone_out[ch] ^= 1u;
                ctr = freq;
            }
            s->tone_ctr[ch] = (uint16_t)(ctr - rem);
        }
        if (s->tone_out[ch])
            acc += vol_table[s->tone_vol[ch]];
    }

    /* ── noise channel ───────────────────────────────────────────────────── */
    {
        uint16_t period = s->noise_tone2 ? s->tone_freq[2] : s->noise_period;
        if (!period) period = 1u;

        uint32_t ctr = s->noise_ctr;
        uint32_t rem = clks;
        while (rem >= ctr) {
            rem -= ctr;
            /* clock LFSR */
            if (s->noise_white)
                s->noise_lfsr = (uint16_t)((s->noise_lfsr >> 1) |
                                ((uint16_t)parity_tap09(s->noise_lfsr) << 15));
            else
                s->noise_lfsr = (uint16_t)((s->noise_lfsr >> 1) |
                                ((s->noise_lfsr & 1u) << 15));
            ctr = period;
        }
        s->noise_ctr = (uint16_t)(ctr - rem);

        if (s->noise_lfsr & 1u)
            acc += vol_table[s->noise_vol];
    }

    /* 4 channels summed -> attenuate by 2 bits to stay in int16 range       */
    return (int16_t)(acc >> 2);
}

/* ── audio callback ──────────────────────────────────────────────────────── */

void sn76489_callback(SN76489State *s, uint8_t *stream, int len) {
    if (!s || len <= 0) return;

    int16_t *out    = (int16_t *)stream;
    int      frames = len / 4;          /* stereo int16: 4 bytes per frame   */

    for (int i = 0; i < frames; i++) {
        s->frac_acc += CLOCK_INC;
        uint32_t clks  = s->frac_acc >> FRAC_BITS;
        s->frac_acc   &= FRAC_MASK;

        int16_t sample = sn76489_tick(s, clks);

        out[i * 2 + 0] = sample;        /* L */
        out[i * 2 + 1] = sample;        /* R */
    }
}
