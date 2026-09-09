/**
 * SD Card Driver for RP2350
 *
 * Supports SPI mode via hardware SPI or PIO.
 * Pin configuration is set via board_config.h (M1 or M2 layout).
 */

#ifndef _SDCARD_H_
#define _SDCARD_H_

// Include board configuration for pin definitions
#ifdef RP2350_BUILD
#include "board_config.h"
#endif

/* SPI pin assignment - defaults if not set by board_config.h */

#ifndef SDCARD_SPI_BUS
#define SDCARD_SPI_BUS spi0
#endif

/* Default pin assignments for Pico Wireless (if not using M1/M2 board) */
#ifndef SDCARD_PIN_SPI0_CS
#define SDCARD_PIN_SPI0_CS     22
#endif

#ifndef SDCARD_PIN_SPI0_SCK
#define SDCARD_PIN_SPI0_SCK    18
#endif

#ifndef SDCARD_PIN_SPI0_MOSI
#define SDCARD_PIN_SPI0_MOSI   19
#endif

#ifndef SDCARD_PIN_SPI0_MISO
#define SDCARD_PIN_SPI0_MISO   16
#endif

/* Recalculate the active SD SPI clock after clk_sys/clk_peri changes. */
void sdcard_reclock(void);

#endif // _SDCARD_H_
