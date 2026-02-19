/**
 * murm386 - SN76489 (Tandy 3-Voice Sound) emulator
 *
 * Based on emu76489.c by Mitsutaka Okazaki 2001-2016
 * Adapted for murm386 by Mikhail Matveev, 2026
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SN76489_H
#define SN76489_H

#include <stdint.h>

typedef struct SN76489State SN76489State;

/* Allocate and initialize a new SN76489 instance */
SN76489State *sn76489_new(void);

/* Free an SN76489 instance */
void sn76489_free(SN76489State *s);

/* Write a byte to the SN76489 data port (I/O OUT) */
void sn76489_write(SN76489State *s, uint8_t val);

/**
 * Audio callback - called from mixer_callback to fill the output buffer.
 * stream : pointer to int16_t stereo interleaved output (same layout as SB16)
 * free   : byte count of the buffer (must be a multiple of 4: 2ch * 2bytes)
 */
void sn76489_callback(SN76489State *s, uint8_t *stream, int free);

#endif /* SN76489_H */
