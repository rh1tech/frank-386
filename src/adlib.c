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
 * Double-buffer layout
 * --------------------
 * buf[2][ADLIB_BATCH_SIZE] — two batches of pre-rendered samples.
 *
 * read_offset  — monotonically increasing counter owned by Core 1.
 *                sample index within the flat view: buf[read_offset / ADLIB_BATCH_SIZE]
 *                                                              [read_offset % ADLIB_BATCH_SIZE]
 *                When it reaches write_offset the buffer is empty → return silence.
 *
 * write_offset — monotonically increasing counter owned by Core 0.
 *                Always a multiple of ADLIB_BATCH_SIZE.
 *                Advances by ADLIB_BATCH_SIZE after each rendered batch.
 *                Wraps together with read_offset at 2*ADLIB_BATCH_SIZE so both
 *                counters always index into buf[0..1] correctly.
 *
 * Invariants:
 *   write_offset - read_offset == 0            → buffer empty  (Core 0 behind)
 *   write_offset - read_offset == ADLIB_BATCH_SIZE → one batch ready
 *   write_offset - read_offset == 2*ADLIB_BATCH_SIZE → both batches full (Core 0 ahead)
 *
 * Core 0 may fill at most one buffer ahead (stops when distance == 2*BATCH).
 * Core 1 returns 0 (silence) when distance == 0.
 *
 * No locks needed: write_offset is written only by Core 0, read_offset only by
 * Core 1. A __dmb() on Core 0 ensures buf contents are visible before the
 * counter update is seen by Core 1.
 */

#define BUF_TOTAL (2 * ADLIB_BATCH_SIZE)   /* wrap modulus */

struct AdlibState {
    uint32_t freq;
    uint16_t adlibregmem[5], adlib_register;
    uint8_t  adlibstatus;
    OPL     *opl;

    /* double-buffer */
    int32_t  buf[2][ADLIB_BATCH_SIZE];
    volatile uint32_t write_offset;   /* Core 0 writes, Core 1 reads */
    volatile uint32_t read_offset;    /* Core 1 writes, Core 0 reads */
};

void adlib_write(void *opaque, uint32_t nport, uint32_t val)
{
    AdlibState *s = opaque;
    switch (nport)
    {
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
    switch (nport)
    {
        case 0x388:
        case 0x389:
            if (!s->adlibregmem[4])
                s->adlibstatus = 0;
            else
                s->adlibstatus = 0x80;
            s->adlibstatus = s->adlibstatus + (s->adlibregmem[4] & 1) * 0x40 + (s->adlibregmem[4] & 2) * 0x10;
            return s->adlibstatus;
    }
    return 0xFF;
}

AdlibState *adlib_new()
{
    AdlibState *s = malloc(sizeof(AdlibState));
    memset(s, 0, sizeof(AdlibState));
    s->freq         = SOUND_FREQUENCY;
    s->write_offset = 0;
    s->read_offset  = 0;
    s->opl = OPL_new(3579552, s->freq);
    if (!s->opl) {
        return NULL;
    }
    return s;
}

// call it 44100 times per sec from timer on core1 (ISR, so should be fast)
int16_t __not_in_flash_func(adlib_getsample)(AdlibState *s) {
    if (!s->opl) {
        return 0;
    }

    volatile uint32_t *offset = &s->read_offset;   /* Core 1 owns this */
    uint32_t  woffset = s->write_offset;

    /* Buffer empty — Core 0 hasn't rendered anything yet. Return silence. */
    if (*offset == woffset) {
        return 0;
    }

    /* Select the right buffer half and hand out the next sample. */
    int32_t *samples = s->buf[(*offset / ADLIB_BATCH_SIZE) & 1];
    int16_t  sample  = (int16_t)samples[*offset % ADLIB_BATCH_SIZE];
    (*offset)++;

    /* Wrap both counters together to keep them in [0, 2*BATCH_SIZE). */
    if (*offset >= BUF_TOTAL) {
        *offset       -= BUF_TOTAL;
        /* write_offset may have also wrapped; keep it consistent.
         * Safe: Core 0 only advances write_offset in multiples of BATCH_SIZE,
         * and always checks distance before writing, so it will never be
         * more than BUF_TOTAL ahead. */
        s->write_offset -= BUF_TOTAL;
    }

    return sample;
}

// call it from main cycle on core0
void __not_in_flash_func(adlib_core0)(AdlibState *s) {
    if (!s->opl) {
        return;
    }

    uint32_t roffset = s->read_offset;
    uint32_t woffset = s->write_offset;

    /* Both buffers are full — Core 1 is behind. Nothing to do. */
    if (woffset - roffset >= BUF_TOTAL) {
        return;
    }

    /* Fill the buffer that write_offset points into. */
    int32_t *samples = s->buf[(woffset / ADLIB_BATCH_SIZE) & 1];
    OPL_calc_buffer_linear(s->opl, samples, ADLIB_BATCH_SIZE);

    /* Ensure all sample writes are visible to Core 1 before counter update. */
    __dmb();

    s->write_offset = woffset + ADLIB_BATCH_SIZE;
}
