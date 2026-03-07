#ifndef HSTX_DATA_ISLAND_QUEUE_H
#define HSTX_DATA_ISLAND_QUEUE_H

#include "pico_hdmi/hstx_packet.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize the Data Island queue and scheduler.
 */
void hstx_di_queue_init(void);

/**
 * Set the audio sample rate for packet timing.
 * @param sample_rate Audio sample rate in Hz (e.g. 44100, 48000)
 */
void hstx_di_queue_set_sample_rate(uint32_t sample_rate);

/**
 * Push a pre-encoded Data Island into the queue.
 * Returns true if successful, false if the queue is full.
 */
bool hstx_di_queue_push(const hstx_data_island_t *island);

/**
 * Get the current number of items in the queue.
 */
uint32_t hstx_di_queue_get_level(void);

/**
 * Set the vertical total lines for audio packet timing calculation.
 * Must be called when the video mode changes.
 * @param v_total Total vertical lines (e.g. 525 for 480p, 262 for 240p)
 */
void hstx_di_queue_set_v_total(uint32_t v_total);

/**
 * Override samples_per_line with a pixel-clock-accurate 16.16 fixed-point value.
 * Use this instead of set_sample_rate when the frame rate is not exactly 60 Hz.
 * @param value samples_per_line in 16.16 fixed-point
 */
void hstx_di_queue_set_samples_per_line_fp(uint32_t value);

/**
 * Advance the Data Island scheduler by one scanline.
 * Must be called exactly once per scanline in the DMA ISR.
 */
void hstx_di_queue_tick(void);

/**
 * Get the next audio Data Island packet if the scheduler determines it's time.
 *
 * @return Pointer to 36-word HSTX data island, or NULL if no packet is due.
 */
const uint32_t *hstx_di_queue_get_audio_packet(void);

#endif // HSTX_DATA_ISLAND_QUEUE_H
