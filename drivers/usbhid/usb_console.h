/*
 * USB CDC console on the native USB port next to the PIO-USB host
 * (USB_CDC_CONSOLE builds: Z2 with USB HID on PIO-USB). See usb_console.c.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef USB_CONSOLE_H
#define USB_CONSOLE_H

#include <stdbool.h>

/* Brings up the device stack on the native port and adds it to stdio.
 * Call on core 0 (its 1 ms timer runs there), after stdio_init_all(). */
void usb_console_init(void);

/* A terminal has the port open (DTR) */
bool usb_console_connected(void);

#endif /* USB_CONSOLE_H */
