// https://archive.org/details/dss-programmers-guide
#pragma GCC optimize("Ofast")
#include "audio.h"
#include "dss.h"
#define INLINE inline

#define FIFO_BUFFER_SIZE 16

static uint8_t fifo_buffer[FIFO_BUFFER_SIZE] = { 0 };
static uint8_t fifo_head = 0;  // Write position
static uint8_t fifo_tail = 0;  // Read position
static uint8_t fifo_count = 0; // Number of elements
static uint8_t dss_data;

int16_t __not_in_flash_func(dss_sample)() {
    static uint8_t held = 0;
    if (fifo_count == 0) {
        return held;
    }
    held = fifo_buffer[fifo_tail];
    fifo_tail = (fifo_tail + 1) & (FIFO_BUFFER_SIZE - 1);
    --fifo_count;
    return ((int16_t)held - 128) << 8;
}

static INLINE void fifo_push_byte(uint8_t value) {
    if (__builtin_expect(fifo_count >= FIFO_BUFFER_SIZE, 0))
        return;

    fifo_buffer[fifo_head] = value;
    fifo_head = (fifo_head + 1) & (FIFO_BUFFER_SIZE - 1);
    ++fifo_count;
}

static INLINE uint8_t fifo_is_full() {
    return fifo_count == FIFO_BUFFER_SIZE ? 0x40 : 0x00;
}

uint8_t dss_in(const uint16_t portnum) {
    return portnum & 1 ? fifo_is_full() : dss_data;
}

void dss_out(uint16_t portnum, uint8_t value) {
    static uint8_t control = 0;

    if (portnum == 0x378) {
        dss_data = value;          // только запомнить
        return;
    }

    if (portnum == 0x37A) {
        // pin17 = SELECTIN = control bit3 (инвертированный).
        // Rising edge on pin17 => bit3 1->0
        const uint8_t old_b3 = control & 0x08;
        const uint8_t new_b3 = value   & 0x08;

        if (old_b3 && !new_b3) {
            fifo_push_byte(dss_data);
        }

        control = value;
    }
}
