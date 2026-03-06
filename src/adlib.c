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
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "adlib.h"
#include "emu8950/emu8950.h"

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

    int32_t  buf[2][ADLIB_BATCH_SIZE];
    volatile uint8_t ready[2];  /* 1 = filled by Core 0, not yet consumed */
    uint8_t  play_buf;          /* Core 1: which buf is being played */
    uint32_t read_pos;          /* Core 1: next sample index in play_buf */

    uint32_t underrun_count;
};

void adlib_write(void *opaque, uint32_t nport, uint32_t val)
{
    AdlibState *s = opaque;
    switch (nport) {
        case 0x388:
            s->adlib_register = val;
            break;
        case 0x389:
            if (s->adlib_register <= 4) {
                s->adlibregmem[s->adlib_register] = val;
                if (s->adlib_register == 4 && (val & 0x80)) {
                    s->adlibstatus = 0;
                    s->adlibregmem[4] = 0;
                }
            }
            OPL_writeReg(s->opl, s->adlib_register, val);
    }
}

uint32_t adlib_read(void *opaque, uint32_t nport)
{
    AdlibState *s = opaque;
    switch (nport) {
        case 0x388:
        case 0x389:
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
    memset(s, 0, sizeof(AdlibState));
    s->freq     = SOUND_FREQUENCY;
    s->play_buf = 0;
    s->read_pos = 0;
    s->opl = OPL_new(3579552, s->freq);
    if (!s->opl) {
        return NULL;
    }
    return s;
}

// call it 44100 times per sec from timer on core1 (ISR, so should be fast)
int16_t __not_in_flash_func(adlib_getsample)(AdlibState *s) {
    if (!s->opl) return 0;

    if (!s->ready[s->play_buf]) {
        s->underrun_count++;
        return 0;
    }

    int16_t sample = (int16_t)s->buf[s->play_buf][s->read_pos++];

    if (s->read_pos >= ADLIB_BATCH_SIZE) {
        /* Mark this buffer as consumed, switch to the other one. */
        s->ready[s->play_buf] = 0;
        s->play_buf ^= 1;
        s->read_pos = 0;
    }

    return sample;
}

// call it from main cycle on core0
void __not_in_flash_func(adlib_core0)(AdlibState *s) {
    if (!s->opl) return;

    /* Fill buf[0] first, then buf[1], alternating.
     * Fill whichever buffer is free (not ready). Prefer the one
     * Core 1 is about to play (play_buf) if it's empty, otherwise
     * fill the other one as look-ahead. */
    for (int i = 0; i < 2; i++) {
        uint8_t fill_buf = (s->play_buf + i) & 1;
        if (s->ready[fill_buf]) continue;  /* already full */

        OPL_calc_buffer_linear(s->opl, s->buf[fill_buf], ADLIB_BATCH_SIZE);
        __dmb();
        s->ready[fill_buf] = 1;
        return;  /* fill one buffer per call */
    }
}

uint32_t adlib_underruns(AdlibState *s) {
    uint32_t u = s->underrun_count;
    s->underrun_count = 0;
    return u;
}
