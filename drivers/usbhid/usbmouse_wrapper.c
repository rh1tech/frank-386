/**
 * USB Mouse wrapper for murm386
 * Provides USB mouse events for the PS/2 mouse emulation
 */

#include "usbmouse_wrapper.h"

#ifdef USB_HID_ENABLED

#include "usbhid.h"

void usbmouse_init(void) {
    // USB HID is initialized by usbkbd_init(), nothing extra needed for mouse
}

void usbmouse_tick(void) {
    // USB HID polling is done by usbkbd_tick(), nothing extra needed here
}

int usbmouse_get_event(int16_t *dx, int16_t *dy, int8_t *dz, uint8_t *buttons) {
    return usbhid_get_mouse_event(dx, dy, dz, buttons);
}

int usbmouse_connected(void) {
    return usbhid_mouse_connected();
}

#endif // USB_HID_ENABLED
