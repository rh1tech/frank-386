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
#include "debug.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/irq.h"

// Target samples per frame
// 44100 / 60 = 735 samples at 60Hz
#define TARGET_SAMPLES_PER_FRAME 735

// Forward declaration of mixer_callback from pc.c
extern void mixer_callback(void *opaque, uint8_t *stream, int free);

//=============================================================================
// State - Chained double buffer (ping-pong) DMA
//=============================================================================

// Use DMA_IRQ_1 to avoid conflicts with VGA (which uses DMA_IRQ_0)
#define AUDIO_DMA_IRQ DMA_IRQ_1

static volatile bool audio_running = false;
static bool pio_sm_enabled = false;

#if FEATURE_AUDIO_I2S

// Fixed DMA channels for audio (keep away from VGA DMA channels)
#define AUDIO_DMA_CH_A 10
#define AUDIO_DMA_CH_B 11

#define DMA_BUFFER_COUNT 2
#define DMA_BUFFER_MAX_SAMPLES AUDIO_BUFFER_SAMPLES

static uint32_t __attribute__((aligned(4))) dma_buffers[DMA_BUFFER_COUNT][DMA_BUFFER_MAX_SAMPLES];

// Pre-roll: fill both buffers before starting playback
#define PREROLL_BUFFERS 2
static volatile int preroll_count = 0;

// Bitmask of buffers the CPU is allowed to write (1 = free)
static volatile uint32_t dma_buffers_free_mask = 0;

static int dma_channel_a = -1;
static int dma_channel_b = -1;
static PIO audio_pio;
static uint audio_sm;
static uint32_t dma_transfer_count;


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

//=============================================================================
// I2S Implementation
//=============================================================================

// I2S configuration structure
typedef struct {
    uint32_t sample_freq;
    uint16_t channel_count;
    uint8_t  data_pin;
    uint8_t  clock_pin_base;
    PIO      pio;
    uint8_t  sm;
    uint8_t  dma_channel;
    uint16_t dma_trans_count;
    uint16_t *dma_buf;
    int8_t   volume;  // >0 = attenuation (right shift), <0 = amplification (left shift)
} i2s_config_t;


// 882 samples per frame for 50Hz (44100 / 50)
// 735 samples per frame for 60Hz (44100 / 60)
// Use 882 for PAL-like timing
static i2s_config_t i2s_config = {
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

// Initialize I2S with the given configuration
static void i2s_init(void) {
    dma_channel_a = -1;
    dma_channel_b = -1;
    preroll_count = 0;
    dma_buffers_free_mask = (1u << DMA_BUFFER_COUNT) - 1u;
    // Use smaller transfer count for lower latency
    i2s_config.dma_trans_count = TARGET_SAMPLES_PER_FRAME;

    DBG_PRINT("Audio: Initializing I2S with chained double-buffer DMA...\n");
    DBG_PRINT("Audio: Sample rate: %u Hz, DMA buffer size: %lu frames\n",
           (unsigned)i2s_config.sample_freq, (unsigned long)i2s_config.dma_trans_count);
           
    audio_pio = i2s_config.pio;
    dma_transfer_count = i2s_config.dma_trans_count;

    // Clear audio DMA IRQ flags (IRQ1)
    dma_hw->ints1 = (1u << AUDIO_DMA_CH_A) | (1u << AUDIO_DMA_CH_B);

    // Configure GPIO for PIO
    gpio_set_function(i2s_config.data_pin, GPIO_FUNC_PIO0);
    gpio_set_function(i2s_config.clock_pin_base, GPIO_FUNC_PIO0);
    gpio_set_function(i2s_config.clock_pin_base + 1, GPIO_FUNC_PIO0);

    gpio_set_drive_strength(i2s_config.data_pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(i2s_config.clock_pin_base, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(i2s_config.clock_pin_base + 1, GPIO_DRIVE_STRENGTH_12MA);

    // Claim state machine
    audio_sm = pio_claim_unused_sm(audio_pio, true);
    i2s_config.sm = audio_sm;
    DBG_PRINT("Audio: Using PIO0 SM%d\n", audio_sm);

    // Add PIO program
    uint offset = pio_add_program(audio_pio, &audio_i2s_program);
    audio_i2s_program_init(audio_pio, audio_sm, offset,
                           i2s_config.data_pin, i2s_config.clock_pin_base);

    // Drain the TX FIFO
    pio_sm_clear_fifos(audio_pio, audio_sm);

    // Set clock divider for sample rate
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t divider = sys_clk * 4 / i2s_config.sample_freq;
    pio_sm_set_clkdiv_int_frac(audio_pio, audio_sm, divider >> 8u, divider & 0xffu);
    DBG_PRINT("Audio: Clock divider: %u.%u (sys=%lu MHz)\n",
           (unsigned)(divider >> 8u), (unsigned)(divider & 0xffu), (unsigned long)(sys_clk / 1000000));

    // Validate transfer count fits our static buffers
    dma_transfer_count = i2s_config.dma_trans_count;
    if (dma_transfer_count == 0) dma_transfer_count = 1;
    if (dma_transfer_count > DMA_BUFFER_MAX_SAMPLES) dma_transfer_count = DMA_BUFFER_MAX_SAMPLES;
    i2s_config.dma_trans_count = (uint16_t)dma_transfer_count;

    // Initialize DMA buffers with silence
    memset(dma_buffers, 0, sizeof(dma_buffers));
    i2s_config.dma_buf = (uint16_t *)(void *)dma_buffers[0];

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
    i2s_config.dma_channel = (uint8_t)dma_channel_a;
    DBG_PRINT("Audio: Using DMA channels %d/%d (IRQ=%d)\n", dma_channel_a, dma_channel_b, AUDIO_DMA_IRQ);

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

    DBG_PRINT("Audio: I2S ready (double buffer DMA with %d buffer pre-roll)\n", PREROLL_BUFFERS);
}

static void i2s_dma_write_count(const int16_t *samples, uint32_t sample_count) {
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

    if (i2s_config.volume == 0) {
        memcpy(write_ptr, samples, sample_count * sizeof(uint32_t));
    } else if (i2s_config.volume > 0) {
        // Attenuation (Right shift)
        for (uint32_t i = 0; i < sample_count * 2; i++) {
            write_ptr16[i] = samples[i] >> i2s_config.volume;
        }
    } else {
        // Amplification (Left shift) - with saturation
        int shift = -i2s_config.volume;
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

// Adjust volume (0 = loudest if attenuation only, or specific shifts)
static void i2s_volume(int8_t volume) {
    if (volume < -8) volume = -8; // Limit max gain to +48dB (<< 8)
    if (volume > 16) volume = 16; // Limit min gain to -96dB (>> 16)
    i2s_config.volume = volume;
}
#else
#if defined(FEATURE_AUDIO_PWM)

#include <hardware/pwm.h>
#define PWM_BITS 12
#define PWM_WRAP ((1 << PWM_BITS) - 1)
#define PWM_AUDIO_RATE        AUDIO_SAMPLE_RATE
#define PWM_DMA_SAMPLES       TARGET_SAMPLES_PER_FRAME

// Ping-pong DMA: два канала и два буфера, аналогично I2S-реализации.
// Канал A воспроизводит буфер 0, затем запускает канал B (chain_to).
// Канал B воспроизводит буфер 1, затем запускает канал A.
// По завершению каждый канал генерирует DMA_IRQ_1; обработчик сбрасывает
// read_addr/trans_count и выставляет бит в pwm_buffers_free_mask.
// audio_needs_samples() проверяет маску без какого-либо ожидания.

#define PWM_DMA_CH_A  8
#define PWM_DMA_CH_B  9

static uint g_pwm_slice = 0;
static int  g_pwm_dma_chan_a = -1;
static int  g_pwm_dma_chan_b = -1;
static uint32_t g_pwm_dma_count = 0;

static uint32_t __attribute__((aligned(4))) pwm_dma_buffers[2][PWM_DMA_SAMPLES];
static volatile uint32_t pwm_buffers_free_mask = 0;  // бит N = буфер N свободен

// Pre-roll: заполнить оба буфера до старта DMA
#define PWM_PREROLL_BUFFERS 2
static volatile int pwm_preroll_count = 0;

static void audio_pwm_dma_irq_handler(void) {
    uint32_t ints = dma_hw->ints1;
    uint32_t mask = 0;
    if (g_pwm_dma_chan_a >= 0) mask |= (1u << g_pwm_dma_chan_a);
    if (g_pwm_dma_chan_b >= 0) mask |= (1u << g_pwm_dma_chan_b);
    ints &= mask;
    if (!ints) return;

    if ((g_pwm_dma_chan_a >= 0) && (ints & (1u << g_pwm_dma_chan_a))) {
        dma_hw->ints1 = (1u << g_pwm_dma_chan_a);
        dma_channel_set_read_addr(g_pwm_dma_chan_a, pwm_dma_buffers[0], false);
        dma_channel_set_trans_count(g_pwm_dma_chan_a, g_pwm_dma_count, false);
        pwm_buffers_free_mask |= 1u;
    }

    if ((g_pwm_dma_chan_b >= 0) && (ints & (1u << g_pwm_dma_chan_b))) {
        dma_hw->ints1 = (1u << g_pwm_dma_chan_b);
        dma_channel_set_read_addr(g_pwm_dma_chan_b, pwm_dma_buffers[1], false);
        dma_channel_set_trans_count(g_pwm_dma_chan_b, g_pwm_dma_count, false);
        pwm_buffers_free_mask |= 2u;
    }
}

static void audio_pwm_init(void) {
    // PWM pins must be adjacent for single-slice stereo via CC
    gpio_set_function(PWM_RIGHT_PIN, GPIO_FUNC_PWM);
    gpio_set_function(PWM_LEFT_PIN,  GPIO_FUNC_PWM);

    g_pwm_slice = pwm_gpio_to_slice_num(PWM_RIGHT_PIN);
    pwm_config pcfg = pwm_get_default_config();
    // PWM frequency = clk_sys / (clkdiv * (PWM_WRAP + 1))
    uint32_t sys_clk = clock_get_hz(clk_sys);
    float clkdiv = (float)sys_clk / (float)(PWM_AUDIO_RATE * (PWM_WRAP + 1));
    pwm_config_set_clkdiv(&pcfg, clkdiv);
    pwm_config_set_wrap(&pcfg, PWM_WRAP);
    pwm_init(g_pwm_slice, &pcfg, true);
    pwm_set_chan_level(g_pwm_slice, PWM_CHAN_A, PWM_WRAP >> 1);
    pwm_set_chan_level(g_pwm_slice, PWM_CHAN_B, PWM_WRAP >> 1);
    pwm_set_enabled(g_pwm_slice, true);

    // Инициализировать DMA буферы тишиной
    uint32_t mid = ((uint32_t)(PWM_WRAP >> 1) << 16) | (PWM_WRAP >> 1);
    for (uint32_t i = 0; i < PWM_DMA_SAMPLES; i++) {
        pwm_dma_buffers[0][i] = mid;
        pwm_dma_buffers[1][i] = mid;
    }

    g_pwm_dma_count   = PWM_DMA_SAMPLES;
    pwm_preroll_count = 0;
    pwm_buffers_free_mask = (1u << 2) - 1u;  // оба свободны

    // Захватить фиксированные каналы (не пересекаются с I2S каналами 10/11)
    dma_channel_abort(PWM_DMA_CH_A);
    dma_channel_abort(PWM_DMA_CH_B);
    while (dma_channel_is_busy(PWM_DMA_CH_A) || dma_channel_is_busy(PWM_DMA_CH_B))
        tight_loop_contents();
    dma_channel_unclaim(PWM_DMA_CH_A);
    dma_channel_unclaim(PWM_DMA_CH_B);
    dma_channel_claim(PWM_DMA_CH_A);
    dma_channel_claim(PWM_DMA_CH_B);
    g_pwm_dma_chan_a = PWM_DMA_CH_A;
    g_pwm_dma_chan_b = PWM_DMA_CH_B;

    // Канал A → chain B, канал B → chain A (ping-pong)
    dma_channel_config cfg_a = dma_channel_get_default_config(g_pwm_dma_chan_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_a, true);
    channel_config_set_write_increment(&cfg_a, false);
    channel_config_set_dreq(&cfg_a, pwm_get_dreq(g_pwm_slice));
    channel_config_set_chain_to(&cfg_a, g_pwm_dma_chan_b);

    dma_channel_config cfg_b = dma_channel_get_default_config(g_pwm_dma_chan_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_b, true);
    channel_config_set_write_increment(&cfg_b, false);
    channel_config_set_dreq(&cfg_b, pwm_get_dreq(g_pwm_slice));
    channel_config_set_chain_to(&cfg_b, g_pwm_dma_chan_a);

    dma_channel_configure(g_pwm_dma_chan_a, &cfg_a,
        &pwm_hw->slice[g_pwm_slice].cc,
        pwm_dma_buffers[0], g_pwm_dma_count, false);

    dma_channel_configure(g_pwm_dma_chan_b, &cfg_b,
        &pwm_hw->slice[g_pwm_slice].cc,
        pwm_dma_buffers[1], g_pwm_dma_count, false);

    // Подключить IRQ1 (не конфликтует с VGA на IRQ0)
    // Если I2S тоже использует IRQ1 — нужен общий handler; здесь PWM и I2S
    // не могут быть активны одновременно (определяется FEATURE_AUDIO_PWM /
    // FEATURE_AUDIO_I2S), поэтому exclusive handler безопасен.
    dma_hw->ints1 = (1u << g_pwm_dma_chan_a) | (1u << g_pwm_dma_chan_b);
    irq_set_exclusive_handler(AUDIO_DMA_IRQ, audio_pwm_dma_irq_handler);
    irq_set_priority(AUDIO_DMA_IRQ, 0x80);
    irq_set_enabled(AUDIO_DMA_IRQ, true);
    dma_channel_set_irq1_enabled(g_pwm_dma_chan_a, true);
    dma_channel_set_irq1_enabled(g_pwm_dma_chan_b, true);

    audio_running = false;
    DBG_PRINT("Audio: PWM ping-pong DMA ready (ch %d/%d, IRQ=%d)\n",
              g_pwm_dma_chan_a, g_pwm_dma_chan_b, AUDIO_DMA_IRQ);
}

static inline uint16_t s16_to_pwm_u16(int16_t s) {
    int32_t v = (int32_t)s + 32768;      // 0..65535
    v >>= (16 - PWM_BITS);               // -> 0..PWM_WRAP
    if (v < 0) v = 0;
    if (v > PWM_WRAP) v = PWM_WRAP;
    return (uint16_t)v;
}

static inline uint32_t pack_pwm_cc(uint16_t left, uint16_t right) {
    return ((uint32_t)right << 16) | left;
}

void pwm_dma_write_count(const int16_t *samples, uint32_t sample_count)
{
    if (!samples || g_pwm_dma_chan_a < 0)
        return;

    if (sample_count > g_pwm_dma_count)
        sample_count = g_pwm_dma_count;

    // Выбрать свободный буфер без блокировки
    uint8_t buf_index = 0;
    int timeout = 10000;
    while (true) {
        uint32_t irq_state = save_and_disable_interrupts();
        uint32_t free_mask = pwm_buffers_free_mask;

        if (!audio_running) {
            // Pre-roll: заполнить буферы по порядку
            buf_index = (uint8_t)pwm_preroll_count;
            if (buf_index < 2 && (free_mask & (1u << buf_index))) {
                pwm_buffers_free_mask &= ~(1u << buf_index);
                restore_interrupts(irq_state);
                break;
            }
        } else if (free_mask) {
            buf_index = (free_mask & 1u) ? 0 : 1;
            pwm_buffers_free_mask &= ~(1u << buf_index);
            restore_interrupts(irq_state);
            break;
        }

        restore_interrupts(irq_state);
        if (--timeout <= 0) return;  // буфер не освободился — пропустить кадр
        tight_loop_contents();
    }

    // Конвертировать и заполнить буфер
    uint32_t *dst = pwm_dma_buffers[buf_index];
    for (uint32_t i = 0; i < sample_count; i++) {
        dst[i] = pack_pwm_cc(s16_to_pwm_u16(samples[i * 2]),
                             s16_to_pwm_u16(samples[i * 2 + 1]));
    }
    uint32_t mid = pack_pwm_cc(PWM_WRAP >> 1, PWM_WRAP >> 1);
    for (uint32_t i = sample_count; i < g_pwm_dma_count; i++)
        dst[i] = mid;

    __dmb();

    if (!audio_running) {
        pwm_preroll_count++;
        if (pwm_preroll_count >= PWM_PREROLL_BUFFERS) {
            // Оба буфера заполнены — запустить воспроизведение
            dma_channel_start(g_pwm_dma_chan_a);
            audio_running = true;
        }
    } else {
        // Защита от underrun: перезапустить цепочку если встала
        if (!dma_channel_is_busy(g_pwm_dma_chan_a) &&
            !dma_channel_is_busy(g_pwm_dma_chan_b)) {
            if (buf_index == 0) dma_channel_start(g_pwm_dma_chan_a);
            else                dma_channel_start(g_pwm_dma_chan_b);
        }
    }
}

#endif
#endif

//=============================================================================
// High-level Audio API
//=============================================================================

static bool audio_initialized = false;
static bool audio_enabled = true;
static int master_volume = 160;  // Default to moderate amplification (x8)

// Startup mute: output silence for first N frames to let hardware settle
#define STARTUP_FADE_FRAMES 60  // ~1 second at 60fps
static int startup_frame_counter = 0;

// Mixed stereo buffer (16-bit stereo samples)
static int16_t __attribute__((aligned(4))) mixed_buffer[AUDIO_BUFFER_SAMPLES * 2];

// Track last sample for single-point frame-boundary interpolation
static int16_t last_sample_l = 0;
static int16_t last_sample_r = 0;

bool audio_init(void) {
    // Reset all state variables
    audio_initialized = false;
    audio_running = false;
    pio_sm_enabled = false;
    startup_frame_counter = 0;
#if FEATURE_AUDIO_I2S
    i2s_init();
#else
#if defined(FEATURE_AUDIO_PWM)
    audio_pwm_init();
#endif
#endif
    // Apply default master volume options
    audio_set_volume(master_volume);

    audio_initialized = true;

    return true;
}

void audio_process_frame(void *pc) {
    if (!audio_initialized || !audio_enabled) return;

    // Startup mute: output pure silence for first N frames
    if (startup_frame_counter < STARTUP_FADE_FRAMES) {
        memset(mixed_buffer, 0, TARGET_SAMPLES_PER_FRAME * 2 * sizeof(int16_t));
        startup_frame_counter++;
        last_sample_l = 0;
        last_sample_r = 0;
#if FEATURE_AUDIO_I2S
        i2s_dma_write_count(mixed_buffer, TARGET_SAMPLES_PER_FRAME);
#else
#if defined(FEATURE_AUDIO_PWM)
        pwm_dma_write_count(mixed_buffer, TARGET_SAMPLES_PER_FRAME);
#endif
#endif
        return;
    }

    // Clear buffer
    memset(mixed_buffer, 0, TARGET_SAMPLES_PER_FRAME * 2 * sizeof(int16_t));

    // Call the PC mixer to fill the buffer
    // mixer_callback expects bytes: samples * 2 channels * 2 bytes per sample
    if (pc) {
        mixer_callback(pc, (uint8_t *)mixed_buffer, TARGET_SAMPLES_PER_FRAME * 4);
    }

    // Anti-click: single midpoint sample at frame boundary only.
    // A 32-sample fade destroys Tandy/PCSpk square waves (60Hz amplitude
    // modulation) and smears DSS transients (fake echo). SB16/Adlib don't
    // need it - their PCM is already smooth across frame boundaries.
    // One interpolated sample is enough to kill the DC-offset click.
    if (mixed_buffer[0] != last_sample_l || mixed_buffer[1] != last_sample_r) {
        mixed_buffer[0] = (int16_t)((last_sample_l + mixed_buffer[0]) >> 1);
        mixed_buffer[1] = (int16_t)((last_sample_r + mixed_buffer[1]) >> 1);
    }

    // Store last samples for next frame's anti-click processing
    last_sample_l = mixed_buffer[(TARGET_SAMPLES_PER_FRAME - 1) * 2];
    last_sample_r = mixed_buffer[(TARGET_SAMPLES_PER_FRAME - 1) * 2 + 1];

#if FEATURE_AUDIO_I2S
    // Submit to I2S
    i2s_dma_write_count(mixed_buffer, TARGET_SAMPLES_PER_FRAME);
#else
#if defined(FEATURE_AUDIO_PWM)
    pwm_dma_write_count(mixed_buffer, TARGET_SAMPLES_PER_FRAME);
#endif
#endif
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
        // 129..255 -> Amplification (shift -1..-8)
        // (val - 128) -> 1..127.
        // >> 4 -> 0..7
        // +1 -> 1..8
        // Negate -> -1..-8
        shift = -((volume - 128) >> 4) - 1;
        if (shift < -8) shift = -8;
    }
#if FEATURE_AUDIO_I2S
    i2s_volume(shift);
#else
#if defined(FEATURE_AUDIO_PWM)
    /// TODO:
#endif
#endif
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

bool __not_in_flash() audio_needs_samples(void) {
    if (!audio_initialized || !audio_enabled) return false;
#if FEATURE_AUDIO_I2S
    return (dma_buffers_free_mask != 0);
#elif defined(FEATURE_AUDIO_PWM)
    return (pwm_buffers_free_mask != 0);
#else
    return false;
#endif
}
