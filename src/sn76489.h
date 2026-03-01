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

void sn76489_reset(void);
void sn76489_out(const uint16_t register_value);
int16_t sn76489_sample(void);

#endif /* SN76489_H */
