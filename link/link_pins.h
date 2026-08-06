/*
 * frank-genesis — C2 inter-processor sound link
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * link_pins.h — GPIO map for the FRANK Core 2 inter-processor link.
 *
 * Every assignment comes from the KiCad netlist via
 * frank_core2/firmware/common/frank_core2_board.h, and is shared by both
 * halves so the two builds cannot drift apart.
 *
 *   U3 = RP2350B (master, QFN-80)   U6 = RP2350 (slave, QFN-60)
 *
 * Two independent 8-bit source-synchronous buses, each with its own
 * clock and VALID strobe, plus three single-wire control signals:
 *
 *   Bus A  master -> slave   M.GPIO20..27 -> S.GPIO1..8
 *   Bus B  slave -> master   S.GPIO11..18 -> M.GPIO30..37
 *
 * Both buses share one relative layout — CLK == DATA_BASE + 8,
 * VALID == DATA_BASE + 9 — which is what lets a single pair of PIO
 * programs serve either direction on either chip.
 */
#ifndef LINK_PINS_H
#define LINK_PINS_H

/* ---- Master side (RP2350B / U3) ---- */
#define M_LINK_A_DATA_BASE   20   /* GPIO20..27, master -> slave (TX) */
#define M_LINK_A_CLK         28
#define M_LINK_A_VALID       29

#define M_LINK_B_DATA_BASE   30   /* GPIO30..37, slave -> master (RX) */
#define M_LINK_B_CLK         38
#define M_LINK_B_VALID       39

#define M_LINK_FS            40   /* frame sync / reset request, out */
#define M_LINK_DB_OUT        41   /* DB_MS, out */
#define M_LINK_DB_IN         42   /* DB_SM, in  */

/* ---- Slave side (RP2350A / U6) ---- */
#define S_LINK_A_DATA_BASE    1   /* GPIO1..8, master -> slave (RX)  */
#define S_LINK_A_CLK          9
#define S_LINK_A_VALID       10

#define S_LINK_B_DATA_BASE   11   /* GPIO11..18, slave -> master (TX) */
#define S_LINK_B_CLK         19
#define S_LINK_B_VALID       20

#define S_LINK_FS            21   /* in  */
#define S_LINK_DB_IN         22   /* DB_MS, in  */
#define S_LINK_DB_OUT        23   /* DB_SM, out */

/* ---- Slave peripherals ---- */
#define S_PSRAM_CS_PIN        0   /* U5 ESP-PSRAM64H chip select      */
#define S_LED_PIN            26   /* LD2, blue, active high           */
#define S_UART_TX_PIN        24   /* J4 header, 115200                */
#define S_UART_RX_PIN        25

/*
 * PIO instance for the link.
 *
 * Master: PIO0 is I2S audio plus the PS/2 keyboard and PIO1 is HDMI
 * video, so PIO2 is the only free instance — and it must be, because
 * bus B reaches GPIO39 and therefore needs the upper GPIO window
 * (pio_set_gpio_base(16)), which is a per-instance setting HDMI on PIO1
 * could not share.
 *
 * Slave: nothing else uses PIO, so PIO0 is free and its pins all sit
 * below 32 anyway.
 */
#ifndef LINK_PIO_MASTER
#define LINK_PIO_MASTER pio2
#endif
#ifndef LINK_PIO_SLAVE
#define LINK_PIO_SLAVE  pio0
#endif

#endif /* LINK_PINS_H */
