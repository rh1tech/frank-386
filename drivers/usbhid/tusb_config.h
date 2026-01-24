/*
 * TinyUSB Configuration for USB Host HID (Keyboard)
 * Uses native USB port for Host mode
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

// RHPort number used for host (0 = native USB port)
#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT 0
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

// Size of buffer for control requests
#define CFG_TUH_ENUMERATION_BUFSIZE 256

// Max number of devices (hub + devices behind it)
#define CFG_TUH_DEVICE_MAX 4

// Enable hub support for USB keyboards connected via hub
#define CFG_TUH_HUB 1

// Max number of HID interfaces
#define CFG_TUH_HID 4

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
