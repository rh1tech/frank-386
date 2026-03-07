/**
 * pico_hdmi True 240p DirectVideo Example
 *
 * Demonstrates true 240p output for retro gaming scalers:
 * - 1280x240 @ 60Hz with 4x pixel repetition (representing 320x240)
 * - Standard 25.2 MHz pixel clock (HDMI-compliant)
 * - HDMI audio with a 440 Hz sine tone
 * - Compatible with Morph4K, RetroTINK 4K, and other scalers
 *
 * Target: RP2350 (Raspberry Pi Pico 2)
 */

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output_rt.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"

#include <math.h>

// ============================================================================
// Configuration
// ============================================================================

// True 240p: 1280x240 (Pixel Quadrupled from 320x240)
// This uses a 25.2 MHz pixel clock (HDMI standard minimum)
#define FRAME_WIDTH 1280
#define FRAME_HEIGHT 240

// Color bars (RGB565)
#define WHITE 0xFFFF
#define YELLOW 0xFFE0
#define CYAN 0x07FF
#define GREEN 0x07E0
#define MAGENTA 0xF81F
#define RED 0xF800
#define BLUE 0x001F
#define BLACK 0x0000

static const uint16_t color_bars[8] = {WHITE, YELLOW, CYAN, GREEN, MAGENTA, RED, BLUE, BLACK};

// Audio configuration
#define AUDIO_SAMPLE_RATE 48000
#define TONE_FREQ 440
#define TONE_AMPLITUDE 6000

// ============================================================================
// Audio State
// ============================================================================

#define SINE_TABLE_SIZE 256
static int16_t sine_table[SINE_TABLE_SIZE];
static uint32_t audio_phase = 0;
static uint32_t phase_increment = 0;
static int audio_frame_counter = 0;

static void init_sine_table(void)
{
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        float angle = (float)i * 2.0F * 3.14159265F / SINE_TABLE_SIZE;
        sine_table[i] = (int16_t)(sinf(angle) * TONE_AMPLITUDE);
    }
}

static inline int16_t get_sine_sample(void)
{
    int16_t s = sine_table[(audio_phase >> 24) & 0xFF];
    audio_phase += phase_increment;
    return s;
}

static void generate_audio(void)
{
    while (hstx_di_queue_get_level() < 200) {
        audio_sample_t samples[4];
        for (int i = 0; i < 4; i++) {
            int16_t s = get_sine_sample();
            samples[i].left = s;
            samples[i].right = s;
        }

        hstx_packet_t packet;
        audio_frame_counter = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);

        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        hstx_di_queue_push(&island);
    }
}

// ============================================================================
// Scanline Callback (runs on Core 1)
// ============================================================================

void __scratch_x("") scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;
    (void)active_line;

    // Draw 8 color bars, each 160 pixels wide (1280 / 8 = 160)
    for (int bar = 0; bar < 8; bar++) {
        uint16_t color = color_bars[bar];
        // Pack two pixels into each uint32_t
        uint32_t packed = color | (color << 16);

        // 160 pixels per bar -> 80 uint32_t words
        int start = bar * 160 / 2;
        int end = (bar + 1) * 160 / 2;
        for (int i = start; i < end; i++) {
            dst[i] = packed;
        }
    }
}

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void)
{
    // Set system clock to 126 MHz (gives 25.2 MHz pixel clock)
    set_sys_clock_khz(126000, true);
    stdio_init_all();

    // Initialize LED for heartbeat
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Initialize audio
    init_sine_table();
    phase_increment = (uint32_t)(((uint64_t)TONE_FREQ << 32) / AUDIO_SAMPLE_RATE);

    // Initialize video output in 240p mode
    hstx_di_queue_init();
    video_output_set_mode(&video_mode_240_p);
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);

    // Use HDMI mode for audio + AVI InfoFrame
    video_output_set_dvi_mode(false);

    // Register scanline callback
    video_output_set_scanline_callback(scanline_callback);

    // Pre-fill audio buffer
    generate_audio();

    // Launch Core 1 for HSTX output
    multicore_launch_core1(video_output_core1_run);

    // Main loop - audio generation on Core 0 (separate from DMA IRQs on Core 1)
    uint32_t last_frame = 0;
    bool led_state = false;

    while (1) {
        generate_audio();

        while (video_frame_count == last_frame) {
            generate_audio();
            tight_loop_contents();
        }
        last_frame = video_frame_count;

        // LED heartbeat (toggle every 30 frames = ~0.5s)
        if ((video_frame_count % 30) == 0) {
            led_state = !led_state;
            gpio_put(PICO_DEFAULT_LED_PIN, led_state);
        }
    }

    return 0;
}
