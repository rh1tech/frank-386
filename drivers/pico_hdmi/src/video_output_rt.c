#include "pico_hdmi/video_output_rt.h"

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/hstx_pins.h"

#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/sync.h"

#include <math.h>
#include <string.h>

// ============================================================================
// DVI/HSTX Constants
// ============================================================================

#define TMDS_CTRL_00 0x354u // vsync=0 hsync=0
#define TMDS_CTRL_01 0x0abu // vsync=0 hsync=1
#define TMDS_CTRL_10 0x154u // vsync=1 hsync=0
#define TMDS_CTRL_11 0x2abu // vsync=1 hsync=1

// Sync symbols: Lane 0 carries sync, Lanes 1&2 are always CTRL_00
#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

// Data Island preamble: Lane 0 = sync, Lanes 1&2 = CTRL_01 pattern
// Per HDMI 1.3a Table 5-2: CTL0=1, CTL1=0, CTL2=1, CTL3=0
#define PREAMBLE_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))

// Video preamble: Lane 0 = sync, Lane 1 = CTRL_01, Lane 2 = CTRL_00
// Per HDMI 1.3a Table 5-2: CTL0=1, CTL1=0, CTL2=0, CTL3=0
#define VIDEO_PREAMBLE_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))
#define VIDEO_PREAMBLE_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))

// Video guard band: Per HDMI 1.3a Table 5-5
// CH0 = 0b1011001100 (0x2CC), CH1 = 0b0100110011 (0x133), CH2 = 0b1011001100 (0x2CC)
#define VIDEO_GUARD_BAND (0x2CCu | (0x133u << 10) | (0x2CCu << 20))

#define HSTX_CMD_RAW (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT (0x1u << 12)
#define HSTX_CMD_TMDS (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP (0xfu << 12)

// Video preamble and guard band widths (HDMI 1.3a Section 5.2.2)
#define W_VIDEO_PREAMBLE 8
#define W_VIDEO_GUARD_BAND 2

// ============================================================================
// Runtime Video Mode Definitions
// ============================================================================

const video_mode_t video_mode_480_p = {
    .h_front_porch = 16,
    .h_sync_width = 96,
    .h_back_porch = 48,
    .h_active_pixels = 640,
    .v_front_porch = 10,
    .v_sync_width = 2,
    .v_back_porch = 33,
    .v_active_lines = 480,
    .h_total_pixels = 800,
    .v_total_lines = 525,
    .hstx_clk_div = 1,
    .hstx_csr_clkdiv = 5,
};

const video_mode_t video_mode_240_p = {
    .h_front_porch = 32,
    .h_sync_width = 192,
    .h_back_porch = 96,
    .h_active_pixels = 1280,
    .v_front_porch = 4,
    .v_sync_width = 4,
    .v_back_porch = 14,
    .v_active_lines = 240,
    .h_total_pixels = 1600,
    .v_total_lines = 262,
    .hstx_clk_div = 1,
    .hstx_csr_clkdiv = 5,
};

const video_mode_t *video_output_active_mode = &video_mode_480_p;

// ============================================================================
// ISR-Cached Timing Variables (written by apply_mode, read by ISR)
// ============================================================================

static uint16_t rt_h_front_porch;
static uint16_t rt_h_sync_width;
static uint16_t rt_h_back_porch;
static uint16_t rt_h_active_pixels;
static uint16_t rt_v_front_porch;
static uint16_t rt_v_sync_width;
static uint16_t rt_v_back_porch;
static uint16_t rt_v_active_lines;
static uint16_t rt_v_total_lines;
static uint16_t rt_sync_after_di;

// ============================================================================
// Audio/Video State
// ============================================================================

uint16_t frame_width = 0;
uint16_t frame_height = 0;
volatile uint32_t video_frame_count = 0;

// DVI mode: when true, disables all HDMI Data Islands (pure DVI output, no audio)
static bool dvi_mode = false;

// Max active pixels across all modes (1280 for 240p)
static uint16_t line_buffer[1280] __attribute__((aligned(4)));
static uint32_t v_scanline = 2;
static bool vactive_cmdlist_posted = false;
static bool dma_pong = false;

static video_output_task_fn background_task = NULL;
static video_output_scanline_cb_t scanline_callback = NULL;
static video_output_vsync_cb_t vsync_callback = NULL;

static uint32_t current_sample_rate = 48000;

// Mode switch / resync flags (set by Core 0, consumed by Core 1)
static volatile const video_mode_t *pending_mode = NULL;
static volatile bool resync_requested = false;

#define DMACH_PING 0
#define DMACH_PONG 1

// ============================================================================
// Command Lists (runtime-filled)
// ============================================================================

// Pure DVI command lists (9 words each)
static uint32_t vblank_line_vsync_off[9];
static uint32_t vblank_line_vsync_on[9];
static uint32_t vactive_line_dvi[9];

static uint32_t vactive_di_ping[128], vactive_di_pong[128], vactive_di_null[128];
static uint32_t vactive_di_len, vactive_di_null_len;

static uint32_t vblank_di_ping[128], vblank_di_pong[128], vblank_di_null[128];
static uint32_t vblank_di_len, vblank_di_null_len;

static uint32_t vblank_acr_vsync_on[64], vblank_acr_vsync_on_len;
static uint32_t vblank_acr_vsync_off[64], vblank_acr_vsync_off_len;
static uint32_t vblank_infoframe_vsync_on[64], vblank_infoframe_vsync_on_len;
static uint32_t vblank_infoframe_vsync_off[64], vblank_infoframe_vsync_off_len;
static uint32_t vblank_avi_infoframe[64], vblank_avi_infoframe_len;

// ACR command lists for genlock (custom CTS values)
static uint32_t genlock_acr_vsync_on[64], genlock_acr_vsync_on_len;
static uint32_t genlock_acr_vsync_off[64], genlock_acr_vsync_off_len;
static bool use_genlock_acr = false;

// ============================================================================
// Build DVI Command Lists
// ============================================================================

static void build_dvi_command_lists(void)
{
    // vblank_line_vsync_off: vsync=1, hsync toggling
    vblank_line_vsync_off[0] = HSTX_CMD_RAW_REPEAT | rt_h_front_porch;
    vblank_line_vsync_off[1] = SYNC_V1_H1;
    vblank_line_vsync_off[2] = HSTX_CMD_NOP;
    vblank_line_vsync_off[3] = HSTX_CMD_RAW_REPEAT | rt_h_sync_width;
    vblank_line_vsync_off[4] = SYNC_V1_H0;
    vblank_line_vsync_off[5] = HSTX_CMD_NOP;
    vblank_line_vsync_off[6] = HSTX_CMD_RAW_REPEAT | (rt_h_back_porch + rt_h_active_pixels);
    vblank_line_vsync_off[7] = SYNC_V1_H1;
    vblank_line_vsync_off[8] = HSTX_CMD_NOP;

    // vblank_line_vsync_on: vsync=0, hsync toggling
    vblank_line_vsync_on[0] = HSTX_CMD_RAW_REPEAT | rt_h_front_porch;
    vblank_line_vsync_on[1] = SYNC_V0_H1;
    vblank_line_vsync_on[2] = HSTX_CMD_NOP;
    vblank_line_vsync_on[3] = HSTX_CMD_RAW_REPEAT | rt_h_sync_width;
    vblank_line_vsync_on[4] = SYNC_V0_H0;
    vblank_line_vsync_on[5] = HSTX_CMD_NOP;
    vblank_line_vsync_on[6] = HSTX_CMD_RAW_REPEAT | (rt_h_back_porch + rt_h_active_pixels);
    vblank_line_vsync_on[7] = SYNC_V0_H1;
    vblank_line_vsync_on[8] = HSTX_CMD_NOP;

    // vactive_line_dvi: active video for DVI (no Data Islands)
    vactive_line_dvi[0] = HSTX_CMD_RAW_REPEAT | rt_h_front_porch;
    vactive_line_dvi[1] = SYNC_V1_H1;
    vactive_line_dvi[2] = HSTX_CMD_NOP;
    vactive_line_dvi[3] = HSTX_CMD_RAW_REPEAT | rt_h_sync_width;
    vactive_line_dvi[4] = SYNC_V1_H0;
    vactive_line_dvi[5] = HSTX_CMD_NOP;
    vactive_line_dvi[6] = HSTX_CMD_RAW_REPEAT | rt_h_back_porch;
    vactive_line_dvi[7] = SYNC_V1_H1;
    vactive_line_dvi[8] = HSTX_CMD_TMDS | rt_h_active_pixels;
}

// ============================================================================
// HSTX Resync - Reset output to sync with input VSYNC
// ============================================================================

static void __scratch_x("") hstx_resync(void)
{
    // 1. Abort DMA chains
    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);

    // 2. Disconnect GPIO before disabling HSTX (no garbage on pins)
    for (int i = PIN_HSTX_CLK; i <= PIN_HSTX_D2 + 1; ++i)
        gpio_set_function(i, GPIO_FUNC_SIO);

    // 3. Disable HSTX (resets shift register, clock generator, and flushes FIFO)
    hstx_ctrl_hw->csr &= ~HSTX_CTRL_CSR_EN_BITS;

    // Small delay to ensure HSTX fully stops
    __asm volatile("nop\nnop\nnop\nnop");

    // 4. Reset state to start of frame
    v_scanline = 0;
    vactive_cmdlist_posted = false;
    dma_pong = false;

    // 5. Clear any pending DMA interrupts
    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);

    // 6. Configure DMA PING to start from beginning of frame (Line 0)
    dma_channel_hw_t *ch_ping = &dma_hw->ch[DMACH_PING];
    ch_ping->read_addr = (uintptr_t)vblank_line_vsync_off;
    ch_ping->transfer_count = 9;

    // 7. Configure DMA PONG for the NEXT line (Line 1)
    dma_channel_hw_t *ch_pong = &dma_hw->ch[DMACH_PONG];
    ch_pong->read_addr = (uintptr_t)vblank_line_vsync_off;
    ch_pong->transfer_count = 9;

    // 8. Re-enable HSTX and start DMA (output goes nowhere — GPIO disconnected)
    hstx_ctrl_hw->csr |= HSTX_CTRL_CSR_EN_BITS;
    dma_channel_start(DMACH_PING);

    // 9. Wait for first valid line to serialize
    while (dma_channel_is_busy(DMACH_PING))
        tight_loop_contents();

    // 10. Reconnect GPIO — TV sees valid TMDS immediately
    for (int i = PIN_HSTX_CLK; i <= PIN_HSTX_D2 + 1; ++i) {
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_4MA);
        gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
        gpio_set_function(i, 0);
    }
}

// ============================================================================
// Internal Helpers
// ============================================================================

static uint32_t build_line_with_di(uint32_t *buf, const uint32_t *di_words, bool vsync, bool active)
{
    uint32_t *p = buf;
    uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
    uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
    uint32_t preamble = vsync ? PREAMBLE_V0_H0 : PREAMBLE_V1_H0;

    *p++ = HSTX_CMD_RAW_REPEAT | rt_h_front_porch;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
    *p++ = preamble;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
    for (int i = 0; i < W_DATA_ISLAND; i++)
        *p++ = di_words[i];
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | rt_sync_after_di;
    *p++ = sync_h0;
    *p++ = HSTX_CMD_NOP;

    if (active) {
        // HDMI 1.3a Section 5.2.2: Video Data Period requires preamble and guard band
        uint32_t video_preamble = vsync ? VIDEO_PREAMBLE_V0_H1 : VIDEO_PREAMBLE_V1_H1;

        // Control period (back porch minus preamble and guard band)
        *p++ = HSTX_CMD_RAW_REPEAT | (rt_h_back_porch - W_VIDEO_PREAMBLE - W_VIDEO_GUARD_BAND);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;

        // Video Preamble (8 pixels)
        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_PREAMBLE;
        *p++ = video_preamble;
        *p++ = HSTX_CMD_NOP;

        // Video Guard Band (2 pixels)
        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_GUARD_BAND;
        *p++ = VIDEO_GUARD_BAND;

        // Active video pixels
        *p++ = HSTX_CMD_TMDS | rt_h_active_pixels;
    } else {
        *p++ = HSTX_CMD_RAW_REPEAT | (rt_h_back_porch + rt_h_active_pixels);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;
    }
    return (uint32_t)(p - buf);
}

typedef struct {
    bool vsync_active;
    bool front_porch;
    bool back_porch;
    bool active_video;
    bool send_acr;
    uint32_t active_line;
} scanline_state_t;

static inline void __scratch_x("") get_scanline_state(uint32_t v_scanline, scanline_state_t *state)
{
    state->vsync_active = (v_scanline >= rt_v_front_porch && v_scanline < (rt_v_front_porch + rt_v_sync_width));
    state->front_porch = (v_scanline < rt_v_front_porch);
    state->back_porch = (v_scanline >= rt_v_front_porch + rt_v_sync_width &&
                         v_scanline < rt_v_front_porch + rt_v_sync_width + rt_v_back_porch);
    state->active_video = (!state->vsync_active && !state->front_porch && !state->back_porch);

    state->send_acr = (v_scanline >= (rt_v_front_porch + rt_v_sync_width) &&
                       v_scanline < (rt_v_total_lines - rt_v_active_lines) && (v_scanline % 4 == 0));

    if (state->active_video) {
        state->active_line = v_scanline - (rt_v_total_lines - rt_v_active_lines);
    } else {
        state->active_line = 0;
    }
}

static inline void __scratch_x("") video_output_handle_vsync(dma_channel_hw_t *ch, uint32_t v_scanline)
{
    if (dvi_mode) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = 9;
        if (v_scanline == rt_v_front_porch) {
            video_frame_count++;
            if (vsync_callback)
                vsync_callback();
        }
    } else {
        if (v_scanline == rt_v_front_porch) {
            if (use_genlock_acr) {
                ch->read_addr = (uintptr_t)genlock_acr_vsync_on;
                ch->transfer_count = genlock_acr_vsync_on_len;
            } else {
                ch->read_addr = (uintptr_t)vblank_acr_vsync_on;
                ch->transfer_count = vblank_acr_vsync_on_len;
            }
            video_frame_count++;
            if (vsync_callback)
                vsync_callback();
        } else {
            ch->read_addr = (uintptr_t)vblank_infoframe_vsync_on;
            ch->transfer_count = vblank_infoframe_vsync_on_len;
        }
    }
}

static inline void __scratch_x("")
    video_output_handle_active_start(dma_channel_hw_t *ch, uint32_t v_scanline, uint32_t active_line, bool dma_pong)
{
    uint32_t *dst32 = (uint32_t *)line_buffer;

    if (scanline_callback) {
        scanline_callback(v_scanline, active_line, dst32);
    } else {
        for (uint32_t i = 0; i < rt_h_active_pixels / 2; i++) {
            dst32[i] = 0;
        }
    }

    if (dvi_mode) {
        ch->read_addr = (uintptr_t)vactive_line_dvi;
        ch->transfer_count = 9;
    } else {
        uint32_t *buf = dma_pong ? vactive_di_ping : vactive_di_pong;
        const uint32_t *di_words = hstx_di_queue_get_audio_packet();
        if (di_words) {
            vactive_di_len = build_line_with_di(buf, di_words, false, true);
            ch->read_addr = (uintptr_t)buf;
            ch->transfer_count = vactive_di_len;
        } else {
            ch->read_addr = (uintptr_t)vactive_di_null;
            ch->transfer_count = vactive_di_null_len;
        }
    }
}

static inline void __scratch_x("")
    video_output_handle_blanking(dma_channel_hw_t *ch, uint32_t v_scanline, bool send_acr, bool dma_pong)
{
    if (dvi_mode) {
        (void)send_acr;
        (void)dma_pong;
        (void)v_scanline;
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = 9;
    } else {
        if (send_acr) {
            if (use_genlock_acr) {
                ch->read_addr = (uintptr_t)genlock_acr_vsync_off;
                ch->transfer_count = genlock_acr_vsync_off_len;
            } else {
                ch->read_addr = (uintptr_t)vblank_acr_vsync_off;
                ch->transfer_count = vblank_acr_vsync_off_len;
            }
        } else if (v_scanline == 0) {
            ch->read_addr = (uintptr_t)vblank_avi_infoframe;
            ch->transfer_count = vblank_avi_infoframe_len;
        } else {
            const uint32_t *di_words = hstx_di_queue_get_audio_packet();
            if (di_words) {
                uint32_t *buf = dma_pong ? vblank_di_ping : vblank_di_pong;
                vblank_di_len = build_line_with_di(buf, di_words, false, false);
                ch->read_addr = (uintptr_t)buf;
                ch->transfer_count = vblank_di_len;
            } else {
                ch->read_addr = (uintptr_t)vblank_di_null;
                ch->transfer_count = vblank_di_null_len;
            }
        }
    }
}

static inline void __scratch_x("") video_output_handle_active_data(dma_channel_hw_t *ch)
{
    ch->read_addr = (uintptr_t)line_buffer;
    ch->transfer_count = (rt_h_active_pixels * sizeof(uint16_t)) / sizeof(uint32_t);
}

// ============================================================================
// DMA IRQ Handler
// ============================================================================

void __scratch_x("") dma_irq_handler()
{
    uint32_t ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1U << ch_num;
    dma_pong = !dma_pong;

    // Advance audio/data-island scheduler exactly once per scanline (HDMI mode only)
    if (!dvi_mode && !vactive_cmdlist_posted) {
        hstx_di_queue_tick();
    }

    scanline_state_t state;
    get_scanline_state(v_scanline, &state);

    if (state.vsync_active) {
        video_output_handle_vsync(ch, v_scanline);
    } else if (state.active_video && !vactive_cmdlist_posted) {
        video_output_handle_active_start(ch, v_scanline, state.active_line, dma_pong);
        vactive_cmdlist_posted = true;
    } else if (state.active_video && vactive_cmdlist_posted) {
        video_output_handle_active_data(ch);
        vactive_cmdlist_posted = false;
    } else {
        video_output_handle_blanking(ch, v_scanline, state.send_acr, dma_pong);
    }
    if (!vactive_cmdlist_posted)
        v_scanline = (v_scanline + 1) % rt_v_total_lines;
}

// ============================================================================
// Apply Mode
// ============================================================================

// ACR N/CTS lookup for 25.2 MHz pixel clock (HDMI spec Table 7-1/7-2)
static void get_acr_params(uint32_t sample_rate, uint32_t *n, uint32_t *cts)
{
    switch (sample_rate) {
        case 32000:
            *n = 4096;
            *cts = 25200;
            break;
        case 44100:
            *n = 6272;
            *cts = 28000;
            break;
        case 48000:
            *n = 6144;
            *cts = 25200;
            break;
        case 88200:
            *n = 12544;
            *cts = 28000;
            break;
        case 96000:
            *n = 12288;
            *cts = 25200;
            break;
        case 176400:
            *n = 25088;
            *cts = 28000;
            break;
        case 192000:
            *n = 24576;
            *cts = 25200;
            break;
        default:
            *n = 6144;
            *cts = 25200;
            break; // fallback to 48kHz
    }
}

static void configure_audio_packets(uint32_t sample_rate)
{
    hstx_di_queue_set_sample_rate(sample_rate);

    // Override samples_per_line with pixel-clock-accurate value.
    // The default set_sample_rate() assumes exactly 60 Hz, which is wrong
    // for 240p (60.114 Hz). Derive from actual timing instead:
    //   samples_per_line = sample_rate * h_total_pixels / pixel_clock
    uint32_t pixel_clock_hz = clock_get_hz(clk_sys) / ((uint32_t)video_output_active_mode->hstx_clk_div *
                                                       video_output_active_mode->hstx_csr_clkdiv);
    uint32_t h_total = video_output_active_mode->h_total_pixels;
    uint32_t spl_fp = (uint32_t)(((uint64_t)sample_rate * h_total << 16) / pixel_clock_hz);
    hstx_di_queue_set_samples_per_line_fp(spl_fp);

    hstx_packet_t packet;
    hstx_data_island_t island;

    uint32_t acr_n;
    uint32_t acr_cts;
    get_acr_params(sample_rate, &acr_n, &acr_cts);
    hstx_packet_set_acr(&packet, acr_n, acr_cts);
    hstx_encode_data_island(&island, &packet, true, true);
    vblank_acr_vsync_on_len = build_line_with_di(vblank_acr_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, true);
    vblank_acr_vsync_off_len = build_line_with_di(vblank_acr_vsync_off, island.words, false, false);

    hstx_packet_set_audio_infoframe(&packet, sample_rate, 2, 16);
    hstx_encode_data_island(&island, &packet, true, true);
    vblank_infoframe_vsync_on_len = build_line_with_di(vblank_infoframe_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, true);
    vblank_infoframe_vsync_off_len = build_line_with_di(vblank_infoframe_vsync_off, island.words, false, false);
}

static void init_rt_from_mode(const video_mode_t *mode)
{
    rt_h_front_porch = mode->h_front_porch;
    rt_h_sync_width = mode->h_sync_width;
    rt_h_back_porch = mode->h_back_porch;
    rt_h_active_pixels = mode->h_active_pixels;
    rt_v_front_porch = mode->v_front_porch;
    rt_v_sync_width = mode->v_sync_width;
    rt_v_back_porch = mode->v_back_porch;
    rt_v_active_lines = mode->v_active_lines;
    rt_v_total_lines = mode->v_total_lines;
    rt_sync_after_di = mode->h_sync_width - W_PREAMBLE - W_DATA_ISLAND;
}

static void build_all_command_lists(const video_mode_t *mode)
{
    build_dvi_command_lists();

    // AVI InfoFrame: VIC=0 (non-standard) to avoid strict pixel clock
    // validation by sinks. Our 25.2 MHz is 0.1% off from standard 25.175 MHz.
    hstx_packet_t packet;
    hstx_data_island_t island;
    uint8_t vic = (mode->v_active_lines == 480) ? 1 : 0;
    hstx_packet_set_avi_infoframe(&packet, vic, 0);
    hstx_encode_data_island(&island, &packet, false, true);
    vblank_avi_infoframe_len = build_line_with_di(vblank_avi_infoframe, island.words, false, false);

    // Null DI command lists
    vblank_di_null_len = build_line_with_di(vblank_di_null, hstx_get_null_data_island(false, true), false, false);
    vactive_di_null_len = build_line_with_di(vactive_di_null, hstx_get_null_data_island(false, true), false, true);

    vblank_di_len = build_line_with_di(vblank_di_ping, hstx_get_null_data_island(false, true), false, false);
    memcpy(vblank_di_pong, vblank_di_ping, sizeof(vblank_di_ping));
}

static void apply_mode(const video_mode_t *mode)
{
    // 1. Disable DMA IRQ to prevent ISR firing with partial state
    irq_set_enabled(DMA_IRQ_0, false);

    // 2. Write all cached timing variables
    init_rt_from_mode(mode);

    // 3. Update public state
    video_output_active_mode = mode;
    frame_width = mode->h_active_pixels;
    frame_height = mode->v_active_lines;

    // 4. Build all command lists
    build_all_command_lists(mode);

    // 5. Update data island queue timing
    hstx_di_queue_set_v_total(mode->v_total_lines);

    // 6. Rebuild audio packets
    configure_audio_packets(current_sample_rate);

    // 7. Resync HSTX
    hstx_resync();

    // 8. Re-enable DMA IRQ
    irq_set_enabled(DMA_IRQ_0, true);
}

// ============================================================================
// Public Interface
// ============================================================================

void video_output_init(uint16_t width, uint16_t height)
{
    // Use pending_mode if set via video_output_set_mode() before init,
    // otherwise fall back to compile-time selection
    const video_mode_t *pm = (const video_mode_t *)pending_mode;
    const video_mode_t *initial_mode;
    if (pm) {
        pending_mode = NULL;
        initial_mode = pm;
    } else {
#ifdef VIDEO_MODE_320x240
        initial_mode = &VIDEO_MODE_240P;
#else
        initial_mode = &video_mode_480_p;
#endif
    }

    // Initialize rt_* cached variables and build command lists
    init_rt_from_mode(initial_mode);

    video_output_active_mode = initial_mode;
    frame_width = width;
    frame_height = height;

    build_all_command_lists(initial_mode);

    // Configure clk_hstx
    uint32_t sys_freq = clock_get_hz(clk_sys);
    clock_configure_int_divider(clk_hstx,
                                0, // No glitchless mux
                                CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys_freq, initial_mode->hstx_clk_div);

    // Claim DMA channels for HSTX (channels 0 and 1)
    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);

    // Update data island queue timing
    hstx_di_queue_set_v_total(initial_mode->v_total_lines);

    // Initialize HDMI audio packets (default 48kHz)
    configure_audio_packets(48000);
}

void video_output_set_background_task(video_output_task_fn task)
{
    background_task = task;
}

bool video_output_get_dvi_mode(void)
{
    return dvi_mode;
}

void video_output_set_dvi_mode(bool enabled)
{
    dvi_mode = enabled;
}

void video_output_set_scanline_callback(video_output_scanline_cb_t cb)
{
    scanline_callback = cb;
}

void video_output_set_vsync_callback(video_output_vsync_cb_t cb)
{
    vsync_callback = cb;
}

void video_output_core1_run(void)
{
    // HSTX Hardware Setup
    hstx_ctrl_hw->expand_tmds = 4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 8 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                                5 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 3 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                                4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 13 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    hstx_ctrl_hw->expand_shift =
        2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS |
                        (uint32_t)video_output_active_mode->hstx_csr_clkdiv << HSTX_CTRL_CSR_CLKDIV_LSB |
                        5U << HSTX_CTRL_CSR_N_SHIFTS_LSB | 2U << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        int bit = 2 + (lane * 2);
        uint32_t lane_data_sel_bits = (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB | (lane * 10 + 1)
                                                                                    << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits;
    }

    // DMA Setup (configured before GPIO connection to avoid TMDS garbage)
    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off, 9, false);

    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off, 9, false);

    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    dma_hw->inte0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, 0);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);

    // Wait for first DMA transfer to complete — HSTX serializes valid TMDS
    // internally but GPIO isn't connected yet, so TV sees nothing.
    while (dma_channel_is_busy(DMACH_PING))
        tight_loop_contents();

    // NOW connect GPIO — TV's first TMDS exposure is valid data
    for (int i = PIN_HSTX_CLK; i <= PIN_HSTX_D2 + 1; ++i) {
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_4MA);
        gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
        gpio_set_function(i, 0);
    }

    while (1) {
        // Check for pending mode switch
        const video_mode_t *new_mode = (const video_mode_t *)pending_mode;
        if (new_mode) {
            pending_mode = NULL;
            apply_mode(new_mode);
        }

        // Check for resync request
        if (resync_requested) {
            resync_requested = false;
            irq_set_enabled(DMA_IRQ_0, false);
            hstx_resync();
            irq_set_enabled(DMA_IRQ_0, true);
        }

        if (background_task) {
            background_task();
        }
        tight_loop_contents();
    }
}

void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate)
{
    current_sample_rate = sample_rate;
    configure_audio_packets(sample_rate);
}

void video_output_set_mode(const video_mode_t *mode)
{
    pending_mode = mode;
    __dmb();
}

uint16_t video_output_get_h_active_pixels(void)
{
    return rt_h_active_pixels;
}

uint16_t video_output_get_v_active_lines(void)
{
    return rt_v_active_lines;
}

void video_output_reconfigure_clock(void)
{
    uint32_t sys_freq = clock_get_hz(clk_sys);
    clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys_freq,
                                video_output_active_mode->hstx_clk_div);
}

void video_output_update_acr(uint32_t pixel_clock_hz)
{
    // Calculate CTS for non-standard pixel clock:
    // CTS = pixel_clock_hz * N / (128 * sample_rate)
    uint32_t acr_n;
    uint32_t acr_cts;
    get_acr_params(current_sample_rate, &acr_n, &acr_cts);

    // Use 64-bit math to avoid overflow
    uint64_t cts64 = ((uint64_t)pixel_clock_hz * acr_n) / (128ULL * current_sample_rate);
    uint32_t custom_cts = (uint32_t)cts64;

    hstx_packet_t packet;
    hstx_data_island_t island;

    hstx_packet_set_acr(&packet, acr_n, custom_cts);
    hstx_encode_data_island(&island, &packet, true, true);
    genlock_acr_vsync_on_len = build_line_with_di(genlock_acr_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, true);
    genlock_acr_vsync_off_len = build_line_with_di(genlock_acr_vsync_off, island.words, false, false);

    __dmb();
    use_genlock_acr = true;
}

void video_output_request_resync(void)
{
    resync_requested = true;
    __dmb();
}
