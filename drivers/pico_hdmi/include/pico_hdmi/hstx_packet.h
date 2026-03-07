#ifndef HSTX_PACKET_H
#define HSTX_PACKET_H

#include <stdbool.h>
#include <stdint.h>

// Data Island timing constants
#define W_GUARDBAND 2                                             // Guard band: 2 pixel clocks
#define W_PREAMBLE 8                                              // Preamble: 8 pixel clocks
#define W_DATA_PACKET 32                                          // Packet data: 32 pixel clocks
#define W_DATA_ISLAND (W_GUARDBAND + W_DATA_PACKET + W_GUARDBAND) // Total: 36

// HSTX outputs 1 symbol per word
#define HSTX_DATA_ISLAND_WORDS W_DATA_ISLAND // 36 words for HSTX

// Packet structure (same as DVI/HSTX spec)
typedef struct {
    uint8_t header[4];       // 3 bytes header + 1 byte BCH parity
    uint8_t subpacket[4][8]; // 4 subpackets, each 7 bytes + 1 byte BCH parity
} hstx_packet_t;

// Pre-encoded data island for HSTX (36 words)
typedef struct {
    uint32_t words[HSTX_DATA_ISLAND_WORDS];
} hstx_data_island_t;

// Audio sample structure
typedef struct {
    int16_t left;
    int16_t right;
} audio_sample_t;

// ============================================================================
// Packet creation functions
// ============================================================================

void hstx_packet_init(hstx_packet_t *packet);
void hstx_packet_set_acr(hstx_packet_t *packet, uint32_t n, uint32_t cts);
void hstx_packet_set_audio_infoframe(hstx_packet_t *packet, uint32_t sample_rate, uint8_t channels,
                                     uint8_t bits_per_sample);
void hstx_packet_set_avi_infoframe(hstx_packet_t *packet, uint8_t vic, uint8_t pixel_repetition);
int hstx_packet_set_audio_samples(hstx_packet_t *packet, const audio_sample_t *samples, int num_samples,
                                  int frame_count);
void hstx_packet_set_null(hstx_packet_t *packet);

// ============================================================================
// TERC4 encoding for HSTX
// ============================================================================

void hstx_encode_data_island(hstx_data_island_t *out, const hstx_packet_t *packet, bool vsync, bool hsync);
const uint32_t *hstx_get_null_data_island(bool vsync, bool hsync);

#endif // HSTX_PACKET_H
