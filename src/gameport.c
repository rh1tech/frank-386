/**
 * frank-386 — PC analog game port (joystick) emulation. See gameport.h.
 */
#include "gameport.h"
#include "pc.h"   /* get_uticks */

/*
 * One-shot durations in microseconds.
 *
 * Real hardware is t = 24.2us + 0.011 * R, with R the pot resistance,
 * 0 to 100k on a standard stick: roughly 24us at one extreme and 1124us
 * at the other. A digital pad only ever produces three positions, so
 * only three values are needed, spread across that range so that games
 * which auto-calibrate see a sane full-scale deflection.
 */
#define GP_US_MIN     24u
#define GP_US_CENTRE  562u
#define GP_US_MAX     1100u

static uint32_t gp_start_us;      /* when the one-shots were fired      */
static uint32_t gp_dur_x = GP_US_CENTRE;
static uint32_t gp_dur_y = GP_US_CENTRE;
static uint8_t  gp_buttons;       /* bit 0 = button 1, bit 1 = button 2 */
static bool     gp_running;

/*
 * Probe counters, readable over SWD. Proof that the guest actually
 * reaches the port, which is otherwise invisible from outside - a game
 * polling for buttons generates hundreds of thousands of reads and no
 * writes at all, and without these there is no way to tell that from a
 * port nothing ever touches.
 *
 * Gated: gameport_read() is called a quarter of a million times in a
 * couple of minutes of Wolf3D, so normal builds should not pay for it.
 */
#if GAMEPORT_TEST
volatile uint32_t g_gp[4] __attribute__((used));   /* writes, reads, x_high, y_high */
#define GP_COUNT(i) (g_gp[i]++)
#else
#define GP_COUNT(i) ((void)0)
#endif

static uint32_t gp_duration(int axis) {
    if (axis < 0) return GP_US_MIN;
    if (axis > 0) return GP_US_MAX;
    return GP_US_CENTRE;
}

void gameport_set(int x, int y, uint8_t buttons) {
    gp_dur_x = gp_duration(x);
    gp_dur_y = gp_duration(y);
    gp_buttons = buttons;
}

void gameport_write(void) {
    GP_COUNT(0);
    gp_start_us = get_uticks();
    gp_running = true;
}

uint8_t gameport_read(void) {
    GP_COUNT(1);
    /* Buttons are active low and are readable without firing the
     * one-shots — plenty of games poll only the buttons. */
    uint8_t v = 0xf0;
    if (gp_buttons & 1u) v &= (uint8_t)~0x10u;
    if (gp_buttons & 2u) v &= (uint8_t)~0x20u;

    if (gp_running) {
        const uint32_t elapsed = get_uticks() - gp_start_us;
        if (elapsed < gp_dur_x) { v |= 0x01u; GP_COUNT(2); }
        if (elapsed < gp_dur_y) { v |= 0x02u; GP_COUNT(3); }
        /* Joystick B is not present: its bits stay low, which is how a
         * one-stick adapter reads. Games probing for a second stick see
         * an immediate timeout and move on. */
        if (elapsed >= gp_dur_x && elapsed >= gp_dur_y)
            gp_running = false;
    }
    return v;
}
