/*
 * QEMU Soundblaster 16 emulation
 *
 * Copyright (c) 2003-2005 Vassili Karpov (malc)
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

#include "sb16.h"
#include "audiodiag.h"
#include <pico.h>
#include <pico/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "i8257.h"
#include <hardware/sync.h>

#if defined(BUILD_ESP32) || defined(RP2350_BUILD)
void *pcmalloc(long size);
#else
#define pcmalloc malloc
#endif

#ifdef SB16_LOG
#define dolog(...) fprintf(stderr, "sb16: " __VA_ARGS__)
#define qemu_log_mask(_, ...) fprintf(stderr, "sb16: " __VA_ARGS__)
#else
#define dolog(...)
#define qemu_log_mask(_, ...)
#endif

/* #define DEBUG */
/* #define DEBUG_SB16_MOST */

#ifdef DEBUG
#define ldebug(...) dolog (__VA_ARGS__)
#else
#define ldebug(...)
#endif

typedef enum {
    AUDIO_FORMAT_U8,
    AUDIO_FORMAT_S8,
    AUDIO_FORMAT_U16,
    AUDIO_FORMAT_S16,
} AudioFormat;

static const char e3[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";

struct SB16State {
//    QEMUSoundCard card;
    void *pic;
    void (*set_irq)(void *pic, int irq, int level);
    uint32_t irq;
    uint32_t dma;
    uint32_t hdma;
    uint32_t port;
    uint32_t ver;
    IsaDma *isa_dma;
    IsaDma *isa_hdma;

    int in_index;
    int out_data_len;
    int fmt_stereo;
    int fmt_signed;
    int fmt_bits;
    AudioFormat fmt;
    int dma_auto;
    int block_size;
    int fifo;
    int freq;
    int time_const;
    int speaker;
    int needed_bytes;
    int cmd;
    int use_hdma;
    int highspeed;
    int can_write;

    int v2x6;

    uint8_t csp_param;
    uint8_t csp_value;
    uint8_t csp_mode;
    uint8_t csp_regs[256];
    uint8_t csp_index;
    uint8_t csp_reg83[4];
    int csp_reg83r;
    int csp_reg83w;

    uint8_t in2_data[10];
    uint8_t out_data[50];
    uint8_t test_reg;
    uint8_t last_read_byte;
    int nzero;

    int left_till_irq;

    int dma_running;
    int bytes_per_second;
    int align;
    int audio_free;
#define AUDIO_BUF_LEN 4096
    uint8_t audio_buf[AUDIO_BUF_LEN];
    unsigned int audio_p, audio_q;
    void *voice;
    int active_out;

    /* Deadline for the DSP 0x80 silence period, in time_us_32() units.
     * QEMU arms a QEMUTimer here; this port has no timer infrastructure, so
     * the deadline is polled from pc_step() instead - see sb16_poll(). */
    uint32_t aux_deadline_us;
    int      aux_pending;
    volatile uint8_t irq_raise_pending;   /* core 1 asked for a rising edge */

    /* mixer state */
    int mixer_nreg;
    uint8_t mixer_regs[256];

    uint8_t e2_valadd;
    uint8_t e2_valxor;
};

/* FRANK_SB16_DIAG_V8_10_1: defined further down, next to write_audio(). */
void sb16_diag_playback_start(void);

/* All interrupt raises and drops funnel through here so the diagnostic trace
 * shows them in order with the DSP command and the DMA run that caused them -
 * which is the only way to tell "the card never interrupted" apart from "the
 * guest never got the interrupt". */
static inline void sb_set_irq(SB16State *s, int level)
{
    frank_diag_ev(FRANK_EV_IRQ, (uint8_t)s->irq, 0, (uint32_t)level);

    /*
     * The interrupt controller belongs to core 0, and only core 0 may touch
     * it.
     *
     * Every other interrupt source in this machine - the PIT, the keyboard,
     * the IDE channels, the RTC - is driven from pc_step(), so the 8259's
     * IRR, ISR and last_irr are private to core 0.  The Sound Blaster is the
     * exception: the block-completion interrupt is raised from
     * sb16_getsample(), which runs in the 44.1 kHz timer callback on core 1.
     * Nothing guards those registers, so that raise could land in the middle
     * of core 0 acknowledging an interrupt - pic_intack() clears IRR, sets
     * ISR and hands back a vector - and the guest then took a far transfer
     * through the wrong interrupt vector.
     *
     * That is what breaks Supaplex, and the two shapes it takes are just the
     * same bad transfer seen from either side of a memory manager: with
     * EMM386 loaded the V86 monitor catches the runaway and the program
     * restarts to its copy-protection screen, and without it the guest runs
     * into unwritten memory and spins on #UD - measured at twenty-two million
     * of them.  It is only the Sound Blaster because it is the only device
     * that interrupts from the other core, and it is intermittent because it
     * is a race.
     *
     * Core 1 therefore only ever *asks*, and pc_step() does the raising.  It
     * only ever asks for a rising edge - lowering happens when the guest
     * acknowledges at base+0x0e, which is core 0 by definition - so a single
     * flag loses nothing.
     */
    if (level && get_core_num() != 0) {
        s->irq_raise_pending = 1;
        __dmb();
        return;
    }
    s->set_irq(s->pic, s->irq, level);
}

/*
 * How far the DMA engine may run ahead of what core 1 has actually played.
 *
 * Expressed as a span of time rather than a byte count, because that is what
 * the hardware constraint is: a real card moves one DMA byte per sample
 * period, so the transfer - and therefore the block-completion interrupt -
 * is paced by the sample clock and by nothing else.  Twenty milliseconds
 * rides out the core 0 stalls that matter here (FatFS issues eight ~475 us
 * reads for one cluster) while staying well short of the block sizes games
 * actually use, so a block still takes a block's worth of time to finish.
 */
#define SB16_LEAD_MS 20

static int sb16_lead_bytes (SB16State *s)
{
    int lead = (s->bytes_per_second / 1000) * SB16_LEAD_MS;
    if (lead < 64) lead = 64;
    if (lead > AUDIO_BUF_LEN / 2) lead = AUDIO_BUF_LEN / 2;
    return lead & ~s->align;
}

static void AUD_set_active_out (SB16State *s, int i)
{
    /*
     * Capture 016 measured sb16_dma_gap_max_us = 17.4 s on a 36 s window,
     * because Draci historie plays short effects and the "gap" spanned the
     * silence between two of them.  A gap is only meaningful inside one
     * continuous playback episode, so restart the clock whenever playback
     * begins.
     */
    if (i && !s->active_out) sb16_diag_playback_start();
    s->active_out = i;
}

static void set_audio(void *s, int format, int freq, int nchan)
{
    dolog("audio fmt %d freq %d chan %d\n", format, freq, nchan);
}

static int magic_of_irq (int irq)
{
    switch (irq) {
    case 5:
        return 2;
    case 7:
        return 4;
    case 9:
        return 1;
    case 10:
        return 8;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "bad irq %d\n", irq);
        return 2;
    }
}

static int irq_of_magic (int magic)
{
    switch (magic) {
    case 1:
        return 9;
    case 2:
        return 5;
    case 4:
        return 7;
    case 8:
        return 10;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "bad irq magic %d\n", magic);
        return -1;
    }
}

#if 0
static void log_dsp (SB16State *dsp)
{
    ldebug ("%s:%s:%d:%s:dmasize=%d:freq=%d:const=%d:speaker=%d\n",
            dsp->fmt_stereo ? "Stereo" : "Mono",
            dsp->fmt_signed ? "Signed" : "Unsigned",
            dsp->fmt_bits,
            dsp->dma_auto ? "Auto" : "Single",
            dsp->block_size,
            dsp->freq,
            dsp->time_const,
            dsp->speaker);
}
#endif

static void speaker (SB16State *s, int on)
{
    s->speaker = on;
    /* AUD_enable (s->voice, on); */
}

static void control (SB16State *s, int hold)
{
    int dma = s->use_hdma ? s->hdma : s->dma;
    IsaDma *isa_dma = s->use_hdma ? s->isa_hdma : s->isa_dma;
    s->dma_running = hold;

    ldebug ("hold %d high %d dma %d\n", hold, s->use_hdma, dma);

    if (hold) {
        i8257_dma_hold_DREQ(isa_dma, dma);
        AUD_set_active_out (s->voice, 1);
    }
    else {
        i8257_dma_release_DREQ(isa_dma, dma);
        AUD_set_active_out (s->voice, 0);
    }
}

#if 0
static void aux_timer (void *opaque)
{
    SB16State *s = opaque;
    s->can_write = 1;
    sb_set_irq(s, 1);
}
#endif

#define DMA8_AUTO 1
#define DMA8_HIGH 2

static void continue_dma8 (SB16State *s)
{
    if (s->freq > 0) {
        set_audio(s, s->fmt, s->freq, 1 << s->fmt_stereo);
        s->voice = s;
    }

    control (s, 1);
}

static void dma_cmd8 (SB16State *s, int mask, int dma_len)
{
    s->fmt = AUDIO_FORMAT_U8;
    s->use_hdma = 0;
    s->fmt_bits = 8;
    s->fmt_signed = 0;
    s->fmt_stereo = (s->mixer_regs[0x0e] & 2) != 0;
    if (-1 == s->time_const) {
        if (s->freq <= 0)
            s->freq = 11025;
    }
    else {
        int tmp = (256 - s->time_const);
        s->freq = (1000000 + (tmp / 2)) / tmp;
        s->freq >>= s->fmt_stereo;
        s->time_const = -1;
    }

    if (dma_len != -1) {
        s->block_size = dma_len << s->fmt_stereo;
    }
    else {
        /* This is apparently the only way to make both Act1/PL
           and SecondReality/FC work

           Act1 sets block size via command 0x48 and it's an odd number
           SR does the same with even number
           Both use stereo, and Creatives own documentation states that
           0x48 sets block size in bytes less one.. go figure */
        s->block_size &= ~s->fmt_stereo;
    }

    s->left_till_irq = s->block_size;
    s->bytes_per_second = (s->freq << s->fmt_stereo);
    frank_diag_ev(FRANK_EV_DSP_CMD, 0x14, (uint16_t)s->block_size,
                  (uint32_t)s->freq);
    /* s->highspeed = (mask & DMA8_HIGH) != 0; */
    s->dma_auto = (mask & DMA8_AUTO) != 0;
    s->align = (1 << s->fmt_stereo) - 1;

    if (s->block_size & s->align) {
        qemu_log_mask(LOG_GUEST_ERROR, "warning: misaligned block size %d,"
                      " alignment %d\n", s->block_size, s->align + 1);
    }

    ldebug ("freq %d, stereo %d, sign %d, bits %d, "
            "dma %d, auto %d, fifo %d, high %d\n",
            s->freq, s->fmt_stereo, s->fmt_signed, s->fmt_bits,
            s->block_size, s->dma_auto, s->fifo, s->highspeed);

    /*
     * Drop anything older than the look-ahead before a new single-shot block.
     *
     * This used to discard the whole pending buffer, which was only necessary
     * because an unpaced transfer could leave up to 4 KB of stale audio
     * queued.  With the DMA paced by playback the buffer never holds more than
     * SB16_LEAD_MS of audio, and that much is not stale - it is what a real
     * card would still be clocking out of the previous block - so discarding
     * it only puts a gap between two blocks a game meant to run back to back.
     * The clamp stays as a guard for a game that reprograms the rate
     * mid-stream, where the queued lead is suddenly worth more milliseconds
     * than it was when it was queued.
     */
    if (!s->dma_auto) {
        int lead = sb16_lead_bytes (s);
        if ((int)(s->audio_q - s->audio_p) > lead) {
            s->audio_p = s->audio_q - lead;
        }
    }

    continue_dma8 (s);
    speaker (s, 1);
}

static void dma_cmd (SB16State *s, uint8_t cmd, uint8_t d0, int dma_len)
{
    frank_diag_ev(FRANK_EV_DSP_CMD, cmd, (uint16_t)dma_len, (uint32_t)d0);
    s->use_hdma = cmd < 0xc0;
    s->fifo = (cmd >> 1) & 1;
    s->dma_auto = (cmd >> 2) & 1;
    s->fmt_signed = (d0 >> 4) & 1;
    s->fmt_stereo = (d0 >> 5) & 1;

    switch (cmd >> 4) {
    case 11:
        s->fmt_bits = 16;
        break;

    case 12:
        s->fmt_bits = 8;
        break;
    }

    if (-1 != s->time_const) {
#if 1
        int tmp = 256 - s->time_const;
        s->freq = (1000000 + (tmp / 2)) / tmp;
#else
        /* s->freq = 1000000 / ((255 - s->time_const) << s->fmt_stereo); */
        s->freq = 1000000 / ((255 - s->time_const));
#endif
        s->time_const = -1;
    }

    s->block_size = dma_len + 1;
    s->block_size <<= (s->fmt_bits == 16);
    if (!s->dma_auto) {
        /* It is clear that for DOOM and auto-init this value
           shouldn't take stereo into account, while Miles Sound Systems
           setsound.exe with single transfer mode wouldn't work without it
           wonders of SB16 yet again */
        s->block_size <<= s->fmt_stereo;
    }

    ldebug ("freq %d, stereo %d, sign %d, bits %d, "
            "dma %d, auto %d, fifo %d, high %d\n",
            s->freq, s->fmt_stereo, s->fmt_signed, s->fmt_bits,
            s->block_size, s->dma_auto, s->fifo, s->highspeed);

    if (16 == s->fmt_bits) {
        if (s->fmt_signed) {
            s->fmt = AUDIO_FORMAT_S16;
        }
        else {
            s->fmt = AUDIO_FORMAT_U16;
        }
    }
    else {
        if (s->fmt_signed) {
            s->fmt = AUDIO_FORMAT_S8;
        }
        else {
            s->fmt = AUDIO_FORMAT_U8;
        }
    }

    s->left_till_irq = s->block_size;

    s->bytes_per_second = (s->freq << s->fmt_stereo) << (s->fmt_bits == 16);
    s->highspeed = 0;
    s->align = (1 << (s->fmt_stereo + (s->fmt_bits == 16))) - 1;
    if (s->block_size & s->align) {
        qemu_log_mask(LOG_GUEST_ERROR, "warning: misaligned block size %d,"
                      " alignment %d\n", s->block_size, s->align + 1);
    }

    if (s->freq) {
        set_audio(s, s->fmt, s->freq, 1 << s->fmt_stereo);
        s->voice = s;
    }

    /*
     * Drop anything older than the look-ahead before a new single-shot block.
     *
     * This used to discard the whole pending buffer, which was only necessary
     * because an unpaced transfer could leave up to 4 KB of stale audio
     * queued.  With the DMA paced by playback the buffer never holds more than
     * SB16_LEAD_MS of audio, and that much is not stale - it is what a real
     * card would still be clocking out of the previous block - so discarding
     * it only puts a gap between two blocks a game meant to run back to back.
     * The clamp stays as a guard for a game that reprograms the rate
     * mid-stream, where the queued lead is suddenly worth more milliseconds
     * than it was when it was queued.
     */
    if (!s->dma_auto) {
        int lead = sb16_lead_bytes (s);
        if ((int)(s->audio_q - s->audio_p) > lead) {
            s->audio_p = s->audio_q - lead;
        }
    }

    control (s, 1);
    speaker (s, 1);
}

static inline void dsp_out_data (SB16State *s, uint8_t val)
{
    ldebug ("outdata %#x\n", val);
    if ((size_t) s->out_data_len < sizeof (s->out_data)) {
        s->out_data[s->out_data_len++] = val;
    }
}

static inline uint8_t dsp_get_data (SB16State *s)
{
    if (s->in_index) {
        return s->in2_data[--s->in_index];
    }
    else {
        dolog ("buffer underflow\n");
        return 0;
    }
}

static void command (SB16State *s, uint8_t cmd)
{
    ldebug ("command %#x\n", cmd);

    if (cmd > 0xaf && cmd < 0xd0) {
        if (cmd & 8) {
            qemu_log_mask(LOG_UNIMP, "ADC not yet supported (command %#x)\n",
                          cmd);
        }

        switch (cmd >> 4) {
        case 11:
        case 12:
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%#x wrong bits\n", cmd);
        }
        s->needed_bytes = 3;
    }
    else {
        s->needed_bytes = 0;

        switch (cmd) {
        case 0x03:
            dsp_out_data (s, 0x10); /* s->csp_param); */
            goto warn;

        case 0x04:
            s->needed_bytes = 1;
            goto warn;

        case 0x05:
            s->needed_bytes = 2;
            goto warn;

        case 0x08:
            /* __asm__ ("int3"); */
            goto warn;

        case 0x0e:
            s->needed_bytes = 2;
            goto warn;

        case 0x09:
            dsp_out_data (s, 0xf8);
            goto warn;

        case 0x0f:
            s->needed_bytes = 1;
            goto warn;

        case 0x10:
            s->needed_bytes = 1;
            goto warn;

        case 0x14:
            s->needed_bytes = 2;
            s->block_size = 0;
            break;

        case 0x1c:              /* Auto-Initialize DMA DAC, 8-bit */
            dma_cmd8 (s, DMA8_AUTO, -1);
            break;

        case 0x20:              /* Direct ADC, Juice/PL */
            dsp_out_data (s, 0xff);
            goto warn;

        case 0x35:
            qemu_log_mask(LOG_UNIMP, "0x35 - MIDI command not implemented\n");
            break;

        case 0x40:
            s->freq = -1;
            s->time_const = -1;
            s->needed_bytes = 1;
            break;

        case 0x41:
            s->freq = -1;
            s->time_const = -1;
            s->needed_bytes = 2;
            break;

        case 0x42:
            s->freq = -1;
            s->time_const = -1;
            s->needed_bytes = 2;
            goto warn;

        case 0x45:
            dsp_out_data (s, 0xaa);
            goto warn;

        case 0x47:                /* Continue Auto-Initialize DMA 16bit */
            break;

        case 0x48:
            s->needed_bytes = 2;
            break;

        case 0x74:
            s->needed_bytes = 2; /* DMA DAC, 4-bit ADPCM */
            qemu_log_mask(LOG_UNIMP, "0x75 - DMA DAC, 4-bit ADPCM not"
                          " implemented\n");
            break;

        case 0x75:              /* DMA DAC, 4-bit ADPCM Reference */
            s->needed_bytes = 2;
            qemu_log_mask(LOG_UNIMP, "0x74 - DMA DAC, 4-bit ADPCM Reference not"
                          " implemented\n");
            break;

        case 0x76:              /* DMA DAC, 2.6-bit ADPCM */
            s->needed_bytes = 2;
            qemu_log_mask(LOG_UNIMP, "0x74 - DMA DAC, 2.6-bit ADPCM not"
                          " implemented\n");
            break;

        case 0x77:              /* DMA DAC, 2.6-bit ADPCM Reference */
            s->needed_bytes = 2;
            qemu_log_mask(LOG_UNIMP, "0x74 - DMA DAC, 2.6-bit ADPCM Reference"
                          " not implemented\n");
            break;

        case 0x7d:
            qemu_log_mask(LOG_UNIMP, "0x7d - Autio-Initialize DMA DAC, 4-bit"
                          " ADPCM Reference\n");
            qemu_log_mask(LOG_UNIMP, "not implemented\n");
            break;

        case 0x7f:
            qemu_log_mask(LOG_UNIMP, "0x7d - Autio-Initialize DMA DAC, 2.6-bit"
                          " ADPCM Reference\n");
            qemu_log_mask(LOG_UNIMP, "not implemented\n");
            break;

        case 0x80:
            s->needed_bytes = 2;
            break;

        case 0x90:
        case 0x91:
            dma_cmd8 (s, ((cmd & 1) == 0) | DMA8_HIGH, -1);
            break;

        case 0xd0:              /* halt DMA operation. 8bit */
            control (s, 0);
            break;

        case 0xd1:              /* speaker on */
            speaker (s, 1);
            break;

        case 0xd3:              /* speaker off */
            speaker (s, 0);
            break;

        case 0xd4:              /* continue DMA operation. 8bit */
            /* KQ6 (or maybe Sierras audblst.drv in general) resets
               the frequency between halt/continue */
            continue_dma8 (s);
            break;

        case 0xd5:              /* halt DMA operation. 16bit */
            control (s, 0);
            break;

        case 0xd6:              /* continue DMA operation. 16bit */
            control (s, 1);
            break;

        case 0xd9:              /* exit auto-init DMA after this block. 16bit */
            s->dma_auto = 0;
            break;

        case 0xda:              /* exit auto-init DMA after this block. 8bit */
            s->dma_auto = 0;
            break;

        case 0xe0:              /* DSP identification */
            s->needed_bytes = 1;
            break;

        case 0xe1:
            dsp_out_data (s, s->ver & 0xff);
            dsp_out_data (s, s->ver >> 8);
            break;

        case 0xe2:
            s->needed_bytes = 1;
            goto warn;

        case 0xe3:
            {
                int i;
                for (i = sizeof (e3) - 1; i >= 0; --i)
                    dsp_out_data (s, e3[i]);
            }
            break;

        case 0xe4:              /* write test reg */
            s->needed_bytes = 1;
            break;

        case 0xe7:
            qemu_log_mask(LOG_UNIMP, "Attempt to probe for ESS (0xe7)?\n");
            break;

        case 0xe8:              /* read test reg */
            dsp_out_data (s, s->test_reg);
            break;

        case 0xf2:
        case 0xf3:
            dsp_out_data (s, 0xaa);
            s->mixer_regs[0x82] |= (cmd == 0xf2) ? 1 : 2;
            sb_set_irq(s, 1);
            break;

        case 0xf9:
            s->needed_bytes = 1;
            goto warn;

        case 0xfa:
            dsp_out_data (s, 0);
            goto warn;

        case 0xfc:              /* FIXME */
        case 0xf8:
            dsp_out_data (s, 0);
            goto warn;

        default:
            qemu_log_mask(LOG_UNIMP, "Unrecognized command %#x\n", cmd);
            break;
        }
    }

    if (!s->needed_bytes) {
        ldebug ("\n");
    }

 exit:
    if (!s->needed_bytes) {
        s->cmd = -1;
    }
    else {
        s->cmd = cmd;
    }
    return;

 warn:
    qemu_log_mask(LOG_UNIMP, "warning: command %#x,%d is not truly understood"
                  " yet\n", cmd, s->needed_bytes);
    goto exit;

}

static uint16_t dsp_get_lohi (SB16State *s)
{
    uint8_t hi = dsp_get_data (s);
    uint8_t lo = dsp_get_data (s);
    return (hi << 8) | lo;
}

static uint16_t dsp_get_hilo (SB16State *s)
{
    uint8_t lo = dsp_get_data (s);
    uint8_t hi = dsp_get_data (s);
    return (hi << 8) | lo;
}

#define NANOSECONDS_PER_SECOND 1000000000LL
static inline uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c)
{
    union {
        uint64_t ll;
        struct {
//#ifdef HOST_WORDS_BIGENDIAN
//            uint32_t high, low;
//#else
            uint32_t low, high;
//#endif
        } l;
    } u, res;
    uint64_t rl, rh;

    u.ll = a;
    rl = (uint64_t)u.l.low * (uint64_t)b;
    rh = (uint64_t)u.l.high * (uint64_t)b;
    rh += (rl >> 32);
    res.l.high = rh / c;
    res.l.low = (((rh % c) << 32) + (rl & 0xffffffff)) / c;
    return res.ll;
}

static void complete (SB16State *s)
{
    int d0, d1, d2;
    ldebug ("complete command %#x, in_index %d, needed_bytes %d\n",
            s->cmd, s->in_index, s->needed_bytes);

    if (s->cmd > 0xaf && s->cmd < 0xd0) {
        d2 = dsp_get_data (s);
        d1 = dsp_get_data (s);
        d0 = dsp_get_data (s);

        if (s->cmd & 8) {
            dolog ("ADC params cmd = %#x d0 = %d, d1 = %d, d2 = %d\n",
                   s->cmd, d0, d1, d2);
        }
        else {
            ldebug ("cmd = %#x d0 = %d, d1 = %d, d2 = %d\n",
                    s->cmd, d0, d1, d2);
            dma_cmd (s, s->cmd, d0, d1 + (d2 << 8));
        }
    }
    else {
        switch (s->cmd) {
        case 0x04:
            s->csp_mode = dsp_get_data (s);
            s->csp_reg83r = 0;
            s->csp_reg83w = 0;
            ldebug ("CSP command 0x04: mode=%#x\n", s->csp_mode);
            break;

        case 0x05:
            s->csp_param = dsp_get_data (s);
            s->csp_value = dsp_get_data (s);
            ldebug ("CSP command 0x05: param=%#x value=%#x\n",
                    s->csp_param,
                    s->csp_value);
            break;

        case 0x0e:
            d0 = dsp_get_data (s);
            d1 = dsp_get_data (s);
            ldebug ("write CSP register %d <- %#x\n", d1, d0);
            if (d1 == 0x83) {
                ldebug ("0x83[%d] <- %#x\n", s->csp_reg83r, d0);
                s->csp_reg83[s->csp_reg83r % 4] = d0;
                s->csp_reg83r += 1;
            }
            else {
                s->csp_regs[d1] = d0;
            }
            break;

        case 0x0f:
            d0 = dsp_get_data (s);
            ldebug ("read CSP register %#x -> %#x, mode=%#x\n",
                    d0, s->csp_regs[d0], s->csp_mode);
            if (d0 == 0x83) {
                ldebug ("0x83[%d] -> %#x\n",
                        s->csp_reg83w,
                        s->csp_reg83[s->csp_reg83w % 4]);
                dsp_out_data (s, s->csp_reg83[s->csp_reg83w % 4]);
                s->csp_reg83w += 1;
            }
            else {
                dsp_out_data (s, s->csp_regs[d0]);
            }
            break;

        case 0x10:
            d0 = dsp_get_data (s);
            dolog ("cmd 0x10 d0=%#x\n", d0);
            break;

        case 0x14:
            dma_cmd8 (s, 0, dsp_get_lohi (s) + 1);
            break;

        case 0x40:
            s->time_const = dsp_get_data (s);
            ldebug ("set time const %d\n", s->time_const);
            break;

        case 0x41:
        case 0x42:
            /*
             * 0x41 is documented as setting the output sample rate,
             * and 0x42 the input sample rate, but in fact SB16 hardware
             * seems to have only a single sample rate under the hood,
             * and FT2 sets output freq with this (go figure).  Compare:
             * http://homepages.cae.wisc.edu/~brodskye/sb16doc/sb16doc.html#SamplingRate
             */
            s->freq = dsp_get_hilo (s);
            ldebug ("set freq %d\n", s->freq);
            break;

        case 0x48:
            s->block_size = dsp_get_lohi (s) + 1;
            ldebug ("set dma block len %d\n", s->block_size);
            break;

        case 0x74:
        case 0x75:
        case 0x76:
        case 0x77:
            /* ADPCM stuff, ignore */
            break;

        case 0x80:
            {
                int freq, samples, bytes;
                int64_t ticks;

                freq = s->freq > 0 ? s->freq : 11025;
                samples = dsp_get_lohi (s) + 1;
                bytes = samples << s->fmt_stereo << (s->fmt_bits == 16);
                ticks = muldiv64(bytes, NANOSECONDS_PER_SECOND, freq);
                s->mixer_regs[0x82] |= 1;
                if (ticks < NANOSECONDS_PER_SECOND / 1024) {
                    sb_set_irq(s, 1);
                } else {
                    /* Arm the deadline instead of dropping the request on
                     * the floor, which is what the missing timer used to
                     * mean: any silence period longer than a millisecond
                     * simply never interrupted. */
                    s->aux_deadline_us = time_us_32() +
                                         (uint32_t)(ticks / 1000);
                    s->aux_pending = 1;
                }
//                else {
//                    if (s->aux_ts) {
//                        timer_mod (
//                            s->aux_ts,
//                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ticks
//                            );
//                    }
//                }
                ldebug ("mix silence %d %d %" PRId64 "\n", samples, bytes, ticks);
            }
            break;

        case 0xe0:
            d0 = dsp_get_data (s);
            s->out_data_len = 0;
            ldebug ("E0 data = %#x\n", d0);
            dsp_out_data (s, ~d0);
            break;

        case 0xe2:
            d0 = dsp_get_data (s);
            s->e2_valadd += ((uint8_t) d0) ^ s->e2_valxor;
            s->e2_valxor = (s->e2_valxor >> 2) | (s->e2_valxor << 6);
            i8257_dma_write_memory(s->isa_dma, s->dma, &(s->e2_valadd),
                                   (int)i8257_dma_get_pos(s->isa_dma, s->dma), 1);
            /* One real DMA cycle, so the channel has to move with it:
             * this command exists so a driver can find the DMA channel by
             * watching the count register change. */
            i8257_dma_advance(s->isa_dma, s->dma, 1);
            break;

        case 0xe4:
            s->test_reg = dsp_get_data (s);
            break;

        case 0xf9:
            d0 = dsp_get_data (s);
            ldebug ("command 0xf9 with %#x\n", d0);
            switch (d0) {
            case 0x0e:
                dsp_out_data (s, 0xff);
                break;

            case 0x0f:
                dsp_out_data (s, 0x07);
                break;

            case 0x37:
                dsp_out_data (s, 0x38);
                break;

            default:
                dsp_out_data (s, 0x00);
                break;
            }
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "complete: unrecognized command %#x\n",
                          s->cmd);
            return;
        }
    }

    ldebug ("\n");
    s->cmd = -1;
}

static void legacy_reset (SB16State *s)
{
    s->freq = 11025;
    s->fmt_signed = 0;
    s->fmt_bits = 8;
    s->fmt_stereo = 0;
    set_audio(s, AUDIO_FORMAT_U8, s->freq, 1);
    s->voice = s;

    /* Not sure about that... */
    /* AUD_set_active_out (s->voice, 1); */
}

static void reset (SB16State *s)
{
    sb_set_irq(s, 0);
    if (s->dma_auto) {
        sb_set_irq(s, 1);
        sb_set_irq(s, 0);
    }

    s->mixer_regs[0x82] = 0;
    /*
     * A reset also cancels an interrupt that DSP command 0x80 armed but has
     * not delivered yet.  Leaving it armed is what broke Supaplex: its
     * BLASTER.SND resets the DSP several times while probing the card, and a
     * deadline that survives one of those fires afterwards with
     * mixer_regs[0x82] already cleared.  The driver acknowledges at
     * base+0x0e, that acknowledge is gated on the very bit the reset wiped,
     * so the interrupt line is never lowered - and because the 8259 is edge
     * triggered, no further Sound Blaster interrupt is ever delivered.  The
     * card goes deaf and the driver waits for a completion that cannot come.
     */
    s->aux_pending = 0;
    s->dma_auto = 0;
    s->in_index = 0;
    s->out_data_len = 0;
    s->left_till_irq = 0;
    s->needed_bytes = 0;
    s->block_size = -1;
    s->nzero = 0;
    s->highspeed = 0;
    s->v2x6 = 0;
    s->cmd = -1;
    s->time_const = -1;

    s->e2_valadd = 0xaa;
    s->e2_valxor = 0x96;

    dsp_out_data (s, 0xaa);
    speaker (s, 0);
    control (s, 0);
    legacy_reset (s);
}

void sb16_dsp_write(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = opaque;
    int iport;

    iport = nport - s->port;

    frank_diag_ev(FRANK_EV_DSP_W, (uint8_t)iport, 0, val);

    ldebug ("write %#x <- %#x\n", nport, val);
    switch (iport) {
    case 0x06:
        switch (val) {
        case 0x00:
            if (s->v2x6 == 1) {
                reset (s);
            }
            s->v2x6 = 0;
            break;

        case 0x01:
        case 0x03:              /* FreeBSD kludge */
            s->v2x6 = 1;
            break;

        case 0xc6:
            s->v2x6 = 0;        /* Prince of Persia, csp.sys, diagnose.exe */
            break;

        case 0xb8:              /* Panic */
            reset (s);
            break;

        case 0x39:
            dsp_out_data (s, 0x38);
            reset (s);
            s->v2x6 = 0x39;
            break;

        default:
            s->v2x6 = val;
            break;
        }
        break;

    case 0x0c:                  /* write data or command | write status */
/*         if (s->highspeed) */
/*             break; */

        /* Simulate DSP busy after receiving a byte.  On real hardware
         * the DSP briefly goes busy (port 0x22C bit 7=1) while it
         * processes each written byte.  Games poll for this busy→ready
         * transition before writing the next byte. */
        s->can_write = 0;

        if (s->needed_bytes == 0) {
            command (s, val);
#if 0
            if (0 == s->needed_bytes) {
                log_dsp (s);
            }
#endif
        }
        else {
            if (s->in_index == sizeof (s->in2_data)) {
                dolog ("in data overrun\n");
            }
            else {
                s->in2_data[s->in_index++] = val;
                if (s->in_index == s->needed_bytes) {
                    s->needed_bytes = 0;
                    complete (s);
#if 0
                    log_dsp (s);
#endif
                }
            }
        }
        break;

    default:
        ldebug ("(nport=%#x, val=%#x)\n", nport, val);
        break;
    }
}

uint32_t sb16_dsp_read(void *opaque, uint32_t nport)
{
    SB16State *s = opaque;
    int iport, retval, ack = 0;

    iport = nport - s->port;

    switch (iport) {
    case 0x06:                  /* reset */
        retval = 0xff;
        break;

    case 0x0a:                  /* read data */
        if (s->out_data_len) {
            retval = s->out_data[--s->out_data_len];
            s->last_read_byte = retval;
        }
        else {
            if (s->cmd != -1) {
                dolog ("empty output buffer for command %#x\n",
                       s->cmd);
            }
            retval = s->last_read_byte;
            /* goto error; */
        }
        break;

    case 0x0c:                  /* 0 can write / write-buffer status */
        /* Bit 7: DSP busy.  Brief pulse when DMA block completes.
         * Auto-clears after one read so the two-phase poll pattern
         * (wait-busy then wait-ready) works even with CLI. */
        retval = s->can_write ? 0 : 0x80;
        if (!s->can_write)
            s->can_write = 1;
        break;

    case 0x0d:                  /* timer interrupt clear */
        /* dolog ("timer interrupt clear\n"); */
        retval = 0;
        break;

    case 0x0e:                  /* data available status | irq 8 ack */
        /* Bit 7: on real SB hardware this indicates an 8-bit IRQ is
         * pending.  Games poll this port to detect DMA completion.
         * Also set when DSP has output data available. */
        retval = (s->mixer_regs[0x82] & 1) ? 0x80
               : (!s->out_data_len || s->highspeed) ? 0 : 0x80;
        if (s->mixer_regs[0x82] & 1) {
            ack = 1;
            s->mixer_regs[0x82] &= ~1;
            sb_set_irq(s, 0);
        }
        break;

    case 0x0f:                  /* irq 16 ack */
        retval = 0xff;
        if (s->mixer_regs[0x82] & 2) {
            ack = 1;
            s->mixer_regs[0x82] &= ~2;
            sb_set_irq(s, 0);
        }
        break;

    default:
        goto error;
    }

    if (!ack) {
        ldebug ("read %#x -> %#x\n", nport, retval);
    }

    frank_diag_ev(FRANK_EV_DSP_R, (uint8_t)iport, 0, (uint32_t)retval);
    return retval;

 error:
    dolog ("warning: dsp_read %#x error\n", nport);
    return 0xff;
}

/*
 * Finish a DSP 0x80 silence period.
 *
 * Command 0x80 asks the card to output silence for a given number of samples
 * and to raise its interrupt when that period is over.  QEMU arms a timer for
 * it; this port has no timer infrastructure, so the arm was left behind as a
 * `dolog("TODO: aux_ts")` and every silence period longer than a millisecond
 * simply never interrupted.
 *
 * That is not an obscure corner.  It is exactly how Tyrian 2000 tests the
 * card's interrupt line: it asks for 17 samples of silence - 1.5 ms at the
 * 11025 Hz default - waits for IRQ5, spins some 65000 times on the status
 * port, gives up after 200 ms, resets the DSP and tries once more, and then
 * refuses the card with "ERROR 253: Sound Effects disabled" even though
 * playback itself works perfectly.
 *
 * Polling a deadline from pc_step() costs a compare per step and lands within
 * one emulation step, about 2.3 ms - the same order as the period being
 * timed, and far inside any driver's timeout.
 */
void sb16_poll (SB16State *s)
{
    /* Raise on core 0 what core 1 asked for.  pc_step() calls this, so this
     * is the right side of the machine to be touching the 8259 from. */
    if (s->irq_raise_pending) {
        s->irq_raise_pending = 0;
        __dmb();
        s->set_irq(s->pic, s->irq, 1);
    }

    if (!s->aux_pending) {
        return;
    }
    if ((int32_t)(time_us_32() - s->aux_deadline_us) < 0) {
        return;
    }
    s->aux_pending = 0;
    s->can_write = 1;
    /* Raise the status bit together with the line: the acknowledge path at
     * base+0x0e refuses to lower an interrupt whose bit is clear, so one
     * raised without it could never be dismissed. */
    s->mixer_regs[0x82] |= 1;
    sb_set_irq (s, 1);
}

static void reset_mixer (SB16State *s)
{
    int i;

    memset (s->mixer_regs, 0xff, 0x7f);
    memset (s->mixer_regs + 0x83, 0xff, sizeof (s->mixer_regs) - 0x83);

    s->mixer_regs[0x02] = 4;    /* master volume 3bits */
    s->mixer_regs[0x06] = 4;    /* MIDI volume 3bits */
    s->mixer_regs[0x08] = 0;    /* CD volume 3bits */
    s->mixer_regs[0x0a] = 0;    /* voice volume 2bits */

    /* d5=input filt, d3=lowpass filt, d1,d2=input source */
    s->mixer_regs[0x0c] = 0;

    /* d5=output filt, d1=stereo switch */
    s->mixer_regs[0x0e] = 0;

    /* voice volume L d5,d7, R d1,d3 */
    s->mixer_regs[0x04] = (4 << 5) | (4 << 1);
    /* master ... */
    s->mixer_regs[0x22] = (4 << 5) | (4 << 1);
    /* MIDI ... */
    s->mixer_regs[0x26] = (4 << 5) | (4 << 1);

    for (i = 0x30; i < 0x48; i++) {
        s->mixer_regs[i] = 0x20;
    }
}

void sb16_mixer_write_indexb(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = opaque;
    (void) nport;
    s->mixer_nreg = val;
}

void sb16_mixer_write_datab(void *opaque, uint32_t nport, uint32_t val)
{
    frank_diag_ev(FRANK_EV_MIX_W, (uint8_t)((SB16State *)opaque)->mixer_nreg,
                  0, val);
    SB16State *s = opaque;

    (void) nport;
    ldebug ("mixer_write [%#x] <- %#x\n", s->mixer_nreg, val);

    switch (s->mixer_nreg) {
    case 0x00:
        reset_mixer (s);
        break;

    case 0x80:
        {
            int irq = irq_of_magic (val);
            ldebug ("setting irq to %d (val=%#x)\n", irq, val);
            if (irq > 0) {
                s->irq = irq;
            }
        }
        break;

    case 0x81:
        {
            int dma, hdma;

            dma = __builtin_ctz (val & 0xf);
            hdma = __builtin_ctz (val & 0xf0);
            if (dma != s->dma || hdma != s->hdma) {
                qemu_log_mask(LOG_GUEST_ERROR, "attempt to change DMA 8bit"
                              " %d(%d), 16bit %d(%d) (val=%#x)\n", dma, s->dma,
                              hdma, s->hdma, val);
            }
#if 0
            s->dma = dma;
            s->hdma = hdma;
#endif
        }
        break;

    case 0x82:
        qemu_log_mask(LOG_GUEST_ERROR, "attempt to write into IRQ status"
                      " register (val=%#x)\n", val);
        return;

    default:
        if (s->mixer_nreg >= 0x80) {
            ldebug ("attempt to write mixer[%#x] <- %#x\n", s->mixer_nreg, val);
        }
        break;
    }

    s->mixer_regs[s->mixer_nreg] = val;
}

uint32_t sb16_mixer_read(void *opaque, uint32_t nport)
{
    SB16State *s = opaque;

    (void) nport;
#ifndef DEBUG_SB16_MOST
    if (s->mixer_nreg != 0x82) {
        ldebug ("mixer_read[%#x] -> %#x\n",
                s->mixer_nreg, s->mixer_regs[s->mixer_nreg]);
    }
#else
    ldebug ("mixer_read[%#x] -> %#x\n",
            s->mixer_nreg, s->mixer_regs[s->mixer_nreg]);
#endif
    return s->mixer_regs[s->mixer_nreg];
}

/*
 * FRANK_SB16_DIAG_V8_10_1
 *
 * sb16_starves counts 44.1 kHz mixer ticks that found the ring empty, and it
 * cannot distinguish 7000 isolated one-sample clicks from a handful of long
 * dropouts.  Only the second is audible as stutter, and the two have opposite
 * causes, so the raw count has never been actionable.
 *
 * starve_runs / starve_max split it: runs is how many separate dropouts there
 * were, max is the longest one in mixer samples (divide by 44.1 for ms).
 *
 * The producer side is measured symmetrically.  refills and refill_bytes say
 * whether data is arriving at all and at what rate; gap_max_us is the longest
 * interval between two refills, which is a direct measure of how long core 0
 * went without servicing the DMA - the same quantity adlib_gap_max_us reports
 * for the OPL, and in capture 012 that was 18 ms.
 *
 * freq / fmtcode / rate are the missing denominators.  AUDIO_BUF_LEN is 4096
 * bytes, but that is 186 ms of 22 kHz 8-bit mono and only 23 ms of 44 kHz
 * 16-bit stereo.  Without the stream format no capture can say whether an
 * 18 ms core 0 gap is harmless or fatal, which is why the existing starve
 * count could never be diagnosed.
 */
uint32_t g_sb16_starve_runs;
uint32_t g_sb16_starve_max;
uint32_t g_sb16_refills;
uint32_t g_sb16_refill_bytes;
uint32_t g_sb16_gap_max_us;
uint32_t g_sb16_rate;
uint32_t g_sb16_freq;
uint32_t g_sb16_fmtcode;      /* fmt | stereo << 8 */
static uint32_t sb16_run_len;
static uint32_t sb16_last_fill_us;

void sb16_diag_playback_start(void)
{
    sb16_last_fill_us = time_us_32();
}

void sb16_diag_snapshot(uint32_t *out)
{
    out[0] = g_sb16_starve_runs;  g_sb16_starve_runs = 0;
    out[1] = g_sb16_starve_max;   g_sb16_starve_max = 0;
    out[2] = g_sb16_refills;      g_sb16_refills = 0;
    out[3] = g_sb16_refill_bytes; g_sb16_refill_bytes = 0;
    out[4] = g_sb16_gap_max_us;   g_sb16_gap_max_us = 0;
    /* Stream parameters are state, not events: reported, never cleared. */
    out[5] = g_sb16_rate;
    out[6] = g_sb16_freq;
    out[7] = g_sb16_fmtcode;
    sb16_last_fill_us = time_us_32();
    sb16_run_len = 0;
}

static int write_audio (SB16State *s, int nchan, int dma_pos,
                        int dma_len, int len)
{
    IsaDma *isa_dma = nchan == s->dma ? s->isa_dma : s->isa_hdma;

    int temp, net;
#if defined(BUILD_ESP32) || defined(RP2350_BUILD)
    uint8_t tmpbuf[512];
#else
    uint8_t tmpbuf[4096];
#endif

    temp = len;
    net = 0;

    while (temp) {
        int left = dma_len - dma_pos;
        int copied;
        size_t to_copy;

        to_copy = temp;
        if (left < temp)
            to_copy = left;
        if (to_copy > sizeof (tmpbuf)) {
            to_copy = sizeof (tmpbuf);
        }

        copied = i8257_dma_read_memory(isa_dma, nchan, tmpbuf, dma_pos, to_copy);

        unsigned int len = AUDIO_BUF_LEN - (s->audio_q - s->audio_p);
        if (len > AUDIO_BUF_LEN)
            len = 0;
        if (copied < len)
            len = copied;
        if (len) {
            unsigned int q = s->audio_q % AUDIO_BUF_LEN;
            if (q + len < AUDIO_BUF_LEN) {
                memcpy(s->audio_buf + q, tmpbuf, len);
            } else {
                unsigned int r = AUDIO_BUF_LEN - q;
                memcpy(s->audio_buf + q, tmpbuf, r);
                memcpy(s->audio_buf, tmpbuf + r, len - r);
            }
            s->audio_q += len;
        }
        copied = len;

        temp -= copied;
        dma_pos = (dma_pos + copied) % dma_len;
        net += copied;

        if (!copied) {
#if defined(RP2350_BUILD) || defined(BUILD_ESP32)
            // Buffer full: release DREQ to stop DMA spinning
            i8257_dma_release_DREQ(isa_dma, nchan);
#endif
            break;
        }
    }

    if (net) {
        const uint32_t now = time_us_32();
        const uint32_t gap = now - sb16_last_fill_us;
        sb16_last_fill_us = now;
        g_sb16_refills++;
        g_sb16_refill_bytes += (uint32_t)net;
        if (gap > g_sb16_gap_max_us) g_sb16_gap_max_us = gap;
        g_sb16_rate = (uint32_t)s->bytes_per_second;
        g_sb16_freq = (uint32_t)s->freq;
        g_sb16_fmtcode = (uint32_t)s->fmt |
                         ((uint32_t)(s->fmt_stereo ? 1 : 0) << 8);
    }

    return net;
}

static int SB_read_DMA (void *opaque, int nchan, int dma_pos, int dma_len)
{
    SB16State *s = opaque;
    int till, copy, written, free;

    if (s->block_size <= 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "invalid block size=%d nchan=%d"
                      " dma_pos=%d dma_len=%d\n", s->block_size, nchan,
                      dma_pos, dma_len);
        return dma_pos;
    }

    if (s->left_till_irq < 0) {
        s->left_till_irq = s->block_size;
    }

    /*
     * The previous call may have stopped exactly at terminal count and left
     * the position there on purpose, so that the guest's current-count
     * register would read 0xffff.  Wrap it here, on the way into the next
     * pass, which is the only place where wrapping is unambiguous.
     */
    if (dma_len > 0 && dma_pos >= dma_len) {
        dma_pos = 0;
    }

    if (s->voice) {
        // RP2350/ESP32: Ignore audio_free, fill buffer as much as possible
#if !defined(RP2350_BUILD) && !defined(BUILD_ESP32)
        free = s->audio_free & ~s->align;
        if ((free <= 0) || !dma_len) {
            return dma_pos;
        }
#else
        /*
         * Pace the transfer by playback rather than by buffer space.
         *
         * This was `free = dma_len`: take the whole block in one call.  The
         * block-completion interrupt then fired as soon as the bytes had been
         * *copied*, not played.  Tyrian 2000 programs single-cycle 384-byte
         * blocks at 10989 Hz - 34.9 ms of audio - and was getting its
         * interrupt 1.4 ms later, so it queued the next block twenty-five
         * times too fast, and dma_cmd8()'s single-shot flush then threw away
         * the nine tenths of each block that had not been played yet.  What
         * came out of the speakers was a rattle at the block rate, and the
         * game's own timing check refused the card outright with "ERROR 253:
         * Sound Effects disabled".
         *
         * Limiting the transfer to a fixed look-ahead makes audio_p the
         * clock: the DMA can only advance as fast as core 1 consumes, which
         * is what the card's sample clock does on real hardware.
         * i8257_dma_run() is called once per pc_step(), roughly every 2.3 ms,
         * so a 20 ms budget is topped up nearly ten times over.
         */
        free = sb16_lead_bytes (s) - (int)(s->audio_q - s->audio_p);
        free &= ~s->align;
        if ((free <= 0) || !dma_len) {
            return dma_pos;
        }
#endif
    }
    else {
        free = dma_len;
    }

    copy = free;
    till = s->left_till_irq;

#ifdef DEBUG_SB16_MOST
    dolog ("pos:%06d %d till:%d len:%d\n",
           dma_pos, free, till, dma_len);
#endif

    if (till <= copy) {
        copy = till;
    }

    written = write_audio (s, nchan, dma_pos, dma_len, copy);
    frank_diag_ev(FRANK_EV_DMA_RUN, (uint8_t)nchan, (uint16_t)written,
                  (uint32_t)dma_pos);
    /*
     * A transfer that ends exactly at the end of the buffer must be left
     * AT the end, not folded back to zero.
     *
     * i8257_channel_run() stores what this returns in regs[n].now[COUNT] and
     * declares terminal count when it equals the programmed length, and
     * i8257_read_chan() reports the guest's current-count register as
     * base[COUNT] - now[COUNT].  Real hardware counts down and reads 0xffff
     * once the last byte has moved; folding the position to 0 instead makes
     * that register read "full" and leaves the terminal-count status bit
     * clear forever - because a single-cycle Sound Blaster block always ends
     * exactly at the end of the buffer, the DSP block size and the DMA count
     * being programmed to the same length.
     *
     * Dune II is the game that shows it.  Its IRQ5 handler decides whether
     * the interrupt is really its own block completing by reading the DMA
     * count register and comparing it against 0xffff; getting 0x3b80 back it
     * concludes the transfer is still running and returns without reading
     * base+0x0e.  The card's interrupt line therefore stays asserted, the
     * edge-triggered 8259 can never see another Sound Blaster edge, and the
     * one second of speech that had already been queued is the last digital
     * audio of the session - while the FM music, which needs no DMA, plays
     * on.  That is the whole "only the first voice is heard" symptom.
     *
     * The position is normalised back to zero on the way in instead (see
     * above), which is what an auto-init transfer needs to keep going.
     */
    dma_pos += written;
    while (dma_pos > dma_len)
        dma_pos -= dma_len;
    s->left_till_irq -= written;

    if (s->left_till_irq <= 0) {
        s->mixer_regs[0x82] |= (nchan & 4) ? 2 : 1;
        sb_set_irq(s, 1);
        /* Signal DSP busy on port 0x22C so polling loops detect the
         * block completion.  Cleared on next read of port 0x22C. */
        if (s->dma_auto == 0) {
            control (s, 0);
            speaker (s, 0);
        }
    }

#ifdef DEBUG_SB16_MOST
    ldebug ("pos %5d free %5d size %5d till % 5d copy %5d written %5d size %5d\n",
            dma_pos, free, dma_len, s->left_till_irq, copy, written,
            s->block_size);
#endif

    while (s->left_till_irq <= 0) {
        s->left_till_irq = s->block_size + s->left_till_irq;
    }

    return dma_pos;
}

static int gcd(int a, int b)
{
    while (b != 0) {
        int temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

// Optimized Resamplers using fixed-point stepping (eliminates large loops for dirty frequencies)

static int resample_s16m(int16_t *out, int olen, int os,
                         int16_t *in, int ip, int ilen, int itlen, int is)
{
    // Mono Input -> Stereo Output
    // olen: output buffer size in int16_t (pairs)
    // ilen: input buffer size in int16_t (samples)
    
    // Safety check: avoid divide by zero
    if (os <= 0) return 0;
    
    uint64_t step = ((uint64_t)is << 32) / os;
    uint64_t pos = 0;
    int j = 0;
    
    while (j + 1 < olen) {
        int input_idx = pos >> 32;
        if (input_idx >= ilen) break;
        
        int16_t val = in[(ip + input_idx) % itlen];
        out[j++] = val;
        out[j++] = val;
        
        pos += step;
    }
    return pos >> 32;
}

static int resample_s16s(int16_t *out, int olen, int os,
                         int16_t *in, int ip, int ilen, int itlen, int is)
{
    // Stereo Input -> Stereo Output
    // ilen: input buffer size in int16_t (samples, L+R interleave)
    // itlen: total buffer size in int16_t

    if (os <= 0) return 0;
    
    uint64_t step = ((uint64_t)is << 32) / os;
    uint64_t pos = 0;
    int j = 0;
    
    while (j + 1 < olen) {
        int input_idx = (pos >> 32) * 2; // Pairs
        if (input_idx + 1 >= ilen) break;
        
        out[j++] = in[(ip + input_idx) % itlen];
        out[j++] = in[(ip + input_idx + 1) % itlen];
        
        pos += step;
    }
    return (pos >> 32) * 2;
}

static int resample_u16m(int16_t *out, int olen, int os,
                         int16_t *in, int ip, int ilen, int itlen, int is)
{
    // U16 Mono -> Stereo
    if (os <= 0) return 0;
    
    uint64_t step = ((uint64_t)is << 32) / os;
    uint64_t pos = 0;
    int j = 0;
    
    while (j + 1 < olen) {
        int input_idx = pos >> 32;
        if (input_idx >= ilen) break;
        
        int16_t val = in[(ip + input_idx) % itlen] - 32768;
        out[j++] = val;
        out[j++] = val;
        
        pos += step;
    }
    return pos >> 32;
}

static int resample_u16s(int16_t *out, int olen, int os,
                         int16_t *in, int ip, int ilen, int itlen, int is)
{
    // U16 Stereo -> Stereo
    if (os <= 0) return 0;
    
    uint64_t step = ((uint64_t)is << 32) / os;
    uint64_t pos = 0;
    int j = 0;
    
    while (j + 1 < olen) {
        int input_idx = (pos >> 32) * 2;
        if (input_idx + 1 >= ilen) break;
        
        out[j++] = in[(ip + input_idx) % itlen] - 32768;
        out[j++] = in[(ip + input_idx + 1) % itlen] - 32768;
        
        pos += step;
    }
    return (pos >> 32) * 2;
}

static int resample_u8m(int16_t *out, int olen, int os,
                        uint8_t *in, int ip, int ilen, int itlen, int is)
{
    // U8 Mono -> Stereo
    if (os <= 0) return 0;

    uint64_t step = ((uint64_t)is << 32) / os;
    uint64_t pos = 0;
    int j = 0;

    while (j + 1 < olen) {
        int input_idx = pos >> 32;
        if (input_idx >= ilen) break;

        uint8_t d = in[(ip + input_idx) % itlen];
        int16_t sample = (int16_t)(d - 128) << 8;
        out[j++] = sample;
        out[j++] = sample;

        pos += step;
    }
    return pos >> 32;
}

static int resample_u8s(int16_t *out, int olen, int os,
                        uint8_t *in, int ip, int ilen, int itlen, int is)
{
    // U8 Stereo -> Stereo
    if (os <= 0) return 0;

    uint64_t step = ((uint64_t)is << 32) / os;
    uint64_t pos = 0;
    int j = 0;

    while (j + 1 < olen) {
        int input_idx = (pos >> 32) * 2;
        if (input_idx + 1 >= ilen) break;

        uint8_t d1 = in[(ip + input_idx) % itlen];
        uint8_t d2 = in[(ip + input_idx + 1) % itlen];
        
        out[j++] = (int16_t)(d1 - 128) << 8;
        out[j++] = (int16_t)(d2 - 128) << 8;

        pos += step;
    }
    return (pos >> 32) * 2;
}

void sb16_audio_callback (void *opaque, uint8_t *stream, int free)
{
    SB16State *s = opaque;
    s->audio_free = free;

    // Continue playing if buffer has data, even if DMA (active_out) has stopped
    if (!s->active_out && s->audio_q == s->audio_p)
        return;

    unsigned int len = s->audio_q - s->audio_p;
    if (len > AUDIO_BUF_LEN) {
        s->audio_p = s->audio_q;
        return;
    }

    unsigned int p = s->audio_p % AUDIO_BUF_LEN;

    int i;
    switch (s->fmt) {
    case AUDIO_FORMAT_S16:
        if (s->fmt_stereo) {
            i = resample_s16s((int16_t *) stream, free / 2, SOUND_FREQUENCY,
                              (int16_t *) s->audio_buf, p / 2, len / 2,
                              AUDIO_BUF_LEN / 2, s->freq);
        } else {
            i = resample_s16m((int16_t *) stream, free / 2, SOUND_FREQUENCY,
                              (int16_t *) s->audio_buf, p / 2, len / 2,
                              AUDIO_BUF_LEN / 2, s->freq);
        }
        i *= 2;
        s->audio_p += i;
        break;
    case AUDIO_FORMAT_U16:
        if (s->fmt_stereo) {
            i = resample_u16s((int16_t *) stream, free / 2, 44100,
                              (int16_t *) s->audio_buf, p / 2, len / 2,
                              AUDIO_BUF_LEN / 2, s->freq);
        } else {
            i = resample_u16m((int16_t *) stream, free / 2, 44100,
                              (int16_t *) s->audio_buf, p / 2, len / 2,
                              AUDIO_BUF_LEN / 2, s->freq);
        }
        i *= 2;
        s->audio_p += i;
        break;
    case AUDIO_FORMAT_U8:
        if (s->fmt_stereo) {
            i = resample_u8s((int16_t *) stream, free / 2, 44100,
                             s->audio_buf, p, len, AUDIO_BUF_LEN, s->freq);
        } else {
            i = resample_u8m((int16_t *) stream, free / 2, 44100,
                             s->audio_buf, p, len, AUDIO_BUF_LEN, s->freq);
        }
        s->audio_p += i;
        break;
    default:
        dolog("bad format %d\n", s->fmt);
        s->audio_p = s->audio_q;
    }

#if defined(RP2350_BUILD) || defined(BUILD_ESP32)
        // Buffer space available: re-assert DREQ if DMA is active
        if (s->dma_running) {
            int dma = s->use_hdma ? s->hdma : s->dma;
            IsaDma *isa_dma = s->use_hdma ? s->isa_hdma : s->isa_dma;
            i8257_dma_hold_DREQ(isa_dma, dma);
        }
#endif
}

#if 0
static int sb16_post_load (void *opaque, int version_id)
{
    SB16State *s = opaque;

    if (s->voice) {
//        AUD_close_out (&s->card, s->voice);
        s->voice = NULL;
    }

    if (s->dma_running) {
        if (s->freq) {
            set_audio(s, s->fmt, s->freq, 1 << s->fmt_stereo);
            s->voice = s;
        }

        control (s, 1);
        speaker (s, s->speaker);
    }
    return 0;
}

static const MemoryRegionPortio sb16_ioport_list[] = {
    {  4, 1, 1, .write = mixer_write_indexb },
    {  5, 1, 1, .read = mixer_read, .write = mixer_write_datab },
    {  6, 1, 1, .read = dsp_read, .write = dsp_write },
    { 10, 1, 1, .read = dsp_read },
    { 12, 1, 1, .write = dsp_write },
    { 12, 4, 1, .read = dsp_read },
    PORTIO_END_OF_LIST (),
};
#endif

SB16State *sb16_new(
    int port, // 0x220
    int irq, // 5
    void *isa_dma,
    void *isa_hdma,
    void *pic,
    void (*set_irq)(void *pic, int irq, int level))
{
    SB16State *s = pcmalloc(sizeof(SB16State));
    memset(s, 0, sizeof(SB16State));
    s->voice = s;

    s->ver = 0x0405;
    s->port = port;
    s->irq = irq;
    s->dma = 1;
    s->hdma = 5;
    s->cmd = -1;

    s->isa_hdma = isa_hdma;
    s->isa_dma = isa_dma;

    s->pic = pic;
    s->set_irq = set_irq;

    s->mixer_regs[0x80] = magic_of_irq (s->irq);
    s->mixer_regs[0x81] = (1 << s->dma) | (1 << s->hdma);
    s->mixer_regs[0x82] = 2 << 5;

    s->csp_regs[5] = 1;
    s->csp_regs[9] = 0xf8;

    reset_mixer (s);
//    s->aux_ts = timer_new_ns(QEMU_CLOCK_VIRTUAL, aux_timer, s);
//    if (!s->aux_ts) {
//        error_setg(errp, "warning: Could not create auxiliary timer");
//    }

    i8257_dma_register_channel(s->isa_hdma, s->hdma, SB_read_DMA, s);

    i8257_dma_register_channel(s->isa_dma, s->dma, SB_read_DMA, s);

    s->can_write = 1;

    return s;
}

// call sb16_getsample 44100 times per second
/*
 * Playback starvation, sampled in the 44.1 kHz mixer callback on core 1.
 *
 * When active_out is set but the ring is empty, advance clamps to zero and the
 * previous sample is emitted again - the voice does not go silent, it sticks.
 * The ring is 4096 bytes but it is refilled incrementally by i8257_dma_run()
 * on core 0, so it runs near-empty rather than full and a core 0 stall shows
 * up here. minfill is the low-water mark of the same window.
 */
uint32_t g_sb16_starves;
uint32_t g_sb16_minfill = 0xffffffffu;


void sb16_starve_snapshot(uint32_t *starves, uint32_t *minfill)
{
    *starves = g_sb16_starves; g_sb16_starves = 0;
    *minfill = g_sb16_minfill; g_sb16_minfill = 0xffffffffu;
}

void __not_in_flash_func(sb16_getsample)(SB16State *s, int* r_v, int* l_v) {
    if (!s->active_out && s->audio_q == s->audio_p)
        return;

    unsigned int len = s->audio_q - s->audio_p;
    if (len > AUDIO_BUF_LEN) {
        s->audio_p = s->audio_q;
        return;
    }

    if (s->active_out) {
        if (len == 0) {
            g_sb16_starves++;
            if (sb16_run_len == 0) g_sb16_starve_runs++;
            sb16_run_len++;
            if (sb16_run_len > g_sb16_starve_max)
                g_sb16_starve_max = sb16_run_len;
        } else {
            sb16_run_len = 0;
        }
        if (len < g_sb16_minfill) g_sb16_minfill = len;
    }

    static uint32_t phase = 0;
    uint32_t step = ((uint32_t)s->freq << 16) / 44100;

    int frame_size = s->fmt_stereo ?
        (s->fmt == AUDIO_FORMAT_U8 || s->fmt == AUDIO_FORMAT_S8 ? 2 : 4) :
        (s->fmt == AUDIO_FORMAT_U8 || s->fmt == AUDIO_FORMAT_S8 ? 1 : 2);

    phase += step;
    int advance = (phase >> 16) * frame_size;
    phase &= 0xffff;

    if (advance > (int)len) advance = len;
    s->audio_p += advance;

    unsigned int p = s->audio_p % AUDIO_BUF_LEN;
    int16_t l = 0, r = 0;

    switch (s->fmt) {
    case AUDIO_FORMAT_S16:
        l = *(int16_t *)(s->audio_buf + p);
        r = s->fmt_stereo ? *(int16_t *)(s->audio_buf + (p + 2) % AUDIO_BUF_LEN) : l;
        break;
    case AUDIO_FORMAT_U16:
        l = *(int16_t *)(s->audio_buf + p) - 32768;
        r = s->fmt_stereo ? *(int16_t *)(s->audio_buf + (p + 2) % AUDIO_BUF_LEN) - 32768 : l;
        break;
    case AUDIO_FORMAT_U8:
        l = (int16_t)(s->audio_buf[p] - 128) << 8;
        r = s->fmt_stereo ? (int16_t)(s->audio_buf[(p + 1) % AUDIO_BUF_LEN] - 128) << 8 : l;
        break;
    case AUDIO_FORMAT_S8:
        l = (int16_t)(int8_t)s->audio_buf[p] << 8;
        r = s->fmt_stereo ? (int16_t)(int8_t)s->audio_buf[(p + 1) % AUDIO_BUF_LEN] << 8 : l;
        break;
    }

    if (s->dma_running) {
        int dma = s->use_hdma ? s->hdma : s->dma;
        IsaDma *isa_dma = s->use_hdma ? s->isa_hdma : s->isa_dma;
        i8257_dma_hold_DREQ(isa_dma, dma);
    }

    *l_v += l;
    *r_v += r;
}
