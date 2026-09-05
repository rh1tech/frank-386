#ifndef ADLIB_H
#define ADLIB_H

#include <stdint.h>

#define FLOAT float

#define ADLIB_BATCH_SIZE 64

/*
 * The ring is addressed per sample, not per batch.
 *
 * It used to hand core 1 whole batches of 64 samples, each rendered from one
 * snapshot of the OPL registers - 1.45 ms of audio per snapshot. That is fine
 * for FM music, where a register write is a note and notes are tens of
 * milliseconds apart, and fatal for the other thing games do with this chip:
 * Electro Body (and it is not alone) sets up one channel and then writes a
 * 6-bit sample into register 0x40, the total-level attenuator, on every IRQ0
 * at 8.5 kHz. Batching threw away ten of every eleven of those samples, which
 * is why its music came back as a slow rattle rather than as music.
 *
 * Same 512 bytes, addressed one sample at a time, so a register write can
 * take effect between any two samples.
 */
#define ADLIB_RING_SAMPLES  (ADLIB_NBUF * ADLIB_BATCH_SIZE)   /* power of two */

/*
 * How far ahead of core 1 core 0 renders.
 *
 * This is the whole buffer minus a little slack, because it is also the
 * stall the ring can absorb: FatFS issues eight ~475 us disk_read() calls for
 * one cluster and core 0 is gone for the duration. 224 samples is 5.08 ms.
 * It is also a constant delay on every OPL write, and 5 ms of latency is
 * inaudible - what matters for a sampled stream is not when a write lands
 * but that the interval between two writes maps to the right number of
 * samples, and chasing a fixed lead ahead of the consumer's own position is
 * exactly what guarantees that.
 */
#define ADLIB_LEAD_SAMPLES  224

/* Output samples per call into the emu8950 renderer.  It lives in flash, so
 * every entry evicts XIP lines the interpreter is using and the call is worth
 * amortising; 32 keeps the two int32 scratch buffers below to 304 bytes of
 * stack, close to the 256 the old whole-batch render needed. */
#define ADLIB_RENDER_CHUNK  32

/*
 * Minimum backlog before the *periodic* producer bothers to run.
 *
 * adlib_core0() is called every 256 guest instructions, about 160 us, by
 * which time only seven samples are due - so rendering on every call paid the
 * flash-entry cost ten times more often than the whole-batch renderer it
 * replaced, and measured 20.2% of core 0 during Tyrian 2000 against the 12%
 * the old one cost.  Waiting for a batch's worth restores that, and costs
 * nothing in accuracy: fine granularity is only needed when the guest writes
 * an OPL register, and adlib_write() asks for it explicitly by passing 1.
 */
#define ADLIB_PERIODIC_MIN  64

/*
 * The chip is not clocked at the output rate, so its samples have to be
 * resampled rather than just handed over.
 *
 * A YM3812 driven by the standard 3.579545 MHz colour-burst crystal produces
 * one sample every 72 clocks, i.e. 49716 Hz, and emu8950 built with
 * EMU8950_NO_RATECONV computes its phase increments from fnum, block and ML
 * alone - neither opl->clk nor opl->rate appears anywhere in the generator.
 * So it always emits 49716 Hz worth of audio no matter what rate it was
 * constructed with, and playing that out at 44100 Hz stretched every note by
 * 49716/44100: all FM music was 11.3% flat and 11.3% slow, about two
 * semitones, envelopes and vibrato included.
 *
 * The fix belongs here and not in the I2S clock. Reconfiguring the output to
 * 49716 Hz was tried on hardware and the board came up with a black screen -
 * it raises the rate of the mixer callback on core 1 by the same 12.7%, on a
 * board already at 504 MHz driving PSRAM, HDMI and an SD card. Resampling
 * costs core 0 about 13% more OPL work instead, roughly 1% of core 0, and
 * leaves the audio ISR exactly where it was.
 *
 * Q16 step: 49716 * 65536 / 44100 = 73882.3, so 73882 is 3 ppm slow.
 */
#define ADLIB_RS_STEP  73882u

/* Chip samples one render pass can consume: (0xffff + 32 * STEP) >> 16. */
#define ADLIB_RS_MAX   38

/*
 * Number of batches in the ring.
 *
 * Depth, not batch size, is what had to grow. Measured on Dune II and Draci
 * historie, a two-batch ping-pong holds 2 x 64 samples = 2.9 ms and 15-22% of
 * every OPL sample came back as silence: FatFS issues eight disk_read() calls
 * for one cluster, roughly 475 us each, and those 3.8 ms run straight through
 * a 2.9 ms buffer without any single read looking slow.
 *
 * 4 x 64 = 256 samples = 5.8 ms. Twice the old depth, and it clears the
 * 3.8 ms cluster burst that does the damage, which 2.9 ms did not.
 *
 * Four is not a design choice, it is the budget. int16 halves the bytes per
 * sample, so four batches occupy exactly the 512 bytes the old int32
 * ping-pong of two already used: the depth doubles for zero extra heap.
 * NBUF 16 and NBUF 8 were both tried on hardware and both died with
 * "Out of memory" in pc_new() - AdlibState is malloc'd from that heap, the
 * boot log showed only 43616 bytes free, and the margin turned out to be
 * under a kilobyte. Anything deeper needs heap freed elsewhere first.
 *
 * Keeping the batch at 64 keeps each OPL render short, so core 0 still hands
 * the interpreter back control roughly every 90 us instead of disappearing
 * into one long render.
 */
#define ADLIB_NBUF 4

typedef struct AdlibState AdlibState;

void adlib_write(void *opaque, uint32_t nport, uint32_t val);
uint32_t adlib_read(void *opaque, uint32_t nport);
AdlibState *adlib_new();
// call it 44100 times per sec from timer on core1 (ISR, so should be fast)
int16_t adlib_getsample(AdlibState *s);
// call it from main cycle on core0
void adlib_core0(AdlibState *s);

/* Render forward until the ring is ADLIB_LEAD_SAMPLES ahead of core 1, but
 * only if at least min_samples are owed.  Core 0 only.  adlib_write() passes
 * 1 so a register write takes effect between samples rather than
 * retroactively; the interpreter loop passes ADLIB_PERIODIC_MIN so it renders
 * in worthwhile batches. */
void adlib_produce(AdlibState *s, uint32_t min_samples);

/* Read and clear the count of times core 1 asked for a sample and core 0 had
 * refilled no batch. The ring holds ADLIB_NBUF x ADLIB_BATCH_SIZE
 * samples, so a non-zero count means core 0 stalled for longer than that. */
uint32_t adlib_underruns(AdlibState *s);

/* Read and clear the producer-gap counters for the window since the last
 * call: how often adlib_core0() ran, the longest interval between two calls,
 * how many intervals exceeded the ring's depth, and the total time by which
 * they exceeded it - i.e. silence no buffer of this depth could have hidden. */
void adlib_gap_snapshot(uint32_t *calls, uint32_t *max_us,
                        uint32_t *over, uint32_t *lost_us);

#endif /* ADLIB_H */
