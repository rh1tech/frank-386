/**
 * PS/2 Keyboard wrapper for frank-386
 * Converts HID keycodes to Linux input keycodes
 */

#ifndef PS2KBD_WRAPPER_H
#define PS2KBD_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

// Initialize PS/2 keyboard driver
// Must be called after ps2_init()
void ps2kbd_init(void);

// Service raw PS/2 input. Called periodically from the core0 timer.
void ps2kbd_tick(void);

// Get next key event
// Returns 1 if event available, 0 if queue empty
// is_down: 1=press, 0=release
// keycode: Linux input keycode for ps2_put_keycode()
int ps2kbd_get_key(int *is_down, int *keycode);

// Read an event with selectable consume/peek and oldest/newest semantics.
// Returns 1 if an event was returned, 0 if the queue is empty.
int ps2kbd_get_event(int *is_down, int *keycode, int consume, int newest);

#ifdef __cplusplus
}
#endif

#endif /* PS2KBD_WRAPPER_H */
