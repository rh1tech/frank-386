#include <stdint.h>
// Place font in RAM for faster access during IRQ rendering
extern const uint8_t __attribute__((section(".data"))) font_8x16[4096];