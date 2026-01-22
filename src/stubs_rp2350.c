/**
 * Stub implementations for disabled features on RP2350
 *
 * These functions provide no-op implementations for features that are
 * disabled in the RP2350 port (network, FPU).
 *
 * Note: Sound is now enabled with I2S output - sound stubs have been removed.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

//=============================================================================
// NE2000 Network Stubs
//=============================================================================

typedef struct NE2000State NE2000State;

NE2000State *isa_ne2000_init(int base, int irq, void *pic,
                              void (*set_irq)(void *pic, int irq, int level)) {
    (void)base;
    (void)irq;
    (void)pic;
    (void)set_irq;
    return NULL;
}

uint8_t ne2000_ioport_read(NE2000State *s, uint32_t addr) {
    (void)s;
    (void)addr;
    return 0xFF;
}

void ne2000_ioport_write(NE2000State *s, uint32_t addr, uint8_t val) {
    (void)s;
    (void)addr;
    (void)val;
}

uint16_t ne2000_asic_ioport_read(NE2000State *s, uint32_t addr) {
    (void)s;
    (void)addr;
    return 0xFFFF;
}

void ne2000_asic_ioport_write(NE2000State *s, uint32_t addr, uint16_t val) {
    (void)s;
    (void)addr;
    (void)val;
}

uint8_t ne2000_reset_ioport_read(NE2000State *s, uint32_t addr) {
    (void)s;
    (void)addr;
    return 0xFF;
}

void ne2000_reset_ioport_write(NE2000State *s, uint32_t addr, uint8_t val) {
    (void)s;
    (void)addr;
    (void)val;
}

void ne2000_step(NE2000State *s) {
    (void)s;
}

//=============================================================================
// FPU Stubs (if FPU is disabled)
//=============================================================================

#ifdef NO_FPU
typedef struct FPU FPU;

FPU *fpu_new(void) {
    return NULL;
}

int fpu_exec1(FPU *f, uint32_t op) {
    (void)f;
    (void)op;
    return 0;  // Not handled
}

int fpu_exec2(FPU *f, uint32_t op, uint32_t addr, uint8_t *mem) {
    (void)f;
    (void)op;
    (void)addr;
    (void)mem;
    return 0;  // Not handled
}
#endif
