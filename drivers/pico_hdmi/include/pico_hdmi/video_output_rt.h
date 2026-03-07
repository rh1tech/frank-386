#ifndef VIDEO_OUTPUT_RT_H
#define VIDEO_OUTPUT_RT_H

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Runtime Video Mode Structure
// ============================================================================

typedef struct {
    uint16_t h_front_porch;
    uint16_t h_sync_width;
    uint16_t h_back_porch;
    uint16_t h_active_pixels;
    uint16_t v_front_porch;
    uint16_t v_sync_width;
    uint16_t v_back_porch;
    uint16_t v_active_lines;
    uint16_t h_total_pixels;
    uint16_t v_total_lines;
    uint8_t hstx_clk_div;
    uint8_t hstx_csr_clkdiv;
} video_mode_t;

extern const video_mode_t video_mode_480_p;
extern const video_mode_t video_mode_240_p;
extern const video_mode_t *video_output_active_mode;

// ============================================================================
// Shared State (same as video_output.h)
// ============================================================================

extern uint16_t frame_width;
extern uint16_t frame_height;
extern volatile uint32_t video_frame_count;

// ============================================================================
// Callback Types
// ============================================================================

typedef void (*video_output_task_fn)(void);
typedef void (*video_output_vsync_cb_t)(void);

/**
 * Scanline Callback:
 * Called by the video output system when it needs pixel data for a scanline.
 *
 * @param v_scanline The current vertical scanline (0 to v_total_lines - 1)
 * @param active_line The current active video line (0 to v_active_lines - 1),
 *                    only valid if active_video is true.
 * @param line_buffer Buffer to fill with h_active_pixels RGB565 pixels (packed as uint32_t pairs).
 *                    The buffer MUST be filled with (h_active_pixels / 2) uint32_t words.
 */
typedef void (*video_output_scanline_cb_t)(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer);

// ============================================================================
// Public Interface (compatible with video_output.h API)
// ============================================================================

/**
 * Initialize HSTX and DMA for video output.
 * Initial mode is selected from compile-time VIDEO_MODE_320x240 define,
 * or defaults to 480p.
 * @param width Framebuffer width in pixels (e.g., 320)
 * @param height Framebuffer height in pixels (e.g., 240)
 */
void video_output_init(uint16_t width, uint16_t height);

void video_output_set_scanline_callback(video_output_scanline_cb_t cb);
void video_output_set_vsync_callback(video_output_vsync_cb_t cb);
void video_output_set_background_task(video_output_task_fn task);

bool video_output_get_dvi_mode(void);
void video_output_set_dvi_mode(bool enabled);

/**
 * Core 1 entry point for video output.
 * This function does not return.
 */
void video_output_core1_run(void);

/**
 * Reconfigure HDMI audio for a different sample rate.
 * @param sample_rate Audio sample rate in Hz (e.g. 32000, 44100, 48000)
 */
void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate);

// ============================================================================
// Runtime Mode Switching API (new)
// ============================================================================

/**
 * Request a video mode switch. Safe to call from Core 0.
 * The switch is applied on the Core 1 main loop (not immediately).
 * @param mode Pointer to a video_mode_t (must remain valid, use VIDEO_MODE_480P/240P)
 */
void video_output_set_mode(const video_mode_t *mode);

/**
 * Get the active horizontal resolution in pixels.
 */
uint16_t video_output_get_h_active_pixels(void);

/**
 * Get the active vertical resolution in lines.
 */
uint16_t video_output_get_v_active_lines(void);

/**
 * Reconfigure clk_hstx after sys clock has changed (e.g. genlock adjustment).
 * Must be called from Core 1, or before video_output_core1_run().
 */
void video_output_reconfigure_clock(void);

/**
 * Recalculate ACR CTS/N values for a new pixel clock frequency.
 * Call after adjusting sys_clk for genlock. Safe from Core 0.
 * @param pixel_clock_hz Actual pixel clock in Hz (e.g. 25175000, 25200000)
 */
void video_output_update_acr(uint32_t pixel_clock_hz);

/**
 * Request an HSTX resync from Core 0. The resync is performed
 * on the Core 1 main loop.
 */
void video_output_request_resync(void);

#endif // VIDEO_OUTPUT_RT_H
