#ifndef ADLIB_H
#define ADLIB_H

#include <stdint.h>

#define FLOAT float

#define ADLIB_BATCH_SIZE 64

typedef struct AdlibState AdlibState;

void adlib_write(void *opaque, uint32_t nport, uint32_t val);
uint32_t adlib_read(void *opaque, uint32_t nport);
AdlibState *adlib_new();
// call it 44100 times per sec from timer on core1 (ISR, so should be fast)
int16_t adlib_getsample(AdlibState *s);
// call it from main cycle on core0
void adlib_core0(AdlibState *s);

#endif /* ADLIB_H */
