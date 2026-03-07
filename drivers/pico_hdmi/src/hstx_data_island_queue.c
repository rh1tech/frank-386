#include "pico_hdmi/hstx_data_island_queue.h"

#include <string.h>

#include "pico.h"

#define DI_RING_BUFFER_SIZE 256
static hstx_data_island_t di_ring_buffer[DI_RING_BUFFER_SIZE];
static volatile uint32_t di_ring_head = 0;
static volatile uint32_t di_ring_tail = 0;

// Single pre-encoded silent audio packet (fixed B-frame flags).
static hstx_data_island_t silence_packet;

// Audio timing state (default 48kHz, 525 lines for 480p)
static uint32_t audio_sample_accum = 0; // Fixed-point accumulator
static uint32_t cached_v_total_lines = 525;
#define DEFAULT_SAMPLES_PER_FRAME (48000 / 60)
static uint32_t samples_per_line_fp = (DEFAULT_SAMPLES_PER_FRAME << 16) / 525;

// Limit accumulator to avoid overflow if we run dry.
// Clamping to 1 packet (plus a tiny margin is implicit) ensures we don't burst.
#define MAX_AUDIO_ACCUM (4 << 16)

void hstx_di_queue_init(void)
{
    di_ring_head = 0;
    di_ring_tail = 0;
    audio_sample_accum = 0;
    // Build a single silent audio packet.
    hstx_packet_t packet;
    audio_sample_t samples[4] = {0};
    (void)hstx_packet_set_audio_samples(&packet, samples, 4, 0);
    hstx_encode_data_island(&silence_packet, &packet, false, true);
}

void hstx_di_queue_set_sample_rate(uint32_t sample_rate)
{
    uint32_t samples_per_frame = sample_rate / 60;
    samples_per_line_fp = (samples_per_frame << 16) / cached_v_total_lines;
}

void hstx_di_queue_set_v_total(uint32_t v_total)
{
    cached_v_total_lines = v_total;
}

void hstx_di_queue_set_samples_per_line_fp(uint32_t value)
{
    samples_per_line_fp = value;
}

bool hstx_di_queue_push(const hstx_data_island_t *island)
{
    uint32_t next_head = (di_ring_head + 1) % DI_RING_BUFFER_SIZE;
    if (next_head == di_ring_tail)
        return false;

    di_ring_buffer[di_ring_head] = *island;
    di_ring_head = next_head;
    return true;
}

uint32_t hstx_di_queue_get_level(void)
{
    uint32_t head = di_ring_head;
    uint32_t tail = di_ring_tail;
    if (head >= tail)
        return head - tail;
    return DI_RING_BUFFER_SIZE + head - tail;
}

void __scratch_x("") hstx_di_queue_tick(void)
{
    audio_sample_accum += samples_per_line_fp;
}

const uint32_t *__scratch_x("") hstx_di_queue_get_audio_packet(void)
{
    // Check if it's time to send a 4-sample audio packet (every ~2.6 lines)
    if (audio_sample_accum >= (4 << 16)) {
        audio_sample_accum -= (4 << 16);
        if (di_ring_tail != di_ring_head) {
            const uint32_t *words = di_ring_buffer[di_ring_tail].words;
            di_ring_tail = (di_ring_tail + 1) % DI_RING_BUFFER_SIZE;
            return words;
        }
        // Queue is empty: return a pre-encoded silent packet to keep HDMI audio active.
        return silence_packet.words;
    }
    return NULL;
}
