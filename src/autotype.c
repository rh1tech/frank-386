/**
 * frank-386 — scripted keystroke injection. See autotype.h.
 */
#include "autotype.h"

#ifdef AUTOTYPE_STR

#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pc.h"
#include "i8042.h"

extern PC *pc;

/* Linux input keycodes, as ps2_put_keycode() expects. */
#define K_ESC 1
#define K_ENTER 28
#define K_SPACE 57
#define K_BACKSLASH 43
#define K_DOT 52
#define K_MINUS 12
#define K_UP    103
#define K_DOWN  108
#define K_LEFT  105
#define K_RIGHT 106

static const uint8_t k_digit[10] = { 11, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

/* a..z in keycode order */
static const uint8_t k_alpha[26] = {
    30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
    49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44
};

static int ascii_to_key(char c) {
    if (c >= 'a' && c <= 'z') return k_alpha[c - 'a'];
    if (c >= 'A' && c <= 'Z') return k_alpha[c - 'A'];
    if (c >= '0' && c <= '9') return k_digit[c - '0'];
    switch (c) {
    case '\r': case '\n': return K_ENTER;
    case ' ':  return K_SPACE;
    case '\\': return K_BACKSLASH;
    case '.':  return K_DOT;
    case '-':  return K_MINUS;
    case 0x1b: return K_ESC;
    /* Arrow keys, for driving in-game menus. Deliberately punctuation no
     * DOS command line needs - not letters, which would shadow real
     * input ('v' for Down would break typing "vc"). */
    case '^': return K_UP;
    case '!': return K_DOWN;
    case '<': return K_LEFT;
    case '>': return K_RIGHT;
    default:   return -1;   /* skip rather than mistype */
    }
}

/*
 * Timing. The guest needs to reach a command prompt before anything is
 * typed, and DOS drops keys pressed faster than it polls, so keys go in
 * slowly and with a real down/up pair each.
 */
#define AT_START_DELAY_US  15000000u   /* let BIOS, DOS and the shell settle */
#define AT_KEY_PERIOD_US      90000u   /* ~11 keys/sec */

void autotype_tick(void) {
    static const char script[] = AUTOTYPE_STR;
    static uint32_t idx;
    static uint64_t next_us;
    static bool down_sent;
    static bool done;

    if (done || !pc || !pc->kbd) return;

    const uint64_t now = time_us_64();
    if (next_us == 0) { next_us = now + AT_START_DELAY_US; return; }
    if (now < next_us) return;

    if (idx >= sizeof(script) - 1) { done = true; return; }

    /* '~' is a pause, not a keystroke. Loading a game takes seconds and
     * the keys that follow it would otherwise all be typed into a
     * program that is not listening yet. */
    if (script[idx] == '~') {
        idx++;
        next_us = now + 3000000u;
        return;
    }

    const int key = ascii_to_key(script[idx]);
    if (key < 0) { idx++; return; }

    if (!down_sent) {
        ps2_put_keycode(pc->kbd, 1, key);
        down_sent = true;
        next_us = now + 20000u;          /* hold briefly */
    } else {
        ps2_put_keycode(pc->kbd, 0, key);
        down_sent = false;
        idx++;
        next_us = now + AT_KEY_PERIOD_US;
    }
}

#endif /* AUTOTYPE_STR */
