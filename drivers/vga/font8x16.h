#include <stdint.h>
/* Kept in flash, not .data.  As RAM it cost 4096 bytes of the 4 KB-aligned
 * gap between .data and .bss, and that gap is carved straight out of the
 * malloc heap pc_new() draws on - see the budget notes in i386.c and
 * bbprofile.h.  Nothing in this firmware programs flash, so XIP is never
 * disabled and the render ISR can read the table straight out of the
 * cache. */
extern const uint8_t font_8x16[4096];