/**
 * USB Mouse wrapper for murm386
 * Provides USB mouse events for the PS/2 mouse emulation
 *
 * Same pattern as usbkbd_wrapper for easy integration
 */

#ifndef USBMOUSE_WRAPPER_H
#define USBMOUSE_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USB_HID_ENABLED

// USB HID enabled: declare functions (implemented in usbmouse_wrapper.c)

// Initialize USB mouse driver (called after usbkbd_init which initializes USB HID)
void usbmouse_init(void);

// Poll for mouse events - no-op, polling done by usbkbd_tick()
void usbmouse_tick(void);

// Get accumulated mouse event
// Returns 1 if event available, 0 if no mouse activity
// dx, dy: movement delta
// dz: scroll wheel delta
// buttons: button state (bit 0=left, 1=right, 2=middle)
int usbmouse_get_event(int16_t *dx, int16_t *dy, int8_t *dz, uint8_t *buttons);

// Check if USB mouse is connected
int usbmouse_connected(void);

#else // !USB_HID_ENABLED

// USB HID disabled: provide inline stub functions

static inline void usbmouse_init(void) {
    // No-op when USB HID is disabled
}

static inline void usbmouse_tick(void) {
    // No-op when USB HID is disabled
}

static inline int usbmouse_get_event(int16_t *dx, int16_t *dy, int8_t *dz, uint8_t *buttons) {
    (void)dx;
    (void)dy;
    (void)dz;
    (void)buttons;
    return 0;
}

static inline int usbmouse_connected(void) {
    return 0;
}

#endif // USB_HID_ENABLED

#ifdef __cplusplus
}
#endif

#endif /* USBMOUSE_WRAPPER_H */
