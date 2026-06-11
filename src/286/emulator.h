#ifndef CPU_286EM_H
#define CPU_286EM_H

#include <stdint.h>
#include "mem.h"

inline static uint8_t read86(uint32_t a) { return pload8(a); }
inline static uint16_t readw86(uint32_t a) { return pload16(a); }
inline static uint32_t readdw86(uint32_t a) { return pload32(a); }

inline static void write86(uint32_t a, uint8_t v) { return pstore8(a, v); }
inline static void writew86(uint32_t a, uint16_t v) { return pstore16(a, v); }
inline static void writedw86(uint32_t a, uint32_t v) { return pstore32(a, v); }

#endif // CPU_286EM_H
