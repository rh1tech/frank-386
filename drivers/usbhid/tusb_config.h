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

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT 0
#endif
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED OPT_MODE_DEFAULT_SPEED
#endif

// Compile both roles; config.ini selects which role is initialized at boot.
#define CFG_TUH_ENABLED 1
#define CFG_TUD_ENABLED 1

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

// Default is max speed that hardware controller supports
#define CFG_TUH_MAX_SPEED BOARD_TUH_MAX_SPEED
#define CFG_TUD_MAX_SPEED BOARD_TUD_MAX_SPEED

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
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 1
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0
#define CFG_TUD_MSC_EP_BUFSIZE 512

//--------------------------------------------------------------------
// HID BUFFER SIZE
//--------------------------------------------------------------------

#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H */
