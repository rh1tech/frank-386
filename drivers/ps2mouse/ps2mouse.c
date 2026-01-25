/**
 * murm386 - i386 PC Emulator for RP2350
 *
 * PS/2 Mouse Driver - GPIO interrupt-based PS/2 mouse interface
 * Based on DnCraptor's implementation from murmdoom
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#include "ps2mouse.h"
#include "board_config.h"
#include <pico/stdlib.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <string.h>
#include <stdio.h>

// Use GPIO interrupt-based approach (PIO is used by keyboard)

#define MOUSE_CLK_PIN   PS2_MOUSE_CLK
#define MOUSE_DATA_PIN  PS2_MOUSE_DATA

// PS/2 Mouse Commands
#define MOUSE_CMD_RESET           0xFF
#define MOUSE_CMD_RESEND          0xFE
#define MOUSE_CMD_SET_DEFAULTS    0xF6
#define MOUSE_CMD_DISABLE_DATA    0xF5
#define MOUSE_CMD_ENABLE_DATA     0xF4
#define MOUSE_CMD_SET_SAMPLE_RATE 0xF3
#define MOUSE_CMD_GET_DEVICE_ID   0xF2
#define MOUSE_CMD_SET_REMOTE_MODE 0xF0
#define MOUSE_CMD_SET_WRAP_MODE   0xEE
#define MOUSE_CMD_RESET_WRAP_MODE 0xEC
#define MOUSE_CMD_READ_DATA       0xEB
#define MOUSE_CMD_SET_STREAM_MODE 0xEA
#define MOUSE_CMD_STATUS_REQUEST  0xE9
#define MOUSE_CMD_SET_RESOLUTION  0xE8
#define MOUSE_CMD_SET_SCALING_2_1 0xE7
#define MOUSE_CMD_SET_SCALING_1_1 0xE6

// Mouse responses
#define MOUSE_RESP_ACK            0xFA
#define MOUSE_RESP_BAT_OK         0xAA
#define MOUSE_RESP_ERROR          0xFC

// Internal state
static volatile int mouse_bitcount = 0;
static volatile uint8_t mouse_incoming = 0;
static volatile uint32_t mouse_prev_us = 0;

// Packet buffer
#define MOUSE_BUFFER_SIZE 16
static volatile uint8_t mouse_buffer[MOUSE_BUFFER_SIZE];
static volatile uint8_t mouse_buffer_head = 0;
static volatile uint8_t mouse_buffer_tail = 0;

// Mouse state
static ps2mouse_state_t mouse_state = {0};
static volatile int mouse_irq_enabled = 0;

// Packet parsing state
static uint8_t packet_data[4];
static uint8_t packet_index = 0;
static uint8_t packet_size = 3;  // 3 for standard, 4 for IntelliMouse

// Previous button state for detecting changes
static uint8_t prev_buttons = 0;

//-----------------------------------------------------------------------------
// Low-level GPIO helpers
//-----------------------------------------------------------------------------

static inline void mouse_clock_lo(void) {
    gpio_set_dir(MOUSE_CLK_PIN, GPIO_OUT);
    gpio_put(MOUSE_CLK_PIN, 0);
}

static inline void mouse_clock_hi(void) {
    gpio_set_dir(MOUSE_CLK_PIN, GPIO_OUT);
    gpio_put(MOUSE_CLK_PIN, 1);
}

static inline int mouse_clock_in(void) {
    gpio_set_dir(MOUSE_CLK_PIN, GPIO_IN);
    asm volatile("nop");
    return gpio_get(MOUSE_CLK_PIN);
}

static inline void mouse_data_lo(void) {
    gpio_set_dir(MOUSE_DATA_PIN, GPIO_OUT);
    gpio_put(MOUSE_DATA_PIN, 0);
}

static inline void mouse_data_hi(void) {
    gpio_set_dir(MOUSE_DATA_PIN, GPIO_OUT);
    gpio_put(MOUSE_DATA_PIN, 1);
}

static inline int mouse_data_in(void) {
    gpio_set_dir(MOUSE_DATA_PIN, GPIO_IN);
    asm volatile("nop");
    return gpio_get(MOUSE_DATA_PIN);
}

static inline void mouse_inhibit(void) {
    mouse_clock_lo();
    mouse_data_hi();
}

static inline void mouse_idle(void) {
    mouse_clock_hi();
    mouse_data_hi();
}

//-----------------------------------------------------------------------------
// Wait helpers with timeout
//-----------------------------------------------------------------------------

static inline uint16_t wait_clock_lo(uint16_t us) {
    while (mouse_clock_in() && us) {
        busy_wait_us_32(1);
        us--;
    }
    return us;
}

static inline uint16_t wait_clock_hi(uint16_t us) {
    while (!mouse_clock_in() && us) {
        busy_wait_us_32(1);
        us--;
    }
    return us;
}

static inline uint16_t wait_data_lo(uint16_t us) {
    while (mouse_data_in() && us) {
        busy_wait_us_32(1);
        us--;
    }
    return us;
}

static inline uint16_t wait_data_hi(uint16_t us) {
    while (!mouse_data_in() && us) {
        busy_wait_us_32(1);
        us--;
    }
    return us;
}

//-----------------------------------------------------------------------------
// IRQ enable/disable
//-----------------------------------------------------------------------------

static inline void mouse_irq_on(void) {
    gpio_set_dir(MOUSE_CLK_PIN, GPIO_IN);
    gpio_set_dir(MOUSE_DATA_PIN, GPIO_IN);
    gpio_set_irq_enabled(MOUSE_CLK_PIN, GPIO_IRQ_EDGE_FALL, 1);
    mouse_irq_enabled = 1;
}

static inline void mouse_irq_off(void) {
    gpio_set_irq_enabled(MOUSE_CLK_PIN, GPIO_IRQ_EDGE_FALL, 0);
    mouse_irq_enabled = 0;
}

//-----------------------------------------------------------------------------
// Send a byte to the mouse (host-to-device)
//-----------------------------------------------------------------------------

static void mouse_send_byte(uint8_t data) {
    int parity = 1;

    mouse_irq_off();

    // Inhibit communication
    mouse_inhibit();
    busy_wait_us_32(200);

    // Request-to-send: pull data low, release clock
    mouse_data_lo();
    busy_wait_us_32(200);
    mouse_clock_hi();

    // Wait for device to pull clock low
    if (!wait_clock_lo(15000)) {
        mouse_idle();
        mouse_irq_on();
        return;
    }

    // Send 8 data bits
    for (uint8_t i = 0; i < 8; i++) {
        busy_wait_us_32(15);
        if (data & (1 << i)) {
            parity = !parity;
            mouse_data_hi();
        } else {
            mouse_data_lo();
        }
        if (!wait_clock_hi(100)) goto done;
        if (!wait_clock_lo(100)) goto done;
    }

    // Send parity bit
    busy_wait_us_32(15);
    if (parity) {
        mouse_data_hi();
    } else {
        mouse_data_lo();
    }
    if (!wait_clock_hi(100)) goto done;
    if (!wait_clock_lo(100)) goto done;

    // Send stop bit (release data line)
    busy_wait_us_32(15);
    mouse_data_hi();

    // Wait for ACK from device (data low)
    wait_data_lo(100);
    wait_data_hi(100);
    wait_clock_hi(100);

done:
    mouse_idle();
    mouse_irq_on();
    busy_wait_ms(25);  // Give device time to respond
}

//-----------------------------------------------------------------------------
// GPIO IRQ handler for mouse clock
//-----------------------------------------------------------------------------

static void __not_in_flash_func(mouse_gpio_callback)(uint gpio, uint32_t events) {
    if (gpio != MOUSE_CLK_PIN) return;

    // Small delay to ensure data line is stable after clock edge
    // At 504 MHz, a few NOPs give ~10-20ns settling time
    asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;");

    uint8_t val = gpio_get(MOUSE_DATA_PIN);
    uint32_t now_us = time_us_32();

    // Timeout detection - reset if too long since last bit
    if (now_us - mouse_prev_us > 250) {
        mouse_bitcount = 0;
        mouse_incoming = 0;
    }
    mouse_prev_us = now_us;

    // Bits 1-8 are data (bit 0 is start, bit 9 is parity, bit 10 is stop)
    if (mouse_bitcount >= 1 && mouse_bitcount <= 8) {
        mouse_incoming |= (val << (mouse_bitcount - 1));
    }

    mouse_bitcount++;

    // Complete byte received (11 bits: start + 8 data + parity + stop)
    if (mouse_bitcount == 11) {
        // Add to circular buffer
        uint8_t next_head = (mouse_buffer_head + 1) % MOUSE_BUFFER_SIZE;
        if (next_head != mouse_buffer_tail) {
            mouse_buffer[mouse_buffer_head] = mouse_incoming;
            mouse_buffer_head = next_head;
        }
        mouse_bitcount = 0;
        mouse_incoming = 0;
    }
}

//-----------------------------------------------------------------------------
// Get byte from buffer
//-----------------------------------------------------------------------------

static int mouse_buffer_get(uint8_t *byte) {
    if (mouse_buffer_head == mouse_buffer_tail) {
        return 0;
    }
    *byte = mouse_buffer[mouse_buffer_tail];
    mouse_buffer_tail = (mouse_buffer_tail + 1) % MOUSE_BUFFER_SIZE;
    return 1;
}

//-----------------------------------------------------------------------------
// Process a complete mouse packet
//-----------------------------------------------------------------------------

static void process_mouse_packet(void) {
    uint8_t status = packet_data[0];

    // PS/2 mouse status byte bit 3 should always be 1 (sync bit)
    // If it's not, this might be a misaligned packet
    if (!(status & 0x08)) {
        // Sync bit not set - discard and try to resync
        return;
    }

    // Check for overflow or invalid packet
    if (status & 0xC0) {
        // X or Y overflow, discard
        return;
    }

    // Extract movement with sign extension
    int16_t dx = packet_data[1];
    int16_t dy = packet_data[2];

    if (status & 0x10) dx |= 0xFF00;  // X sign bit
    if (status & 0x20) dy |= 0xFF00;  // Y sign bit

    // Extract buttons (bits 0-2 of status byte)
    uint8_t buttons = status & 0x07;

    // Extract wheel if IntelliMouse
    int8_t wheel = 0;
    if (packet_size == 4) {
        wheel = (int8_t)packet_data[3];
        // Limit wheel to 4 bits signed
        if (wheel > 7) wheel = 7;
        if (wheel < -8) wheel = -8;
    }

    // Accumulate movement
    mouse_state.delta_x += dx;
    mouse_state.delta_y += dy;
    mouse_state.wheel += wheel;
    mouse_state.buttons = buttons;
}

//-----------------------------------------------------------------------------
// Initialize mouse hardware
//-----------------------------------------------------------------------------

void ps2mouse_init(void) {
    memset((void*)&mouse_state, 0, sizeof(mouse_state));
    mouse_buffer_head = 0;
    mouse_buffer_tail = 0;
    mouse_bitcount = 0;
    packet_index = 0;
    packet_size = 3;
    prev_buttons = 0;

    // Initialize GPIO pins
    gpio_init(MOUSE_CLK_PIN);
    gpio_init(MOUSE_DATA_PIN);
    gpio_pull_up(MOUSE_CLK_PIN);
    gpio_pull_up(MOUSE_DATA_PIN);
    gpio_set_drive_strength(MOUSE_CLK_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(MOUSE_DATA_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_dir(MOUSE_CLK_PIN, GPIO_IN);
    gpio_set_dir(MOUSE_DATA_PIN, GPIO_IN);

    // Set up IRQ handler
    gpio_set_irq_enabled_with_callback(MOUSE_CLK_PIN, GPIO_IRQ_EDGE_FALL, 1,
                                       (gpio_irq_callback_t)mouse_gpio_callback);

    // Reset mouse
    mouse_send_byte(MOUSE_CMD_RESET);
    busy_wait_ms(500);  // Reset can take up to 500ms

    // Clear any response bytes
    uint8_t dummy;
    while (mouse_buffer_get(&dummy)) {}

    // Try to enable IntelliMouse mode (wheel support)
    // Magic sequence: set sample rate to 200, 100, 80, then get device ID
    mouse_send_byte(MOUSE_CMD_SET_SAMPLE_RATE);
    mouse_send_byte(200);
    mouse_send_byte(MOUSE_CMD_SET_SAMPLE_RATE);
    mouse_send_byte(100);
    mouse_send_byte(MOUSE_CMD_SET_SAMPLE_RATE);
    mouse_send_byte(80);

    // Get device ID to check if IntelliMouse mode activated
    mouse_send_byte(MOUSE_CMD_GET_DEVICE_ID);
    busy_wait_ms(50);

    // Check response
    uint8_t id = 0;
    if (mouse_buffer_get(&dummy)) {  // ACK
        if (mouse_buffer_get(&id)) {
            if (id == 0x03) {
                // IntelliMouse detected
                mouse_state.has_wheel = 1;
                packet_size = 4;
            }
        }
    }

    // Clear buffer
    while (mouse_buffer_get(&dummy)) {}

    // Set resolution (8 counts per mm)
    mouse_send_byte(MOUSE_CMD_SET_RESOLUTION);
    mouse_send_byte(0x03);  // 8 counts/mm

    // Set scaling 1:1
    mouse_send_byte(MOUSE_CMD_SET_SCALING_1_1);

    // Set sample rate (100 samples/sec for responsive emulator input)
    mouse_send_byte(MOUSE_CMD_SET_SAMPLE_RATE);
    mouse_send_byte(100);

    // Enable data reporting
    mouse_send_byte(MOUSE_CMD_ENABLE_DATA);
    busy_wait_ms(25);

    // Clear any pending bytes
    while (mouse_buffer_get(&dummy)) {}

    mouse_state.initialized = 1;

    printf("PS/2 Mouse initialized%s\n",
           mouse_state.has_wheel ? " (IntelliMouse with wheel)" : "");
}

//-----------------------------------------------------------------------------
// Poll for mouse data
//-----------------------------------------------------------------------------

void ps2mouse_poll(void) {
    uint8_t byte;

    while (mouse_buffer_get(&byte)) {
        // Skip ACK bytes
        if (byte == MOUSE_RESP_ACK) continue;

        // If this is the first byte of a packet, validate it
        // The status byte (first byte) must have bit 3 set (always 1 sync bit)
        if (packet_index == 0) {
            if (!(byte & 0x08)) {
                // Invalid first byte - skip it and try to resync
                continue;
            }
        }

        // Add byte to packet
        packet_data[packet_index++] = byte;

        // Check for complete packet
        if (packet_index >= packet_size) {
            process_mouse_packet();
            packet_index = 0;
        }
    }
}

//-----------------------------------------------------------------------------
// Get accumulated mouse state
//-----------------------------------------------------------------------------

int ps2mouse_get_state(int16_t *dx, int16_t *dy, int8_t *wheel, uint8_t *buttons) {
    // First poll for any new data
    ps2mouse_poll();

    int has_data = (mouse_state.delta_x != 0 ||
                    mouse_state.delta_y != 0 ||
                    mouse_state.wheel != 0 ||
                    mouse_state.buttons != prev_buttons);

    if (dx) *dx = mouse_state.delta_x;
    if (dy) *dy = mouse_state.delta_y;
    if (wheel) *wheel = mouse_state.wheel;
    if (buttons) *buttons = mouse_state.buttons;

    // Remember button state for change detection
    prev_buttons = mouse_state.buttons;

    // Clear accumulators
    mouse_state.delta_x = 0;
    mouse_state.delta_y = 0;
    mouse_state.wheel = 0;

    return has_data;
}

int ps2mouse_is_initialized(void) {
    return mouse_state.initialized;
}
