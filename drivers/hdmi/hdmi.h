#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "inttypes.h"
#include "stdbool.h"
#include "board_config.h"

#include "hardware/pio.h"

#ifndef PIO_VIDEO
#define PIO_VIDEO pio0
#define PIO_VIDEO_ADDR pio0
#define VIDEO_DMA_IRQ (DMA_IRQ_0)
#endif

#ifndef HDMI_BASE_PIN
#define HDMI_BASE_PIN (6)
#endif

#ifndef HDMI_PIN_RGB_notBGR
#if defined(BOARD_Z0) || defined(BOARD_Z2)
	#define HDMI_PIN_RGB_notBGR (0)
	#define HDMI_PIN_invert_diffpairs (0)
	#define beginHDMI_PIN_data (HDMI_BASE_PIN)
	#define beginHDMI_PIN_clk (HDMI_BASE_PIN + 6)
#else
	#define HDMI_PIN_RGB_notBGR (1)
	#define HDMI_PIN_invert_diffpairs (1)
	#define beginHDMI_PIN_clk (HDMI_BASE_PIN)
	#define beginHDMI_PIN_data (HDMI_BASE_PIN+2)
#endif
#endif

void hdmi_set_dma_high_priority(void);

/* the text palette (CGA pairs) into entries 0..255; hdmi_palette_reload:
 * the guest's palette is to be applied again at the next frame */
void hdmi_text_palette(void);
extern volatile bool hdmi_palette_reload;

/* HDMI audio: one 44.1 kHz stereo frame from the mixer (core 0); dropped
 * until the HDMI output runs */
void hdmi_audio_push(int16_t left, int16_t right);

/* counters for the bench (~STAT~): frames sent, the longest line IRQ so far
 * (us; a line is 31.7), frames in which the output starved, audio frames
 * repeated (ring empty) and dropped (ring full), restarts of the output */
extern volatile uint32_t hdmi_frame_count;
extern volatile uint32_t hdmi_irq_max_us;
extern volatile uint32_t hdmi_underruns;
extern volatile uint32_t hdmi_audio_underruns, hdmi_audio_drops;
extern volatile uint32_t hdmi_resyncs;

/* call often from core 1 (its IRQ's core): restarts a stopped output */
void hdmi_watchdog(void);

#define TEXTMODE_COLS 53
#define TEXTMODE_ROWS 30


// TODO: Сделать настраиваемо
static const uint8_t textmode_palette[16] = {
    200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215
};

#ifdef __cplusplus
}
#endif
