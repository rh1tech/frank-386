#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t fdos_mcb_type(uint16_t s);
uint16_t fdos_mcb_owner(uint16_t s);
uint16_t fdos_mcb_size(uint16_t s);
void fdos_mcb_set_type(uint16_t s, uint8_t v);
void fdos_mcb_set_owner(uint16_t s, uint16_t v);
void fdos_mcb_set_size(uint16_t s, uint16_t v);
void fdos_mcb_add_size(uint16_t s, uint16_t add);
uint8_t fdos_mcb_name_byte(uint16_t s, unsigned index);
void fdos_mcb_set_name_byte(uint16_t s, unsigned index, uint8_t value);
void fdos_mcb_set_name8(uint16_t s, const char name[8]);

#ifdef __cplusplus
}
#endif
