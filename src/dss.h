#include <stdint.h>

void dss_out(const uint16_t portnum, const uint8_t value);
uint8_t dss_in(const uint16_t portnum);
// 7 kHz fn
int16_t dss_sample();