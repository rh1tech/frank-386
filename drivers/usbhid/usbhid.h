/*
 * USB HID Host Driver Header for murm386
 * Provides keyboard input via USB Host
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USBHID_H
#define USBHID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// API Functions
//--------------------------------------------------------------------

/**
 * Initialize USB Host HID driver
 * Call this during system initialization
 */
void usbhid_init(void);

/**
 * Poll USB Host for events
 * Must be called periodically (e.g., in main loop)
 */
void usbhid_task(void);

/**
 * Check if a USB keyboard is connected
 * @return Non-zero if keyboard connected
 */
int usbhid_keyboard_connected(void);

/**
 * Get pending key action
 * @param keycode HID keycode of the key (0x04-0x65, 0xE0-0xE7 for modifiers)
 * @param down Non-zero if key pressed, 0 if released
 * @return Non-zero if action available, 0 if queue empty
 */
int usbhid_get_key_action(uint8_t *keycode, int *down);

#ifdef __cplusplus
}
#endif

#endif /* USBHID_H */
