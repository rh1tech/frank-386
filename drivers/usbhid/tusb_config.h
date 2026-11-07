/*
 * TinyUSB Configuration for USB Host HID (Keyboard)
 * Host on the native USB port, or on PIO-USB when CFG_TUH_RPI_PIO_USB=1
 *
 * NOTE: When USB HID is enabled, USB CDC stdio is DISABLED!
 * Use UART for debug output instead.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

// MCU type
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

// RHPort number used for host: 0 = native USB port, 1 = PIO-USB
#ifndef CFG_TUH_RPI_PIO_USB
#define CFG_TUH_RPI_PIO_USB 0
#endif
#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT (CFG_TUH_RPI_PIO_USB ? 1 : 0)
#endif

// RHPort max speed: Full-Speed (12 Mbps)
#define BOARD_TUH_MAX_SPEED OPT_MODE_FULL_SPEED

// Enable Host mode (disables Device mode including CDC stdio!)
#define CFG_TUH_ENABLED 1
#define CFG_TUD_ENABLED 0

// Default is max speed that hardware controller supports
#define CFG_TUH_MAX_SPEED BOARD_TUH_MAX_SPEED

//--------------------------------------------------------------------
// HOST CONFIGURATION
//--------------------------------------------------------------------

/*
 * Host limits, sized like pico-spec's on the same boards: there a keyboard
 * that frank-386 failed to enumerate worked on both ports. Composite
 * keyboards (boot + NKRO + media keys + vendor) can carry a configuration
 * descriptor over 256 bytes and need more HID slots; when a limit is hit
 * TinyUSB abandons enumeration and the keyboard just keeps resetting.
 */
// Size of buffer for control requests (the full configuration descriptor)
#define CFG_TUH_ENUMERATION_BUFSIZE 1024

// Host event queue; hub + composite device enumeration posts bursts
#define CFG_TUH_TASK_QUEUE_SZ 64

// Max number of devices (hub + devices behind it)
#define CFG_TUH_DEVICE_MAX 6

// Enable hub support for USB keyboards connected via hub
#define CFG_TUH_HUB 1

// Max number of HID interfaces (composite keyboards use several)
#define CFG_TUH_HID 8

// Disable unused classes
#define CFG_TUH_CDC 0
#define CFG_TUH_VENDOR 0
#define CFG_TUH_MSC 0

//--------------------------------------------------------------------
// HID BUFFER SIZE
//--------------------------------------------------------------------

#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H */
