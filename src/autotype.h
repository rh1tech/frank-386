/**
 * frank-386 — scripted keystroke injection, for unattended benchmarking.
 *
 * SPDX-License-Identifier: MIT
 *
 * A benchmark that needs a human to launch it is not a benchmark you can
 * A/B twenty builds against. This types a fixed command into the guest a
 * few seconds after boot, so a build can be flashed and measured with no
 * one at the keyboard.
 *
 * It also fixes a subtler problem: an idle DOS prompt spends most of its
 * time in HLT, so "MIPS at the prompt" mostly measures how fast the
 * emulator idles. Driving the guest into a real workload — Wolf3D's
 * attract demo loops forever without input — is what makes the
 * throughput figure mean anything.
 *
 * Build with e.g.
 *   ./build.sh -C2 --vga --autotype 'cd wolf3d\rwolf3d\r~~~~ \r'
 * where \r is Enter and '~' is a 3-second pause — needed because a game
 * takes seconds to load and anything typed meanwhile is lost. Only the
 * characters DOS command lines need are mapped; anything else is
 * skipped rather than mistyped.
 */
#ifndef AUTOTYPE_H
#define AUTOTYPE_H

#ifdef AUTOTYPE_STR

/* Call once per main-loop iteration; cheap and self-limiting. */
void autotype_tick(void);

#else

static inline void autotype_tick(void) {}

#endif

#endif /* AUTOTYPE_H */
