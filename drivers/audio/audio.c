/**
 * MIT License
 *
 * Copyright (c) 2022 Vincent Mistler
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "pc.h"
#include "audio.h"
#include "dss.h"
#include "adlib.h"
#include "board_config.h"
#include <pico/time.h>

#ifdef FEATURE_AUDIO_PWM
#include <hardware/pwm.h>
#include <hardware/clocks.h>
#endif

static uint8_t volume = 0; // 0 - MAX vol, 16 - silece (for i2s, for pwm - 12)

#ifdef FEATURE_AUDIO_I2S
/**
 * return the default i2s context used to store information about the setup
 */
i2s_config_t i2s_get_default_config(void) {
    i2s_config_t i2s_config = {
            .sample_freq = 44100,
            .channel_count = 2,
    		.data_pin = I2S_DATA_PIN,
	    	.clock_pin_base = I2S_CLOCK_PIN_BASE,
            .pio = pio2, // RP2350 only
            .sm = 0,
            .dma_channel = 0,
            .dma_buf = NULL,
            .dma_trans_count = 0
    };
    return i2s_config;
}

/**
 * Initialize the I2S driver. Must be called before calling i2s_write or i2s_dma_write
 * i2s_config: I2S context obtained by i2s_get_default_config()
 */
void i2s_init(i2s_config_t *i2s_config) {
    uint8_t func;
    if      (i2s_config->pio == pio0) func = GPIO_FUNC_PIO0;
    else if (i2s_config->pio == pio1) func = GPIO_FUNC_PIO1;
    else                              func = GPIO_FUNC_PIO2;  // RP2350 only
    gpio_set_function(i2s_config->data_pin, func);
    gpio_set_function(i2s_config->clock_pin_base, func);
    gpio_set_function(i2s_config->clock_pin_base + 1, func);

    i2s_config->sm = pio_claim_unused_sm(i2s_config->pio, true);

    /* Set PIO clock */
    uint32_t system_clock_frequency = clock_get_hz(clk_sys);
    uint32_t divider = system_clock_frequency * 4 / i2s_config->sample_freq; // avoid arithmetic overflow

#ifdef I2S_CS4334
    uint offset = pio_add_program(i2s_config->pio, &audio_i2s_cs4334_program);
    audio_i2s_cs4334_program_init(i2s_config->pio, i2s_config->sm , offset, i2s_config->data_pin , i2s_config->clock_pin_base);
    divider >>= 3;
#else
    uint offset = pio_add_program(i2s_config->pio, &audio_i2s_program);
    audio_i2s_program_init(i2s_config->pio, i2s_config->sm, offset, i2s_config->data_pin, i2s_config->clock_pin_base);
#endif

    pio_sm_set_clkdiv_int_frac(i2s_config->pio, i2s_config->sm, divider >> 8u, divider & 0xffu);

    pio_sm_set_enabled(i2s_config->pio, i2s_config->sm, false);
    /* Allocate memory for the DMA buffer */
    i2s_config->dma_buf = malloc(i2s_config->dma_trans_count * sizeof(uint32_t));

    /* Direct Memory Access setup */
    i2s_config->dma_channel = dma_claim_unused_channel(true);

    dma_channel_config dma_config = dma_channel_get_default_config(i2s_config->dma_channel);
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);

    volatile uint32_t *addr_write_DMA = &(i2s_config->pio->txf[i2s_config->sm]);
    channel_config_set_dreq(&dma_config, pio_get_dreq(i2s_config->pio, i2s_config->sm, true));
    dma_channel_configure(i2s_config->dma_channel,
                          &dma_config,
                          addr_write_DMA,    // Destination pointer
                          i2s_config->dma_buf,                        // Source pointer
                          i2s_config->dma_trans_count,                // Number of 32 bits words to transfer
                          false                                       // Start immediately
    );
    pio_sm_set_enabled(i2s_config->pio, i2s_config->sm, true);
}

/**
 * Write samples to I2S directly and wait for completion (blocking)
 * i2s_config: I2S context obtained by i2s_get_default_config()
 *     sample: pointer to an array of len x 32 bits samples
 *             Each 32 bits sample contains 2x16 bits samples, 
 *             one for the left channel and one for the right channel
 *        len: length of sample in 32 bits words
 */
void i2s_write(const i2s_config_t *i2s_config, const int16_t *samples, const size_t len) {
    for (size_t i = 0; i < len; i++) {
        pio_sm_put_blocking(i2s_config->pio, i2s_config->sm, (uint32_t) samples[i]);
    }
}

/**
 * Write samples to DMA buffer and initiate DMA transfer (non blocking)
 * i2s_config: I2S context obtained by i2s_get_default_config()
 *     sample: pointer to an array of dma_trans_count x 32 bits samples
 */
void __not_in_flash_func(i2s_dma_write)(i2s_config_t *i2s_config, const int16_t *samples) {
    /* Wait the completion of the previous DMA transfer */
    dma_channel_wait_for_finish_blocking(i2s_config->dma_channel);
    /* Copy samples into the DMA buffer */
    for (int i = 0; i < i2s_config->dma_trans_count * 2; ++i) {
        i2s_config->dma_buf[i] = samples[i] >> volume;
    }
    /* Initiate the DMA transfer */
    dma_channel_transfer_from_buffer_now(i2s_config->dma_channel,
                                         i2s_config->dma_buf,
                                         i2s_config->dma_trans_count);
}

/**
 * Adjust the output volume
 * i2s_config: I2S context obtained by i2s_get_default_config()
 *     volume: desired volume between 0 (highest. volume) and 16 (lowest volume)
 */
void i2s_volume(i2s_config_t *i2s_config, uint8_t vol) {
    if (vol > 16) vol = 16;
    volume = vol;
}

/**
 * Increases the output volume
 */
void i2s_increase_volume(i2s_config_t *i2s_config) {
    if (volume > 0) {
        volume--;
    }
}

/**
 * Decreases the output volume
 */
void i2s_decrease_volume(i2s_config_t *i2s_config) {
    if (volume < 16) {
        volume++;
    }
}

static i2s_config_t i2s_config;
#elif FEATURE_AUDIO_PWM || FEATURE_AUDIO_HW
static pwm_config pwm;
#endif

static uint8_t prev = 0;
void audio_set_enabled(bool v) {
    if (v) {
        volume = prev;
    } else {
        prev = volume;
        volume = 16;
    }
}

void audio_set_volume(uint8_t vol) {
    prev = 16 - vol;
}

uint8_t audio_get_volume(void) {
    return 16 - prev;
}


void audio_init(void) {
#if FEATURE_AUDIO_I2S
    i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = SOUND_FREQUENCY;
    i2s_config.dma_trans_count = 1;
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);
    sleep_ms(100);
#elif FEATURE_AUDIO_PWM
    pwm = pwm_get_default_config();
    gpio_set_function(PWM_LEFT_PIN, GPIO_FUNC_PWM);
    gpio_set_function(PWM_RIGHT_PIN, GPIO_FUNC_PWM);
    #ifdef BEEPER_PIN
        gpio_set_function(BEEPER_PIN, GPIO_FUNC_PWM);
    #endif
    pwm_config_set_clkdiv(&pwm, 1.0f);
    pwm_config_set_wrap(&pwm, (1 << 12) - 1); // MAX PWM value
    uint sln_l = pwm_gpio_to_slice_num(PWM_LEFT_PIN);
    pwm_init(sln_l, &pwm, true);
    uint sln_r = pwm_gpio_to_slice_num(PWM_RIGHT_PIN);
    if (sln_r != sln_l) pwm_init(sln_r, &pwm, true);
    #ifdef BEEPER_PIN
        uint sln_b = pwm_gpio_to_slice_num(BEEPER_PIN);
        if (sln_r != sln_b && sln_l != sln_b) pwm_init(sln_b, &pwm, true);
    #endif
#elif FEATURE_AUDIO_HW
    init_74hc595();
    pwm = pwm_get_default_config();
    gpio_set_function(PCM_PIN, GPIO_FUNC_PWM);
    pwm_config_set_clkdiv(&pwm, 1.0f);
    pwm_config_set_wrap(&pwm, (1 << 12) - 1); // MAX PWM value
    pwm_init(pwm_gpio_to_slice_num(PCM_PIN), &pwm, true);
#endif
}

static int16_t samples[2] = { 0 };

//=============================================================================
// Core 1 Entry Point (Audio processing)
//=============================================================================
bool __not_in_flash_func(timer_callback)(repeating_timer_t *rt) {
    static uint64_t t_dss = 0;
    static int dss_v = 0;
    PC* pc = (PC*)rt->user_data;
    // Disney Sound Source 7 kHz
    if (pc->dss_enabled) {
        uint64_t t = time_us_64();
        if (t - t_dss >= 1000000 / 7000) { // 142 us for 7 kHz
            t_dss = t;
            dss_v = dss_sample();
        }
    }
    // output:
    int b_v = 0;
    int r_v = 0;
    int l_v = 0;
    if (pc->pcspk_enabled) {
        b_v = pcspk_sample(pc->pcspk);
    }
    if (pc->covox_enabled && pc->covox_sample) {
        int16_t sample = ((int16_t)pc->covox_sample - 127) << 8;
        r_v += sample;
        l_v += sample;
    }
    if (pc->tandy_enabled) {
        int16_t sample = sn76489_sample(); // 16-bit
        r_v += sample;
        l_v += sample;
    }
    if (pc->mpu401_enabled) {
        int16_t sample = midi_sample();
        r_v += sample;
        l_v += sample;
    }
    if (pc->adlib_enabled) {
        int16_t sample = adlib_getsample(pc->adlib);
        r_v += sample;
        l_v += sample;
    }
    if (pc->sb16_enabled) {
        sb16_getsample(pc->sb16, &r_v, &l_v);
    }
    r_v += dss_v;
    l_v += dss_v;
    #if FEATURE_AUDIO_PWM
        r_v >>= volume;
        l_v >>= volume;
        uint16_t ur_v = (r_v + 32768) >> 4; // 16 signed bit to 12 unsigned
        uint16_t ul_v = (l_v + 32768) >> 4;
        if (ur_v > 4095) ur_v = 4095;
        if (ul_v > 4095) ul_v = 4095;
        #ifdef BEEPER_PIN
            b_v = b_v ? (4095 >> volume) : 0;
            pwm_set_gpio_level(BEEPER_PIN, b_v);
        #else
            if (b_v) { r_v = l_v = 32767; }
        #endif
        pwm_set_gpio_level(PWM_RIGHT_PIN, ur_v);
        pwm_set_gpio_level(PWM_LEFT_PIN, ul_v);
    #elif FEATURE_AUDIO_I2S
        if (b_v) { r_v = l_v = 32767; }
        if (r_v > 32767) r_v = 32767;
        if (r_v < -32768) r_v = -32768;
        if (l_v > 32767) l_v = 32767;
        if (l_v < -32768) l_v = -32768;
        samples[0] = r_v;
        samples[1] = l_v;
    #endif
    return true;
}

// to call DMA-wait not from ISR for timer
bool __not_in_flash_func(repeat_me_often)(void) {
    #if FEATURE_AUDIO_I2S
        i2s_dma_write(&i2s_config, samples);
    #endif
}
