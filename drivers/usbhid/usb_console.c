/*
 * USB CDC console on the native USB port, while the USB host (keyboard,
 * mouse, gamepad) runs on PIO-USB: TinyUSB in both roles, as its
 * dual/host_hid_to_device_cdc example.
 *
 * The SDK's pico_stdio_usb compiles to nothing once TinyUSB's host library
 * is linked (stdio_usb.c is wrapped in #ifndef LIB_TINYUSB_HOST), so this
 * is a stdio driver of its own: printf/getchar reach it with the UART's.
 * TinyUSB is only ever entered on core 0: its OS-less queue code disables
 * and enables the USB interrupt on the calling core, and on core 1 that
 * would turn the interrupt on there too (it took the HDMI line IRQ down).
 * A 1 ms timer on core 0 runs the device stack and sends what core 1
 * printed, through a lock-free ring: core 1 never holds its interrupts off
 * for the console (the HDMI line IRQ is on core 1 and must not be late).
 * A spin lock keeps core 0's own callers (thread and timer) apart.
 *
 * SPDX-License-Identifier: MIT
 */

#include "usb_console.h"

#include <string.h>

#include "hardware/sync.h"
#include "pico/stdio/driver.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"

#if USB_CDC_CONSOLE

//--------------------------------------------------------------------
// Descriptors: one CDC ACM function
//--------------------------------------------------------------------

#define USBD_VID 0x2E8A /* Raspberry Pi */
#define USBD_PID 0x000A /* Raspberry Pi Pico SDK CDC UART, as pico_stdio_usb */

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };
#define EP_CDC_NOTIF 0x81
#define EP_CDC_OUT 0x02
#define EP_CDC_IN 0x82
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const uint8_t desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 250),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
};

static const char *const desc_strings[] = {"Raspberry Pi", "frank-386", NULL /* serial */, "Console"};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_config;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc[32];
    static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    int n;
    if (index == 0) {
        desc[1] = 0x0409; /* English */
        n = 1;
    } else {
        if (index >= sizeof(desc_strings) / sizeof(desc_strings[0])) return NULL;
        const char *str = desc_strings[index];
        if (!str) {
            if (!serial[0]) pico_get_unique_board_id_string(serial, sizeof(serial));
            str = serial;
        }
        n = (int)strlen(str);
        if (n > 31) n = 31;
        for (int i = 0; i < n; i++) desc[1 + i] = (uint8_t)str[i];
    }
    desc[0] = (uint16_t)(TUSB_DESC_STRING << 8 | (2 * n + 2));
    return desc;
}

//--------------------------------------------------------------------
// The device stack, under a lock shared by both cores
//--------------------------------------------------------------------

static spin_lock_t *lock;
static repeating_timer_t task_timer;

/* core 1's output, sent by core 0: one writer (core 1, ring_head), one
 * reader (core 0, ring_tail) */
#define RING 512 /* a power of 2 */
static char ring[RING];
static volatile uint32_t ring_head, ring_tail;

/* core 0, the lock held: the ring into the CDC FIFO, as it has room */
static void drain_ring(void) {
    uint32_t t = ring_tail, h = ring_head;
    __dmb();
    while (t != h) {
        uint32_t n = h - t, at = t & (RING - 1);
        if (n > RING - at) n = RING - at;
        n = tud_cdc_write(ring + at, n);
        if (!n) break;
        t += n;
    }
    __dmb();
    ring_tail = t;
    tud_cdc_write_flush();
}

static bool task_tick(repeating_timer_t *t) {
    (void)t;
    uint32_t save = spin_lock_blocking(lock);
    tud_task();
    if (tud_cdc_connected()) drain_ring();
    else ring_tail = ring_head; /* no terminal: dropped */
    spin_unlock(lock, save);
    return true;
}

/* Waits for room up to a few ms while a terminal is attached; drops the
 * output when none is (a printf must never hang on a missing host). */
static void out_chars(const char *buf, int len) {
    if (get_core_num() != 0) {
        uint32_t h = ring_head;
        for (int i = 0; i < len && h - ring_tail < RING; i++)
            ring[h++ & (RING - 1)] = buf[i];
        __dmb();
        ring_head = h;
        return;
    }
    absolute_time_t deadline = make_timeout_time_ms(20);
    while (len > 0) {
        uint32_t save = spin_lock_blocking(lock);
        if (!tud_cdc_connected()) {
            spin_unlock(lock, save);
            return;
        }
        drain_ring(); /* core 1's first, as it came first */
        uint32_t n = tud_cdc_write(buf, (uint32_t)len);
        tud_cdc_write_flush();
        if (!n) tud_task(); /* full: let the stack move it along */
        spin_unlock(lock, save);
        buf += n;
        len -= (int)n;
        if (!n && time_reached(deadline)) return;
    }
}

static void out_flush(void) {
    if (get_core_num() != 0) return; /* core 0's timer sends it */
    uint32_t save = spin_lock_blocking(lock);
    tud_cdc_write_flush();
    spin_unlock(lock, save);
}

static int in_chars(char *buf, int len) {
    if (get_core_num() != 0) return PICO_ERROR_NO_DATA;
    uint32_t save = spin_lock_blocking(lock);
    int n = tud_cdc_available() ? (int)tud_cdc_read(buf, (uint32_t)len) : 0;
    spin_unlock(lock, save);
    return n ? n : PICO_ERROR_NO_DATA;
}

static stdio_driver_t cdc_driver = {
    .out_chars = out_chars,
    .out_flush = out_flush,
    .in_chars = in_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = true,
#endif
};

void usb_console_init(void) {
    lock = spin_lock_init(spin_lock_claim_unused(true));
    tud_init(BOARD_TUD_RHPORT);
    add_repeating_timer_us(-1000, task_tick, NULL, &task_timer);
    stdio_set_driver_enabled(&cdc_driver, true);
}

bool usb_console_connected(void) {
    if (get_core_num() != 0) return false;
    uint32_t save = spin_lock_blocking(lock);
    bool c = tud_cdc_connected();
    spin_unlock(lock, save);
    return c;
}

#endif /* USB_CDC_CONSOLE */
