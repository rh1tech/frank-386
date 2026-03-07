/**
 * pico_hdmi Pin Configuration
 *
 * Default HSTX output pin mapping for the pico_hdmi library.
 */

#ifndef HSTX_PINS_H
#define HSTX_PINS_H

// =============================================================================
// DVI/HSTX Output Pins (GPIO 12-19)
// =============================================================================
// These are the default pins for the RP2350's HSTX peripheral.
#define PIN_HSTX_CLK 12 // Clock pair base (12=CLK-, 13=CLK+)
#define PIN_HSTX_D0 14  // Data 0 pair base (14=D0-, 15=D0+)
#define PIN_HSTX_D1 16  // Data 1 pair base (16=D1-, 17=D1+)
#define PIN_HSTX_D2 18  // Data 2 pair base (18=D2-, 19=D2+)

#endif // HSTX_PINS_H
