/**
 * murm386 - I2S Audio Driver with Chained Double Buffer DMA
 *
 * Uses two DMA channels in ping-pong configuration:
 * - Channel A plays buffer 0, then triggers channel B
 * - Channel B plays buffer 1, then triggers channel A
 *
 * Each channel completion raises DMA_IRQ_1; the IRQ handler re-arms the
 * completed channel (reset read addr + transfer count) and marks its buffer
 * free for the CPU to refill.
 */

#include "audio.h"
#include "board_config.h"
#include "audio_i2s.pio.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/irq.h"

// Forward declaration of mixer_callback from pc.c
extern void mixer_callback(void *opaque, uint8_t *stream, int free);

//=============================================================================
// State - Chained double buffer (ping-pong) DMA
//=============================================================================

// Use DMA_IRQ_1 to avoid conflicts with VGA (which uses DMA_IRQ_0)
#define AUDIO_DMA_IRQ DMA_IRQ_1

// Fixed DMA channels for audio (keep away from VGA DMA channels)
#define AUDIO_DMA_CH_A 10
#define AUDIO_DMA_CH_B 11

#define DMA_BUFFER_COUNT 2
#define DMA_BUFFER_MAX_SAMPLES AUDIO_BUFFER_SAMPLES

static uint32_t __attribute__((aligned(4))) dma_buffers[DMA_BUFFER_COUNT][DMA_BUFFER_MAX_SAMPLES];

// Bitmask of buffers the CPU is allowed to write (1 = free)
static volatile uint32_t dma_buffers_free_mask = 0;

// Pre-roll: fill both buffers before starting playback
#define PREROLL_BUFFERS 2
static volatile int preroll_count = 0;

static int dma_channel_a = -1;
static int dma_channel_b = -1;
static PIO audio_pio;
static uint audio_sm;
static uint32_t dma_transfer_count;

static volatile bool audio_running = false;

static void audio_dma_irq_handler(void);

//=============================================================================
// I2S Implementation
//=============================================================================

i2s_config_t i2s_get_default_config(void) {
    // 882 samples per frame for 50Hz (44100 / 50)
    // 735 samples per frame for 60Hz (44100 / 60)
    // Use 882 for PAL-like timing
    i2s_config_t config = {
        .sample_freq = AUDIO_SAMPLE_RATE,
        .channel_count = 2,
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_CLOCK_PIN_BASE,
        .pio = pio0,
        .sm = 0,
        .dma_channel = 0,
        .dma_trans_count = 882,
        .dma_buf = NULL,
        .volume = 0,
    };
    return config;
}

void i2s_init(i2s_config_t *config) {
    printf("Audio: Initializing I2S with chained double-buffer DMA...\n");
    printf("Audio: Sample rate: %u Hz, DMA buffer size: %lu frames\n",
           (unsigned)config->sample_freq, (unsigned long)config->dma_trans_count);

    audio_pio = config->pio;
    dma_transfer_count = config->dma_trans_count;

    // Clear audio DMA IRQ flags (IRQ1)
    dma_hw->ints1 = (1u << AUDIO_DMA_CH_A) | (1u << AUDIO_DMA_CH_B);

    // Configure GPIO for PIO
    gpio_set_function(config->data_pin, GPIO_FUNC_PIO0);
    gpio_set_function(config->clock_pin_base, GPIO_FUNC_PIO0);
    gpio_set_function(config->clock_pin_base + 1, GPIO_FUNC_PIO0);

    gpio_set_drive_strength(config->data_pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(config->clock_pin_base, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(config->clock_pin_base + 1, GPIO_DRIVE_STRENGTH_12MA);

    // Claim state machine
    audio_sm = pio_claim_unused_sm(audio_pio, true);
    config->sm = audio_sm;
    printf("Audio: Using PIO0 SM%d\n", audio_sm);

    // Add PIO program
    uint offset = pio_add_program(audio_pio, &audio_i2s_program);
    audio_i2s_program_init(audio_pio, audio_sm, offset,
                           config->data_pin, config->clock_pin_base);

    // Drain the TX FIFO
    pio_sm_clear_fifos(audio_pio, audio_sm);

    // Set clock divider for sample rate
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t divider = sys_clk * 4 / config->sample_freq;
    pio_sm_set_clkdiv_int_frac(audio_pio, audio_sm, divider >> 8u, divider & 0xffu);
    printf("Audio: Clock divider: %u.%u (sys=%lu MHz)\n",
           (unsigned)(divider >> 8u), (unsigned)(divider & 0xffu), (unsigned long)(sys_clk / 1000000));

    // Validate transfer count fits our static buffers
    dma_transfer_count = config->dma_trans_count;
    if (dma_transfer_count == 0) dma_transfer_count = 1;
    if (dma_transfer_count > DMA_BUFFER_MAX_SAMPLES) dma_transfer_count = DMA_BUFFER_MAX_SAMPLES;
    config->dma_trans_count = (uint16_t)dma_transfer_count;

    // Initialize DMA buffers with silence
    memset(dma_buffers, 0, sizeof(dma_buffers));
    config->dma_buf = (uint16_t *)(void *)dma_buffers[0];

    // Use fixed DMA channels for audio
    dma_channel_abort(AUDIO_DMA_CH_A);
    dma_channel_abort(AUDIO_DMA_CH_B);
    while (dma_channel_is_busy(AUDIO_DMA_CH_A) || dma_channel_is_busy(AUDIO_DMA_CH_B)) {
        tight_loop_contents();
    }

    dma_channel_unclaim(AUDIO_DMA_CH_A);
    dma_channel_unclaim(AUDIO_DMA_CH_B);
    dma_channel_claim(AUDIO_DMA_CH_A);
    dma_channel_claim(AUDIO_DMA_CH_B);
    dma_channel_a = AUDIO_DMA_CH_A;
    dma_channel_b = AUDIO_DMA_CH_B;
    config->dma_channel = (uint8_t)dma_channel_a;
    printf("Audio: Using DMA channels %d/%d (IRQ=%d)\n", dma_channel_a, dma_channel_b, AUDIO_DMA_IRQ);

    // Configure DMA channels in ping-pong chain
    dma_channel_config cfg_a = dma_channel_get_default_config(dma_channel_a);
    channel_config_set_read_increment(&cfg_a, true);
    channel_config_set_write_increment(&cfg_a, false);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_a, pio_get_dreq(audio_pio, audio_sm, true));
    channel_config_set_chain_to(&cfg_a, dma_channel_b);

    dma_channel_config cfg_b = dma_channel_get_default_config(dma_channel_b);
    channel_config_set_read_increment(&cfg_b, true);
    channel_config_set_write_increment(&cfg_b, false);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_b, pio_get_dreq(audio_pio, audio_sm, true));
    channel_config_set_chain_to(&cfg_b, dma_channel_a);

    dma_channel_configure(
        dma_channel_a,
        &cfg_a,
        &audio_pio->txf[audio_sm],
        dma_buffers[0],
        dma_transfer_count,
        false
    );

    dma_channel_configure(
        dma_channel_b,
        &cfg_b,
        &audio_pio->txf[audio_sm],
        dma_buffers[1],
        dma_transfer_count,
        false
    );

    // Set up DMA IRQ1 handler
    irq_set_exclusive_handler(AUDIO_DMA_IRQ, audio_dma_irq_handler);
    irq_set_priority(AUDIO_DMA_IRQ, 0x80);
    irq_set_enabled(AUDIO_DMA_IRQ, true);

    // Enable IRQ1 for both channels
    dma_hw->ints1 = (1u << dma_channel_a) | (1u << dma_channel_b);
    dma_channel_set_irq1_enabled(dma_channel_a, true);
    dma_channel_set_irq1_enabled(dma_channel_b, true);

    // DON'T enable PIO state machine yet - enable when DMA starts
    // This avoids any interference with VGA during boot
    // pio_sm_set_enabled(audio_pio, audio_sm, true);

    // Initialize state
    preroll_count = 0;
    dma_buffers_free_mask = (1u << DMA_BUFFER_COUNT) - 1u; // both free
    audio_running = false;

    printf("Audio: I2S ready (double buffer DMA with %d buffer pre-roll)\n", PREROLL_BUFFERS);
}

static bool pio_sm_enabled = false;

void i2s_dma_write_count(i2s_config_t *config, const int16_t *samples, uint32_t sample_count) {
    if (sample_count > dma_transfer_count) sample_count = dma_transfer_count;
    if (sample_count == 0) sample_count = 1;

    // Wait for a free buffer with timeout to prevent blocking VGA updates
    uint8_t buf_index = 0;
    int timeout_counter = 0;
    const int MAX_TIMEOUT = 10000;  // Prevent infinite blocking

    while (true) {
        uint32_t irq_state = save_and_disable_interrupts();
        uint32_t free_mask = dma_buffers_free_mask;

        if (!audio_running) {
            // Pre-roll fills buffer 0 then buffer 1 to preserve ordering
            buf_index = (uint8_t)preroll_count;
            if (buf_index < DMA_BUFFER_COUNT && (free_mask & (1u << buf_index))) {
                dma_buffers_free_mask &= ~(1u << buf_index);
                restore_interrupts(irq_state);
                break;
            }
        } else {
            if (free_mask) {
                buf_index = (free_mask & 1u) ? 0 : 1;
                dma_buffers_free_mask &= ~(1u << buf_index);
                restore_interrupts(irq_state);
                break;
            }
        }

        restore_interrupts(irq_state);

        // Timeout to prevent infinite blocking
        if (++timeout_counter >= MAX_TIMEOUT) {
            // No free buffer available, skip this frame
            return;
        }
        tight_loop_contents();
    }

    uint32_t *write_ptr = dma_buffers[buf_index];
    int16_t *write_ptr16 = (int16_t *)(void *)write_ptr;

    if (config->volume == 0) {
        memcpy(write_ptr, samples, sample_count * sizeof(uint32_t));
    } else if (config->volume > 0) {
        // Attenuation (Right shift)
        for (uint32_t i = 0; i < sample_count * 2; i++) {
            write_ptr16[i] = samples[i] >> config->volume;
        }
    } else {
        // Amplification (Left shift) - with saturation
        int shift = -config->volume;
        for (uint32_t i = 0; i < sample_count * 2; i++) {
            int32_t val = (int32_t)samples[i] << shift;
            if (val > 32767) val = 32767;
            if (val < -32768) val = -32768;
            write_ptr16[i] = (int16_t)val;
        }
    }

    // Pad remainder with silence to keep DMA transfer size stable
    if (sample_count < dma_transfer_count) {
        memset(&write_ptr[sample_count], 0, (dma_transfer_count - sample_count) * sizeof(uint32_t));
    }

    // Memory barrier to ensure writes are visible before DMA reads
    __dmb();

    if (!audio_running) {
        preroll_count++;
        if (preroll_count >= PREROLL_BUFFERS) {
            // Enable PIO state machine now (deferred from init)
            if (!pio_sm_enabled) {
                pio_sm_set_enabled(audio_pio, audio_sm, true);
                pio_sm_enabled = true;
            }
            // Both buffers are filled and queued; start playback on channel A
            dma_channel_start(dma_channel_a);
            audio_running = true;
        }
    } else {
        // Safety check: if underrun stopped the DMA chain, restart it
        if (!dma_channel_is_busy(dma_channel_a) && !dma_channel_is_busy(dma_channel_b)) {
             if (buf_index == 0) dma_channel_start(dma_channel_a);
             else dma_channel_start(dma_channel_b);
        }
    }
}

void i2s_dma_write(i2s_config_t *config, const int16_t *samples) {
    i2s_dma_write_count(config, samples, dma_transfer_count);
}

void i2s_volume(i2s_config_t *config, int8_t volume) {
    if (volume < -4) volume = -4; // Limit max gain to +24dB (<< 4)
    if (volume > 16) volume = 16; // Limit min gain to -96dB (>> 16)
    config->volume = volume;
}

//=============================================================================
// High-level Audio API
//=============================================================================

static bool audio_initialized = false;
static bool audio_enabled = true;
static int master_volume = 160;  // Default to mild amplification (x2)
static i2s_config_t i2s_config;

// Startup mute: output silence for first N frames to let hardware settle
#define STARTUP_FADE_FRAMES 60  // ~1 second at 60fps
static int startup_frame_counter = 0;

// Mixed stereo buffer (16-bit stereo samples)
static int16_t __attribute__((aligned(4))) mixed_buffer[AUDIO_BUFFER_SAMPLES * 2];

// Anti-click filter: track last sample values for smooth transitions
static int16_t last_sample_l = 0;
static int16_t last_sample_r = 0;

// Anti-click fade length (samples) - short fade to prevent clicks
#define ANTICLICK_FADE_SAMPLES 32

// Target samples per frame
// 44100 / 60 = 735 samples at 60Hz
#define TARGET_SAMPLES_PER_FRAME 735

bool audio_init(void) {
    // Reset all state variables
    audio_initialized = false;
    audio_running = false;
    pio_sm_enabled = false;
    dma_channel_a = -1;
    dma_channel_b = -1;
    preroll_count = 0;
    dma_buffers_free_mask = (1u << DMA_BUFFER_COUNT) - 1u;
    startup_frame_counter = 0;

    i2s_config = i2s_get_default_config();
    // Use smaller transfer count for lower latency
    i2s_config.dma_trans_count = TARGET_SAMPLES_PER_FRAME;

    i2s_init(&i2s_config);

    audio_initialized = true;

    return true;
}

void audio_shutdown(void) {
    if (!audio_initialized) return;

    // Stop producing new audio and stop the PIO state machine first
    audio_running = false;
    if (pio_sm_enabled) {
        pio_sm_set_enabled(audio_pio, audio_sm, false);
        pio_sm_enabled = false;
    }

    // Disable DMA IRQ and per-channel IRQ generation
    irq_set_enabled(AUDIO_DMA_IRQ, false);

    if (dma_channel_a >= 0) {
        dma_channel_set_irq1_enabled(dma_channel_a, false);
        dma_channel_abort(dma_channel_a);
        dma_hw->ints1 = (1u << dma_channel_a);
        dma_channel_unclaim(dma_channel_a);
        dma_channel_a = -1;
    }

    if (dma_channel_b >= 0) {
        dma_channel_set_irq1_enabled(dma_channel_b, false);
        dma_channel_abort(dma_channel_b);
        dma_hw->ints1 = (1u << dma_channel_b);
        dma_channel_unclaim(dma_channel_b);
        dma_channel_b = -1;
    }

    dma_buffers_free_mask = (1u << DMA_BUFFER_COUNT) - 1u;
    preroll_count = 0;

    audio_initialized = false;
}

bool audio_is_initialized(void) {
    return audio_initialized;
}

static void audio_dma_irq_handler(void) {
    uint32_t ints = dma_hw->ints1;
    uint32_t mask = 0;
    if (dma_channel_a >= 0) mask |= (1u << dma_channel_a);
    if (dma_channel_b >= 0) mask |= (1u << dma_channel_b);
    ints &= mask;
    if (!ints) return;

    if ((dma_channel_a >= 0) && (ints & (1u << dma_channel_a))) {
        dma_hw->ints1 = (1u << dma_channel_a);
        dma_channel_set_read_addr(dma_channel_a, dma_buffers[0], false);
        dma_channel_set_trans_count(dma_channel_a, dma_transfer_count, false);
        dma_buffers_free_mask |= 1u;
    }

    if ((dma_channel_b >= 0) && (ints & (1u << dma_channel_b))) {
        dma_hw->ints1 = (1u << dma_channel_b);
        dma_channel_set_read_addr(dma_channel_b, dma_buffers[1], false);
        dma_channel_set_trans_count(dma_channel_b, dma_transfer_count, false);
        dma_buffers_free_mask |= 2u;
    }
}

void audio_process_frame(void *pc) {
    if (!audio_initialized || !audio_enabled) return;

    // Startup mute: output pure silence for first N frames
    if (startup_frame_counter < STARTUP_FADE_FRAMES) {
        memset(mixed_buffer, 0, TARGET_SAMPLES_PER_FRAME * 2 * sizeof(int16_t));
        startup_frame_counter++;
        last_sample_l = 0;
        last_sample_r = 0;
        i2s_dma_write_count(&i2s_config, mixed_buffer, TARGET_SAMPLES_PER_FRAME);
        return;
    }

    // Clear buffer
    memset(mixed_buffer, 0, TARGET_SAMPLES_PER_FRAME * 2 * sizeof(int16_t));

    // Call the PC mixer to fill the buffer
    // mixer_callback expects bytes: samples * 2 channels * 2 bytes per sample
    if (pc) {
        mixer_callback(pc, (uint8_t *)mixed_buffer, TARGET_SAMPLES_PER_FRAME * 4);
    }

    // Anti-click filter: smooth discontinuities at buffer boundaries
    // Check if there's a large jump from last frame's ending to this frame's start
    int16_t first_l = mixed_buffer[0];
    int16_t first_r = mixed_buffer[1];
    int diff_l = first_l - last_sample_l;
    int diff_r = first_r - last_sample_r;

    // Apply fade-in from last sample if there's a discontinuity
    // This creates a smooth transition over ANTICLICK_FADE_SAMPLES
    if (diff_l != 0 || diff_r != 0) {
        for (int i = 0; i < ANTICLICK_FADE_SAMPLES && i < TARGET_SAMPLES_PER_FRAME; i++) {
            // Linear interpolation from last_sample to current sample
            int fade_in = i;
            int fade_out = ANTICLICK_FADE_SAMPLES - i;

            int idx_l = i * 2;
            int idx_r = i * 2 + 1;

            // Blend: (last_sample * fade_out + current_sample * fade_in) / ANTICLICK_FADE_SAMPLES
            int blended_l = (last_sample_l * fade_out + mixed_buffer[idx_l] * fade_in) / ANTICLICK_FADE_SAMPLES;
            int blended_r = (last_sample_r * fade_out + mixed_buffer[idx_r] * fade_in) / ANTICLICK_FADE_SAMPLES;

            mixed_buffer[idx_l] = (int16_t)blended_l;
            mixed_buffer[idx_r] = (int16_t)blended_r;
        }
    }

    // Store last samples for next frame's anti-click processing
    last_sample_l = mixed_buffer[(TARGET_SAMPLES_PER_FRAME - 1) * 2];
    last_sample_r = mixed_buffer[(TARGET_SAMPLES_PER_FRAME - 1) * 2 + 1];

    // Submit to I2S
    i2s_dma_write_count(&i2s_config, mixed_buffer, TARGET_SAMPLES_PER_FRAME);
}

void audio_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    master_volume = volume;

    // Map 0-255 volume to shift:
    // 0..128 -> Attenuation (shift 16..0)
    // 129..255 -> Amplification (shift -1..-4)
    int8_t shift;
    if (volume <= 128) {
        shift = (128 - volume) >> 3; // 0->16, 128->0
    } else {
        // 129-> -1, 160-> -2, 192-> -3, 255-> -4
        shift = -((volume - 128) >> 5) - 1;
        if (shift < -4) shift = -4;
    }
    i2s_volume(&i2s_config, shift);
}

int audio_get_volume(void) {
    return master_volume;
}

void audio_set_enabled(bool enabled) {
    audio_enabled = enabled;
}

bool audio_is_enabled(void) {
    return audio_enabled;
}

i2s_config_t* audio_get_i2s_config(void) {
    return &i2s_config;
}

bool audio_needs_samples(void) {
    if (!audio_initialized || !audio_enabled) return false;
    // Check if any buffer is free in the mask
    return (dma_buffers_free_mask != 0);
}
