#ifndef SB16_H
#define SB16_H

#include <stdint.h>

typedef struct SB16State SB16State;
uint32_t sb16_dsp_read(void *opaque, uint32_t nport);
void sb16_dsp_write(void *opaque, uint32_t nport, uint32_t val);
/* Complete a pending DSP 0x80 silence period. Call once per pc_step(). */
void sb16_poll(SB16State *s);
uint32_t sb16_mixer_read(void *opaque, uint32_t nport);
void sb16_mixer_write_indexb(void *opaque, uint32_t nport, uint32_t val);
void sb16_mixer_write_datab(void *opaque, uint32_t nport, uint32_t val);
void sb16_audio_callback (void *opaque, uint8_t *stream, int free);

SB16State *sb16_new(
    int port, // 0x220
    int irq, // 5
    void *isa_hdma,
    void *isa_dma,
    void *pic,
    void (*set_irq)(void *pic, int irq, int level));

void sb16_getsample(SB16State *s, int* r_v, int* l_v);

/* Read and clear the playback-starvation counters for the window since the
 * last call. minfill comes back as 0xffffffff if output was never active. */
void sb16_starve_snapshot(uint32_t *starves, uint32_t *minfill);
/* FRANK_SB16_DIAG_V8_10_1: 8 words - starve_runs, starve_max, refills,
 * refill_bytes, gap_max_us, rate, freq, fmtcode.  See sb16.c. */
void sb16_diag_snapshot(uint32_t *out);
void sb16_diag_playback_start(void);

#endif /* SB16_H */
