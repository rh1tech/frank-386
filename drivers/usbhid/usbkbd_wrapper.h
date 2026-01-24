/**
 * USB Keyboard wrapper for murm386
 * Converts HID keycodes to Linux input keycodes for ps2_put_keycode()
 *
 * Same interface as ps2kbd_wrapper for easy integration
 */

#ifndef USBKBD_WRAPPER_H
#define USBKBD_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USB_HID_ENABLED

// USB HID enabled: declare functions (implemented in usbkbd_wrapper.c)

// Initialize USB keyboard driver
void usbkbd_init(void);

// Poll for keyboard events, call frequently
void usbkbd_tick(void);

// Get next key event
// Returns 1 if event available, 0 if queue empty
// is_down: 1=press, 0=release
// keycode: Linux input keycode for ps2_put_keycode()
int usbkbd_get_key(int *is_down, int *keycode);

// Check if USB keyboard is connected
int usbkbd_connected(void);

#else // !USB_HID_ENABLED

// USB HID disabled: provide inline stub functions

static inline void usbkbd_init(void) {
    // No-op when USB HID is disabled
}

static inline void usbkbd_tick(void) {
    // No-op when USB HID is disabled
}

static inline int usbkbd_get_key(int *is_down, int *keycode) {
    (void)is_down;
    (void)keycode;
    return 0;
}

static inline int usbkbd_connected(void) {
    return 0;
}

#endif // USB_HID_ENABLED

#ifdef __cplusplus
}
#endif

#endif /* USBKBD_WRAPPER_H */
