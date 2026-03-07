#ifndef VIDEO_OUTPUT_H
#define VIDEO_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Video Output Configuration
// ============================================================================

// Video mode selection (define VIDEO_MODE_320x240 before including this header)
#ifdef VIDEO_MODE_320x240

// 1280x240 @ 60Hz - True 240p (Quad Clock Rate)
// Pixel Clock: 25.2 MHz (15kHz scan rate)
// H_TOTAL = 1600 pixels, V_TOTAL = 262 lines
// Active: 1280 (320x4), Front: 32 (8x4), Sync: 192 (48x4), Back: 96 (24x4)
#define MODE_H_FRONT_PORCH 32
#define MODE_H_SYNC_WIDTH 192
#define MODE_H_BACK_PORCH 96
#define MODE_H_ACTIVE_PIXELS 1280

#define MODE_V_FRONT_PORCH 4
#define MODE_V_SYNC_WIDTH 4
#define MODE_V_BACK_PORCH 14
#define MODE_V_ACTIVE_LINES 240

// HSTX clock divider: clk_sys / 1 = 126 MHz -> 25.2 MHz pixel clock (with CSR_CLKDIV=5)
#define MODE_HSTX_CLK_DIV 1
#define MODE_HSTX_CSR_CLKDIV 5

#else

// 640x480 @ 60Hz (VIC 1) - Default mode
// Pixel clock: 25.2 MHz
#define MODE_H_FRONT_PORCH 16
#define MODE_H_SYNC_WIDTH 96
#define MODE_H_BACK_PORCH 48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH 10
#define MODE_V_SYNC_WIDTH 2
#define MODE_V_BACK_PORCH 33
#define MODE_V_ACTIVE_LINES 480

// HSTX clock divider: 1 (no division)
#define MODE_HSTX_CLK_DIV 1
#define MODE_HSTX_CSR_CLKDIV 5

#endif

#define MODE_H_TOTAL_PIXELS (MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL_LINES (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

// Frame dimensions (set via video_output_init)
extern uint16_t frame_width;
extern uint16_t frame_height;

// ============================================================================
// Global State
// ============================================================================

extern volatile uint32_t video_frame_count;

// ============================================================================
// Public Interface
// ============================================================================

typedef void (*video_output_task_fn)(void);
typedef void (*video_output_vsync_cb_t)(void);

/**
 * Scanline Callback:
 * Called by the video output system when it needs pixel data for a scanline.
 *
 * @param v_scanline The current vertical scanline (0 to MODE_V_TOTAL_LINES - 1)
 * @param active_line The current active video line (0 to MODE_V_ACTIVE_LINES - 1),
 *                    only valid if active_video is true.
 * @param line_buffer Buffer to fill with MODE_H_ACTIVE_PIXELS RGB565 pixels (packed as uint32_t pairs).
 *                    The buffer MUST be filled with (MODE_H_ACTIVE_PIXELS / 2) uint32_t words.
 *                    - 640x480 mode: 640 pixels = 320 uint32_t words
 *                    - 320x240 mode: 1280 pixels = 640 uint32_t words (4x pixel repetition)
 */
typedef void (*video_output_scanline_cb_t)(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer);

/**
 * Initialize HSTX and DMA for video output.
 * @param width Framebuffer width in pixels (e.g., 320)
 * @param height Framebuffer height in pixels (e.g., 240)
 */
void video_output_init(uint16_t width, uint16_t height);

/**
 * Register the scanline callback.
 */
void video_output_set_scanline_callback(video_output_scanline_cb_t cb);

/**
 * Register a VSYNC callback, called once per frame at the start of vertical sync.
 */
void video_output_set_vsync_callback(video_output_vsync_cb_t cb);

/**
 * Register a background task to run in the Core 1 loop.
 * This is typically used for audio processing.
 */
void video_output_set_background_task(video_output_task_fn task);

/**
 * Get DVI mode status.
 * @return true if DVI mode (no HDMI audio), false if HDMI mode
 */
bool video_output_get_dvi_mode(void);

/**
 * Set DVI mode.
 * When enabled, disables all HDMI Data Islands (no audio output).
 * Some monitors have trouble syncing with HDMI Data Islands.
 * Default: false (HDMI mode with audio).
 * @param enabled true for DVI mode, false for HDMI mode with audio
 */
void video_output_set_dvi_mode(bool enabled);

/**
 * Core 1 entry point for video output.
 * This function does not return.
 */
void video_output_core1_run(void);

/**
 * Reconfigure HDMI audio for a different sample rate.
 * Updates ACR, Audio InfoFrame, and packet timing.
 * Can be called after video_output_init() to override the default 48kHz.
 * @param sample_rate Audio sample rate in Hz (e.g. 32000, 44100, 48000)
 */
void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate);

#endif // VIDEO_OUTPUT_H
